# Chapter 19 — Runtime Polymorphism

> **Part VIII · Polymorphism** &nbsp;|&nbsp; **Level 4** (compiler/runtime) → 5 (hardware) &nbsp;|&nbsp; **≈ 6 hours**
> **Prerequisites:** [Chapter 2](../part-02-object-model-and-lifetime/02-object-model.md), [Chapter 3](../part-02-object-model-and-lifetime/03-lifetime-and-storage.md) &nbsp;|&nbsp; **Standards:** C++98 (virtual), C++11 (`override`, `final`), C++20 (`constexpr` virtual) &nbsp;|&nbsp; **Tools:** `g++-14`, `clang++-18`, `nm`, `objdump`, `perf`

[← Previous: Chapter 18](../part-07-compile-time/18-reflection-landscape.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 20 — Static polymorphism →](20-static-polymorphism.md)

---

**In one sentence:** a virtual call is *"load the object's hidden pointer, index a table, call indirectly"*, and everything else in this chapter (layout, thunks, virtual inheritance, `dynamic_cast`, devirtualization, the cost) follows from that one mechanism.

> **How to read the layers in this chapter.** The C++ **standard** specifies the *behaviour* of virtual functions (which overrider runs, what happens in constructors, what `dynamic_cast` returns). It says **nothing** about vtables, vptrs, or object layout. Everything labelled *vtable*, *vptr*, *thunk*, *offset-to-top* is the **Itanium C++ ABI** (used by GCC and Clang on Linux, macOS and most other Unix-like systems) and **MSVC does it differently**. I mark each statement ⚖️ *standard*, 🧩 *Itanium ABI* or 🔧 *compiler optimization*.

**By the end of this chapter you can:**

- draw the memory layout of an object with single, multiple and virtual inheritance, and say what the standard guarantees about it (nothing) versus what the ABI fixes
- read a vtable from a running program and explain every slot
- explain what a thunk is and when it appears
- predict when a virtual call is *free*, *cheap* or *expensive* on real hardware
- explain why a virtual call in a constructor doesn't reach the derived class
- decide between virtual functions and the alternatives in Chapters 20–21

---

## 1. Problem

You need code that works with *different types through one interface*, where the concrete type is **only known at run time** (a plug-in, a GUI widget tree, a parsed config, a network message):

```cpp
void render(const std::vector<Shape*>& scene) {
    for (Shape* s : scene) s->draw();          // which draw()? decided per element, at run time
}
```

C has the idiom (a struct of function pointers, `FILE`'s ops, the Linux `file_operations` table). The questions C++ answers are: how to make that **type-safe**, **automatic** (the pointer to the table is installed by the constructor), and **extensible** (a derived type reuses and overrides entries).

---

## 2. Historical context

| Year | Event |
|---|---|
| 1967 | Simula 67 introduces classes and virtual procedures |
| 1979–83 | Stroustrup's "C with Classes" / C++: `virtual` implemented with **a table of function pointers per class** (the vtable) and one hidden pointer per object |
| 1985–90 | Multiple inheritance (C++ 2.0) forces **`this`-adjustment**: the first appearance of thunks. Virtual base classes added for the diamond |
| 1998 | **Itanium C++ ABI** is written for the IA-64 port; it ends up adopted by GCC for *all* targets and by Clang → effectively the Linux/macOS standard |
| 2011 | `override` and `final` (catch signature mismatches; enable devirtualization) |
| 2011 | Guaranteed: a class with virtual functions is not trivially copyable; `std::is_polymorphic` |
| 2020 | `constexpr` virtual functions; `consteval` interplay |
| 2020s | GCC/Clang: speculative devirtualization, `-fdevirtualize-at-ltrans`, `-fstrict-vtable-pointers` (Clang), whole-program vtables, CFI (control-flow integrity) schemes that protect indirect calls |

Dynamic dispatch has barely changed in 40 years. What changed is **the surrounding alternatives** (templates, `std::variant`, type erasure) and the **optimizers** (devirtualization).

---

## 3. Modern solution

```cpp
class Shape {
public:
    virtual ~Shape() = default;                       // 1. polymorphic base: virtual destructor
    virtual double area() const = 0;                  // 2. pure virtual: interface
    virtual std::string name() const { return "shape"; }   // 3. virtual with a default
};

class Circle final : public Shape {                   // 4. final: no further overrides; devirtualizable
public:
    explicit Circle(double r) : r_(r) {}
    double area() const override { return 3.14159265 * r_ * r_; }    // 5. override: checked against the base
private:
    double r_;
};
```

| Keyword | Meaning | Why it matters |
|---|---|---|
| `virtual` | The call is resolved on the *dynamic* type | The whole feature |
| `= 0` | Pure virtual: the class is abstract | Interfaces |
| `override` | This function must override a base virtual | Compile error on a signature typo (catches silent non-overrides) |
| `final` (function or class) | No further overriding / derivation | Lets the compiler **devirtualize** |
| `virtual ~T()` | Delete through a base pointer is defined | Without it, `delete base_ptr` on a derived object is **undefined behaviour** ⚖️ |

---

## 4. Mental model

### The picture (single inheritance)  🧩

```text
   object b (type B : A)                         vtable for B  (read-only data, ONE per class)
   ┌───────────────────────────┐                ┌──────────────────────────────────────────┐
   │ vptr  ──────────────────────────────────►  │ [-2] offset-to-top = 0                    │
   ├───────────────────────────┤  (points HERE) │ [-1] typeinfo for B                       │
   │ int A::a                  │       ───────► │ [ 0] B::f   (overrides A::f)              │
   ├───────────────────────────┤                │ [ 1] A::g   (inherited, not overridden)   │
   │ int B::b                  │                │ [ 2] B::~B  (complete dtor)               │
   └───────────────────────────┘                │ [ 3] B::~B  (deleting dtor)               │
                                                └──────────────────────────────────────────┘

   p->f()   ≡   (*(p->vptr)[0])(p)       // load vptr, load slot, indirect call with `this` in the first argument register
```

The cost of a virtual call, step by step:

```text
   mov  rax, [rdi]        ; load the vptr          (a memory load: likely in L1 if the object was just touched)
   call [rax + 8*slot]    ; load the slot + indirect call (a second load, then an indirect branch)
```

Two dependent loads and an **indirect branch**. On modern CPUs this is cheap *when the branch predictor predicts the target*, and expensive (a ~15–20-cycle pipeline flush) when it doesn't. The real cost of virtual dispatch is almost never the two loads; it is **(1)** the lost inlining and the optimizations that depend on it, and **(2)** indirect-branch misprediction when the dynamic type varies unpredictably.

### What the standard says vs. what the ABI says

| Question | ⚖️ Standard | 🧩 Itanium ABI |
|---|---|---|
| Which function runs for `p->f()`? | The *final overrider* in the dynamic type of `*p` | — |
| How is that found? | Unspecified | vtable lookup through the vptr |
| Where is the vptr? | Unspecified | Offset 0 of the (primary-base) object, if the class has a dynamic base or virtual function |
| Is there a vtable at all? | Unspecified | Yes, one per polymorphic class, in `.data.rel.ro`/`.rodata` |
| Size of a polymorphic object | Unspecified | `sizeof(non-virtual state) + 8` (one vptr), plus another vptr per non-primary polymorphic base, plus vbase pointers |
| What happens when you call a virtual function in a constructor? | The version of the class **being constructed** is called (never a more-derived override) | The vptr is rewritten as each base/derived constructor starts (Experiment 5) |
| `typeid`/`dynamic_cast` | Defined behaviour | `typeinfo` pointer at vtable[-1], offset-to-top at vtable[-2] |

---

## 5. Language rules

### 5.1 Overriding  `[class.virtual]` ⚖️

A member function `D::f` **overrides** `B::f` if `B::f` is virtual and `D::f` has the same name, parameter-type-list, cv-qualifiers and ref-qualifiers (the return type may be *covariant*: a pointer/reference to a class derived from the base's return class). `virtual` on the derived declaration is optional; **always write `override`** so a mismatch is an error.

Default arguments are **not** virtual: they are bound **statically** by the type of the expression used in the call:

```cpp
struct A { virtual void f(int x = 1) { std::printf("A %d\n", x); } };
struct B : A { void f(int x = 2) override { std::printf("B %d\n", x); } };
A* p = new B; p->f();     // prints "B 1": B::f runs (dynamic), but the default argument 1 comes from A (static type)
```

### 5.2 Abstract classes, destructors

- A class with a pure virtual function cannot be instantiated; the pure function **may still have a definition** (and a derived class can call it explicitly). A **pure virtual destructor must be defined**.
- Deleting a derived object through a base pointer is UB unless the base destructor is `virtual` ⚖️. If a class has virtual functions but no virtual destructor, `-Wnon-virtual-dtor` / `-Wdelete-non-virtual-dtor` warn.
- Rule: *polymorphic base ⇒ public virtual destructor, or protected non-virtual destructor* (the second prevents deletion through the base).

### 5.3 Virtual calls during construction/destruction  `[class.cdtor]` ⚖️

While a base-class constructor runs, the object's dynamic type **is the base**; a virtual call in a constructor or destructor resolves to the version in the constructor's own class (a pure virtual call there is undefined behaviour). Experiment 5 shows it and the mechanism (vptr is updated during construction).

### 5.4 Multiple inheritance  🧩

Each polymorphic base gets its own vptr and its own sub-object. A conversion `Derived* → SecondBase*` **adjusts the pointer** (adds the sub-object offset). A virtual call through the second base needs `this` re-adjusted back to the full object before running the overrider: the vtable entry points at a **thunk**, a tiny stub that subtracts the offset and jumps to the real function.

```text
   M : A, X                      M's layout (sizeof = 32, measured in Experiment 2)
   ┌────────────────────────┐   0   ← &m, also the A* (primary base: shares M's first vptr)
   │ vptr_A                 │
   │ int A::a               │
   ├────────────────────────┤   16  ← the X* points here  (X subobject, with its OWN vptr)
   │ vptr_X                 │
   │ int X::xx              │
   └────────────────────────┘
```

### 5.5 Virtual inheritance  🧩

`struct V1 : virtual A` makes `A` a **shared** sub-object that the *most-derived class* places somewhere and everyone else locates at run time. So each class with a virtual base stores either an offset in its vtable (**vbase offset**, at a negative index) or a pointer; accessing a member of the virtual base is an *extra indirection*: load vptr → load vbase offset → add. Constructors take a hidden "is-most-derived" parameter, and the most-derived class initializes the virtual base. This is why virtual inheritance is rarely worth it (Opinion below), and why `static_cast` from a virtual base to a derived is ill-formed (you need `dynamic_cast`).

### 5.6 `dynamic_cast`, `typeid`  `[expr.dynamic.cast]` ⚖️

- `dynamic_cast<T*>(p)` returns `nullptr` if `*p` is not a `T` (references throw `std::bad_cast`). It requires a polymorphic source type and **walks the class hierarchy using the typeinfo graph** (🧩 `__dynamic_cast` in libstdc++/libc++abi), including string comparison of names in some ABI configurations; it is the slowest of the three operations (Experiment 6).
- `typeid(*p)` returns the dynamic `std::type_info`; its `name()` is implementation-defined (mangled on Itanium).
- Both require RTTI (`-fno-rtti` disables them; Qt, LLVM and many game engines build without it).

### 5.7 `final`, `override`, and what the optimizer can learn

`final` on a class or function promises there is no further override, so the compiler can replace the virtual call with a direct call (and inline it). Without `final`, GCC/Clang can still devirtualize when they *know the dynamic type* (a local object, `new T` in the same function) or, in **whole-program** mode (LTO + `-fwhole-program-vtables` in Clang, `-fdevirtualize-speculatively` in GCC), when the class hierarchy is closed.

### Layer check

| Layer | Decides |
|---|---|
| **Standard** | Overrider selection; the semantics in constructors/destructors; `dynamic_cast`/`typeid`; UB of deleting without a virtual destructor; `final`/`override` meaning |
| **Compiler** | Devirtualization decisions; emission point of the vtable (the *key function* rule: the vtable is emitted in the TU defining the first non-inline, non-pure virtual function, otherwise weak in every TU); thunk generation |
| **ABI (Itanium)** | vptr position, vtable contents and order, thunks, vbase offsets, `offset-to-top`, `typeinfo` layout, mangled names (`_ZTV…` vtable, `_ZTI…` typeinfo, `_ZTS…` typeinfo name, `_ZThn…` non-virtual thunk, `_ZTv…` virtual thunk) |
| **OS** | Maps vtables into read-only pages; with RELRO the pointers are relocated at load then protected; shared-library symbol interposition applies to vtables and typeinfo |
| **CPU** | Indirect branch prediction (BTB, indirect predictor), the dependent-load latency chain, I-cache footprint of many distinct targets |

---

## 6. Implementation model

### Itanium vtable contents (🧩), for a class with virtual functions

```text
   address point ─►                 slot     contents
                                    -3-N…    (virtual base offsets, if any virtual bases)         ← grows downward
                                    -2       offset-to-top  (ptrdiff_t: how far this sub-object is from the complete object; 0 for the primary base)
                                    -1       pointer to the std::type_info of the most derived class
   vptr points here ───────────────► 0       first virtual function (declaration order)
                                     1       second virtual function
                                     …
                                     n       virtual destructor (complete object dtor)
                                     n+1     virtual destructor (deleting dtor: dtor + operator delete)
```

The destructor appears **twice** because there are two entry points: *complete-object destructor* (`D1`: runs the destructor body and base destructors) and *deleting destructor* (`D0`: does `D1` then calls `operator delete`). The `delete p` expression calls the **deleting** one through the vtable.

### Where does the vptr get set?

At the **start of each constructor** (after base subobjects are constructed, before member initializers and the body), the constructor stores the address of *its own class's* vtable into the object's vptr(s). So during `A`'s constructor the vptr points at `A`'s vtable; then `B`'s constructor overwrites it with `B`'s vtable. In the destructor the same happens in reverse. That is the **entire mechanism** behind the "virtual call in constructor" rule (Experiment 5), and the reason a constructor of a polymorphic class always writes at least one store, which can defeat dead-store elimination or make `memset` + inline construction cheaper than expected.

### Emission

```text
   class with a key function (first non-inline, non-pure virtual):     vtable + typeinfo emitted ONLY in the TU defining that function
   class with no key function (all virtuals inline or pure):           vtable + typeinfo emitted as WEAK symbols in EVERY TU that needs them
```

This is a practical, observable, ABI-level fact: it is why a link error "undefined reference to `vtable for Foo`" almost always means *you declared a non-inline virtual function and never defined it*.

---

## 7. Experiments

### Experiment 1 🧩: Read the vtable of a running object

This is **not portable and not defined C++**; it relies on the Itanium ABI. It is a way of *seeing* the mechanism.

```cpp
// @test run -std=c++23 -O0
#include <cstdint>
#include <cstdio>
#include <typeinfo>

struct A {
    virtual void f() { std::puts("A::f"); }
    virtual void g() { std::puts("A::g"); }
    virtual ~A() = default;
    int a = 1;
};
struct B : A {
    void f() override { std::puts("B::f"); }     // overrides slot 0
    int b = 2;                                   // adds data, no new virtual
};

int main() {
    B b;
    A* p = &b;

    // Itanium ABI: the first word of the object is the vptr; it points at "slot 0" of the vtable.
    void** vtable = *reinterpret_cast<void***>(p);

    auto offset_to_top = reinterpret_cast<std::ptrdiff_t>(vtable[-2]);
    auto* ti           = static_cast<const std::type_info*>(vtable[-1]);
    std::printf("offset-to-top : %td\n", offset_to_top);
    std::printf("typeinfo      : %s (same as typeid(b)? %d)\n", ti->name(), ti == &typeid(b));

    // Call through the table by hand: `this` is passed as the first argument.
    std::puts("calling through the table slots by hand:");
    using Fn = void (*)(A*);
    reinterpret_cast<Fn>(vtable[0])(p);          // slot 0 = f  -> B::f   (overridden)
    reinterpret_cast<Fn>(vtable[1])(p);          // slot 1 = g  -> A::g   (inherited)

    // The same table, reached the normal way:
    p->f(); p->g();

    // A different object of the same dynamic type shares THE SAME vtable (one per class, not per object):
    B b2;
    void** vtable2 = *reinterpret_cast<void***>(static_cast<A*>(&b2));
    std::printf("two B objects share one vtable: %d\n", vtable == vtable2);

    // A different class has a different vtable:
    A a;
    void** vtable_a = *reinterpret_cast<void***>(&a);
    std::printf("A and B vtables differ: %d\n", vtable != vtable_a);

    std::printf("sizeof(A) = %zu (vptr 8 + int 4 + padding 4),  sizeof(B) = %zu\n", sizeof(A), sizeof(B));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
offset-to-top : 0
typeinfo      : 1B (same as typeid(b)? 1)
calling through the table slots by hand:
B::f
A::g
B::f
A::g
two B objects share one vtable: 1
A and B vtables differ: 1
sizeof(A) = 16 (vptr 8 + int 4 + padding 4),  sizeof(B) = 16
```

`ti->name()` prints the *mangled* name (`1B`), a visible reminder that `type_info::name()` is implementation-defined. The slot numbers match **declaration order**, with the overridden `f` replaced in place: this is *the* reason adding or reordering a virtual function breaks binary compatibility (Chapter 37).

### Experiment 2 🧩: Layout under single, multiple and virtual inheritance

```cpp
// @test run -std=c++23 -O0
#include <cstdio>

struct A { virtual void f() {} int a = 1; };
struct B : A { int b = 2; };                                // single: same vptr as A
struct X { virtual void x() {} int xx = 3; };
struct M : A, X { };                                        // multiple: TWO vptrs, X subobject at an offset
struct V1 : virtual A { int v1 = 5; };                      // virtual base: vptr + shared A elsewhere
struct V2 : virtual A { int v2 = 6; };
struct D : V1, V2 { int d = 7; };                           // the diamond with a SINGLE shared A

struct Empty {};
struct NonPoly { int i; };
struct Poly { virtual void f() {} };

int main() {
    std::printf("sizeof: Empty=%zu NonPoly=%zu Poly=%zu A=%zu B=%zu M=%zu V1=%zu D=%zu\n",
                sizeof(Empty), sizeof(NonPoly), sizeof(Poly), sizeof(A), sizeof(B), sizeof(M), sizeof(V1), sizeof(D));

    M m;
    A* pa = &m; X* px = &m;
    std::printf("M: A* == &m ? %d ;  X* - A* = %td bytes  (pointer conversion ADJUSTS the address)\n",
                (void*)pa == (void*)&m, (char*)px - (char*)pa);

    D d;
    V1* v1 = &d; V2* v2 = &d; A* a = &d;
    std::printf("D: V1* offset %td, V2* offset %td, shared A* offset %td\n",
                (char*)v1 - (char*)&d, (char*)v2 - (char*)&d, (char*)a - (char*)&d);
    std::printf("D has ONE A: (V1*)->A and (V2*)->A are the same object: %d\n",
                static_cast<A*>(static_cast<V1*>(&d)) == static_cast<A*>(static_cast<V2*>(&d)));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sizeof: Empty=1 NonPoly=4 Poly=8 A=16 B=16 M=32 V1=32 D=48
M: A* == &m ? 1 ;  X* - A* = 16 bytes  (pointer conversion ADJUSTS the address)
D: V1* offset 0, V2* offset 16, shared A* offset 32
D has ONE A: (V1*)->A and (V2*)->A are the same object: 1
```

The numbers are Itanium-ABI facts: the standard only promises that the pointer conversions yield pointers to the right sub-objects. In particular the shared `A` in `D` lives **at the end** (offset 32 here), found at run time through the `vbase offset` slot, and `sizeof(V1)` is 32 rather than 24 because `V1` needs its own vptr, its `int`, and room for the virtual `A` (with *its* vptr).

### Experiment 3 🔧: What a virtual call compiles to, and when it disappears

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fcf-protection=none filter=call_virtual,call_final,call_known,call_nonvirtual
struct Base      { virtual int f() const; int h() const { return 1; } };
struct Derived   : Base { int f() const override; };
struct Leaf final : Base { int f() const override { return 7; } };

int call_virtual(const Base& b)    { return b.f(); }            // truly dynamic: load vptr, load slot, call
int call_final(const Leaf& l)      { return l.f(); }            // Leaf is final: devirtualized and inlined -> constant
int call_known()                   { Derived d; return d.f(); }   // dynamic type known locally: direct call (f is defined elsewhere)
int call_nonvirtual(const Base& b) { return b.h(); }            // not virtual: inlined constant
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
call_virtual(Base const&):
	mov	rax, QWORD PTR [rdi]
	jmp	[QWORD PTR [rax]]

call_final(Leaf const&):
	mov	eax, 7
	ret

call_known():
	sub	rsp, 24
	lea	rax, vtable for Derived[rip+16]
	lea	rdi, 8[rsp]
	mov	QWORD PTR 8[rsp], rax
	call	Derived::f() const@PLT
	add	rsp, 24
	ret

call_nonvirtual(Base const&):
	mov	eax, 1
	ret
```

Reading it:

- `call_virtual`: two instructions, `mov rax, [rdi]` (load the vptr) and `jmp [rax]` (load slot 0 and tail-jump through it); nothing can be inlined. For a later slot the displacement changes: `jmp [rax+8*slot]`.
- `call_final`: the whole function is `mov eax, 7`: `final` made the call *statically known* and inlinable. **This is the practical value of `final`**: it converts a dynamic call to a constant.
- `call_known`: a **direct** `call Derived::f()` because the compiler can see the object's exact type; the body is not available so it cannot be inlined, but the indirect branch is gone. Note the leftover `lea rax, vtable for Derived[rip+16]` / `mov [rsp+8], rax`: the constructor's **vptr store** is still emitted (the callee might read it), a small reminder that polymorphic construction always writes the vptr. The `+16` skips `offset-to-top` and `typeinfo` so the pointer lands on slot 0, exactly as the §4 picture shows.
- `call_nonvirtual`: constant `1`.

### Experiment 4 🧩: Symbols: where vtables and typeinfo live, and thunks

```bash
cat > sym.cpp <<'EOF'
struct A { virtual void f(); virtual ~A(); int a; };     // key function: A::f (first non-inline, non-pure virtual)
struct X { virtual void x() {} virtual ~X() {} int b; };   // no key function: everything inline
struct M : A, X { void f() override; void x() override; };
void A::f() {}  A::~A() {}  void M::f() {}  void M::x() {}
EOF
g++-14 -std=c++23 -O0 -c sym.cpp -o sym.o
nm -C sym.o | grep -E "vtable|typeinfo|thunk"       # 'V' = weak object, 'D' = data (strong), 'r' = read-only local
```

```text
# output (GCC 14.2, run by hand; this block is not auto-verified)
V typeinfo for A     V typeinfo for M     V typeinfo for X
V typeinfo name for A / M / X
V vtable for A       V vtable for M       V vtable for X
U vtable for __cxxabiv1::__class_type_info
U vtable for __cxxabiv1::__vmi_class_type_info
T non-virtual thunk to M::x()
W non-virtual thunk to M::~M()   (twice: complete and deleting destructor)

$ objdump -d -C sym.o     # the thunk, entire body:
non-virtual thunk to M::x():
    endbr64
    sub    $0x10,%rdi        # this -= 16  (convert X* back to M*)
    jmp    M::x()
```

Look for: (1) the **thunk** `non-virtual thunk to M::x()`: the stub that subtracts 16 from `this` (X's offset inside `M`, matching Experiment 2) and jumps to the real `M::x`; (2) *two* thunks for `~M` (complete and deleting destructor); (3) typeinfo for `M` references `__vmi_class_type_info` (the "virtual/multiple inheritance" typeinfo class) while a single-inheritance class would use `__si_class_type_info`.

**The key-function rule, checked from the other side.** A second file that only *uses* these classes shows who emits what:

```bash
g++-14 -std=c++23 -O0 -c use.cpp -o use.o     # uses A (key function A::f declared, defined elsewhere) and X (all inline)
nm -C use.o | grep -E "vtable for (A|X)$"
#           U vtable for A        <- NOT emitted here: left for the TU that defines A::f
# 0000...   V vtable for X        <- emitted here, weak: every TU that needs X's vtable gets its own copy; the linker merges them
```

(In the defining TU GCC 14 happens to put *all* vtables in COMDAT groups, so `nm` shows `V` for `A` there too; the observable difference is in the *other* TU.)

### Experiment 5 ⚖️: Virtual calls in constructors and destructors

```cpp
// @test run -std=c++23 -O0
#include <cstdio>

struct Base {
    Base()  { std::puts("Base()  -> calling who() from the constructor:"); who(); }
    virtual ~Base() { std::puts("~Base() -> calling who() from the destructor:"); who(); }
    virtual void who() const { std::puts("    Base::who"); }
};
struct Derived : Base {
    Derived()  { std::puts("Derived()"); who(); }
    ~Derived() override { std::puts("~Derived()"); who(); }
    void who() const override { std::puts("    Derived::who"); }
};

int main() {
    {
        Derived d;
        std::puts("-- fully constructed, calling through a base reference:");
        const Base& b = d;
        b.who();
        std::puts("-- leaving scope:");
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
Base()  -> calling who() from the constructor:
    Base::who
Derived()
    Derived::who
-- fully constructed, calling through a base reference:
    Derived::who
-- leaving scope:
~Derived()
    Derived::who
~Base() -> calling who() from the destructor:
    Base::who
```

During `Base()` the call goes to **`Base::who`** even though the object *will be* a `Derived`; the same during `~Base()` after `~Derived()` has run. Mechanism (🧩): each constructor stores its own vtable address into the vptr before running its body. The standard rule exists because the derived members **do not exist yet** (or no longer exist): a call to `Derived::who` could read uninitialized data. A *pure* virtual call here is undefined behaviour and, on Itanium, typically aborts with `pure virtual method called`.

### Experiment 6 🔧: What do `virtual`, `dynamic_cast` and `typeid` cost?

Honest microbenchmarks need a warning: **a virtual call in a tight loop measures branch prediction, not "virtual dispatch"**. We measure four situations with the loops compiled in separate `noinline` functions (the lesson of Chapter 14's benchmark trap).

```cpp
// @test run -std=c++23 -O2
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <typeinfo>
#include <variant>
#include <vector>

struct Shape { virtual ~Shape() = default; virtual int area() const = 0; };
struct Sq  final : Shape { int s; explicit Sq(int s) : s(s) {}  int area() const override { return s * s; } };
struct Rect final : Shape { int w, h; Rect(int w, int h) : w(w), h(h) {} int area() const override { return w * h; } };
struct Tri final : Shape { int b, h; Tri(int b, int h) : b(b), h(h) {} int area() const override { return b * h / 2; } };
struct Circ final : Shape { int r; explicit Circ(int r) : r(r) {} int area() const override { return 3 * r * r; } };
struct Plain : Shape { int area() const override { return 1; } };          // a non-final class for the dynamic_cast test
struct Plain2 : Plain {};

using Clock = std::chrono::steady_clock;
template <class F> double best_ms(F&& f, long& sink, int reps = 7) {
    double best = 1e30;
    for (int i = 0; i < reps; ++i) {
        auto t0 = Clock::now(); sink += f();
        best = std::min(best, std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    }
    return best;
}

[[gnu::noinline]] long sum_virtual(const std::vector<Shape*>& v) { long s = 0; for (auto* p : v) s += p->area(); return s; }
[[gnu::noinline]] long sum_dyncast(const std::vector<Shape*>& v) { long s = 0; for (auto* p : v) if (auto* q = dynamic_cast<const Plain*>(p)) s += q->area(); return s; }
[[gnu::noinline]] long sum_typeid(const std::vector<Shape*>& v) { long s = 0; for (auto* p : v) if (typeid(*p) == typeid(Sq)) s += 1; return s; }
[[gnu::noinline]] long sum_static(const std::vector<Sq*>& v) { long s = 0; for (auto* p : v) s += p->s * p->s; return s; }   // devirtualized by hand

using Var = std::variant<Sq, Rect, Tri, Circ>;
[[gnu::noinline]] long sum_variant(const std::vector<Var>& v) {
    long s = 0;
    for (auto& x : v) s += std::visit([](const auto& a) { return a.area(); }, x);
    return s;
}

int main() {
    constexpr int N = 1'000'000;
    std::mt19937 rng(7);

    // Storage: one allocation per object, in creation order (so layout is the same for both mixes).
    std::vector<std::unique_ptr<Shape>> owner;
    std::vector<Shape*> mono, mixed;
    std::vector<Sq*> only_sq;
    std::vector<Var> var_mono, var_mixed;
    for (int i = 0; i < N; ++i) {
        owner.push_back(std::make_unique<Sq>(i % 10 + 1));
        mono.push_back(owner.back().get());
        only_sq.push_back(static_cast<Sq*>(owner.back().get()));
        var_mono.emplace_back(Sq(i % 10 + 1));
    }
    for (int i = 0; i < N; ++i) {
        switch (rng() % 4) {
            case 0: owner.push_back(std::make_unique<Sq>(3));     var_mixed.emplace_back(Sq(3)); break;
            case 1: owner.push_back(std::make_unique<Rect>(3, 4)); var_mixed.emplace_back(Rect(3, 4)); break;
            case 2: owner.push_back(std::make_unique<Tri>(3, 4));  var_mixed.emplace_back(Tri(3, 4)); break;
            default: owner.push_back(std::make_unique<Circ>(2));   var_mixed.emplace_back(Circ(2)); break;
        }
        mixed.push_back(owner.back().get());
    }
    std::vector<Shape*> plain_objs; auto p1 = std::make_unique<Plain>(); auto p2 = std::make_unique<Plain2>();
    for (int i = 0; i < N; ++i) plain_objs.push_back(i % 2 ? static_cast<Shape*>(p1.get()) : static_cast<Shape*>(p2.get()));

    long sink = 0;
    std::printf("N = %d per test, best of 7, -O2\n", N);
    std::printf("  direct (no virtual, Sq* known)        %7.2f ms\n", best_ms([&] { return sum_static(only_sq); }, sink));
    std::printf("  virtual, monomorphic (all Sq)         %7.2f ms\n", best_ms([&] { return sum_virtual(mono); }, sink));
    std::printf("  virtual, 4 types random order         %7.2f ms\n", best_ms([&] { return sum_virtual(mixed); }, sink));
    std::printf("  std::variant visit, all Sq            %7.2f ms\n", best_ms([&] { return sum_variant(var_mono); }, sink));
    std::printf("  std::variant visit, 4 types random    %7.2f ms\n", best_ms([&] { return sum_variant(var_mixed); }, sink));
    std::printf("  typeid(*p) == typeid(Sq)              %7.2f ms\n", best_ms([&] { return sum_typeid(mono); }, sink));
    std::printf("  dynamic_cast<const Plain*> (success)  %7.2f ms\n", best_ms([&] { return sum_dyncast(plain_objs); }, sink));
    std::printf("  dynamic_cast<const Plain*> (fail)     %7.2f ms\n", best_ms([&] { return sum_dyncast(mono); }, sink));
    return sink == 12345 ? 1 : 0;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
N = 1000000 per test, best of 7, -O2
  direct (no virtual, Sq* known)           4.94 ms
  virtual, monomorphic (all Sq)            4.83 ms
  virtual, 4 types random order            9.85 ms
  std::variant visit, all Sq               3.43 ms
  std::variant visit, 4 types random       9.23 ms
  typeid(*p) == typeid(Sq)                 4.31 ms
  dynamic_cast<const Plain*> (success)    14.70 ms
  dynamic_cast<const Plain*> (fail)       22.17 ms
```

Measured on one machine (GCC 14.2, `-O2`; the output above): monomorphic virtual calls cost the same as the direct version (about 4.9 ms for 10⁶ objects, i.e. these loops are bounded by *memory traffic* over separately allocated objects, not by the indirect call), while mixing **four types in random order doubles** the time (9.9 ms) because the indirect branch is mispredicted. `std::variant` + `visit` is faster when monomorphic (3.4 ms: contiguous values, no pointer chase) but suffers the **same** misprediction penalty when mixed (9.2 ms): it is a jump table or compare chain, not magic. `typeid(*p) == typeid(Sq)` costs about the same as a virtual call, while `dynamic_cast` is **about 3× (success, 14.7 ms) to 4.5× (failure, 22.2 ms)** slower than a virtual call; the failing cast is the worst case because it must walk the entire hierarchy before giving up. So `dynamic_cast` is slow relative to alternatives, but at tens of nanoseconds per cast, not catastrophic; it becomes a problem only in hot paths over millions of objects. Remember these are memory-bound at N = 10⁶ (each object is a separate heap allocation: Chapter 27), so read the shape, not the absolute numbers.

---

## 8. Assembly / runtime investigation

Experiment 3 is the assembly investigation. Three further tools:

```bash
# (1) The vtable as the linker sees it: relocation entries show which functions fill each slot
g++-14 -std=c++23 -O0 -c prog.cpp -o prog.o
objdump -R prog.o 2>/dev/null | head ; readelf -r prog.o | grep -i vtable
objdump -d -C prog.o | grep -A8 "<vtable for"            # not disassembly: vtables are data; use objdump -s -j .data.rel.ro.

# (2) Print the Itanium class layout the compiler computed (GCC; the most useful single flag in this chapter)
g++-14 -std=c++23 -fdump-lang-class -c prog.cpp -o /dev/null && cat prog.cpp.*class
# Clang:
clang++-18 -std=c++23 -Xclang -fdump-record-layouts -Xclang -fdump-vtable-layouts -fsyntax-only prog.cpp

# (3) Count indirect-branch mispredictions on a real workload (Linux perf; the event name differs by CPU vendor)
perf stat -e branches,branch-misses,instructions ./a.out
perf record -e branch-misses ./a.out && perf annotate --stdio | grep -B2 -A2 "call.*\*"
```

`-fdump-lang-class` (GCC) and `-fdump-vtable-layouts` (Clang) print every vtable slot, every thunk and every base offset: use them whenever you debug an ABI question or a layout surprise rather than trusting your mental model.

---

## 9. Implementation exercise

Build a **hand-rolled vtable** class system in C++ with no `virtual`, to internalize the mechanism:

1. A `struct VTable { void (*draw)(const void*); double (*area)(const void*); void (*destroy)(void*); const std::type_info* type; }` and a base `struct Shape { const VTable* vt; }` (the vptr at offset 0).
2. `Circle` and `Rect` as structs whose **first member is a `Shape`** (so the struct pointer is also a `Shape*`: standard-layout common initial sequence rules), plus a static `constexpr VTable` for each; constructors set `vt`.
3. Free functions `draw(Shape*)`, `area(Shape*)`, `destroy(Shape*)` that dispatch through `vt`. Verify your system produces the same observable output as the equivalent `virtual` version.
4. Add a **second interface** (`Serializable`) and show you need a second vptr in the object, an offset adjustment on conversion, and a thunk-like adjuster: you have re-derived multiple inheritance.
5. Compare the assembly of your `area(Shape*)` with a real virtual call. They should be near identical.

<details>
<summary><strong>Solution sketch</strong> (steps 1–3)</summary>

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <typeinfo>

struct Shape;
struct VTable {
    double (*area)(const Shape*);
    void   (*print)(const Shape*);
    const std::type_info* type;
};
struct Shape { const VTable* vt; };                       // "vptr" at offset 0

inline double area(const Shape* s)  { return s->vt->area(s); }
inline void   print(const Shape* s) { s->vt->print(s); }

struct Circle {
    Shape base; double r;                                // first member: &circle == &circle.base
    explicit Circle(double r);
};
struct Rect {
    Shape base; double w, h;
    Rect(double w, double h);
};

static double circle_area(const Shape* s) { auto* c = reinterpret_cast<const Circle*>(s); return 3.0 * c->r * c->r; }
static void   circle_print(const Shape* s) { auto* c = reinterpret_cast<const Circle*>(s); std::printf("Circle r=%.1f\n", c->r); }
static double rect_area(const Shape* s) { auto* r = reinterpret_cast<const Rect*>(s); return r->w * r->h; }
static void   rect_print(const Shape* s) { auto* r = reinterpret_cast<const Rect*>(s); std::printf("Rect %.1fx%.1f\n", r->w, r->h); }

constexpr VTable circle_vt{circle_area, circle_print, &typeid(Circle)};
constexpr VTable rect_vt{rect_area, rect_print, &typeid(Rect)};
Circle::Circle(double r_) : base{&circle_vt}, r(r_) {}
Rect::Rect(double w_, double h_) : base{&rect_vt}, w(w_), h(h_) {}

int main() {
    Circle c(2); Rect r(3, 4);
    const Shape* shapes[] = {&c.base, &r.base};
    for (auto* s : shapes) { print(s); std::printf("  area = %.1f  (%s)\n", area(s), s->vt->type->name()); }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
Circle r=2.0
  area = 12.0  (6Circle)
Rect 3.0x4.0
  area = 12.0  (4Rect)
```

This is the C idiom (`FILE`, `file_operations`, GObject) and the exact structure the compiler builds for you. The C++ version adds: automatic vptr installation in the constructor chain, the override rules, thunks for multiple inheritance, `dynamic_cast`/`typeid` integration, and (importantly) the type checking that `reinterpret_cast` above lacks.

</details>

---

## 10. Real-world example

| Where | How virtual dispatch is used |
|---|---|
| **Qt** | `QObject` and `QWidget`: `event()`, `paintEvent()`, `QAbstractItemModel::data()`; the *moc-generated* `metaObject()`/`qt_metacall()` are virtual (Chapter 48) |
| **LLVM / Clang** | Deliberately avoids virtual functions in AST node hierarchies: uses its own **RTTI-free** `isa<>`/`dyn_cast<>` with a `Kind` enum, so a node's class check is a compare, not a `dynamic_cast` |
| **Linux kernel (C)** | The same pattern hand-written: `struct file_operations` |
| **Game engines** | Entity/component with virtual `update()`; moved to data-oriented (Chapter 27) when profiles show cost |
| **Databases / query engines** | Virtual `next()` per operator (Volcano model) → vectorized or compiled execution to amortize the per-row indirect call |
| **Plug-in systems, drivers, GUI toolkits** | Open sets of types loaded at run time: the case virtual functions are *for* |
| **Compilers' CFI** | Clang's `-fsanitize=cfi-vcall` verifies that a virtual call's target is a valid override: security hardening of exactly this mechanism |

> **Opinion.** Runtime polymorphism is the right tool when the **set of types is open** (plug-ins, user-extensible hierarchies) or when you need a **stable binary interface** across a library boundary (a pure-virtual interface class is the classic C++ ABI-stable API, Chapter 37). It is the wrong tool when the **set is closed and known**: use `std::variant` + `visit` (same cost model, no heap, value semantics) or a template. Default recommendations: make leaf classes `final`; prefer **non-virtual interface (NVI)** (public non-virtual function calling a private virtual one) to preserve invariants; keep hierarchies **shallow** (two levels); never put data in an interface; **avoid virtual inheritance** except for pure-interface "mixins" with no data, where it is cheap and harmless; and **never** call virtual functions from constructors. If you catch yourself writing `dynamic_cast` for control flow, the abstraction is wrong (add a virtual function, or use a visitor/variant).

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Base without `virtual ~Base()` and `delete` through base pointer | **UB**: derived destructor skipped, leaks, corruption | Virtual destructor, or protected non-virtual |
| Signature mismatch (`const`, parameter type) → hides instead of overrides | Base version silently called | **Always** `override`; `-Wsuggest-override`, `-Woverloaded-virtual` |
| Virtual call in a constructor/destructor | Base version runs; pure virtual → crash | Two-phase init (factory), or non-virtual helper |
| Default arguments on virtual functions differing between base and derived | Surprising argument values | No defaults on virtuals; overload instead |
| Slicing: `Base b = derived;` | Derived part and dynamic type lost | Pass by reference/pointer; make base abstract/non-copyable |
| `dynamic_cast` in hot paths | Roughly 3–4× slower than a virtual call in Experiment 6 (more for deep or multiple-inheritance hierarchies) | Add a virtual function, a `Kind` tag, or a `variant` |
| `dynamic_cast` across shared-library boundaries without exported typeinfo | Cast returns null for the "same" type (duplicated typeinfo with hidden visibility) | Export typeinfo; consistent `-fvisibility`; or avoid RTTI across `.so` |
| Adding/reordering a virtual function in a released library class | Old binaries call the wrong slot | ABI discipline (Chapter 37): append at the end only if no derived classes exist outside, or use pImpl/new class |
| Deep hierarchies / "fragile base class" | Changes ripple; override-vs-extension ambiguity | Composition; shallow interface hierarchies |
| Virtual inheritance for data sharing | Extra indirection per access, complicated constructors, `static_cast` errors | Avoid; make the shared base a pure interface |
| Returning a reference/pointer from a virtual function into an object that dies | Dangling | Value semantics or shared ownership explicitly |
| Copying polymorphic objects through the base | Can't (no virtual copy) | `virtual std::unique_ptr<Base> clone() const` or type erasure (Chapter 21) |
| Missing definition of the key function | `undefined reference to 'vtable for X'` | Define the first non-inline virtual function |
| Expecting `-O2` to devirtualize everything | Indirect calls remain | `final`, LTO, PGO (speculative devirtualization), or restructure |

---

## 12. Exercises

1. **Dump the layout.** Use `-fdump-lang-class` (GCC) or `-fdump-vtable-layouts` (Clang) on three hierarchies: single, multiple, diamond with virtual bases. Annotate each dump with the picture from §4/§5.
2. **Where did the thunk go?** In Experiment 4, find `M::x`'s thunk and disassemble it (`objdump -d -C`). Explain each instruction. Then change `M` to inherit `X` first and observe which functions need thunks.
3. **Count dispatches.** Use `perf stat -e branches,branch-misses` on Experiment 6's mixed vs monomorphic runs. Compute mispredictions per call and relate that to the time difference.
4. **`final` experiment.** Remove `final` from the shape classes in Experiment 6, and compile with `-O2 -flto` and with `-fdevirtualize-speculatively`/`-O3`. Which loops change? Verify in the assembly.
5. **Hand-rolled multiple inheritance** (Exercise 9.4): implement two interfaces with two vptrs and an adjuster; compare layout with the compiler's.
6. **Stable-ABI interface.** Design a pure-virtual C++ interface (`IWidget`) loaded from a `.so` through an `extern "C" IWidget* create()` factory. List everything you must freeze (vtable order, destructor, calling convention) and everything that can still change.
7. **Replace with `variant`.** Take a closed three-class hierarchy and convert it to `std::variant` + `visit`. Compare lines of code, `sizeof`, and run-time for N=10⁶ in monomorphic and random mixes.
8. **NVI.** Rewrite an interface with the non-virtual interface idiom and add pre/post-condition checks in the public wrapper. Show a bug that NVI prevents.
9. **RTTI off.** Compile a small program with `-fno-rtti`; which constructs stop compiling? What does the typeinfo symbol table look like (`nm -C | grep typeinfo`)? How would you implement `dyn_cast`?

---

## 13. Challenge: a cost model for dispatch

Design and run a benchmark suite that answers: *"For my workload, which dispatch mechanism should I use?"* Cover: virtual call; `std::function`; function pointer; `std::variant` + `visit`; CRTP/template; and a hand-written `switch` on a tag. Vary: number of types (1, 2, 4, 16, 64), type-mix predictability (sorted, bursty, random), object storage (contiguous array of values vs scattered heap pointers), and body size (tiny vs 200 instructions). Report for each cell the time per call and the misprediction rate from `perf`. Produce the decision chart you wish you had when you started this chapter, and test one prediction of it on real code from your own project.

---

## 14. Knowledge check

1. What does the standard say about vtables? What is the Itanium ABI's layout of a vtable?
2. Why does the vtable have two destructor entries?
3. What is `offset-to-top`, and when is it nonzero?
4. What is a thunk, and in which two situations does the compiler generate one?
5. Why does a virtual call in a constructor not reach the derived override?
6. Why are default arguments not "virtual"?
7. What does `final` allow the optimizer to do? Give two other ways a virtual call can be devirtualized.
8. Why is `dynamic_cast` slower than a virtual call (about 3–4× in Experiment 6)?
9. How does virtual inheritance change member access and construction?
10. What is the key function, and what link error can it cause?
11. When is runtime polymorphism the *right* choice over `variant` or templates?
12. Why might adding a new virtual function to a released library class be a breaking change even if source code still compiles?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. The standard defines behaviour (final overrider semantics) and says nothing about vtables or layout. Itanium: one vtable per polymorphic class: [vbase offsets…][offset-to-top][typeinfo ptr] then function pointers in declaration order; the vptr points at the first function slot.
2. One entry (complete-object destructor) runs the destructor body and base destructors; the other (deleting destructor) also calls `operator delete`. `delete p` uses the deleting one.
3. The (signed) distance from this sub-object's address to the complete object's address; zero for the primary base, nonzero (negative) for secondary bases in multiple inheritance; used by `dynamic_cast<void*>` and by thunks.
4. A small stub that adjusts `this` then jumps to the real function. Generated for overriders called through a non-primary base (multiple inheritance, `this`-adjustment) and for covariant return types / virtual bases (virtual thunks that read the adjustment from the vtable).
5. The standard says the dynamic type during a base constructor is the base, because derived members are not yet initialized. Mechanically, each constructor stores its own vtable into the vptr before its body, so the call resolves to the base's version.
6. They are substituted at compile time using the *static* type of the call expression; only the function body selection is dynamic.
7. It lets the compiler call the function directly and inline it (the dynamic type cannot differ). Others: the dynamic type is known from construction in the same function; whole-program/LTO analysis proving a closed hierarchy; profile-guided speculative devirtualization (guarded direct call).
8. It walks the class hierarchy graph comparing type_info objects (potentially including string comparisons of mangled names), instead of two loads and an indirect jump; it also handles multiple/virtual inheritance paths.
9. Members of the virtual base are located via a vbase offset read from the vtable at run time (extra indirection); the most-derived class constructs the virtual base, and intermediate classes' constructors take a hidden flag to skip it.
10. The first non-inline, non-pure virtual function; the vtable and typeinfo are emitted in its TU. If you declare it but never define it, you get "undefined reference to `vtable for X`".
11. Open sets of types (plug-ins, user extensions), a stable binary interface across shared-library boundaries, or when the set of types is only known at run time, or when compile-time/code-size of templates is unacceptable.
12. It changes vtable slot numbering (and size) for the class and its derived classes; previously compiled client code or derived classes in other binaries use the old slot indices and call the wrong function.

</details>

---

[← Previous: Chapter 18](../part-07-compile-time/18-reflection-landscape.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 20 — Static polymorphism →](20-static-polymorphism.md)
