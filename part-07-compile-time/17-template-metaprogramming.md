# Chapter 17 — Template Metaprogramming

> **Part VII · Compile-Time C++** &nbsp;|&nbsp; **Level 4** &nbsp;|&nbsp; **≈ 6 hours**
> **Prerequisites:** [Chapter 8](../part-04-generic-programming/08-templates-deep-dive.md), [Chapter 9](../part-04-generic-programming/09-type-traits.md), [Chapter 10](../part-04-generic-programming/10-concepts.md), [Chapter 16](16-constexpr.md) &nbsp;|&nbsp; **Standards:** C++11 (variadics) → C++26 &nbsp;|&nbsp; **Tools:** `g++-14`, `clang++-18`, `-ftime-report`

[← Previous: Chapter 16](16-constexpr.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 18 — The reflection landscape →](18-reflection-landscape.md)

---

**In one sentence:** template metaprogramming (TMP) computes with **types** as the values, and **template instantiation** as the evaluation mechanism, a functional language discovered by accident in 1994, that C++11–23 progressively made unnecessary for *values* but still indispensable for *types*.

**By the end of this chapter you can:**

- read and write type-level code: type lists, filters, maps, index sequences
- say what each construct costs the compiler (instantiations, depth, memory)
- replace recursive TMP with pack expansion, fold expressions, `constexpr` and concepts
- decide when a type-level computation is still the right tool
- use compiler builtins (`__type_pack_element`) knowing they are not standard

---

## 1. Problem

Chapter 16 handles **value** computation. But some questions are about *types*, and values can't answer them:

- What is the return type of calling `f` with these arguments?
- Which of these 12 types is the largest? What is the type of a tuple with the `int`s removed?
- Which element type does a `variant` alternative at index *i* hold?
- Generate one class member per type in a list (a `tuple`, a visitor, a dispatcher).

A `constexpr` function cannot *return a type*. Types are not values (until reflection, Chapter 18). So type-level problems need a mechanism that manipulates types: templates.

---

## 2. Historical context

| Year | Event |
|---|---|
| 1994 | Erwin Unruh shows a program whose **compiler error messages print prime numbers**: the committee realizes templates are a Turing-complete compile-time language |
| 1995–2000 | Veldhuizen: expression templates and "C++ templates are Turing-complete" (a paper); Blitz++ |
| 2001 | Alexandrescu, *Modern C++ Design*: **Loki typelists**; policy-based design |
| 2001–2005 | Boost.MPL (Abrahams & Gurtovoy): a full compile-time STL: `vector`, `transform`, `fold` on types |
| 2011 | **Variadic templates** (parameter packs) replace most recursive "cons-list" typelists; `<type_traits>` standardized |
| 2012 | C++11 `constexpr` handles value computation: half of TMP becomes unnecessary |
| 2014 | C++14 `index_sequence`, variable templates, relaxed `constexpr`; Boost.Hana (Louis Dionne) shows *heterogeneous values as the unit* |
| 2017 | **Fold expressions**, `if constexpr`, CTAD, `std::void_t`; the "recursive specialization" style is largely obsolete |
| 2020 | **Concepts** replace SFINAE; `consteval`; `std::type_identity`; C++20 `requires` |
| 2023 | `std::tuple` sizing improvements; `std::forward_like`; `std::is_scoped_enum` |
| 2026 | C++26: **pack indexing** `Ts...[N]` (P2662) and **reflection** (P2996, Chapter 18) |

The trend is the same as Chapter 16: *each revision removes a reason to write template-recursion tricks.* What remains is the part that genuinely needs types: and **reflection** is expected to absorb most of what remains.

---

## 3. Modern solution

| Task | Pre-C++11 | C++11/14 | C++17/20/23 |
|---|---|---|---|
| Compute a *value* from a constant | recursion + `enum { value = ... }` | `constexpr` function | `constexpr` / `consteval` function (Chapter 16) |
| Iterate a pack | recursive overloads | recursive variadic overload | **fold expression**: `(f(args), ...)` |
| Select between types | specialization on `bool` | `std::conditional_t`, `enable_if_t` | `if constexpr`, **concepts** |
| Index into a type list | recursive `At<N, L>` | recursive; `std::tuple_element_t` | `__type_pack_element<N, Ts...>` (builtin), **`Ts...[N]` (C++26)** |
| Run code for each index 0..N−1 | recursive struct | `std::index_sequence` | `index_sequence` + fold; `template for` (proposed) |
| Query types | `sizeof`-trick SFINAE | `void_t` detection | **requires-expressions** |
| Generate members | Loki-style inheritance chain | `std::tuple<Ts...>` | `std::tuple`, aggregate pack inheritance (`struct S : Ts... {}`) |

---

## 4. Mental model

### A functional program whose values are types

```text
   ordinary function                         metafunction
   ─────────────────                         ────────────
   int add(int a, int b)                     template <class A, class B> struct Pair { using type = ...; };
   returns a value                           "returns" a nested ::type  (or ::value, or the alias itself)
   call: add(1, 2)                           call: Pair<int, char>::type   (instantiation = evaluation)
   no side effects needed                    **pure**: instantiating twice yields the same class
   recursion for loops                       recursion = recursive instantiation, depth-limited
   if/else                                   specialization / conditional_t / if constexpr
```

Three facts make the model precise:

1. **Instantiation is evaluation.** `Foo<int>::type` causes the compiler to instantiate `Foo<int>` (once; the result is *memoized* for the whole TU), then read `type`.
2. **It is pure and immutable.** A type, once computed, never changes. There are no variables, only new types (hence the style resembles Haskell and Lisp).
3. **It is lazy about members, strict about the class header.** Naming `Foo<int>` does *not* instantiate its members' definitions; using a member does (Chapter 8's lazy-instantiation rule).

### Three styles of metafunction

```cpp
// 1. Struct with a nested typedef/constant  (C++03 style, still the base of <type_traits>)
template <class T> struct remove_pointer          { using type = T; };
template <class T> struct remove_pointer<T*>      { using type = T; };

// 2. Alias template: the result *is* the alias; no ::type, no instantiation of a class
template <class T> using remove_pointer_t = typename remove_pointer<T>::type;

// 3. constexpr function returning a *value* describing a type  (the modern way when the answer is a value)
template <class... Ts> constexpr std::size_t largest() { return std::max({sizeof(Ts)...}); }
```

Prefer, in order: **(a)** a `constexpr` function if the result is a value; **(b)** an **alias template** built from other aliases (cheapest for the compiler); **(c)** a struct metafunction only when you need specialization.

### Cost model

```text
   compile time  ≈  (number of distinct template instantiations) × (cost of each)
                    + (depth of recursive instantiation)  → bounded by -ftemplate-depth (default 900 in GCC)
   memory        ≈  every instantiated class/function stays in the compiler's memory for the whole TU
```

> **Rule.** Count *instantiations*, not lines. A recursive `At<N, List>` instantiates N classes; a `__type_pack_element` builtin instantiates **none**; the fold form instantiates **one function**. Experiment 6 measures that.

---

## 5. Language rules

### 5.1 Parameter packs  `[temp.variadic]`

A **template parameter pack** (`typename... Ts`) binds zero or more arguments; a **function parameter pack** (`Ts... args`) takes zero or more function arguments. A pack must be **expanded** by a pattern followed by `...`. The expansion is *syntactic*: it replicates the pattern once per element.

| Context | Example | Expands to |
|---|---|---|
| Function arguments | `f(g(args)...)` | `f(g(a1), g(a2), …)` |
| Template arguments | `tuple<Ts*...>` | `tuple<T1*, T2*, …>` |
| Base list | `struct S : Ts... {}` | `struct S : T1, T2, … {}` |
| Initializer list | `{ args... }` | `{ a1, a2, … }` |
| `sizeof...(Ts)` | | count; **not** an expansion |
| Fold expression (C++17) | `(args + ...)` | `(a1 + (a2 + …))` |
| **Pack indexing** (C++26) | `Ts...[0]` | the first element, *standardized* (P2662) |
| Capture (C++20) | `[...xs = std::move(args)]` | pack init-capture |
| Using-declaration (C++17) | `using Ts::operator()...;` | one `using` per base (the **overloaded** idiom) |

### 5.2 Fold expressions  `[expr.prim.fold]`

| Form | Meaning |
|---|---|
| `(pack op ...)` | unary right fold: `a1 op (a2 op (… op aN))` |
| `(... op pack)` | unary left fold: `(((a1 op a2) op …) op aN)` |
| `(pack op ... op init)` | binary right fold with initial value |
| `(init op ... op pack)` | binary left fold |

Empty pack: only `&&` (→ `true`), `||` (→ `false`) and `,` (→ `void()`) are allowed in a unary fold. With the comma operator, a fold is a **"for each" statement at compile time**: `(use(args), ...);`.

### 5.3 `std::integer_sequence` and `index_sequence`  `[intseq]`

```cpp
template <std::size_t... Is> void f(std::index_sequence<Is...>) { /* Is... is a pack of constants 0,1,2,... */ }
f(std::make_index_sequence<4>{});          // Is... = 0,1,2,3
```

Since C++14 the standard library implements `make_index_sequence<N>` with a **compiler builtin** (`__integer_pack` in GCC and Clang) so it costs O(1) instantiations, not O(N), which is why you must *not* hand-roll it.

### 5.4 Template-template parameters, variable templates, alias templates

- **Template-template parameters** let a metafunction be generic over *template shapes* (`template <template <class...> class F, class... Ts>`).
- **Variable templates** (C++14) are the cleanest way to expose a constant result: `template <class T> inline constexpr bool is_big_v = sizeof(T) > 64;`
- **Alias templates** are *transparent*: `Alias<int>` **is** the aliased type; they can't be specialized and can't be deduced through, but cost almost nothing.

### 5.5 Recursion limits and the evaluation order

- Recursion depth is limited by **`-ftemplate-depth=N`** (GCC default 900, Clang default 1024, both verified above); the Standard recommends 1024 as a minimum. Exceeding it is a hard error.
- Substitution is **depth-first and memoized**; the order of instantiation is *not* specified, so metafunctions must be **pure** (no friend-injection state tricks; the "stateful metaprogramming" hack is a known compiler-dependent abuse and CWG explicitly flagged it).

### 5.6 What C++20 `requires` replaced

| Old | New |
|---|---|
| `std::enable_if_t<cond, int> = 0` default template parameter | `requires cond` |
| `void_t<decltype(expr)>` detection | `requires { expr; }` |
| Tag dispatch with `true_type`/`false_type` | `if constexpr` / constrained overloads |
| `static_assert` in a primary template to reject types | a concept on the parameter |

See Chapter 10; the TMP that *survives* is *computing types*, not *selecting overloads*.

### Layer check

| Layer | Decides |
|---|---|
| **Standard** | Template instantiation semantics, pack expansion, fold, `integer_sequence` interface, the minimum recommended depth (1024) |
| **Compiler** | How instantiation is implemented and cached (hash-consing of template-ids), actual depth limit, **builtins** (`__type_pack_element`, `__integer_pack`, `__is_same`, `__underlying_type`), compile time and memory |
| **ABI** | Every distinct instantiation is a distinct type with a mangled name; a deep TMP result (`Cons<A, Cons<B, ...>>`) **appears in symbol names**, making them long and fragile; avoid exposing such types across a shared-library boundary |
| **CPU** | Nothing: the whole computation is gone by code generation, **unless** the result *is* a type that determines layout/dispatch (e.g. `std::tuple` layout) |

---

## 6. Implementation model

### Where the compiler spends its time

```text
   TU  ─►  parse templates  ─►  *instantiate on demand*  ─►  code generation
                                      │
                      each distinct (template, args) pair → one entry in the instantiation table
                      each entry holds: a class (members declared lazily) or function body (instantiated when used)
```

GCC and Clang both:

- **Hash-cons** template-ids: `std::tuple<int, char>` mentioned a thousand times is one entity.
- Instantiate *declarations* eagerly when a complete type is needed, *definitions* lazily.
- Keep every instantiation until the end of the TU: **memory** is proportional to the number of distinct instantiations, even if the result is thrown away.

### The two hot spots

1. **Deep recursion**: each level is an instantiation *and* a stack frame inside the compiler.
2. **Large pack × large pack**: `(Ts, Us)...` patterns and quadratic algorithms (`unique`, `sort`) create *N²* instantiations; typelists beyond a few hundred elements need care (Experiment 6).

### Pack expansion is cheap; recursion is expensive

A fold such as `(sizeof(Ts) + ... + 0)` is **one** instantiation (one function or one variable template) plus an expansion the compiler performs in a single pass. The recursive struct `Sum<T, Ts...>` is **N** instantiations. Compare in §7, Experiment 6.

---

## 7. Experiments

### Experiment 1: A typelist library, the C++17/20 way

We implement `List`, `size`, `at`, `append`, `concat`, `contains`, `transform`, `filter`, `unique`, `reverse` using *pack expansion and aliases* wherever possible. The helper `name<T>()` prints types so we can see the results.

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <utility>

template <class... Ts> struct List { static constexpr std::size_t size = sizeof...(Ts); };

namespace tl {
    // --- at: the compiler builtin: 0 recursive instantiations (GCC >= 13, Clang) ---
    template <std::size_t I, class... Ts> using pack_at = __type_pack_element<I, Ts...>;
    template <class L, std::size_t I> struct at_impl;
    template <class... Ts, std::size_t I> struct at_impl<List<Ts...>, I> { using type = pack_at<I, Ts...>; };
    template <class L, std::size_t I> using at = typename at_impl<L, I>::type;

    // --- append / concat: pack expansion, no recursion ---
    template <class L, class T> struct append_impl;
    template <class... Ts, class T> struct append_impl<List<Ts...>, T> { using type = List<Ts..., T>; };
    template <class L, class T> using append = typename append_impl<L, T>::type;

    template <class A, class B> struct concat_impl;
    template <class... As, class... Bs> struct concat_impl<List<As...>, List<Bs...>> { using type = List<As..., Bs...>; };
    template <class A, class B> using concat = typename concat_impl<A, B>::type;

    // --- transform: apply a metafunction to every element in ONE expansion ---
    template <template <class> class F, class L> struct transform_impl;
    template <template <class> class F, class... Ts> struct transform_impl<F, List<Ts...>> { using type = List<F<Ts>...>; };
    template <template <class> class F, class L> using transform = typename transform_impl<F, L>::type;

    // --- filter: build List<T> or List<> per element, then flatten (linear recursion in the flatten) ---
    template <class A, class B> struct cat;
    template <class... Xs, class... Ys> struct cat<List<Xs...>, List<Ys...>> { using type = List<Xs..., Ys...>; };
    template <class... Ls> struct flatten { using type = List<>; };                         // empty pack -> empty list
    template <class L0, class... Ls> struct flatten<L0, Ls...> { using type = typename cat<L0, typename flatten<Ls...>::type>::type; };

    template <template <class> class Pred, class L> struct filter_impl;
    template <template <class> class Pred, class... Ts>
    struct filter_impl<Pred, List<Ts...>> {
        template <class T> using keep = std::conditional_t<Pred<T>::value, List<T>, List<>>;
        using type = typename flatten<keep<Ts>...>::type;
    };
    template <template <class> class Pred, class L> using filter = typename filter_impl<Pred, L>::type;

    // --- contains: a fold expression over a bool pack ---
    template <class T, class L> struct contains_impl;
    template <class T, class... Ts> struct contains_impl<T, List<Ts...>> : std::bool_constant<(std::is_same_v<T, Ts> || ...)> {};
    template <class T, class L> inline constexpr bool contains = contains_impl<T, L>::value;

    // --- unique: keep the first occurrence of each type ---
    template <class Out, class... Ts> struct unique_impl { using type = Out; };
    template <class... Os, class T, class... Ts>
    struct unique_impl<List<Os...>, T, Ts...> {
        using type = typename unique_impl<std::conditional_t<(std::is_same_v<T, Os> || ...), List<Os...>, List<Os..., T>>, Ts...>::type;
    };
    template <class L> struct unique_of;
    template <class... Ts> struct unique_of<List<Ts...>> { using type = typename unique_impl<List<>, Ts...>::type; };
    template <class L> using unique = typename unique_of<L>::type;

    // --- reverse: via an index_sequence + pack_at, no recursion at all ---
    template <class L, class Seq> struct reverse_impl;
    template <class... Ts, std::size_t... Is>
    struct reverse_impl<List<Ts...>, std::index_sequence<Is...>> { using type = List<pack_at<sizeof...(Ts) - 1 - Is, Ts...>...>; };
    template <class L> struct reverse_of;
    template <class... Ts> struct reverse_of<List<Ts...>> { using type = typename reverse_impl<List<Ts...>, std::index_sequence_for<Ts...>>::type; };
    template <class L> using reverse = typename reverse_of<L>::type;
}

// ---- tests: all verified by the compiler ----
using L = List<int, char, long, char, int, double>;

static_assert(L::size == 6);
static_assert(std::is_same_v<tl::at<L, 2>, long>);
static_assert(std::is_same_v<tl::append<L, float>, List<int, char, long, char, int, double, float>>);
template <class T> using ptr = T*;                       // an ALIAS metafunction: transform applies F<T> directly
static_assert(std::is_same_v<tl::transform<ptr, List<int, char>>, List<int*, char*>>);
// (passing std::add_pointer would give List<add_pointer<int>, ...>: that is the struct; you want add_pointer_t.)
static_assert(std::is_same_v<tl::filter<std::is_integral, L>, List<int, char, long, char, int>>);
static_assert(tl::contains<long, L> && !tl::contains<float, L>);
static_assert(std::is_same_v<tl::unique<L>, List<int, char, long, double>>);
static_assert(std::is_same_v<tl::reverse<List<int, char, long>>, List<long, char, int>>);
static_assert(std::is_same_v<tl::concat<List<int>, List<char, long>>, List<int, char, long>>);

// ---- printing a List<...> at run time to *see* the types ----
template <class T> constexpr const char* type_name() {
    const char* p = __PRETTY_FUNCTION__;                 // compiler-specific: not standard
    return p;
}
template <class... Ts> void show(List<Ts...>) {
    std::printf("List<%zu>:", sizeof...(Ts));
    ((std::printf(" %s", std::is_same_v<Ts, int> ? "int" : std::is_same_v<Ts, char> ? "char" : std::is_same_v<Ts, long> ? "long" :
                         std::is_same_v<Ts, double> ? "double" : std::is_same_v<Ts, float> ? "float" : "?")), ...);
    std::puts("");
}

int main() {
    show(L{});
    show(tl::unique<L>{});
    show(tl::reverse<tl::unique<L>>{});
    show(tl::filter<std::is_floating_point, L>{});
    std::puts("all static_asserts passed at compile time");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
List<6>: int char long char int double
List<4>: int char long double
List<4>: double long char int
List<1>: double
all static_asserts passed at compile time
```

What this shows:

- **`at`, `reverse`, `transform`, `concat`, `append`, `contains`** have *no recursion*: each is **one** pack expansion. In a Boost.MPL or Loki typelist they'd be linear recursions.
- **`filter` and `unique`** still need an accumulate step. `filter` here builds `List<T>` or `List<>` per element and flattens (linear recursion in the flatten); `unique` walks with an accumulator (linear). They are the *irreducible* TMP: they depend on the result of previous steps.
- The tests are `static_assert`s: **the test suite runs when you compile.** That is the testing story for TMP.

### Experiment 2: Fold expressions as compile-time `for` loops

```cpp
// @test run -std=c++23 -O0
#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

// (1) Reduce over a pack of values and of types in a single expression.
template <class... Ts> constexpr std::size_t total_size = (sizeof(Ts) + ... + 0);
template <class... Ts> constexpr std::size_t max_align  = std::max({alignof(Ts)..., std::size_t{1}});

// (2) "for each argument" using the comma operator; evaluation order is LEFT TO RIGHT for the comma fold.
template <class... Args> void print_all(const Args&... args) {
    std::size_t i = 0;
    ((std::printf("  arg%zu = %s\n", i++, std::to_string(args).c_str())), ...);
}

// (3) apply a function to each tuple element with index_sequence
template <class Tuple, class F, std::size_t... Is>
void for_each_impl(Tuple&& t, F&& f, std::index_sequence<Is...>) {
    (f(std::integral_constant<std::size_t, Is>{}, std::get<Is>(std::forward<Tuple>(t))), ...);
}
template <class Tuple, class F> void for_each(Tuple&& t, F&& f) {
    for_each_impl(std::forward<Tuple>(t), std::forward<F>(f), std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<Tuple>>>{});
}

// (4) Build a std::array from a generator f(i), in one expansion
template <class F, std::size_t... Is> constexpr auto make_array_impl(F f, std::index_sequence<Is...>) { return std::array{f(Is)...}; }
template <std::size_t N, class F> constexpr auto make_array(F f) { return make_array_impl(f, std::make_index_sequence<N>{}); }

// (5) Dispatch a runtime index to a compile-time index (a jump table generated by expansion)
template <class F, std::size_t... Is>
void visit_index_impl(std::size_t i, F&& f, std::index_sequence<Is...>) {
    ((i == Is ? (f(std::integral_constant<std::size_t, Is>{}), 0) : 0), ...);
}
template <std::size_t N, class F> void visit_index(std::size_t i, F&& f) { visit_index_impl(i, f, std::make_index_sequence<N>{}); }

int main() {
    static_assert(total_size<char, int, double> == 13);
    static_assert(max_align<char, int, double> == 8);
    std::printf("total_size<char,int,double> = %zu, max_align = %zu\n", total_size<char, int, double>, max_align<char, int, double>);

    print_all(1, 2.5, 3L);

    auto t = std::make_tuple(10, 'x', 2.5);
    for_each(t, [](auto I, const auto& v) { std::printf("  tuple[%zu] = %s\n", decltype(I)::value, std::to_string(v).c_str()); });

    constexpr auto squares = make_array<6>([](std::size_t i) { return int(i * i); });
    static_assert(squares[5] == 25);
    std::printf("squares:"); for (int v : squares) std::printf(" %d", v); std::puts("");

    std::tuple<int, std::string, double> rec{7, "seven", 7.0};
    for (std::size_t i = 0; i < 3; ++i)
        visit_index<3>(i, [&](auto I) { std::printf("  runtime i=%zu -> compile-time field %zu\n", i, std::size_t(decltype(I)::value)); (void)rec; });
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
total_size<char,int,double> = 13, max_align = 8
  arg0 = 1
  arg1 = 2.500000
  arg2 = 3
  tuple[0] = 10
  tuple[1] = 120
  tuple[2] = 2.500000
squares: 0 1 4 9 16 25
  runtime i=0 -> compile-time field 0
  runtime i=1 -> compile-time field 1
  runtime i=2 -> compile-time field 2
```

Pattern (5) is the bridge between *run-time* and *compile-time* values and underlies `std::visit`: a run-time index is turned into a compile-time constant by generating all N alternatives and picking one. (Real implementations generate a *table of function pointers* so the dispatch is O(1) rather than the O(N) comparison chain shown here: Chapter 13's `visit` experiment.)

### Experiment 3: Function-signature introspection (types you cannot get from values)

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <tuple>
#include <type_traits>

// Extract return and argument types from any callable: free fn, member fn, lambda, function object.
template <class T> struct fn_traits : fn_traits<decltype(&T::operator())> {};                       // functor / lambda

template <class R, class... A> struct fn_traits<R (*)(A...)> {                                      // function pointer
    using result = R; using args = std::tuple<A...>; static constexpr std::size_t arity = sizeof...(A);
};
template <class R, class... A> struct fn_traits<R (A...)> : fn_traits<R (*)(A...)> {};              // function type
template <class C, class R, class... A> struct fn_traits<R (C::*)(A...)> : fn_traits<R (*)(A...)> {};          // member
template <class C, class R, class... A> struct fn_traits<R (C::*)(A...) const> : fn_traits<R (*)(A...)> {};    // const member
template <class C, class R, class... A> struct fn_traits<R (C::*)(A...) noexcept> : fn_traits<R (*)(A...)> {};
template <class C, class R, class... A> struct fn_traits<R (C::*)(A...) const noexcept> : fn_traits<R (*)(A...)> {};

template <std::size_t I, class F> using arg_t = std::tuple_element_t<I, typename fn_traits<F>::args>;

double plain(int, char*) { return 0; }
struct S { long method(float) const { return 0; } };

int main() {
    auto lam = [](short a, unsigned long b) -> bool { return a < long(b); };

    static_assert(std::is_same_v<fn_traits<decltype(&plain)>::result, double>);
    static_assert(fn_traits<decltype(&plain)>::arity == 2);
    static_assert(std::is_same_v<arg_t<1, decltype(&plain)>, char*>);

    static_assert(std::is_same_v<fn_traits<decltype(&S::method)>::result, long>);
    static_assert(std::is_same_v<arg_t<0, decltype(&S::method)>, float>);

    static_assert(std::is_same_v<fn_traits<decltype(lam)>::result, bool>);
    static_assert(std::is_same_v<arg_t<1, decltype(lam)>, unsigned long>);

    std::printf("lambda arity = %zu, plain arity = %zu\n", fn_traits<decltype(lam)>::arity, fn_traits<decltype(&plain)>::arity);
    std::puts("(a generic lambda or an overloaded functor has no single signature: &T::operator() is ambiguous or a template)");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
lambda arity = 2, plain arity = 2
(a generic lambda or an overloaded functor has no single signature: &T::operator() is ambiguous or a template)
```

**Limitation (a design lesson).** This works only for callables with *one, non-template* `operator()`. For anything overloaded or generic, the question "what are its parameter types?" has no answer: the right API takes **the argument types you intend to pass** and asks `std::invoke_result_t<F, Args...>` / `std::is_invocable_v<F, Args...>` instead. *Prefer asking "can I call it with these?" to asking "what is its signature?".*

### Experiment 4: Mixins and member generation: types that *create* types

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <string>
#include <tuple>
#include <type_traits>

// (1) Overload set from lambdas: inheritance + using-declaration pack expansion (the "overloaded" idiom).
template <class... Fs> struct overloaded : Fs... { using Fs::operator()...; };
template <class... Fs> overloaded(Fs...) -> overloaded<Fs...>;

// (2) Policy-based class: inherit the behaviors you ask for.
struct Logs    { void log(const char* m) const { std::printf("  [log] %s\n", m); } };
struct Counts  { int n = 0; void bump() { ++n; } };
struct Timestamps { long ts() const { return 12345; } };

template <class... Policies> struct Widget : Policies... {
    void act() {
        if constexpr (std::is_base_of_v<Counts, Widget>) this->bump();
        if constexpr (std::is_base_of_v<Logs, Widget>)   this->log("act()");
    }
};

// (3) Heterogeneous "table": one member per type via std::tuple, access by TYPE.
template <class... Ts> struct Registry {
    std::tuple<Ts...> items;
    template <class T> T& get() { return std::get<T>(items); }          // ill-formed if T appears twice: a compile-time check
};

int main() {
    auto v = overloaded{ [](int i) { return std::string("int:") + std::to_string(i); },
                         [](double d) { return std::string("double:") + std::to_string(d); },
                         [](const char* s) { return std::string("str:") + s; } };
    std::printf("%s %s %s\n", v(1).c_str(), v(2.5).c_str(), v("x").c_str());

    Widget<Logs, Counts> w;
    w.act(); w.act();
    std::printf("count = %d, sizeof(Widget<Logs,Counts>) = %zu, sizeof(Widget<>) = %zu (empty-base optimization)\n",
                w.n, sizeof(Widget<Logs, Counts>), sizeof(Widget<>));
    std::printf("sizeof(Widget<Logs,Timestamps>) = %zu\n", sizeof(Widget<Logs, Timestamps>));

    Registry<int, std::string, double> reg{};
    reg.get<std::string>() = "hello"; reg.get<int>() = 5;
    std::printf("registry: %d %s\n", reg.get<int>(), reg.get<std::string>().c_str());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
int:1 double:2.500000 str:x
  [log] act()
  [log] act()
count = 2, sizeof(Widget<Logs,Counts>) = 4, sizeof(Widget<>) = 1 (empty-base optimization)
sizeof(Widget<Logs,Timestamps>) = 1
registry: 5 hello
```

*Pack expansion in the base-class list and in `using` declarations* is the modern TMP workhorse: it **generates classes** from a list of types in one line, which in 1998 required recursive inheritance chains.

### Experiment 5: Obsolete versus current: the same task three ways

The task: *"keep only the integral types of a list, count them, and get the largest `sizeof`"*.

```cpp
// @test run -std=c++23 -O0
#include <algorithm>
#include <cstdio>
#include <type_traits>

// ---------- (A) C++03/11 recursive metafunctions ----------
template <class... Ts> struct CountIntegral;
template <> struct CountIntegral<> { static const int value = 0; };
template <class T, class... Ts> struct CountIntegral<T, Ts...> {
    static const int value = (std::is_integral<T>::value ? 1 : 0) + CountIntegral<Ts...>::value;
};
template <class... Ts> struct MaxSize;
template <> struct MaxSize<> { static const std::size_t value = 0; };
template <class T, class... Ts> struct MaxSize<T, Ts...> {
    static const std::size_t value = sizeof(T) > MaxSize<Ts...>::value ? sizeof(T) : MaxSize<Ts...>::value;
};

// ---------- (B) C++17: fold expressions ----------
template <class... Ts> inline constexpr int count_integral_b = (int(std::is_integral_v<Ts>) + ... + 0);
template <class... Ts> inline constexpr std::size_t max_size_b = std::max({sizeof(Ts)..., std::size_t{0}});

// ---------- (C) C++20: a constexpr *function* over an array of facts about the types ----------
template <class... Ts> consteval auto analyse() {
    struct R { int integral = 0; std::size_t max = 0; } r;
    for (auto [is_int, sz] : { std::pair<bool, std::size_t>{std::is_integral_v<Ts>, sizeof(Ts)}... }) {
        r.integral += is_int; r.max = std::max(r.max, sz);
    }
    return r;
}

int main() {
    static_assert(CountIntegral<int, double, char, float, long>::value == 3);
    static_assert(count_integral_b<int, double, char, float, long> == 3);
    constexpr auto r = analyse<int, double, char, float, long>();
    static_assert(r.integral == 3 && r.max == 8);
    static_assert(MaxSize<int, double, char>::value == 8 && max_size_b<int, double, char> == 8);
    std::printf("all three styles agree: integral=%d, max sizeof=%zu\n", r.integral, r.max);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
all three styles agree: integral=3, max sizeof=8
```

The (C) form is **the direction of travel**: *describe the types as ordinary values* (a list of `{bool, size}`), then use ordinary loops. It works only when the *answer* is a value. When the answer is a *type* (the filtered list), you still need (A)/(B)-style type-level code until reflection gives us `std::meta::info` values that can hold types.

### Experiment 6: What does TMP cost the compiler? (instantiation counts and time)

We compare four ways to compute "the size of the *N*-th type" and "the sum of `sizeof` over N types", for N = 100, 400 and 800, by *generating* test programs. This is a shell experiment (not auto-verified); numbers are from one machine, GCC 14.2 and Clang 18.

```bash
#!/bin/bash
# gen.sh N STYLE   -> writes t.cpp
N=$1; STYLE=$2
{
echo '#include <cstddef>'
echo 'template <class...> struct L {};'
case $STYLE in
 recursive)  # classic recursive At<N, L>
  cat <<'EOF'
template <std::size_t I, class L> struct At;
template <class H, class... T> struct At<0, L<H, T...>> { using type = H; };
template <std::size_t I, class H, class... T> struct At<I, L<H, T...>> { using type = typename At<I - 1, L<T...>>::type; };
EOF
  ;;
 builtin) echo 'template <std::size_t I, class... Ts> struct At1 { using type = __type_pack_element<I, Ts...>; };' ;;
esac
# generate N distinct tagged types
for ((i = 0; i < N; ++i)); do echo "struct T$i { char c[$((i%7+1))]; };"; done
echo -n 'using List = L<'; for ((i = 0; i < N; ++i)); do [ $i -gt 0 ] && echo -n ', '; echo -n "T$i"; done; echo '>;'
case $STYLE in
 recursive) echo "static_assert(sizeof(At<$((N-1)), List>::type) > 0);" ;;
 builtin)   echo 'template <class> struct Ex; template <class... Ts> struct Ex<L<Ts...>> { using last = typename At1<sizeof...(Ts)-1, Ts...>::type; };'
            echo 'static_assert(sizeof(Ex<List>::last) > 0);' ;;
esac
echo 'int main(){}'
} > t.cpp
```

Measured (wall-clock for the whole `-c` compile, `-ftemplate-depth=5000`, one machine, one run each, so treat differences under ~20 ms as noise):

| N | recursive `At`: GCC 14.2 | recursive `At`: Clang 18 | `__type_pack_element`: GCC 14.2 | `__type_pack_element`: Clang 18 |
|---:|---:|---:|---:|---:|
| 100 | 0.03 s | 1.82 s ¹ | 0.03 s | 0.04 s |
| 400 | 0.09 s | 0.15 s | 0.04 s | 0.05 s |
| 800 | 0.26 s | 0.47 s | 0.03 s | 0.05 s |
| 2000 | 1.34 s | 2.69 s ² | 0.05 s | 0.06 s |

¹ A cold-start outlier (first Clang invocation); ignore it. ² Clang printed a *"stack nearly exhausted"* warning here.

The shape is the lesson. The recursive form grows **faster than linearly** (2.5× more elements cost about 5× more time from 800 to 2000, because every level re-forms a pack of the remaining types, so the total work is about N²/2 type arguments), while the builtin is flat. And the depth limit is a real wall at the *default* setting: with default flags, a 5000-deep recursion is rejected by GCC at **900** and by Clang at **1024**:

```text
GCC:   fatal error: template instantiation depth exceeds maximum of 900 (use '-ftemplate-depth=' to increase the maximum)
Clang: fatal error: recursive template instantiation exceeded maximum depth of 1024
```

A runnable, deterministic part of this experiment: the **depth limit** itself.

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
template <int N> struct Depth { static constexpr int value = Depth<N - 1>::value + 1; };
template <> struct Depth<0> { static constexpr int value = 0; };
int main() {
    std::printf("Depth<500>::value = %d\n", Depth<500>::value);        // fine
    // std::printf("%d\n", Depth<5000>::value);                       // ERROR with default flags: GCC says 'depth exceeds maximum of 900'
    //   compile with  -ftemplate-depth=6000  to make it work (the limit is a compiler setting, not the standard)
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
Depth<500>::value = 500
```

---

## 8. Assembly / runtime investigation

TMP's runtime footprint is normally **zero**, which is exactly what you should verify:

```bash
# (1) Did any metafunction survive into the binary?  There should be nothing but your functions.
g++-14 -std=c++23 -O0 -c prog.cpp -o prog.o && nm -C prog.o | grep -ciE "CountIntegral|MaxSize|At<|List<"

# (2) What did TMP do to symbol names?  Long template-ids appear in mangled names of every instantiated function.
nm prog.o | awk '{ print length($3), $3 }' | sort -n | tail -3

# (3) Instantiation profile (GCC):  which templates are expensive?
g++-14 -std=c++23 -ftime-report -c prog.cpp -o /dev/null 2>&1 | grep -E "template instantiation|TOTAL"
# (Clang):  -ftime-trace  → open the JSON in chrome://tracing, "Total InstantiateClass/InstantiateFunction"
```

If (1) prints anything but your own functions, a *type* computation is leaking into run-time code (an un-inlined helper at `-O0` is the usual cause: re-check at `-O2`).

**Where TMP *does* reach the machine:** through the *types* it computes. A `std::tuple<Ts...>` made from a filtered list has exactly the layout of the filtered types; a policy-based `Widget<Logs, Counts>` has exactly the members of those bases (Experiment 4 prints the `sizeof`). TMP determines **layout and dispatch**, not run-time work.

---

## 9. Implementation exercise

1. **`tuple_cat_types`, `tuple_filter`**: given `std::tuple<Ts...>` and a predicate, produce the filtered `std::tuple` *type* and the filtered *value* (moving elements), using `index_sequence`.
2. **`visit` from scratch**: given a `std::variant<Ts...>` and an overloaded visitor, generate a *static table of function pointers* (`std::array<R(*)(Visitor&, V&), N>`) with one pack expansion; time it against a chain of `if`s for N = 4, 16, 64.
3. **A compile-time `Sort<List, Cmp>`**: first implement insertion sort recursively on a typelist (`Cmp` a metafunction on two types); then re-implement by **sorting an array of `{sizeof, alignof, index}`** in a `consteval` function and re-materializing the types with `pack_at`. Compare code size, compile time, N = 16/64/256.
4. **`static_for` and `static_switch`**: a `template for`-like utility using fold expressions, usable with *run-time breaks* (`return bool` from the body to stop).
5. **A small expression-template library** (vector `+`, `*`, scalar broadcast) with `static_assert` on the *type* of `a + b * 2` (tree shape), then a run-time benchmark vs naive temporaries.
6. **Detect, don't specialize**: write `has_serialize<T>` three ways (SFINAE `void_t`, `requires`, `if constexpr` with `is_detected`); compare compile-error quality on a type that almost has it.

<details>
<summary><strong>Solution sketch for the table-driven <code>visit</code> (item 2)</strong></summary>

```cpp
// @test run -std=c++23 -O0
#include <array>
#include <cstdio>
#include <string>
#include <utility>
#include <variant>

template <class R, class V, class F, std::size_t... Is>
constexpr auto make_table(std::index_sequence<Is...>) {
    // One function per alternative, each a separate instantiation, address taken into a constexpr array.
    return std::array<R (*)(F&&, V&), sizeof...(Is)>{
        +[](F&& f, V& v) -> R { return std::forward<F>(f)(*std::get_if<Is>(&v)); }...
    };
}

template <class F, class... Ts>
decltype(auto) my_visit(F&& f, std::variant<Ts...>& v) {
    using R = decltype(std::forward<F>(f)(std::get<0>(v)));              // assumes all alternatives return the same type
    static constexpr auto table = make_table<R, std::variant<Ts...>, F>(std::index_sequence_for<Ts...>{});
    return table[v.index()](std::forward<F>(f), v);                       // O(1) indirect call
}

struct Print {
    void operator()(int i)               const { std::printf("int %d\n", i); }
    void operator()(const std::string& s) const { std::printf("string %s\n", s.c_str()); }
    void operator()(double d)            const { std::printf("double %.1f\n", d); }
};

int main() {
    std::variant<int, std::string, double> v = std::string("hi");
    my_visit(Print{}, v);
    v = 3.5; my_visit(Print{}, v);
    v = 7;   my_visit(Print{}, v);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
string hi
double 3.5
int 7
```

The *pack expansion `…`* after the lambda creates **N distinct lambdas** (one per `Is`), each converting to a plain function pointer through unary `+`. That single expansion is the whole "jump table" mechanism; `std::visit` in libstdc++ is a more elaborate version of the same idea (it handles multiple variants by building a multi-dimensional table).

</details>

---

## 10. Real-world example

| Where | TMP in action |
|---|---|
| **`std::tuple`, `std::variant`, `std::visit`** | Pack expansion, index sequences, tables of function pointers |
| **`std::function`/`invoke_result`/`common_type`** | Signature introspection and type computation |
| **Eigen, Blaze, xtensor** | Expression templates: `a + b * c` has a *type* that encodes the whole tree, fused into one loop |
| **Boost.Hana, mp11** | Modern, pack-based metaprogramming (`mp_transform`, `mp_filter`) replacing MPL |
| **Qt** | `QMetaType` / `QtPrivate::List<Args...>`, `FunctionPointer<Func>`: Qt's `connect(&A::sig, &B::slot)` uses TMP to *type-check* signal/slot signatures at compile time (`QtPrivate::CheckCompatibleArguments`) |
| **pybind11** | `detail::make_caster<T>` and signature description built with type lists; argument unpacking via `index_sequence` |
| **Ranges** | Adaptor pipeline types (Chapter 14) |
| **Serialization / ORM / RPC** | Struct → field-list reflection (via `boost::pfr` tricks, or macros) → `tuple` of members: the use case reflection will absorb |

> **Opinion.** Write TMP *only when the answer is a type*, and write it as shallow as possible: aliases and one pack expansion beat recursion; **fold expressions beat recursive specialization; `consteval` beats both when the answer is a value.** Prefer **`mp11`-style flat algorithms** to hand-rolled ones for anything beyond a dozen lines. Every TMP library you write creates a maintenance cost: diagnostics are hostile, compile time grows silently, and the next engineer has to learn a new mini-language. If a *code generator* (or, soon, **reflection**) would be clearer, use that. And do not use TMP merely because it is "zero-cost": a virtual call is cheaper than a 3-second increase in a header every TU includes.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Recursive typelist on 1000+ elements | "template instantiation depth exceeds maximum" | `__type_pack_element`/`index_sequence`; flat algorithms; raise `-ftemplate-depth` only for diagnosis |
| Quadratic algorithms (`unique` on thousands of types) | Build minutes, GBs of RAM | Sort-based or hash-based formulations; do it as a `consteval` value computation |
| Metafunction with hidden side effects (friend injection, `__COUNTER__` tricks) | Order-dependent results, different on GCC/Clang | Don't; use explicit indices |
| `std::tuple_element<I, T>` with `I >= size` | Hard error far from the cause | `static_assert(I < N, "…")` in a wrapper |
| Using a fold with an **empty pack** and an operator without identity | "empty fold expression" error | Binary fold with an init value: `(args + ... + 0)` |
| Left vs right fold with a non-associative op | Different result | Pick deliberately; test with a non-commutative op |
| Deducing through an alias template | Deduction fails (aliases are not deducible) | Deduce on the underlying template |
| Comma-fold with overloaded `operator,` | Evaluation does something unexpected | `(void(expr), ...)` |
| Evaluation-order assumption in `f(g(args)...)` | Function-argument expansion order is *unspecified* (comma fold *is* ordered) | Use a braced init list or a comma fold when order matters |
| Printing types by `typeid(...).name()` | Mangled, drops cv/ref | `__PRETTY_FUNCTION__` / `boost::type_index` / Compiler Explorer |
| Exposing deep TMP types in a public header | ABI breakage, 5 KB symbol names, long compile | Hide behind an alias or type erasure |
| Reimplementing `make_index_sequence` recursively | O(N) instantiations, depth limit | Use the standard one |
| Writing TMP to compute a *value* | Needless complexity | `constexpr`/`consteval` function |

---

## 12. Exercises

1. **Port.** Take a Boost.MPL (or Loki) typelist example from the web (e.g. `mpl::vector<...>` + `transform` + `fold`) and port it to this chapter's style. Count lines, instantiations (`-ftime-report`), and compile time.
2. **Instrumented instantiation.** Add a `static_assert`-free "instantiation counter" using `__PRETTY_FUNCTION__` + the linker map to count how many distinct classes a recursive `At<N, L>` instantiates for N = 10, 100, 500; compare with the builtin.
3. **`unique` complexity.** Implement `unique` in three ways (accumulator recursion; sort-by-`typeid`-hash then dedupe in a `consteval` function; `mp11`-style). Measure N = 50, 200, 800.
4. **Tuple utilities.** Implement `tuple_transform`, `tuple_zip`, `tuple_apply_each`, and `tuple_reverse` with index sequences only (no recursion). `static_assert` the resulting types.
5. **`visit` benchmark.** Benchmark your table-driven `visit` against `std::visit` and an `if` chain, for N = 2, 8, 32 alternatives, uniformly random and heavily biased index distributions; inspect assembly (jump table vs compare chain).
6. **Overload detection.** Write `is_callable_with<F, Args...>` without `<type_traits>` (SFINAE and `requires` versions). Then `common_return_type<F, Ts...>`.
7. **Compile-time limits.** Find the depth at which your compiler fails for: (a) a recursive struct, (b) a recursive `constexpr` function (Chapter 16), (c) a recursive lambda via `auto`. Record the error text and the flag that raises each limit.
8. **Expression templates.** Implement lazy vector arithmetic. Show a case where it is *faster* than naive temporaries and one where it is *slower* or *dangerous* (hint: `auto x = a + b;` stores references to temporaries).

---

## 13. Challenge: a type-safe, zero-allocation event dispatcher

Build `Dispatcher<Events...>` where `Events...` are event types:

- `on<E>(handler)` registers a handler for event type `E` only; registering for a type not in the list is a **compile-time error** with a readable message (use a concept)
- `emit(e)` dispatches **without** any `std::function`, using a per-event-type fixed-capacity `std::array<Handler, N>` of **type-erased small callables** (see Chapter 21) or function pointers + context
- `emit` for a **runtime event tag** (e.g. from a network message): a `switch` generated by fold + `index_sequence` over the event list
- compile-time **introspection**: `Dispatcher<...>::event_count`, `handles<E>()`, `static_assert`s listing exactly which events a component handles
- show that a given component does not pay for events it does not handle (`sizeof`, and the assembly of `emit<E>` for an event with zero handlers)
- document compile-time and binary-size scaling for 4, 16, 64 event types

---

## 14. Knowledge check

1. Why is a function template instantiation "evaluation" in TMP? What property of metafunctions makes memoization safe?
2. For each, say how many instantiations it needs for N elements: recursive `At<N, L>`; `__type_pack_element`; a fold over `sizeof...`.
3. What does `(args + ...)` expand to for three arguments? And `(... + args)`? When does the difference matter?
4. Why are only `&&`, `||` and `,` allowed to fold over an empty pack without an initial value?
5. What is the evaluation order of `f(g(args)...)` versus `(g(args), ...)`?
6. Why does the standard library implement `make_index_sequence` with a builtin?
7. When is a `consteval` function a better tool than TMP, and when is it impossible to use?
8. What do you give up when you use an alias template instead of a struct with `::type`? What do you gain?
9. Why should you ask `is_invocable_v<F, Args...>` instead of "what is `F`'s signature"?
10. Explain how `overloaded : Fs... { using Fs::operator()...; }` works. What would go wrong without the `using`?
11. What does TMP cost at run time, and where could it still show up?
12. Name two developments (one C++20, one C++26) that shrink the need for classic TMP.

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Naming `Foo<Args>::type` forces instantiation of `Foo<Args>`, which determines `type`. Metafunctions are pure (same inputs → same class), so the compiler caches (hash-conses) each instantiation: it is evaluated once per TU.
2. Recursive: ~N class instantiations (depth N). `__type_pack_element`: 0 recursive instantiations, handled inside the compiler. Fold: 1 instantiation (one variable/function template) plus a single pack expansion.
3. `a1 + (a2 + a3)` and `(a1 + a2) + a3`. It matters for non-associative operations (subtraction, string concatenation with side effects, floating point rounding, `<<`) and for evaluation of overloaded operators.
4. Those are the only operators with a defined identity result for an empty fold (`true`, `false`, `void()`). For others the empty result would be meaningless, so you must supply an init value.
5. Function arguments are *unspecified order*; a comma fold evaluates left to right (the built-in comma sequences its operands).
6. A recursive library implementation costs O(N) instantiations and hits the depth limit; the builtin (`__integer_pack`) produces the whole sequence in one step.
7. Better when the *answer is a value*. Impossible when the answer must be a *type* (until reflection gives `std::meta::info` values).
8. Gain: no extra class instantiation, transparent equivalence, shorter. Lose: you can't specialize an alias, can't deduce through it, and can't forward-declare the result as a class.
9. Callables may be overloaded, generic, or have defaulted/variadic parameters: there is no single signature; invocability with specific arguments is well-defined and what callers actually need.
10. The class inherits every lambda's `operator()`; without the `using`, each base's `operator()` hides the others (name lookup finds the name in several bases → ambiguity), so no overload resolution happens across them.
11. Nothing: it is gone by code generation. It shows up only through the *types* it computes (layout of tuples/policy classes, dispatch tables, and long symbol names).
12. C++20 concepts/`requires` and `consteval` (removing SFINAE and value-level TMP); C++26 reflection (P2996) and pack indexing (P2662).

</details>

---

[← Previous: Chapter 16](16-constexpr.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 18 — The reflection landscape →](18-reflection-landscape.md)
