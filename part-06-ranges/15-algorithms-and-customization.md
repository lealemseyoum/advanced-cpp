# Chapter 15 — Algorithms and Customization Points

> **Part VI · Ranges** &nbsp;|&nbsp; **Level 4** (compiler/library mechanics) &nbsp;|&nbsp; **≈ 6 hours**
> **Prerequisites:** [Chapter 7](../part-03-value-categories-and-move/07-perfect-forwarding.md) (`invoke`), [Chapter 10](../part-04-generic-programming/10-concepts.md), [Chapter 14](14-ranges.md) &nbsp;|&nbsp; **Standards:** C++11 (ADL idiom), C++20 (CPOs, iterator concepts, `ranges::` algorithms) &nbsp;|&nbsp; **Tools:** `g++-14`, `nm`

[← Previous: Chapter 14](14-ranges.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 16 — constexpr →](../part-07-compile-time/16-constexpr.md)

---

**In one sentence:** generic code must call operations (`swap`, `begin`, `size`) that *user types* get to define, and C++ spent twenty years refining *how* a library finds those operations safely — from the `using std::swap;` incantation to **customization point objects** (CPOs).

**By the end of this chapter you can:**

- explain ADL and the *two-step* idiom, and exactly how it breaks
- say what a customization point object is, and why `std::ranges::swap` is an object rather than a function
- implement your own CPO with member / ADL / default fallbacks
- state the iterator concept refinements and `std::indirectly_*` families
- explain why `std::ranges::` algorithms are "niebloids" and what that forbids
- design a customization mechanism for your own library

---

## 1. Problem

Generic code needs operations on types it knows nothing about:

```cpp
template <class T> void reverse_pair(T& a, T& b) {
    swap(a, b);        // which swap?  std::swap<T>?  the one for MyType in MyType's namespace?
}
```

Two requirements pull in opposite directions:

| Requirement | Pulls toward |
|---|---|
| A user type may provide a **better** `swap` (cheap pointer swap, not three moves) | Look in the *user's* namespace |
| A type that provides nothing must still work | Fall back to `std::swap` |
| A broken or mismatched user `swap` must not silently hijack generic code | Check the operation, constrain it |
| Generic code must not depend on what headers happen to be included | Lookup must be predictable |

The pre-C++20 solution satisfied the first two and violated the last two.

---

## 2. Historical context

| Era | Mechanism | Weakness |
|---|---|---|
| C++98 | **ADL** (argument-dependent lookup, Koenig): an unqualified call also searches the namespaces of its arguments | Can find *anything* with that name |
| C++98 | **Specializing `std::swap<T>`** | Function templates can't be partially specialized; class templates need overloads, which are *not allowed* in `std` |
| C++03/11 | **The two-step idiom**: `using std::swap; swap(a, b);` | Every caller must remember it; qualifying (`std::swap(a, b)`) silently disables customization |
| C++11 | `std::begin(c)` / `std::end(c)` with ADL fallbacks | Same pitfalls; two-step again |
| C++17 | `std::invoke`, `std::size`, `std::data` | More free functions, same lookup trouble |
| 2014 | Eric Niebler: **customization point objects** (range-v3) | — |
| **C++20** | `std::ranges::swap`, `begin`, `end`, `size`, `data`, `iter_move`, `iter_swap`, `compare_three_way` … are CPOs; `std::ranges::` algorithms are function objects | Complex to write yourself (this chapter) |
| C++26 | `std::execution` (senders/receivers) is built entirely on CPOs (`connect`, `start`, `schedule`) | — |

---

## 3. Modern solution

```cpp
namespace rg = std::ranges;
rg::swap(a, b);        // one call. Finds a member/ADL swap if there is one; else falls back; checks constraints.
rg::begin(c);          // member begin(), ADL begin(), or array: all in one object
rg::sort(v, {}, &P::age);   // a function OBJECT: ADL cannot see it, users cannot overload it
```

The CPO is **a constexpr function object** that does the lookup *itself*, in a controlled context where only the intended candidates are visible, and is **constrained** so a wrongly-typed customization is a clear compile error.

---

## 4. Mental model

### Two kinds of "customization"

```text
 1. Operations users MAY redefine         2. Algorithms users may NOT redefine
    swap  begin  end  size  data               sort  find  copy  transform  …
    iter_move  iter_swap  <=>
          │                                          │
   Customization point objects                 Function objects ("niebloids")
   look for the user's version                 fixed behaviour; customize via
   (member → ADL → default)                    the operations they call
```

> **Rule of thumb.** *Customize operations, never algorithms.* If you want `ranges::sort` to behave differently for your container, give it better iterators, `iter_swap`/`iter_move`, or a better `<=>`; do not try to overload `sort`.

### How a CPO decides  (`ranges::begin(r)`, simplified)

```text
              ranges::begin(r)
                     │
     r is an array T[N]? ──yes──► return r                       (decays to T*)
                     │ no
     r.begin() valid, returns an iterator? ──yes──► r.begin()    (member)
                     │ no
     begin(r) valid via ADL *only*, returns an iterator? ──yes──► begin(r)
                     │ no
                 ill-formed: not a range
```

The ADL step is done in a **poisoned** context: a deleted `void begin(auto&) = delete;` is declared in the CPO's own scope so that *only* ADL (and not ordinary lookup into `std::`) can find free functions. This is what makes the result independent of headers and `using` directives.

### Why an *object*, not a function?

```text
   function template               function object (variable)
   ─────────────────               ──────────────────────────
   found by ADL as a candidate     ADL ignores non-functions: if ordinary lookup finds an
   → users can overload it         OBJECT, the search stops; no user overload is considered
   → user overloads can hijack     → the library's behaviour is fixed
   cannot be passed as a value     can be passed to algorithms, stored, composed
   (overload set)                  `auto f = std::ranges::sort;` works
```

---

## 5. Language rules

### 5.1 ADL  `[basic.lookup.argdep]`

For an **unqualified function call** `f(args)`, the candidate set is the union of ordinary lookup and a search in the **associated namespaces** (and classes) of each argument's type:

| Argument type | Associated namespaces |
|---|---|
| class `N::C` | `N` (and namespaces of base classes) |
| `N::C<M::D>` (template-id) | `N`, and the namespaces of **template arguments** (`M`) |
| pointer/reference/array to `T` | those of `T` |
| function type | those of parameter and return types |
| fundamental types | none |

Hidden-friend functions (defined in-class as `friend`) are found **only** by ADL: the best way to provide a customization (§7, Experiment 2).

**Three ADL surprises**

1. **Template arguments widen the net**: `std::vector<N::T>` also searches `N`.
2. **`std::` is searched too** if any argument lives there: unqualified `swap(a, b)` on `std::string`s finds `std::swap`.
3. **Qualification disables ADL**: `std::swap(a, b)` and `(swap)(a, b)` do not perform ADL.

### 5.2 The two-step idiom and its failures

```cpp
using std::swap;    // make the default visible
swap(a, b);         // unqualified: ADL finds N::swap if any, else std::swap
```

| Failure | Cause |
|---|---|
| Forgot `using` | A type with no ADL `swap` cannot be swapped at all |
| Wrote `std::swap(a, b)` | User customization silently ignored |
| User `swap` has wrong signature, e.g. takes non-const lvalue-ref to a different type | Silently chosen or silently not; no concept checks |
| An unrelated function named `swap` in an associated namespace | It hijacks the call |

### 5.3 What a standard CPO must satisfy  `[customization.point.object]`

A *customization point object* is a `const` function object of literal class type such that:

1. All instances of its type are **equal**, and **copies are equal** (so it can be passed by value).
2. Its call operators are **constexpr, `noexcept`-propagating where appropriate**, and **constrained**.
3. It is **not overloadable and not ADL-findable** (it's a variable).
4. It is **not a template** you can specialize.

Because the *type* is not nameable the same way each time (it's an anonymous/unspecified type), the standard uses `inline constexpr` variables inside an `inline namespace __cust`-style scope (libstdc++: `std::ranges::__access::_Begin`).

### 5.4 The iterator concept refinements  `[iterator.concepts]`

```text
                        input_or_output_iterator          (++ , *, weakly_incrementable)
                          │              
                  ┌───────┴────────┐
            input_iterator      output_iterator<T>
                  │
            forward_iterator          (multi-pass, equality)
                  │
         bidirectional_iterator       (--)
                  │
         random_access_iterator       (+=, [], <)
                  │
         contiguous_iterator          (to_address works; elements adjacent)
```

Notable differences from the old *iterator categories*:

| | C++17 tag-based | C++20 concept-based |
|---|---|---|
| Determined by | `iterator_traits<It>::iterator_category` | The **operations the type actually supports** (`ITER_CONCEPT`) |
| Proxy references (`zip`, `vector<bool>`) | Forced to be *input* because `reference` isn't a true reference | Can be random access: the `iterator_category` says input (legacy) but `iterator_concept` says random access |
| Sentinels | Not representable | `sentinel_for<S, I>`, `sized_sentinel_for` |

Because of that split, a `zip_view` over vectors is a **C++20 random-access range** but a **C++17 input iterator**. Legacy algorithms (`std::sort(zip.begin(), zip.end())`) see the C++17 category and **refuse or misbehave**; `ranges::sort` uses the concept, and `iter_swap`/`iter_move` to handle proxies.

### 5.5 The `indirectly_*` concepts: algorithm preconditions as types

| Concept | Meaning |
|---|---|
| `indirectly_readable<I>` | `*it` yields a value; `iter_value_t`, `iter_reference_t`, `iter_rvalue_reference_t` |
| `indirectly_writable<O, T>` | `*o = t` valid |
| `indirectly_movable<I, O>` / `indirectly_copyable` | `*out = std::move(*in)` / `*out = *in` valid |
| `indirectly_swappable<I1, I2>` | `ranges::iter_swap(i1, i2)` valid |
| `indirectly_comparable<I1, I2, Comp, P1, P2>` | `comp(proj1(*i1), proj2(*i2))` valid |
| `indirect_unary_predicate<P, I>` | `pred(*it)` valid, boolean-testable |
| `indirect_strict_weak_order<R, I1, I2>` | A comparison forming a strict weak order (*syntactic only*: Chapter 10's semantic-constraint caveat) |
| `permutable<I>` | Elements can be rearranged (move + swap) |
| `sortable<I, Comp, Proj>` | `permutable` + comparison with projection |
| `mergeable<I1, I2, O, Comp, P1, P2>` | Merge preconditions |

`ranges::sort`'s real signature, read as a contract:

```cpp
template <random_access_range R, class Comp = ranges::less, class Proj = identity>
  requires sortable<iterator_t<R>, Comp, Proj>
constexpr borrowed_iterator_t<R> sort(R&& r, Comp comp = {}, Proj proj = {});
```

### 5.6 Projections and `std::invoke`

Every projected algorithm calls `std::invoke(proj, *it)`. Hence valid projections are: a pointer to data member, a pointer to member function, a function, a lambda, or a function object (Chapter 7's `invoke` handles all). `std::identity` is the default. **The projection is applied only to the *argument of the comparison/predicate*, not to the stored element**.

A *pair* of projections exists for binary algorithms (`ranges::equal(a, b, eq, proj1, proj2)`): they let you compare different types without writing an adapter.

### 5.7 `std::ranges::` algorithms are *niebloids*

*Niebloid* (after Eric Niebler) is the community name for a **function object that behaves like a function template but is not one**. The standard says (`[algorithms.requirements]`) that none of the `ranges::` algorithms can be found by ADL, and that explicitly-specified template arguments are not guaranteed to work (they're not required to be function templates at all).

Consequences:

| You can | You cannot |
|---|---|
| Pass `std::ranges::sort` as an argument (it's an object) | Overload `ranges::sort` for your type |
| Call it qualified or via `using namespace std::ranges` | Rely on ADL to pick a user `sort` over it |
| `rg::sort(v)` unambiguously, even with `using namespace std;` | Write `rg::sort<std::vector<int>&>(v)` portably (explicit template args aren't guaranteed) |

### 5.8 Customization via `tag_invoke` / member names (the design space)

| Strategy | Used by | Notes |
|---|---|---|
| **Member function by name** | `begin`, `size`, `data` | Simple; pollutes class scope; no way to add to a type you don't own |
| **ADL free function** (hidden friend) | `swap`, `iter_move`, `begin` | Non-intrusive; name collisions possible; mitigated by CPO constraints |
| **Trait specialization** | `std::hash`, `std::formatter`, `std::tuple_size` | Works for types you don't own; specializations must be in the right namespace |
| **`tag_invoke(CPO, args...)`** | Proposed (P1895), used by libunifex / earlier `std::execution` | A single, unambiguous ADL name; **replaced** in the C++26 design by member functions (`.connect()`, `.start()`) |
| **Concepts + overloads on your own types** | Typical library code | Simplest for *your* API |

> **Opinion.** For *your own* libraries: provide **hidden-friend ADL functions** or **member functions**, and write one CPO wrapper only if you need constraint checking and header-independence. `tag_invoke` was an interesting detour; the C++26 `std::execution` design dropped it in favour of member functions precisely because it was too hard to use and read. Don't adopt it in new code.

### Layer check

| Layer | Decides |
|---|---|
| **Standard** | Which operations are CPOs; exactly how they search (member → ADL → default); the concept definitions; that algorithms are non-ADL-findable |
| **Compiler / library** | How the CPO is encoded (libstdc++ `__access`/`__cust` namespaces, deleted poison-pill overloads); error-message quality; inlining of the call (it's a constexpr object with an `operator()`, so it is as cheap as a function) |
| **ABI** | CPOs are `inline constexpr` objects; they have **no ABI footprint** (no symbol after inlining) |
| **CPU** | Nothing: all dispatch is at compile time |

---

## 6. Implementation model

### What GCC actually does for `ranges::begin`

Roughly (libstdc++ `<bits/ranges_base.h>`):

```cpp
namespace ranges {
  namespace __access {
    template <class T> void begin(T&) = delete;            // poison pills: stop ordinary lookup
    template <class T> void begin(const T&) = delete;      // from finding std::begin

    struct _Begin {
      template <class T> requires is_array_v<remove_reference_t<T>> || __member_begin<T> || __adl_begin<T>
      constexpr auto operator()(T&& t) const noexcept(/* … */) {
        if constexpr (is_array_v<remove_reference_t<T>>) return t + 0;
        else if constexpr (__member_begin<T>)           return t.begin();
        else                                            return begin(t);        // finds ONLY ADL candidates (poison pills lose)
      }
    };
  }
  inline namespace __cust { inline constexpr __access::_Begin begin{}; }
}
```

Three tricks to notice:

1. **Poison pills** (`= delete` templates) are *worse matches* than any real overload but *better than nothing*, so ordinary lookup finds "something" and stops, while ADL still finds the user's function and wins by being more specialized.
2. The object lives in an **`inline namespace`** so `std::ranges::begin` and `std::ranges::__cust::begin` name the same thing, but the *poison pills* live in a sibling namespace and don't leak.
3. `operator()` is **constexpr and noexcept-propagating**, so `ranges::begin(r)` costs exactly what `r.begin()` costs.

### CPOs are free at run time

A call `ranges::begin(v)` constant-folds to `v.begin()` at `-O0` *after* inlining a trivial call; at `-O1` and above no trace of the object remains. The *compile-time* cost is real (more templates and concept checks, longer error messages), which is the actual price of this design.

---

## 7. Experiments

### Experiment 1: ADL in action, and how qualification and templates change the lookup

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <utility>
#include <vector>

namespace lib {
    struct Widget { int id; };
    void inspect(const Widget& w) { std::printf("  lib::inspect(Widget %d)\n", w.id); }
    void inspect(int i)           { std::printf("  lib::inspect(int %d)\n", i); }
}

namespace other { void inspect(const lib::Widget&) { std::printf("  other::inspect\n"); } }

namespace ns {
    struct Gadget { };
    void touch(const Gadget&) { std::printf("  ns::touch(Gadget)\n"); }
}

template <class T> void call_unqualified(const T& x) { inspect(x); }          // dependent name: ADL at instantiation
template <class T> void call_touch(const T& x) { touch(x); }

int main() {
    lib::Widget w{7};

    std::puts("1. unqualified call, argument in lib:");
    call_unqualified(w);                                           // ADL finds lib::inspect

    std::puts("2. template argument widens the associated namespaces:");
    std::vector<ns::Gadget> gadgets(1);
    // touch(gadgets) would search namespace std AND ns (template argument): but the function takes a Gadget:
    call_touch(gadgets[0]);                                        // ADL → ns::touch

    std::puts("3. qualified call turns ADL off:");
    lib::inspect(w);                                               // plain qualified lookup

    std::puts("4. fundamental types have no associated namespace:");
    // call_unqualified(3);                                        // would NOT compile: no `inspect(int)` visible at the template definition and ADL finds nothing for int
    std::puts("  (skipped: does not compile; try it)");

    std::puts("5. std::swap vs ADL swap:");
    int a = 1, b = 2;
    using std::swap;
    swap(a, b);                                                    // unqualified, with a using-declaration
    std::printf("  a=%d b=%d\n", a, b);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
1. unqualified call, argument in lib:
  lib::inspect(Widget 7)
2. template argument widens the associated namespaces:
  ns::touch(Gadget)
3. qualified call turns ADL off:
  lib::inspect(Widget 7)
4. fundamental types have no associated namespace:
  (skipped: does not compile; try it)
5. std::swap vs ADL swap:
  a=2 b=1
```

### Experiment 2: The two-step idiom breaks silently; a CPO fixes it

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <ranges>
#include <utility>

namespace gfx {
    struct Image {
        int* pixels; int n;
        // A hidden friend: found ONLY by ADL. Cheap: swaps two pointers, no pixel copies.
        friend void swap(Image& a, Image& b) noexcept {
            std::puts("  gfx::swap(Image&, Image&)  (pointer swap)");
            std::swap(a.pixels, b.pixels); std::swap(a.n, b.n);
        }
    };
}

template <class T> void swap_wrong(T& a, T& b)  { std::swap(a, b); }         // qualified: ignores customization
template <class T> void swap_twostep(T& a, T& b) { using std::swap; swap(a, b); }
template <class T> void swap_cpo(T& a, T& b)     { std::ranges::swap(a, b); }

int main() {
    int px1[3] = {1, 2, 3}, px2[3] = {4, 5, 6};
    gfx::Image a{px1, 3}, b{px2, 3};

    std::puts("qualified std::swap (customization silently ignored):");
    swap_wrong(a, b);                       // uses move construct + 2 move assignments, no message printed

    std::puts("two-step idiom:");
    swap_twostep(a, b);

    std::puts("ranges::swap CPO:");
    swap_cpo(a, b);

    int x = 1, y = 2;
    std::ranges::swap(x, y);                // default path for types with no custom swap
    std::printf("ints: x=%d y=%d\n", x, y);

    // The CPO also swaps arrays element-wise, which std::swap(ADL) handles only via a template overload:
    int arr1[2] = {1, 2}, arr2[2] = {3, 4};
    std::ranges::swap(arr1, arr2);
    std::printf("arrays: %d %d | %d %d\n", arr1[0], arr1[1], arr2[0], arr2[1]);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
qualified std::swap (customization silently ignored):
two-step idiom:
  gfx::swap(Image&, Image&)  (pointer swap)
ranges::swap CPO:
  gfx::swap(Image&, Image&)  (pointer swap)
ints: x=2 y=1
arrays: 3 4 | 1 2
```

The first call prints **nothing**: the qualified `std::swap` was chosen, the user's cheap `swap` never ran, and the program is still *correct*; it is just slower. That is the worst kind of bug: **silent performance regression**. (This is the argument for CPOs in a sentence.)

### Experiment 3: Implement a CPO — member, then ADL, then default

We build `my::size` with the same three-tier strategy, including poison pills and constraints.

```cpp
// @test run -std=c++23 -O0
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdio>
#include <list>
#include <string>
#include <type_traits>
#include <vector>

namespace my {
    namespace detail {
        void size(const auto&) = delete;                 // poison pill: blocks ordinary lookup of `size`

        template <class T> concept has_member_size = requires(T&& t) { { t.size() } -> std::convertible_to<std::size_t>; };
        template <class T> concept has_adl_size    = requires(T&& t) { { size(t) } -> std::convertible_to<std::size_t>; };

        struct size_fn {
            template <class T, std::size_t N>
            constexpr std::size_t operator()(T (&)[N]) const noexcept { return N; }       // arrays

            template <class T> requires has_member_size<T>
            constexpr std::size_t operator()(T&& t) const noexcept(noexcept(t.size())) {
                std::puts("    [member]"); return static_cast<std::size_t>(t.size());
            }
            template <class T> requires (!has_member_size<T> && has_adl_size<T>)
            constexpr std::size_t operator()(T&& t) const noexcept(noexcept(size(t))) {
                std::puts("    [ADL]"); return static_cast<std::size_t>(size(t));
            }
        };
    }
    inline constexpr detail::size_fn size{};            // the CPO: a constexpr object
}

namespace legacy {
    struct Blob { int bytes; };
    constexpr int size(const Blob& b) { return b.bytes; }           // free function, found only by ADL
}

struct NoSize { };
template <class T> concept sizeable = requires(T& t) { my::size(t); };

int main() {
    std::vector<int> v(5);
    int arr[7];
    legacy::Blob blob{42};

    std::printf("vector : %zu\n", my::size(v));
    std::printf("array  : %zu\n", my::size(arr));
    std::printf("blob   : %zu\n", my::size(blob));

    static_assert(sizeable<std::vector<int>>);
    static_assert(sizeable<legacy::Blob>);
    static_assert(!sizeable<NoSize>);                   // constrained: a clean "not satisfied", not a hard error

    // It is a value: can be stored, passed, composed.
    auto f = my::size;
    std::printf("via stored copy: %zu\n", f(v));

    // And, being an object, a same-named function in the user's namespace cannot hijack it:
    //   namespace my { template<class T> size_t size(T&); }   // would be a redefinition error, not an overload
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
    [member]
vector : 5
array  : 7
    [ADL]
blob   : 42
    [member]
via stored copy: 5
```

What to extract:

- The **poison pill** (`= delete`) exists so that, inside `detail`, an unqualified `size(t)` finds *something* in ordinary lookup, and therefore cannot find a *different* `size` (e.g. `std::size`) there. ADL still finds `legacy::size` and, being a better match than a deleted template, wins.
- The three tiers are mutually exclusive by constraints: a type with a member `size()` never reaches the ADL tier, a **deterministic** priority.
- `sizeable<NoSize>` is *false*, not a hard error: the CPO participates in concept checks. A plain function `size(t)` in a generic context would have been a hard error at instantiation.

### Experiment 4: Niebloids and what they forbid

```cpp
// @test run -std=c++23 -O0
#include <algorithm>
#include <cstdio>
#include <functional>
#include <ranges>
#include <type_traits>
#include <vector>

namespace rg = std::ranges;

namespace mine {
    struct V { std::vector<int> d; };
    // A user "overload" of sort in the user's namespace:
    template <class T> void sort(T&) { std::puts("  mine::sort called"); }
}

int main() {
    mine::V v{{3, 1, 2}};
    std::vector<int> w{3, 1, 2};

    // (1) std::ranges::sort is an object: it can be stored and passed.
    auto s = rg::sort;
    s(w);
    std::printf("(1) via stored object: %d %d %d\n", w[0], w[1], w[2]);

    // (2) An unqualified call in a scope that merely has `using namespace std::ranges` still finds the OBJECT first,
    //     and ADL on std::vector<int> does not search ::mine.
    using namespace std::ranges;
    std::vector<int> u{9, 8, 7};
    sort(u);
    std::printf("(2) unqualified sort(u): %d %d %d\n", u[0], u[1], u[2]);

    // (3) The type is a class (not a function pointer type); no explicit template arguments allowed portably:
    static_assert(std::is_class_v<std::remove_cvref_t<decltype(rg::sort)>>);
    static_assert(std::is_object_v<std::remove_cvref_t<decltype(rg::sort)>>);
    static_assert(std::is_empty_v<std::remove_cvref_t<decltype(rg::sort)>>);
    std::puts("(3) ranges::sort is an empty class object (no runtime state)");

    // (4) ...but the classic std::sort IS a function template: ADL-able, overloadable, not a value.
    // auto bad = std::sort;           // ERROR: cannot deduce type of an overload set / template
    std::puts("(4) std::sort cannot be stored without picking an instantiation");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
(1) via stored object: 1 2 3
(2) unqualified sort(u): 7 8 9
(3) ranges::sort is an empty class object (no runtime state)
(4) std::sort cannot be stored without picking an instantiation
```

### Experiment 5: Proxy iterators: why `ranges::` algorithms work where `std::` ones do not

```cpp
// @test run -std=c++23 -O0
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <ranges>
#include <type_traits>
#include <vector>

namespace rg = std::ranges;
namespace vw = std::views;

int main() {
    std::vector<int>    keys = {3, 1, 2};
    std::vector<char>   vals = {'c', 'a', 'b'};
    auto z = vw::zip(keys, vals);                                   // reference type = tuple<int&, char&>: a PROXY

    using It = rg::iterator_t<decltype(z)>;
    std::printf("C++20 concept : random_access_iterator = %d\n", std::random_access_iterator<It>);
    std::printf("C++17 category: is input_iterator_tag  = %d\n",
                std::is_same_v<std::iterator_traits<It>::iterator_category, std::input_iterator_tag>);
    std::printf("reference type is a prvalue proxy       = %d\n", !std::is_reference_v<std::iter_reference_t<It>>);

    // A legacy algorithm trusts the C++17 category and cannot work with a proxy reference:
    //   std::sort(z.begin(), z.end());       // does not compile (requires a true reference / forward iterators)
    // The ranges algorithm uses iter_swap / iter_move customization and the concept:
    rg::sort(z);                                                     // sorts BOTH vectors in lock step, by tuple order
    std::printf("after ranges::sort(zip): ");
    for (std::size_t i = 0; i < keys.size(); ++i) std::printf("(%d,%c) ", keys[i], vals[i]);
    std::puts("");

    // Sort one column by the other with a projection:
    std::vector<int> a = {30, 10, 20};
    std::vector<int> b = {1, 2, 3};
    rg::sort(vw::zip(a, b), {}, [](auto&& t) { return std::get<0>(t); });
    std::printf("projection on zip: a=%d,%d,%d b=%d,%d,%d\n", a[0], a[1], a[2], b[0], b[1], b[2]);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
C++20 concept : random_access_iterator = 1
C++17 category: is input_iterator_tag  = 1
reference type is a prvalue proxy       = 1
after ranges::sort(zip): (1,a) (2,b) (3,c) 
projection on zip: a=10,20,30 b=2,3,1
```

This is the strongest practical argument for the iterator-concept redesign: **two parallel arrays (structure of arrays, Chapter 27) can be sorted together** with one call, because `iter_swap` on a `zip` iterator swaps the *components*, something no C++17 iterator could express.

### Experiment 6: Algorithm results are richer, and projections compose

```cpp
// @test run -std=c++23 -O0
#include <algorithm>
#include <cstdio>
#include <functional>
#include <iterator>
#include <ranges>
#include <string>
#include <vector>

namespace rg = std::ranges;

struct Emp { std::string name; std::string dept; int salary; };

int main() {
    std::vector<Emp> v = {{"ada", "R&D", 120}, {"alan", "R&D", 110}, {"grace", "Ops", 140}, {"edsger", "Ops", 90}, {"linus", "Kernel", 130}};

    // (1) sort by dept then salary desc using a projection that returns a tuple-like key
    rg::sort(v, std::less{}, [](const Emp& e) { return std::pair(e.dept, -e.salary); });
    std::printf("(1) ");
    for (auto& e : v) std::printf("%s/%s/%d ", e.name.c_str(), e.dept.c_str(), e.salary);
    std::puts("");

    // (2) binary search on the projected key; partition_point returns an iterator; wrap the tail in a subrange
    auto pp = rg::partition_point(v, [](const std::string& d) { return d < "Ops"; }, &Emp::dept);
    auto tail = rg::subrange(pp, v.end());
    std::printf("(2) partition_point -> %zu elements from %s\n", tail.size(), tail.empty() ? "-" : tail.front().dept.c_str());

    // (3) copy returns BOTH iterators (in_out_result): chain without recomputing
    std::vector<int> out(10);
    auto salaries = v | std::views::transform(&Emp::salary);
    auto [in_end, out_end] = rg::copy(salaries, out.begin());
    std::printf("(3) copied %td salaries; in_end==end: %d\n", out_end - out.begin(), in_end == rg::end(salaries));

    // (4) minmax returns min_max_result; structured bindings
    auto [lo, hi] = rg::minmax(v, {}, &Emp::salary);
    std::printf("(4) lowest %s (%d), highest %s (%d)\n", lo.name.c_str(), lo.salary, hi.name.c_str(), hi.salary);

    // (5) unique with projection: dedupe adjacent by dept; returns the subrange of removed tail
    auto removed = rg::unique(v, {}, &Emp::dept);
    v.erase(removed.begin(), removed.end());
    std::printf("(5) one per dept: ");
    for (auto& e : v) std::printf("%s ", e.name.c_str());
    std::puts("");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
(1) linus/Kernel/130 grace/Ops/140 edsger/Ops/90 ada/R&D/120 alan/R&D/110 
(2) partition_point -> 4 elements from Ops
(3) copied 5 salaries; in_end==end: 1
(4) lowest edsger (90), highest grace (140)
(5) one per dept: linus grace ada 
```

---

## 8. Assembly / runtime investigation

### Are CPOs truly free?

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fcf-protection=none filter=via_cpo,via_member
#include <ranges>
#include <vector>
long via_cpo(std::vector<int>& v)    { return std::ranges::end(v) - std::ranges::begin(v); }
long via_member(std::vector<int>& v) { return v.end() - v.begin(); }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
via_cpo(std::vector<int, std::allocator<int> >&):
	mov	rax, QWORD PTR 8[rdi]
	sub	rax, QWORD PTR [rdi]
	sar	rax, 2
	ret

via_member(std::vector<int, std::allocator<int> >&):
	mov	rax, QWORD PTR 8[rdi]
	sub	rax, QWORD PTR [rdi]
	sar	rax, 2
	ret
```

If the two bodies are identical (they are: load two pointers, subtract, shift), the CPO costs nothing at run time. At `-O0` the CPO *is* a real call chain (`_Begin::operator()` → `begin()`); this is one reason debug builds of ranges code are slow, and why Chapter 42 recommends `-Og` for debugging templates-heavy code.

### Symbols: where did my customization get called from?

```bash
g++-14 -std=c++23 -O0 -c prog.cpp -o prog.o
nm -C prog.o | grep -E "ranges::__access|_Begin|_Swap"      # the CPO objects and their call operators at -O0
nm -C prog.o | grep -c "gfx::swap"                         # was the hidden friend instantiated/emitted?
```

---

## 9. Implementation exercise

Build `my::ranges_lite`:

1. **`my::swap` CPO** with tiers: ADL `swap` → array element-wise swap → move-based default; constrained on `move_constructible && assignable`. Verify with the test types from Experiment 2 plus an array of `gfx::Image`.
2. **`my::begin`/`my::end`** CPOs for arrays, members and ADL, with the **return-type constraint** (`input_or_output_iterator`) so that a member `begin()` returning `void` is rejected.
3. **`my::iter_move` / `my::iter_swap`** for a user-defined proxy iterator; write a small `ColumnIterator` over a pair of parallel arrays and sort it with `std::ranges::sort`.
4. **`my::sort_by`**: a wrapper `sort_by(range, projection)` on top of `ranges::sort` that rejects projections returning non-`totally_ordered` types with a *readable* diagnostic (a `requires` with a named concept).

<details>
<summary><strong>Solution sketch for the proxy iterator (item 3)</strong></summary>

The shape (full code in the solutions folder when you build it):

```cpp
struct Columns { int* key; char* val; };
struct Ref {                          // the proxy "reference"
    int& k; char& v;
    Ref& operator=(const Ref& o) { k = o.k; v = o.v; return *this; }
    friend void swap(Ref a, Ref b) { std::swap(a.k, b.k); std::swap(a.v, b.v); }   // hidden friend
};
// value_type = std::pair<int,char>;  reference = Ref;  iter_move returns a pair (a value) to avoid dangling.
// Provide:  friend value_type iter_move(const iterator& i);
//           friend void iter_swap(const iterator& a, const iterator& b);
// Then:     std::ranges::sort(range_of_columns);
```

Key insight: `ranges::sort` never touches `*it` directly for moves and swaps; it calls `ranges::iter_move(it)` and `ranges::iter_swap(a, b)`, which your ADL overloads (hidden friends) intercept. The C++17 `std::sort` calls `std::swap(*a, *b)` and `std::move(*a)` and so needs a *real* reference.

</details>

---

## 10. Real-world example

| Where | Mechanism |
|---|---|
| **`std::ranges::` everywhere** | Niebloid algorithms and CPOs `begin/end/size/data` |
| **`std::format`** | `std::formatter<T>` specialization: the *trait specialization* strategy |
| **`std::hash`** | Same: specialization in `std` |
| **`std::execution` (C++26)** | CPOs (`schedule`, `connect`, `start`, `set_value`) dispatching to *member functions* of senders/receivers |
| **`<=>`** | `std::compare_three_way` and synthesized three-way comparison |
| **Boost / fmt / spdlog** | `format_as`, `operator<<`: ADL-found extension hooks |
| **Qt** | `qHash(const T&)` and `qHashMulti` found by ADL; `QMetaType` registration; `qSwap` (obsolete: use `std::swap`) |
| **abseil** | `AbslHashValue`, `AbslStringify` as hidden friends: the exact ADL pattern |

> **Opinion.** When you design an extension point for your own library, the best default is a **hidden friend with a documented name**, plus a *concept* that checks it. Do not invent a CPO unless you must guarantee header-independence or provide fallbacks for types you do not control. Never invite users to **specialize function templates** or to open `namespace std` to add overloads: both are fragile (and the latter is undefined behaviour except for the explicitly allowed specializations).

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| `std::swap(a, b)` in generic code | User's `swap` silently skipped (slow, not wrong) | `using std::swap; swap(a, b);` or `ranges::swap` |
| Adding overloads to `namespace std` | UB (outside allowed specializations); breaks on upgrade | Hidden friend in your namespace |
| Customizing via a *non-const* or mismatched-signature `swap`/`begin` | Not selected or ambiguous; hard to see | Constrain, then `static_assert(std::swappable<T>)` |
| Free function named like a std name found by ADL (`size`, `begin`, `data`) in a namespace that shares types with std | Hijacks generic code | Constrain, hide in a `friend`, or use a distinct name |
| Expecting `ranges::sort` to pick up your `sort` | It never will: niebloid | Customize `iter_swap`, `iter_move`, `<=>`, iterators |
| Passing explicit template arguments to a `ranges::` algorithm | Not portable | Let deduction work; use a lambda/projection |
| Treating `iterator_category` as truth for ranges code | A proxy iterator advertises *input* (legacy) | Use the concepts (`std::random_access_iterator`) |
| Forgetting to check **sentinel** concepts in your iterator pair | `sentinel_for` unsatisfied; confusing errors | `static_assert(std::sentinel_for<S, It>)` |
| Hiding a *throwing* customization behind `noexcept` | `std::terminate` | Propagate `noexcept(noexcept(...))` |
| Writing a CPO as a function template | ADL and overloading hijack | A constexpr function-object variable |
| Relying on `tag_invoke` in new code | Out of favour; C++26 design dropped it | Members or hidden friends |
| ODR: defining a CPO object as `static` in a header, differing per TU | Different addresses; fine for stateless but surprises identity comparisons | `inline constexpr` |

---

## 12. Exercises

1. **ADL hunt.** For each call in a 200-line generic file of your own, write down which namespaces ADL searches. Find one place where it finds something unintended.
2. **Hijack.** Create a type in namespace `n` that has a hidden friend `begin(T&)` returning `int`. Show what `std::begin`, `rg::begin` and a hand-written `using std::begin; begin(t);` do. Explain each.
3. **Specialization vs overload.** Customize hashing for a user type via `std::hash` specialization, then via an ADL `hash_value` (Boost style). Write the lookup the consumer must do in each case.
4. **A `my::swap` CPO.** Implement as in §9; test with: a type with hidden-friend swap, a type with member swap only (what should happen?), an array, and a move-only type.
5. **Concepts vs CPOs.** `std::swappable<T>` is defined in terms of `ranges::swap`. Show a type for which `std::is_swappable_v<T>` is true but `std::swappable<T>` is false (or vice versa), and explain.
6. **Proxy iterator.** Implement the column iterator from §9; run `std::ranges::sort` on it and `std::sort`; record what the legacy call does; inspect the diagnostic.
7. **Niebloid test.** Write a small trait that detects *whether a name is a function template or a function object* and apply it to `std::sort`, `std::ranges::sort`, `std::ranges::begin`.
8. **Design review.** Look at an extension point in a library you use (Qt's `qHash`, abseil's `AbslHashValue`, fmt's `formatter`). Classify its strategy (member / ADL / specialization / CPO), list its failure modes, and say what you would change.

---

## 13. Challenge: a mini `execution`-style customization layer

Design a tiny asynchronous-operation framework where **senders** and **receivers** can be user types:

- CPOs `my::connect(sender, receiver)`, `my::start(op)`, `my::set_value(rcv, v...)`, `my::set_error(rcv, e)`, each dispatching to a **member function** (preferred) then an **ADL hidden friend**
- concepts `sender`, `receiver_of<Ts...>`, `operation_state` defined *in terms of* the CPOs (not the other way round)
- a `just(v...)` sender, a `then(f)` adaptor (usable with `|` by the technique of Chapter 14), and a blocking `sync_wait`
- a compile-time test matrix with `static_assert`s showing exactly which user types satisfy which concept, including *near-misses* (wrong return type, missing `noexcept`, const-qualification mismatch)
- a document explaining which choices you made differently from `tag_invoke` and why

---

## 14. Knowledge check

1. What is the associated-namespace set for a call whose argument is `std::vector<ns::T>`?
2. Why does `std::swap(a, b)` defeat customization, and what does `using std::swap; swap(a, b);` do differently?
3. What is a *poison pill* and what problem does it solve inside a CPO?
4. Why is `std::ranges::begin` a variable and not a function template?
5. State the member → ADL → default tiers for `ranges::begin`, and what happens if both a member and an ADL candidate exist.
6. What is a niebloid, and name two things you can do with `ranges::sort` that you can't with `std::sort`.
7. Why is a `zip_view` iterator a C++20 random-access iterator but a C++17 input iterator?
8. What do `iter_move` and `iter_swap` exist to support?
9. Where does a projection apply, and where does it *not* apply?
10. Name three alternatives to CPOs for providing extension points, with one drawback each.
11. Why did the C++26 `std::execution` design move from `tag_invoke` to member functions?
12. Why is adding a function overload to `namespace std` undefined behaviour, and what should you do instead?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. `std` (the class template) and `ns` (the template argument), plus global namespace only if ordinary lookup finds it; the call's other arguments add their own.
2. A qualified name bypasses ADL, so only `std::swap` is considered. The `using`-declaration makes `std::swap` a candidate in ordinary lookup while the unqualified call also performs ADL, so a better-matching user `swap` wins by overload resolution.
3. A deleted function template declared in the CPO's scope that makes ordinary unqualified lookup find *something* (and stop) so it can't find `std::begin`, etc.; real ADL candidates are better matches and still win. It makes the lookup depend only on ADL, independent of included headers.
4. ADL treats a function-object variable as a non-function found by ordinary lookup and doesn't consider user overloads, so users can't hijack or overload it; it can also be stored and passed as a value.
5. Array → member `begin()` → ADL `begin(r)` (poison-pilled) → ill-formed. If a member exists it is used and the ADL candidate is never considered.
6. A function *object* that is not ADL-findable and not a user-overloadable template. `ranges::sort` can be stored/passed as a value, takes a range and a projection, works with proxy iterators and sentinels, and returns the end iterator; `std::sort` is an overloadable function template.
7. Its `reference` is a prvalue proxy (`tuple<T&, U&>`), which can't satisfy the C++17 requirement that `reference` be a true reference, so `iterator_category` is `input_iterator_tag`; but the concept-based `iterator_concept` checks the operations, and says random access.
8. To let algorithms move and swap *through proxy references*: `iter_move` yields the rvalue-reference-like type of the element, `iter_swap` swaps the underlying elements.
9. To the *argument of the comparison/predicate/key extraction* (via `std::invoke`); not to the stored element, which is moved/swapped/copied unchanged.
10. Member functions (can't extend foreign types), ADL free functions (name collisions), trait specialization (must be in the right namespace, requires the trait's owner to provide the primary template), `tag_invoke` (hard to read, abandoned), virtual functions (runtime cost, intrusive).
11. `tag_invoke` was hard to read and diagnose, interacted poorly with ADL and templates, and gave poor error messages; members are explicit, scoped to the type, and need no CPO machinery for the common case.
12. The standard forbids adding declarations to `std` except explicit specializations of certain templates for user types; vendors may add overloads or change signatures. Put your function in your own namespace (hidden friend) and let ADL find it.

</details>

---

[← Previous: Chapter 14](14-ranges.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 16 — constexpr →](../part-07-compile-time/16-constexpr.md)
