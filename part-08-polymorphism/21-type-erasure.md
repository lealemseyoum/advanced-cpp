# Chapter 21 — Type Erasure

> **Part VIII · Polymorphism** &nbsp;|&nbsp; **Level 3–4** &nbsp;|&nbsp; **≈ 8 hours**
> **Prerequisites:** [Chapters 6–7](../part-03-value-categories-and-move/06-move-semantics.md), [Chapter 13](../part-05-standard-library/13-optional-variant-any-expected.md), [Chapters 19–20](19-runtime-polymorphism.md) &nbsp;|&nbsp; **Standards:** C++11 (`std::function`), C++17 (`std::any`), C++23 (`std::move_only_function`), C++26 (`std::function_ref`, `std::copyable_function`) &nbsp;|&nbsp; **Tools:** `g++-14`, `nm`, `objdump`

[← Previous: Chapter 20](20-static-polymorphism.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 22 — Exceptions →](../part-09-error-handling/22-exceptions.md)

---

**In one sentence:** type erasure gives you *value semantics with run-time polymorphism*: a concrete, ordinary class (`std::function`, `std::any`, your own `AnyShape`) that can hold *any* type with the right operations, without that type inheriting from anything.

**By the end of this chapter you can:**

- explain the pattern (external polymorphism: *concept* + *model* + *owner*) and derive it from first principles
- implement `Any` and `Function` with **small-buffer optimization** and a hand-built dispatch table
- predict, for `std::function`, which callables allocate and which do not (and measure it)
- state the ownership, copy, `const` and lifetime semantics of each erased type, including the non-owning `function_ref`
- decide between virtual, `variant`, templates and type erasure for a given interface

---

## 1. Problem

Chapters 19 and 20 left two gaps.

```text
   Virtual functions                    Templates
   ─────────────────                    ──────────
   + heterogeneous containers           − one type per container
   + open set of types                  − closed set at each call site
   − the type MUST inherit the base     + any type with the right operations (non-intrusive)
   − handled by pointer (heap, lifetime) + value semantics
```

Concretely, you want this to work:

```cpp
std::vector<Drawable> scene;          // values, not pointers; one container; mixed types
scene.push_back(Circle{1.0});         // Circle knows nothing about 'Drawable'
scene.push_back(std::string("hi"));   // neither does std::string (it only needs a draw() found somehow)
scene.push_back([](Canvas& c) { c.line(0, 0, 1, 1); });   // not even a class
for (auto& d : scene) draw(d, canvas);
```

and for callbacks, the same shape: `std::vector<std::function<void()>>` holding lambdas, function pointers and functors in one container, as values.

| You want | Virtual | Template | Type erasure |
|---|---|---|---|
| Values (copy/move/store) | pointer only | yes | **yes** |
| Mixed types in one container | yes | no | **yes** |
| Types need no common base | no | yes | **yes** |
| Non-intrusive (works on `int`, `std::string`, lambdas) | no | yes | **yes** |
| Hidden implementation, one compiled copy of client code | yes | no | **yes** |

---

## 2. Historical context

| Year | Event |
|---|---|
| 1990s | `void*` + function pointer pairs (qsort comparators, `pthread_create`, callback+`user_data` in C) |
| 1998 | Boost.Any (Kevlin Henney); the vocabulary of "holding anything" |
| 1999–2001 | **Boost.Function** (Doug Gregor) → `std::tr1::function`: a callable of a given *signature*, any callable |
| 2000 | Kevlin Henney, "Valued Conversions" and the term/idea of **external polymorphism** (Cleeland, Schmidt, Harrison, 1996) |
| 2011 | `std::function`, `std::shared_ptr`'s deleter erasure (the *type of the deleter is not part of the `shared_ptr` type*: type erasure in the smart-pointer control block), `std::thread`'s stored callable |
| 2013–15 | Sean Parent, *Inheritance Is the Base Class of Evil* / *Value Semantics and Concept-based Polymorphism*: the concept/model idiom as a general design |
| 2017 | `std::any` |
| 2020 | `std::function` limitations (copyable only, `const` `operator()` that can mutate) become a pain point; P0288 → |
| 2023 | **`std::move_only_function`** (C++23): move-only callables, proper `const`/`noexcept`/ref-qualifier support in the signature |
| 2025–26 | **`std::function_ref`** (non-owning callable view) and **`std::copyable_function`** adopted for C++26 (GCC 16 / libstdc++; not in GCC 14) |
| 2020s | Libraries: Boost.TypeErasure, `dyno` (Louis Dionne), `proxy` (Microsoft, **P3086 proposed for C++26 standardization**) |

---

## 3. Modern solution

```cpp
class Drawable {                                    // 1. an ordinary value class with a template constructor
public:
    template <class T> Drawable(T x) : self_(std::make_unique<Model<T>>(std::move(x))) {}
    friend void draw(const Drawable& d, Canvas& c) { d.self_->draw(c); }

private:
    struct Concept {                                // 2. the *erased* interface: abstract, hidden
        virtual ~Concept() = default;
        virtual void draw(Canvas&) const = 0;
        virtual std::unique_ptr<Concept> clone() const = 0;
    };
    template <class T> struct Model final : Concept {   // 3. the *adapter*: knows T, implements the interface using T's real operations
        T value;
        explicit Model(T v) : value(std::move(v)) {}
        void draw(Canvas& c) const override { draw_impl(value, c); }   // free function found by overloading/ADL
        std::unique_ptr<Concept> clone() const override { return std::make_unique<Model>(*this); }
    };
    std::unique_ptr<Concept> self_;                 // 4. the owner: type-agnostic pointer to the model
};
```

Three nested ideas: **template constructor** (captures the type), **virtual interface hidden inside** (erases it), **value wrapper** (owns it and gives value semantics). Everything else (SBO, manual vtables, non-owning variants) is an optimization of that skeleton.

---

## 4. Mental model

```text
   Drawable (what the client sees)          one non-template class
   ┌────────────────────────────┐
   │ owner:  ptr / buffer       │──┐
   └────────────────────────────┘  │
                                   ▼
   Model<Circle> : Concept     ┌─────────────┐        Model<Lambda> : Concept        …any number of Model<T>,
   ┌──────────────┐            │ vptr        │        ┌─────────────┐                  each created where the
   │ vptr  ───────────────────►│ Circle val  │        │ vptr        │                  Drawable is constructed
   │ Circle value │            └─────────────┘        │ lambda val  │                  (the one place T is known)
   └──────────────┘                                   └─────────────┘

   Client code is compiled ONCE against 'Drawable'.   Each Model<T> is compiled where T becomes a Drawable.
   Dispatch cost = one virtual call (or one manual function-pointer call) through the hidden interface.
```

**The erasure boundary.** Before the converting constructor, `T` is a full static type (template world: inlining, no indirection). After it, only the *operations you chose to expose* are left, and calling them is a dynamic dispatch (virtual world). The erased type is "forgotten" except as a pointer into its own `Model`.

### The three degrees of freedom

| Decision | Options | Consequence |
|---|---|---|
| **Storage** | heap (`unique_ptr`) · small inline buffer + heap fallback (**SBO**) · always inline fixed size · non-owning (pointer) | allocation, `sizeof`, copy cost, lifetime hazards |
| **Dispatch** | virtual `Concept` · manual table of function pointers | one extra allocation-free indirection; manual tables avoid the extra vptr and let you hand-tune layout |
| **Value semantics** | copyable (needs `clone`) · move-only · non-owning reference | requirements placed on T; `std::function` copyable, `move_only_function` not, `function_ref` neither owns nor copies a callable |

### The standard's erased types

| Type | Erases | Owns? | Copyable | Typical `sizeof` (libstdc++, 64-bit) |
|---|---|---|---|---|
| `std::function<R(A...)>` | "callable with this signature" | yes | yes (T must be copyable) | 32 |
| `std::move_only_function<R(A...)>` (C++23) | same | yes | no | 40 (measured, Experiment 7) |
| `std::function_ref<R(A...)>` (C++26) | same | **no** | yes (it's a view) | 2 pointers (16) |
| `std::any` (C++17) | "any copyable value" | yes | yes | 16 |
| `std::shared_ptr<void>` | "an owned object, with a type-erased deleter" | shared | yes | 16 |
| `std::unique_ptr<T, D>` | *not erased*: `D` is part of the type | yes | no | 8 + `sizeof(D)` |
| `std::shared_ptr<T>`'s deleter/allocator | **erased** inside the control block | shared | yes | (see Chapter 25) |

---

## 5. Language rules

### 5.1 What the language provides

Nothing special: type erasure is a **library pattern** built from templates (capture the type), virtual functions or function pointers (erase it), and value semantics (own it). The rules that matter are the ordinary ones from earlier chapters:

- A converting constructor `template <class T> X(T&&)` is a **forwarding constructor** (Chapter 7): it hijacks copy/move unless constrained. **Always** constrain it: `requires (!std::same_as<std::remove_cvref_t<T>, X>)`.
- `Model<T>` must be able to *copy*, *move* and *destroy* `T`: these are the capabilities your wrapper then exposes (copyable only if every `T` is).
- **Alignment**: the inline buffer must be aligned for `T` (`alignas(std::max_align_t)` or `alignof(T)` check).
- **Lifetime of inline objects**: construct with placement `new`; the buffer is raw storage and `T` has its own lifetime (Chapter 3); access through the pointer *returned* by placement `new` or `std::launder`ed pointers when the type of the buffer is `unsigned char[]`.
- **Exception safety**: moving an inline object between wrappers is only `noexcept` if `T`'s move is. libstdc++'s `std::function` sidesteps the issue differently: it stores a callable inline only if it is **trivially copyable** (`__is_location_invariant`, which is `is_trivially_copyable`), so relocating it is a plain byte copy that cannot throw. (Our own `Any`/`Function` below use `nothrow_move_constructible` instead, which admits more types.)

### 5.2 `std::function` precisely  `[func.wrap]`

- `function<R(A...)>` holds a copy of any callable `F` such that `invoke_r<R>(F&, A...)` is valid, and `F` is **Cpp17CopyConstructible**.
- `operator()` is `const` **but invokes the stored callable as a non-const lvalue** (a known defect/design wart): a `std::function` holding a mutable lambda can be called through a `const std::function&`, making it a loophole for hidden state and a data-race hazard.
- Calling an empty `function` throws `std::bad_function_call`.
- **The standard does not mandate small-buffer optimization**; it only *encourages* "avoid allocation for small callables" (and requires none for function pointers and `reference_wrapper`). What is stored inline is **implementation-defined**; Experiment 4 measures libstdc++'s rule.
- `target<T>()` and `target_type()` need RTTI.

### 5.3 `std::move_only_function`  `[func.wrap.move]` (C++23)

The signature carries `const`, ref-qualifiers and `noexcept`: `move_only_function<int(int) const noexcept>`. Its `operator()` invokes the callable with the **constness and value category specified in the signature**, which fixes `std::function`'s const loophole. Stores move-only callables (e.g. lambdas capturing a `unique_ptr`). GCC 12+ / libstdc++, Clang with libc++ 17+. `std::copyable_function` (C++26) is its copyable sibling and the intended eventual replacement of `std::function` in new code.

### 5.4 `std::function_ref`  (C++26, adopted; GCC 16)

A **non-owning** reference to a callable: two words (a pointer to the object or function plus a thunk pointer). No allocation, no copy of the callable, trivially copyable. The right parameter type for a function that *calls* a callback synchronously (`for_each`-like) and does not store it. **Lifetime rule: it must not outlive the callable** (binding a temporary lambda to a stored `function_ref` dangles; Experiment 6 builds one and shows the hazard).

### 5.5 `std::any`  `[any]` (C++17)

Holds a copyable value of any type; small types may be stored inline (implementation-defined; libstdc++: when nothrow-move-constructible and size/align fit in `void*`); `any_cast<T>` returns the value (or pointer) if the dynamic type is *exactly* `T`, otherwise throws `bad_any_cast`/returns null. Needs RTTI (`typeid`). Chapter 13 measured its small-buffer behaviour.

### Layer check

| Layer | Decides |
|---|---|
| **Standard** | Interfaces and semantics of `function`, `move_only_function`, `any`, `shared_ptr` deleters; the *requirements* on stored types; that SBO is permitted/encouraged but not mandated |
| **Compiler / library** | The SBO threshold and rule (libstdc++: stored locally if trivially copyable, `sizeof(F) ≤ 16` and `alignof` fits; libc++ and MSVC differ); `Model<T>` code size; whether the erased call is inlined (it usually cannot be, unless LTO/PGO devirtualizes it) |
| **ABI** | Layout of the wrapper (`sizeof(std::function)` = 32 on libstdc++/64-bit); the mangled names of `Model<T>`/manager functions; **`std::function`'s layout is ABI** and part of libstdc++'s stable ABI, so it cannot be improved without breaking compatibility |
| **OS / CPU** | A heap allocation when SBO misses (malloc cost, cache misses); an indirect call per invocation (prediction behaviour as in Chapter 19) |

---

## 6. Implementation model

### How libstdc++'s `std::function` is built (simplified)

```text
   std::function<int(int)>  (32 bytes)
   ┌──────────────────────────────────────────┐
   │ _M_functor   [16 bytes: union of          │  ← the callable stored inline, OR a pointer to a heap copy
   │               void*, function ptr, member ptr, aligned storage] │
   │ _M_manager   [8]  ─► one function that clone / destroy / get-type-info / get-pointer  (selected by an opcode)
   │ _M_invoker   [8]  ─► R (*)(const _Any_data&, A&&...)   the "call" function, instantiated per stored type
   └──────────────────────────────────────────┘
```

It is a **manual vtable with two function pointers** rather than a virtual base: `_M_invoker` is the call (kept as a direct member so the hot path is a single indirect call with no vptr load), `_M_manager` is a catch-all for the cold operations (copy, destroy, `typeid`). That design is what your own `Function` will imitate in Experiment 3.

### SBO: when is a callable stored inline?

libstdc++ (checked in `<bits/std_function.h>` of GCC 14) stores `F` locally if all hold: `is_trivially_copyable_v<F>` (the `__is_location_invariant` trait), `sizeof(F) ≤ 16`, and `alignof(F)` fits the storage alignment. A lambda capturing **two `long`s or pointers** fits; one capturing a `std::string` (32 bytes, non-trivial) does not, and neither does a *small but non-trivially-copyable* callable such as a `std::bind` result or a functor with a user-provided move constructor (Experiment 4).

### Cost model

```text
   construct (SBO hit):   copy/move F into buffer;  set two function pointers           ~ cost of moving F
   construct (SBO miss):  operator new + move F;                                         + a malloc/free pair, and a pointer chase on every call
   call:                  load invoker pointer, indirect call, (inside: load F's address, call F::operator())   ~ 1 indirect call; F usually not inlined into the caller
   copy:                  manager(clone):  copy F (+ allocate if heap)
   destroy:               manager(destroy)
```

---

## 7. Experiments

### Experiment 1: Concept/Model from scratch: a heterogeneous vector of values

```cpp
// @test run -std=c++23 -O0
#include <concepts>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// ---- Three unrelated types; none inherits anything. ----
struct Circle { double r; };
struct Square { double s; };
struct Label  { std::string text; };

// The "operation" is a free function found by overloading/ADL: this is how non-intrusive customization works.
void draw(const Circle& c, int indent) { std::printf("%*scircle r=%.1f\n", indent, "", c.r); }
void draw(const Square& s, int indent) { std::printf("%*ssquare s=%.1f\n", indent, "", s.s); }
void draw(const Label& l,  int indent) { std::printf("%*slabel \"%s\"\n", indent, "", l.text.c_str()); }
void draw(int n,           int indent) { std::printf("%*sint %d (an int is drawable too!)\n", indent, "", n); }

class Drawable {
    struct Concept {
        virtual ~Concept() = default;
        virtual void draw(int indent) const = 0;
        virtual std::unique_ptr<Concept> clone() const = 0;
    };
    template <class T> struct Model final : Concept {
        T value;
        explicit Model(T v) : value(std::move(v)) {}
        void draw(int indent) const override { ::draw(value, indent); }        // static dispatch on T happens HERE
        std::unique_ptr<Concept> clone() const override { return std::make_unique<Model>(*this); }
    };
    std::unique_ptr<Concept> self_;

public:
    // Constrained so that Drawable's own copy/move constructors are not hijacked (Chapter 7).
    template <class T> requires (!std::same_as<std::remove_cvref_t<T>, Drawable>)
    Drawable(T&& x) : self_(std::make_unique<Model<std::remove_cvref_t<T>>>(std::forward<T>(x))) {}

    Drawable(const Drawable& o) : self_(o.self_->clone()) {}                    // value semantics: deep copy
    Drawable(Drawable&&) noexcept = default;
    Drawable& operator=(Drawable o) noexcept { self_ = std::move(o.self_); return *this; }

    friend void draw(const Drawable& d, int indent) { d.self_->draw(indent); }
};

int main() {
    std::vector<Drawable> scene;
    scene.push_back(Circle{1.5});
    scene.push_back(Square{2.0});
    scene.push_back(Label{"hello"});
    scene.push_back(42);

    std::puts("scene:");
    for (auto& d : scene) draw(d, 2);

    auto copy = scene;                 // deep copy of every element, by value
    scene.clear();
    std::puts("copy survives clearing the original:");
    for (auto& d : copy) draw(d, 2);

    std::printf("sizeof(Drawable) = %zu (one pointer)\n", sizeof(Drawable));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
scene:
  circle r=1.5
  square s=2.0
  label "hello"
  int 42 (an int is drawable too!)
copy survives clearing the original:
  circle r=1.5
  square s=2.0
  label "hello"
  int 42 (an int is drawable too!)
sizeof(Drawable) = 8 (one pointer)
```

Notice what **Circle, Square, Label, int** do *not* have: a base class, a virtual function, an adapter written by their authors. The only requirement is that `draw(T, int)` is findable. This is the **non-intrusive** property that virtual functions cannot give you: you can make `int` or `std::string` or a type from a third-party library polymorphic without touching it. The cost sits in `Drawable`: one heap allocation per element and one virtual call per `draw`.

### Experiment 2: Implement `Any` with small-buffer optimization

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstddef>
#include <new>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>

// ---- allocation counting for the whole program ----
static int g_allocs = 0;
void* operator new(std::size_t n) { ++g_allocs; if (void* p = std::malloc(n)) return p; throw std::bad_alloc{}; }
void  operator delete(void* p) noexcept { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept { std::free(p); }

class Any {
    static constexpr std::size_t BufSize = 3 * sizeof(void*);              // 24 bytes of inline storage

    // The manual "vtable": one per stored type T, a constexpr object with static storage.
    struct VTable {
        const std::type_info& (*type)() noexcept;
        void (*destroy)(void* storage) noexcept;
        void (*copy)(const void* src, void* dst_storage);                   // may allocate or throw
        void (*move)(void* src, void* dst_storage) noexcept;
        bool is_inline;
    };

    template <class T>
    static constexpr bool fits = sizeof(T) <= BufSize && alignof(T) <= alignof(std::max_align_t) && std::is_nothrow_move_constructible_v<T>;

    template <class T> static T* ptr(void* s) noexcept { if constexpr (fits<T>) return std::launder(static_cast<T*>(s)); else return *static_cast<T**>(s); }
    template <class T> static const T* ptr(const void* s) noexcept { if constexpr (fits<T>) return std::launder(static_cast<const T*>(s)); else return *static_cast<T* const*>(s); }

    template <class T> static constexpr VTable table = {
        [](  ) noexcept -> const std::type_info& { return typeid(T); },
        [](void* s) noexcept { if constexpr (fits<T>) ptr<T>(s)->~T(); else delete ptr<T>(s); },
        [](const void* src, void* dst) { if constexpr (fits<T>) ::new (dst) T(*ptr<T>(src)); else *static_cast<T**>(dst) = new T(*ptr<T>(src)); },
        [](void* src, void* dst) noexcept { if constexpr (fits<T>) { ::new (dst) T(std::move(*ptr<T>(src))); ptr<T>(src)->~T(); } else *static_cast<T**>(dst) = ptr<T>(src); },
        fits<T>
    };

    alignas(std::max_align_t) unsigned char storage_[BufSize];
    const VTable* vt_ = nullptr;

public:
    Any() = default;
    template <class T> requires (!std::is_same_v<std::remove_cvref_t<T>, Any>)
    Any(T&& v) {
        using D = std::remove_cvref_t<T>;
        if constexpr (fits<D>) ::new (storage_) D(std::forward<T>(v));
        else *reinterpret_cast<D**>(storage_) = new D(std::forward<T>(v));
        vt_ = &table<D>;
    }
    Any(const Any& o) : vt_(o.vt_) { if (vt_) vt_->copy(o.storage_, storage_); }
    Any(Any&& o) noexcept : vt_(o.vt_) { if (vt_) { vt_->move(o.storage_, storage_); o.vt_ = nullptr; } }
    Any& operator=(Any o) noexcept { reset(); vt_ = o.vt_; if (vt_) { vt_->move(o.storage_, storage_); o.vt_ = nullptr; } return *this; }
    ~Any() { reset(); }

    void reset() noexcept { if (vt_) { vt_->destroy(storage_); vt_ = nullptr; } }
    bool has_value() const noexcept { return vt_ != nullptr; }
    const std::type_info& type() const noexcept { return vt_ ? vt_->type() : typeid(void); }
    bool stored_inline() const noexcept { return vt_ && vt_->is_inline; }

    template <class T> T* cast() noexcept { return vt_ && vt_->type() == typeid(T) ? ptr<T>(storage_) : nullptr; }
    template <class T> const T* cast() const noexcept { return vt_ && vt_->type() == typeid(T) ? ptr<T>(storage_) : nullptr; }
};

struct Big { char data[100]; int tag; };

int main() {
    std::printf("sizeof(Any) = %zu\n", sizeof(Any));

    int before = g_allocs;
    Any a = 42;
    Any b = 3.14;
    std::printf("int/double stored inline: %d, %d;  allocations: %d\n", a.stored_inline(), b.stored_inline(), g_allocs - before);

    before = g_allocs;
    Any c = std::string("this string is long enough to exceed the std::string SSO buffer");
    std::printf("std::string (32 bytes) inline? %d;  allocations: %d (string heap buffer + our box)\n", c.stored_inline(), g_allocs - before);

    before = g_allocs;
    Any d = Big{{}, 7};
    std::printf("Big (104 bytes) inline? %d;  allocations: %d\n", d.stored_inline(), g_allocs - before);

    std::printf("a holds int: %d, cast<int>=%d, cast<double>=%p (null)\n", a.type() == typeid(int), *a.cast<int>(), (void*)a.cast<double>());

    Any a2 = a;                     // copy: independent object
    *a2.cast<int>() = 99;
    std::printf("copy is independent: a=%d a2=%d\n", *a.cast<int>(), *a2.cast<int>());

    Any e = std::move(d);           // move of a heap-held value: pointer steal, no new allocation
    std::printf("after moving Big: d.has_value=%d e.tag=%d\n", d.has_value(), e.cast<Big>()->tag);

    Any f = c;                      // copy of the string: allocates again
    std::printf("copy of string holds same text: %d\n", *f.cast<std::string>() == *c.cast<std::string>());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sizeof(Any) = 32
int/double stored inline: 1, 1;  allocations: 0
std::string (32 bytes) inline? 0;  allocations: 2 (string heap buffer + our box)
Big (104 bytes) inline? 0;  allocations: 1
a holds int: 1, cast<int>=42, cast<double>=(nil) (null)
copy is independent: a=42 a2=99
after moving Big: d.has_value=0 e.tag=7
copy of string holds same text: 1
```

What each piece teaches:

- **The `VTable` is a `constexpr` object per `T`** (`table<T>` is a variable template): the wrapper stores *one pointer* to it. This is exactly the structure the compiler builds for virtual functions (Chapter 19), written by hand so you can choose its contents: here `destroy`/`copy`/`move`/`type` plus an `is_inline` flag.
- `fits<T>` is the **SBO decision**. The two storage representations (the object in the buffer, or a `T*` in the buffer) are why every operation is a `if constexpr (fits<T>)` pair.
- `std::launder`: the buffer is `unsigned char[]`; after `placement new` into it, accesses must go through a pointer obtained from `new` or laundered (Chapter 3). Forgetting this is technically UB even though it "works".
- The **move constructor requires `nothrow`** because `vector<Any>` reallocation will use it (Chapter 6); that is also why `fits<T>` requires `nothrow_move_constructible`.

### Experiment 3: Implement `Function<R(Args...)>`: a move-only callable with SBO

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

static int g_allocs = 0;
void* operator new(std::size_t n) { ++g_allocs; if (void* p = std::malloc(n)) return p; throw std::bad_alloc{}; }
void  operator delete(void* p) noexcept { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept { std::free(p); }

template <class Sig> class Function;                           // primary: undefined

template <class R, class... Args>
class Function<R(Args...)> {
    static constexpr std::size_t BufSize = 3 * sizeof(void*);

    template <class F> static constexpr bool fits =
        sizeof(F) <= BufSize && alignof(F) <= alignof(std::max_align_t) && std::is_nothrow_move_constructible_v<F>;

    struct VTable {
        R    (*call)(void* storage, Args&&... args);
        void (*destroy)(void* storage) noexcept;
        void (*move)(void* src, void* dst) noexcept;            // move-construct into dst, destroy src
    };

    template <class F> static F& get(void* s) noexcept {
        if constexpr (fits<F>) return *std::launder(static_cast<F*>(s));
        else return **static_cast<F**>(s);
    }

    template <class F> static constexpr VTable table = {
        [](void* s, Args&&... a) -> R { return std::invoke(get<F>(s), std::forward<Args>(a)...); },
        [](void* s) noexcept { if constexpr (fits<F>) get<F>(s).~F(); else delete *static_cast<F**>(s); },
        [](void* src, void* dst) noexcept {
            if constexpr (fits<F>) { ::new (dst) F(std::move(get<F>(src))); get<F>(src).~F(); }
            else { *static_cast<F**>(dst) = *static_cast<F**>(src); }
        }
    };

    alignas(std::max_align_t) unsigned char storage_[BufSize];
    const VTable* vt_ = nullptr;

public:
    Function() = default;
    Function(std::nullptr_t) {}

    template <class F> requires (!std::is_same_v<std::remove_cvref_t<F>, Function> && std::is_invocable_r_v<R, std::decay_t<F>&, Args...>)
    Function(F&& f) {
        using D = std::decay_t<F>;
        if constexpr (fits<D>) ::new (storage_) D(std::forward<F>(f));
        else *reinterpret_cast<D**>(storage_) = new D(std::forward<F>(f));
        vt_ = &table<D>;
    }

    Function(Function&& o) noexcept : vt_(o.vt_) { if (vt_) { vt_->move(o.storage_, storage_); o.vt_ = nullptr; } }
    Function& operator=(Function&& o) noexcept {
        if (this != &o) { reset(); vt_ = o.vt_; if (vt_) { vt_->move(o.storage_, storage_); o.vt_ = nullptr; } }
        return *this;
    }
    Function(const Function&) = delete;
    ~Function() { reset(); }

    void reset() noexcept { if (vt_) { vt_->destroy(storage_); vt_ = nullptr; } }
    explicit operator bool() const noexcept { return vt_ != nullptr; }

    R operator()(Args... args) {                                // note: NOT const: honest about possibly mutating the callable
        if (!vt_) throw std::bad_function_call{};
        return vt_->call(storage_, std::forward<Args>(args)...);
    }
};

int plain(int x) { return x + 1; }
struct Functor { int k; int operator()(int x) const { return x * k; } };

int main() {
    std::printf("sizeof(Function<int(int)>) = %zu   sizeof(std::function<int(int)>) = %zu\n",
                sizeof(Function<int(int)>), sizeof(std::function<int(int)>));

    int before = g_allocs;
    Function<int(int)> f1 = plain;                          // function pointer
    Function<int(int)> f2 = Functor{3};                     // small functor
    int a = 10, b = 20;
    Function<int(int)> f3 = [a, b](int x) { return x + a + b; };        // 8 bytes of captures
    std::printf("small callables: results %d %d %d;  allocations: %d\n", f1(1), f2(2), f3(3), g_allocs - before);

    before = g_allocs;
    long big[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    Function<long(int)> f4 = [big](int i) { return big[i]; };             // 64 bytes of captures
    std::printf("64-byte capture: result %ld;  allocations: %d\n", f4(5), g_allocs - before);

    before = g_allocs;
    auto up = std::make_unique<int>(41);
    Function<int()> f5 = [p = std::move(up)]() { return *p + 1; };          // MOVE-ONLY capture: std::function can't hold this
    std::printf("move-only lambda: result %d;  allocations beyond make_unique: %d\n", f5(), g_allocs - before - 1);

    Function<int(int)> moved = std::move(f2);
    std::printf("moved-from is empty: %d; moved-to works: %d\n", !f2, moved(5));

    try { Function<void()> empty; empty(); } catch (const std::bad_function_call&) { std::puts("empty call throws bad_function_call"); }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sizeof(Function<int(int)>) = 32   sizeof(std::function<int(int)>) = 32
small callables: results 2 6 33;  allocations: 0
64-byte capture: result 6;  allocations: 1
move-only lambda: result 42;  allocations beyond make_unique: 0
moved-from is empty: 1; moved-to works: 15
empty call throws bad_function_call
```

The 24-byte buffer is a design choice: with `std::function`'s 16 bytes of inline storage (§6) a lambda with three captured pointers allocates; here it doesn't. You are *trading `sizeof` for allocation avoidance*, and you can choose the trade for your workload: that is the point of implementing it yourself.

### Experiment 4: Which callables does `std::function` allocate for?

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <new>
#include <string>

static int g_allocs = 0;
void* operator new(std::size_t n) { ++g_allocs; if (void* p = std::malloc(n)) return p; throw std::bad_alloc{}; }
void  operator delete(void* p) noexcept { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept { std::free(p); }

int plain(int x) { return x; }
struct Empty { int operator()(int x) const { return x; } };

template <class F> void probe(const char* what, F f) {
    int before = g_allocs;
    std::function<int(int)> fn = std::move(f);
    int made = g_allocs - before;
    std::printf("  %-44s sizeof(callable)=%-3zu allocations=%d\n", what, sizeof(F), made);
    (void)fn(0);
}

int main() {
    std::printf("sizeof(std::function<int(int)>) = %zu\n", sizeof(std::function<int(int)>));
    probe("function pointer", &plain);
    probe("empty functor", Empty{});
    probe("captureless lambda", [](int x) { return x; });
    long a = 1, b = 2, c = 3;
    probe("lambda capturing 1 long (8 B)", [a](int x) { return x + int(a); });
    probe("lambda capturing 2 longs (16 B)", [a, b](int x) { return x + int(a + b); });
    probe("lambda capturing 3 longs (24 B)", [a, b, c](int x) { return x + int(a + b + c); });
    std::string s = "a string long enough to avoid the small-string optimization buffer......";
    probe("lambda capturing a std::string (32 B)", [s](int x) { return x + int(s.size()); });
    probe("std::bind(plain, 1) result", std::bind(plain, 1));
    struct Throwing { Throwing() = default; Throwing(Throwing&&) noexcept(false) {} Throwing(const Throwing&) = default; int operator()(int x) const { return x; } };
    probe("tiny functor with throwing move ctor", Throwing{});
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sizeof(std::function<int(int)>) = 32
  function pointer                             sizeof(callable)=8   allocations=0
  empty functor                                sizeof(callable)=1   allocations=0
  captureless lambda                           sizeof(callable)=1   allocations=0
  lambda capturing 1 long (8 B)                sizeof(callable)=8   allocations=0
  lambda capturing 2 longs (16 B)              sizeof(callable)=16  allocations=0
  lambda capturing 3 longs (24 B)              sizeof(callable)=24  allocations=1
  lambda capturing a std::string (32 B)        sizeof(callable)=32  allocations=1
  std::bind(plain, 1) result                   sizeof(callable)=16  allocations=1
  tiny functor with throwing move ctor         sizeof(callable)=1   allocations=1
```

*Reading it:* the rule on libstdc++/GCC 14.2 is visible in the output: callables that are **trivially copyable and at most 16 bytes** are stored inline (zero allocations). Larger ones allocate (24-byte lambda), and so do **small callables that are not trivially copyable**: the `std::bind` result is only 16 bytes but allocates, and so does a 1-byte functor whose move constructor is user-provided. (I first assumed the criterion was "nothrow move"; the output and then the header `<bits/std_function.h>` showed it is trivial copyability.) This is an **implementation choice** (libc++ and MSVC use different thresholds), which is why you should never *rely* on `std::function` being allocation-free; measure on the platforms you ship, or use your own erased type with a known buffer size.

### Experiment 5: What an erased call costs (speed)

```cpp
// @test run -std=c++23 -O2
#include <chrono>
#include <cstdio>
#include <functional>
#include <vector>

struct Op { int k; int operator()(int x) const { return x * k + 1; } };

template <class F> [[gnu::noinline]] long run_template(F f, int n) { long s = 0; for (int i = 0; i < n; ++i) s += f(i); return s; }
[[gnu::noinline]] long run_function(const std::function<int(int)>& f, int n) { long s = 0; for (int i = 0; i < n; ++i) s += f(i); return s; }
[[gnu::noinline]] long run_fnptr(int (*f)(int), int n) { long s = 0; for (int i = 0; i < n; ++i) s += f(i); return s; }
[[gnu::noinline]] int fn_k3(int x) { return x * 3 + 1; }

template <class F> double best_ms(F&& f, long& sink) {
    double best = 1e30;
    for (int r = 0; r < 7; ++r) {
        auto t0 = std::chrono::steady_clock::now(); sink += f();
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    return best;
}

int main() {
    constexpr int N = 100'000'000;
    long sink = 0;
    Op op{3};
    std::function<int(int)> sf = op;
    std::printf("N = %d calls, best of 7 (-O2)\n", N);
    std::printf("  template functor (inlined)        %8.2f ms\n", best_ms([&] { return run_template(op, N); }, sink));
    std::printf("  function pointer (noinline loop)  %8.2f ms\n", best_ms([&] { return run_fnptr(fn_k3, N); }, sink));
    std::printf("  std::function (SBO, indirect)     %8.2f ms\n", best_ms([&] { return run_function(sf, N); }, sink));
    return sink == 1 ? 1 : 0;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
N = 100000000 calls, best of 7 (-O2)
  template functor (inlined)           70.16 ms
  function pointer (noinline loop)    163.60 ms
  std::function (SBO, indirect)       160.38 ms
```

Measured (the output above, 10⁸ calls): the inlined template functor takes **70 ms**, the function pointer and the `std::function` both take about **160 ms**, i.e. roughly **2.3× slower**, and the SBO-stored `std::function` is no slower than a plain function pointer: the cost is the *indirect call that cannot be inlined*, not the wrapper. (The template loop is cheap enough that GCC probably simplified it substantially; look at the assembly before quoting a ratio.) What matters is the *shape* and what it says about a design: erasing the type of a callback costs about one non-inlinable call per invocation, which is irrelevant for a button click and decisive for a comparator called a billion times in `sort`. **That is the real rule: erase at coarse granularity, not in the inner loop.**

### Experiment 6: A non-owning `FunctionRef` and how it dangles

C++26 standardizes `std::function_ref`; here is its essence in 25 lines, and the hazard that comes with it.

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <type_traits>
#include <utility>

template <class Sig> class FunctionRef;

template <class R, class... Args>
class FunctionRef<R(Args...)> {
    void* obj_ = nullptr;                                         // pointer to the callable (NOT a copy)
    R (*thunk_)(void*, Args...) = nullptr;                        // knows how to call it
public:
    template <class F> requires (!std::is_same_v<std::remove_cvref_t<F>, FunctionRef> && std::is_invocable_r_v<R, F&, Args...>)
    FunctionRef(F&& f) noexcept
        : obj_(const_cast<void*>(static_cast<const void*>(&f))),
          thunk_([](void* o, Args... a) -> R { return (*static_cast<std::remove_reference_t<F>*>(o))(std::forward<Args>(a)...); }) {}

    R operator()(Args... args) const { return thunk_(obj_, std::forward<Args>(args)...); }
};

// A function that CALLS a callback synchronously and does not keep it: the ideal use of a non-owning reference.
int apply_n(FunctionRef<int(int)> f, int n) { int s = 0; for (int i = 0; i < n; ++i) s += f(i); return s; }

struct Holder { FunctionRef<int(int)> f; };                          // STORES a reference: the dangerous use

Holder make_dangling() {
    int k = 10;
    auto lambda = [k](int x) { return x + k; };
    return Holder{lambda};                                           // 'lambda' dies at the end of this function; Holder::f now points to dead stack
}

int main() {
    int offset = 100;
    std::printf("apply_n with a capturing lambda (no allocation, no copy): %d\n", apply_n([&](int i) { return i + offset; }, 4));
    std::printf("sizeof(FunctionRef) = %zu (two pointers)\n", sizeof(FunctionRef<int(int)>));

    // Holder h = make_dangling();  h.f(1);       // UNDEFINED BEHAVIOR: the lambda object no longer exists
    std::puts("make_dangling() is left commented out: calling it is undefined behaviour (try it under -fsanitize=address)");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
apply_n with a capturing lambda (no allocation, no copy): 406
sizeof(FunctionRef) = 16 (two pointers)
make_dangling() is left commented out: calling it is undefined behaviour (try it under -fsanitize=address)
```

**Lifetime rule:** a `function_ref` parameter is safe because the callable outlives the call (the caller's temporary lives until the end of the full-expression); a `function_ref` *member or return value* is a dangling-reference bug waiting to happen. This is the same "non-owning abstractions create lifetime responsibilities" principle as `string_view` and `span` in Chapter 12.

### Experiment 7: Size and standard library erased types

```cpp
// @test run -std=c++23 -O0
#include <any>
#include <cstdio>
#include <functional>
#include <memory>

int main() {
    std::printf("std::function<void()>              %2zu bytes\n", sizeof(std::function<void()>));
    std::printf("std::move_only_function<void()>    %2zu bytes\n", sizeof(std::move_only_function<void()>));
    std::printf("std::any                           %2zu bytes\n", sizeof(std::any));
    std::printf("std::shared_ptr<int>               %2zu bytes (control block erases the deleter)\n", sizeof(std::shared_ptr<int>));
    std::printf("std::unique_ptr<int>               %2zu bytes (default deleter: part of the TYPE, zero size)\n", sizeof(std::unique_ptr<int>));
    auto del = [](int* p) { delete p; };
    std::printf("std::unique_ptr<int, lambda>       %2zu bytes (stateless lambda: EBO)\n", sizeof(std::unique_ptr<int, decltype(del)>));
    std::printf("std::unique_ptr<int, void(*)(int*)> %2zu bytes (function pointer: stored)\n", sizeof(std::unique_ptr<int, void (*)(int*)>));

    // shared_ptr erases the deleter, so these have the SAME TYPE despite different deleters:
    std::shared_ptr<int> a(new int(1));
    std::shared_ptr<int> b(new int(2), [](int* p) { std::puts("custom deleter ran"); delete p; });
    static_assert(std::is_same_v<decltype(a), decltype(b)>);
    std::puts("shared_ptr with different deleters have the same type: assignable to each other");
    a = b;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
std::function<void()>              32 bytes
std::move_only_function<void()>    40 bytes
std::any                           16 bytes
std::shared_ptr<int>               16 bytes (control block erases the deleter)
std::unique_ptr<int>                8 bytes (default deleter: part of the TYPE, zero size)
std::unique_ptr<int, lambda>        8 bytes (stateless lambda: EBO)
std::unique_ptr<int, void(*)(int*)> 16 bytes (function pointer: stored)
shared_ptr with different deleters have the same type: assignable to each other
custom deleter ran
```

`shared_ptr` versus `unique_ptr` is the cleanest illustration of *what erasure buys and costs*: the deleter in `shared_ptr` is erased into the control block (one type, one heap block, one indirect call on destruction), whereas in `unique_ptr<T, D>` the deleter is part of the type (no allocation, zero size when stateless, but different types for different deleters).

---

## 8. Assembly / runtime investigation

```bash
# (1) What does calling an erased function look like?  (std::function: load invoker, indirect call.)
cat > call.cpp <<'EOF'
#include <functional>
int call(const std::function<int(int)>& f, int x) { return f(x); }
EOF
g++-14 -std=c++23 -O2 -S -masm=intel -o - call.cpp | c++filt | awk '/^call\(/,/\.cfi_endproc/'
# expect: test of the manager pointer (empty check -> throw path), then  mov rax,[rdi+24]; jmp rax   (the invoker pointer at offset 24)

# (2) Where do the per-type thunks live?  one 'invoker' and one 'manager' per stored callable type:
g++-14 -std=c++23 -O0 -c prog.cpp -o prog.o && nm -C prog.o | grep -E "_M_invoke|_M_manager" | head

# (3) Did the allocation happen?  ltrace/heaptrack/valgrind, or count operator new as in Experiments 2-4:
valgrind --tool=none ./a.out   # or  heaptrack ./a.out
```

The invoker is at a fixed offset in the object, so the call is *one load and one indirect jump*: there is no vptr step. That is the concrete reason the library chose a manual two-pointer layout over a virtual base.

---

## 9. Implementation exercise

1. **Complete `Function`**: add a *copyable* version (a `clone` slot in the table), `target<T>()`, `swap`, comparison with `nullptr`, and `noexcept`/`const` signature support (`Function<int(int) const>`).
2. **Make `Any` heterogeneous-container friendly**: add `std::any_cast`-style free functions, `emplace<T>(args...)`, `make_any<T>`, and the in-place constructor `Any(std::in_place_type<T>, args...)`. Verify no extra copies with an instrumented type.
3. **Generic erased interface**: implement a mini `AnyIterator<T>` (input iterator over `T` hiding the container type) and use it to implement a non-template `void print_all(AnyRange<int>)`. Measure the per-element cost versus a template.
4. **A `Signal<R(Args...)>`**: a list of `Function`s with connect/disconnect handles, safe to disconnect during emission. Compare with Qt's signals (Chapter 47) on features: thread safety, auto-disconnection when the receiver dies, queued connections.
5. **`unique_function` with a custom allocator**: take an `std::pmr::memory_resource*` for the heap fallback (Chapter 26) so that the callbacks of one subsystem come from an arena.
6. **Compare four implementations of a "Shape" erased type**: (a) concept/model with `unique_ptr`; (b) with SBO of 32 bytes; (c) manual vtable (like `Function`); (d) `std::variant`. For small shapes and 10⁶ elements, report `sizeof`, allocation count, build time, and traversal time.

<details>
<summary><strong>Solution sketch for item 1 (copyable <code>Function</code> and <code>target&lt;T&gt;</code>)</strong></summary>

Extend the table with `copy` and `type`:

```cpp
// Added to VTable:
void (*copy)(const void* src, void* dst);        // copy-construct (may allocate; may throw)
const std::type_info& (*type)() noexcept;

// In table<F>:
[](const void* src, void* dst) {
    if constexpr (fits<F>) ::new (dst) F(*std::launder(static_cast<const F*>(src)));
    else *static_cast<F**>(dst) = new F(**static_cast<F* const*>(src));
},
[]() noexcept -> const std::type_info& { return typeid(F); },

// Function(const Function& o) : vt_(o.vt_) { if (vt_) vt_->copy(o.storage_, storage_); }

// target<T>(): the exact-type check you used in Any
// template <class T> T* target() noexcept { return vt_ && vt_->type() == typeid(T) ? &get<T>(storage_) : nullptr; }
```

and constrain the converting constructor with `std::is_copy_constructible_v<D>` for the copyable version. The two classes differ only in that constraint plus the `copy` slot: that is the whole difference between `std::function` (copy required) and `std::move_only_function` (no copy slot).

</details>

---

## 10. Real-world example

| Where | The erased type |
|---|---|
| **Callbacks, event loops, thread pools, futures** | `std::function`, `std::move_only_function` (thread pool tasks are move-only: `packaged_task` holds a promise) |
| **`std::shared_ptr`** | Erased deleter and allocator in the control block |
| **`std::thread`, `std::async`, `std::packaged_task`** | Erased callable and its arguments |
| **`std::format`** | `basic_format_arg` is a type-erased handle to an argument (`void*` + formatter function pointer), which is why `vformat` is a *non-template* function: one compiled copy, small code |
| **Ranges** | `std::generator`/`std::ranges::any_view` (proposed) to hide pipeline types across API boundaries (Chapter 14) |
| **Qt** | `QVariant` (erased value with a meta-type registry), `QMetaObject::Connection` (erased callable for functor connections), `QFuture`/`QPromise`, `QAnyStringView` (non-owning, erases the string encoding) (Chapter 47) |
| **LLVM** | `llvm::function_ref` (the origin of `std::function_ref`), `llvm::unique_function`, `PassConcept`/`PassModel<T>` (the concept/model idiom, literally named so) |
| **Abseil** | `absl::AnyInvocable` (the move-only function), `absl::FunctionRef` |
| **Folly** | `folly::Function`, `folly::poly` |
| **Microsoft `proxy` library / Boost.TypeErasure** | Generalized, facade-based erasure for arbitrary interfaces (P3086 proposal) |
| **Game/GUI toolkits, ECS** | Components held as erased values with a table of operations |

> **Opinion.** Type erasure is **the right default for an *owned, stored, heterogeneous* abstraction** in modern C++: it gives you value semantics, non-intrusive adaptation and a single compiled copy of client code. Use it at **coarse granularity**: callbacks, tasks, strategy objects, plug-in handles. Do **not** erase in an inner loop (use a template or `variant`). Use **`function_ref`** (or a template parameter) for callbacks that are only *called*; use **`move_only_function`** for callbacks that are *stored*, and reserve `std::function` for APIs that genuinely need copyability. And remember: `std::function` allocations, `std::any` RTTI dependence, and the extra indirect call are *all* costs you are choosing, not defects. For a dynamic interface with **many operations** (not one callable), write the concept/model class by hand or use a library (`proxy`, Boost.TypeErasure) rather than a hierarchy of virtual interfaces with `shared_ptr` everywhere.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Unconstrained converting constructor | The wrapper's copy/move constructor is bypassed; infinite recursion or wrong behaviour | `requires (!same_as<remove_cvref_t<T>, Wrapper>)` |
| Assuming `std::function` never allocates | Latency spikes in real-time code | Measure; own type with a known SBO; or `function_ref`/template |
| Mutable lambda inside `std::function` called through `const&` from several threads | Data race (the `const operator()` lie) | `move_only_function<R(A) const>` (enforces const call) or avoid mutable state |
| Copying an erased type that holds a large object | Hidden deep copy per push/return | Pass by reference/move; use `move_only_function`; shared ownership explicitly |
| Binding `function_ref` to a temporary and storing it | **Dangling**, UB | Use it only as a parameter; store owning types |
| `std::any_cast<T>` with the wrong exact type (e.g. `const char*` vs `std::string`) | `bad_any_cast` | Cast to the exact stored type; `type()` first |
| Erasing in the inner loop | About 2× or worse for tiny bodies (2.3× in Experiment 5), more when inlining would have enabled vectorization | Template/`variant`; erase at the boundary |
| `-fno-rtti` with `std::any` / `function::target` | Compile errors / disabled features | Keep RTTI, or use your own type-id (a static address per `T`) |
| Throwing or user-provided move constructor in the stored type | Wrapper cannot be `noexcept`-movable (vector reallocation copies); `std::function` refuses inline storage (it requires trivially copyable) | Make moves `noexcept`; prefer trivially copyable captures for hot callbacks |
| Aliasing/UB with raw buffers (no `launder`, wrong alignment) | Works on one compiler, breaks at `-O2` or on ARM | `alignas`, placement `new`, `std::launder` |
| Recursive erasure: a `Function` that stores a lambda capturing itself | Cycle / destructor recursion | Weak reference or explicit lifetime management |
| ABI: exporting `std::function`/`std::any` in a library interface compiled with different standard-library versions | Layout mismatch → crashes | Version-controlled interface, or a C function pointer + `void*` at the boundary (Chapter 45) |
| One `Concept` that grows to 40 virtual functions | Every model implements everything; compile time and code size balloon | Split by capability; use `proxy`-style facades; templates for the hot parts |

---

## 12. Exercises

1. **Anatomy.** Using `nm -C` and `objdump -d` on a small program using `std::function`, find the `_M_invoke` and `_M_manager` instantiations for each stored type and draw the dispatch for a call (as in §6).
2. **Break the SBO rule.** Add a test to Experiment 4 that finds the exact byte at which libstdc++ starts allocating (capture 1…4 `long`s, then misaligned/over-aligned types). Check Clang+libc++ (if available) and compare.
3. **`move_only_function` vs `function`**: write a thread-pool task queue with each; show the code that fails with `std::function` (a task capturing a `unique_ptr`), and measure the overhead difference.
4. **Concept/model without virtual**: re-implement Experiment 1 with a manual function-pointer table (a `struct Ops { void (*draw)(const void*, int); void (*destroy)(void*); void* (*clone)(const void*); }`) and no virtual functions. Compare `sizeof(Drawable)`, code size and call assembly.
5. **Customization point.** Make `Drawable`'s `draw` customization go through a CPO (Chapter 15) instead of an unqualified call, so that a missing `draw` is a *concept* failure at the point of construction. Show the diagnostics before and after.
6. **Two-interface erasure.** Extend `Drawable` so a model can optionally implement `serialize()` if `T` supports it (detected by a concept), and expose `Drawable::try_serialize()`. Discuss: how does this relate to `dynamic_cast` and to interface-segregation?
7. **Allocator-aware erased type** (Chapter 26): support `std::pmr` so that `Drawable`'s models are allocated from a monotonic buffer; measure scene-build time against `make_unique`.
8. **Trace.** Instrument constructors/destructors/copies of a payload type and trace `vector<Drawable>::push_back` growth: how many copies/moves per element? Is `Drawable`'s move `noexcept` (and why does that matter, Chapter 6)?

---

## 13. Challenge: a small `proxy`-style generic erasure library

Design `Erased<Facade>` where a *facade* declares a set of operations as function signatures and a conforming type is anything for which each operation resolves:

```cpp
struct Drawable : facade<
    operation<"draw", void(Canvas&) const>,
    operation<"area", double() const>,
    optional_operation<"serialize", std::string() const>> {};

Erased<Drawable> d = Circle{1};   // storage policy selectable: heap, 32-byte SBO, non-owning
d.call<"draw">(canvas);
```

Requirements: (1) a manual dispatch table generated from the facade's operation list with a fold over the pack (Chapter 17); (2) three storage policies (owning heap, SBO with a fallback, non-owning reference) selected by a template parameter; (3) `copy`/`move` capabilities derived from the facade and the stored type; (4) concept checking at construction with readable diagnostics; (5) a benchmark against a virtual-interface equivalent and `std::variant` for 1, 4 and 16 types; (6) a README comparing your design with Microsoft's `proxy` and Boost.TypeErasure (read their documentation first and cite it).

---

## 14. Knowledge check

1. Name the three parts of the type-erasure pattern and what each does.
2. Why does a type-erased wrapper need a *constrained* template constructor?
3. How is `std::function` laid out on libstdc++, and why does it use two function pointers instead of a virtual base class?
4. When does `std::function` allocate? Who decides that rule?
5. What is the `const` problem of `std::function::operator()`, and how does `move_only_function` fix it?
6. What is `function_ref`, when is it the right parameter type, and what is its main hazard?
7. Why can `shared_ptr<T>` have a custom deleter without it appearing in its type, while `unique_ptr<T, D>` cannot?
8. What is the SBO decision `fits<T>` made of? Why `nothrow_move_constructible`?
9. Why does `std::any_cast<T>` require the exact stored type?
10. Compare type erasure with virtual functions and with `variant` along: openness, ownership, allocation, intrusiveness.
11. Why is erasing a comparator in an inner loop usually a mistake, and what do you use instead?
12. What happens to `std::function` in code built with `-fno-rtti`?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. The **concept** (the hidden abstract interface listing the erased operations), the **model** (a template `Model<T>` implementing the interface by forwarding to `T`'s real operations), and the **owner** (the wrapper that holds a pointer/buffer to a model and gives value semantics: copy, move, destroy).
2. A greedy `template <class T> Wrapper(T&&)` also matches `Wrapper&` and `const Wrapper&`, hijacking copy/move and producing a wrapper that holds a wrapper. Constrain it to exclude the wrapper's own type.
3. `{_M_functor (16 B union), _M_manager, _M_invoker}` = 32 B. The invoker is a direct function pointer so the hot call is one load + indirect jump (no vptr); the manager is a single catch-all function for rarely used operations (clone, destroy, type info), keeping the object small.
4. When the callable doesn't qualify for the inline buffer (libstdc++: more than 16 bytes, over-aligned, or **not trivially copyable**, as measured in Experiment 4). The standard permits but does not require SBO, so the rule is implementation-defined.
5. `operator()` is `const` but calls the stored target as a non-const object, so a `const std::function` can mutate state (and races under concurrent calls). `move_only_function`'s signature includes cv/ref/noexcept qualifiers and the call is made with exactly that constness.
6. A non-owning reference to a callable (object pointer + call thunk). Right for parameters that only *call* the callback during the call; the hazard is dangling when stored beyond the callable's lifetime.
7. `shared_ptr` stores the deleter in its control block behind a virtual/indirect interface (erased); `unique_ptr` stores the deleter as a member of type `D` to avoid allocation/overhead, so `D` is part of the type.
8. `sizeof(T) ≤ buffer`, `alignof(T)` compatible with the buffer alignment, and `nothrow_move_constructible` so the wrapper's own move can be `noexcept` and the buffer can be relocated safely (e.g. during `vector` reallocation).
9. The check compares `typeid(T)` with the stored dynamic type; a base or convertible type is not the same type (no implicit conversions), so you must name the exact stored type.
10. Virtual: open set, intrusive (base class), usually heap + pointers, by reference. `variant`: closed set, non-intrusive, by value, no allocation, exhaustive visitation. Type erasure: open set, non-intrusive, by value (owning wrapper, SBO or one allocation), dispatch via hidden virtual/table.
11. Each call becomes an indirect call that cannot be inlined (and a possible allocation at construction), which dominates for tiny bodies called billions of times. Use a template parameter (`std::sort`'s `Compare`) or a closed `variant`.
12. `std::function` itself still works, but `target()`/`target_type()` (which need `typeid`) are unavailable; `std::any` requires RTTI and won't compile.

</details>

---

[← Previous: Chapter 20](20-static-polymorphism.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 22 — Exceptions →](../part-09-error-handling/22-exceptions.md)
