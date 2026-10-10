# Chapter 2 — The C++ Object Model

> **Part II · Object model and lifetime** &nbsp;|&nbsp; **Level 2** (language mechanisms) &nbsp;|&nbsp; **≈ 5 hours**
> **Prerequisites:** [Chapter 1](../part-01-mental-model/01-modern-cpp-philosophy.md) &nbsp;|&nbsp; **Standards:** C++11 → C++23 &nbsp;|&nbsp; **Tools:** `g++`, `clang++`, UBSan, `objdump`

[← Previous: Chapter 1](../part-01-mental-model/01-modern-cpp-philosophy.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 3 — Lifetime and storage →](03-lifetime-and-storage.md)

---

**In one sentence:** in C++, *memory is not an object*. Storage is just bytes; an object is something that **comes into existence** at a precise moment, has a type and an identity, and **stops existing** at another precise moment, and almost every rule about "safe" low-level code is a rule about those two moments.

**By the end of this chapter you can:**

- say exactly what an *object*, a *subobject*, and *storage* are, and how they differ
- predict `sizeof`, `alignof` and member offsets, and explain *why* (including tail-padding reuse)
- classify any type as trivial / trivially copyable / standard-layout / aggregate / implicit-lifetime, and say what each class of type permits
- explain what happens between `void* memory = …;` and `T object;`
- choose correctly between `memcpy`, `reinterpret_cast`, `std::bit_cast`, `std::launder` and `std::start_lifetime_as`

---

## 1. Problem

C has a simple model: memory is bytes, and a **type is a lens** you view them through. You may `malloc` a block and treat it as an array of `struct S`. The rules about *effective type* exist but are rarely a practical concern.

C++ cannot work that way, because **C++ types have invariants**.

```cpp
std::string s;        // not 32 bytes of anything; a pointer + size + capacity + SSO buffer,
                      // whose pointer must point somewhere valid, or into itself
```

If an arbitrary bit pattern is viewed as a `std::string`, the class's guarantees (*"the pointer owns its allocation"*, *"the destructor will free it exactly once"*) are not merely unmet; they are meaningless. A constructor is the thing that **establishes** the invariant; a destructor is the thing that **retires** it. Between those two events the object exists. Before and after, there is only raw storage.

This raises questions that C never needed to answer:

- When does an object *begin* to exist? When storage is allocated? When the constructor finishes?
- What may I do with a pointer to storage where no object currently lives?
- Can I copy an object by copying its bytes? Always? Sometimes?
- Can two different objects have the same address?
- If I destroy an object and build another in the same place, is my old pointer still good?
- How can `std::vector` allocate room for 1000 elements without creating 1000 elements, and is *that* legal?

The **object model** is C++'s answer. It is not an optional "advanced topic": it is the rulebook that determines whether code like `std::vector`, `std::optional`, an arena allocator, or a binary-protocol parser is **defined behaviour or merely works on your compiler**.

---

## 2. Historical context

| Era | Approach | Limitation |
|---|---|---|
| **C** | *Effective type* rules: memory takes the type of the last store; `char` may alias anything | Silently permits things that break C++ invariants |
| **C++98/03** | **POD** ("plain old data") types behave like C structs; everything else is "an object, created by a constructor" | `memcpy` and `malloc` were blessed for PODs, but the wording about *when* objects exist was vague; `std::vector` was, strictly, not implementable |
| **C++11** | POD split into **trivial** and **standard-layout**; `alignas`/`alignof`; `std::aligned_storage` | Better vocabulary, but still no way to say "these bytes now hold an object" without a constructor call |
| **C++17** | `std::byte`; `std::launder` (a pointer *optimization barrier*) | `launder` solves one problem and confuses everyone; `reinterpret_cast`-based parsing still technically UB |
| **C++20** | **Implicit object creation** (P0593, applied retroactively); `std::bit_cast`; `std::construct_at`/`destroy_at`; POD deprecated; `[[no_unique_address]]` | Parsing a byte buffer *in place* still required a copy |
| **C++23** | `std::start_lifetime_as` (library support still arriving: GCC 16); `std::is_implicit_lifetime` | |

The thread running through this history: **the standard slowly learned to describe what systems programmers were already doing**, in a way that keeps the optimizer's assumptions sound.

> [!NOTE]
> Today the rules are much friendlier than folklore says. P0593 made operations like `malloc`, `operator new` and `memcpy` *implicitly create* the objects needed to make a program correct, so most "obviously fine" low-level code is now defined. Know both what is defined and what is still a trap.

---

## 3. Modern solution

Modern C++ gives you a precise vocabulary and a small toolbox.

**Vocabulary** (details in §5):

```text
storage      bytes that exist with a given size, alignment and storage duration
object       a region of storage with a type, a lifetime and an identity
subobject    a member, base class part, or array element of another object
lifetime     the interval during which the object may be used
```

**Toolbox:**

| To do this | Use | Chapter / section |
|---|---|---|
| Create an object in existing storage | `std::construct_at(p, args…)` or `::new (p) T(args…)` | §7.6, [Ch. 3](03-lifetime-and-storage.md) |
| End an object without freeing storage | `std::destroy_at(p)` or `p->~T()` | [Ch. 3](03-lifetime-and-storage.md) |
| Reinterpret bytes as a value (copy) | `std::memcpy` into an object, or `std::bit_cast<T>(x)` | §7.7 |
| Treat existing bytes *as* an object (no copy) | `std::start_lifetime_as<T>(p)` (C++23) | §8 |
| Get a usable pointer to the object now at an address | `std::launder(p)` | §5.7 |
| Provide properly aligned raw storage | `alignas(T) std::byte buf[sizeof(T)]` | §9 |
| Express "no unique address" for empty members | `[[no_unique_address]]` | §7.3 |
| Ask what a type permits | `std::is_trivially_copyable_v<T>`, `std::is_standard_layout_v<T>`, … | §7.4 |

---

## 4. Mental model

Hold this picture of three layers, and of *events* that move you between them:

```text
        ┌───────────────────────────────────────────────────────────────┐
 BYTES  │ storage: 0x…10  00 00 00 00 00 00 00 00 00 00 00 00 00 00 …   │   exists as soon as it's obtained
        └───────────────────────────────────────────────────────────────┘
                 │                                          ▲
   construction  │  new / construct_at / definition /       │  destruction: ~T() / destroy_at /
   (initialization completes)   implicit creation / memcpy  │  storage reuse / storage release
                 ▼                                          │
        ┌───────────────────────────────────────────────────────────────┐
 OBJECT │ T at 0x…10  — type, identity, lifetime  (invariants hold)     │   exists ONLY between the events
        └───────────────────────────────────────────────────────────────┘
                 │
                 ▼
        ┌───────────────────────────────────────────────────────────────┐
 VALUE  │ { id = 7, name = "x" }  — what the object currently means     │
        └───────────────────────────────────────────────────────────────┘
```

Three consequences you should internalize:

1. **Objects are events, not places.** The same bytes can hold a `Widget`, then nothing, then a `Gadget`. The address stays the same; the *object* does not.
2. **A pointer is more than an address.** It points *at an object* (or at one-past-the-end, or at nothing valid). Two pointers with the same numeric value can still be different things; this is why `std::launder` exists.
3. **Types constrain what bytes may mean.** `bool` has two valid bit patterns; a `std::string` has an invariant; a `double` may have padding bits on exotic hardware. Copying bytes is only meaning-preserving for types that say it is (*trivially copyable*).

> [!IMPORTANT]
> If a chapter in this course ever says *"it works on GCC"*, ask yourself which of the two events (construction, destruction) the code skipped.

---

## 5. Language rules

The numbers in `[brackets]` are stable section names in the [C++ draft](https://eel.is/c++draft/), so you can read the normative text.

### 5.1 What is an object?  `[intro.object]`

> **An object is created** by a definition, by a *new-expression*, by an operation that **implicitly creates objects**, when changing the active member of a union, or when a temporary object is created.

An object has a **type**, a **storage duration**, a **lifetime**, and optionally a **name**. Every object occupies **storage** (`sizeof(T)` bytes, at least one). Notice what is *not* on the list: *"it is an instance of a class"*. `int x;` defines an object. So does an array element, a function parameter, a lambda capture.

- A **complete object** is one that is not a subobject.
- A **subobject** is a *member subobject*, a *base class subobject*, or an *array element*.
- A **potentially-overlapping subobject** is a base class subobject, or a member declared `[[no_unique_address]]`. These are special: they are allowed to share bytes with their enclosing object's padding.

### 5.2 Identity and addresses  `[intro.object]`

Two objects with overlapping lifetimes that are not bit-fields have **distinct addresses** and disjoint storage, *unless*

- one is **nested within** the other (a member and its parent can share an address), or
- at least one is a **zero-size subobject** and they are of **different types**.

That second exception is the entire basis of the *empty base optimization*. It is also the reason a struct holding two members of the same empty type must be at least two bytes: two distinct objects of the same type cannot have the same address.

### 5.3 Object representation and value representation  `[basic.types.general]`

- The **object representation** of `T` is the sequence of `sizeof(T)` bytes it occupies.
- The **value representation** is the subset of bits that participate in the *value*.
- Everything else is **padding**: bits (usually whole bytes) inside the object that carry no value. Their contents are **unspecified** and **may change** whenever the object is written.

This single fact has two consequences you will meet again and again:

> **`memcmp` is not `operator==` for structs**, and **hashing the bytes of a struct is not hashing its value**. Use `std::has_unique_object_representations_v<T>` to find out when the bytes *are* the value.

### 5.4 Size and alignment  `[basic.align]`

- `alignof(T)` is the number of bytes between successive addresses at which an object of type `T` may be placed. It is always a power of two.
- `sizeof(T)` is always a multiple of `alignof(T)`, so that arrays of `T` keep every element aligned.
- `alignas(N)` can raise the alignment of a type or variable. A type requiring more than `alignof(std::max_align_t)` is **over-aligned**; `operator new` has had overloads taking `std::align_val_t` for these since C++17.
- Accessing an object through a misaligned pointer is **undefined behaviour**, even on hardware (x86) that would tolerate it. Compilers emit aligned SIMD loads, which fault.

### 5.5 What the standard says about layout

Remarkably little. The *guarantees*:

| Guarantee | Where |
|---|---|
| `sizeof(char) == sizeof(signed char) == sizeof(unsigned char) == sizeof(std::byte) == 1` | `[expr.sizeof]` |
| Every complete object has `sizeof ≥ 1` | `[expr.sizeof]` |
| Non-static data members are laid out in **declaration order** (increasing addresses). Older standards allowed reordering between members of different access control; no implementation ever did | `[class.mem]` |
| In a **standard-layout** class, a pointer to the object, converted with `reinterpret_cast`, points to its **first member** (they are *pointer-interconvertible*) | `[basic.compound]` |
| Array elements are contiguous | `[dcl.array]` |

Everything else (padding sizes, base-class placement, vtable pointer position, bit-field packing) is **left to the implementation**, and on Linux the implementation is *the ABI* (§6).

### 5.6 Type categories and what they allow

These names come from `<type_traits>`. They are *not* bureaucracy: each one unlocks specific, valuable operations.

| Category | Informal definition | What it unlocks |
|---|---|---|
| **Trivially copyable** | Copy/move constructors and assignments are all trivial (or deleted, with at least one usable), and the destructor is trivial | `memcpy`/`memmove` between objects; `std::bit_cast`; copying across process boundaries (with care) |
| **Trivial** | Trivially copyable **and** has a trivial default constructor | Uninitialized storage can be treated as an object without running anything |
| **Standard-layout** | No virtuals; same access control on all non-static members; at most one class in the hierarchy has data members; no repeated base types… | Layout compatible with C; `offsetof`; first-member pointer-interconvertibility |
| **Aggregate** | No user-declared/inherited constructors, no private non-static members, no virtual functions (C++20) | Brace initialization `T{a, b, c}`; designated initializers |
| **Implicit-lifetime** | An aggregate, array, scalar, or class with a trivial eligible constructor and a trivial destructor | Objects of this type can be **implicitly created** by `malloc`, `memcpy`, `operator new` and friends |
| **POD** *(deprecated in C++20)* | Trivial **and** standard-layout | Old name for "C-compatible"; prefer asking the precise question |

Two facts these categories encode:

- **Trivially copyable ≠ trivially constructible.** `struct WithCtor { WithCtor(int); int a; };` is trivially copyable (you may `memcpy` it) but has no trivial default constructor.
- **Standard-layout and trivially-copyable are independent.** A class with a non-trivial destructor can be standard-layout; a class with two access levels can be trivially copyable.

### 5.7 Lifetime  `[basic.life]`

> The lifetime of an object **begins** when storage with proper alignment and size is obtained **and its initialization is complete**.
> It **ends** when (class type with non-trivial destructor) the destructor call **starts**, or otherwise when the storage is released **or reused** by an object that is not nested within it.

Everything that follows depends on those two sentences. (Chapter 3 is entirely about them, so this is a preview.)

**Outside its lifetime, what may you do with a pointer to the old object?** Very little: treat it as a `void*`, compare it, cast it, read the `sizeof`. You may *not* call members, `delete` through it, or dereference it in the usual way.

**Ending a lifetime without a destructor call is legal.** Reusing the storage ends the old object's lifetime. If your program depends on the destructor's side effects, that is *your* problem; the standard permits it.

**Transparent replacement.** After you destroy an object and create a new one in the *same* storage, your old pointers, references and names may **automatically refer to the new object**. This is *transparent replacement*, and in the current wording it requires that

1. the new object's storage **exactly overlays** the old one's,
2. both have the **same type** (ignoring top-level cv-qualification),
3. the old object is **not a complete `const` object**,
4. neither is a **potentially-overlapping subobject** (base class or `[[no_unique_address]]` member).

When any of these fails, the old pointer **does not** refer to the new object, and you must use the pointer returned by `new`/`construct_at`, or `std::launder` it.

> [!NOTE]
> **Version note.** C++17 *also* excluded classes with `const` or reference data members from transparent replacement. The current draft no longer does; modern compilers behave accordingly. If you maintain code that supports older compilers, keep the `launder`.

### 5.8 `std::launder`  `[ptr.launder]`

`std::launder(p)` returns a pointer to the object **that currently lives at the address** `p` holds. Preconditions: an object of the right type exists there, alive, and every byte reachable through the result was reachable through `p`.

It is *not* a cast and does not make code safe. It is a statement to the compiler: *"forget what you know about the object this pointer used to refer to"*. Its main uses:

- replacing a **`const` complete object** or a **base class subobject** in place,
- obtaining a pointer to an object created in a `std::byte` buffer when you did not keep the pointer `new` returned.

### 5.9 Implicit object creation  `[intro.object]`

Since P0593 (applied to C++20 as a defect report, so compilers apply it to older modes too), certain operations **implicitly create objects** of *implicit-lifetime types* in the storage they provide, **if doing so gives the program defined behaviour**:

- `operator new`, `malloc`, `calloc`, `realloc`
- `memcpy` and `memmove` (into the destination)
- creating an array of `unsigned char` or `std::byte`
- `std::allocator<T>::allocate`
- `std::bit_cast` and `std::start_lifetime_as` (the library call *is* the creation)

This is what makes `std::vector` implementable: `allocator<T>::allocate(n)` implicitly creates an *array of `T`*, and the individual elements are then created by construction. And it is why this works:

```cpp
auto* a = static_cast<Point*>(std::malloc(sizeof(Point)));   // a Point object now exists, if Point is implicit-lifetime
a->x = 1;                                                    // defined, since P0593
```

---

## 6. Implementation model: the Itanium layout algorithm

On Linux (and every non-Windows platform) GCC and Clang both follow the **Itanium C++ ABI**. Its layout rules, simplified:

```text
for each class:
   1. start with the vptr (if dynamic and no base has one): 8 bytes at offset 0
   2. lay out non-virtual base subobjects, in declaration order
   3. for each non-static data member, in declaration order:
          offset = next multiple of alignof(member) at or after the current end
   4. size  = current end, rounded up to the class's alignment
      align = max alignment of any member / base / vptr
```

Two numbers that the algorithm tracks and that `sizeof` hides:

| Name | Meaning |
|---|---|
| **`sizeof`** | Total size, including tail padding |
| **`dsize`** ("data size") | Where the last member ends, *before* tail padding |
| **`nvsize`** | Size of the class excluding virtual bases |

### The tail-padding rule

> **If a class is not "POD for the purposes of layout", a derived class may place its own members into the base's tail padding.**

Here "POD for layout" is the C++03 notion (roughly: no user-provided constructors, destructors, or assignment, no private members, no base classes, no virtuals). The reasoning is about `memcpy`: a POD base might be copied by `memcpy(&dst, &src, sizeof(Base))`, which would also overwrite the derived members stored in its padding. For a non-POD base, the ABI authors knew `memcpy` of the base subobject is not valid, and reclaimed the bytes.

You will see this in Experiment 2. It is also *why* the standard says `memcpy` into a **potentially-overlapping subobject** is undefined.

### Layer check

| Layer | Who decides what |
|---|---|
| **Standard** | Declaration order, `sizeof ≥ 1`, standard-layout first-member rule, alignment constraints |
| **Compiler** | Follows the ABI; may warn about padding (`-Wpadded`) |
| **ABI (Itanium + System V AMD64)** | Offsets, padding, `dsize` reuse, vptr placement, empty-base handling, bit-field packing |
| **OS** | Provides alignment of allocations (`malloc` returns 16-byte aligned on x86-64 glibc) |
| **CPU** | Penalizes misalignment (cache-line splits); faults on aligned SIMD instructions |

---

## 7. Experiments

Run each of these. Predict the output first.

### Experiment 1 — Size, alignment, offsets

```cpp
// @test run -std=c++23
#include <cstddef>
#include <cstdio>

struct A { char c; int i; char d; };     // padding around i, and at the end
struct B { int i; char c; char d; };     // same members, better order
struct C { char c; double d; char e; };
struct alignas(32) D { char c; };        // over-aligned type
struct E { char c[3]; short s; };

#define LAYOUT(T) std::printf("%-2s size=%2zu align=%2zu\n", #T, sizeof(T), alignof(T))

int main() {
    LAYOUT(A); LAYOUT(B); LAYOUT(C); LAYOUT(D); LAYOUT(E);
    std::printf("A offsets: c=%zu i=%zu d=%zu\n", offsetof(A, c), offsetof(A, i), offsetof(A, d));
    std::printf("C offsets: c=%zu d=%zu e=%zu\n", offsetof(C, c), offsetof(C, d), offsetof(C, e));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
A  size=12 align= 4
B  size= 8 align= 4
C  size=24 align= 8
D  size=32 align=32
E  size= 6 align= 2
A offsets: c=0 i=4 d=8
C offsets: c=0 d=8 e=16
```

Work out `A` by hand using the algorithm in §6: `c` at 0; `i` needs 4-byte alignment so goes at 4 (3 bytes of padding); `d` at 8; end = 9; round up to alignment 4 → **12**. Reordering to `B` (largest first) gives 8. That is a 33 % reduction for free, and the first optimization to try when a struct is copied millions of times (see [Chapter 27](../part-10-memory/27-cache-and-data-oriented-cpp.md)).

> [!TIP]
> Ask the compiler to point out wasted bytes: `g++ -Wpadded` (noisy but informative), or `clang++ -Xclang -fdump-record-layouts -fsyntax-only` for the full picture.

### Experiment 2 — Tail-padding reuse

```cpp
// @test run -std=c++23
#include <cstdio>

struct PodBase    { int i; char c; };                // aggregate: POD for layout
struct NonPodBase { int i; char c; NonPodBase() {} }; // user-provided constructor: not POD for layout

struct D1 : PodBase    { char d; };
struct D2 : NonPodBase { char d; };

int main() {
    std::printf("sizeof  PodBase=%zu  NonPodBase=%zu\n", sizeof(PodBase), sizeof(NonPodBase));
    std::printf("sizeof  D1=%zu  D2=%zu\n", sizeof(D1), sizeof(D2));

    D1 a; D2 b;
    std::printf("offset of D1::d = %td\n", reinterpret_cast<char*>(&a.d) - reinterpret_cast<char*>(&a));
    std::printf("offset of D2::d = %td\n", reinterpret_cast<char*>(&b.d) - reinterpret_cast<char*>(&b));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sizeof  PodBase=8  NonPodBase=8
sizeof  D1=12  D2=8
offset of D1::d = 8
offset of D2::d = 5
```

`PodBase` and `NonPodBase` have identical members and identical size (8). Yet `D2` fits `d` into the base's tail padding (offset 5) while `D1` places it after the whole base (offset 8). The compiler's own view, from Clang's layout dump:

```text
$ clang++-18 -Xclang -fdump-record-layouts -fsyntax-only layouts.cpp
         0 | struct PodBase
         0 |   int i
         4 |   char c
           | [sizeof=8, dsize=8, align=4,
           |  nvsize=8, nvalign=4]
         0 | struct D1
         0 |   struct PodBase (base)
         0 |     int i
         4 |     char c
         8 |   char d
           | [sizeof=12, dsize=9, align=4,
           |  nvsize=9, nvalign=4]
         0 | struct NonPodBase
         0 |   int i
         4 |   char c
           | [sizeof=8, dsize=5, align=4,
           |  nvsize=5, nvalign=4]
         0 | struct D2
         0 |   struct NonPodBase (base)
         0 |     int i
         4 |     char c
         5 |   char d
           | [sizeof=8, dsize=6, align=4,
           |  nvsize=6, nvalign=4]
```

Look at `dsize`: `PodBase` has `dsize=8` (no reusable tail), `NonPodBase` has `dsize=5` (three bytes free). GCC produces the same layout. **Standard vs ABI:** the standard says nothing about this; it is a pure ABI decision, and one with a real consequence: it is the reason a base-class subobject is *potentially overlapping*, and the reason you must never `memcpy` one.

### Experiment 3 — Identity: when two objects share an address

```cpp
// @test run -std=c++23
#include <cstdio>

struct Empty {};
struct TwoEmpty { Empty a; Empty b; };                          // two distinct objects: distinct addresses
struct OneEmpty { Empty a; int x; };                            // member must occupy a byte, padded to int
struct EBO : Empty { int x; };                                  // empty BASE subobject: may overlap
struct NUA { [[no_unique_address]] Empty a; int x; };           // C++20: same effect for a member
struct NUA2 { [[no_unique_address]] Empty a; [[no_unique_address]] Empty b; int x; };  // same type twice

int main() {
    std::printf("Empty=%zu  TwoEmpty=%zu  OneEmpty=%zu\n", sizeof(Empty), sizeof(TwoEmpty), sizeof(OneEmpty));
    std::printf("EBO=%zu  NUA=%zu  NUA2=%zu\n", sizeof(EBO), sizeof(NUA), sizeof(NUA2));

    NUA n;
    std::printf("empty member and int member share an address? %s\n",
                static_cast<void*>(&n.a) == static_cast<void*>(&n.x) ? "yes" : "no");

    NUA2 m;
    auto off = [&](const void* member) {
        return reinterpret_cast<const char*>(member) - reinterpret_cast<const char*>(&m);
    };
    std::printf("NUA2 offsets: a=%td b=%td x=%td\n", off(&m.a), off(&m.b), off(&m.x));

    Empty e1, e2;
    std::printf("two Empty locals: distinct addresses? %s\n", &e1 != &e2 ? "yes" : "no");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
Empty=1  TwoEmpty=2  OneEmpty=8
EBO=4  NUA=4  NUA2=4
empty member and int member share an address? yes
NUA2 offsets: a=0 b=1 x=0
two Empty locals: distinct addresses? yes
```

`Empty` is size 1 because a complete object must have a unique address. As a *base* or a `[[no_unique_address]]` member it takes no room. `NUA2` shows the identity rule at work: `a` and `b` have the *same* type, so they may not share an address, and the compiler puts `b` one byte in. That byte lies *inside* `x`'s four bytes, which is legal because an empty object occupies no storage of its own, so the struct does not grow. Note how this makes `std::unique_ptr<T, Deleter>` the same size as `T*` when `Deleter` is empty: that is the empty-member trick from [Chapter 1](../part-01-mental-model/01-modern-cpp-philosophy.md), now with an explanation.

### Experiment 4 — The trait zoo

```cpp
// @test run -std=c++23
#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

struct Agg         { int a; double b; };
struct WithCtor    { int a; WithCtor(int x) : a(x) {} };
struct WithDtor    { int a; ~WithDtor() {} };
struct Virt        { virtual void f() {} int a; };
struct NoCopy      { int a; NoCopy() = default; NoCopy(const NoCopy&) = delete; };
struct MixedAccess { public: int a; private: int b; };
struct Base1 { int x; };
struct Der1 : Base1 { int y; };

template <class T>
void row(const char* name) {
    auto yn = [](bool b) { return b ? 'Y' : '.'; };
    std::printf("%-22s  %c    %c    %c    %c    %c\n", name,
                yn(std::is_trivially_copyable_v<T>),
                yn(std::is_trivially_default_constructible_v<T>),
                yn(std::is_standard_layout_v<T>),
                yn(std::is_aggregate_v<T>),
                yn(std::is_trivially_destructible_v<T>));
}

int main() {
    std::printf("%-22s  tcopy tdflt stdlay aggr tdtor\n", "");
    row<int>("int");
    row<Agg>("Agg");
    row<WithCtor>("WithCtor(int)");
    row<WithDtor>("WithDtor");
    row<Virt>("Virt");
    row<NoCopy>("NoCopy (deleted copy)");
    row<MixedAccess>("MixedAccess");
    row<Der1>("Der1 : Base1");
    row<std::array<int, 3>>("std::array<int,3>");
    row<std::pair<int, int>>("std::pair<int,int>");
    row<std::string>("std::string");
    row<std::vector<int>>("std::vector<int>");
    row<std::unique_ptr<int>>("std::unique_ptr<int>");
    row<std::optional<int>>("std::optional<int>");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
                        tcopy tdflt stdlay aggr tdtor
int                     Y    Y    Y    .    Y
Agg                     Y    Y    Y    Y    Y
WithCtor(int)           Y    .    Y    .    Y
WithDtor                .    .    Y    Y    .
Virt                    .    .    .    .    Y
NoCopy (deleted copy)   Y    Y    Y    .    Y
MixedAccess             Y    Y    .    .    Y
Der1 : Base1            Y    Y    .    Y    Y
std::array<int,3>       Y    Y    Y    Y    Y
std::pair<int,int>      .    .    Y    .    Y
std::string             .    .    Y    .    .
std::vector<int>        .    .    Y    .    .
std::unique_ptr<int>    .    .    Y    .    .
std::optional<int>      Y    .    Y    .    Y
```

Read it as a set of surprises worth understanding:

- **`WithCtor`** is trivially copyable (you can `memcpy` it) but *not* trivially default-constructible.
- **`WithDtor`**, with a mere empty destructor, loses trivial copyability. Giving a class a destructor changes what you may do with its bytes. This is why a "harmless" `~Foo() {}` is an anti-pattern.
- **`Virt`** is not standard-layout (it has a vptr) but *is* trivially destructible.
- **`MixedAccess`** is trivially copyable but not standard-layout (different access levels).
- **`Der1`** is an aggregate (C++17 allows bases) but not standard-layout (data in both base and derived).
- **`std::pair<int,int>`** is *not* trivially copyable in libstdc++, because its assignment operators are user-provided. A `pair` of ints looks like two ints but cannot be `memcpy`'d legally. **This is an implementation property** (the standard does not require it either way).
- **`std::optional<int>`** *is* trivially copyable: the library works to propagate triviality. Expect that of a well-made library; check it with `static_assert`.

> [!NOTE]
> `std::is_implicit_lifetime<T>` (C++23) asks the last question directly. It was not yet shipped in libstdc++ 14, so this course derives it from the traits above. Check your library's version.

### Experiment 5 — Padding is not part of the value

```cpp
// @test run -std=c++23 -O0
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <type_traits>

struct S { char c; int i; };   // 3 bytes of padding after c

int main() {
    std::printf("has_unique_object_representations<S>   = %d\n", std::has_unique_object_representations_v<S>);
    std::printf("has_unique_object_representations<int> = %d\n", std::has_unique_object_representations_v<int>);

    alignas(S) unsigned char b1[sizeof(S)];
    alignas(S) unsigned char b2[sizeof(S)];
    std::memset(b1, 0x00, sizeof b1);       // pre-fill with different garbage
    std::memset(b2, 0xFF, sizeof b2);

    S* s1 = ::new (static_cast<void*>(b1)) S{'x', 7};
    S* s2 = ::new (static_cast<void*>(b2)) S{'x', 7};

    std::printf("members equal:   %s\n", (s1->c == s2->c && s1->i == s2->i) ? "yes" : "no");
    std::printf("bytes equal:     %s\n", std::memcmp(s1, s2, sizeof(S)) == 0 ? "yes" : "no");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
has_unique_object_representations<S>   = 0
has_unique_object_representations<int> = 1
members equal:   yes
bytes equal:     no
```

The two objects have identical *values* and different *bytes*. Writing members does not touch padding, so whatever was in the storage before survives, and `memcmp` reports a difference. (Compiled at `-O0` so the result does not depend on how an optimizer chooses to copy the struct; at `-O2` a compiler may copy all 8 bytes at once, and then padding may or may not be written. That unpredictability *is* the lesson.)

### Experiment 6 — From `void*` to `T`: the whole lifecycle

This is the experiment the chapter title promises. Follow the *events*:

```cpp
// @test run -std=c++23
#include <cstddef>
#include <cstdio>
#include <memory>
#include <new>

struct Tracer {
    int id;
    explicit Tracer(int i) : id(i) { std::printf("    [ctor] id=%d\n", id); }
    ~Tracer()                       { std::printf("    [dtor] id=%d\n", id); }
};

int main() {
    // 1. STORAGE, but no object. Properly aligned, big enough, nothing constructed.
    alignas(Tracer) std::byte buf[sizeof(Tracer)];
    std::printf("1. storage obtained, no Tracer exists, no constructor ran\n");

    // 2. Create an object IN that storage. Lifetime begins when construction completes.
    Tracer* p = ::new (static_cast<void*>(buf)) Tracer(1);
    std::printf("2. object created; p points at the start of buf? %s\n",
                static_cast<void*>(p) == static_cast<void*>(buf) ? "yes" : "no");

    // 3. End the lifetime. Storage is still ours. Same bytes, no object.
    std::destroy_at(p);
    std::printf("3. object destroyed; the 'buf' storage still exists\n");

    // 4. Create a *different* object in the same storage.
    //    Same type, not const, not a base: p transparently refers to the new object.
    ::new (static_cast<void*>(buf)) Tracer(2);
    std::printf("4. new object, old pointer p still valid: p->id = %d\n", p->id);

    std::destroy_at(p);

    // 5. Automatic storage: the compiler does steps 1 + 2 for you, and step 3 at scope exit.
    {
        std::printf("5. a plain definition does it all:\n");
        Tracer t(3);
    }
    std::printf("6. end\n");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
1. storage obtained, no Tracer exists, no constructor ran
    [ctor] id=1
2. object created; p points at the start of buf? yes
    [dtor] id=1
3. object destroyed; the 'buf' storage still exists
    [ctor] id=2
4. new object, old pointer p still valid: p->id = 2
    [dtor] id=2
5. a plain definition does it all:
    [ctor] id=3
    [dtor] id=3
6. end
```

A local variable definition `Tracer t(3);` is **storage + construction**, and the closing brace is **destruction + storage release**. Everything `std::vector`, `std::optional`, `std::variant` and `std::function` do with raw storage is some arrangement of these same events, with the programmer, not the compiler, in charge of timing.

### Experiment 7 — Reading bytes: `memcpy` is not slow

You have a byte buffer from a socket and want the `uint32_t` at its start. Four ways, in increasing legitimacy:

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=load_
#include <cstddef>
#include <cstdint>
#include <cstring>

// 1. reinterpret_cast: tempting, and UB (strict aliasing + possible misalignment).
std::uint32_t load_cast(const std::byte* p) { return *reinterpret_cast<const std::uint32_t*>(p); }

// 2. memcpy into an object: defined, handles any alignment.
std::uint32_t load_memcpy(const std::byte* p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

// 3. memcpy plus a byte swap (network byte order). Still just a load and a bswap.
std::uint32_t load_be(const std::byte* p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap32(v);   // C++23 has std::byteswap
}
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
load_cast(std::byte const*):
	mov	eax, DWORD PTR [rdi]
	ret

load_memcpy(std::byte const*):
	mov	eax, DWORD PTR [rdi]
	ret

load_be(std::byte const*):
	mov	eax, DWORD PTR [rdi]
	bswap	eax
	ret
```

The defined version (`memcpy`) and the undefined one (`reinterpret_cast`) generate **the same single `mov`**. The compiler recognizes a fixed-size `memcpy` as a load. So you pay nothing for being correct. Prefer `memcpy` (or `std::bit_cast` for value-to-value reinterpretation, [Chapter 3](03-lifetime-and-storage.md)). Choosing the UB version because "it's faster" buys you nothing and puts the optimizer's strict-aliasing assumptions against you.

> [!WARNING]
> The generated code being the same *today* is not a defence of the UB version. Once inlined into a larger function, the cast version can be reordered relative to writes through other pointer types. The bug will be in a different function from the cast.

### Experiment 8 — Misaligned access is undefined even where the hardware allows it

```cpp
// @test crash -std=c++23 -O0 -fsanitize=alignment -fno-sanitize-recover=alignment err=misaligned
#include <cstdio>

int main() {
    alignas(int) char buf[16] = {};
    int* p = reinterpret_cast<int*>(buf + 1);   // 4-byte int at an odd address
    return *p;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
snippet.cpp:7:13: runtime error: load of misaligned address 0x7ffdf66133a1 for type 'int', which requires 4 byte alignment
0x7ffdf66133a1: note: pointer points here
 7f 00 00  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  00 72 6b 82 c4
              ^ 
```

x86 would happily perform this load, so the program "works" without the sanitizer. UBSan (`-fsanitize=alignment`) reports it. Make UBSan part of your test builds; it finds exactly the bugs that pass every test on your development machine.

---

## 8. Assembly / runtime investigation: object creation costs nothing

What *is* "creating an object" in machine code? For types with trivial construction, nothing at all:

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=make,unmake
#include <memory>
#include <new>

int* make(void* mem) { return new (mem) int(42); }   // placement new of an int

void unmake(int* p) { std::destroy_at(p); }          // destroying an int

struct W { W(); ~W(); };                             // opaque, user-provided
void unmake_w(W* p) { std::destroy_at(p); }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
make(void*):
	mov	DWORD PTR [rdi], 42
	mov	rax, rdi
	ret

unmake(int*):
	ret

unmake_w(W*):
	jmp	W::~W()@PLT
```

- `make` is a single store: `*mem = 42`. The "object creation" left **no instructions** beyond the initialization itself.
- `unmake(int*)` is `ret`. Destroying a trivially destructible object compiles to *nothing*, exactly as C code would.
- `unmake_w` is a jump to `W::~W()`. For a type with a real destructor, destruction is the call.

Lifetime is a **compile-time concept**. It tells the optimizer what it may assume. It does not cost cycles by existing.

### The still-missing piece: `std::start_lifetime_as`  🟡

`memcpy` copies bytes into a *new* object. Sometimes you want to treat bytes **already in a buffer** as an object without copying (a memory-mapped file, a DMA buffer, a ring buffer slot). C++23 adds the right tool:

```cpp
// @test skip needs libstdc++ 16 (std::start_lifetime_as); try it on Compiler Explorer with GCC trunk
#include <memory>

struct Header { unsigned magic; unsigned length; };   // implicit-lifetime type

const Header& parse(const std::byte* mapped) {
    return *std::start_lifetime_as<Header>(mapped);   // lifetime of a Header begins; bytes are preserved
}
```

Its contract: the storage must be suitably aligned and sized, `Header` must be an implicit-lifetime type, and the bytes must hold a valid representation. Until your library ships it, `memcpy` into a local is the portable answer, and as Experiment 7 showed, it costs nothing.

---

## 9. Implementation exercise: `Slot<T>`

You will build the primitive underneath `std::vector` elements, `std::optional`, and every small-buffer container: **raw storage that may or may not contain an object**.

Requirements for `Slot<T>`:

1. Owns `alignas(T) std::byte storage_[sizeof(T)]`.
2. `template <class... A> T& emplace(A&&...)`: destroys any current object, constructs a new one, returns a reference.
3. `void reset() noexcept`: destroys the object if there is one.
4. `T& get() noexcept`: returns the object. Use `std::launder(reinterpret_cast<T*>(storage_))`, or better, remember the pointer `new` returned.
5. `bool has_value() const noexcept`.
6. The destructor destroys any live object.
7. Copy and move are **deleted** for now (do them properly in Project 3).

Then pass this test program, which counts constructions and destructions:

```cpp
// @test skip exercise: write Slot<T> above, then compile this against it
#include <cassert>
#include <string>

struct Counted {
    static inline int live = 0;
    int v;
    explicit Counted(int x) : v(x) { ++live; }
    ~Counted() { --live; }
};

int main() {
    {
        Slot<Counted> s;
        assert(!s.has_value() && Counted::live == 0);   // storage exists, no object
        s.emplace(1);
        assert(s.has_value() && Counted::live == 1 && s.get().v == 1);
        s.emplace(2);                                   // old destroyed, new created
        assert(Counted::live == 1 && s.get().v == 2);
        s.reset();
        assert(Counted::live == 0);
        s.emplace(3);
    }                                                   // destructor must clean up
    assert(Counted::live == 0);

    Slot<std::string> str;
    str.emplace(100, 'x');                              // forwards constructor args
    assert(str.get().size() == 100);
}
```

Also add `static_assert(sizeof(Slot<int>) == 2 * sizeof(int))` or similar and explain the number you got.

<details>
<summary><strong>Reference solution</strong> (try first; tested code)</summary>

```cpp
// @test run -std=c++23 -fsanitize=address,undefined
#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <string>
#include <utility>

template <class T>
class Slot {
    alignas(T) std::byte storage_[sizeof(T)];
    bool engaged_ = false;

public:
    Slot() = default;
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    ~Slot() { reset(); }

    template <class... A>
    T& emplace(A&&... args) {
        reset();
        // If the constructor throws, engaged_ stays false: the strong guarantee for this operation.
        T* p = ::new (static_cast<void*>(storage_)) T(std::forward<A>(args)...);
        engaged_ = true;
        return *p;
    }

    void reset() noexcept {
        if (engaged_) {
            std::destroy_at(&get());
            engaged_ = false;
        }
    }

    // The object was created at storage_ by placement new, but we did not keep the returned
    // pointer; launder gives us one that is guaranteed to refer to it.
    T& get() noexcept { return *std::launder(reinterpret_cast<T*>(storage_)); }
    bool has_value() const noexcept { return engaged_; }
};

struct Counted {
    static inline int live = 0;
    int v;
    explicit Counted(int x) : v(x) { ++live; }
    ~Counted() { --live; }
};

int main() {
    {
        Slot<Counted> s;
        assert(!s.has_value() && Counted::live == 0);
        s.emplace(1);
        assert(s.has_value() && Counted::live == 1 && s.get().v == 1);
        s.emplace(2);
        assert(Counted::live == 1 && s.get().v == 2);
        s.reset();
        assert(Counted::live == 0);
        s.emplace(3);
    }
    assert(Counted::live == 0);

    Slot<std::string> str;
    str.emplace(100, 'x');
    assert(str.get().size() == 100);

    static_assert(sizeof(Slot<int>) == 8);   // 4 bytes of storage + 1 flag, padded to alignof(int)
}
```

The `static_assert` is the lesson: the `bool` costs **4 extra bytes** because of alignment. Real implementations (`std::optional`) have the same overhead for `optional<int>`. `std::optional<int>` is 8 bytes, as Chapter 1's ledger showed.

</details>

---

## 10. Real-world example: how `std::optional` and `std::vector` use the object model

### `std::optional<T>`: a union with manual lifetime

A `Slot` using a byte array needs `launder` or a stored pointer. Standard library implementations avoid the problem by using a **union**, which lets the compiler *see* the member and keeps everything `constexpr`-friendly:

```cpp
// @test run -std=c++23 -fsanitize=address,undefined
#include <cassert>
#include <memory>
#include <string>
#include <utility>

template <class T>
class MiniOptional {
    union Payload {
        char empty_;
        T value_;
        constexpr Payload() noexcept : empty_{} {}
        constexpr ~Payload() {}               // lifetime of value_ is managed by MiniOptional
    } p_;
    bool has_ = false;

public:
    constexpr MiniOptional() = default;
    MiniOptional(const MiniOptional&) = delete;   // omitted for brevity

    template <class... A>
    constexpr T& emplace(A&&... args) {
        reset();
        std::construct_at(&p_.value_, std::forward<A>(args)...);   // activates value_ (constexpr-legal)
        has_ = true;
        return p_.value_;
    }
    constexpr void reset() noexcept {
        if (has_) { std::destroy_at(&p_.value_); has_ = false; }
    }
    constexpr ~MiniOptional() { reset(); }

    constexpr bool has_value() const noexcept { return has_; }
    constexpr T& operator*() noexcept { return p_.value_; }
};

// Usable at compile time, which a byte-array + launder implementation could not be:
static_assert([] { MiniOptional<int> o; o.emplace(5); return *o; }() == 5);

int main() {
    MiniOptional<std::string> s;
    s.emplace("hello, object model");
    assert(*s == "hello, object model");
}
```

Changing the active member of a union **creates an object** (`[intro.object]`), so `construct_at(&p_.value_, …)` is exactly "start a lifetime here". libstdc++'s `_Optional_payload` is built on this idea.

### `std::vector<T>`: capacity is storage, size is objects

```text
          begin()                       end()                        begin()+capacity()
             │                            │                              │
             ▼                            ▼                              ▼
        ┌────┬────┬────┬────┬────┬────┬ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┐
        │ T  │ T  │ T  │ T  │ T  │ T  │   raw storage: NO objects live   │
        └────┴────┴────┴────┴────┴────┴ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┘
        \_______ size() objects _______/ \_ capacity() − size() bytes of storage _/
```

`reserve(1000)` allocates 1000 × `sizeof(T)` bytes and constructs **nothing**. `push_back` constructs one object at `end()`. `pop_back` destroys one and leaves the storage. `clear()` destroys all, keeps the storage. **Reallocation** allocates new storage, *move-constructs* objects into it, then *destroys* the old ones and frees the old storage. Everything in [Chapter 11](../part-05-standard-library/11-containers.md) and [Project 2](../part-19-projects/project-02-custom-vector.md) is this picture.

---

## 11. Failure modes

| Mistake | Why it is wrong | Fix |
|---|---|---|
| `memcmp(&a, &b, sizeof a)` to compare structs | Padding bytes are unspecified | Compare members; define `operator==`; use `std::has_unique_object_representations` before byte-wise hashing |
| `reinterpret_cast<T*>(buf)` then `*p` | `T` was never created at `buf`; strict aliasing; alignment | `new (buf) T`, `construct_at`, `start_lifetime_as`, or `memcpy` |
| `char buf[sizeof(T)]` as storage | Not aligned for `T` | `alignas(T) std::byte buf[sizeof(T)]` |
| `std::aligned_storage_t<N, A>` | Deprecated in C++23: poor interface, no guarantees about the "storage" being an array of bytes | `alignas(T) std::byte buf[N]` |
| `memcpy`/`memset` on a non-trivially-copyable object | Skips constructors/copy logic; breaks invariants (a `std::string` would share its heap buffer or SSO pointer) | Copy-construct; use `std::is_trivially_copyable_v` in a `static_assert` |
| `memcpy` onto a **base class subobject** | Potentially overlapping: may overwrite derived members stored in its tail padding (Experiment 2) | Use the copy assignment operator |
| Forgetting to destroy objects created with placement `new` | The destructor never runs: leaks, locks stay held | Pair every placement `new` with a `destroy_at` on every path, including exceptions |
| Destroying twice | Double destruction is UB (double free for an owning type) | Track engagement (`bool`, or a sentinel) |
| Using an old pointer after replacing a **`const` complete object** or a **base subobject** | Transparent replacement does not apply | Use the pointer `new` returned, or `std::launder` |
| Reading a union member other than the active one (type punning) | UB in C++ (unlike C). GCC and Clang document it as supported for `union` access, which is an *extension* | `std::bit_cast`, `memcpy` |
| `offsetof` on a non-standard-layout type | Only *conditionally supported*; GCC/Clang warn (`-Winvalid-offsetof`) | Redesign, or compute offsets from a live object |
| Assuming `sizeof(A) + sizeof(B) == sizeof(Derived)` | Tail-padding reuse, vptr, EBO | Never compute sizes by addition; ask `sizeof` |
| Assuming a layout across compilers or ABIs | MSVC's layout rules differ from Itanium's in several places | Do not serialize structs by `memcpy` across platforms or releases |

---

## 12. Exercises

1. **Reorder.** For each of `A`, `C`, `E` in Experiment 1, find the member order with the smallest `sizeof`. Is "descending alignment" always optimal? Find a counterexample or prove it.
2. **Predict, then check.** Without running, predict `sizeof` for:
   `struct F { char a; short b; char c; int d; char e; };`
   `struct G : F { char f; };` (is `F` POD-for-layout?)
   `struct H { F f; char g; };`
   Then run on both GCC and Clang.
3. **The trait audit.** Write a `static_assert`-based test that verifies, for a type you design, that it is trivially copyable, standard-layout, and *not* trivially default-constructible. Then add a member that flips each property in turn, and note which line the compiler flags.
4. **Padding hunter.** Write a function template `padding_bytes<T>()` that returns `sizeof(T)` minus the sum of the `sizeof`s of its members. (Without reflection, you will need to provide the member list manually or via structured bindings on aggregates. Use structured bindings with a fixed arity.) Apply it to a dozen structs.
5. **Safe parsing.** Parse a 12-byte network packet header (`uint16_t` type, `uint16_t` flags, `uint32_t` length, `uint32_t` id; big-endian on the wire) from a `std::span<const std::byte>` into a struct, without UB and without `reinterpret_cast`. Verify with `-fsanitize=undefined` and check that the assembly is a handful of loads and `bswap`s.
6. **Catch a lifetime bug.** Write a program that creates a `std::string` in a byte buffer with placement `new`, forgets to destroy it, and is clean under UBSan but flagged by ASan/LeakSanitizer. Then write one that uses the string *after* `destroy_at` and see which sanitizer notices (hint: it may not: why?).

---

## 13. Challenge: an Itanium layout calculator

Write a `constexpr` function template

```cpp
template <class... Ts>
constexpr auto layout();   // returns { std::array<size_t, sizeof...(Ts)> offsets; size_t size; size_t align; }
```

that computes, **from `sizeof(Ts)` and `alignof(Ts)` alone**, the offsets, size and alignment the Itanium algorithm assigns to `struct { Ts... }`.

Then:

1. `static_assert` it against real structs for at least ten type lists (generate the structs by hand; use `offsetof`).
2. Extend it to a **base class** with a non-POD base to model `dsize` (you will need to pass `dsize` of the base explicitly).
3. Explain why `std::tuple<char, int, short, char>` in libstdc++ gives member offsets `8, 4, 2, 0`, the *reverse* order of declaration, and what that implies about relying on `std::tuple` for binary layout. (Check this with `&std::get<I>(t)`.)

---

## 14. Knowledge check

1. What is the difference between **storage** and an **object**? Give an example where storage exists but no object does.
2. At what exact moment does an object's lifetime begin? When does it end for a class with a non-trivial destructor? For `int`?
3. Why can `struct { Empty a; Empty b; }` not be size 1, while `struct : Empty { int x; }` can have `sizeof == sizeof(int)`?
4. Which of these may you `memcpy` between: `std::string`, `std::pair<int,int>` (libstdc++), `std::array<int,3>`, a class with a user-provided destructor, `std::optional<int>`? For each answer, say whether the guarantee comes from the standard or merely from your library.
5. `struct P { int i; char c; };` and `struct Q { int i; char c; Q() {} };`. `struct DP : P { char d; };` and `struct DQ : Q { char d; };`. Compare `sizeof(DP)` and `sizeof(DQ)` on Linux. What rule explains it, and **who** owns that rule: the standard, the compiler, the ABI, or the CPU?
6. After `p->~T(); ::new (p) T(args);`, when is it safe to keep using the pointer `p`, and when must you use the pointer `new` returned or call `std::launder`?
7. Why is `memcmp` wrong for comparing two `struct { char c; int i; }` values, even when they have equal members?
8. `reinterpret_cast<const uint32_t*>(p)` and `memcpy(&v, p, 4)` compile to the same `mov`. Give two reasons to prefer `memcpy` anyway.

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. **Storage** is bytes with a size, alignment and storage duration. An **object** is a region of storage with a type, identity and lifetime. An `alignas(T) std::byte buf[sizeof(T)];` before any construction is storage with no `T` in it. So is the unused capacity of a `std::vector`.
2. It begins when properly aligned, sized storage is obtained **and initialization is complete** (constructor has finished). For a class with a non-trivial destructor, it ends when the destructor call **starts**; for `int` (or any type with a trivial destructor), when the storage is released or reused.
3. Two *distinct objects of the same type* must have distinct addresses, so `a` and `b` need different bytes. A zero-size *base subobject* of a different type than the `x` member may share an address (`[intro.object]`), so the empty base takes no room (EBO).
4. `std::array<int,3>`: yes. The standard requires `array` to be an aggregate wrapping a C array, so it is trivially copyable whenever its element type is. `std::optional<int>`: yes. The standard requires its copy/move operations and destructor to be trivial when `T`'s are. `std::string` and a class with a user-provided destructor: no, they are not trivially copyable (a bytewise copy of a `std::string` would produce two objects owning, or pointing into, the same buffer). `std::pair<int,int>`: **the standard does not say**. libstdc++ makes it *non*-trivially copyable (observable in Experiment 4), so don't `memcpy` it. That is a library property, not a language one.
5. `sizeof(DP)` is 12 and `sizeof(DQ)` is 8. The rule: **tail padding of a non-POD (for layout) base may be reused by the derived class**. It is part of the **Itanium C++ ABI**. The standard does not specify it, the compiler merely implements it, and the CPU does not care.
6. You may keep using `p` if the new object has the same type, exactly overlays the old storage, the old object was not a complete `const` object and neither object is a base class subobject or `[[no_unique_address]]` member (transparent replacement). Otherwise use the pointer returned by placement `new` / `construct_at`, or `std::launder(p)`.
7. The padding bytes after `c` are not part of the value; they are unspecified, may differ between otherwise-identical objects, and may change when the object is written. `memcmp` compares them anyway.
8. (a) The `reinterpret_cast` version is undefined behaviour (strict aliasing, alignment), so any change in surrounding code or compiler version may break it silently. (b) `memcpy` works for any alignment and is portable to platforms where unaligned loads fault. (c) It documents that you are *copying a value* rather than pretending an object exists.

</details>

---

[← Previous: Chapter 1](../part-01-mental-model/01-modern-cpp-philosophy.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 3 — Lifetime and storage →](03-lifetime-and-storage.md)
