# Chapter 37 — ABI: The Binary Contract

> **Part XIV · Compilation and Linking** &nbsp;|&nbsp; **Level 4** (compiler/runtime) &nbsp;|&nbsp; **≈ 9 hours**
> **Prerequisites:** [Chapter 19 (virtual functions)](../part-08-polymorphism/), [Chapter 36 (compilation model)](36-compilation-model.md) &nbsp;|&nbsp; **Standards:** the C++ standard defines **no ABI** (🧩 everything here is the Itanium C++ ABI, the System V x86-64 psABI and libstdc++'s own rules); stable across GCC and Clang on Linux/macOS, **different** on MSVC &nbsp;|&nbsp; **Tools:** `g++-14`, `clang++-18`, `nm`, `objdump`, `c++filt`

[← Previous: Chapter 36](36-compilation-model.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 38 — Modules →](38-modules.md)

---

**In one sentence:** an **API** is what the *source* of two pieces of code agree on (names, types, signatures); an **ABI** is what their *machine code* agrees on (symbol names, struct layouts, vtable order, which registers carry arguments), and a change that is harmless to the first can silently destroy the second.

**By the end of this chapter you can:**

- name every layer of the contract: symbol mangling, type layout, calling convention, vtables, exceptions, the standard-library ABI
- predict which source changes to a class break binary compatibility, and **reproduce the breakage** with real shared libraries
- read the calling convention off a function's assembly (what goes in registers, what is passed by invisible reference)
- explain the `std::string` dual ABI, libstdc++ symbol versioning, and why the standard library cannot change `std::unordered_map`
- design a library boundary that survives upgrades: pImpl, opaque handles, a C API, and `inline namespace` versioning

> **A frank warning.** If you only ever build your whole program from source with one toolchain, ABI mostly doesn't matter to you. The moment you ship a `.so`/`.dll`, load a plug-in, use a pre-built dependency, or upgrade a compiler under a running system, it is the *only* thing that matters.

---

## 1. Problem

Imagine you ship `libconfig.so` and an application that uses it. Next year you add one field to a struct in a header and ship a bug fix: **only the `.so`**, not the application. The application was compiled against the old header. Does it still work?

The *source* is fine: every line that compiled before still compiles. But the application's machine code has `offsetof(Config, height) == 4` baked in, while the new library thinks it is 8. No compiler ran when you swapped the library, so nothing was checked. Experiment 1 shows what happens.

Binary compatibility is therefore a **separate contract** from source compatibility, covering:

| Part of the contract | Question it answers |
|---|---|
| **Symbol names** (mangling) | What string does the linker look up for `ns::f(int)`? |
| **Type layout** | Where is each member? How big, how aligned is the object? |
| **Calling convention** | Which registers/stack slots carry arguments and the return value? Who cleans up? |
| **Virtual dispatch** | What does a vtable look like, in what order, and where is the vptr? |
| **Object lifetime protocol** | Who runs constructors/destructors of parameters and temporaries? |
| **Exceptions and RTTI** | How is a thrown object stored and unwound, how are `type_info`s compared? |
| **Standard library types** | What is inside `std::string`, `std::list`, `std::function`? |
| **Runtime support** | Which `libstdc++`, `libgcc`, `libc` symbols must exist, at which version? |

---

## 2. Historical context

| Year | Event |
|---|---|
| 1980s–90s | Each C++ compiler vendor invents its own layout and mangling; a library built by one compiler is useless to another |
| 1999–2001 | **Itanium C++ ABI** is written for the IA-64 architecture by a multi-vendor group; GCC 3.0 adopts it for all platforms and it becomes the *de facto* Unix standard (Clang, ICC, later ARM's AAPCS-adapted variant) |
| 2003 | GCC 3.4 stabilises libstdc++'s ABI; symbol versioning (`GLIBCXX_3.4`) begins |
| 2011 | C++11 requires `std::string` to be non-copy-on-write and `std::list::size()` to be O(1): **the standard forces an ABI break**; libstdc++ answers with the *dual ABI* (`_GLIBCXX_USE_CXX11_ABI`, inline namespace `__cxx11`), released in GCC 5 (2015) |
| 2016–2020 | The committee repeatedly discusses (and declines) breaking ABI for performance (`std::regex`, `std::unordered_map`, `std::map` node sizes); Google's "ABI stability costs performance" papers (P1863, P2028) |
| 2020 | C++20 `std::jthread`, `std::format`, ranges added as new types, without touching old ones |
| 2020s | **"ABI is a promise we keep by not changing things"**: MSVC has kept its STL ABI stable since VS 2015 and cites it as the reason several known inefficiencies persist; libc++ has an opt-in unstable ABI |
| 2024–26 | libstdc++ 14 remains `GLIBCXX_3.4.33`-compatible with 2004-built binaries; `std::print`, `std::expected` etc. are additions only |

---

## 3. Modern solution

You cannot make the compiler guarantee ABI stability for arbitrary C++ types across versions. The "solution" is **discipline at the boundary** plus the compilers' stable platform ABI:

| Technique | What it protects | Cost |
|---|---|---|
| **C API** (`extern "C"`, opaque handles, plain structs) | Everything: C has a stable ABI on every platform | You give up overloads, templates, RAII at the boundary (wrap it back in C++ on both sides, Chapter 45) |
| **pImpl** (opaque pointer) | Data layout of your classes | One heap allocation and an indirection per object; no inline members |
| **Abstract interface + factory** | Virtual dispatch layout, if you only ever *append* virtuals | Dynamic dispatch; can never reorder or remove |
| **`inline namespace v1 { … }`** | Lets v1 and v2 symbols coexist in the same binary | Mangled names change (that is the point) |
| **Symbol versioning** (`.symver`, version scripts) | Evolve one function's implementation while old binaries keep the old one | Build complexity; what libstdc++ and glibc do |
| **Visibility control** (Chapter 36) | Shrinks the surface you must keep stable | none |
| **ABI checkers** (`abidiff`/libabigail, `abi-compliance-checker`) | Detects accidental breaks automatically in CI | A CI job |
| **Static linking / same toolchain** | Avoids the question entirely | No sharing, bigger binaries, can't hot-swap |

---

## 4. Mental model

```text
 API (source)                              ABI (binary)
 ─────────────────                         ──────────────────────────────────────────────────────
 names, types, signatures,                 symbol: _ZN3geo4areaERKNS_5ShapeE
 template definitions, macros              layout: sizeof 24, vptr at 0, `radius` at 16
 "what you write"                          convention: arg0 in rdi (pointer), result in xmm0
                                           vtable: [typeinfo][dtor D1][dtor D0][area][perimeter]
                                           ──► what the OTHER object file assumes without asking
```

### The layers

```text
 C++ language          sizeof(T)? alignof? member order?  → all implementation-defined
      │
 Itanium C++ ABI       layout algorithm, mangling, vtables, RTTI, exceptions, guard variables, ctor/dtor variants
      │
 Platform C ABI        System V x86-64 psABI: calling convention, register classes, red zone, struct passing
      │
 Standard library ABI  libstdc++: size/layout of std::string, versioned symbols, _GLIBCXX_USE_CXX11_ABI
      │
 OS / loader           ELF, GOT/PLT, symbol versioning, SONAME, search paths
      │
 CPU ISA               registers, alignment, endianness
```

A change is **ABI-breaking** if any assumption baked into already-compiled code is no longer true. The assumptions that are baked in:

| Baked into every client object file | So these changes break it |
|---|---|
| `sizeof(T)`, `alignof(T)`, member offsets (inline accessors, copies, stack allocation of `T`) | adding/removing/reordering data members; changing a member's type; adding a virtual function (adds a vptr) to a non-polymorphic class; adding a base class |
| Virtual call = “load vptr, load slot *N*, call” | inserting, removing or reordering virtual functions; changing which base class declares them |
| Inline function bodies (copied into the client) | changing an inline function's behaviour: old clients keep the old behaviour (an ODR-like split) |
| Default arguments (evaluated at the call site) | changing a default argument value |
| Mangled name of every function/type it references | changing a parameter type, constness, namespace, or `inline namespace`; changing `noexcept` (part of the type since C++17) |
| Whether a type is trivially copyable / destructible (decides how it is passed) | adding a user-declared copy constructor or destructor turns a register-passed type into an invisible-reference type |
| Calling convention and enum underlying type | changing an enum's range so its underlying type changes |
| Template instantiations compiled into the client | any change to a template defined in a header (the client has its own copy) |

What is **safe**: adding new non-virtual member functions, adding new free functions, adding virtual functions **at the end** of a class with no derived classes in client code (Itanium: and even that breaks clients that derive from it), changing function bodies in the `.so` (non-inline), adding new classes.

---

## 5. Language rules

| Topic | Rule |
|---|---|
| **C++ standard** | Specifies *nothing* about ABI. `sizeof`, layout of non-standard-layout classes, vtables, mangling, calling conventions are **implementation-defined or unspecified**. Only *standard-layout* types have guaranteed properties: first member at offset 0, members in declaration order within an access level, compatible with a C struct |
| **Standard-layout** | No virtual functions/bases, all non-static members have the same access control, no non-standard-layout members or bases, at most one class in the hierarchy has non-static data members. `std::is_standard_layout_v<T>` |
| **Trivially copyable** | Copy/move are bytewise copies. `std::is_trivially_copyable_v<T>` is exactly the property that lets the Itanium ABI pass the object in registers (together with a trivial destructor) |
| **`noexcept` (C++17)** | Part of a function's *type*, therefore of its mangled name and of function-pointer types |
| **Inline namespaces** | Members of an inline namespace appear to be in the enclosing namespace for lookup, but are part of the mangled name; libstdc++'s `std::__cxx11` is one |
| **Empty base / `[[no_unique_address]]`** (C++20) | Empty members/bases may occupy zero bytes; changes `sizeof` and member offsets, and is implemented differently in MSVC (`[[msvc::no_unique_address]]`): a documented cross-compiler ABI split |
| **`extern "C"`** | C linkage: no mangling, C calling convention; the portable ABI boundary |
| **`_GLIBCXX_USE_CXX11_ABI`** | libstdc++-specific macro; 🔧 not C++. Selects which `std::string`/`std::list` ABI the translation unit uses |
| **Defaults, inlines, templates** | Evaluated/instantiated in the *client*, so they are not covered by the library's ABI at all: the client keeps its own copy |

### Layer check

| Layer | Question | Answer |
|---|---|---|
| **C++ standard** | Is there an ABI guarantee? | **No.** Even `sizeof(int)` is implementation-defined |
| **Compiler** | Who defines layout/mangling/vtables? | The compiler implements the **Itanium C++ ABI** (GCC, Clang on Linux) or the Microsoft ABI (MSVC, clang-cl). Same source, different binary |
| **ABI** | What is the calling convention? | System V AMD64: first six integer/pointer args in `rdi, rsi, rdx, rcx, r8, r9`; floats in `xmm0–7`; return in `rax`/`rdx` or `xmm0`; small trivial structs in registers; non-trivial objects by invisible reference; large returns through a hidden pointer (`sret`) |
| **OS** | What carries versions? | ELF symbol versions (`GLIBCXX_3.4.33`), SONAME (`libstdc++.so.6`), `ld.so` search order |
| **CPU** | Why does passing in registers matter? | Avoids memory round trips: the ABI sets the cost of passing a type (Experiment 3) |

---

## 6. Implementation model

### Layout: the Itanium rules in brief

- Members are laid out **in declaration order**, each at the next offset aligned for its type; the struct's alignment is the max member alignment; size is rounded up to it.
- A **polymorphic** class begins with a pointer (the **vptr**) to a vtable; the first non-virtual base at offset 0 may share it (the “primary base”).
- **Non-virtual multiple inheritance** places bases one after another; converting a `Derived*` to its second base **adds a constant to the pointer** (Experiment 2), so a `static_cast` is not always a no-op.
- **Virtual inheritance** puts the shared base at a location found through the vtable (an offset stored there), because different most-derived classes place it differently.
- Empty bases use the **empty base optimisation**; potentially-overlapping members use tail padding.

### Vtable

```text
 object:  [ vptr ]──────────►  vtable for Shape
          [ members ... ]       [ offset-to-top   ]   -16   (for multiple inheritance adjustment)
                                [ typeinfo ptr    ]    -8   (RTTI: typeid, dynamic_cast)
                       vptr ──► [ ~Shape() complete ]     0    <- first virtual (D1: complete-object destructor)
                                [ ~Shape() deleting ]     8    (D0: destroy + operator delete)
                                [ sides()           ]    16
                                [ corners()         ]    24
```

A virtual call is `mov rax,[obj]; call [rax + 8*slot]` where `slot` is compiled into the client. **Inserting a virtual function before others shifts every later slot**: the client calls the wrong function (Experiment 1b). The Itanium ABI also defines **constructor/destructor variants** (`C1/C2`, `D0/D1/D2`) because base-subobject and complete-object construction differ; you see them as `_ZN5ShapeC1Ev` and `_ZN5ShapeC2Ev` in `nm`.

### Calling convention (System V x86-64)

| Argument/return type | Passed/returned |
|---|---|
| `int`, `long`, pointer, reference, enum | integer register `rdi, rsi, rdx, rcx, r8, r9`, then stack |
| `float`, `double` | `xmm0–xmm7` |
| Trivially-copyable struct ≤ 16 bytes | **in registers**, classified per 8-byte chunk (INTEGER or SSE) |
| Struct > 16 bytes, or with a **non-trivial copy constructor or destructor** | **in memory**; caller makes a temporary and passes its *address* (the “invisible reference”) |
| Return value, trivial ≤ 16 bytes | `rax`/`rdx` (or `xmm0`/`xmm1`) |
| Return value, non-trivial or large | caller allocates, passes hidden pointer in `rdi` (`sret`), callee returns it in `rax` |

The practical consequence is famous: **`std::unique_ptr<T>` is passed in memory, a raw `T*` in a register**, because `unique_ptr` has a non-trivial destructor (the ABI makes the *caller* destroy it after the call). That is a measurable cost the standard cannot remove without an ABI break (Experiment 3).

### The standard-library ABI

libstdc++ promises that code built against any GCC since 3.4 runs on the newest `libstdc++.so.6`, by **versioning symbols** (`GLIBCXX_3.4.x`) and never changing the layout of existing types. The cost: the standard library is **frozen**. `std::unordered_map`'s node-based design, `std::regex`'s speed, `std::function`'s size are all locked in by binary compatibility, not by the language.

---

## 7. Experiments

Shell experiments use sources in [`code/ch37/`](code/ch37/) (`bash run.sh` reproduces every one) and are pasted from real runs (GCC 14.2, libstdc++ 6.0.33); they are **not auto-verified**. Single-file experiments are verified by the snippet checker.

### Experiment 1 🧩: Break the ABI on purpose (three ways)

**1a. Insert a data member.** `Config` is `{width, height}` in v1 and `{width, depth, height}` in v2. `config.cpp` is the library; `client_config.cpp` calls `make_config()` and `area()`. The client is compiled against v1 and **never rebuilt**; we swap the library underneath it.

```bash
g++-14 -O1 -fPIC -shared -Iv1 config.cpp -o libcfg_v1.so        # library built from v1 headers
g++-14 -O1 -fPIC -shared -Iv2 config.cpp -o libcfg_v2.so
g++-14 -O1 -Iv1 client_config.cpp -L. -l:libcfg_v1.so -Wl,-rpath,'$ORIGIN' -o cfg_client
./cfg_client                      # with the v1 library
cp libcfg_v2.so libcfg_v1.so      # "upgrade" the library, same file name and SONAME
./cfg_client
```

```text
client sees width=3 height=4   library area()=12              <- matched: all good
client sees width=3 height=0   library area()=1079662848      <- v2 library, v1 client
```

(The garbage in the last number differs on every run, because the library reads past the end of the client's 8-byte object.) What happened: `make_config()` in v2 returns a 12-byte struct whose first 8 bytes (`width`, `depth`) travel in `rax` and whose last 4 (`height`) travel in `rdx`; the v1 client expects an 8-byte struct entirely in `rax` and reads `height` as v2's `depth` (0); `area()` receives a pointer to the client's 8-byte object and reads `height` at offset 8, **beyond the end of it**. No crash, no diagnostic, wrong results, and a stack over-read.

**1b. Insert a virtual function.** `Shape` has `sides()` and `corners()`; v2 adds `name()` *before* them.

```text
v1 library:
client calls sides()   -> 4
client calls corners() -> 40
v2 library swapped in, client NOT recompiled:
client calls sides()   -> -538259456        <- actually called name(): returned a char* truncated to int
client calls corners() -> 4                 <- actually called sides()
```

The client has “slot 3” compiled in for `sides()` and “slot 4” for `corners()`; in v2 slot 3 is `name()` and slot 4 is `sides()`. Every virtual call lands one function away, and `delete s` still works only because the destructor slots happen to come first. If the signatures had been incompatible (an `int` vs a `std::string` return) this would crash.

**1c. The same change behind pImpl survives.** `Widget` holds only `std::unique_ptr<Impl>`; the library's private `Impl` changes completely between versions (`int a` becomes a `double`, a `long[4]`, then `int a`), and the client is not recompiled:

```text
v1 library: client: Widget::value() = 10  (sizeof(Widget) = 8)
v2 library: client: Widget::value() = 42  (sizeof(Widget) = 8)    <- new private layout, same client binary
```

The client's compiled-in knowledge is `sizeof(Widget) == 8` and the mangled names of the four public functions, and both are unchanged. This is *why* pImpl exists: it is an ABI firewall, at the price of an allocation and an indirection per object.

### Experiment 2 ✅: What the layout rules actually do

`sizeof`, offsets and pointer adjustments, printed by a program (all 🧩 Itanium on x86-64; other platforms differ):

```cpp
// @test run -std=c++23 -O0
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

struct Plain       { char c; int i; char d; };                  // padding between members, and at the end
struct Reordered   { int i; char c; char d; };                  // same members, different order => different size
struct Poly        { virtual ~Poly() = default; int x; };       // gains a vptr
struct Empty       {};
struct WithEmpty   { Empty e; int i; };                         // an empty member still takes space
struct EmptyBase : Empty { int i; };                            // empty base optimisation: zero bytes
struct WithNUA     { [[no_unique_address]] Empty e; int i; };  // C++20: also zero bytes (GCC/Clang)

struct A { int a; virtual void fa() {} };
struct B { int b; virtual void fb() {} };
struct C : A, B { int c; };                                     // non-virtual multiple inheritance

struct VBase { int v; };
struct V1 : virtual VBase { int x; };

int main() {
    std::printf("Plain %zu, Reordered %zu          (members: char,int,char vs int,char,char)\n", sizeof(Plain), sizeof(Reordered));
    std::printf("offsets in Plain: c=%zu i=%zu d=%zu   alignof=%zu\n", offsetof(Plain, c), offsetof(Plain, i), offsetof(Plain, d), alignof(Plain));
    std::printf("Poly (one int + virtual dtor): %zu   (8 for the vptr + 4 + 4 padding)\n", sizeof(Poly));
    std::printf("Empty %zu, WithEmpty %zu, EmptyBase %zu, WithNUA %zu\n", sizeof(Empty), sizeof(WithEmpty), sizeof(EmptyBase), sizeof(WithNUA));
    std::printf("A %zu, B %zu, C : A, B => %zu\n", sizeof(A), sizeof(B), sizeof(C));

    C obj;
    C* pc = &obj;
    A* pa = pc;
    B* pb = pc;                                                  // implicit conversion: the compiler ADDS an offset
    std::printf("C* -> A*: byte offset %td;   C* -> B*: byte offset %td   <- static_cast between bases is not always a no-op\n",
                reinterpret_cast<char*>(pa) - reinterpret_cast<char*>(pc), reinterpret_cast<char*>(pb) - reinterpret_cast<char*>(pc));
    std::printf("virtual base: sizeof(V1) = %zu (vptr + x + shared VBase placed after)\n", sizeof(V1));

    std::printf("standard_layout: Plain=%d Poly=%d C=%d;  trivially_copyable: Plain=%d Poly=%d\n",
                std::is_standard_layout_v<Plain>, std::is_standard_layout_v<Poly>, std::is_standard_layout_v<C>,
                std::is_trivially_copyable_v<Plain>, std::is_trivially_copyable_v<Poly>);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
Plain 12, Reordered 8          (members: char,int,char vs int,char,char)
offsets in Plain: c=0 i=4 d=8   alignof=4
Poly (one int + virtual dtor): 16   (8 for the vptr + 4 + 4 padding)
Empty 1, WithEmpty 8, EmptyBase 4, WithNUA 4
A 16, B 16, C : A, B => 32
C* -> A*: byte offset 0;   C* -> B*: byte offset 16   <- static_cast between bases is not always a no-op
virtual base: sizeof(V1) = 16 (vptr + x + shared VBase placed after)
standard_layout: Plain=1 Poly=0 C=0;  trivially_copyable: Plain=1 Poly=0
```

Each line is a fact the ABI fixes *and* the standard does not: a class's size depends on member **order** (so reordering members is an ABI break and also a free size optimisation), a virtual function adds 8 bytes, the empty-base trick and `[[no_unique_address]]` save the byte(s) that an ordinary empty member would cost, and `static_cast<B*>(c)` does arithmetic.

### Experiment 3 🧩: The calling convention, read from the assembly

Same shapes, different ABI treatment: a trivial two-int struct, the same struct with a user-declared destructor, a large struct, a raw pointer vs `unique_ptr`:

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fcf-protection=none filter=take_pair,take_nontrivial,take_ptr,take_unique,make_pair,make_big
#include <memory>

struct Pair { int a, b; };                         // trivially copyable, 8 bytes
struct NonTrivial { int a, b; ~NonTrivial() {} };  // user-provided destructor => passed in memory
struct Big { long a, b, c; };                      // 24 bytes > 16

int take_pair(Pair p)             { return p.a + p.b; }
int take_nontrivial(NonTrivial n) { return n.a + n.b; }
int take_ptr(int* p)              { return *p; }
int take_unique(std::unique_ptr<int> p) { return *p; }
Pair make_pair(int a, int b)      { return Pair{a, b}; }
Big  make_big(long a)             { return Big{a, a + 1, a + 2}; }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
take_pair(Pair):
	mov	rax, rdi
	shr	rax, 32
	add	eax, edi
	ret

take_nontrivial(NonTrivial):
	mov	eax, DWORD PTR [rdi]
	add	eax, DWORD PTR 4[rdi]
	ret

take_ptr(int*):
	mov	eax, DWORD PTR [rdi]
	ret

take_unique(std::unique_ptr<int, std::default_delete<int> >):
	mov	rax, QWORD PTR [rdi]
	mov	eax, DWORD PTR [rax]
	ret

make_pair(int, int):
	sal	rsi, 32
	mov	eax, edi
	or	rax, rsi
	ret

make_big(long):
	lea	rdx, 1[rsi]
	mov	QWORD PTR [rdi], rsi
	add	rsi, 2
	mov	rax, rdi
	mov	QWORD PTR 8[rdi], rdx
	mov	QWORD PTR 16[rdi], rsi
	ret
```

Read the register usage (System V x86-64):

- `take_pair`: the whole 8-byte struct arrives **packed in `rdi`** (`edi` = `a`, upper half = `b`; the code shifts to separate them).
- `take_nontrivial`: the argument is an **address in `rdi`**: the caller built a temporary in memory and will run the destructor after the call.
- `take_ptr`: one pointer, one register, a single load.
- `take_unique`: also an address in `rdi`, and the callee does two dependent loads (the pointer out of the `unique_ptr` object in memory, then the `int`). Note what is *absent*: there is no `delete` here. With the Itanium ABI the **caller** owns the parameter object and destroys it after the call returns, so the cost of by-value `unique_ptr` is paid at every call site (a spill to memory before, a destructor check after), not inside the callee.
- `make_pair`: returns both ints **packed in `rax`**. `make_big`: writes through a hidden pointer passed in `rdi` (`sret`) and returns that pointer in `rax`.

> **Opinion.** “Pass `unique_ptr` by value to express ownership transfer” is the right *design*; know that on this ABI it costs a trip through memory and a destructor call, which is irrelevant for cold code and visible in a hot call. The standard library cannot fix it without breaking binary compatibility (P1819 and others were rejected for exactly that reason); `[[clang::trivial_abi]]` (Clang) is the vendor extension that opts a type into register passing, at the price of changing *when* its destructor runs.

### Experiment 4 🧩: The `std::string` dual ABI

`dual.cpp` defines `std::string greet(const std::string&)`. Compile it twice: with the default new ABI and with `-D_GLIBCXX_USE_CXX11_ABI=0`.

```bash
g++-14 -std=c++17 -c dual.cpp -o dual_new.o
g++-14 -std=c++17 -D_GLIBCXX_USE_CXX11_ABI=0 -c dual.cpp -o dual_old.o
nm dual_new.o | grep greet
nm dual_old.o | grep greet
g++-14 -std=c++17 -c dual_main.cpp -o dual_main_new.o
g++-14 dual_main_new.o dual_old.o -o dual_bad
```

```text
0000000000000000 T _Z5greetRKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE      <- new ABI: std::__cxx11::basic_string
0000000000000000 T _Z5greetRKSs                                                         <- old ABI: "Ss" = std::string (COW)
/usr/bin/ld: dual_main_new.o: in function `main':
dual_main.cpp:(.text+0x4f): undefined reference to `greet(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&)'
```

The two `std::string` types are **different types** with different mangled names (`Ss` vs `NSt7__cxx11…`), so a mismatch becomes a *link error* instead of silent corruption, which is the friendly outcome of libstdc++'s design: **the ABI choice is encoded in the symbol name, so the linker catches it.** A pre-2015 prebuilt library and your GCC ≥ 5 build is the classic way to meet this error (“undefined reference to … `std::__cxx11::basic_string` …” or the reverse); the fix is to build both sides with the same `_GLIBCXX_USE_CXX11_ABI`, never to ignore it.

### Experiment 5 ✅: A vtable, found and called by hand

An Itanium-specific look inside a polymorphic object: the first word is the vptr; the entries after the (destructor) slots are function pointers in declaration order. **This is not portable and is UB by the standard**: a demonstration, never code to ship.

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstring>

struct Shape {
    virtual ~Shape() = default;
    virtual int sides() const   { return 4; }
    virtual int corners() const { return 40; }
    int tag = 7;
};

int main() {
    Shape s;
    void** vptr = *reinterpret_cast<void***>(&s);        // word 0 of the object: pointer to the vtable's first function slot

    std::printf("first word of the object (vptr) is non-null: %s\n", vptr != nullptr ? "yes" : "no");
    std::printf("vtable[-1] (typeinfo) non-null: %s;  vtable[-2] (offset-to-top) = %ld\n", vptr[-1] != nullptr ? "yes" : "no", reinterpret_cast<long>(vptr[-2]));

    // slots: 0 = ~Shape (complete), 1 = ~Shape (deleting), 2 = sides, 3 = corners
    using Fn = int (*)(const Shape*);
    auto sides_fn   = reinterpret_cast<Fn>(vptr[2]);
    auto corners_fn = reinterpret_cast<Fn>(vptr[3]);
    std::printf("calling slot 2 by hand -> %d   (Shape::sides)\n", sides_fn(&s));
    std::printf("calling slot 3 by hand -> %d   (Shape::corners)\n", corners_fn(&s));
    std::printf("normal virtual calls   -> %d, %d\n", s.sides(), s.corners());

    char* bytes = reinterpret_cast<char*>(&s);
    std::printf("member `tag` is at byte offset %td (after the 8-byte vptr)\n", reinterpret_cast<char*>(&s.tag) - bytes);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
first word of the object (vptr) is non-null: yes
vtable[-1] (typeinfo) non-null: yes;  vtable[-2] (offset-to-top) = 0
calling slot 2 by hand -> 4   (Shape::sides)
calling slot 3 by hand -> 40   (Shape::corners)
normal virtual calls   -> 4, 40
member `tag` is at byte offset 8 (after the 8-byte vptr)
```

This is exactly what the compiler emits for `s.sides()` when it cannot devirtualise: *load the vptr, index slot 2, call*. Experiment 1b is this program with the slot numbers out of date.

### Experiment 6 🧩: Symbol versioning: how libstdc++ keeps twenty years of binaries alive

```bash
L=/usr/lib/x86_64-linux-gnu/libstdc++.so.6        # libstdc++.so.6.0.33 on this machine
objdump -T $L | grep -oE 'GLIBCXX_[0-9.]+' | sort -uV | wc -l       # how many distinct versions
objdump -T $L | grep -oE 'GLIBCXX_[0-9.]+' | sort -uV | tail -3
printf '#include <iostream>\n#include <string>\nint main(){std::string s="x";std::cout<<s;}\n' > req.cpp
g++-14 -std=c++17 req.cpp -o req
objdump -T req | grep -oE 'GLIBCXX_[0-9.]+|CXXABI_[0-9.]+|GLIBC_[0-9.]+' | sort -uV | tr '\n' ' '
```

```text
34 distinct GLIBCXX versions;  the newest:  GLIBCXX_3.4.31  GLIBCXX_3.4.32  GLIBCXX_3.4.33
this small program requires:  CXXABI_1.3 GLIBCXX_3.4 GLIBCXX_3.4.21 GLIBCXX_3.4.32 GLIBC_2.2.5 GLIBC_2.4 GLIBC_2.34
```

Every exported libstdc++ symbol is tagged with the library version in which it appeared. A binary records the **maximum versions it needs**; at load time `ld.so` checks the installed library defines them. Hence: a program built on a machine with GCC 14 (needs `GLIBCXX_3.4.32`) **refuses to start** on a system whose libstdc++ is older (“version `GLIBCXX_3.4.32' not found”), while any program built by older GCC runs on the newer library. This is the real-world “works on my build machine, not on the customer's Ubuntu 20.04” failure, and the reason distributions pin the oldest supported GCC runtime and companies build on old base images (or link `libstdc++` statically with `-static-libstdc++ -static-libgcc`).

For comparison, the object sizes that the standard library's ABI freezes (libstdc++, x86-64; GCC and Clang agree because they share the library):

```text
string 32   vector 24   unique_ptr 8   shared_ptr 16   function 32   map 48   list 24   optional<int> 8
```

(`std::string` is 32 bytes in libstdc++ and 24 in libc++: two standard libraries, two ABIs: another reason a `.so` exposing `std::string` ties its users to your standard library.)

---

## 8. Assembly / runtime investigation

```bash
# 1. Mangling both ways
c++filt _ZN3geo4areaERKNS_5ShapeE            # geo::area(geo::Shape const&)
g++-14 -S -o - x.cpp | grep -E '^_Z[0-9A-Za-z_]+:'          # the symbols the compiler is defining
nm -C --defined-only obj.o ; nm -u obj.o                     # defined vs needed

# 2. Layout, straight from the compiler
g++-14 -fdump-lang-class -c x.cpp           # GCC: writes x.cpp.*.class with offsets, vptr, bases, vtable layout
clang++-18 -Xclang -fdump-record-layouts -fsyntax-only x.cpp # Clang: record layouts (sizes, offsets, padding)
clang++-18 -Xclang -fdump-vtable-layouts -fsyntax-only x.cpp # Clang: vtables
pahole -C Config ./a.out                    # dwarves: struct layouts with holes from DWARF

# 3. Vtables and RTTI as symbols
nm -C obj.o | grep -E 'vtable for|typeinfo for'              # _ZTV, _ZTI, _ZTS symbols
objdump -s -j .data.rel.ro obj.o | head                     # raw vtable contents

# 4. Calling convention: look at it
g++-14 -O2 -S -o - x.cpp | less             # arguments in rdi/rsi/rdx/rcx/r8/r9, xmm0-7; return in rax/rdx/xmm0
#    -fdump-tree-original shows the compiler's view of "invisible reference" temporaries

# 5. Detect ABI breaks automatically (libabigail)
abidw --out-file v1.xml libfoo.so.1          # dump ABI of the old library
abidw --out-file v2.xml libfoo.so.2
abidiff v1.xml v2.xml                         # reports added/removed/changed symbols and type layouts
abidiff --headers-dir1 inc1 --headers-dir2 inc2 libfoo.so.1 libfoo.so.2   # only public-header types

# 6. What does the loader think?
readelf --dyn-syms -W lib.so | head          # versioned symbols appear as name@@VERSION
readelf -V lib.so                             # version definitions and requirements
LD_DEBUG=versions ./app 2>&1 | head           # load-time version checks
readelf -d ./app | grep -E "SONAME|NEEDED"
```

---

## 9. Implementation exercise

Design and implement a **small ABI-stable C++ library** and prove it.

1. Write `libtext.so` with a class `Document` (open, `line_count()`, `get_line(i)`, `search(pattern)`). Expose it two ways: (a) a pImpl C++ class, (b) an `extern "C"` API with an opaque `doc_t*` handle and explicit `doc_open/doc_close`.
2. Build `app_v1` against v1 of the library. Then create v2 with: a new private data member, a new public method, a changed internal algorithm, a bug fix in an inline function. Without recompiling `app_v1`, swap the library and confirm it still works.
3. Now make the *forbidden* changes one at a time (reorder members of a public struct, add a virtual function, change a default argument, change a parameter from `int` to `long`) and record what `abidiff` reports and what the old binary does.
4. Add an `inline namespace v2 { }` to the new API and keep the old `v1` symbols exported so both generations of client run against one library.

<details>
<summary><strong>Solution sketch: the C API wrapper and the C++ RAII on top</strong></summary>

The stable boundary is a tiny C interface; the C++ class is a header-only wrapper compiled into each client, so it is free to change.

```cpp
// @test run -std=c++23 -O1 -g -fsanitize=address,undefined
// ---- what would be text_api.h (the ABI: C types only, opaque handle, explicit ownership) ----
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
typedef struct doc doc_t;                                   // opaque: clients never see the layout
doc_t* doc_open(const char* text);                          // returns NULL on failure
void   doc_close(doc_t*);                                   // the only way to free
size_t doc_line_count(const doc_t*);
const char* doc_line(const doc_t*, size_t i);               // valid until doc_close
}

// ---- the library implementation (in the .so; free to change its internals) ----
struct doc { std::vector<std::string> lines; };
extern "C" doc_t* doc_open(const char* text) {
    if (!text) return nullptr;
    auto* d = new (std::nothrow) doc;
    if (!d) return nullptr;
    std::string_view v(text);
    for (size_t pos = 0; pos <= v.size();) {
        auto nl = v.find('\n', pos);
        if (nl == std::string_view::npos) nl = v.size();
        d->lines.emplace_back(v.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return d;
}
extern "C" void doc_close(doc_t* d) { delete d; }
extern "C" size_t doc_line_count(const doc_t* d) { return d->lines.size(); }
extern "C" const char* doc_line(const doc_t* d, size_t i) { return i < d->lines.size() ? d->lines[i].c_str() : nullptr; }

// ---- the client-side C++ wrapper (header-only, compiled into the client): RAII on top of the C ABI ----
class Document {
    struct Deleter { void operator()(doc_t* d) const noexcept { doc_close(d); } };
    std::unique_ptr<doc_t, Deleter> h_;
public:
    explicit Document(const char* text) : h_(doc_open(text)) { if (!h_) throw std::runtime_error("doc_open failed"); }
    std::size_t line_count() const { return doc_line_count(h_.get()); }
    std::string_view line(std::size_t i) const { const char* s = doc_line(h_.get(), i); return s ? s : std::string_view{}; }
};

int main() {
    Document d("alpha\nbeta\ngamma");
    for (std::size_t i = 0; i < d.line_count(); ++i) std::printf("%zu: %.*s\n", i, static_cast<int>(d.line(i).size()), d.line(i).data());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
0: alpha
1: beta
2: gamma
```

The ABI surface is five C functions, an opaque pointer and `const char*`: nothing in it can drift when you change `std::vector<std::string>` for something else. The wrapper gives clients RAII and `string_view` without exposing a single C++ type across the boundary (this is Chapter 45's pattern).

</details>

---

## 10. Real-world example

| Where | ABI strategy |
|---|---|
| **libstdc++ / glibc** | Symbol versioning, never change an existing type's layout; `GLIBCXX_3.4.x` accumulates, old binaries keep running |
| **MSVC STL** | Stable ABI since VS 2015 (until the announced "vNext"); known inefficiencies (`std::mutex` size, `std::regex`) preserved on purpose |
| **libc++ (LLVM)** | Stable ABI by default on Apple platforms; an opt-in `_LIBCPP_ABI_UNSTABLE` for people who rebuild everything |
| **Qt** | Binary-compatibility promise within a major version: every public class uses the **d-pointer** (pImpl), `Q_DECLARE_PRIVATE`, reserved virtual slots, `Q_DECL_EXPORT`; adding a virtual function is forbidden in minor releases (Chapter 47) |
| **KDE Frameworks** | Written "Binary Compatibility Issues With C++" guidelines (what you may and may not change), still the best practical checklist |
| **COM / CORBA / Windows API** | Abstract-interface-only ABIs: pure virtual classes with `QueryInterface`, never changed once published |
| **Python / Lua / Node native modules** | `PyObject*` C ABI; C++ extensions must match the interpreter's runtime or use the stable ABI (`abi3`), see Chapter 46 |
| **Rust, Go** | Do not promise a stable ABI; `extern "C"` is the interoperability layer, as in C++ |
| **Linux distributions** | Rebuild the world on a compiler-ABI bump; `abi-compliance-checker` in CI; “ABI break” bugs are release-blockers |

> **Opinion.** Treat **ABI stability as a product feature you must choose to pay for**, not a default. If you control all the code, don't pay: build from source with one toolchain and enjoy `std::string_view`, templates and inline everything in your interfaces. If you must ship a binary interface, **make it small and C-shaped**: opaque handles, plain-old-data, explicit lifetimes, versioned entry points. The designs that survive decades (POSIX, Win32, COM, SQLite, zlib, Qt's d-pointers) are all deliberately boring. And the standardisation lesson: the committee's refusal to break ABI is not laziness; every user of a prebuilt library would pay. That is *why* `std::vector<bool>`, `std::regex`, `std::unordered_map`'s bucket structure and `unique_ptr`'s parameter passing are what they are.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Adding/reordering/removing a **data member** of a public class | Wrong values, over-reads, crashes in clients not rebuilt (Experiment 1a) | pImpl; or version the type (`Config2`) and add new functions |
| Adding or reordering a **virtual function** | Calls land on the wrong function (Experiment 1b) | Never change published vtables; add new interfaces; append only if no client derives |
| Changing a **default argument** | Old clients keep the old default (it is baked into the call site) | Use overloads instead of default arguments in ABI surfaces |
| Changing an **inline function** | Old clients keep running the old body; new ones the new: a split-brain (ODR-adjacent) | Do not rely on inline in ABI surfaces; version them |
| Changing a **parameter type or constness**, adding `noexcept` | Mangled name changes: old binaries fail to link/load (`undefined symbol`) | Keep the old overload exported (`inline namespace`/`.symver`) |
| **Mixing `_GLIBCXX_USE_CXX11_ABI` settings** | Link errors mentioning `std::__cxx11::…` (Experiment 4); worse with `-fpermissive` hacks | One setting per link; rebuild dependencies |
| **Different standard libraries** (libstdc++ vs libc++) across a boundary that exposes `std::` types | Crashes, bad_alloc, wrong sizes | Only C types at the boundary |
| **Different compilers/versions with `-std=` mismatch** for types whose layout depends on it | Rare, but real (`std::variant`, `std::tuple` layout changes across versions or `-fabi-version`) | Same compiler major version and flags; or C boundary |
| **Exceptions across a library boundary** built with different runtimes/flags (`-fno-exceptions`, different `libgcc`) | `terminate`, "exception not caught" | Catch at the boundary, convert to error codes; or one runtime |
| **Passing ownership types across a shared-library boundary with different allocators** | Heap corruption (allocated by one `malloc`/`operator new`, freed by another) | Free in the library that allocated; give the API its own `destroy()` |
| **Relying on `sizeof` or layout of a standard type in a binary file format or shared memory** | Silent breakage on a compiler/library change | Define the wire format explicitly, fixed-width integers, `static_assert` on size and offsets |
| **`#pragma pack` or `-fpack-struct` on one side only** | Misaligned or mis-sized structs | Same packing everywhere, or avoid it in public types |
| **A new compiler version that changed the ABI** (rare; e.g. GCC's `-fabi-version`, bitfield or empty-class bugs fixed) | Mixed old/new objects misbehave | Read release notes ("ABI changes"); `-Wabi` |
| **Trusting “it worked in testing”** | Production break after a *minor* library update | Run `abidiff` in CI against the previous release |

---

## 12. Exercises

1. **Predict, then verify.** For 10 small struct/class definitions you write (mixing `char/int/double`, virtuals, bases, empty classes, bitfields), predict `sizeof`/`alignof`/offsets, then check with `-Xclang -fdump-record-layouts` and `pahole`.
2. **Reproduce an upgrade bug.** Pick a field of a *real* open-source library's public struct, simulate adding a member in the middle, and demonstrate wrong behaviour in an unmodified client exactly as in Experiment 1a.
3. **Registers.** Write six functions taking: `Pair{int,int}`, `Pair3{int,int,int}`, `struct{double,double}`, `struct{int,double}`, `struct{double[3]}`, and `std::string_view`. Predict how each is passed (registers/memory) from the System V rules, then confirm in the assembly.
4. **`[[clang::trivial_abi]]`.** Compile a smart-pointer-like class with and without `[[clang::trivial_abi]]`; compare the assembly of a by-value call and describe what changes about destructor timing.
5. **Dual ABI.** Build one source file in both `_GLIBCXX_USE_CXX11_ABI` modes, link a program that mixes a `std::string` argument and a `std::list` size call; list which symbols differ in `nm` and which combinations link and run.
6. **`abidiff`.** Create libfoo v1/v2 with five different changes (one safe, four unsafe); have `abidiff` classify each, and decide for each whether *you* would call it a break.
7. **Symbol versioning.** Write a version script that exports `foo@VER_1` (old behaviour) and `foo@@VER_2` (new default) from one library; link an old client and a new client and show each calls its own version.
8. **Read.** Skim the Itanium C++ ABI document: find the rules for (a) the vtable of a class with a virtual base, (b) the mangling of a lambda, (c) the guard variable of a function-local static. Summarise each in two sentences.

---

## 13. Challenge: an ABI contract tester

Write a tool and CI script that protects a library from accidental ABI breaks. Inputs: the last released `libfoo.so` and the current build. Outputs: a report from `abidiff` filtered to the **public headers**; a generated C++ test that `static_assert`s `sizeof`, `alignof`, `offsetof` of every public struct and the vtable slot of every public virtual function (use a short Clang AST or `-fdump-record-layouts` parser); and a runtime test that loads the *previous* release's client binaries against the new library. Fail the build on any difference not whitelisted in an `abi_changes.txt` that requires a reviewer's sign-off. Apply it to a real library you maintain and report what it finds in the history of its last ten releases.

---

## 14. Knowledge check

1. What is the difference between API and ABI? Give a change that breaks one but not the other.
2. List five things compiled into a client object file that a library change can invalidate.
3. Why does inserting a virtual function in the middle of a class break old clients while adding a non-virtual member function does not?
4. Why did Experiment 1a return garbage for `height` and read out of bounds in `area()`?
5. How does pImpl act as an ABI firewall? What does it cost?
6. How are a trivially-copyable `struct {int a, b;}` and the same struct with a user-provided destructor passed on x86-64 System V? What does that imply for `unique_ptr` parameters?
7. What is the “dual ABI” of libstdc++, and why does a mismatch show up as a *link* error?
8. What does symbol versioning (`GLIBCXX_3.4.32`) protect, and what error do you see when a program is moved to an older system?
9. Why does `static_cast<B*>(derived_ptr)` sometimes change the address? What is `offset-to-top`?
10. Why can the C++ standard library not make `std::unordered_map` faster, even though everyone knows how?
11. Which of these are ABI-safe for a published class: adding a data member / adding a non-virtual method / changing a function body in the `.so` / changing a default argument / adding `noexcept` to a method?
12. Give the design you would choose for a plug-in interface to be loaded by applications you don't control, and explain each choice.

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. API = source-level interface; ABI = binary-level. Renaming a private data member breaks neither; reordering members breaks ABI but not API (source recompiles); removing a deprecated function breaks both; changing an inline function body breaks no API but creates two behaviours at the ABI level.
2. Struct sizes/offsets, vtable slot numbers, mangled names of every referenced function, inline function bodies, default-argument values, template instantiations, enum underlying types, how types are passed (trivial vs non-trivial).
3. Virtual calls use a vtable slot index compiled into the client; inserting shifts all later indices. A non-virtual member function is a normal symbol looked up by name at link/load time and does not change layout.
4. The v1 client believes `Config` is 8 bytes with `height` at offset 4 (returned entirely in `rax`); the v2 library returns 12 bytes (`rax` + `rdx`), so the client reads `depth` where it expects `height`; `area()` was compiled for v2's layout and reads `height` at offset 8 of an object the client allocated with only 8 bytes.
5. The client only knows the size of one pointer and the mangled names of public functions; everything else lives behind the pointer in the library. Cost: a heap allocation per object, an indirection per access, no inline access to members.
6. The trivial 8-byte struct is passed packed in a register (`rdi`); the struct with a user-provided destructor is passed by invisible reference (address in `rdi`) because the caller must destroy it. `unique_ptr<T>` thus passes in memory with a destructor call by the caller, unlike `T*`.
7. libstdc++ ships two implementations of `std::string`/`std::list` (old COW/old-size and C++11-conforming), the new one in inline namespace `std::__cxx11`. The namespace is part of the mangled name, so mixing yields different symbol names → an undefined reference at link time rather than silent misbehaviour.
8. It lets the library add new symbols without disturbing old ones and lets old binaries keep binding to the old versions. A binary records the newest versions it uses; if the installed libstdc++ is older, `ld.so` reports "version `GLIBCXX_3.4.32' not found" and refuses to start.
9. With multiple inheritance, the second base subobject lives at a nonzero offset within the derived object; the conversion adds that offset. `offset-to-top` in the vtable stores the amount to add back to get from the base subobject to the full object (used by `dynamic_cast<void*>` and virtual thunks).
10. Its representation (node layout, bucket array, hashing helpers) is inlined into every program that uses it and also lives in the compiled library; changing it would make every existing binary read the wrong layout, so implementers refuse.
11. Safe: adding a non-virtual method; changing a body in the `.so` (non-inline). Unsafe: adding a data member; changing a default argument (baked into callers); adding `noexcept` (changes the mangled name since C++17, so old clients can't find the symbol).
12. A C ABI: a single `extern "C"` entry point returning a struct of function pointers (versioned, with a size/version field first) or an opaque handle plus functions; POD types only; explicit create/destroy in the same module; no exceptions or STL types across the boundary; hidden visibility except the entry point; SONAME/version in the struct to allow evolution; optional C++ RAII wrapper compiled into the host.

</details>

---

[← Previous: Chapter 36](36-compilation-model.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 38 — Modules →](38-modules.md)
