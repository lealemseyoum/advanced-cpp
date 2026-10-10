# Chapter 20 — Static Polymorphism

> **Part VIII · Polymorphism** &nbsp;|&nbsp; **Level 3–4** &nbsp;|&nbsp; **≈ 6 hours**
> **Prerequisites:** [Chapter 8](../part-04-generic-programming/08-templates-deep-dive.md), [Chapter 10](../part-04-generic-programming/10-concepts.md), [Chapter 19](19-runtime-polymorphism.md) &nbsp;|&nbsp; **Standards:** C++98 (templates, CRTP), C++11 (`static_assert`, `decltype`), C++17 (`if constexpr`), C++20 (concepts), **C++23 (deducing `this`)** &nbsp;|&nbsp; **Tools:** `g++-14`, `clang++-18`, `nm`

[← Previous: Chapter 19](19-runtime-polymorphism.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 21 — Type erasure →](21-type-erasure.md)

---

**In one sentence:** static polymorphism resolves *which code runs* during compilation, so there is no vptr, no indirect call and everything can inline, at the price of **the set of types being known at compile time** and of **one copy of the code per type**.

**By the end of this chapter you can:**

- choose between overloads, templates, concepts, tag dispatch, `if constexpr`, CRTP and deducing `this` for a given problem
- write and debug CRTP mixins and say why most of them can now be written without CRTP
- explain what static polymorphism *cannot* do (heterogeneous containers, run-time selection, binary interfaces)
- measure the real cost trade-off: speed, code size, compile time
- fill in the five-way comparison *virtual / templates / CRTP / concepts / type erasure*

---

## 1. Problem

Chapter 19's virtual functions solve "many types, one interface" at run time. But they impose costs you may not need to pay:

```text
   cost of virtual dispatch                                 when it hurts
   ─────────────────────────────────────────────────────    ─────────────────────────────────────────────
   +8 bytes per object (vptr); object is no longer trivial  millions of small objects (particles, nodes)
   indirect call; no inlining across it                     tiny hot functions (comparators, accessors)
   dynamic type must be a class with a common base          you want int, double and your own types alike
   heap allocation + pointers (to get polymorphism)         cache misses; ownership questions
```

If every use site **knows the concrete type at compile time** (a container of one type, a function called with a statically known policy), none of that is needed. The compiler can pick the right function itself.

---

## 2. Historical context

| Year | Mechanism | Notes |
|---|---|---|
| 1979 | Function overloading | The first static polymorphism: same name, different parameter types |
| 1990–94 | **Templates** + STL | Parametric polymorphism: `std::sort` works for anything with the right *operations* ("duck typing at compile time") |
| 1995 | **CRTP**: *Curiously Recurring Template Pattern* (Coplien named it; Jim Coplien observed it in early templates) `struct D : Base<D>` | Gives the base class access to the derived type without virtual calls |
| 2000s | **Tag dispatch** (`std::iterator_category`); `enable_if` SFINAE | Select an implementation by *trait* (Chapter 9) |
| 2011 | `decltype`/`auto`/`static_assert` | Cleaner compile-time contracts |
| 2017 | `if constexpr` | One function body, branches chosen at compile time (Chapter 8) |
| 2020 | **Concepts** | Name the interface, check it, overload on it, get readable errors (Chapter 10) |
| **2023** | **Deducing `this`** (P0847) | A member function can take its object as an explicit template parameter: **removes the main reason for CRTP** |

Each step moved *interface checking* earlier and *syntax* toward plain code. The trend: **templates + concepts for the interface, deducing `this` for mixins, CRTP only where legacy or specific needs remain.**

---

## 3. Modern solution

Five tools, one idea: *the compiler picks the implementation*.

```cpp
// 1. Overloading / templates: implicit interface
template <class Shape> double total_area(const std::vector<Shape>& v);

// 2. Concepts: explicit, checked interface
template <class T> concept HasArea = requires(const T& t) { { t.area() } -> std::convertible_to<double>; };
double describe(const HasArea auto& s);

// 3. if constexpr: one body, compile-time branch
template <class T> auto serialize(const T& v) { if constexpr (std::is_arithmetic_v<T>) ...; else ...; }

// 4. CRTP: base reuses the derived class's operations
template <class D> struct Comparable { friend bool operator>(const D& a, const D& b) { return b < a; } };
struct Version : Comparable<Version> { bool operator<(const Version&) const; };

// 5. Deducing this (C++23): same effect, no template on the base
struct Printable { void print(this const auto& self) { std::cout << self.to_string(); } };
```

---

## 4. Mental model

### Where the dispatch happens

```text
   virtual:          call site ──► vptr ──► vtable ──► target           (decided at RUN time, per call)
                                              ▲
   static:           call site ──► target      (decided at COMPILE time; each instantiation is separate code)

   Template<T1>::f()    Template<T2>::f()    Template<T3>::f()          ← three copies, three optimizations
```

### The trade, as a table

| | Virtual | Templates / concepts | CRTP | Deducing `this` | Type erasure |
|---|---|---|---|---|---|
| Dispatch decided | run time | compile time | compile time | compile time | run time |
| Set of types | open | closed (each call site) | closed | closed | open |
| Heterogeneous container | yes (pointers) | **no** (one `T`) | **no** | **no** | **yes** (by value) |
| Per-object cost | vptr (+8 B) | none | none | none | pointer(s) + small buffer |
| Per-call cost | indirect call | direct, inlinable | direct, inlinable | direct, inlinable | indirect call |
| Interface visible to the compiler as | base class | concept / implicit | base template | member template | wrapper class |
| Error messages | clear | concepts: clear; else terrible | terrible-ish | better | clear |
| Code size | 1 copy | **N copies** | **N copies** | **N copies** | 1 wrapper + N small thunks |
| Binary interface (shared library) | **yes** | no (templates in headers) | no | no | only if wrapper is non-template |
| Needs headers | no | yes (templates in headers) | yes | yes | mostly no |

> **Rule.** Choose **static** when types are known at the point of use and speed or value semantics matter. Choose **dynamic** (virtual or type erasure) when the type is genuinely chosen at run time, or when the code must live behind a stable binary boundary.

---

## 5. Language rules

### 5.1 CRTP  `[temp.inst]`

```cpp
template <class Derived>
struct Base {
    void interface() { static_cast<Derived*>(this)->implementation(); }   // "virtual call", resolved at compile time
};
struct Impl : Base<Impl> { void implementation(); };
```

Rules that bite:

1. **`Derived` is incomplete inside `Base<Derived>`'s class body.** Member *declarations* of `Base` cannot use `Derived`'s members (e.g. `typename Derived::value_type` as a member type or return type fails). Member *function bodies* are instantiated lazily, **after** `Derived` is complete, and can use anything. Use a trailing `decltype(auto)` return or a helper trait evaluated later.
2. **`static_cast<Derived*>(this)` is only valid if the object really is a `Derived`.** If you write `struct Wrong : Base<Impl>`, the cast is UB. Defend with a `private` constructor in `Base` and `friend Derived` so only `Derived` can construct it.
3. **Each `Base<D>` is a different class**: there is no common base to put in a container. A CRTP hierarchy gives code reuse, **not** run-time substitutability.
4. Empty-base optimization still applies (`sizeof` of a CRTP mixin with no data adds nothing, Experiment 1).

### 5.2 Deducing `this`  `[dcl.fct]` (C++23)

```cpp
struct Printable {
    template <class Self> void print(this Self&& self) { std::cout << self.to_string(); }   // Self = the most-derived type
};
struct Point : Printable { std::string to_string() const; };
Point{}.print();      // Self deduced as Point&& - same effect as CRTP, no `Printable<Point>`
```

`this` becomes an explicit first parameter whose type is deduced from the object expression, so a base-class member function **sees the derived type**. It also deduces **value category and cv** (one function replaces `const`/non-`const`/`&`/`&&` overloads) and enables recursive lambdas. Support: GCC 14, Clang 18, MSVC 19.32.

### 5.3 Overload resolution as polymorphism  `[over.match]`

For `f(x)` the compiler builds an overload set, discards non-viable candidates (including constraints that fail), and picks the best by conversion sequences, then **more specialized template**, then **more constrained** (Chapter 10's subsumption). Key interplay:

- Constrained overloads let you provide **several implementations selected by what the type can do**: `advance` for random-access vs bidirectional vs input (Experiment 4).
- **ADL** (Chapter 15) extends the set with functions from the argument's namespace: this is how `std::swap`-style extension points work.

### 5.4 Tag dispatch vs `if constexpr` vs concepts

All three choose an implementation from a *property of the type*:

| Technique | Writing cost | Reads like | Can add a new case without editing the dispatcher? | Error when no case applies |
|---|---|---|---|---|
| **Tag dispatch** (`f(x, category_tag{})` + overloads) | high: one overload per tag + a forwarding function | a table of tiny functions | yes (new tag + overload) | no matching function (long) |
| **`if constexpr`** | low | one function with branches | no (edit the function) | falls through to your `else`/`static_assert` |
| **Constrained overloads (concepts)** | medium | independent functions with named requirements | yes | "constraints not satisfied" with the failing concept |

Use `if constexpr` for **a few branches inside one function**; use constrained overloads for **an extensible family**; tag dispatch survives in pre-C++20 code and in places where the tag is a *real* parameter (e.g. allocator propagation).

### 5.5 Templates as duck typing; concepts as the contract

A plain `template <class T>` imposes only what the body uses ("implicit interface"). A concept (`template <HasArea T>`) states it, checks it at the call site, and participates in overloading. **Syntactic** conformance is checked; **semantic** conformance (does `area()` return something sensible?) is not and cannot be (Chapter 10).

### 5.6 Static polymorphism's limits (what it cannot do)

- No **heterogeneous container**: `std::vector<T>` holds one `T`. Use `std::variant` for a closed set or type erasure (Chapter 21) for an open one.
- No **run-time selection** without a mapping from run-time value to compile-time type (a `switch` generating the instantiations, as in Chapter 17's `visit_index`).
- No **stable binary interface**: templates live in headers and are instantiated in the client.
- **Code size scales with the number of instantiations**; compile time with template depth (Chapter 17).

### Layer check

| Layer | Decides |
|---|---|
| **Standard** | Overload resolution, template instantiation and constraint rules, deduction of `this`, EBO is permitted not required |
| **Compiler** | Whether to inline each instantiation; folding identical instantiations (GCC `-fipa-icf`, Clang `MergeFunctions`, linker ICF with `--icf=all`); code bloat control |
| **ABI** | Each instantiation is a distinct function with its own mangled name: `Base<Impl>::interface()` appears as `_ZN4BaseI4ImplE9interfaceEv`; no vptr, so layout is just the data; templates in headers means the instantiation is **part of the client's binary**, not the library's |
| **CPU** | Direct calls and inlining keep the code in straight-line form: better I-cache locality *per type*, but N copies can overflow the I-cache; no indirect-branch prediction needed |

---

## 6. Implementation model

### What the compiler generates

For `Base<Impl>::interface()` calling `static_cast<Impl*>(this)->implementation()`:

```text
   no vptr, no table, no indirect call:        Base<Impl>::interface   =   call Impl::implementation   (or, at -O1+, the body inlined)
```

An *inlined* CRTP call is the same machine code as writing the derived function's body at the call site. Experiment 2 shows `interface()` vanishing at `-O2`.

### Code duplication and what the toolchain does about it

Each instantiation is a separate function. Many instantiations are *identical* after optimization (e.g. `std::vector<int*>` vs `std::vector<char*>`: same machine code, different types). The toolchain mitigates:

- **Identical Code Folding (ICF)**: the linker (gold/lld `--icf=all`/`safe`) or the compiler (`-fipa-icf`) merges functions with identical machine code. `--icf=all` can break programs that compare function addresses.
- **LTO and inlining** shrink or remove most tiny instantiations.
- **Explicit instantiation** (`extern template`) moves instantiation to one TU to save *compile* time (not code size).

Experiment 5 measures the code-size side.

---

## 7. Experiments

### Experiment 1: CRTP is free, and so is a mixin

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <string>

// A CRTP mixin that derives all comparison operators from operator< (the pre-C++20 idiom).
template <class D> struct Ordered {
    friend bool operator>(const D& a, const D& b)  { return b < a; }
    friend bool operator<=(const D& a, const D& b) { return !(b < a); }
    friend bool operator>=(const D& a, const D& b) { return !(a < b); }
protected:
    Ordered() = default;                 // only D can construct the base
};

// A counter mixin: data in the base, interface through the derived type
template <class D> struct InstanceCounter {
    static inline int live = 0;
protected:
    InstanceCounter()                       { ++live; }
    InstanceCounter(const InstanceCounter&) { ++live; }
    ~InstanceCounter()                      { --live; }
};

struct Version : Ordered<Version>, InstanceCounter<Version> {
    int major, minor;
    Version(int a, int b) : major(a), minor(b) {}
    friend bool operator<(const Version& a, const Version& b) {
        return a.major != b.major ? a.major < b.major : a.minor < b.minor;
    }
};
struct Widget : InstanceCounter<Widget> {};          // a separate counter: Widget::live != Version::live

// An empty CRTP base adds nothing to the size (empty-base optimization):
struct Plain { int major, minor; };
struct OnlyOrdered : Ordered<OnlyOrdered> { int major, minor; bool operator<(const OnlyOrdered&) const { return false; } };

int main() {
    Version a{1, 2}, b{1, 10};
    std::printf("a<b=%d a>b=%d a<=b=%d a>=b=%d\n", a < b, a > b, a <= b, a >= b);

    {
        Version c{2, 0}; Widget w1, w2;
        std::printf("live: Version=%d Widget=%d (inside scope)\n", InstanceCounter<Version>::live, InstanceCounter<Widget>::live);
    }
    std::printf("live: Version=%d Widget=%d (after scope)\n", InstanceCounter<Version>::live, InstanceCounter<Widget>::live);

    std::printf("sizeof(Plain)=%zu  sizeof(OnlyOrdered)=%zu  (empty CRTP base costs 0)\n", sizeof(Plain), sizeof(OnlyOrdered));
    std::printf("sizeof(Version)=%zu (two ints; the counter base is empty, the static is not per object)\n", sizeof(Version));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
a<b=1 a>b=0 a<=b=1 a>=b=0
live: Version=3 Widget=2 (inside scope)
live: Version=2 Widget=0 (after scope)
sizeof(Plain)=8  sizeof(OnlyOrdered)=8  (empty CRTP base costs 0)
sizeof(Version)=8 (two ints; the counter base is empty, the static is not per object)
```

Two things to notice: every `Ordered<Version>` and `Ordered<OnlyOrdered>` is a **different class** (they cannot be mixed in a container), and `InstanceCounter<Version>::live` and `InstanceCounter<Widget>::live` are **different static variables**, the main reason CRTP is used for *per-class* statics.

### Experiment 2: Static dispatch compiles to a direct call (or to nothing)

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fcf-protection=none filter=run_virtual,run_crtp,run_template
struct IShape { virtual int area() const = 0; };
struct Sq final : IShape { int s; int area() const override { return s * s; } };

template <class D> struct ShapeBase {
    int area_twice() const { return 2 * static_cast<const D*>(this)->area(); }
};
struct SqC : ShapeBase<SqC> { int s; int area() const { return s * s; } };

int run_virtual(const IShape& p)        { return 2 * p.area(); }          // virtual: dynamic target
int run_crtp(const SqC& p)              { return p.area_twice(); }         // CRTP: inlined arithmetic
template <class T> int twice(const T& p){ return 2 * p.area(); }
int run_template(const SqC& p)          { return twice(p); }               // plain template: the same
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
run_virtual(IShape const&):
	mov	rax, QWORD PTR [rdi]
	lea	rdx, Sq::area() const[rip]
	mov	rax, QWORD PTR [rax]
	cmp	rax, rdx
	jne	.L4
	mov	eax, DWORD PTR 8[rdi]
	imul	eax, eax
	add	eax, eax
	ret
.L4:
	sub	rsp, 8
	call	rax
	add	rsp, 8
	add	eax, eax
	ret

run_crtp(SqC const&):
	mov	eax, DWORD PTR [rdi]
	imul	eax, eax
	add	eax, eax
	ret

run_template(SqC const&):
	mov	eax, DWORD PTR [rdi]
	imul	eax, eax
	add	eax, eax
	ret
```

`run_crtp` and `run_template` are identical: load `s`, multiply, double, return, with no call at all.

`run_virtual` is the surprise. Instead of the plain `mov / jmp [rax]` of Chapter 19, GCC 14 emitted a **guarded direct call**: it loads the vptr, loads slot 0, compares it with the address of `Sq::area()`, and on a match runs the *inlined* body (`imul` / `add`); only on a mismatch (`.L4`) does it make the real indirect call. This is GCC's **speculative devirtualization** (`-fdevirtualize-speculatively`, on at `-O2`): the only final class in this translation unit that overrides `area` is `Sq`, so it bets on that one. A *compiler optimization* (🔧), not something the language guarantees, and it disappears when more classes override the function in the TU or when the guess fails. The virtual version therefore still costs a load, a compare and a predictable branch here, but not an indirect call.

A template with no common base achieves what CRTP does; **CRTP's only extra power is that the base class (not just free code) gets to call the derived class** to inject members, operators and typedefs.

### Experiment 3: Deducing `this` replaces most CRTP

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <string>
#include <utility>

// (1) The CRTP version needs a template base and a cast.
template <class D> struct PrintableCRTP {
    void print() const { std::printf("[CRTP] %s\n", static_cast<const D*>(this)->to_string().c_str()); }
};
struct A : PrintableCRTP<A> { std::string to_string() const { return "A"; } };

// (2) The deducing-this version: a plain non-template base class; the member is a template over the object type.
struct Printable {
    void print(this const auto& self) { std::printf("[this] %s\n", self.to_string().c_str()); }
};
struct B : Printable { std::string to_string() const { return "B"; } };

// (3) One member function replaces const / non-const / & / && overloads and keeps the value category.
struct Buffer {
    std::string data = "payload";
    auto&& get(this auto&& self) { return std::forward_like<decltype(self)>(self.data); }   // string&, const string&, or string&&
};

// (4) Recursive lambda without std::function or the Y-combinator.
int main() {
    A{}.print();
    B{}.print();

    Buffer b;               b.get() += "!";                     // non-const lvalue -> string&, modifiable
    const Buffer cb{};      // const lvalue -> const string&
    std::printf("get() on lvalue: %s;  on const: %s;  on rvalue: %s\n", b.get().c_str(), cb.get().c_str(), Buffer{}.get().c_str());
    static_assert(std::is_same_v<decltype(b.get()), std::string&>);
    static_assert(std::is_same_v<decltype(cb.get()), const std::string&>);
    static_assert(std::is_same_v<decltype(Buffer{}.get()), std::string&&>);

    auto fact = [](this auto&& self, int n) -> int { return n <= 1 ? 1 : n * self(n - 1); };
    std::printf("5! via recursive lambda = %d\n", fact(5));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
[CRTP] A
[this] B
get() on lvalue: payload!;  on const: payload;  on rvalue: payload
5! via recursive lambda = 120
```

The *same* behaviour with no template parameter on the base class: that means a mixin can sit in a normal `.cpp`-friendly class, derived classes list it as `: Printable` (no repeat of their own name, which removes the classic CRTP error "wrong class in the template argument"), and the value category is deduced for free. **Opinion: in new C++23 code, prefer deducing `this` over CRTP.** Keep CRTP for pre-23 code, for mixins that must inject *types or friend functions or static data* (deducing `this` only helps with member functions), and for interop with code that spells `Base<Derived>`.

### Experiment 4: Constrained overloads: dispatch by capability

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <forward_list>
#include <iterator>
#include <list>
#include <vector>

// Most specialized wins: random_access > bidirectional > forward/input. No tags, no SFINAE, no if-constexpr chain.
template <std::input_iterator It>
void my_advance(It& it, std::iter_difference_t<It> n) { std::puts("  [input]         linear forward walk"); while (n-- > 0) ++it; }

template <std::bidirectional_iterator It>
void my_advance(It& it, std::iter_difference_t<It> n) {
    std::puts("  [bidirectional] linear walk, either direction");
    if (n >= 0) while (n-- > 0) ++it; else while (n++ < 0) --it;
}

template <std::random_access_iterator It>
void my_advance(It& it, std::iter_difference_t<It> n) { std::puts("  [random access] O(1) jump"); it += n; }

// The same family using if constexpr: ONE function, branches by capability.
template <class It>
void advance_ifc(It& it, std::iter_difference_t<It> n) {
    if constexpr (std::random_access_iterator<It>)       { std::puts("  [ifc random access]"); it += n; }
    else if constexpr (std::bidirectional_iterator<It>) { std::puts("  [ifc bidirectional]"); if (n >= 0) while (n-- > 0) ++it; else while (n++ < 0) --it; }
    else                                                 { std::puts("  [ifc input]"); while (n-- > 0) ++it; }
}

int main() {
    std::vector<int> v{0, 1, 2, 3, 4, 5};
    std::list<int> l{0, 1, 2, 3, 4, 5};
    std::forward_list<int> f{0, 1, 2, 3, 4, 5};

    std::puts("overloads constrained by concepts:");
    auto iv = v.begin(); my_advance(iv, 3); std::printf("    -> %d\n", *iv);
    auto il = l.begin(); my_advance(il, 3); std::printf("    -> %d\n", *il);
    auto jf = f.begin(); my_advance(jf, 3); std::printf("    -> %d\n", *jf);

    std::puts("one function with if constexpr:");
    auto kv = v.begin(); advance_ifc(kv, 4); std::printf("    -> %d\n", *kv);
    auto kl = l.begin(); advance_ifc(kl, 4); std::printf("    -> %d\n", *kl);
    auto kf = f.begin(); advance_ifc(kf, 4); std::printf("    -> %d\n", *kf);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
overloads constrained by concepts:
  [random access] O(1) jump
    -> 3
  [bidirectional] linear walk, either direction
    -> 3
  [input]         linear forward walk
    -> 3
one function with if constexpr:
  [ifc random access]
    -> 4
  [ifc bidirectional]
    -> 4
  [ifc input]
    -> 4
```

The overload set works because the concepts **subsume** each other (`random_access_iterator` ⊃ `bidirectional_iterator` ⊃ `forward_iterator` ⊃ `input_iterator`, Chapter 10): the compiler picks the *most constrained* viable candidate with no ambiguity and no manual priority tags (the old way was `iterator_category` tag classes with inheritance, which gave the same effect).

### Experiment 5: What does it cost? Speed, code size, compile time

The same workload (sum of areas of `N` shapes) written five ways. Speed is measured in-process; code size and instantiation counts come from `nm`/`size`.

```cpp
// @test run -std=c++23 -O2
#include <chrono>
#include <concepts>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <variant>
#include <vector>

struct Sq   { int s; int area() const { return s * s; } };
struct Rect { int w, h; int area() const { return w * h; } };

// 1. virtual
struct IShape { virtual ~IShape() = default; virtual int area() const = 0; };
struct VSq   final : IShape { int s; explicit VSq(int s) : s(s) {} int area() const override { return s * s; } };
struct VRect final : IShape { int w, h; VRect(int w, int h) : w(w), h(h) {} int area() const override { return w * h; } };

// 2. template + concept over a homogeneous vector (the best case for static dispatch)
template <class T> concept HasArea = requires(const T& t) { { t.area() } -> std::convertible_to<int>; };
template <HasArea T> [[gnu::noinline]] long sum_template(const std::vector<T>& v) { long s = 0; for (auto& x : v) s += x.area(); return s; }

// 3. variant (closed set, by value)
using Var = std::variant<Sq, Rect>;
[[gnu::noinline]] long sum_variant(const std::vector<Var>& v) { long s = 0; for (auto& x : v) s += std::visit([](const auto& a) { return a.area(); }, x); return s; }

// 4. virtual, pointers
[[gnu::noinline]] long sum_virtual(const std::vector<std::unique_ptr<IShape>>& v) { long s = 0; for (auto& p : v) s += p->area(); return s; }

// 5. type erasure with std::function (by value)
[[gnu::noinline]] long sum_function(const std::vector<std::function<int()>>& v) { long s = 0; for (auto& f : v) s += f(); return s; }

template <class F> double best_ms(F&& f, long& sink, int reps = 7) {
    double best = 1e30;
    for (int i = 0; i < reps; ++i) {
        auto t0 = std::chrono::steady_clock::now(); sink += f();
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    return best;
}

int main() {
    constexpr int N = 1'000'000;
    std::mt19937 rng(3);
    std::vector<Sq> hom;                                   // homogeneous: only Sq
    std::vector<Var> var;
    std::vector<std::unique_ptr<IShape>> virt;
    std::vector<std::function<int()>> fn;
    for (int i = 0; i < N; ++i) {
        bool sq = rng() & 1;
        int a = int(rng() % 9) + 1, b = int(rng() % 9) + 1;
        hom.push_back(Sq{a});
        if (sq) { var.emplace_back(Sq{a});  virt.push_back(std::make_unique<VSq>(a));     fn.emplace_back([a] { return a * a; }); }
        else    { var.emplace_back(Rect{a, b}); virt.push_back(std::make_unique<VRect>(a, b)); fn.emplace_back([a, b] { return a * b; }); }
    }
    long sink = 0;
    std::printf("sum of areas, N=%d (best of 7, -O2)\n", N);
    std::printf("  template over vector<Sq> (1 type)     %7.2f ms\n", best_ms([&] { return sum_template(hom); }, sink));
    std::printf("  variant<Sq,Rect> (2 types, by value)  %7.2f ms\n", best_ms([&] { return sum_variant(var); }, sink));
    std::printf("  virtual (2 types, unique_ptr)         %7.2f ms\n", best_ms([&] { return sum_virtual(virt); }, sink));
    std::printf("  std::function<int()> (type erasure)   %7.2f ms\n", best_ms([&] { return sum_function(fn); }, sink));
    return sink == 7 ? 1 : 0;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sum of areas, N=1000000 (best of 7, -O2)
  template over vector<Sq> (1 type)        0.62 ms
  variant<Sq,Rect> (2 types, by value)     5.01 ms
  virtual (2 types, unique_ptr)            8.36 ms
  std::function<int()> (type erasure)      8.07 ms
```

*This compares different problems*: the template version solves "all one type", the others solve "two types mixed". The numbers above (one machine, GCC 14.2): the **homogeneous template loop takes 0.62 ms**, about 8 to 13 times faster than the mixed-type versions (variant 5.0 ms; virtual 8.4 ms; `std::function` 8.1 ms), and the reason is not "templates are fast" but that a vector of one small type is contiguous, has no branch to mispredict, and can be vectorized. Once types are mixed, `variant` is the static answer: it is faster than virtual here because the objects are contiguous values rather than separately allocated heap objects, and it still pays the same branch-misprediction cost (Chapter 19, Experiment 6). Note what these runs do **not** capture: I-cache effects of many instantiations and per-type code size.

Now code size and instantiation count, from the object file:

```bash
# generate K distinct types with one template instantiation each, compile, count and size
for K in 1 8 64; do
  { echo 'template <int N> struct Op { static int run(int x) { return x * N + (x >> N % 7); } };'
    echo 'template <class T> int apply(int x) { int r = x; for (int i = 0; i < 4; ++i) r = T::run(r); return r; }'
    for ((i=0;i<K;++i)); do echo "int f$i(int x) { return apply<Op<$i>>(x); }"; done; } > bloat.cpp
  g++-14 -std=c++23 -O1 -c bloat.cpp -o bloat.o
  printf "K=%-3s text bytes: %s   weak template symbols: %s\n" $K "$(size bloat.o | awk 'NR==2{print $1}')" "$(nm -C bloat.o | grep -c ' W ')"
done
```

```text
# output (GCC 14.2, run by hand; not auto-verified)
-O1:  K=1   text bytes: 87     weak template symbols: 0
-O1:  K=8   text bytes: 389    weak template symbols: 0
-O1:  K=64  text bytes: 2839   weak template symbols: 0
-O0:  K=64  text bytes: 13470  weak template symbols: 128   (2 per type: apply<Op<i>> and Op<i>::run)
```

The shape: code size grows **linearly with the number of distinct instantiations**, about 44 bytes per type here at `-O1` (each `f_i` is a separately optimized copy of the loop), whereas at `-O0` every instantiation also survives as a weak symbol (128 for 64 types) and the text is about five times larger. At `-O1` the `apply`/`run` instantiations were all inlined and *left no symbols at all*, so counting weak symbols only measures bloat in unoptimized builds; **measure `size` on the build you ship**. An identical-code-folding linker can recover part of the growth when the copies are byte-identical (they are not here: each `N` is baked in as a constant).

---

## 8. Assembly / runtime investigation

Experiment 2 is the assembly investigation. Further checks:

```bash
# (1) How many instantiations of my template did the compiler create?  (weak symbols; each is a separate function)
g++-14 -std=c++23 -O0 -c prog.cpp -o prog.o && nm -C prog.o | grep -c ' W .*MyTemplate'

# (2) What does the linker fold?  Compare size with and without ICF
g++-14 -O2 -fuse-ld=gold -Wl,--icf=safe prog.o -o a.out && size a.out

# (3) Is a CRTP base really empty?  Check the layout (GCC prints it; look for "empty base" and size)
g++-14 -std=c++23 -fdump-lang-class -c prog.cpp -o /dev/null && grep -A12 "^Class Version" prog.cpp.*class

# (4) Template instantiation cost on the compiler
g++-14 -std=c++23 -ftime-report -c prog.cpp -o /dev/null 2>&1 | grep -iE "template instantiation|TOTAL"
```

---

## 9. Implementation exercise

1. **Policy-based `Logger<Sink, Format>`**: sinks (`StdoutSink`, `FileSink`, `NullSink`) and formatters are template parameters; verify that `NullSink` logging compiles to *nothing* at `-O2`. Compare with a virtual `ISink` version in code size and in the call-site assembly.
2. **A CRTP `Iterable<D>` mixin**: given `D::begin()/end()`, provide `size()`, `empty()`, `front()`, `back()`, `operator[]` for random access. Then rewrite it using **C++23 deducing `this`** and compare lines, error messages for a wrong `D`, and compile time. (C++20's `std::ranges::view_interface` is the standard's CRTP version of this exact mixin: read its implementation.)
3. **Capability-dispatched algorithms**: implement `my_distance`, `my_copy` (memmove when trivially copyable and contiguous; element loop otherwise), `my_reverse` by constrained overloads. Prove with the assembly that the contiguous case becomes `memmove`.
4. **A static interface check**: define a concept `Shape` (area, perimeter, `name() -> string_view`) and a `static_assert(Shape<Circle>)` for each implementation; make the diagnostics *good*: when a method is missing, the error should name which one. Compare with CRTP + `static_assert` inside the base.
5. **Hybrid**: expose a static template API (`draw(const Shape auto&)`) and a dynamic one (`draw(const AnyShape&)`) over the same classes. This is Chapter 21's type erasure; sketch the bridge now.
6. **Measure bloat**: take a real template-heavy header of yours; count instantiations (`nm`), compute size per instantiation, and find five candidates for *type-erasing the cold path* to shrink code.

<details>
<summary><strong>Solution sketch for item 2 (deducing-this mixin)</strong></summary>

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <iterator>
#include <vector>

// A container-interface mixin: derives size/empty/front/back/operator[] from begin()/end().
struct ContainerInterface {
    bool empty(this const auto& self) { return self.begin() == self.end(); }
    auto size(this const auto& self) { return std::distance(self.begin(), self.end()); }
    decltype(auto) front(this auto&& self) { return *self.begin(); }
    decltype(auto) back(this auto&& self) { return *std::prev(self.end()); }
    decltype(auto) operator[](this auto&& self, std::ptrdiff_t i) { return *(self.begin() + i); }
};

class Ring : public ContainerInterface {
    std::vector<int> data_;
public:
    explicit Ring(std::vector<int> d) : data_(std::move(d)) {}
    auto begin() const { return data_.begin(); }
    auto end() const { return data_.end(); }
    auto begin() { return data_.begin(); }
    auto end() { return data_.end(); }
};

int main() {
    Ring r({10, 20, 30});
    std::printf("size=%td empty=%d front=%d back=%d r[1]=%d\n", r.size(), r.empty(), r.front(), r.back(), r[1]);
    r[1] = 99;                                              // non-const overload deduced automatically
    std::printf("after r[1]=99: %d\n", r[1]);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
size=3 empty=0 front=10 back=30 r[1]=20
after r[1]=99: 99
```

One definition covers const and non-const `Ring`, with no template parameter on the base and no `static_cast`. The standard's `std::ranges::view_interface<D>` does the same job with CRTP because it predates C++23; a C++23-only library could drop the template parameter.

</details>

---

## 10. Real-world example

| Where | Static polymorphism in use |
|---|---|
| **STL algorithms and containers** | Templates over iterator/range concepts; allocators and comparators as policy parameters |
| **`std::ranges::view_interface`, `std::enable_shared_from_this`, `std::ranges::range_adaptor_closure`** | Standard CRTP |
| **Eigen, Blaze, xtensor** | CRTP expression templates: `MatrixBase<Derived>` lets `a + b * c` stay a lazy tree with a statically known type |
| **Boost.Iterator / Boost.Operators** | CRTP `iterator_facade`, `totally_ordered<T>` generate boilerplate |
| **LLVM** | CRTP visitors (`InstVisitor<Derived>`, `RecursiveASTVisitor<Derived>`): the AST walk is statically dispatched with user overrides picked up by name |
| **Linux kernel / embedded C++** | Policy templates replacing function-pointer tables |
| **Qt** | `QObject` uses *virtual* functions and the meta-object system (dynamic); but `QList`, `QMap`, `QtConcurrent`, and `QScopedPointer<T, Cleanup>` use static policies (Chapter 48) |
| **fmt / `std::format`** | `formatter<T>` specialization: static dispatch on type, with a type-erased `basic_format_arg` for the runtime part |

> **Opinion.** Default to **templates constrained by concepts** for algorithms and containers. For *mixins*, in C++23 use deducing `this`; in C++20 and earlier use CRTP with a protected constructor. Do not use CRTP "to avoid virtual calls" in cold code: you pay N instantiations and worse error messages for no measurable gain. Do use it in hot inner loops over homogeneous data, where inlining across the call matters. And never build a **public library API** whose template parameters leak into every client's binary unless you accept the ABI and compile-time consequences (Chapters 37, 44).

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| `struct Wrong : Base<Right>` (CRTP typo) | `static_cast<Right*>(this)` on a `Wrong`: **UB** | Private base constructor + `friend D`; or deducing `this` |
| Using `Derived::type` in `Base<Derived>`'s member declarations | "incomplete type" | Move to function bodies, or a trait computed later (`decltype(auto)`) |
| Trying to store `Base<A>*` and `Base<B>*` together | Types differ; no common base | `std::variant`, type erasure or a real virtual base |
| Template error messages 200 lines long | Unreadable | Concept on the parameter; `static_assert` with a message |
| Code bloat: hundreds of instantiations in a header | Large binaries, slow builds | Type-erase the cold path, `extern template`, ICF, shrink the generic part (non-template core) |
| Header-only template API at a shared-library boundary | ODR / ABI breakage when versions mix | Stable non-template interface at the boundary |
| `if constexpr` branch with invalid code in a non-dependent condition | Discarded branch still must be well-formed | Make the condition dependent on a template parameter |
| Ambiguous overloads from two constraints that don't subsume | "ambiguous call" | Constrain via a *named concept* that includes the other (Chapter 10's same-text trap) |
| Tag dispatch with inheritance of tags forgotten | Wrong overload or ambiguity | Use concepts; or tag classes inheriting in order |
| Deducing `this` and then slicing | `Self` deduced as the base when called via a base reference | Call through the derived type; document it |
| Over-generic `template <class T> void f(T)` hijacking overloads | Greedy template wins over a better non-template conversion | Constrain it |
| Using static polymorphism where the type is chosen at run time | A giant `switch` that instantiates every case | Virtual or type erasure |

---

## 12. Exercises

1. **Which tool?** For each scenario choose *virtual / template / CRTP / deducing this / variant / type erasure* and justify in one sentence: (a) a GUI widget tree loaded from a file; (b) a numeric kernel over `float`/`double`; (c) comparison operators for 40 value types; (d) a plug-in loaded with `dlopen`; (e) an event queue holding callbacks from many modules; (f) a parser AST with eight node kinds.
2. **CRTP vs concept.** Reimplement Boost.Operators' `totally_ordered<T>` once with CRTP and once with C++20 `operator<=>` (defaulted). Compare line count, binary size and error message for a missing `<`.
3. **Measure CRTP's "zero cost".** Write `Derived::impl` in a separate TU (no LTO) and call it through the CRTP base. Is it still free? Add `-flto`. Explain.
4. **Concept subsumption lab.** Write three overloads on `Shape`, `Shape && Drawable`, and `Shape && Drawable && Serializable`. Call with types satisfying each subset. Then reproduce an ambiguity and fix it.
5. **Bloat budget.** For a template container you wrote (Project 2 in this course), count instantiations in a program using 10 element types, compute size per type, then apply `--icf=safe` and re-measure.
6. **Deducing `this` migration.** Take a CRTP mixin from an open-source project and port it. List what became simpler and what you could *not* port (injected typedefs? friends? static members?).
7. **Hybrid design.** Design a `Drawable` facility with: a static concept for the hot path, a virtual interface for plug-ins, and a type-erased value wrapper for the scene graph. Draw which layer calls which.

---

## 13. Challenge: expression templates

Implement a tiny expression-template vector library:

- `Vec<N>` of `float`, with `+`, `-`, `*` (element-wise) and scalar broadcast, using a CRTP `Expr<E>` base so that `a + b * c` has a type encoding the tree
- evaluation in `operator=` with a **single fused loop**; verify at `-O2` that `d = a + b * c` is one loop with no temporaries and compare with the naive operator-returning-`Vec` version
- a measurement table: time and **number of memory allocations** (count `operator new`) for 1, 3, 6 chained operations at N = 10⁴ and 10⁷
- demonstrate the classic **dangling pitfall** (`auto e = a + b;` stores references to temporaries) and defend against it (delete rvalue overloads, or `static_assert` on `auto` capture via a wrapper)
- now rewrite using **C++23 deducing `this`** and **C++20 ranges** (`views::zip_transform`) and compare code size, compile time, and generated loops

---

## 14. Knowledge check

1. When is a call "statically polymorphic"? Where is the dispatch decision made?
2. What does CRTP give a base class that a plain template function does not?
3. Why can `Derived::value_type` not be used in a CRTP base's member *declarations*, but can be used in member function *bodies*?
4. What does deducing `this` change, and which CRTP uses can it *not* replace?
5. Why can't you store `Base<A>` and `Base<B>` objects in one `std::vector`?
6. Compare tag dispatch, `if constexpr`, and constrained overloads in terms of extensibility.
7. What makes concepts-based overload sets resolve without ambiguity? What breaks it?
8. Name two costs of static polymorphism that dynamic polymorphism does not have.
9. Why are templates a poor fit for a stable binary interface?
10. How does the linker or compiler reduce the code bloat of many identical instantiations?
11. In Experiment 5, why is the template/`vector<Sq>` case not comparable with the virtual case?
12. When would you pick `std::variant` over both virtual functions and templates?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. When overload resolution/template instantiation selects the target during compilation; the decision is made by the compiler for each call site (each instantiation is its own code).
2. It lets the base class *call into the derived class* (and inject members, operators, typedefs and statics that depend on it) without virtual functions. A free template function can only use what is passed to it.
3. `Derived` is incomplete while `Base<Derived>` is being instantiated as a base; declarations in the class body are instantiated immediately, function bodies lazily after `Derived` is complete.
4. It lets a non-template base member function deduce the object's type (and cv/ref category) as an explicit parameter. It cannot inject *types, static data or friend functions* that depend on the derived class, nor help pre-C++23 code.
5. They are different, unrelated classes. A vector holds one type; CRTP offers code reuse, not substitutability.
6. Tag dispatch: extensible by adding a tag + overload but verbose; `if constexpr`: concise but closed (edit the function); constrained overloads: extensible and readable, with subsumption deciding priority.
7. The concepts' *subsumption* ordering (a more constrained overload is more specialized). It breaks when constraints are written with equal-looking but distinct expressions instead of named concepts that include one another (the same-text trap), or when two unrelated constraints both apply.
8. Code size grows per instantiation; longer compile times; no heterogeneous containers; no run-time selection; implementation in headers; ABI exposure.
9. Their implementation is instantiated in the client; any change to the template changes every client's compiled code; layout and symbol names depend on the template arguments.
10. Inlining/LTO remove tiny instantiations; Identical Code Folding (`-fipa-icf`, linker `--icf`) merges identical machine code; `extern template` and explicit instantiation reduce compile time (not code size).
11. The template version handles only one type; the virtual version handles two mixed types with a per-element pointer. It solves an easier problem, so the time difference mostly shows the benefit of homogeneity and contiguous storage.
12. When the set of types is closed and known, you want value semantics (no heap), and operations over them are best written as visitors; you gain exhaustiveness checking and no vptr/heap.

</details>

---

[← Previous: Chapter 19](19-runtime-polymorphism.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 21 — Type erasure →](21-type-erasure.md)
