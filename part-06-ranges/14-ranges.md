# Chapter 14 — C++20 Ranges

> **Part VI · Ranges** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 7 hours**
> **Prerequisites:** [Chapter 10](../part-04-generic-programming/10-concepts.md), [Chapter 12](../part-05-standard-library/12-views-and-non-owning-types.md) &nbsp;|&nbsp; **Standards:** C++20, C++23 (`zip`, `chunk`, `slide`, `stride`, `join_with`, `to`, `generator`, `fold`), C++26 (`concat`, `enumerate` refinements) &nbsp;|&nbsp; **Tools:** `g++-14`, asm

[← Previous: Chapter 13](../part-05-standard-library/13-optional-variant-any-expected.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 15 — Algorithms and customization points →](15-algorithms-and-customization.md)

---

**In one sentence:** a *range* is anything you can ask for a `begin` and an `end`; the ranges library turns that idea into **concepts**, **lazy composable views** and **constrained algorithms** that can be chained with `|` into pipelines that (usually) compile to the loop you would have written by hand.

**By the end of this chapter you can:**

- state the range concept hierarchy and what each refinement buys you
- explain **sentinels** and why `end()` need not be an iterator
- write pipelines with the standard adaptors (including the C++23 ones), and say what each costs
- use **projections** instead of hand-written comparator lambdas
- implement your own *view* and *range adaptor closure* so that it works with `|`
- read the generated code of a pipeline and decide whether it is free in your case

---

## 1. Problem

The iterator-pair interface of the STL (`first, last`) has four recurring problems:

```cpp
std::vector<int> v = ...;
std::sort(v.begin(), v.end());                                // (1) every call repeats the container twice
std::sort(v.begin(), other.end());                            // (2) nothing checks that the pair belongs together
auto it = std::find_if(v.begin(), v.end(), pred);             // (3) algorithms cannot be composed:
auto sorted_squares = ???                                     //     "square, filter, sort, take 5" needs 3 temporaries
std::list<int> l; std::sort(l.begin(), l.end());              // (4) the error is a 100-line message from <algorithm>
```

| Problem | Consequence |
|---|---|
| Boilerplate | `v.begin(), v.end()` everywhere; easy to mismatch |
| No composition | Each stage materializes a full intermediate container |
| `end` must have the same type as `begin` | A *null-terminated string*, an *infinite sequence* or a *"stop when predicate holds"* range cannot be expressed without a wrapper iterator carrying a flag |
| Unconstrained | Wrong iterator category surfaces deep inside the algorithm |
| Comparator lambdas for sub-fields | `std::sort(v.begin(), v.end(), [](auto& a, auto& b){ return a.age < b.age; })`: every line repeats the field |

---

## 2. Historical context

| Year | Event |
|---|---|
| 1994 | STL: *iterator pairs*; the algorithms are the interface |
| 2005 | Boost.Range (Thorsten Ottosen): a range = `begin/end` pair; adaptors via `\|` for the first time (`rng \| filtered(p) \| transformed(f)`) |
| 2009 | Concepts are removed from C++0x (Chapter 10) and, with them, the first design of a range library |
| 2013–2015 | **Eric Niebler's range-v3**: *concepts emulation*, sentinels, views, projections, customization point objects; the base of the standard design |
| 2017 | Ranges TS published (based on Concepts TS) |
| **2019–2020** | **`<ranges>` merged into C++20** (P0896): range concepts, `std::ranges::` algorithms, views `filter transform take drop reverse join split iota …`, `operator\|`, `borrowed_range` |
| 2021–2022 | **Fixes**: P2415 (`owning_view`), P2210 (`split` redesign), P2325 (`default_initializable`-less views), P2432 (`istream_view`) |
| 2023 | **C++23 additions**: `zip`, `zip_transform`, `adjacent`, `adjacent_transform`, `pairwise`, `chunk`, `slide`, `chunk_by`, `stride`, `cartesian_product`, `join_with`, `repeat`, `as_const`, `as_rvalue`, `enumerate`, `ranges::to`, `fold_left`/`fold_right`, `starts_with`/`ends_with`, `std::generator` |
| 2026 | C++26: `views::concat`, `views::cache_latest`, `views::to_input`, `views::indices`, `ranges::reserve_hint`, `std::execution` interop (Chapter 33+) |

Ranges are therefore the **one library design that came from outside the standard committee and was adopted nearly whole**, and one of the few where C++ got generic-programming *theory* (regular types, sentinels, projections) ahead of most other languages.

---

## 3. Modern solution

```cpp
namespace rg = std::ranges;
namespace vw = std::views;

rg::sort(v);                                         // range overload: one argument
rg::sort(people, {}, &Person::age);                  // projection: sort by a member, no lambda

auto top5 = v | vw::filter(is_prime)                 // lazy: nothing runs yet
              | vw::transform(square)
              | vw::take(5)
              | rg::to<std::vector>();               // C++23: the single *eager* step
```

Four ideas:

| Idea | Mechanism |
|---|---|
| **Range** | A type for which `std::ranges::begin(r)` and `end(r)` are valid (a concept) |
| **Sentinel** | `end(r)` returns a type that need only be *comparable* with the iterator (`it == sent`), not an iterator itself |
| **View** | A cheap-to-copy, non-owning (or owning-but-O(1)-movable) range: the *lazy* unit that adaptors produce |
| **Projection** | A unary callable applied to each element before the algorithm uses it, passed as an extra argument |

---

## 4. Mental model

### The concept hierarchy: capabilities, not classes

```text
                      range                      has begin() / end()
                        │
        ┌───────────────┼──────────────────┐
  input_range      sized_range         borrowed_range      (orthogonal properties)
        │              ▲                 view
 forward_range  ◄──(also: common_range, viewable_range, contiguous...)
        │
 bidirectional_range            ++it  --it
        │
 random_access_range            it += n,  it[n],  it - it    O(1)
        │
 contiguous_range               elements adjacent in memory:  data()  → pointer
```

Each step *adds* operations and *tightens guarantees*. An algorithm declares the weakest category it can work with, e.g. `ranges::find` needs `input_range`; `ranges::sort` needs `random_access_range`; `ranges::reverse` needs `bidirectional_range`.

| Category | What you can do | Examples |
|---|---|---|
| input | One pass, `++` and `*`; copies are *not* independent | `istream_view`, `generator` |
| forward | Multi-pass: copying an iterator saves a position | `forward_list`, `unordered_*`, `filter_view` of a forward range |
| bidirectional | Also `--` | `list`, `set`, `map`, `reverse_view` |
| random access | Also `+= n`, `[n]`, `a - b` in O(1) | `deque`, `iota_view<int>`, `take_view` of random-access |
| contiguous | Also adjacent in memory | `vector`, `array`, `string`, `span`, `string_view`, C arrays |

### A view is a **recipe + a reference** (recap of Chapter 12, now with laziness)

```text
  v | filter(p) | transform(f) | take(3)
        │              │            │
        ▼              ▼            ▼
   filter_view<ref_view<vector>, P>
        └──► transform_view<filter_view<…>, F>
                  └──► take_view<transform_view<…>>        ← the final TYPE encodes the whole pipeline
  sizeof ≈ a few pointers + the stored lambdas: copyable in O(1)
  nothing has executed; iteration drives the whole chain *one element at a time*.
```

### Sentinels: why `end` is a different type

```text
   iterator :  it  ──► element ──► element ──► element ──► …
   sentinel :  "a thing that can tell me whether it has arrived"

   null-terminated string :   it = const char*        sentinel = "points to '\0'"   (no strlen needed up front)
   counted range          :   it = counted_iterator   sentinel = default_sentinel    ("count reached 0")
   infinite range         :   it = iota iterator      sentinel = unreachable_sentinel (comparison is always false: the optimizer deletes the check)
   "until condition"      :   it = any iterator       sentinel = predicate-based     (take_while)
```

The classic `for (auto it = begin; it != end; ++it)` loop works unchanged; `it != end` just calls a *heterogeneous* `operator==`. A **`common_range`** is one where `begin` and `end` have the same type, which is what legacy `std::` algorithms (`std::accumulate`, `std::sort` with iterator pairs) require; `views::common` adapts a non-common range to a common one at a small cost.

---

## 5. Language rules

### 5.1 The range concepts  `[range.range]`

```cpp
template <class T> concept range          = requires(T& t) { ranges::begin(t); ranges::end(t); };
template <class T> concept sized_range    = range<T> && requires(T& t) { ranges::size(t); };
template <class T> concept view           = range<T> && movable<T> && enable_view<T>;
template <class T> concept input_range    = range<T> && input_iterator<iterator_t<T>>;
template <class T> concept common_range   = range<T> && same_as<iterator_t<T>, sentinel_t<T>>;
template <class T> concept borrowed_range = range<T> && (is_lvalue_reference_v<T> || enable_borrowed_range<remove_cvref_t<T>>);
template <class T> concept viewable_range = range<T> && ((view<remove_cvref_t<T>> && constructible_from<remove_cvref_t<T>, T>) || (!view<remove_cvref_t<T>> && (is_lvalue_reference_v<T> || movable<remove_reference_t<T>>)));
```

`ranges::begin/end/size/data` are **customization point objects** (Chapter 15): they accept a member function, an ADL free function, or a built-in array.

### 5.2 What makes something a *view*  `[range.view]`

A view is a range that is **O(1) to move, and, if copyable, O(1) to copy and assign** (C++20 relaxed the original "cheap copy" rule so move-only views like `generator` can exist). The opt-in is `enable_view<T>` (usually by deriving from `std::ranges::view_base` or `view_interface`).

### 5.3 Adaptors: `views::` objects  `[range.adaptor.object]`

A *range adaptor object* is a callable that builds a view; a *range adaptor closure object* is one that is waiting for its range argument. Both of these are equivalent:

```cpp
vw::filter(v, pred)          vw::filter(pred)(v)          v | vw::filter(pred)
```

and the pipe is **left-associative**: `a | f | g` is `(a | f) | g`. Closures compose with each other (`f | g` is a closure) so you can name a partial pipeline:

```cpp
auto squares_of_odds = vw::filter(is_odd) | vw::transform(square);       // a reusable closure
for (int x : v | squares_of_odds) ...
```

### 5.4 The adaptor catalogue (what exists, what it costs)

**Standard since C++20.**

| Adaptor | Result | Category preserved (≤) | Notes |
|---|---|---|---|
| `views::all(r)` | `ref_view` / `owning_view` / the view itself | same | Implicit in pipelines |
| `views::filter(p)` | elements where `p` holds | bidirectional | Caches `begin()`; not `const`-iterable; not `sized` |
| `views::transform(f)` | `f(element)` | random access | Recomputes on each deref; `sized` if input is |
| `views::take(n)`, `drop(n)` | first n / all but first n | random access | `drop` caches `begin()` for non-random-access |
| `views::take_while(p)`, `drop_while(p)` | prefix / suffix by predicate | forward | `take_while` introduces a *sentinel* |
| `views::reverse` | backwards | bidirectional | Needs bidirectional input; caches `begin()` if not common |
| `views::join` | flatten a range of ranges | input…bidirectional | Inner prvalue ranges make it input only |
| `views::split(delim)`, `lazy_split` | subranges separated by a delimiter | forward | **Yields subranges, not strings** |
| `views::elements<N>`, `keys`, `values` | tuple element N | same | `map | views::keys` |
| `views::iota(a[, b])` | a counting range | random access | Infinite when unbounded |
| `views::counted(it, n)`, `views::common` | | | |
| `views::empty<T>`, `views::single(x)` | | | |
| `views::istream<T>(is)` | input from a stream | input | |

**Standard in C++23** (all available in GCC 14's libstdc++, verified):

| Adaptor | What it does |
|---|---|
| `views::zip(a, b, …)`, `zip_transform(f, …)` | Iterate several ranges in lock step; reference type is a tuple of references (a *proxy*, see §5.7) |
| `views::enumerate(r)` | `zip(iota, r)`: index + element |
| `views::adjacent<N>`, `pairwise` | Sliding windows as tuples (`adjacent<2>` = consecutive pairs) |
| `views::slide(n)`, `views::chunk(n)`, `views::chunk_by(p)` | Sliding windows / fixed-size chunks / runs of equal-ish elements, as subranges |
| `views::stride(n)` | Every n-th element |
| `views::cartesian_product(a, b, …)` | Nested loops as a range |
| `views::join_with(delim)` | `join` with a separator between inner ranges |
| `views::repeat(x[, n])` | An infinite/finite repetition |
| `views::as_const`, `views::as_rvalue` | Const view; move-from-each-element view |
| `ranges::to<C>()` | **Materialize** a range into a container (the eager end of a pipeline) |
| `ranges::fold_left`, `fold_right`, `starts_with`, `ends_with`, `contains` | Algorithms |
| `std::generator<T>` (`<generator>`) | A coroutine-based *input range* (Chapter 33) |

**C++26**: `views::concat`, `views::cache_latest`, `views::to_input`, `views::indices`, `views::enumerate` refinements. GCC 14 does **not** have `views::concat` (verified with the feature-test macro `__cpp_lib_ranges_concat`).

### 5.5 Iterator and range properties that matter in practice

| Property | Meaning | Gotcha |
|---|---|---|
| `sized_range` | `size()` in O(1) | `filter`, `drop_while`, `split` aren't sized. `ranges::distance` is then O(n) |
| `common_range` | `begin` and `end` have the same type | Needed by legacy algorithms; `take_while`, `iota(0)` unbounded, etc. are not common |
| `const`-iterable | `const R` is also a range | `filter_view`, `drop_while_view`, `split` on non-const-iterable bases are not: take generic ranges by `R&&`, never `const R&` |
| `borrowed_range` | iterators outlive the range object | `views::take(sv, 3)` is borrowed iff its base is |
| *Proxy references* | `*it` returns a prvalue/proxy not a real reference | `zip` of `vector<bool>`; `std::sort` of a zip view works with `ranges::sort` because it uses `iter_swap`/`iter_move` customization |

### 5.6 Algorithms: the `std::ranges` versions

Every algorithm in `<algorithm>` has a `ranges::` counterpart that:

1. takes a **range** (or iterator + sentinel)
2. is **constrained** with concepts (clear diagnostics)
3. accepts **projections**: `ranges::sort(people, std::less{}, &Person::name)`
4. returns **richer results**: `ranges::find` returns the iterator, `ranges::copy` returns `{in, out}` (`in_out_result`), `ranges::minmax` a `min_max_result`, `ranges::remove`/`unique` a `subrange` of the discarded tail
5. is a **function object**, not a function template: it can't be found by ADL and can be passed as a value (`auto f = std::ranges::sort;`), and **can't be overloaded or customized** by users (Chapter 15)
6. returns `ranges::dangling` on rvalue non-borrowed ranges

### 5.7 Projections  `[algorithms.requirements]`

```cpp
template <input_range R, class Proj = identity, indirect_strict_weak_order<projected<iterator_t<R>, Proj>> Comp = ranges::less>
constexpr borrowed_iterator_t<R> sort(R&& r, Comp comp = {}, Proj proj = {});
```

`proj` is invoked with `std::invoke`, so it can be a **pointer to member data, a pointer to member function, a lambda or any callable**. The comparison sees `proj(*it)`; the *elements* are moved/swapped unchanged. Compare:

```cpp
std::sort(v.begin(), v.end(), [](const P& a, const P& b){ return a.age < b.age; });   // C++11
ranges::sort(v, {}, &P::age);                                                          // C++20
```

### Layer check

| Layer | Decides |
|---|---|
| **Standard** | Concepts, adaptors, algorithms, `view` requirements, laziness and caching rules, the C++23 additions |
| **Compiler / library** | Pipeline types (deeply nested templates), iterator class layout, whether `filter_view` fully inlines; compile time |
| **ABI** | The *type* of a pipeline is part of any function signature that returns it: **do not export pipeline types across a `.so`**; use `ranges::to` or type erasure (`any_view`: not standard) |
| **CPU** | A fused pipeline is one loop; but `filter` introduces an unpredictable branch, and `join`/`split` can defeat vectorization |

---

## 6. Implementation model

### Iteration is a state machine layered per adaptor

```text
   for (auto&& x : p)      where  p = v | filter(P) | transform(F)

   begin():                                      operator++ :                       operator* :
     transform_view::begin()                       ++underlying_filter_iter           F(*underlying_filter_iter)
        └─ filter_view::begin()  (CACHED)            └─ advance until P(*it) holds
              └─ vector::begin()

   Inlined at -O2: the three layers collapse into one loop with the predicate test and the function body in line.
```

What the optimizer must do to produce the hand-written loop: inline every `operator++`, `operator*` and `operator==`, hoist the cached `begin()`, and delete the empty-check branches. GCC and Clang do this for most pipelines of depth ≤ 4 to 5 over contiguous data, with **two systematic residual costs**: `filter` adds a *second* loop structure (the "find next" loop nests inside the iteration loop), and the **cached `begin()`** of `filter` / `drop` / `reverse` adds a small prologue (Experiment 4 shows both).

### The size of a pipeline

Each adaptor stores its base view *by value* and its callable *by value*. A lambda with no captures is an empty class (1 byte, but **`[[no_unique_address]]`** in libstdc++'s `_Movable_box` collapses it). So `v | filter(p) | transform(f) | take(5)` is typically **24–48 bytes**: a pointer pair, a counter, plus the cached iterator: *a view is a handful of words*. A capturing lambda adds its captures. A `std::function` as the callable adds 32 bytes **and** a virtual dispatch per element: avoid.

---

## 7. Experiments

### Experiment 1: The concept ladder, as a compilable table

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <deque>
#include <forward_list>
#include <istream>
#include <list>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rg = std::ranges;
namespace vw = std::views;

template <class R> const char* strongest() {
    if constexpr (rg::contiguous_range<R>)          return "contiguous";
    else if constexpr (rg::random_access_range<R>)  return "random_access";
    else if constexpr (rg::bidirectional_range<R>)  return "bidirectional";
    else if constexpr (rg::forward_range<R>)        return "forward";
    else if constexpr (rg::input_range<R>)          return "input";
    else                                            return "(not a range)";
}
template <class R> void row(const char* name) {
    std::printf("  %-34s %-14s sized=%d common=%d borrowed=%d view=%d const_iter=%d\n", name, strongest<R>(),
        rg::sized_range<R>, rg::common_range<R>, rg::borrowed_range<R>, rg::view<R>, rg::range<const R>);
}
#define ROW(...) row<__VA_ARGS__>(#__VA_ARGS__)

int main() {
    using V = std::vector<int>;
    auto even = [](int x) { return x % 2 == 0; };
    auto sq   = [](int x) { return x * x; };
    ROW(V);
    ROW(std::deque<int>);
    ROW(std::list<int>);
    ROW(std::forward_list<int>);
    ROW(std::map<int, int>);
    ROW(std::unordered_map<int, int>);
    ROW(std::string_view);
    ROW(std::span<int>);
    ROW(int[4]);
    ROW(rg::iota_view<int, int>);
    ROW(rg::iota_view<int>);                                              // unbounded: infinite
    ROW(decltype(std::declval<V&>() | vw::filter(even)));
    ROW(decltype(std::declval<V&>() | vw::transform(sq)));
    ROW(decltype(std::declval<V&>() | vw::take(3)));
    ROW(decltype(std::declval<V&>() | vw::take_while(even)));
    ROW(decltype(std::declval<V&>() | vw::reverse));
    ROW(decltype(std::declval<std::list<int>&>() | vw::reverse));
    ROW(decltype(std::declval<V&>() | vw::drop(2)));
    ROW(decltype(std::declval<V&>() | vw::stride(2)));
    ROW(decltype(std::declval<V&>() | vw::chunk(2)));
    ROW(decltype(std::declval<V&>() | vw::slide(2)));
    ROW(decltype(vw::zip(std::declval<V&>(), std::declval<V&>())));
    ROW(rg::basic_istream_view<int, char>);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
  V                                  contiguous     sized=1 common=1 borrowed=0 view=0 const_iter=1
  std::deque<int>                    random_access  sized=1 common=1 borrowed=0 view=0 const_iter=1
  std::list<int>                     bidirectional  sized=1 common=1 borrowed=0 view=0 const_iter=1
  std::forward_list<int>             forward        sized=0 common=1 borrowed=0 view=0 const_iter=1
  std::map<int, int>                 bidirectional  sized=1 common=1 borrowed=0 view=0 const_iter=1
  std::unordered_map<int, int>       forward        sized=1 common=1 borrowed=0 view=0 const_iter=1
  std::string_view                   contiguous     sized=1 common=1 borrowed=1 view=1 const_iter=1
  std::span<int>                     contiguous     sized=1 common=1 borrowed=1 view=1 const_iter=1
  int[4]                             contiguous     sized=1 common=1 borrowed=0 view=0 const_iter=1
  rg::iota_view<int, int>            random_access  sized=1 common=1 borrowed=1 view=1 const_iter=1
  rg::iota_view<int>                 random_access  sized=0 common=0 borrowed=1 view=1 const_iter=1
  decltype(std::declval<V&>() | vw::filter(even)) bidirectional  sized=0 common=1 borrowed=0 view=1 const_iter=0
  decltype(std::declval<V&>() | vw::transform(sq)) random_access  sized=1 common=1 borrowed=0 view=1 const_iter=1
  decltype(std::declval<V&>() | vw::take(3)) contiguous     sized=1 common=1 borrowed=1 view=1 const_iter=1
  decltype(std::declval<V&>() | vw::take_while(even)) contiguous     sized=0 common=0 borrowed=0 view=1 const_iter=1
  decltype(std::declval<V&>() | vw::reverse) random_access  sized=1 common=1 borrowed=1 view=1 const_iter=1
  decltype(std::declval<std::list<int>&>() | vw::reverse) bidirectional  sized=1 common=1 borrowed=1 view=1 const_iter=1
  decltype(std::declval<V&>() | vw::drop(2)) contiguous     sized=1 common=1 borrowed=1 view=1 const_iter=1
  decltype(std::declval<V&>() | vw::stride(2)) random_access  sized=1 common=1 borrowed=1 view=1 const_iter=1
  decltype(std::declval<V&>() | vw::chunk(2)) random_access  sized=1 common=1 borrowed=1 view=1 const_iter=1
  decltype(std::declval<V&>() | vw::slide(2)) random_access  sized=1 common=1 borrowed=1 view=1 const_iter=1
  decltype(vw::zip(std::declval<V&>(), std::declval<V&>())) random_access  sized=1 common=1 borrowed=1 view=1 const_iter=1
  rg::basic_istream_view<int, char>  input          sized=0 common=0 borrowed=0 view=1 const_iter=0
```

What to extract from the table:

- `filter` → no `sized`, **no `const_iter`**; `take_while` → no `common` (its end is a sentinel); `iota_view<int>` (unbounded) → not `sized`.
- `transform`, `take`, `reverse`, `stride`, `zip` **preserve** random access when the base has it.
- `std::list | reverse` is *bidirectional*: reversing doesn't upgrade a category.
- Only the *owning containers* are not `view`. Every adaptor result is.

### Experiment 2: A ladder of pipelines (and what each one lowers to)

```cpp
// @test run -std=c++23 -O0
#include <algorithm>
#include <cstdio>
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace rg = std::ranges;
namespace vw = std::views;

int main() {
    // 1. squares of the first 5 odd numbers (infinite source, lazily bounded)
    std::printf("1. ");
    for (int x : vw::iota(1) | vw::filter([](int n) { return n % 2; }) | vw::transform([](int n) { return n * n; }) | vw::take(5))
        std::printf("%d ", x);
    std::puts("");

    // 2. Pythagorean triples with a nested comprehension via cartesian_product (C++23)
    std::printf("2. ");
    int found = 0;
    for (auto [a, b, c] : vw::cartesian_product(vw::iota(1, 21), vw::iota(1, 21), vw::iota(1, 21)))
        if (a < b && a * a + b * b == c * c && found++ < 4) std::printf("(%d,%d,%d) ", a, b, c);
    std::puts("");

    // 3. split a CSV line; split yields SUBRANGES: convert explicitly
    std::string line = "ada,36,london,engineer";
    std::printf("3. ");
    for (auto field : line | vw::split(','))
        std::printf("[%s] ", std::string(field.begin(), field.end()).c_str());     // or: std::string_view(field) in C++23
    std::puts("");

    // 4. zip + enumerate: no index variable, no iterator juggling
    std::vector<std::string> names = {"ada", "alan", "grace"};
    std::vector<int> ages = {36, 41, 85};
    std::printf("4. ");
    for (auto [i, name, age] : vw::zip(vw::iota(0), names, ages))
        std::printf("%d:%s=%d ", i, name.c_str(), age);
    std::puts("");
    for (auto [i, name] : names | vw::enumerate) std::printf("   enumerate %zd -> %s", i, name.c_str());
    std::puts("");

    // 5. sliding windows & chunks: moving average, batching
    std::vector<int> data = {1, 2, 3, 4, 5, 6, 7, 8};
    std::printf("5. slide(3) sums: ");
    for (auto w : data | vw::slide(3)) { int s = 0; for (int x : w) s += x; std::printf("%d ", s); }
    std::printf("\n   chunk(3) sizes: ");
    for (auto c : data | vw::chunk(3)) std::printf("%zu ", rg::size(c));
    std::puts("");

    // 6. group runs with chunk_by, then join_with into a string
    std::vector<int> runs = {1, 1, 2, 2, 2, 3, 1, 1};
    std::printf("6. runs: ");
    for (auto g : runs | vw::chunk_by(std::equal_to{})) std::printf("%dx%zu ", *g.begin(), rg::size(g));
    std::vector<std::string> words = {"to", "be", "or", "not"};
    auto joined = words | vw::join_with(' ') | rg::to<std::string>();
    std::printf("\n   join_with: \"%s\"\n", joined.c_str());

    // 7. a map | keys/values; stride; reverse; fold
    std::map<std::string, int> score = {{"ada", 9}, {"alan", 7}, {"grace", 10}};
    std::printf("7. keys: ");
    for (auto& k : score | vw::keys) std::printf("%s ", k.c_str());
    std::printf("\n   total (fold_left): %d   every 3rd of 1..10: ", rg::fold_left(score | vw::values, 0, std::plus{}));
    for (int x : vw::iota(1, 11) | vw::stride(3)) std::printf("%d ", x);
    std::puts("");

    // 8. materialize at the end, and the "eager" algorithms
    auto evens_desc = vw::iota(1, 11) | vw::filter([](int x) { return x % 2 == 0; }) | vw::reverse | rg::to<std::vector>();
    std::printf("8. to<vector>: ");
    for (int x : evens_desc) std::printf("%d ", x);
    std::puts("");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
1. 1 9 25 49 81 
2. (3,4,5) (5,12,13) (6,8,10) (8,15,17) 
3. [ada] [36] [london] [engineer] 
4. 0:ada=36 1:alan=41 2:grace=85 
   enumerate 0 -> ada   enumerate 1 -> alan   enumerate 2 -> grace
5. slide(3) sums: 6 9 12 15 18 21 
   chunk(3) sizes: 3 3 2 
6. runs: 1x2 2x3 3x1 1x2 
   join_with: "to be or not"
7. keys: ada alan grace 
   total (fold_left): 26   every 3rd of 1..10: 1 4 7 10 
8. to<vector>: 10 8 6 4 2 
```

Compare number 2 with the pre-C++20 spelling: three nested `for` loops and a counter. They compile to the same nest after inlining; the version above is **declarative** and the ranges can be reused or reordered. Number 3 shows the most common surprise: `views::split` yields **subranges of the original**, not `std::string`s: no copy happens until you ask for one (`std::string_view(field)` works in C++23 via the range constructor).

### Experiment 3: Sentinels, in practice

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <iterator>
#include <ranges>

namespace rg = std::ranges;

// A range over a C string that never calls strlen: the end is a *sentinel*, not an iterator.
struct NulSentinel { bool operator==(const char* p) const { return *p == '\0'; } };

struct CStringRange {
    const char* s;
    const char* begin() const { return s; }
    NulSentinel end()   const { return {}; }
};

int main() {
    CStringRange str{"sentinel!"};
    static_assert(rg::range<CStringRange>);
    static_assert(!rg::common_range<CStringRange>);     // begin() and end() have different types
    static_assert(!rg::sized_range<CStringRange>);

    int n = 0;
    for (char c : str) { std::putchar(c); ++n; }         // range-for supports sentinels since C++17
    std::printf("   (%d chars, length never computed up front)\n", n);

    // composes with views: take_while and transform work on a non-common range
    for (char c : str | std::views::take(4) | std::views::transform([](char c) { return char(c - 32); })) std::putchar(c);
    std::puts("");

    // unreachable_sentinel: the "infinite range" end: the comparison is always false, so the check disappears
    auto first_square_over_1000 = *rg::find_if(std::views::iota(1), [](int x) { return x * x > 1000; });
    std::printf("first x with x*x > 1000: %d (searched an unbounded iota)\n", first_square_over_1000);

    // counted_iterator + default_sentinel: iterate n elements of a single-pass source
    const char* text = "counted range";
    for (auto it = std::counted_iterator(text, 7); it != std::default_sentinel; ++it) std::putchar(*it);
    std::puts("");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sentinel!   (9 chars, length never computed up front)
SENT
first x with x*x > 1000: 32 (searched an unbounded iota)
counted
```

`ranges::find_if` over `iota(1)` terminates only because the *predicate* eventually succeeds; the **sentinel is `unreachable_sentinel`**, so the loop has no end-check at all (the optimizer drops the comparison). That is the practical meaning of a sentinel: *the end test can be a different, cheaper or even absent check*.

### Experiment 4: Is the pipeline free? Assembly and timing

First the code. The pipeline `v | filter(x%3==0) | transform(x*x)` summed, against the hand-written loop (shown in the planning probe and re-measured here):

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fcf-protection=none filter=pipeline,handloop
#include <ranges>
#include <vector>
namespace vw = std::views;

long pipeline(const std::vector<int>& v) {
    long s = 0;
    for (int x : v | vw::filter([](int x) { return x % 3 == 0; }) | vw::transform([](int x) { return x * x; })) s += x;
    return s;
}
long handloop(const std::vector<int>& v) {
    long s = 0;
    for (int x : v) if (x % 3 == 0) s += x * x;
    return s;
}
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
pipeline(std::vector<int, std::allocator<int> > const&):
	mov	r8, QWORD PTR 8[rdi]
	mov	rsi, QWORD PTR [rdi]
	cmp	r8, rsi
	jne	.L4
	jmp	.L8
.L15:
	add	rsi, 4
	cmp	r8, rsi
	je	.L8
.L4:
	mov	ecx, DWORD PTR [rsi]
	imul	eax, ecx, -1431655765
	add	eax, 715827882
	cmp	eax, 1431655764
	ja	.L15
	xor	edi, edi
.L13:
	cmp	rsi, r8
	je	.L1
	imul	ecx, ecx
	lea	rax, 4[rsi]
	movsx	rcx, ecx
	add	rdi, rcx
	cmp	r8, rax
	je	.L1
.L6:
	mov	ecx, DWORD PTR [rax]
	mov	rsi, rax
	imul	edx, ecx, -1431655765
	add	edx, 715827882
	cmp	edx, 1431655764
	jbe	.L13
	add	rax, 4
	cmp	r8, rax
	jne	.L6
.L1:
	mov	rax, rdi
	ret
.L8:
	xor	edi, edi
	mov	rax, rdi
	ret

handloop(std::vector<int, std::allocator<int> > const&):
	mov	rdx, QWORD PTR [rdi]
	mov	rsi, QWORD PTR 8[rdi]
	xor	edi, edi
	cmp	rsi, rdx
	je	.L16
.L19:
	mov	eax, DWORD PTR [rdx]
	imul	ecx, eax, -1431655765
	add	ecx, 715827882
	cmp	ecx, 1431655764
	ja	.L18
	imul	eax, eax
	cdqe
	add	rdi, rax
.L18:
	add	rdx, 4
	cmp	rsi, rdx
	jne	.L19
.L16:
	mov	rax, rdi
	ret
```

Read it as a **qualified yes**: no calls, no allocation, the lambdas are inlined, and the `x % 3` is compiled to the multiplication-by-inverse trick in both. But the pipeline is about **twice as long** in instructions: `filter` becomes a *find-next-match loop nested in the consumption loop* (the `.L6`/`.L13` pair), plus a prologue that locates the first match (the cached `begin()`). The hand loop is a single loop with a conditional add. Is the nested structure slower? Measure (the benchmark isolates each side in a `noinline` function; see the trap noted below):

```cpp
// @test run -std=c++23 -O2
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <ranges>
#include <random>
#include <vector>

namespace vw = std::views;

[[gnu::noinline]] long loop_fn(const std::vector<int>& v) { long s = 0; for (int x : v) if (x % 3 == 0) s += x * x; return s; }
[[gnu::noinline]] long pipe_fn(const std::vector<int>& v) { long s = 0; for (int x : v | vw::filter([](int x) { return x % 3 == 0; }) | vw::transform([](int x) { return x * x; })) s += x; return s; }

template <class F> double best_ms(F&& f, long& sink, int reps = 7) {
    double best = 1e30;
    for (int i = 0; i < reps; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        sink += f();
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    return best;
}

int main() {
    constexpr int N = 20'000'000;
    std::vector<int> v(N);
    std::mt19937 rng(1);
    for (auto& x : v) x = int(rng() % 1000);

    long sink = 0;
    double t_loop = best_ms([&] { return loop_fn(v); }, sink);
    double t_pipe = best_ms([&] { return pipe_fn(v); }, sink);
    double t_algo = best_ms([&] { return std::transform_reduce(v.begin(), v.end(), 0L, std::plus{}, [](int x) { return x % 3 == 0 ? long(x) * x : 0L; }); }, sink);

    std::printf("sum of squares of multiples of 3, N=%d (best of 7)\n", N);
    std::printf("  hand loop              %7.2f ms\n", t_loop);
    std::printf("  ranges pipeline        %7.2f ms   (%.2fx)\n", t_pipe, t_pipe / t_loop);
    std::printf("  transform_reduce       %7.2f ms   (%.2fx)\n", t_algo, t_algo / t_loop);

    // A pipeline that does NOT fuse well: join of many small ranges
    std::vector<std::vector<int>> nested(2'000'000, std::vector<int>(10, 3));
    double j_loop = best_ms([&] { long s = 0; for (auto& in : nested) for (int x : in) s += x; return s; }, sink, 5);
    double j_pipe = best_ms([&] { long s = 0; for (int x : nested | vw::join) s += x; return s; }, sink, 5);
    std::printf("flatten + sum 2e6 inner vectors of 10\n  nested loops           %7.2f ms\n  views::join            %7.2f ms   (%.2fx)\n", j_loop, j_pipe, j_pipe / j_loop);
    return sink == 42 ? 1 : 0;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sum of squares of multiples of 3, N=20000000 (best of 7)
  hand loop                86.40 ms
  ranges pipeline          83.70 ms   (0.97x)
  transform_reduce         79.12 ms   (0.92x)
flatten + sum 2e6 inner vectors of 10
  nested loops             14.29 ms
  views::join              14.45 ms   (1.01x)
```

*How to read it.* With both loops compiled as separate `noinline` functions, the **filter+transform pipeline runs at the same speed as the hand loop** (about 1.0x; the unpredictable `x % 3` branch dominates both), and `views::join` over 2×10⁶ small vectors is also on par with nested loops (about 1.0x). Despite the longer assembly above, the extra structure costs nothing measurable here.

> **A benchmarking trap I fell into while writing this.** My first version timed the hand loop as a lambda inlined into `main`, and the pipeline came out **3.5× slower**. The pipeline itself had not changed; I did not investigate exactly how the compiler treated the inlined baseline differently (a good exercise with `-S`). Once both sides were isolated in `noinline` functions the gap disappeared. Moral: *a microbenchmark compares two compiler outputs, not two abstractions*; always read the assembly of both sides (Chapter 40). Other caveats: GCC 14 does not vectorize either loop at `-O2` (the `long` accumulator and the data-dependent branch), single machine, no frequency pinning.

The reliable rule: *use pipelines for clarity everywhere; in the one hot loop that a profile points at, compare the pipeline against the hand loop in isolation before accepting or rejecting it.*

### Experiment 5: Projections replace comparator lambdas

```cpp
// @test run -std=c++23 -O0
#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace rg = std::ranges;

struct Person { std::string name; int age; double score() const { return age * 1.5; } };

int main() {
    std::vector<Person> v = {{"grace", 85}, {"ada", 36}, {"alan", 41}, {"edsger", 72}};

    rg::sort(v, {}, &Person::age);                                         // member data pointer as projection
    std::printf("by age     : "); for (auto& p : v) std::printf("%s ", p.name.c_str()); std::puts("");

    rg::sort(v, std::greater{}, &Person::name);                            // custom comparator + projection
    std::printf("name desc  : "); for (auto& p : v) std::printf("%s ", p.name.c_str()); std::puts("");

    rg::sort(v, {}, &Person::score);                                       // member FUNCTION pointer as projection
    std::printf("by score() : "); for (auto& p : v) std::printf("%s ", p.name.c_str()); std::puts("");

    auto it = rg::find(v, "alan", &Person::name);                          // find by projected value
    std::printf("find alan  : age %d\n", it->age);

    auto oldest = rg::max_element(v, {}, &Person::age);
    std::printf("max age    : %s\n", oldest->name.c_str());

    auto [lo, hi] = rg::minmax(v, {}, [](const Person& p) { return p.name.size(); });   // lambda projection
    std::printf("shortest/longest name: %s / %s\n", lo.name.c_str(), hi.name.c_str());

    // The projection is applied for the COMPARISON only: the sort still moves whole Person objects.
    auto count = rg::count_if(v, [](int a) { return a > 40; }, &Person::age);           // predicate sees int, not Person
    std::printf("older than 40: %td\n", count);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
by age     : ada alan edsger grace 
name desc  : grace edsger alan ada 
by score() : ada alan edsger grace 
find alan  : age 41
max age    : grace
shortest/longest name: ada / edsger
older than 40: 3
```

`&Person::age` is a pointer-to-member; the algorithm calls it through `std::invoke` (Chapter 7's `invoke`), which is why a data member, a member function and a lambda are all valid projections. The comparison `lo`/`hi` returns **`min_max_result<const Person&>`**: structured bindings work with it.

### Experiment 6: Build your own view and adaptor (so that `|` works)

A *view* is a class with `begin`/`end` that derives from `view_interface`; an *adaptor closure* is an object with `operator()(range)` plus a pipe operator. We implement `every_nth(n)`: yield every n-th element (what `views::stride` does).

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <iterator>
#include <ranges>
#include <vector>

namespace rg = std::ranges;

template <rg::input_range V> requires rg::view<V>
class every_nth_view : public rg::view_interface<every_nth_view<V>> {
    V base_;
    std::ptrdiff_t n_;

    class iterator {
        rg::iterator_t<V> cur_;
        rg::sentinel_t<V> end_;
        std::ptrdiff_t n_;
    public:
        using value_type        = rg::range_value_t<V>;
        using difference_type   = std::ptrdiff_t;
        using iterator_category = std::input_iterator_tag;
        iterator() = default;
        iterator(rg::iterator_t<V> cur, rg::sentinel_t<V> end, std::ptrdiff_t n) : cur_(cur), end_(end), n_(n) {}

        decltype(auto) operator*() const { return *cur_; }
        iterator& operator++()   { for (std::ptrdiff_t i = 0; i < n_ && cur_ != end_; ++i) ++cur_; return *this; }
        void operator++(int)     { ++*this; }
        friend bool operator==(const iterator& a, const iterator& b) { return a.cur_ == b.cur_; }
        friend bool operator==(const iterator& a, std::default_sentinel_t) { return a.cur_ == a.end_; }
    };

public:
    every_nth_view() = default;
    every_nth_view(V base, std::ptrdiff_t n) : base_(std::move(base)), n_(n) {}

    auto begin() { return iterator(rg::begin(base_), rg::end(base_), n_); }
    auto end()   { return std::default_sentinel; }                       // a sentinel: non-common range
};
template <class R> every_nth_view(R&&, std::ptrdiff_t) -> every_nth_view<std::views::all_t<R>>;

// ---- the adaptor closure: holds n; `range | every_nth(3)` calls operator() ----
struct every_nth_closure {
    std::ptrdiff_t n;
    template <rg::viewable_range R>
    auto operator()(R&& r) const { return every_nth_view(std::forward<R>(r), n); }
    template <rg::viewable_range R>
    friend auto operator|(R&& r, const every_nth_closure& c) { return c(std::forward<R>(r)); }
};
inline every_nth_closure every_nth(std::ptrdiff_t n) { return {n}; }

static_assert(rg::input_range<every_nth_view<std::views::all_t<std::vector<int>&>>>);
static_assert(rg::view<every_nth_view<std::views::all_t<std::vector<int>&>>>);

int main() {
    std::vector<int> v = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    for (int x : v | every_nth(3)) std::printf("%d ", x);                // 0 3 6 9
    std::puts("");

    // composes with the standard adaptors on both sides:
    for (int x : v | std::views::transform([](int x) { return x * 10; }) | every_nth(4) | std::views::take(2))
        std::printf("%d ", x);                                           // 0 40
    std::puts("");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
0 3 6 9 
0 40 
```

What the implementation shows about the machinery:

1. `view_interface<D>` (CRTP, Chapter 20) supplies `empty()`, `front()`, `operator bool` and `operator[]` from your `begin`/`end`.
2. `views::all_t<R>` is how the *deduction guide* turns an lvalue container into a `ref_view` (a view that references it) and an rvalue container into an `owning_view`.
3. The closure's `operator|` is what makes `r | every_nth(3)` work. The standard provides `std::ranges::range_adaptor_closure<D>` (C++23) to supply this and enable **closure composition** (`every_nth(3) | vw::take(2)` as a reusable closure); we wrote the pipe by hand to show there is no magic.
4. The **sentinel** (`std::default_sentinel`) is why the view is *not* a `common_range`: pass it to `std::accumulate(v.begin(), v.end())` and it will not compile; wrap with `views::common`.

### Experiment 7: Where the abstraction leaks

```cpp
// @test run -std=c++23 -O0
#include <algorithm>
#include <cstdio>
#include <numeric>
#include <ranges>
#include <vector>

namespace rg = std::ranges;
namespace vw = std::views;

int main() {
    std::vector<int> v = {5, 3, 8, 1, 9, 2};

    // (1) A non-common range cannot feed a legacy iterator-pair algorithm:
    auto tw = v | vw::take_while([](int x) { return x != 9; });
    static_assert(!rg::common_range<decltype(tw)>);
    // std::accumulate(tw.begin(), tw.end(), 0);                    // ERROR: begin/end have different types
    std::printf("(1) accumulate via views::common: %d\n", std::accumulate((tw | vw::common).begin(), (tw | vw::common).end(), 0));

    // (2) size() is not always available, and distance is O(n) when it is not:
    auto f = v | vw::filter([](int x) { return x > 2; });
    static_assert(!rg::sized_range<decltype(f)>);
    std::printf("(2) filter has no size(); ranges::distance walks it: %td\n", rg::distance(f));

    // (3) A view of an lvalue shows later changes to the source (it's a reference):
    auto big = v | vw::filter([](int x) { return x > 4; });
    v[1] = 100;                                                     // modify the source after building the view (begin() not yet cached)
    std::printf("(3) sees later change: ");
    for (int x : big) std::printf("%d ", x);
    std::puts("");

    // (4) ...but MUTATING THE SOURCE'S STRUCTURE invalidates iterators held by the view/cached begin:
    //     v.push_back(...) after big.begin() was called is UB for the cached position.

    // (5) transform yields prvalues: `for (int& x : v | vw::transform(f))` does not compile
    //     (cannot bind int& to an rvalue). Views that write through (keys, values, take, filter) yield references.
    for (int& x : v | vw::take(2)) x += 1;
    std::printf("(5) wrote through take: %d %d\n", v[0], v[1]);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
(1) accumulate via views::common: 17
(2) filter has no size(); ranges::distance walks it: 4
(3) sees later change: 5 100 8 9 
(5) wrote through take: 6 101
```

Point (5): `transform` yields **values**, so `int& x : v | vw::transform(f)` is a compile error (try it); adaptors that pass elements through (`take`, `filter`, `keys`, `values`) yield references and write through.

---

## 8. Assembly / runtime investigation

Experiment 4's assembly is the investigation. Three further questions to put to your own pipelines:

```bash
# (1) did it vectorize?  (look for packed instructions: paddd, vpaddd, pmulld …)
g++-14 -std=c++23 -O3 -S -masm=intel -o - prog.cpp | c++filt | grep -E 'paddd|pmulld|vpadd|vpmul'
g++-14 -std=c++23 -O3 -fopt-info-vec-missed 2>&1 | grep 'prog.cpp:LINE'      # why it did NOT

# (2) is anything left out of line?  A pipeline of depth 4 that still has `call` inside the loop means inlining was refused:
g++-14 -std=c++23 -O2 -S -masm=intel -o - prog.cpp | c++filt | awk '/^pipeline/,/ret$/' | grep -c call

# (3) how big are the types?   (and the template instantiation depth)
echo 'int main(){ auto p = std::vector<int>{} | std::views::filter([](int){return 1;}) | std::views::take(2); static_assert(sizeof(p) == 0); }' ...
```

**What vectorizes.** `transform` + `reduce`-like loops over contiguous data usually vectorize at `-O3` (and often `-O2` in GCC 14); **`filter` tends to block auto-vectorization** because the predicate controls a data-dependent store/skip (the loop is not countable). A branch-free formulation (`transform` to a 0/1 mask and multiply) can vectorize where `filter` cannot. That is a *data-oriented design* decision (Chapter 27), not a library problem.

---

## 9. Implementation exercise

Implement the following pieces, each as a conforming view with a closure object, and verify each with `static_assert(std::ranges::view<…>)` and the concept ladder from Experiment 1:

1. **`every_nth`** (done above) → upgrade it to a **forward / random access** iterator when the base supports it (`iterator_category` from the base; `operator+=`; `size()` when sized).
2. **`take_last(n)`** for bidirectional ranges: a view of the last n elements, in O(1) `begin()` for random-access and O(n) for bidirectional (document it).
3. **`cycle`**: repeat a (forward) range forever (an infinite view with an `unreachable_sentinel`), and combine with `views::take` and `views::zip` for the classic round-robin.
4. **`scan(f, init)`**: lazy running fold (prefix sums), yielding `f(acc, x)` for each element as a *transform with state* (note why a stateful `transform` lambda is **not allowed**: `regular_invocable` requires equality-preserving calls!).
5. **A `generator`-based variant** of (3) using `std::generator<T>`, and compare the generated code and the semantics (input range only, single pass, a coroutine frame allocation).

<details>
<summary><strong>Solution sketch for `scan` (prefix sums), with the purity discussion</strong></summary>

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <functional>
#include <iterator>
#include <ranges>
#include <vector>

namespace rg = std::ranges;

template <rg::input_range V, class T, class F> requires rg::view<V>
class scan_view : public rg::view_interface<scan_view<V, T, F>> {
    V base_; T init_; F f_;
    struct iterator {
        using value_type = T; using difference_type = std::ptrdiff_t; using iterator_category = std::input_iterator_tag;
        rg::iterator_t<V> it_{}; const scan_view* parent_ = nullptr; T acc_{};
        iterator() = default;
        iterator(rg::iterator_t<V> it, const scan_view* p) : it_(std::move(it)), parent_(p), acc_(p->init_) { if (it_ != rg::end(const_cast<V&>(p->base_))) acc_ = std::invoke(p->f_, acc_, *it_); }
        const T& operator*() const { return acc_; }
        iterator& operator++() { ++it_; if (it_ != rg::end(const_cast<V&>(parent_->base_))) acc_ = std::invoke(parent_->f_, acc_, *it_); return *this; }
        void operator++(int) { ++*this; }
        friend bool operator==(const iterator& a, std::default_sentinel_t) { return a.it_ == rg::end(const_cast<V&>(a.parent_->base_)); }
    };
public:
    scan_view() = default;
    scan_view(V b, T init, F f) : base_(std::move(b)), init_(std::move(init)), f_(std::move(f)) {}
    auto begin() const { return iterator(rg::begin(const_cast<V&>(base_)), this); }
    auto end() const { return std::default_sentinel; }
};
template <class R, class T, class F> scan_view(R&&, T, F) -> scan_view<std::views::all_t<R>, T, F>;

int main() {
    std::vector<int> v = {1, 2, 3, 4, 5};
    for (int s : scan_view(v, 0, std::plus{})) std::printf("%d ", s);   // 1 3 6 10 15
    std::puts("");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
1 3 6 10 15 
```

Why not `transform` with a mutable lambda that carries `acc`? `std::views::transform` requires its function to be `regular_invocable`: *calling it with equal arguments must give equal results, and must not change anything observable*. A running total violates that: the second pass over the same view sees different values. The standard library does not enforce it (it can't, syntactically); you'd get a view whose second traversal differs, and a **second `begin()`** would double-count. `scan_view` holds `acc_` **in the iterator**, so every traversal starts fresh. The state belongs in the *iterator*, not in the *function object*.

</details>

---

## 10. Real-world example

| Where | How ranges are used |
|---|---|
| **Data-processing code** | `lines | views::transform(parse) | views::filter(valid) | ranges::to<std::vector>()` replaces 40 lines of loops and temporaries |
| **Game/engine code** | `zip` over SoA columns (`positions`, `velocities`): the idiomatic way to iterate parallel arrays (Chapter 27) |
| **Parsers** | `views::split`/`chunk_by`/`take_while` over `string_view` + `std::from_chars` |
| **Libraries** | `std::format("{}", views::join_with(…))`; `std::print` ranges formatting (C++23: `std::format("{}", vec)` prints `[1, 2, 3]`) |
| **Compilers/tools** | LLVM's `llvm::make_filter_range`, `zip`, `enumerate` (pre-C++20 equivalents); Eigen/oneTBB ranges (`blocked_range`: a *range* in the parallel-algorithm sense, a different but related concept) |
| **Qt** | Qt containers are ranges (`QList`, `QMap` provide `begin/end` and `size`); `QSpan`/`QRangeModel` (Qt 6.10) and `QtConcurrent` work with iterator pairs: use `ranges::to<QList<int>>()`? Needs `QList(first, last)` constructor and `ranges::to` support: check your Qt version |

> **Opinion.** Ranges are the biggest quality-of-life improvement in modern C++ since lambdas. **Use the `std::ranges::` algorithms everywhere** (better diagnostics, projections, return values; free of cost). **Use `views::` pipelines for transformations that read like a description**, and **always `ranges::to<std::vector>` before you traverse more than once**. Be skeptical of pipelines in the *single hottest loop*: measure against the loop. And don't return a pipeline type from an API: the type is unspeakable, unstable, and pins every caller to your implementation; return a container or a concrete view type you control.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Taking `const R&` in a generic range function | `filter_view`, `drop_while`, `split` rejected | `std::ranges::range auto&& r`, and `std::forward<decltype(r)>(r)` when passing on |
| Traversing a lazy pipeline repeatedly | Predicates/transforms rerun every pass; side effects repeat | `ranges::to<std::vector>()` once |
| Stateful lambda in `transform`/`filter` | Different results on re-traversal; UB-adjacent (violates `regular_invocable`) | Put state in an iterator; or materialize |
| Returning a view of a local container | Dangling `ref_view` | Return the container, or `ranges::to` |
| `views::split` expecting strings | Gets subranges; `std::string(sub)` was ill-formed before C++23 | `std::string(sub.begin(), sub.end())`, or `std::string_view(sub)` (C++23), or `ranges::to<std::string>()` |
| Passing a non-common range to a legacy algorithm | "no matching function" with a mismatched-iterator-type note | `views::common`, or use the `ranges::` version |
| `std::ranges::sort(list)` | Constraint error `random_access_range` | `list.sort()`; or copy to a `vector` |
| Mutating the source container's *structure* while a view exists | Dangling iterators, cached `begin()` stale | Finish with the view first |
| `auto x = v | views::filter(p)` then `x.size()` | No such member | `ranges::distance(x)` (O(n)) |
| Using a `std::function` as an adaptor argument | Virtual call per element; blocks inlining | A lambda or function object |
| Deep nesting of adaptors with `join` over prvalue ranges | The result is only an *input* range (lazy inner ranges) | Materialize the inner ranges, or restructure |
| Assuming `views::zip` yields references | It yields a *tuple of references* (a prvalue proxy): `for (auto& [a, b] : zip(...))` doesn't compile | `for (auto&& [a, b] : …)` |
| Assuming a C++23 view exists on every compiler | `views::enumerate`/`chunk_by`/`concat` missing | Check the feature-test macros (`__cpp_lib_ranges_enumerate`, …) and the compiler-support page |
| Returning `auto` pipeline from a `.h` function in a library | The type leaks into the ABI and compile times | A concrete return type; or `std::generator`/`ranges::to` |
| Using `views::iota(0, n)` with mismatched types (`int` vs `size_t`) | Wrong common type; sign-compare surprises | `views::iota(std::size_t{0}, n)` |

---

## 12. Exercises

1. **Hierarchy table.** Extend Experiment 1 with ten more types/adaptors of your choice (`views::keys`, `views::split`, `std::generator`, `views::join`, `std::string`, a C array of `char`, `rg::subrange`, …). Predict the whole row before running.
2. **Rewrite.** Convert five iterator-pair loops from your code base (or from Chapter 11's exercises) to ranges: for each compare compile time (`-ftime-report`), binary size (`size`), and run time.
3. **Pipeline or loop?** For a pipeline of your choice (e.g. word frequency over a 100 MB text: `split | transform(lower) | filter(alpha) | ...`), measure pipeline vs hand loop at `-O2`/`-O3`. Where does the pipeline lose? Check the assembly of the hottest loop.
4. **Sentinels.** Write a sentinel-terminated range over a null-terminated array of `char*` (like `argv` or `environ`) and a **line range** over a `FILE*`/`std::istream` that ends at EOF. Verify both with `std::ranges::input_range` and compose them with `transform`.
5. **View laws.** For `every_nth`, write a test that checks the *copy is O(1)*, the *iterator is default-constructible*, and a copied iterator is **independent** (forward-range semantics): which of the three can you promise for an input-category view?
6. **Eager vs lazy.** Implement `top_k(range, k)` three ways: sort + take, `partial_sort_copy`, and a lazy heap-based view. Compare complexity and memory; which one is a *view*, and why can't the other two be?
7. **`zip` and structure of arrays.** For a struct-of-arrays particle system (`x[], y[], vx[], vy[]`), write `step(dt)` with `views::zip` and `ranges::for_each`. Compare codegen with the index loop; does it vectorize? (Chapter 27.)
8. **Compile-time cost.** Chain 1, 2, 4, 8, 16 `views::transform` adaptors and measure compile time and the length of the demangled type name. Where does it become a problem?
9. **`generator` vs view.** Implement `fibonacci` as (a) a handwritten view with stateful iterator, (b) `std::generator<long>`, (c) a plain function returning `std::vector`. Compare lines of code, readability, allocation count (counting `operator new`), and run time for the first 10⁶ numbers.

---

## 13. Challenge: a lazy log-analysis pipeline

Given a 2 GB text log (memory-mapped), one entry per line `TIMESTAMP LEVEL SERVICE MESSAGE…`:

- produce, **without materializing lines**, the 10 most frequent `(SERVICE, LEVEL=ERROR)` pairs in the window `[t0, t1)` using: `string_view` over the mmap, `views::split('\n')`, `views::transform(parse_line)` returning `std::optional<Entry>` (with `string_view` fields), `views::filter` (engaged and in window and level == ERROR), `views::transform(service)`, and a *counting* step into `std::unordered_map<std::string_view, std::size_t>` (heterogeneous lookup!)
- a **parallel** version splitting the mmap into N chunks aligned to newline boundaries (`views::chunk` isn't enough: write the boundary logic), with `std::execution::par` or a thread pool (Project 6), then merging maps
- measure: bytes/s, allocations (counting `operator new`; the target is *zero allocations in the scan*), and compare to `grep | awk | sort | uniq -c`
- show, with `perf`, where the time goes: parsing, hashing, cache misses. Is the pipeline abstraction visible in the profile?
- write down every **lifetime obligation** (the mmap outlives every `string_view`; the map's keys view into it)

---

## 14. Knowledge check

1. What are the three requirements of `std::ranges::range`, and how does `borrowed_range` differ from `view`?
2. Why is `end()` allowed to have a different type than `begin()`? Give three uses.
3. What property of a view makes `std::views::filter` not `const`-iterable?
4. What is a *projection*, and how is it invoked? Give an example where it replaces a lambda.
5. Why can a `std::ranges::` algorithm not be found by ADL or specialized by users?
6. What does `views::all` produce for an lvalue vector, for an rvalue vector, and for a view?
7. What are `common_range` and `views::common` for?
8. Explain why a `transform` lambda that mutates captured state is a bug, even if it "works" the first time.
9. Which C++23 adaptors would you use for: moving average; batching into groups of 100; pairing two columns; nested loops; running index?
10. How does a `filter` pipeline's generated code differ from the hand loop, and how would you check that the difference matters?
11. What does `ranges::to<std::vector>()` do that nothing else in a pipeline does?
12. What does a range pipeline's *type* imply for API design?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. `ranges::begin(r)` and `ranges::end(r)` are valid expressions (by member, ADL or array rules). `borrowed_range`: iterators remain valid after the range object is destroyed (an lvalue or an `enable_borrowed_range` type); `view`: O(1) move (and copy if copyable), by opt-in. They are independent properties.
2. So `end` can be a *sentinel* that is only comparable with the iterator. Uses: null-terminated strings (no `strlen`), unbounded ranges (`unreachable_sentinel`: the end check vanishes), predicate-terminated ranges (`take_while`), counted ranges (`default_sentinel`).
3. `begin()` is cached on first call (to make it amortized O(1)); that requires mutating the view, so `begin()` is non-const only.
4. A unary callable applied to each element before the algorithm uses it (invoked by `std::invoke`): `ranges::sort(v, {}, &Person::age)` replaces `[](auto& a, auto& b){ return a.age < b.age; }`. It works with data-member pointers, member-function pointers, lambdas.
5. They are **function objects** (niebloids/CPOs), not function templates: name lookup finds an object, not a function, so ADL doesn't apply; this prevents user overloads from hijacking the algorithm and from changing behaviour depending on includes. Customize the *operations they use* (iterators, `swap`, `begin/end`), not the algorithm.
6. `ref_view<R>` for an lvalue range (a reference), `owning_view<R>` for an rvalue range (it takes ownership; C++20 after P2415), the view itself for a view (no wrapper).
7. `common_range`: `begin` and `end` have the same type. Legacy iterator-pair algorithms (`std::accumulate`, container constructors from iterators, `std::sort(first, last)`) need that; `views::common` wraps a non-common range into a common one (at the cost of a branch in the iterator).
8. `transform` requires `regular_invocable`: equality-preserving. A stateful lambda makes results depend on how many times and in what order elements are visited: a second traversal, `begin()` called twice, or a copy of the view gives different results (and `filter`/`take` evaluate elements an unspecified number of times). State belongs in an iterator or an explicit fold.
9. `slide(n)` (or `adjacent<n>`); `chunk(100)`; `zip`; `cartesian_product`; `enumerate` (or `zip(iota, r)`).
10. Longer code (a nested find-next loop plus a cached-begin prologue) and, in general, no auto-vectorization through `filter`; but in the measured case above it ran at the same speed as the hand loop, as did `views::join`. Never assume either way: isolate both sides and measure.
11. It's the **eager** step: it evaluates the lazy pipeline and **stores the elements in a container**, so later passes don't recompute and random access is available.
12. The type is unspeakable, depends on your implementation (change the pipeline, change the type, change the ABI), and ties compile time to every caller; return a container, a concrete view type you control, or type-erase.

</details>

---

[← Previous: Chapter 13](../part-05-standard-library/13-optional-variant-any-expected.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 15 — Algorithms and customization points →](15-algorithms-and-customization.md)
