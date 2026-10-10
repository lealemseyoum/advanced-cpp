# Chapter 10 — Concepts

> **Part IV · Generic programming** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 6 hours**
> **Prerequisites:** [Chapter 8](08-templates-deep-dive.md), [Chapter 9](09-type-traits.md) &nbsp;|&nbsp; **Standards:** C++20 (+ library concepts), C++23 &nbsp;|&nbsp; **Tools:** `g++-14`, `clang++-18`

[← Previous: Chapter 9](09-type-traits.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 11 — Containers →](../part-05-standard-library/11-containers.md)

---

**In one sentence:** a *concept* is a **named, compile-time predicate on template arguments** that is checked at the point of use, ordered by logical strength, and reported in diagnostics, so that a template's *requirements* become part of its *interface*.

**By the end of this chapter you can:**

- write concepts using `requires`-expressions (simple, type, compound, nested requirements)
- constrain templates five ways (type-constraint, `requires` clause, trailing `requires`, abbreviated `auto`, constrained members) and know when each reads best
- explain **subsumption** and predict which constrained overload wins, including the case where it silently does *not*
- read and compare concept diagnostics on GCC and Clang
- separate **syntactic** from **semantic** requirements and document the latter
- compare, with numbers, SFINAE, concepts, runtime polymorphism and duck typing, and pick one

---

## 1. Problem

A template that says `template <class T>` promises to accept *any* type, then fails for most of them: late, inside the implementation, with a 100-line message about something you did not write.

```cpp
std::list<int> l;
std::sort(l.begin(), l.end());
```

```text
.../bits/stl_algo.h:1906:50: error: no match for 'operator-' (operand types are 'std::_List_iterator<int>' and ...)
.../bits/stl_iterator.h:618:5: note: candidate: 'template<class _IteratorL, class _IteratorR> ... operator-(const reverse_iterator ...'
... (many more notes about unrelated operator- candidates)
```

The *requirement* ("random-access iterators") is a fact about `std::sort`'s **interface**, but it exists only in documentation and in a failure deep in the body. That causes four distinct problems:

| Problem | Consequence |
|---|---|
| Requirements are **implicit** | Readers and tools cannot see them; IDEs cannot offer completion |
| Errors appear at the **wrong place** | You debug library internals instead of your call |
| **No overloading** on capability | You cannot write "one version for random-access ranges and another for forward ranges" without `enable_if` contortions |
| No **ordering** between alternatives | `enable_if` conditions must be made *mutually exclusive* by hand |

---

## 2. Historical context

| Year | Event |
|---|---|
| 1998 | STL documents iterator *categories* and "Container requirements" in prose: the concepts exist as **words in the standard** |
| 2003–2009 | **"C++0x concepts"**: a large design with `concept_map`s and separate checking of template definitions. **Removed from the draft in July 2009** (too complex, risk of slipping) |
| 2011–2014 | Library authors resort to SFINAE; Boost.ConceptCheck; range-v3 (Eric Niebler) builds a concept emulation layer |
| 2013 | **Concepts Lite** (Sutton, Stroustrup, Dos Reis): a much smaller design: *constraints on use only*, no concept maps, no definition checking |
| 2017 | Concepts TS published; GCC 6 implements it |
| **2020** | **C++20 concepts**, plus `<concepts>`, iterator and range concepts. Ranges (Chapter 14) are built on them |
| 2023 | `std::expected`, `std::move_only_function` use constrained constructors throughout |

The lesson in the history: the 2009 design tried to make *template definitions* type-checkable against their concepts. The adopted design checks only **whether the arguments satisfy the constraint at the call site**; the body is still checked at instantiation (so a template can use more than its concept promises, and nothing stops it). It is a weaker design and one that actually shipped.

---

## 3. Modern solution

```cpp
template <class T>
concept Addable = requires(T a, T b) {            // a named predicate...
    { a + b } -> std::convertible_to<T>;          // ...built from requirements on expressions
};

template <Addable T>                              // constrained template parameter
T sum2(T a, T b) { return a + b; }

auto twice(std::integral auto x) { return x * 2; }          // abbreviated function template

template <class T> requires std::is_trivially_copyable_v<T> // requires-clause with any constant expression
void relocate(T*, T*, std::size_t);
```

A failed call now says **which concept**, and **which requirement**:

```text
GCC 14   : note: constraints not satisfied
           required for the satisfaction of 'Addable<T>' [with T = Opaque]
           note: the required expression '(a + b)' is invalid
Clang 18 : note: candidate template ignored: constraints not satisfied [with T = Opaque]
           note: because 'Opaque' does not satisfy 'Addable'
           note: because 'a + b' would be invalid: invalid operands to binary expression ('Opaque' and 'Opaque')
```

---

## 4. Mental model

### A concept is a function from types to `bool`, with an *interface-shaped* body

```text
   template <class T> concept C = <constraint-expression>;
                                    │
                                    ▼
          a boolean expression made of ATOMIC constraints combined with  &&  ||  !(within an atom only)

   satisfied?   substitute T into each atom:
                  · expression is ill-formed        → atom is FALSE (not an error!)
                  · expression is well-formed       → evaluate to bool (must be exactly bool, no conversion)
```

The key difference from a `static_assert`: **an ill-formed expression inside a constraint is not an error; it is the answer *false***. That is exactly what SFINAE did, except now it is a first-class language feature with a name, and it can be **reasoned about**.

### Three layers of a constraint

| Layer | What it expresses | Example |
|---|---|---|
| **Syntactic** | "These expressions are valid and have these types" | `requires(T a) { ++a; a++; *a; }` |
| **Semantic** | "...and they mean what you expect" | `a == b` is an equivalence relation; `++a` returns `a` itself |
| **Complexity** | "...at this cost" | `++it` is O(1); `vector::size()` is O(1) |

**The compiler checks only the syntactic layer.** Semantic and complexity requirements are *documented* contracts: violating them is undefined behavior or just wrong results, and no diagnostic exists (§7, Experiment 4). Concepts like `std::regular`, `std::totally_ordered`, `std::sortable` carry a semantic part in prose.

### Subsumption: concepts are *ordered*

```text
          std::integral<T>                 less constrained
               ▲
               │  (subsumes: every T satisfying the one below satisfies this)
     std::signed_integral<T>  =  integral<T> && is_signed_v<T>
               ▲
               │
       (your concept)  =  signed_integral<T> && (sizeof(T) >= 4)
                                                       more constrained  →  preferred by overload resolution
```

When two constrained templates are both viable and otherwise equally good, the **more constrained** one wins, with no need to write mutually-exclusive conditions. That single feature (§5.4) is what replaces tag dispatch and `enable_if` ladders.

---

## 5. Language rules

### 5.1 Defining a concept  `[temp.concept]`

```cpp
template <class T> concept Name = constraint-expression;      // must be a bool constant expression
```

- A concept **is not a type, not a variable you can modify and cannot be specialized**. It cannot be recursive.
- It **can have several template parameters**: `template <class T, class U> concept SameSize = sizeof(T) == sizeof(U);` and the first parameter is the one "constrained" in `Name T`.
- It cannot be constrained itself (`template <class T> requires X<T> concept ...` is ill-formed).

### 5.2 Requires-expressions  `[expr.prim.req]`

```cpp
requires (parameter-list) { requirement-seq }       // a prvalue bool: true iff all requirements hold
```

| Requirement | Syntax | Satisfied when |
|---|---|---|
| **Simple** | `expr;` | the expression is **valid** (not evaluated) |
| **Type** | `typename T::value_type;` | the type name is valid |
| **Compound** | `{ expr } noexcept -> TypeConstraint;` | valid; `noexcept` if stated; the expression's type, **as `decltype((expr))`**, satisfies the constraint |
| **Nested** | `requires constant-expr;` | the constant expression is `true` |

Details that bite:

- In `{ a + b } -> std::same_as<int>;` the left side is `decltype((a + b))`, so for an lvalue-returning expression it is `int&` and `same_as<int>` **fails**. Write `std::same_as<int&>` or `std::convertible_to<int>`.
- Parameters in the list are *not* variables with real storage; they're like `declval`s (unevaluated, any value category the type implies).
- Substitution failures **in a nested requirement's expression** are not soft: `requires sizeof(T) > 2;` is a nested requirement that evaluates a constant; a hard error inside it is a hard error.
- Putting `requires requires` — "requires-clause containing a requires-expression" — is correct when you want an ad-hoc inline constraint: `template <class T> requires requires(T t) { t.foo(); } void f(T);`. It looks silly and is fine.

### 5.3 Five ways to constrain a template

```cpp
// (1) type-constraint in the parameter list   [most readable for one concept on one parameter]
template <std::integral T> T f1(T);

// (2) requires-clause after the template header   [general: any bool constant expression, &&, ||]
template <class T> requires std::integral<T> && (sizeof(T) >= 4)  T f2(T);

// (3) trailing requires-clause   [can mention function parameters/members, needed for member functions]
template <class T> T f3(T x) requires std::integral<T>;

// (4) abbreviated function template (placeholder)   [no template header at all]
auto f4(std::integral auto x) { return x; }

// (5) constrained member of an unconstrained class template   [conditional API]
template <class T> struct Box {
    void print() const requires std::formattable<T, char> { /* … */ }     // only exists when T is formattable
};
```

Grammar notes: in a *requires-clause* the expression must be a **primary expression** or a parenthesized one joined by `&&`/`||`. `requires std::is_integral_v<T> && !std::is_same_v<T, bool>` is **ill-formed** as written, since `!` begins a non-primary expression; write `requires std::is_integral_v<T> && (!std::is_same_v<T, bool>)`. This trips up everyone once.

### 5.4 Constraint normalization and subsumption  `[temp.constr]`

To compare constraints, the compiler **normalizes** each into a boolean formula over *atomic constraints*:

- `C<T> && D<T>` normalizes to the **conjunction** of normalizations of `C<T>`, `D<T>` (the concept body is expanded)
- `C<T> || D<T>` to the **disjunction**
- anything else, including `!C<T>` and any expression like `sizeof(T) > 2` or `std::is_integral_v<T>`, is an **atomic constraint**

`P` **subsumes** `Q` if, treating atomic constraints as opaque propositions, `P ⇒ Q` in propositional logic. Two atoms are "the same" **only if they come from the same expression at the same source location** (the same concept definition, expanded). *Textually identical expressions in two different places are different atoms.*

**Consequence 1 (works).** A concept built from another subsumes it:

```cpp
template <class T> concept Integral2 = std::is_integral_v<T>;
template <class T> concept Signed2   = Integral2<T> && std::is_signed_v<T>;     // subsumes Integral2

void g(Integral2 auto)  { /* general */ }
void g(Signed2   auto)  { /* preferred when both are viable */ }
```

**Consequence 2 (the trap).** If you *repeat* a boolean expression instead of naming it, subsumption fails and the call is ambiguous:

```cpp
template <class T> requires std::is_integral_v<T>                        void h(T) {}
template <class T> requires std::is_integral_v<T> && std::is_signed_v<T> void h(T) {}   // different source location for is_integral_v!
// h(1) → error: call of overloaded 'h(int)' is ambiguous
```

**Rule: to be ordered by subsumption, constraints must be built from the *same named concepts*, not from the same text.** (Using a concept like `std::integral<T>` is atomic for the standard library's `is_integral_v`: `std::integral<T>` is `is_integral_v<T>`, and any other `std::integral` appearance subsumes correctly because it's the *same concept*.) Experiment 2 shows it running.

### 5.5 Where constraints are checked

| Context | Checked |
|---|---|
| Function template call | During overload resolution: after deduction and substitution; failure removes the candidate |
| Class template | When the class is **named with arguments** (`Box<int>`): the constraint must hold, a hard error otherwise (no overloading, only specialization) |
| Member functions of class templates | Only when that member is **used** (the constrained member is dropped from the overload set when false) |
| Partial specializations | Constraint participates in "more specialized" ordering, which replaces `enable_if` as an extra defaulted parameter |
| `if constexpr (requires { … })` | Anywhere: concept as an expression gives a `bool` |
| Destructors, special members (C++20, P0848) | Can be constrained: *conditionally trivial* special members (Experiment 6) |

### 5.6 Library concepts you will meet daily (`<concepts>`, `<iterator>`, `<ranges>`)

| Group | Concepts |
|---|---|
| Core language | `same_as`, `derived_from`, `convertible_to`, `common_reference_with`, `integral`, `signed_integral`, `floating_point`, `assignable_from`, `swappable`, `destructible`, `constructible_from`, `default_initializable`, `move_constructible`, `copy_constructible` |
| Comparison | `equality_comparable`, `totally_ordered`, `three_way_comparable` |
| **Object** | `movable`, `copyable`, `semiregular` (copyable + default-init), **`regular`** (semiregular + equality-comparable) |
| Callable | `invocable`, `regular_invocable`, `predicate`, `relation`, `strict_weak_order` |
| Iterator | `input_iterator` → `forward_iterator` → `bidirectional_iterator` → `random_access_iterator` → `contiguous_iterator`; `sentinel_for`, `indirectly_readable`, `sortable`, `mergeable` |
| Range | `range`, `input_range`, …, `contiguous_range`, `sized_range`, `view`, `borrowed_range`, `common_range` |

`std::regular` is the formalization of *value semantics*: it is Stepanov's "regular type" turned into a concept (copyable, default-constructible, equality-comparable, assignment preserves equality). It is what [Chapter 6](../part-03-value-categories-and-move/06-move-semantics.md)'s whole value-semantics story points at.

### 5.7 Concepts and `auto`

```cpp
std::integral auto n = f();        // a constrained placeholder: n has the deduced type, which must satisfy integral
std::ranges::range auto r = g();   // works for variables, return types, structured bindings, lambda params
auto lam = [](std::floating_point auto x) { return x / 2; };
```

A **constrained placeholder** is also documentation: `std::integral auto n` tells the reader more than `auto n`, with no cost.

### Layer check

| Layer | Decides |
|---|---|
| **Standard** | Syntax of requires-expressions, normalization and subsumption, satisfaction rules, the library concepts' *semantic* wording |
| **Compiler** | Diagnostic quality (GCC prints a chain of "required for the satisfaction of…", Clang a "because…" chain), caching of satisfaction results, and `-fconcepts-diagnostics-depth=N` (GCC) |
| **ABI** | **Nothing observable**: constraints affect the *mangling* of constrained templates (the `requires` clause is part of the template's identity, mangled since Itanium ABI additions), so changing a constraint is an ABI-visible change for *exported template instantiations* |
| **CPU** | Nothing. Concepts are erased: a constrained and unconstrained function generate identical code |

---

## 6. Implementation model

At a call, the compiler:

1. performs ordinary overload resolution for the candidate templates (deduction, substitution of the signature)
2. for each surviving candidate, **checks the constraints** (evaluating atoms in order, left to right, short-circuiting `&&`/`||`), and caches the result per (concept, arguments)
3. if several remain *and are otherwise equal*, picks by **subsumption**

The check happens **before** the body is instantiated. That is why the diagnostics can mention only the requirement, and why a constrained template is cheaper to reject than an SFINAE-ed one (no class template instantiation chain; evaluation of atoms is cached per argument list).

Compile time: concept evaluation is typically **faster** than the equivalent `enable_if`/`void_t` chain, because each `void_t<...>` instantiates a class template, while an atom is a direct expression check. It can still be quadratic if you build deeply nested concept hierarchies on expensive atoms (`std::sortable` pulls in `indirectly_*`, roughly 50 atoms): measure.

---

## 7. Experiments

### Experiment 1: Diagnostics: unconstrained vs constrained

Three failing calls, same underlying mistake (sorting a `std::list`), differing in how the library declares its requirements. Output is real, trimmed.

```cpp
// @test fail -std=c++23 err=constraints
#include <algorithm>
#include <list>

int main() {
    std::list<int> l;
    std::ranges::sort(l);        // constrained: random_access_range
}
```

**`std::sort(l.begin(), l.end())` (unconstrained)**, GCC 14: 29 lines, first error inside `stl_algo.h`:

```text
bits/stl_algo.h:1906:50: error: no match for 'operator-' (operand types are 'std::_List_iterator<int>' and 'std::_List_iterator<int>')
bits/stl_iterator.h:618:5: note: candidate: 'template<class _IteratorL, class _IteratorR> constexpr decltype ((__y.base() - __x.base())) std::operator-(...)'
...
```

**`std::ranges::sort(l)` (constrained)**, Clang 18:

```text
error: no matching function for call to object of type 'const __sort_fn'
note: candidate template ignored: constraints not satisfied [with _Range = std::list<int> &, ...]
note: because 'std::list<int> &' does not satisfy 'random_access_range'
note: because 'iterator_t<list<int>&>' (aka '_List_iterator<int>') does not satisfy 'random_access_iterator'
note: because '__is_base_of(std::random_access_iterator_tag, std::bidirectional_iterator_tag)' evaluated to false
```

The error points at **your call**, names **the requirement** (`random_access_range`), and drills down until the actual cause: a list iterator's category is `bidirectional`, not `random_access`. GCC prints the same chain as "required for the satisfaction of …" (reading bottom-up). If GCC's output is too shallow, raise it with `-fconcepts-diagnostics-depth=3`.

> **Verdict.** For **API authors**, this is the strongest argument for concepts: your error messages become a *feature* you can design. Write the concept first, and name it for the *capability* (`Serializable`, `Allocator`), not for the implementation (`HasToStringAndSizeMethods`).

### Experiment 2: Overloading by subsumption, and the textual-repeat trap

```cpp
// @test run -std=c++23 -O0
#include <concepts>
#include <cstdio>
#include <type_traits>
#include <vector>
#include <list>

// ---- (a) subsumption through NAMED concepts: works ----
template <class T> concept Integral2 = std::is_integral_v<T>;
template <class T> concept Signed2   = Integral2<T> && std::is_signed_v<T>;

void g(Integral2 auto)  { std::puts("  g(Integral2)  [less constrained]"); }
void g(Signed2 auto)    { std::puts("  g(Signed2)    [more constrained: wins when both match]"); }

// ---- (b) the iterator-category ladder, written with concepts instead of tag dispatch ----
template <std::input_iterator I>        void advance2(I& it, int n) { std::puts("  advance2: input    O(n) ++"); while (n--) ++it; }
template <std::random_access_iterator I> void advance2(I& it, int n) { std::puts("  advance2: random   O(1) +="); it += n; }
// (bidirectional: could be added as a third overload; random_access subsumes bidirectional subsumes input)

// ---- (c) the same-text trap: the compiler cannot see these two are "ordered" ----
template <class T> requires std::is_integral_v<T>                          void h(T) { std::puts("  h(1)"); }
template <class T> requires std::is_integral_v<T> && std::is_signed_v<T>   void h(T) { std::puts("  h(2)"); }
// ---- (d) the fix: name it, or use the concept both times ----
template <std::integral T>                                  void k(T) { std::puts("  k(integral)"); }
template <std::integral T> requires std::is_signed_v<T>     void k(T) { std::puts("  k(integral && signed)  wins"); }

int main() {
    std::puts("(a) named concepts");
    g(1);            // int is both: Signed2 wins
    g(1u);           // unsigned: only Integral2
    std::puts("(b) iterator categories");
    std::vector<int> v(10); std::list<int> l(10);
    auto vi = v.begin(); auto li = l.begin();
    advance2(vi, 5); advance2(li, 5);
    std::puts("(d) fix: reuse the concept");
    k(1); k(1u);
    // h(1);   // would be ambiguous: see the next snippet
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
(a) named concepts
  g(Signed2)    [more constrained: wins when both match]
  g(Integral2)  [less constrained]
(b) iterator categories
  advance2: random   O(1) +=
  advance2: input    O(n) ++
(d) fix: reuse the concept
  k(integral && signed)  wins
  k(integral)
```

Case (c) demonstrates the rule from §5.4: even though *you* can see `h(2)` is stricter, the compiler treats the two `std::is_integral_v<T>` expressions as unrelated atoms. Case (d) works because `std::integral<T>` is **one concept** referenced from both places.

The two `h` templates are declared, and individually valid; only a *call* is ambiguous. Here it is, as a compile-time failure:

```cpp
// @test fail -std=c++23 err=ambiguous
#include <type_traits>
template <class T> requires std::is_integral_v<T>                          void h(T) {}
template <class T> requires std::is_integral_v<T> && std::is_signed_v<T>   void h(T) {}
int main() { h(1); }      // error: call of overloaded 'h(int)' is ambiguous
```

### Experiment 3: The anatomy of a `requires` expression

```cpp
// @test run -std=c++23 -O0
#include <concepts>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

// 1. simple requirement: expression validity only
template <class T> concept Incrementable = requires(T t) { ++t; t++; };

// 2. type requirement
template <class T> concept HasValueType = requires { typename T::value_type; };

// 3. compound requirement: validity + return-type constraint (+ noexcept)
template <class T> concept Hashable = requires(const T& t) {
    { t.hash() } noexcept -> std::convertible_to<std::size_t>;
};

// 4. nested requirement: an extra bool expression
template <class T> concept SmallTrivial = requires { requires sizeof(T) <= 16 && std::is_trivially_copyable_v<T>; };

// 5. the decltype((expr)) subtlety:   { t[0] } -> same_as<int>  FAILS for lvalue-returning operator[]
template <class T> concept IndexesToIntExact = requires(T t) { { t[0] } -> std::same_as<int>;  };
template <class T> concept IndexesToIntRef   = requires(T t) { { t[0] } -> std::same_as<int&>; };
template <class T> concept IndexesToIntAny   = requires(T t) { { t[0] } -> std::convertible_to<int>; };

// 6. constraints on several types
template <class From, class To> concept LosslessTo = std::convertible_to<From, To> && (sizeof(From) <= sizeof(To));

struct H1 { std::size_t hash() const noexcept { return 1; } };
struct H2 { std::size_t hash() const         { return 2; } };      // may throw → fails `noexcept`
struct H3 { int hash() const noexcept { return 3; } };             // int converts to size_t

#define T(...) std::printf("  %-52s %d\n", #__VA_ARGS__, int(__VA_ARGS__))

int main() {
    T(Incrementable<int>);              T(Incrementable<std::string>);
    T(HasValueType<std::vector<int>>);  T(HasValueType<int>);
    T(Hashable<H1>);  T(Hashable<H2>);  T(Hashable<H3>);  T(Hashable<int>);
    T((SmallTrivial<double>));          T(SmallTrivial<std::string>);
    T(IndexesToIntExact<std::vector<int>>);   // t[0] is int& → decltype((t[0])) = int&  → not same_as<int>
    T(IndexesToIntRef<std::vector<int>>);
    T(IndexesToIntAny<std::vector<int>>);
    T((LosslessTo<int, long>));         T((LosslessTo<long, int>));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
  Incrementable<int>                                   1
  Incrementable<std::string>                           0
  HasValueType<std::vector<int>>                       1
  HasValueType<int>                                    0
  Hashable<H1>                                         1
  Hashable<H2>                                         0
  Hashable<H3>                                         1
  Hashable<int>                                        0
  (SmallTrivial<double>)                               1
  SmallTrivial<std::string>                            0
  IndexesToIntExact<std::vector<int>>                  0
  IndexesToIntRef<std::vector<int>>                    1
  IndexesToIntAny<std::vector<int>>                    1
  (LosslessTo<int, long>)                              1
  (LosslessTo<long, int>)                              0
```

`H2` fails `Hashable` *only* because its `hash()` isn't `noexcept`: the compound requirement said `noexcept`. The `IndexesToIntExact` row is the one that breaks code in practice: **write the return-type constraint as `convertible_to<X>` unless you truly require an exact type, including its value category**.

### Experiment 4: What concepts *cannot* check: semantic requirements

```cpp
// @test run -std=c++23 -O0
#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdio>
#include <vector>

// 1. float satisfies std::equality_comparable: syntactically fine, semantically broken by NaN
static_assert(std::equality_comparable<double>);
static_assert(std::totally_ordered<double>);

// 2. a "less" that is not a strict weak ordering satisfies the SYNTACTIC part of strict_weak_order
auto not_swo = [](int a, int b) { return a <= b; };                 // <= is not irreflexive
static_assert(std::strict_weak_order<decltype(not_swo), int, int>);

int main() {
    double nan = std::nan("");
    std::printf("nan == nan : %d   (equality_comparable<double> is satisfied!)\n", nan == nan);

    // 3. ranges::sort accepts the bogus comparator; the result is UNDEFINED (it can even crash on big inputs).
    std::vector<int> v(40, 7);
    std::ranges::sort(v, not_swo);          // compiles; behavior is undefined (introsort may run past the ends)
    std::puts("ranges::sort(v, <=) compiled and ran: the concept could not protect us");

    // 4. a std::vector<double> containing NaN sorted with operator<: also not a strict weak order
    std::vector<double> d = {3, nan, 1, 2};
    std::ranges::sort(d);
    std::printf("sorted with a NaN present: %.0f %.0f %.0f %.0f  (unspecified: not sorted)\n", d[0], d[1], d[2], d[3]);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
nan == nan : 0   (equality_comparable<double> is satisfied!)
ranges::sort(v, <=) compiled and ran: the concept could not protect us
sorted with a NaN present: 1 3 nan 2  (unspecified: not sorted)
```

Concepts **narrow the set of programs that compile**; they do not make the remaining ones correct. Treat each library concept as *syntax + a promise from you*. To find the violation, use `-D_GLIBCXX_DEBUG` (libstdc++ checks comparator validity in debug mode: `__glibcxx_requires_irreflexive_pred` and aborts with "comparison doesn't meet irreflexive requirements") and the sanitizers of Chapter 28:

```bash
g++-14 -std=c++23 -D_GLIBCXX_DEBUG -fsanitize=address,undefined exp4.cpp && ./a.out
```

### Experiment 5: Four ways to say "anything drawable": cost and consequences

The spec asks for a comparison between **SFINAE, concepts, runtime polymorphism and duck typing**. Here is the same function written each way, and then measured:

```cpp
// @test run -std=c++23 -O2
#include <chrono>
#include <concepts>
#include <cstdio>
#include <memory>
#include <type_traits>
#include <vector>

struct Circle { double r;  double area() const { return 3.14159 * r * r; } };
struct Square { double s;  double area() const { return s * s; } };

// (1) Duck typing: an unconstrained template. No declared requirement; errors appear in the body.
template <class S> double total_duck(const std::vector<S>& v) { double t = 0; for (auto& s : v) t += s.area(); return t; }

// (2) SFINAE: the requirement is a hack in the signature.
template <class S, class = std::void_t<decltype(std::declval<const S&>().area())>>
double total_sfinae(const std::vector<S>& v) { double t = 0; for (auto& s : v) t += s.area(); return t; }

// (3) Concepts: the requirement is named, checked, and part of the interface.
template <class S> concept Shape = requires(const S& s) { { s.area() } -> std::convertible_to<double>; };
template <Shape S> double total_concept(const std::vector<S>& v) { double t = 0; for (auto& s : v) t += s.area(); return t; }

// (4) Runtime polymorphism: one implementation, a heterogeneous container, a virtual call per element.
struct IShape { virtual ~IShape() = default; virtual double area() const = 0; };
struct VCircle : IShape { double r; explicit VCircle(double r) : r(r) {} double area() const override { return 3.14159 * r * r; } };
struct VSquare : IShape { double s; explicit VSquare(double s) : s(s) {} double area() const override { return s * s; } };
double total_virtual(const std::vector<std::unique_ptr<IShape>>& v) { double t = 0; for (auto& s : v) t += s->area(); return t; }

template <class F> double time_ms(F&& f, double& sink) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) sink += f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int main() {
    constexpr int N = 1'000'000;
    std::vector<Circle> circles(N, Circle{1.5});
    std::vector<std::unique_ptr<IShape>> shapes;
    shapes.reserve(N);
    for (int i = 0; i < N; ++i) {
        if (i % 2) shapes.push_back(std::make_unique<VCircle>(1.5));
        else       shapes.push_back(std::make_unique<VSquare>(1.5));
    }

    double sink = 0;
    double a = time_ms([&] { return total_duck(circles); },    sink);
    double b = time_ms([&] { return total_sfinae(circles); },  sink);
    double c = time_ms([&] { return total_concept(circles); }, sink);
    double d = time_ms([&] { return total_virtual(shapes); },  sink);

    std::printf("200 passes over 1e6 shapes   (sink=%g)\n", sink > 0 ? 1.0 : 0.0);
    std::printf("  duck typing  (template)   : %7.1f ms\n", a);
    std::printf("  SFINAE       (template)   : %7.1f ms\n", b);
    std::printf("  concepts     (template)   : %7.1f ms\n", c);
    std::printf("  virtual, heterogeneous    : %7.1f ms\n", d);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
200 passes over 1e6 shapes   (sink=1)
  duck typing  (template)   :   140.8 ms
  SFINAE       (template)   :   128.0 ms
  concepts     (template)   :   141.9 ms
  virtual, heterogeneous    :  1033.2 ms
```

*How to read this.* The three template versions are **the same machine code** (check it with `-S`): the constraint costs nothing at runtime because it exists only during overload resolution. The virtual version is slower here for **two reasons that should not be confused**: (1) the dispatch itself, an indirect call per element which the compiler cannot inline or vectorize, and (2) **memory layout**: 10⁶ separately allocated heap objects instead of one contiguous array of doubles. Chapter 27 separates the two; the cache effect dominates. The timings are indicative, not a rigorous benchmark (Chapter 39 explains what this one omits: pinning, warm-up, variance).

| | Duck typing | SFINAE | **Concepts** | Virtual |
|---|---|---|---|---|
| Requirement visible in the signature | ❌ | ⚠️ as machinery | ✅ **named** | ✅ (base class) |
| Error message at the call site | ❌ deep inside | ⚠️ "no matching function" | ✅ names the failing requirement | ✅ clear |
| Overload by capability | ❌ | ⚠️ hand-made exclusivity | ✅ **subsumption** | ✅ via the hierarchy |
| Works with types you can't modify (`int`, a library's `std::string`) | ✅ | ✅ | ✅ | ❌ needs an adapter/wrapper |
| Heterogeneous container (`vector<Shape>` of mixed types) | ❌ | ❌ | ❌ | ✅ |
| Runtime cost per call | 0, inlinable | 0, inlinable | 0, inlinable | indirect call + cache effects |
| Binary compatibility (can ship in a `.so` with a stable ABI) | ❌ templates are in headers | ❌ | ❌ | ✅ (vtable ABI, with discipline) |
| Compile-time cost | low | **high** | moderate | none (separate TU) |
| Code size | one copy per type | one per type | one per type | one |

> **Verdict.** Use **concepts** whenever you want a template. Use **virtual functions** when you need a *heterogeneous collection at runtime* or an *ABI boundary*. Use **type erasure** (Chapter 21) when you want *value semantics with runtime polymorphism*. **Do not write new `enable_if`/`void_t` code in a C++20 project** (except in a header that must still compile as C++17). Duck typing is *concepts without the name*: acceptable in a private helper, not in a public interface.

### Experiment 6: Constrained special members: conditionally trivial types

C++20 lets you constrain special member functions, giving **multiple overloads of the destructor or copy constructor**; the compiler picks the *eligible* one. That's how `std::optional<T>` can be *trivially destructible exactly when `T` is*, without a partial-specialization pyramid:

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

template <class T>
class MiniOptional {
    union { T value_; };
    bool has_ = false;
public:
    constexpr MiniOptional() noexcept {}
    template <class U = T> requires std::constructible_from<T, U>
    constexpr MiniOptional(U&& u) : value_(std::forward<U>(u)), has_(true) {}

    // Two destructors, selected by constraint (most constrained eligible wins):
    constexpr ~MiniOptional() requires std::is_trivially_destructible_v<T> = default;     // trivial when possible
    constexpr ~MiniOptional() { if (has_) value_.~T(); }                                   // otherwise do the work

    MiniOptional(const MiniOptional&) requires std::is_trivially_copy_constructible_v<T> = default;
    MiniOptional(const MiniOptional& o) : has_(o.has_) { if (has_) ::new (&value_) T(o.value_); }

    bool has_value() const { return has_; }
};

int main() {
    std::printf("MiniOptional<int>:         trivially destructible=%d trivially copyable=%d\n",
        std::is_trivially_destructible_v<MiniOptional<int>>, std::is_trivially_copyable_v<MiniOptional<int>>);
    std::printf("MiniOptional<std::string>: trivially destructible=%d trivially copyable=%d\n",
        std::is_trivially_destructible_v<MiniOptional<std::string>>, std::is_trivially_copyable_v<MiniOptional<std::string>>);

    MiniOptional<std::string> s{std::string("hello")};
    MiniOptional<std::string> c = s;
    std::printf("copy has_value=%d\n", c.has_value());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
MiniOptional<int>:         trivially destructible=1 trivially copyable=1
MiniOptional<std::string>: trivially destructible=0 trivially copyable=0
copy has_value=1
```

Triviality **matters at the ABI level** (Chapter 37): in the Itanium C++ ABI, a type with a *trivial* copy constructor and destructor can be passed in registers; anything else is passed by hidden pointer. `std::optional<int>` is passed in registers *because* of this; pre-C++20 this needed a hand-built hierarchy of base classes (see libstdc++'s `_Optional_payload`). With constrained special members the standard library can progressively simplify those implementations.

### Experiment 7: Concepts inside the function body: `if constexpr (requires …)`

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <concepts>
#include <string>
#include <vector>

// One generic "describe" that adapts to what T offers, with no overloads at all:
template <class T>
std::string describe(const T& x) {
    if constexpr (requires { x.to_string(); })                       return x.to_string();
    else if constexpr (requires { std::to_string(x); })               return std::to_string(x);
    else if constexpr (requires { std::string(x); })                  return std::string(x);
    else if constexpr (requires { x.begin(); x.end(); })              return "range[" + std::to_string(std::distance(x.begin(), x.end())) + "]";
    else                                                              return "<opaque>";
}

struct Money { long cents; std::string to_string() const { return "$" + std::to_string(cents / 100) + "." + std::to_string(cents % 100); } };
struct Opaque {};

int main() {
    std::printf("%s | %s | %s | %s | %s\n",
        describe(Money{1999}).c_str(), describe(42).c_str(), describe("text").c_str(),
        describe(std::vector<int>{1, 2, 3}).c_str(), describe(Opaque{}).c_str());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
$19.99 | 42 | text | range[3] | <opaque>
```

A `requires`-expression is a **boolean expression you can use anywhere** (`if constexpr`, `static_assert`, a `constexpr bool`). This is the cleanest replacement for the `void_t` detection idiom of Chapter 9 when the question is local to one function. Place the *order* of the branches deliberately: the first matching one wins, which is a design decision (should `to_string()` beat `std::to_string(x)`?), not an accident.

---

## 8. Assembly / runtime investigation

Prove that constraints leave **no trace** in the code. Compile the unconstrained and constrained version of the same function and compare:

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=plain_twice,constrained_twice
#include <concepts>

template <class T> T twice_impl(T x) { return x + x; }

int plain_twice(int x)                       { return twice_impl(x); }
template <std::integral T> T twice_c(T x)    { return x + x; }
int constrained_twice(int x)                 { return twice_c(x); }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
plain_twice(int):
	lea	eax, [rdi+rdi]
	ret

constrained_twice(int):
	lea	eax, [rdi+rdi]
	ret
```

Identical. Where concepts **do** leave a trace is the *symbol name*: constrained templates mangle the constraint into the name (`_Z7twice_cITkSt8integraliET_S0_`). Look for the `Tk` with `nm`. Consequences: **changing a constraint on an exported template is an ABI change** for any shared library that exports an instantiation; and the same function *with* and *without* a constraint are different overloads that can coexist.

```bash
nm m.o | grep twice_c
```

```text
0000000000000000 W _Z7twice_cITkSt8integraliET_S0_
0000000000000000 W _Z7twice_cIiET_S0_
```

Two symbols at the same address: the constrained name (`Tk` = type-constraint, then `St8integral` = `std::integral`) and an **alias without the constraint**, which GCC 14 emits for compatibility with objects built before constraints were mangled. Clang emits only the constrained name. `c++filt` 2.42 does not decode the `Tk` form and prints it unchanged, and `nm -C` shows just `int twice_c<int>(int)` for the alias. Treat the exact scheme as a compiler/ABI detail that is still settling.

---

## 9. Implementation exercise

Build a miniature constrained library:

1. **A concept hierarchy**: `Readable<T>` (`T::value_type`, `*t`), `Incrementable<T>`, `InputIt<T>` = both + `==` against a sentinel, `ForwardIt<T>` = `InputIt` + copyable + multi-pass guarantee (a *semantic* requirement: document it), `RandomIt<T>` = `ForwardIt` + `+= n`, `-`, `[]`.
2. **`my::advance(it, n)`, `my::distance(first, last)`**: each with three overloads (input, bidirectional/forward, random) selected **only** by subsumption (no tag dispatch).
3. **`my::sort(first, last, cmp)` constrained** by `RandomIt` and `strict_weak_order`; use a simple insertion sort + heap fallback.
4. **Test** your concepts against `std::vector`, `std::list`, `std::forward_list`, `std::istream_iterator<int>`, a `int*`, and a hand-written input iterator. Print the highest category each satisfies, then compare with `std::iterator_traits<I>::iterator_category`.
5. **Break each one**: remove an operator from your test iterator and read the diagnostic. Is it good? If not, split the concept into smaller named parts until it is.

<details>
<summary><strong>Solution: advance/distance by subsumption and the category printer</strong></summary>

```cpp
// @test run -std=c++23 -O0
#include <concepts>
#include <cstdio>
#include <forward_list>
#include <iterator>
#include <list>
#include <sstream>
#include <vector>

namespace my {

template <class I> concept Incrementable = requires(I i) { { ++i } -> std::same_as<I&>; i++; };
template <class I> concept Readable      = requires(const I i) { *i; };
template <class I> concept InputIt       = Incrementable<I> && Readable<I> && std::equality_comparable<I>;
// SEMANTIC (not checkable): a forward iterator must allow multiple passes: copies of `it` traverse the same sequence.
template <class I> concept ForwardIt     = InputIt<I> && std::copyable<I> && std::default_initializable<I>;
template <class I> concept BidirIt       = ForwardIt<I> && requires(I i) { { --i } -> std::same_as<I&>; };
template <class I> concept RandomIt      = BidirIt<I> && requires(I i, I j, std::ptrdiff_t n) {
    { i += n } -> std::same_as<I&>;
    { i - j }  -> std::convertible_to<std::ptrdiff_t>;
    i[n];
};

template <InputIt I>   void advance(I& it, std::ptrdiff_t n) { std::printf("    [input   ] "); while (n-- > 0) ++it; }
template <BidirIt I>   void advance(I& it, std::ptrdiff_t n) { std::printf("    [bidir   ] "); if (n >= 0) while (n--) ++it; else while (n++) --it; }
template <RandomIt I>  void advance(I& it, std::ptrdiff_t n) { std::printf("    [random  ] "); it += n; }

template <class I> consteval const char* category() {
    if constexpr (RandomIt<I>)   return "random access";
    else if constexpr (BidirIt<I>)   return "bidirectional";
    else if constexpr (ForwardIt<I>) return "forward";
    else if constexpr (InputIt<I>)   return "input";
    else                              return "(not an iterator)";
}
} // namespace my

int main() {
    std::vector<int> v(10); std::list<int> l(10); std::forward_list<int> f(10);
    std::printf("vector       : mine=%-14s std=random_access? %d\n", my::category<std::vector<int>::iterator>(), std::random_access_iterator<std::vector<int>::iterator>);
    std::printf("list         : mine=%-14s std=bidirectional? %d\n", my::category<std::list<int>::iterator>(),   std::bidirectional_iterator<std::list<int>::iterator>);
    std::printf("forward_list : mine=%-14s std=forward? %d\n",       my::category<std::forward_list<int>::iterator>(), std::forward_iterator<std::forward_list<int>::iterator>);
    std::printf("int*         : mine=%-14s\n", my::category<int*>());
    std::printf("istream_it   : mine=%-14s std=input? %d\n", my::category<std::istream_iterator<int>>(), std::input_iterator<std::istream_iterator<int>>);
    std::printf("int          : mine=%-14s\n", my::category<int>());

    auto vi = v.begin(); auto li = l.begin(); auto fi = f.begin();
    std::puts("advance by 5:");
    my::advance(vi, 5); std::puts("vector");
    my::advance(li, 5); std::puts("list");
    my::advance(fi, 5); std::puts("forward_list");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
vector       : mine=random access  std=random_access? 1
list         : mine=bidirectional  std=bidirectional? 1
forward_list : mine=forward        std=forward? 1
int*         : mine=random access 
istream_it   : mine=forward        std=input? 1
int          : mine=(not an iterator)
advance by 5:
    [random  ] vector
    [bidir   ] list
    [input   ] forward_list
```

`istream_iterator` is an *input* iterator in the standard's category sense, but it is **copyable and default-constructible**, so a purely **syntactic** `ForwardIt` concept classifies it as forward. The multi-pass guarantee is the **semantic** requirement the compiler cannot see, which is the same wall Experiment 4 hit. The standard resolves it with an explicit *tag* (`iterator_category`/`iterator_concept`), i.e. the author *asserts* the semantics. Real library concepts are therefore **syntax + opt-in tag**, not syntax alone.

</details>

---

## 10. Real-world example

| Where | How concepts show up |
|---|---|
| **`<ranges>` / `<algorithm>`** (Chapters 14–15) | Every `std::ranges::` algorithm is constrained; the iterator/range concept hierarchy *is* the type system of the library |
| **`std::formatter`**, `std::formattable` (C++23) | "Can I format `T`?" is a concept, so `std::format("{}", x)` errors name the missing formatter |
| **`std::move_only_function`, `std::expected`** | Constructors constrained so the template constructor never hijacks copy/move (Chapter 7's problem) |
| **`std::allocator_traits`/PMR** | The allocator *requirements* (prose before C++20) are expressible as a concept |
| **Eigen, Boost.Asio, `{fmt}`** | Migrating from SFINAE to `requires` under a C++20 `#if`; Asio's `completion_token` concepts |
| **Your code** | Replace every `enable_if` and every comment saying "T must support …" |

### Where concepts earn their keep, and where they are overkill

| Use concepts for | Do not bother for |
|---|---|
| Public template APIs and library boundaries | A private helper used once |
| Overloading by capability (the iterator ladder) | A template with a single instantiation: `template <class T>` with a doc comment is enough |
| Error messages that are part of the product | Hot-path code where the constraint has *no benefit*: concepts won't make it faster |
| Constraining special members for conditional triviality | Encoding business rules ("`T` must be a positive int"): that is a runtime check |

> **Opinion.** The best test for a concept: *can you name it in one word that a domain expert would recognize?* `Allocator`, `Hashable`, `Serializable`: yes. `HasBeginAndEndAndSize`: no, you've written a **checklist**, not a concept. A concept should be a *capability with a meaning*, ideally with a semantic definition you can write down. Checklists belong in `requires` clauses on a specific function.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Repeating the same boolean expression instead of reusing a named concept | **Ambiguous overload**: subsumption doesn't see through text | Reuse the concept; or make the expression a concept |
| `requires !std::is_same_v<T, U>` (negation at top level of a requires-clause) | Syntax error (not a primary expression) | Parenthesize: `requires (!std::is_same_v<T, U>)` |
| Compound requirement return type `-> std::same_as<int>` against an lvalue expression | Constraint silently false (type is `int&`) | `std::same_as<int&>` or `std::convertible_to<int>` |
| Concept that a type satisfies *accidentally* (same syntax, different meaning) | `Drawable` satisfied by a `Cowboy::draw()`; wrong code runs | Add a *tag* or a semantic marker; keep concepts domain-specific |
| Assuming concepts check the *body* | Template body uses `t.foo()` that isn't in the concept: compiles for types that happen to have it, breaks for others | Test with a **minimal archetype** (a type that has *only* what the concept promises) |
| Over-constraining (`std::regular` when you only copy) | Rejects valid types (non-default-constructible) | Ask for the *least* you use: `copy_constructible` |
| Under-constraining | Same as before: late, ugly errors | Add the concept; write a test that expects failure |
| Constraint evaluation recursion (`requires C<T>` inside `C`) | Hard error: "satisfaction of constraint depends on itself" | Restructure; concepts cannot be recursive |
| Constrained function *declared* and *defined* with textually different constraints | "redeclaration with different constraints" / "does not match any declaration" | The constraints of a redeclaration must be *equivalent* (same tokens): copy-paste, or use a named concept |
| Changing a public constraint | **ABI/API break** (mangling includes constraints) and silent overload changes | Treat constraints as part of the interface, version them |
| Using `requires` where `static_assert` gives a clearer message | The user sees "no matching function" instead of your helpful message | For a *single* overload with one mistake, `static_assert(Concept<T>, "T must be …")` can be friendlier; for overload sets, use constraints |
| Treating `std::sortable`/`std::regular` as complete contracts | Semantic UB (Experiment 4) | Read the semantic wording; enable checked-STL builds in CI |

---

## 12. Exercises

1. **Rewrite.** Take five `enable_if` uses (from Chapter 9's exercises, or your own code) and convert to `requires`. Record the old and new error messages for a failing call.
2. **Subsumption by hand.** For each pair, state whether `A` subsumes `B`, `B` subsumes `A`, neither: (a) `A = integral<T>`, `B = integral<T> && signed_integral<T>`; (b) `A = is_integral_v<T>`, `B = integral<T>`; (c) `A = C<T> || D<T>`, `B = C<T>`; (d) `A = C<T> && D<T>`, `B = D<T> && C<T>`. Verify with overloads.
3. **Archetype testing.** Write `struct MinimalHashable { std::size_t hash() const noexcept; }` and use it to instantiate every function templated on `Hashable` in a code base. Which ones use *more* than the concept promised?
4. **A `Serializable` concept with semantics.** Design `Serializable<T>` (`write(Out&)`, `static read(In&)`), specify the *semantic* requirement (round-trip: `read(write(x)) == x`), and write a **property test** (random values) that checks the semantic requirement for every type that opts in. Why can this not be a `static_assert`?
5. **Constrained members.** Write `template <class T> class Wrapper` with `operator<<` only when `T` is streamable, `operator==` only when `T` is equality-comparable, and `swap` only when `T` is swappable. Test all combinations, including a type with *none*.
6. **Compile-time cost.** Write 20 nested concepts each referencing the previous one, instantiate with 100 types, and compare compile time (`-ftime-report`) with the same via `void_t`. Then flatten the concepts. What changed?
7. **Diagnostics survey.** For `std::ranges::sort`, `std::ranges::find`, `std::format`, `std::vector<std::unique_ptr<int>>(other)`, collect the GCC 14 and Clang 18 messages. Rank them. What would you change in the concepts' *names* to improve them?
8. **Conditional special members.** Extend `MiniOptional` (Experiment 6) with a move constructor, a copy assignment and a move assignment, each with a trivial/non-trivial pair. Check `is_trivially_copyable` for `MiniOptional<int>` and `MiniOptional<std::string>`; compare with `std::optional`.

---

## 13. Challenge: a concept-based `Allocator`-aware container API

Design the *interface* (concepts and signatures only, plus a minimal implementation that proves it works) of a `FlatMap<K, V, Alloc>`:

- `Key` requires `std::totally_ordered` *or* a user-supplied `Compare` satisfying `std::strict_weak_order`, and `std::movable`
- `Alloc` requires the allocator completeness requirements (`value_type`, `allocate`, `deallocate`, equality) expressed as a `StdAllocator<A, T>` concept, rebindable
- iterator type must satisfy `std::random_access_iterator`, and the container must model `std::ranges::sized_range` and `std::ranges::contiguous_range` for *keys* separately from values (two parallel arrays? an array of pairs? decide, and justify with Chapter 27)
- heterogenous lookup `find(const Q&)` enabled **only** if `Compare::is_transparent` exists
- each member function documents (in comments) its **syntactic** and **semantic** preconditions and its complexity

Then write five *negative tests* (compile failures that you expect) as `static_assert(!requires { … })` and check the diagnostics produced for each when the failure *is not* expected. Are they good enough to ship?

---

## 14. Knowledge check

1. What is the difference between a *concept*, a *requires-clause* and a *requires-expression*?
2. Why is an ill-formed expression in a constraint "false" and not an error? How does this relate to SFINAE?
3. Explain subsumption. Why does `requires std::is_integral_v<T>` repeated in two overloads give an ambiguity?
4. What are the four kinds of requirement in a requires-expression? Give an example of each.
5. What does the type in `{ expr } -> C<…>` get tested against, exactly (`decltype(...)` of what)?
6. Which parts of a concept's contract does the compiler **not** check? Give two examples where a type satisfies a standard concept but violates its semantics.
7. What is the difference between `template <std::integral T> void f(T)` and `void f(std::integral auto)`?
8. What does a constrained destructor allow, and why does it matter for ABI?
9. When should you use `static_assert` rather than a constraint?
10. Why does changing a constraint on an exported function template affect ABI?
11. Name three advantages of concepts over `enable_if` that are *not* about error messages.
12. Why can't concepts be recursive or specialized, and what does that buy the compiler?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. A *concept* is a named template whose value is a bool constraint-expression. A *requires-clause* (`requires C<T>`) attaches a constraint to a template or function. A *requires-expression* (`requires(T t) { … }`) is a boolean **expression** testing the validity of other expressions; it can appear in a concept, a clause or an `if constexpr`.
2. Constraint satisfaction substitutes into each atom; an invalid substitution makes that atom *unsatisfied*, so the candidate is dropped. SFINAE was the same mechanism, applied implicitly to signatures. Concepts **name and order** it.
3. `P` subsumes `Q` if `P` implies `Q` after normalization into atomic constraints (identified by *source expression*). The two copies of `std::is_integral_v<T>` are different atoms (different locations) so neither implies the other, hence ambiguous. Using the same named concept in both places gives the *same* atom.
4. Simple (`++t;`), type (`typename T::value_type;`), compound (`{ t.hash() } noexcept -> std::convertible_to<size_t>;`), nested (`requires sizeof(T) <= 16;`).
5. Against `decltype((expr))`: the expression with its value category, so an lvalue-returning call has type `T&`. `std::same_as<T>` fails for lvalues; use `same_as<T&>` or `convertible_to<T>`.
6. Semantics and complexity. `double` satisfies `equality_comparable` and `totally_ordered` but `NaN != NaN`; a comparator using `<=` satisfies `strict_weak_order` syntactically but is not irreflexive; an `istream_iterator` can look like a forward iterator syntactically.
7. They are equivalent for a single parameter. The abbreviated form introduces a distinct *invented* template parameter per `auto` (so you can't name the type), and two parameters `(integral auto a, integral auto b)` may have different types. The explicit form lets you reuse `T` (`T a, T b`) and mention it in the body.
8. It lets a class have **several destructors/special members with different constraints**, and the most constrained *eligible* one is selected. That makes a class **trivially destructible/copyable exactly when its members are**, which determines how it is passed (registers vs memory) in the Itanium ABI and enables `memcpy` optimizations.
9. When you have a single template (no overload set to rescue) and want a custom, domain-friendly message: `static_assert(Concept<T>, "T must be …")`. Also for invariants about the template arguments *beyond* applicability.
10. The constraint is mangled into the function template's name (`Tk…`/`requires` mangling). Changing it changes the symbol of every instantiation, so already compiled callers no longer link to the library's exported instantiations.
11. Overloading by capability with automatic ordering (subsumption); constrained member functions and special members (conditional APIs and triviality); usable in `if constexpr`/`static_assert`/lambdas/auto-placeholders; compile-time cost and readability (the requirement lives in the interface, not the machinery).
12. Concepts that can be recursive or specialized could make satisfaction undecidable or dependent on declaration order and could not be *normalized* into the formula used for subsumption. Forbidding both keeps satisfaction a pure, cacheable function of the arguments.

</details>

---

[← Previous: Chapter 9](09-type-traits.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 11 — Containers →](../part-05-standard-library/11-containers.md)
