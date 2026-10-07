# Chapter 12 — Views and Non-Owning Types

> **Part V · The modern standard library** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 4 hours**
> **Prerequisites:** [Chapter 3](../part-02-object-model-and-lifetime/03-lifetime-and-storage.md), [Chapter 11](11-containers.md) &nbsp;|&nbsp; **Standards:** C++17 (`string_view`), C++20 (`span`, ranges), C++23 &nbsp;|&nbsp; **Tools:** ASan, `clang++` lifetime warnings

[← Previous: Chapter 11](11-containers.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 13 — optional, variant, any, expected →](13-optional-variant-any-expected.md)

---

**In one sentence:** `std::string_view`, `std::span`, iterators and range views let a function *look at* data it does not own with zero copies, **at the price of a lifetime obligation that the type system cannot check**.

> **Non-owning abstractions create lifetime responsibilities.** Every view is a promise by the *caller* that something else outlives it.

**By the end of this chapter you can:**

- list every non-owning type in the standard library and say for each *what it points to* and *what invalidates it*
- recognize the five classic dangling patterns on sight
- explain what a **borrowed range** is and why `std::ranges::find(make_vector(), 3)` returns `std::ranges::dangling`
- reason about **lazy** views: when a predicate runs, what is cached, what is recomputed, why `filter` isn't `const`-iterable
- choose a parameter type (`T&`, `span`, `string_view`, `const T&`, `unique_ptr`, `T*`) for an API from its ownership semantics
- use compiler and sanitizer support (`[[clang::lifetimebound]]`, `-Wdangling`, ASan, C++23 reference-from-temporary traits)

---

## 1. Problem

Copying is expensive and often pointless:

```cpp
void parse(std::string line);          // copies the whole line to read it once
void process(std::vector<int> v);      // copies a million ints to sum them
bool starts_with(const std::string&, const char*);   // forces a std::string to exist even for a literal
```

Before C++17, interfaces took `const std::string&` or `const char*, size_t` pairs. The first forces a *heap allocation* when the caller has only a literal or a substring; the second is error-prone and carries no semantics. What was missing was a **vocabulary type for "a read-only window onto some characters/elements I don't own."**

But a window can outlive what it looks through, and C++ has no borrow checker.

---

## 2. Historical context

| Era | Approach | Weakness |
|---|---|---|
| C | `(const char* p, size_t n)` | Two parameters that must travel together, no `const` on length |
| C++98 | `const std::string&`; iterator pairs `(first, last)` | Reference forces an owning type; iterator pairs are verbose and unverified |
| Libraries 2000s | `llvm::StringRef`, `llvm::ArrayRef`, `boost::string_ref`, Google `StringPiece`, `gsl::span` | Each codebase reinvents it |
| C++17 | **`std::string_view`** (from Library Fundamentals TS) | Immutable, string-only |
| C++20 | **`std::span<T, N>`** (mutable, contiguous), `<ranges>`: **views**, `subrange`, `ref_view`, `borrowed_range`, `ranges::dangling` | Lifetime errors are now *easier* to write |
| C++23 | `std::mdspan`, `std::views::zip/chunk/slide/…`, `reference_constructs_from_temporary`, `lifetimebound` in libstdc++/libc++ internals, `views::as_rvalue` | |
| C++26 | `views::concat`, `views::enumerate` (adopted), `std::optional<T&>` (adopted), hardened library preconditions | Not yet widely implemented |

The standard library's own answer to lifetime safety has been *vocabulary and conventions*: `ranges::dangling`, `borrowed_range`, `[[clang::lifetimebound]]` in implementation, `-Wdangling-gsl` in compilers. There's no language-level solution yet (profiles for lifetime safety are a proposal, not standard).

---

## 3. Modern solution

```cpp
void parse(std::string_view line);                 // any string-like source, no copy, no allocation
double mean(std::span<const double> xs);           // vector, array, C array, subrange: one function
for (int x : v | std::views::filter(even) | std::views::take(5)) { /* lazy pipeline */ }
```

Three families:

| Family | Types | Looks at |
|---|---|---|
| **Contiguous views** | `string_view`, `span<T>`, `mdspan` | A pointer + length (+ extents) |
| **Iterator/range views** | `subrange<I, S>`, `ref_view<R>`, `iota_view`, `filter_view`, `transform_view`, … | A *range*, via iterators; adaptors compose lazily |
| **Pointer-like observers** | `T*`, `T&`, `weak_ptr<T>`, `reference_wrapper<T>`, iterators themselves | One object, or a position in a sequence |

---

## 4. Mental model

### A view is a *borrowed pair of positions*

```text
   owner:    std::string s = "hello world, how are you";
                              ┌──────────────────────────┐
   storage:                   │h e l l o   w o r l d , … │   ◄─── lifetime belongs to `s`
                              └──────────────────────────┘
                                 ▲           ▲
   view:   string_view sv ───────┘───────────┘            { const char* data; size_t size }   16 bytes
   view:   span<char> sp  ───────┘                        { char* data; size_t size }          16 bytes

   RULE:   the view is valid ⇔ the owner is alive  AND  the owner has not reallocated / modified the region.
```

A view has two properties the type system **does not** express:

1. **Provenance**: *which owner* it points into
2. **Validity window**: *for how long* that owner's storage stays put

Both live in your head, or in documentation. Every view bug is a failure to track one of them.

### The "five dangling patterns" (memorize these)

```text
 1. Temporary owner:       string_view sv = make_string();             owner dies at the semicolon
 2. Returned local:        string_view f() { std::string s = …; return s; }     owner dies at return
 3. Invalidated by growth: auto sp = span(v); v.push_back(1); sp[0];   storage moved
 4. Stored view of param:  struct Config { string_view name; }; Config c{ std::string("x") };
 5. Lazy pipeline over temporary:  auto v = make_vec() | views::filter(p);   (the filter holds a ref to a dead vec)
```

### Lazy views are *recipes*, not data

```text
   v | views::filter(p) | views::transform(f) | views::take(3)
                  │               │                  │
                  └───────────────┴──────────────────┘
                  nothing runs until you iterate.
                  Every iteration re-runs the pipeline (filter caches begin() only).
                  The pipeline stores iterators/refs into v: it is itself a view, and dangles if v dies.
```

---

## 5. Language rules

### 5.1 `std::string_view`  `[string.view]`

```cpp
template <class CharT, class Traits = char_traits<CharT>> class basic_string_view;   // {const CharT* data_; size_t size_;}
```

| Fact | Consequence |
|---|---|
| **Not null-terminated.** `sv.data()` points into the middle of a larger buffer for a substring | Passing `sv.data()` to a C API (`fopen`, `strlen`, `atoi`) reads past the end of the view |
| Implicitly constructible from `std::string`, `const char*`, string literals | Hides dangling: `string_view sv = std::string("x");` compiles |
| **Trivially copyable**, 16 bytes | Pass by value, never by `const&` |
| `remove_prefix`/`remove_suffix`/`substr` are O(1), modify only the view | |
| `substr` of a `string_view` returns a `string_view` (no allocation); of a `string` returns a **copy** | `s.substr(3)` allocates; `std::string_view(s).substr(3)` does not |
| Concatenation `sv + sv` does not exist | Needs a `std::string` |
| C++23: `string_view` constructor from `nullptr` is deleted; range constructor is `explicit` | |

### 5.2 `std::span<T, Extent>`  `[views.span]`

| Fact | Consequence |
|---|---|
| `{T* data; size_t size}` for `dynamic_extent`; `{T* data}` for a static `Extent` | `span<T, 4>` is one pointer |
| Mutable by default: `span<int>` writes through; `span<const int>` is read-only | Const-correctness is on the *element type*, not on the span |
| Constructs from arrays, `std::array`, `std::vector`, any contiguous sized range | Implicit for `span<const T>` from `const vector<T>&`; **a temporary vector** also binds to `span<const T>` (dangling, Pattern 1) |
| `front()`, `back()`, `operator[]`, `first/last/subspan` | **Not bounds-checked** in the standard (UB if out of range); libstdc++ with `_GLIBCXX_ASSERTIONS` / `-D_GLIBCXX_HARDEN=1` and libc++ hardened mode *do* check |
| C++26 adds `span::at` | `at()` is not in C++20/23; it's checked-index access |

### 5.3 Borrowed ranges and `ranges::dangling`  `[range.range]`

A range type `R` is a **`borrowed_range`** if iterators obtained from an *rvalue* of `R` remain valid after the rvalue is destroyed. That holds when the range **doesn't own its elements**:

| Type | `borrowed_range`? | Why |
|---|:-:|---|
| `std::vector<int>` | ❌ | Iterators die with the vector |
| `std::string_view`, `std::span<T>` | ✅ | Iterators point into storage owned elsewhere |
| `std::ranges::subrange<I, S>` | ✅ | A pair of iterators |
| `std::ranges::ref_view<R>` | ✅ | Reference to an lvalue range |
| `std::ranges::owning_view<R>` | ❌ | Owns the range |
| `std::ranges::iota_view` | ✅ | Values computed, no storage |
| `filter_view<…>`, `transform_view<…>` | depends | Borrowed iff the underlying view is **and** the object holds no state of its own (they're *not*: they store the predicate/function) |

**Why it exists.** Algorithms like `std::ranges::find(rng, x)` return an iterator into `rng`. If `rng` is an rvalue owning container, the iterator would dangle immediately. So the algorithm checks `borrowed_range<R>` and **returns `std::ranges::dangling`** (an empty tag type) when it fails, converting a runtime bug into a compile error the moment you dereference it (Experiment 3).

### 5.4 Range adaptors: laziness and caching  `[range.adaptors]`

| Adaptor | Evaluation | Caches | `const`-iterable | Notes |
|---|---|---|:-:|---|
| `views::transform(f)` | lazy, **on every dereference** | no | ✅ | `f` runs again every pass |
| `views::filter(p)` | lazy | **`begin()`** (first matching element, "amortized O(1) required") | ❌ | Needs mutation to cache: a `const filter_view` is not a range |
| `views::take(n)`, `drop(n)` | lazy | `drop`: `begin()` for non-random-access | partly | |
| `views::reverse` | lazy | `begin()` for non-common bidirectional | partly | |
| `views::split`, `chunk_by`, `slide` | lazy | `begin()` of the first element | partly | `slide`/`chunk` need forward ranges |
| `views::join` | lazy | inner range | ❌ for prvalue inner | |
| `views::all(r)` | `ref_view` for an lvalue; `owning_view` for an rvalue (C++20) | — | ✅ | Chooses ownership for you |
| `views::as_rvalue` (C++23) | `move_iterator` view | — | ✅ | |
| `views::to<C>()` / `ranges::to<C>` (C++23) | **eager**: materializes into a container | — | — | Where a pipeline ends in an owner |

**A view is a range with O(1) move and (preferably) O(1) copy** `[range.view]`. A pipeline's final type is a view; it holds *copies of the adaptors* and *references to the data*.

### 5.5 Detecting reference-to-temporary bugs (C++23)

```cpp
std::reference_constructs_from_temporary_v<const std::string&, const char*>   // true:  binding would create a temporary
std::reference_converts_from_temporary_v  <const std::string&, const char*>   // true
```

Libraries use these to **delete** the constructor that would dangle (`std::pair`, `std::tuple`, `std::function`'s `target`, and `std::views` internals). You can use them in your own wrapper types (Experiment 4).

### 5.6 Compiler support for lifetime checking

| Mechanism | Compiler | What it catches |
|---|---|---|
| `-Wdangling-gsl` (default in Clang) | Clang 14+ | `string_view sv = make_string();` and similar for `[[gsl::Pointer]]`/`[[gsl::Owner]]` types |
| `-Wdangling-pointer`, `-Wdangling-reference` | GCC 12 / 13 | Pointer to a local escaping; reference bound to a temporary through a function call (`-Wdangling-reference` has false positives) |
| `[[clang::lifetimebound]]` | Clang (GCC: `[[gnu::lifetimebound]]`, not yet) | Return value of a function **must not outlive** the annotated parameter |
| `-fsanitize=address` + `ASAN_OPTIONS=detect_stack_use_after_return=1` | GCC/Clang | The dynamic backstop for all of the above |
| Lifetime profile (C++ Core Guidelines checker, clang-tidy `bugprone-dangling-handle`) | Clang-tidy | Static analysis |

> **GCC 14's warnings are weaker than Clang 18's here.** In Experiment 1, Clang warns at compile time; GCC 14 compiles silently and ASan catches it at run time. Always run both compilers and the sanitizer.

### Layer check

| Layer | Decides |
|---|---|
| **Standard** | What a view *is* (`view`, `borrowed_range`), preconditions (UB for dangling), `dangling` tag, hardened preconditions in C++26 |
| **Compiler** | What warnings you get; whether `lifetimebound` is honoured |
| **ABI** | A `string_view`/`span` is two registers: **passed in registers** on x86-64 SysV (trivially copyable, 16 bytes), unlike `const std::string&` (a pointer) or `std::string` (hidden pointer) |
| **CPU** | A contiguous view is a streaming access pattern; a lazy pipeline of `filter` + `transform` over a `vector` compiles to one fused loop (Chapter 14) |

---

## 6. Implementation model

`string_view` and `span` are plain structs `{ptr, size}` (libstdc++: `_M_len`, `_M_str`; `span<T,N>` uses `__span_extent_storage`, which is empty for static extents). Because they are trivially copyable and 16 bytes, both fit in two registers:

```text
   void f(std::string_view s)         →  rdi = length, rsi = pointer     (libstdc++ declares `_M_len` first; no memory traffic)
   void f(const std::string& s)       →  rdi = pointer to string; the callee loads data and size: 2 more loads
   void f(std::string s)              →  rdi = hidden pointer to a copy that the caller constructed
```

**Range pipelines** are nested class templates holding the previous view by value, plus a callable. `v | filter(p) | transform(f)` has type `transform_view<filter_view<ref_view<vector<int>>, P>, F>` (an owning-by-value chain of small structs). Iteration instantiates iterator classes layered the same way; after inlining, a pipeline typically collapses to the loop you'd have written by hand (Chapter 14 verifies this in assembly).

---

## 7. Experiments

### Experiment 1: The classic dangling `string_view`

```cpp
// @test crash -std=c++23 -O0 -fsanitize=address -g err=heap-use-after-free
#include <cstdio>
#include <string>
#include <string_view>

std::string make() { return std::string(40, 'a'); }       // longer than SSO: heap-allocated

int main() {
    std::string_view sv = make();      // the temporary std::string dies at the end of this full-expression
    std::printf("%c\n", sv[0]);        // reads freed heap memory
}
```

ASan prints `heap-use-after-free` with **three** stacks: the read in `main`, the free (the `std::string` destructor at the semicolon), and the allocation (inside `make`). That triple is the signature of every temporary-owner bug.

Compile the same code with Clang for the *static* diagnostic:

```text
v3.cpp:5:35: warning: object backing the pointer will be destroyed at the end of the full-expression [-Wdangling-gsl]
    5 | int main(){ std::string_view sv = make(); std::printf("%c\n", sv[0]); }
      |                                   ^~~~~~
```

GCC 14.2 with `-Wall -Wextra` produced **no warning** for the same file (verified). What survives for short strings (≤ 15 chars) is a **stack** bug rather than a heap bug, harder to see: SSO puts the characters inside the dead `std::string` object, so the program usually "works" until the stack slot is reused. That is why dangling views to *short* strings pass tests and fail in production.

The three safe shapes:

```cpp
// @test run -std=c++23 -O0 -fsanitize=address,undefined
#include <cstdio>
#include <string>
#include <string_view>

std::string make() { return std::string(40, 'a'); }

// 1. Name the owner so it outlives the view:
void ok_named()   { std::string owner = make(); std::string_view sv = owner; std::printf("named   : %c\n", sv[0]); }

// 2. Consume within the same full-expression:
std::size_t length_of(std::string_view v) { return v.size(); }
void ok_temp()    { std::printf("temp    : %zu\n", length_of(make())); }           // the temporary lives until the end of THIS statement

// 3. Own the data when you need to keep it:
struct Config { std::string name; };                                                // std::string, not string_view
void ok_stored()  { Config c{make()}; std::printf("stored  : %c\n", c.name[0]); }

int main() { ok_named(); ok_temp(); ok_stored(); }
```

```text
# output (gcc 14.2.0, x86-64 Linux)
named   : a
temp    : 40
stored  : a
```

### Experiment 2: `string_view` is not null-terminated

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

int main() {
    std::string s = "alpha=beta;gamma=delta";
    std::string_view key = std::string_view(s).substr(0, 5);      // "alpha"

    std::printf("view size            : %zu\n", key.size());
    std::printf("strlen(key.data())   : %zu   <- reads past the view until the next NUL\n", std::strlen(key.data()));
    std::printf("printf(\"%%s\", data()) : %s\n", key.data());
    std::printf("printf(\"%%.*s\")       : %.*s\n", int(key.size()), key.data());

    // Safe ways to hand a view to a C API that needs a NUL-terminated string:
    std::string z(key);                                            // allocation-free for ≤15 chars (SSO), but a copy
    std::printf("std::string copy     : %s (strlen %zu)\n", z.c_str(), std::strlen(z.c_str()));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
view size            : 5
strlen(key.data())   : 22   <- reads past the view until the next NUL
printf("%s", data()) : alpha=beta;gamma=delta
printf("%.*s")       : alpha
std::string copy     : alpha (strlen 5)
```

`key.data()` is the address of `s[0]`; `strlen` runs until the first `\0` in the **whole** buffer (22 characters), not the view's 5. Any API taking `const char*` and reading to the terminator (`std::stoi`, `fopen`, `getenv`, `std::filesystem::path(const char*)`, Qt's `QString::fromUtf8(const char*)`) is a trap. Prefer the `(ptr, len)` overloads, or accept the copy.

### Experiment 3: Borrowed ranges and `dangling`

```cpp
// @test run -std=c++23 -O0
#include <algorithm>
#include <cstdio>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace rg = std::ranges;

std::vector<int> make() { return {1, 2, 3, 4, 5, 6}; }

// What is borrowed? (a table you can compile)
static_assert( rg::borrowed_range<std::string_view>);
static_assert( rg::borrowed_range<std::span<int>>);
static_assert( rg::borrowed_range<rg::subrange<int*>>);
static_assert( rg::borrowed_range<rg::iota_view<int, int>>);
static_assert( rg::borrowed_range<std::vector<int>&>);                // an lvalue reference is always borrowed
static_assert(!rg::borrowed_range<std::vector<int>>);                 // an rvalue vector is not
static_assert(!rg::borrowed_range<std::string>);

int main() {
    auto it = rg::find(make(), 3);                                    // rvalue vector → returns `dangling`, not an iterator
    static_assert(std::is_same_v<decltype(it), rg::dangling>);
    // *it;                                                           // ← compile error: dangling has no operator*

    std::vector<int> v = make();
    auto it2 = rg::find(v, 3);                                        // lvalue → real iterator
    std::printf("found %d at index %td\n", *it2, it2 - v.begin());

    auto sub = rg::find(std::span<int>(v), 4);                        // span rvalue is a borrowed_range → real iterator
    std::printf("through a span prvalue: %d (valid because the storage is v's)\n", *sub);

    // The algorithm that returns BOTH: rg::subrange of the found tail
    auto tail = rg::find(v, 4);
    std::printf("tail starts at %d, remaining %zu elements\n", *tail, size_t(v.end() - tail));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
found 3 at index 2
through a span prvalue: 4 (valid because the storage is v's)
tail starts at 4, remaining 3 elements
```

This is the design pattern: **a type-level flag converts a lifetime hazard into an error at the point of use**. If you write an algorithm that returns an iterator into its argument, constrain it with `borrowed_range` and return `ranges::dangling` otherwise.

### Experiment 4: Laziness, caching, and recomputation

```cpp
// @test run -std=c++23 -O0
#include <algorithm>
#include <cstdio>
#include <ranges>
#include <vector>

namespace vw = std::views;

int main() {
    std::vector<int> v{1, 2, 3, 4, 5, 6, 7, 8};
    int calls = 0;

    auto evens = v | vw::filter([&](int x) { ++calls; return x % 2 == 0; });
    std::printf("after building the pipeline        : %d predicate calls (nothing ran)\n", calls);

    auto b1 = evens.begin(); auto b2 = evens.begin(); auto b3 = evens.begin();
    std::printf("after three calls to begin()       : %d  (first call scanned to the first match, then CACHED)\n", calls);

    calls = 0;
    for (int x : evens) (void)x;
    std::printf("one full pass                      : %d  (one call per source element)\n", calls);

    calls = 0;
    for (int x : evens) (void)x;
    for (int x : evens) (void)x;
    std::printf("two full passes                    : %d  (filter does NOT cache elements: recomputed)\n", calls);

    int tcalls = 0;
    auto doubled = v | vw::transform([&](int x) { ++tcalls; return x * 2; });
    long s = 0;
    for (int x : doubled) s += x;
    for (int x : doubled) s += x;
    std::printf("transform, two passes over 8 items : %d function calls\n", tcalls);

    // materialize ONCE if you will traverse many times or need random access:
    tcalls = 0;
    auto vec = doubled | std::ranges::to<std::vector>();
    for (int x : vec) s += x;
    for (int x : vec) s += x;
    std::printf("ranges::to<vector> then 2 passes   : %d function calls\n", tcalls);
    (void)b1; (void)b2; (void)b3; (void)s;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
after building the pipeline        : 0 predicate calls (nothing ran)
after three calls to begin()       : 2  (first call scanned to the first match, then CACHED)
one full pass                      : 6  (one call per source element)
two full passes                    : 12  (filter does NOT cache elements: recomputed)
transform, two passes over 8 items : 16 function calls
ranges::to<vector> then 2 passes   : 8 function calls
```

Two practical rules fall out:

- **A lazy pipeline that you traverse more than once re-runs its functions** every time. If the function is expensive or has side effects, materialize (`ranges::to<std::vector>()`) or restructure.
- `filter_view::begin()` is cached, which is why a **`const` filter view is not a range**:

```cpp
// @test fail -std=c++23 err=discards
#include <ranges>
#include <vector>
int main() {
    std::vector<int> v{1, 2};
    auto f = v | std::views::filter([](int x) { return x > 1; });
    const auto& cf = f;
    for (int x : cf) (void)x;       // error: passing 'const filter_view' as 'this' discards qualifiers
}
```

Which means a function taking `const auto& r` (`template <std::ranges::range R> void f(const R&)`) **does not accept a filter view**. Take ranges by **forwarding reference**: `std::ranges::range auto&& r`.

### Experiment 5: Pipelines that dangle

```cpp
// @test crash -std=c++23 -O0 -fsanitize=address -g err=use-after
#include <cstdio>
#include <ranges>
#include <vector>

std::vector<int> make() { return {1, 2, 3, 4, 5, 6, 7, 8}; }

int main() {
    // `views::all` of a prvalue vector would be an owning_view and is fine (C++20):
    auto fine = make() | std::views::filter([](int x) { return x > 2; });
    long s = 0; for (int x : fine) s += x;
    std::printf("owning pipeline over a prvalue: %ld (OK: owning_view keeps the vector alive)\n", s);

    // A view of a NAMED lvalue that goes out of scope is not:
    auto make_view = []() {
        std::vector<int> local = {1, 2, 3, 4};
        return local | std::views::filter([](int x) { return x > 1; });   // ref_view<local>: dangles when `local` dies
    };
    auto v = make_view();
    for (int x : v) std::printf("%d ", x);                                // use-after-scope / use-after-return
}
```

Rule: **a pipeline starting from an lvalue holds a reference to it** (`ref_view`); a pipeline starting from an rvalue *container* **owns it** (`owning_view`, C++20; before GCC 11 / the P2415 fix it was a hard error). The bug above is the lvalue case: you returned a *view* of something local.

### Experiment 6: Lifetime traits as a guard in your own types

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>

// A "view-holding" struct that refuses to be built from a temporary:
template <class T>
struct Ref {
    const T& r;
    template <class U>
        requires std::is_convertible_v<U, const T&>
    Ref(U&& u) : r(std::forward<U>(u)) {
        static_assert(!std::reference_constructs_from_temporary_v<const T&, U&&>,
                      "Ref<T> would bind to a temporary and dangle");
    }
};

static_assert( std::reference_constructs_from_temporary_v<const std::string&, const char*>);   // literal → temporary string
static_assert(!std::reference_constructs_from_temporary_v<const std::string&, std::string&>);  // lvalue: fine

int main() {
    std::string owner = "stable";
    Ref<std::string> ok(owner);                      // binds to a named lvalue
    std::printf("ok: %s\n", ok.r.c_str());
    // Ref<std::string> bad("literal");              // ← static_assert fires: would bind to a temporary std::string
    // Ref<std::string> bad2(std::string("x"));      // ← an rvalue std::string&& converts with NO new temporary, and dangles
                                                     //   at the end of the statement: the trait cannot see it. See below.
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
ok: stable
```

The trait catches *conversion* temporaries (`"literal"` → `std::string`), **not** the case where the argument is *itself* a temporary of the right type (`std::string("x")`): there the reference binds directly and the dangling comes from the argument's own lifetime. A complete defence is **two overloads**: `Ref(const T&)` and `Ref(T&&) = delete`. The cost is that it also rejects legitimate uses with a `std::move`d local. Annotate instead with `[[clang::lifetimebound]]` where available:

```cpp
// @test skip clang-only attribute; compiles on GCC as an unknown attribute warning
struct Name {
    std::string_view v;
    Name(std::string_view s [[clang::lifetimebound]]) : v(s) {}      // warns: `Name n{std::string("x")}` dangles
};
```

---

## 8. Assembly / runtime investigation

**Passing convention: string_view vs `const std::string&` vs `std::string`:**

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=len_view,len_ref,len_copy
#include <string>
#include <string_view>

std::size_t len_view(std::string_view s)    { return s.size(); }
std::size_t len_ref (const std::string& s)  { return s.size(); }
std::size_t len_copy(std::string s)         { return s.size(); }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
len_view(std::basic_string_view<char, std::char_traits<char> >):
	mov	rax, rdi
	ret

len_ref(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&):
	mov	rax, QWORD PTR 8[rdi]
	ret

len_copy(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >):
	mov	rax, QWORD PTR 8[rdi]
	ret
```

`len_view`: the length **is** the first argument register (libstdc++ declares `_M_len` before `_M_str`, so the pair arrives as `rdi = length`, `rsi = pointer`): `mov rax, rdi; ret`, no memory access at all. `len_ref`: loads the length from the string object in memory. `len_copy`: the callee body is as short as `len_ref` (it reads the length from the copy through its hidden pointer), but the caller must *construct* that copy (allocation if > 15 chars) and destroy it afterwards; the callee is as short as the other two but the call site is not. Check `-O0` vs `-O2` at the *call site* to see the construction; this is the cost of a by-value `std::string` parameter that Chapter 6's "by value then move" idiom is *for*: only worth it when the callee stores the string.

---

## 9. Implementation exercise

Implement a minimal `StringView` and `Span<T>` (no `<string_view>`/`<span>`) with:

1. constructors from `(const char*, size_t)`, `const char*` (using `strlen`), `std::string`, `std::array`, `std::vector`, C arrays; **delete** construction from rvalue `std::string`/rvalue vector *where the result would dangle* but allow it for function-argument use (hint: the standard doesn't, since a safe, usable API for `f(make())` requires allowing it)
2. `substr/first/last/subspan`, `remove_prefix`, comparison, hashing, iterators satisfying `std::contiguous_iterator`
3. `static_assert` that `std::ranges::borrowed_range<StringView>` holds (you must opt in: `enable_borrowed_range<StringView> = true`)
4. a **`checked` build mode** (`#ifdef MYVIEW_CHECKED`) that asserts bounds on `operator[]` and records a *generation counter*: the owner (`CheckedVector`) bumps it on reallocation, and the view asserts it still matches (a runtime lifetime checker). This is how `_GLIBCXX_DEBUG` and ASan's container annotations work in spirit

<details>
<summary><strong>Solution sketch: `Span<T>` with `enable_borrowed_range`</strong></summary>

```cpp
// @test run -std=c++23 -O0
#include <array>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <ranges>
#include <vector>

template <class T>
class Span {
    T* p_ = nullptr;
    std::size_t n_ = 0;
public:
    using element_type = T; using value_type = std::remove_cv_t<T>; using iterator = T*;
    constexpr Span() noexcept = default;
    constexpr Span(T* p, std::size_t n) noexcept : p_(p), n_(n) {}
    template <std::size_t N> constexpr Span(T (&a)[N]) noexcept : p_(a), n_(N) {}
    template <class R> requires std::ranges::contiguous_range<R> && std::ranges::sized_range<R>
        && std::convertible_to<std::remove_reference_t<std::ranges::range_reference_t<R>>(*)[], T(*)[]>
    constexpr Span(R&& r) noexcept : p_(std::ranges::data(r)), n_(std::ranges::size(r)) {}

    constexpr T* begin() const noexcept { return p_; }
    constexpr T* end()   const noexcept { return p_ + n_; }
    constexpr T* data()  const noexcept { return p_; }
    constexpr std::size_t size() const noexcept { return n_; }
    constexpr T& operator[](std::size_t i) const noexcept { return p_[i]; }
    constexpr Span subspan(std::size_t off, std::size_t cnt) const noexcept { return {p_ + off, cnt}; }
};

template <class T> inline constexpr bool std::ranges::enable_borrowed_range<Span<T>> = true;   // opt in

static_assert(std::ranges::contiguous_range<Span<int>>);
static_assert(std::ranges::borrowed_range<Span<int>>);                   // NOT automatic for user types

int main() {
    std::vector<int> v = {5, 6, 7, 8};
    std::array<int, 3> a = {1, 2, 3};
    int c[2] = {9, 10};
    long total = 0;
    for (Span<int> s : {Span<int>(v), Span<int>(a), Span<int>(c)}) for (int x : s) total += x;
    auto it = std::ranges::find(Span<int>(v), 7);                        // prvalue Span is borrowed: returns a real iterator
    std::printf("total=%ld, found %d via borrowed prvalue span\n", total, *it);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
total=51, found 7 via borrowed prvalue span
```

</details>

---

## 10. Real-world example

### Ownership and parameter passing: the decision table

The course brief asks for an explicit comparison of the "raw pointer / `unique_ptr` / `shared_ptr` / reference / `span` / observer" family. This is the guide for **parameters and return values**: it follows from *who owns* and *how many*.

| Intent at the call | Parameter type | Notes |
|---|---|---|
| *Read* a single object, must exist | `const T&` | Caller keeps ownership; cannot be null; can bind a temporary: fine **inside** the call |
| *Modify* a single object in place, must exist | `T&` | Rejects temporaries, which is right for an out-parameter |
| *Maybe* an object (optional argument) | `T*` (non-owning) or `std::optional<std::reference_wrapper<T>>` / C++26 `optional<T&>` | `T*` = "nullable borrowed reference". **Never** `T*` to mean "I take ownership" |
| *Read* a sequence of contiguous elements | `std::span<const T>` | One signature for `vector`, `array`, C array, `subspan` |
| *Modify* contiguous elements in place | `std::span<T>` | Cannot resize: the callee can't change the container's size, a feature |
| *Read* text | `std::string_view` | **By value**; never `const string_view&` |
| *Read* a generic range | `std::ranges::input_range auto&&` / `forward_range` | Forwarding reference (Experiment 4) |
| *Take ownership* of a heap object | `std::unique_ptr<T>` **by value** | The signature *says* "you are responsible now"; caller writes `std::move(p)` |
| *Share* ownership, keep alive past the call | `std::shared_ptr<T>` **by value** | Only when the callee *stores* it. For observing without ownership use `T&`/`T*` plus `.get()` |
| *Share*, but **don't extend** lifetime | `std::weak_ptr<T>` | The callee must `lock()`; use for caches, observer lists, back-references |
| *Store* a string/vector that was passed in | by value, then `std::move` (Chapter 6) | One move for rvalues, one copy for lvalues |
| *Return* a freshly created object | by value | NRVO/guaranteed elision |
| *Return* "this object's internal data" | `const T&`, `span`, `string_view` | **Valid only while `*this` is alive and unmodified**: document it |
| *Return* a **newly allocated** polymorphic object | `std::unique_ptr<Base>` | Factory |

**What `shared_ptr` is *not* for.** It's "shared ownership with deterministic last-owner destruction," not "a safer pointer." If you can name a single owner, use `unique_ptr` and pass borrowed references; `shared_ptr` costs an allocation (or a combined control block), an atomic increment/decrement per copy, and **obscures who is responsible for lifetime**. Chapter 25 quantifies this.

### Where the standard itself is careful

| Place | What it does |
|---|---|
| `std::format("{}", sv)` | Takes by forwarding reference; the formatted string is built inside the call, so temporaries are fine |
| `std::thread(f, sv)` | **Decays and copies** the `string_view` (Chapter 7): you copy the *view*, not the characters. A `std::string_view` to a local string passed to a thread is the dangling-in-another-thread bug |
| `std::unordered_map<std::string_view, V>` | Keys are views: the map doesn't own the characters. Valid only if each backing string outlives the map |
| `std::filesystem::path(sv)` | Copies |
| Qt: `QStringView`, `QByteArrayView` (Qt 6) | The same idea, with the same lifetime obligation; `QStringView` from a `QString` temporary: dangling |

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| `string_view sv = make_string();` | Heap-use-after-free (long) or silent garbage (short, SSO) | Name the owner, or use `std::string` |
| Returning `string_view`/`span` of a **local** | Use-after-return | Return the owning type |
| `string_view` data member initialized from a parameter | Dangles when the argument was a temporary | Store `std::string`; or document & annotate `lifetimebound` |
| Concatenating through `string_view`: `std::string s = sv1 + sv2;` | Doesn't compile | `std::string(sv1) + sv2`, or `std::format`, or `s.append(sv)` |
| `sv.data()` into a C API | Reads past the view's end | Copy to `std::string` / use the `(ptr, len)` function |
| `span` kept across `push_back` | UAF (Pattern 3) | Re-create the span after mutating the owner; or use indices |
| `span<const T>` from a temporary `vector` | Dangling at the end of the full-expression | Name it |
| `for (auto x : f().items())` (Chapter 3) | Dangling pre-GCC 15 / Clang 19 | `auto&& r = f(); for (auto x : r.items())` |
| Taking `const R&` for generic range functions | Rejects `filter_view` | Forwarding reference |
| Assuming a lazy pipeline is cached | Predicate/function re-runs each pass; side effects repeat | Materialize with `ranges::to` |
| Mutating the underlying container while iterating a view | UB (iterators invalidated; `filter` cached `begin`) | Don't; collect first |
| `views::filter` with a predicate that has side effects or isn't `std::regular_invocable`-pure | Elements dropped or doubled differently on repeated passes | Pure predicates |
| `auto sv = std::string_view(std::string("x"))` | Dangles at the end of the statement | |
| Assuming `std::span::at()` exists | C++20/23 compile error | C++26, or `_GLIBCXX_ASSERTIONS` |
| Using `string_view` as a `map` key when the backing string can change | Corrupts the map (hash/ordering changes under the key) | Use `std::string` keys, or stable storage |
| Passing `std::string_view` where a null-terminated string is needed, via `.data()` | See above | Prefer APIs with a length |
| Treating a `reference_wrapper`, `T*`, or iterator as "safe because it's a standard type" | They are all non-owning too | Same obligations |

---

## 12. Exercises

1. **Find the bug.** For each of the five dangling patterns in §4, write a minimal program, make ASan report it, then fix it. Which can GCC 14 warn about at compile time, which Clang 18, which neither?
2. **`string_view` vs `const string&`.** Benchmark `starts_with(const std::string&, ...)` vs `(string_view, ...)` called with a string literal in a loop. Count allocations with a replaced `operator new`. Explain the result in terms of the implicit conversion.
3. **Break `filter`.** Write a predicate that counts calls and a pipeline `v | filter(p) | transform(f) | take(3)`. Predict the number of calls to `p` and `f` for each of: one pass, two passes, `begin()` twice, `size()`-like operations (`ranges::distance`). Compare.
4. **A safe `Ref`.** Extend Experiment 6's `Ref<T>` with the `T&&` deleted overload. Which valid uses does that block? Try to write a version that blocks only the dangling ones. (You can't in general; why?)
5. **Borrowed or not.** For ten range types (including `views::reverse(v)`, `views::take(sv, 3)`, `views::transform(v, f)`, `std::array<int,3>`, a `std::ranges::subrange`), predict `borrowed_range` and check with `static_assert`. Which surprised you?
6. **Lifetime-checked span.** Implement the *generation-counter* checked span from §9.4, wire it into a `CheckedVector<T>`, and write tests that trigger each failure. Measure the overhead in a tight loop with and without checks.
7. **Hash keys.** Build `std::unordered_set<std::string_view>` from words in a `std::string` document, then *modify the document* (e.g. insert a character). Show the corruption. Fix with an owning-key design and heterogeneous lookup.
8. **The thread boundary.** Pass `std::string_view` into `std::jthread` three ways (by value, `std::ref`, copied into a `std::string`) with the owner destroyed before the thread runs. Which are safe, and why? Verify with TSan/ASan.
9. **Audit.** Take 500 lines of a real C++ codebase (yours, or a library). List every non-owning type in public signatures and write the one-line lifetime contract for each.

---

## 13. Challenge: a tokenizer that never copies

Build a lexer `tokens(std::string_view src)` returning a lazy range of `Token{Kind kind; std::string_view text; Pos pos;}` (identifiers, numbers, strings with escapes, punctuation, comments):

- zero allocations for scanning (verify with a counting `operator new`)
- the `text` of every token is a view into `src`; document and **enforce** the lifetime contract: `tokens(std::string&&)` must be **deleted**, `tokens(const std::string&)` accepted, and a pipeline-ending `ranges::to<std::vector<Token>>()` must keep working
- it's a `std::ranges::input_range` and a `borrowed_range` iff its source is; implement it as a `view_interface`-derived class with a sentinel
- add a `views::filter(not_comment) | views::take_while(not_eof)` pipeline over it and prove, with a call counter, that no token is scanned twice
- provide an *owning* variant `OwningTokens` which keeps a `std::shared_ptr<const std::string>` alive, so tokens may outlive the caller's string; compare cost (one allocation + atomic refcount) and ownership clarity with the borrowing version

Then answer: for a compiler front-end handling 10⁶ tokens/s per core, which of the two designs do you ship, and what do you put in the documentation to prevent the dangling bug?

---

## 14. Knowledge check

1. Name five non-owning standard types and what each can dangle on.
2. Why should `std::string_view` be passed by value and not by `const&`?
3. Why is `sv.data()` not safe to pass to `puts`?
4. What is a `borrowed_range`, and what does `std::ranges::find(make_vec(), 3)` return? Why?
5. What does `filter_view::begin()` cache, and why does that make a `const filter_view` non-iterable?
6. How many times does a `transform` lambda run for a view traversed twice? How do you make it run once?
7. Which of these dangles: `auto a = make_vec() | views::filter(p);` and `auto b = local_vec | views::filter(p); return b;`?
8. Why does a dangling short `string_view` often "work" in tests?
9. What does `std::reference_constructs_from_temporary_v<const std::string&, const char*>` tell you, and what can it not detect?
10. When is `std::span<T>` better than `const std::vector<T>&`? When is it worse?
11. Why does `std::thread(f, sv)` not make `sv` safe?
12. Give the parameter type for: "borrow one object, non-null", "borrow optionally", "take ownership", "share ownership", "observe without keeping alive".

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. `string_view` (the character buffer), `span` (the element buffer, including reallocation), iterators (their container), `ref_view`/pipelines (the underlying range *and* the adaptor objects), `weak_ptr` (the object, via `lock()`), `reference_wrapper`/`T&`/`T*` (the object), `subrange` (the range).
2. It is two words (pointer, length), trivially copyable, and passed in two registers; `const string_view&` adds an indirection (a pointer to a pointer pair) and aliasing concerns, and *prevents* the optimizer from keeping the length in a register.
3. It isn't null-terminated: a view onto a substring or onto a `string` with trailing content continues past its end until the next `\0` (or beyond the buffer for a view of a `char[N]` without NUL).
4. A range whose iterators remain valid after the range object itself is destroyed (it doesn't own the elements). `find(make_vec(), 3)` returns `std::ranges::dangling`, an empty tag, because iterators into an rvalue `vector` would dangle immediately.
5. The first element satisfying the predicate (so `begin()` is amortized O(1)). Caching mutates the view, so `begin()` is non-const; a `const filter_view` therefore has no `begin()` and isn't a range.
6. Twice for each element each time (`transform` doesn't cache). Materialize with `ranges::to<std::vector>()` (or a loop) and traverse the result.
7. `a` doesn't dangle: an rvalue container makes `owning_view` (C++20). `b` dangles: it holds a `ref_view` to `local_vec`, and the function returns it after `local_vec` dies.
8. SSO places the characters inside the (dead) `std::string` object on the stack; until that stack slot is reused, the bytes are still there. The bug fires later, under different stack usage or with longer strings.
9. That binding a `const std::string&` to a `const char*` would create a temporary `std::string` (which dies at the end of the full-expression). It can't detect the case where the argument is *itself* a temporary of the right type, or where the owner dies for any other reason.
10. Better: one signature for vector/array/C array/subrange, fixed-extent compile-time sizes, passed in registers, no dependence on a specific container. Worse: it can't resize; it conveys *no ownership or allocator* info; it can dangle if the vector reallocates while you hold it.
11. The thread copies the *view* (pointer and length), not the characters. If the owning string is destroyed (or modified) before the thread reads it, the thread reads freed memory. Copy into a `std::string` inside the thread's arguments (`std::string(sv)`).
12. `const T&`/`T&`; `T*` (or C++26 `optional<T&>`); `std::unique_ptr<T>` by value; `std::shared_ptr<T>` by value (only if the callee stores it); `std::weak_ptr<T>`.

</details>

---

[← Previous: Chapter 11](11-containers.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 13 — optional, variant, any, expected →](13-optional-variant-any-expected.md)
