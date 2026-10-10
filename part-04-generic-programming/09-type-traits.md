# Chapter 9 — Type Traits and Compile-Time Introspection

> **Part IV · Generic programming** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 5 hours**
> **Prerequisites:** [Chapter 8](08-templates-deep-dive.md) &nbsp;|&nbsp; **Standards:** C++11 → C++23 &nbsp;|&nbsp; **Tools:** `g++`, `clang++`, `__PRETTY_FUNCTION__`

[← Previous: Chapter 8](08-templates-deep-dive.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 10 — Concepts →](10-concepts.md)

---

**In one sentence:** a *type trait* is a class template that **answers a question about a type** (or computes a new type from it) at compile time, which makes it the way generic code looks at the types it was given and chooses an implementation.

**By the end of this chapter you can:**

- classify any trait in `<type_traits>` as a *query* (`is_*`, `has_*`), a *transformation* (`remove_*`, `add_*`, `decay`, `conditional`) or a *relation* (`is_same`, `is_base_of`, `is_convertible`)
- implement the core traits yourself with partial specialization
- use `void_t` and the detection idiom to ask "does `T` have a member `x`?"
- use SFINAE (`enable_if`), tag dispatch and `if constexpr` to select an implementation, and say which to prefer
- explain the *soft error vs hard error* distinction
- say, for each classic use of traits, why C++20 concepts do or do not replace it

---

## 1. Problem

Generic code is written once and used with unknown types, but **correct and fast code often depends on properties of the type**:

```cpp
template <class T> void relocate(T* dst, T* src, std::size_t n);
// For int, Point{double,double}: memmove is correct and fast.
// For std::string, std::unique_ptr: memmove would be wrong (or right only by luck): must move-construct and destroy.
// For a type with a throwing move: needs a different error strategy again.
```

The language gives you templates, but a template body **cannot ask a question about `T`** without extra machinery. You need compile-time predicates and type-level functions.

---

## 2. Historical context

| Era | Approach | Limits |
|---|---|---|
| C++98 | Hand-written traits: `iterator_traits<It>::value_type`, `char_traits<C>`, `numeric_limits<T>` | One trait class per library; every library reinvents |
| 2000s | **Boost.TypeTraits** (`is_pod`, `has_trivial_copy`, `is_base_of`) with compiler intrinsics for the questions that can't be answered in the language | Needed compiler help for most of them |
| 1998 → | **SFINAE** (Vandevoorde, Josuttis) turns a substitution failure into "remove this overload", the only conditional compilation available | Cryptic, brittle, error messages terrible |
| C++11 | **`<type_traits>`** standardized; `enable_if`, `decay`, `common_type`, `declval`, `decltype`; `static_assert` | Verbose `typename X<T>::type` |
| C++14 | `_t` aliases (`enable_if_t`, `remove_cv_t`) | |
| C++17 | `_v` variable templates (`is_same_v`), `void_t`, `conjunction`/`disjunction`/`negation`, `is_invocable`, **`if constexpr`** | Removed much SFINAE |
| C++20 | **Concepts**; `remove_cvref`, `type_identity`, `is_bounded_array`; deprecated `is_pod`, `result_of` removed later | SFINAE's reason for being is mostly gone |
| C++23 | `is_scoped_enum`, `is_implicit_lifetime`, `reference_constructs_from_temporary` (dangling checks, Chapter 12) | |
| C++26 | `is_trivially_relocatable` etc. (P2786, adopted); reflection adds `std::meta` as the future of the whole subject | Chapter 18 |

---

## 3. Modern solution

```cpp
#include <type_traits>

static_assert(std::is_integral_v<int>);                                   // query
using T2 = std::remove_cvref_t<const std::string&>;                       // transformation → std::string
using R  = std::conditional_t<(sizeof(long) > 4), long, int>;             // type-level if
using C  = std::common_type_t<int, double, float>;                        // type-level function over a pack → double
static_assert(std::is_nothrow_move_constructible_v<std::vector<int>>);    // property
```

The three vocabulary conventions:

| Suffix | Meaning | Example |
|---|---|---|
| (none) | the trait class, with `::value` or `::type` | `std::is_same<A,B>::value` |
| `_v` | the *value*, as a `constexpr` variable template (C++17) | `std::is_same_v<A,B>` |
| `_t` | the *resulting type*, as an alias (C++14) | `std::remove_const_t<T>` |

---

## 4. Mental model

### Traits are functions that run in the compiler

```text
   a RUNTIME function:     bool   is_even(int n)               values  → values
   a TRAIT (query):        template<class T> is_pointer<T>     types   → bool constant
   a TRAIT (transform):    template<class T> remove_const<T>   types   → type
   a TRAIT (relation):     template<class A, class B> is_same  types   → bool constant
```

- **Pattern matching is partial specialization.** There are no `if` statements at the type level: you write the *general case*, then *specialize* for the shapes you recognize. The compiler picks the most specialized match:

```cpp
template <class T> struct is_pointer       : std::false_type {};          // general case
template <class T> struct is_pointer<T*>   : std::true_type  {};          // "if the type looks like T*"
```

- **Recursion is iteration.** To process a pack or a list, the trait recurses on the tail (`Ts...` → `Head, Tail...`). Fold expressions and `if constexpr` replace much of this since C++17.
- **`std::integral_constant<T, v>`** is the base of all boolean traits: a *type* that carries a *value*, convertible to `bool`. `true_type` is `integral_constant<bool, true>`.

### The decision tree for "how do I make this generic code depend on `T`?"

```text
 Need to choose an IMPLEMENTATION inside one function?
     └─ yes → if constexpr (trait<T>)                              [C++17]
 Need to choose between OVERLOADS / reject a call?
     └─ yes → concepts and `requires`                              [C++20]  ← prefer
             (before C++20: enable_if / void_t / tag dispatch)
 Need to COMPUTE a type from another type?
     └─ yes → a transformation trait / alias template
 Need to give a CLASS different layout/members per type?
     └─ yes → partial specialization (still the right tool)
```

---

## 5. Language rules

### 5.1 The three kinds of trait  `[meta]`

| Kind | Form | Examples |
|---|---|---|
| **UnaryTypeTrait**: property of one type | `::value` (bool) | `is_integral`, `is_const`, `is_trivially_copyable`, `is_empty`, `is_final`, `is_abstract` |
| **BinaryTypeTrait**: relation between types | `::value` (bool) | `is_same`, `is_base_of`, `is_convertible`, `is_assignable`, `is_constructible` (n-ary) |
| **TransformationTrait**: produces a type | `::type` | `remove_cv`, `add_pointer`, `decay`, `conditional`, `common_type`, `underlying_type`, `invoke_result` |

### 5.2 The ones you will use constantly

| Trait | Result | Notes |
|---|---|---|
| `is_same_v<A, B>` | Identical types? | `int` ≠ `const int` ≠ `int&`: **cv and ref matter** |
| `remove_cvref_t<T>` (C++20) | Strip `const`, `volatile` and `&`/`&&` | The "what is the underlying value type" trait. Use this, not `decay`, unless you want arrays/functions to decay too |
| `decay_t<T>` | What `T` becomes when **passed by value**: strip ref, strip cv, array→pointer, function→pointer | What `std::thread`/`std::make_pair` store |
| `conditional_t<B, T, F>` | `T` if `B`, else `F` | Both branches must be **valid types** (evaluated eagerly) |
| `common_type_t<Ts...>` | Type all `Ts` convert to under `?:` | Basis of `std::min`-style mixed-type APIs; `chrono::duration` |
| `invoke_result_t<F, Args...>` | Return type of `std::invoke(F, Args...)` | Replaces `result_of` |
| `underlying_type_t<E>` | Integer type of an enum | `to_underlying` (C++23) wraps it |
| `is_constructible_v<T, Args...>` | Can `T(declval<Args>()...)` be formed? | **Direct-initialization**; includes explicit ctors |
| `is_convertible_v<From, To>` | Can `From` be *implicitly* converted? | Not the same as constructible (`explicit` ⇒ false) |
| `is_trivially_copyable_v<T>` | Is `memcpy` of its bytes a valid copy? | The gate for `memcpy`-based optimizations |
| `is_nothrow_move_constructible_v<T>` | Move can't throw? | Consulted by `vector` ([Chapter 6](../part-03-value-categories-and-move/06-move-semantics.md)) |
| `has_unique_object_representations_v<T>` | No padding, every bit significant | Safe to hash/`memcmp` as bytes |
| `is_standard_layout_v<T>`, `is_aggregate_v<T>` | See [Chapter 2](../part-02-object-model-and-lifetime/02-object-model.md) | |
| `type_identity_t<T>` (C++20) | `T`, but **non-deduced** | Blocks deduction from one argument (`clamp(x, lo, hi)`) |

### 5.3 `declval` and unevaluated operands

`std::declval<T>()` produces an expression of type `T&&` **without constructing anything**; it can only appear in unevaluated contexts (`decltype`, `sizeof`, `noexcept`, `requires`). It is how traits ask "what would this expression do?" for types that have no default constructor:

```cpp
using R = decltype(std::declval<A&>() + std::declval<B&>());   // type of a + b, no objects needed
```

### 5.4 SFINAE  `[temp.deduct]/8`

> **S**ubstitution **F**ailure **I**s **N**ot **A**n **E**rror: if substituting deduced or explicit arguments into a function template's **declaration** (its *immediate context*: the signature and the return type) produces an invalid type or expression, that template is silently *removed from the overload set* rather than causing a diagnostic.

```cpp
template <class T>
std::enable_if_t<std::is_integral_v<T>, T> half(T x) { return x / 2; }   // removed when T is not integral
template <class T>
std::enable_if_t<std::is_floating_point_v<T>, T> half(T x) { return x * 0.5; }
```

**Soft vs hard errors.** Only failures in the *immediate context* of the declaration are soft. Errors **inside the body**, or inside the *instantiation of another template's definition*, are **hard errors** (the program is ill-formed): `half("a")` produces "no matching function", but a `static_assert` fired by a trait while instantiating a class is a *hard* failure that no other overload can rescue. Experiment 4.

### 5.5 `void_t` and the detection idiom

```cpp
template <class...> using void_t = void;                         // std::void_t, C++17

template <class T, class = void>                    struct has_size : std::false_type {};
template <class T> struct has_size<T, void_t<decltype(std::declval<T>().size())>> : std::true_type {};
```

Mechanism: the partial specialization is viable **only if** `decltype(declval<T>().size())` is valid: if not, substitution fails *softly*, the specialization is discarded and the primary (`false_type`) is used. The default argument `= void` is what makes the two lines match: the specialization's second argument is always `void`.

The general form is the **detection idiom** (`std::experimental::is_detected`):

```cpp
template <class Default, class AlwaysVoid, template <class...> class Op, class... Args>
struct detector { using value_t = std::false_type; using type = Default; };
template <class Default, template <class...> class Op, class... Args>
struct detector<Default, std::void_t<Op<Args...>>, Op, Args...> { using value_t = std::true_type; using type = Op<Args...>; };
```

### 5.6 Why `std::is_*` traits may need the compiler

Several traits cannot be written in ISO C++ and are implemented with **compiler intrinsics** (`__is_trivially_copyable`, `__is_base_of`, `__is_union`, `__is_final`, `__has_unique_object_representations` …). The *standard* guarantees the result; *how* it's computed is a **compiler** matter. This is also why **adding a specialization of a standard trait for your own type is undefined behaviour** (except `std::common_type`, `std::hash`-style customization points explicitly allowed) (`[meta.rqmts]`).

### 5.7 Traits and the standard's "completeness" rule

Most traits require `T` to be a **complete type** (or `void` / array of unknown bound). Using them on an incomplete type is **undefined behavior** (ill-formed NDR): `std::is_empty_v<Fwd>` with only a forward declaration may give a wrong answer *and then be cached* if the type is later completed (one-definition-rule violation by the instantiation cache).

### Layer check

| Layer | Decides |
|---|---|
| **Standard** | Trait names, results, completeness requirements, the ban on specializing standard traits |
| **Compiler** | Intrinsics that implement them; evaluation cost; SFINAE implementation details; **whether `is_trivially_copyable` of a class with a deleted copy ctor is true (it can be)** |
| **ABI** | `is_trivially_copyable` / trivial destructor determine **how a class is passed in registers** (Itanium: a type with a non-trivial copy ctor or destructor goes by hidden reference; Chapter 37) |

---

## 6. Implementation model

Every trait is a **class template instantiation**: instantiating `std::is_pointer<int*>` creates a class, looks up `::value`, and caches it. That costs compile time, and it is why traits stacked in deep template chains slow builds. C++17's `_v` variable templates and recent compilers' intrinsics (GCC 13+, Clang) turn many `is_*` into one built-in call, much cheaper than the class-template route.

`std::conditional_t<B, T, F>` is the cheapest type-level `if`: it is implemented as a partial specialization on `B`. But **both `T` and `F` are named**, hence must be valid, so `conditional_t<is_pointer_v<T>, remove_pointer_t<T>, T>` is fine, while `conditional_t<is_pointer_v<T>, decltype(*t), void>` is a hard error for non-pointers. Use `if constexpr` (a value) or lazily wrap each branch in a trait class and apply `::type` after choosing.

---

## 7. Experiments

### Experiment 1: Implement the core traits and cross-check against `std`

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <type_traits>

namespace my {

// ---- the building block ----
template <class T, T v> struct integral_constant { static constexpr T value = v; using type = integral_constant; constexpr operator T() const noexcept { return value; } };
using true_type  = integral_constant<bool, true>;
using false_type = integral_constant<bool, false>;

// ---- relation: pattern matching with two parameters ----
template <class, class> struct is_same       : false_type {};
template <class T>      struct is_same<T, T> : true_type  {};
template <class A, class B> inline constexpr bool is_same_v = is_same<A, B>::value;

// ---- queries by shape ----
template <class T> struct is_pointer                    : false_type {};
template <class T> struct is_pointer<T*>                : true_type  {};
template <class T> struct is_pointer<T* const>          : true_type  {};
template <class T> struct is_pointer<T* volatile>       : true_type  {};
template <class T> struct is_pointer<T* const volatile> : true_type  {};

template <class T> struct is_lvalue_reference     : false_type {};
template <class T> struct is_lvalue_reference<T&> : true_type  {};

// ---- transformations ----
template <class T> struct remove_reference      { using type = T; };
template <class T> struct remove_reference<T&>  { using type = T; };
template <class T> struct remove_reference<T&&> { using type = T; };
template <class T> using remove_reference_t = typename remove_reference<T>::type;

template <class T> struct remove_const          { using type = T; };
template <class T> struct remove_const<const T> { using type = T; };
template <class T> using remove_const_t = typename remove_const<T>::type;

template <bool B, class T, class F> struct conditional            { using type = T; };
template <class T, class F>         struct conditional<false, T, F> { using type = F; };
template <bool B, class T, class F> using conditional_t = typename conditional<B, T, F>::type;

// enable_if: has ::type only when B is true: that absence is what SFINAE exploits
template <bool B, class T = void> struct enable_if {};
template <class T>                struct enable_if<true, T> { using type = T; };
template <bool B, class T = void> using enable_if_t = typename enable_if<B, T>::type;

// ---- array and function handling: decay ----
template <class T> struct remove_extent        { using type = T; };
template <class T> struct remove_extent<T[]>   { using type = T; };
template <class T, unsigned long N> struct remove_extent<T[N]> { using type = T; };

template <class T> struct is_array                   : false_type {};
template <class T> struct is_array<T[]>              : true_type  {};
template <class T, unsigned long N> struct is_array<T[N]> : true_type {};
template <class T> struct is_function                : false_type {};
template <class R, class... A> struct is_function<R(A...)> : true_type {};

template <class T> struct add_pointer { using type = remove_reference_t<T>*; };

template <class T> struct decay {
private:
    using U = remove_reference_t<T>;
public:
    using type = conditional_t<is_array<U>::value,
                     typename remove_extent<U>::type*,                 // arrays keep element cv: const char[5] → const char*
                     conditional_t<is_function<U>::value,
                         typename add_pointer<U>::type,
                         remove_const_t<U>>>;                          // (a real one also strips volatile)
};
template <class T> using decay_t = typename decay<T>::type;

} // namespace my

void fn(int);

// every my:: trait is checked against the std:: one on a battery of types
#define CHECK_SAME_PAIR(A, B)   static_assert(my::is_same_v<A, B> == std::is_same_v<A, B>)
CHECK_SAME_PAIR(int, int); CHECK_SAME_PAIR(int, const int); CHECK_SAME_PAIR(int, int&); CHECK_SAME_PAIR(int*, int*);

static_assert(my::is_pointer<int*>::value && my::is_pointer<int* const>::value && !my::is_pointer<int>::value);
static_assert(my::is_same_v<my::remove_reference_t<int&&>, int>);
static_assert(my::is_same_v<my::conditional_t<true, int, double>, int>);
static_assert(my::is_same_v<my::conditional_t<false, int, double>, double>);
static_assert(my::is_same_v<my::decay_t<const int&>, std::decay_t<const int&>>);
static_assert(my::is_same_v<my::decay_t<int[3]>, int*>);
static_assert(my::is_same_v<my::decay_t<decltype(fn)>, void (*)(int)>);
static_assert(my::is_same_v<my::decay_t<const char(&)[5]>, std::decay_t<const char(&)[5]>>);

int main() { std::puts("all my:: traits agree with std::"); }
```

```text
# output (gcc 14.2.0, x86-64 Linux)
all my:: traits agree with std::
```

Six lines in the middle repeat one idea: **a primary template for "no", a partial specialization for the shape that means "yes"**. `is_same<T, T>` is the cleverest: the pattern uses the *same* parameter twice, so it only matches when both arguments are identical. `decay` is *composition*: peel layers with other traits.

### Experiment 2: A cheat-sheet you can print: what each trait says about real types

`__PRETTY_FUNCTION__` (GCC and Clang, **a compiler extension**, not standard) shows a deduced type as text, which is the quickest way to see what a transformation really produced.

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

template <class T>
const char* type_name() {                                       // GCC/Clang extension
    static char buf[160];
    const char* p = __PRETTY_FUNCTION__;
    const char* b = std::strstr(p, "T = ") + 4;
    const char* e = std::strchr(b, ';');
    if (!e) e = std::strrchr(b, ']');
    std::snprintf(buf, sizeof buf, "%.*s", int(e - b), b);
    return buf;
}
#define SHOW(...) std::printf("  %-44s -> %s\n", #__VA_ARGS__, type_name<__VA_ARGS__>())

struct Base {}; struct Derived : Base {};
int fn(double);

int main() {
    std::puts("decay_t (what you get when passing by value)");
    SHOW(std::decay_t<const int&>);
    SHOW(std::decay_t<int[4]>);
    SHOW(std::decay_t<const char(&)[6]>);
    SHOW(std::decay_t<decltype(fn)>);
    std::puts("remove_cvref_t (just strip qualifiers)");
    SHOW(std::remove_cvref_t<const volatile int&&>);
    SHOW(std::remove_cvref_t<int[4]>);
    std::puts("common_type_t");
    SHOW(std::common_type_t<int, long>);
    SHOW(std::common_type_t<int, double, float>);
    SHOW(std::common_type_t<char, short>);
    SHOW(std::common_type_t<Derived*, Base*>);
    SHOW(std::common_type_t<std::string, const char*>);
    std::puts("invoke_result_t / underlying_type_t");
    SHOW(std::invoke_result_t<decltype(&fn), int>);
    SHOW(std::invoke_result_t<decltype([](int x) { return x * 1.5f; }), int>);
    enum class E : std::uint8_t { a };
    SHOW(std::underlying_type_t<E>);
    std::puts("conditional_t / make_unsigned_t / add_pointer_t");
    SHOW(std::conditional_t<(sizeof(void*) == 8), std::int64_t, std::int32_t>);
    SHOW(std::make_unsigned_t<int>);
    SHOW(std::add_pointer_t<int&>);
    SHOW(std::add_lvalue_reference_t<void>);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
decay_t (what you get when passing by value)
  std::decay_t<const int&>                     -> int
  std::decay_t<int[4]>                         -> int*
  std::decay_t<const char(&)[6]>               -> const char*
  std::decay_t<decltype(fn)>                   -> int (*)(double)
remove_cvref_t (just strip qualifiers)
  std::remove_cvref_t<const volatile int&&>    -> int
  std::remove_cvref_t<int[4]>                  -> int [4]
common_type_t
  std::common_type_t<int, long>                -> long int
  std::common_type_t<int, double, float>       -> double
  std::common_type_t<char, short>              -> int
  std::common_type_t<Derived*, Base*>          -> Base*
  std::common_type_t<std::string, const char*> -> std::__cxx11::basic_string<char>
invoke_result_t / underlying_type_t
  std::invoke_result_t<decltype(&fn), int>     -> int
  std::invoke_result_t<decltype([](int x) { return x * 1.5f; }), int> -> float
  std::underlying_type_t<E>                    -> unsigned char
conditional_t / make_unsigned_t / add_pointer_t
  std::conditional_t<(sizeof(void*) == 8), std::int64_t, std::int32_t> -> long int
  std::make_unsigned_t<int>                    -> unsigned int
  std::add_pointer_t<int&>                     -> int*
  std::add_lvalue_reference_t<void>            -> void
```

Things worth noticing: `common_type<char, short>` is `int` (integer promotion); `common_type<Derived*, Base*>` is `Base*`; `common_type<std::string, const char*>` is `std::string`; and `add_lvalue_reference_t<void>` is `void` (it is not an error: "is it possible to add a reference?" is part of the trait's contract; this soft behavior is what makes traits composable in SFINAE).

### Experiment 3: Queries that surprise

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

struct Plain { int a; double b; };
struct WithDtor { ~WithDtor() {} };
struct Explicit { explicit Explicit(int) {} };
struct Pad { char c; int i; };                        // has padding
struct NoPad { int a; int b; };
struct NoCopy { NoCopy() = default; NoCopy(const NoCopy&) = delete; };

#define Q(expr) std::printf("  %-52s %d\n", #expr, int(expr))

int main() {
    std::puts("category quirks");
    Q(std::is_integral_v<bool>);                       // bool, char types, wchar_t: all integral
    Q(std::is_integral_v<char8_t>);
    Q(std::is_arithmetic_v<bool>);
    Q(std::is_signed_v<float>);                        // "signed" includes floating point
    Q(std::is_signed_v<char>);                         // implementation-defined: true on x86-64 Linux
    Q(std::is_class_v<int[3]>);
    Q(std::is_function_v<int(*)()>);                   // a pointer to a function is NOT a function
    Q(std::is_const_v<const int&>);                    // a reference is never const itself
    Q(std::is_const_v<const int*>);                    // pointer to const: the pointer itself isn't
    Q(std::is_const_v<int* const>);

    std::puts("constructible vs convertible");
    Q((std::is_constructible_v<Explicit, int>));       // direct-init: yes
    Q((std::is_convertible_v<int, Explicit>));         // implicit: no (explicit ctor)
    Q((std::is_constructible_v<std::string, const char*>));
    Q((std::is_convertible_v<const char*, std::string>));
    Q((std::is_convertible_v<std::unique_ptr<int>, std::shared_ptr<int>>));   // rvalue unique→shared: yes

    std::puts("triviality and layout");
    Q(std::is_trivially_copyable_v<Plain>);
    Q(std::is_trivially_copyable_v<WithDtor>);         // user-provided dtor → not trivially destructible
    Q(std::is_trivially_copyable_v<NoCopy>);           // deleted copy ctor, trivial otherwise: still true!
    Q(std::is_trivially_copyable_v<std::string>);
    Q(std::is_trivially_copyable_v<std::vector<int>>);
    Q(std::has_unique_object_representations_v<NoPad>);
    Q(std::has_unique_object_representations_v<Pad>);  // padding bytes → false → don't hash/memcmp as bytes
    Q(std::has_unique_object_representations_v<float>);// two representations of zero (+0/−0)
    Q(std::is_standard_layout_v<Plain>);
    Q(std::is_empty_v<WithDtor>);

    std::puts("noexcept-ness");
    Q(std::is_nothrow_move_constructible_v<std::string>);
    Q(std::is_nothrow_move_constructible_v<std::vector<int>>);
    Q(std::is_nothrow_move_assignable_v<std::vector<int>>);
    Q((std::is_nothrow_default_constructible_v<std::string>));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
category quirks
  std::is_integral_v<bool>                             1
  std::is_integral_v<char8_t>                          1
  std::is_arithmetic_v<bool>                           1
  std::is_signed_v<float>                              1
  std::is_signed_v<char>                               1
  std::is_class_v<int[3]>                              0
  std::is_function_v<int(*)()>                         0
  std::is_const_v<const int&>                          0
  std::is_const_v<const int*>                          0
  std::is_const_v<int* const>                          1
constructible vs convertible
  (std::is_constructible_v<Explicit, int>)             1
  (std::is_convertible_v<int, Explicit>)               0
  (std::is_constructible_v<std::string, const char*>)  1
  (std::is_convertible_v<const char*, std::string>)    1
  (std::is_convertible_v<std::unique_ptr<int>, std::shared_ptr<int>>) 1
triviality and layout
  std::is_trivially_copyable_v<Plain>                  1
  std::is_trivially_copyable_v<WithDtor>               0
  std::is_trivially_copyable_v<NoCopy>                 1
  std::is_trivially_copyable_v<std::string>            0
  std::is_trivially_copyable_v<std::vector<int>>       0
  std::has_unique_object_representations_v<NoPad>      1
  std::has_unique_object_representations_v<Pad>        0
  std::has_unique_object_representations_v<float>      0
  std::is_standard_layout_v<Plain>                     1
  std::is_empty_v<WithDtor>                            1
noexcept-ness
  std::is_nothrow_move_constructible_v<std::string>    1
  std::is_nothrow_move_constructible_v<std::vector<int>> 1
  std::is_nothrow_move_assignable_v<std::vector<int>>  1
  (std::is_nothrow_default_constructible_v<std::string>) 1
```

Read these as a set of **gotchas the compiler will never warn you about**:

- `is_trivially_copyable<NoCopy>` is **true** although copying is deleted: *trivially copyable* means "if you copy it, `memcpy` is a valid way to do it", and says nothing about whether copying is allowed. Gate `memcpy` code on `is_trivially_copyable && is_copy_constructible` if that matters.
- `is_convertible` and `is_constructible` disagree exactly where `explicit` is involved: **use the one that matches how your code will initialize**.
- `has_unique_object_representations<float>` is false (`+0.0` and `-0.0` compare equal but differ in bits), so **do not hash or `memcmp`-compare floats as bytes**.

### Experiment 4: SFINAE, `void_t` detection, and the same thing in concepts

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <list>
#include <string>
#include <type_traits>
#include <vector>

// ---------- 1. C++11 SFINAE with enable_if on the return type ----------
template <class T>
std::enable_if_t<std::is_integral_v<T>, const char*> describe(T)       { return "integral"; }
template <class T>
std::enable_if_t<std::is_floating_point_v<T>, const char*> describe(T) { return "floating point"; }

// ---------- 2. The detection idiom with void_t ----------
template <class T, class = void>            struct has_size : std::false_type {};
template <class T>                          struct has_size<T, std::void_t<decltype(std::declval<const T&>().size())>> : std::true_type {};

template <class T, class = void>            struct has_push_back : std::false_type {};
template <class T>                          struct has_push_back<T, std::void_t<decltype(std::declval<T&>().push_back(std::declval<typename T::value_type>()))>> : std::true_type {};

// ---------- 3. The same predicates as C++20 concepts ----------
template <class T> concept HasSize     = requires(const T& t) { t.size(); };
template <class T> concept HasPushBack = requires(T& t, typename T::value_type v) { t.push_back(v); };

// ---------- 4. The selection, three ways ----------
template <class C> auto count1(const C& c) -> std::enable_if_t<has_size<C>::value, std::size_t> { return c.size(); }     // SFINAE
template <class C> auto count1(const C& c) -> std::enable_if_t<!has_size<C>::value, std::size_t> {                     // (fallback)
    std::size_t n = 0; for (auto it = c.begin(); it != c.end(); ++it) ++n; return n; }

template <class C> std::size_t count2(const C& c) {                                                                    // if constexpr
    if constexpr (has_size<C>::value) return c.size();
    else { std::size_t n = 0; for (auto it = c.begin(); it != c.end(); ++it) ++n; return n; }
}

template <HasSize C>     std::size_t count3(const C& c) { return c.size(); }                                           // concepts
template <class C> requires (!HasSize<C>) std::size_t count3(const C& c) { std::size_t n = 0; for (auto it = c.begin(); it != c.end(); ++it) ++n; return n; }

struct Counted { int items[3]{}; const int* begin() const { return items; } const int* end() const { return items + 3; } };   // no size()

int main() {
    std::printf("describe(1)=%s  describe(1.5)=%s\n", describe(1), describe(1.5));
    // describe("x");   // error: no matching function (SFINAE removed both candidates)

    std::printf("has_size:      vector=%d list=%d Counted=%d int=%d\n",
                has_size<std::vector<int>>::value, has_size<std::list<int>>::value, has_size<Counted>::value, has_size<int>::value);
    std::printf("has_push_back: vector=%d string=%d Counted=%d\n",
                has_push_back<std::vector<int>>::value, has_push_back<std::string>::value, has_push_back<Counted>::value);
    static_assert(HasSize<std::vector<int>> == has_size<std::vector<int>>::value);
    static_assert(HasPushBack<std::string>);

    std::vector<int> v(5); Counted c;
    std::printf("count1: %zu %zu | count2: %zu %zu | count3: %zu %zu\n",
                count1(v), count1(c), count2(v), count2(c), count3(v), count3(c));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
describe(1)=integral  describe(1.5)=floating point
has_size:      vector=1 list=1 Counted=0 int=0
has_push_back: vector=1 string=1 Counted=0
count1: 5 3 | count2: 5 3 | count3: 5 3
```

The three `count` functions are semantically the same. Compare how they read:

| Style | Era | Reads as | Error when no overload matches |
|---|---|---|---|
| `enable_if` in the return type | C++11 | machinery before meaning | "no matching function", plus a note naming each rejected substitution |
| `if constexpr` + trait | C++17 | one function with a compile-time branch | *only if* you add a `static_assert`/`else` |
| **`requires` clause / concept** | C++20 | the requirement is stated, and **named** | "constraints not satisfied: `HasSize<C>` evaluated to false" |

> **Verdict.** In new code, a trait is for **computing a type or a constant**; a *concept* is for **selecting or rejecting**. If you are writing `enable_if` in a post-C++20 codebase, stop and write a `requires`. If you are writing `void_t` partial specializations to *detect* something, write a `requires`-expression concept instead; keep traits for results that are *types* (`iterator_traits<It>::value_type`).

Now the **hard-error** boundary: a failure *inside* a class instantiation is not SFINAE-friendly.

```cpp
// @test fail -std=c++23 err=static.assertion
#include <string>
#include <type_traits>

template <class T> struct Checked { static_assert(std::is_integral_v<T>, "need an integral type"); using type = T; };

template <class T> typename Checked<T>::type pick(T x) { return x; }      // overload 1
void pick(std::string) {}                                                 // overload 2: the only sensible one for "x"

int main() {
    pick("x");     // overload 1 is deduced with T = const char*; naming Checked<const char*>::type
                   // instantiates the class, and its static_assert fires. That is a HARD error:
                   // had the failure been *soft*, overload 1 would be dropped and overload 2 chosen.
}
```

> [!NOTE]
> Compilers differ on *when* they instantiate here. With `void pick(const char*)` as overload 2 (a non-template exact match), Clang 18 still reports the error while GCC 14 compiles the program without complaint, because it never needed the template's return type to rank the candidates. The standard does not promise either: code whose validity depends on whether a class template is instantiated is fragile. Do not rely on a hard error being *avoided*.

The rule: **the check must be in the *declaration* (the immediate context), not in a nested class's body**, for the overload to be dropped quietly. This is why `std::enable_if` is a class that *has no member* (soft), rather than a class with a `static_assert` (hard).

### Experiment 5: Traits choosing an implementation, and the assembly that results

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=relocate_trivial,relocate_string
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <type_traits>

// Relocate n objects from src to dst (non-overlapping), leaving src destroyed.
template <class T>
void relocate(T* dst, T* src, std::size_t n) {
    if constexpr (std::is_trivially_copyable_v<T>) {
        std::memcpy(static_cast<void*>(dst), static_cast<const void*>(src), n * sizeof(T));   // bytes ARE the value
    } else {
        for (std::size_t i = 0; i < n; ++i) {                 // real move + destroy
            ::new (static_cast<void*>(dst + i)) T(std::move(src[i]));
            src[i].~T();
        }
    }
}

struct Point { double x, y; };
void relocate_trivial(Point* d, Point* s, std::size_t n)           { relocate(d, s, n); }
void relocate_string (std::string* d, std::string* s, std::size_t n) { relocate(d, s, n); }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
relocate_trivial(Point*, Point*, unsigned long):
	sal	rdx, 4
	jmp	memcpy@PLT

relocate_string(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >*, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >*, unsigned long):
	test	rdx, rdx
	je	.L3
	sal	rdx, 5
	lea	rax, 16[rsi]
	lea	rsi, [rdi+rdx]
	jmp	.L13
.L5:
	mov	QWORD PTR [rdi], rdx
	mov	rdx, QWORD PTR [rax]
	mov	QWORD PTR 16[rdi], rdx
.L22:
	mov	rdx, QWORD PTR -8[rax]
.L12:
	mov	QWORD PTR 8[rdi], rdx
	add	rdi, 32
	add	rax, 32
	cmp	rdi, rsi
	je	.L3
.L13:
	lea	rcx, 16[rdi]
	mov	QWORD PTR [rdi], rcx
	mov	rdx, QWORD PTR -16[rax]
	cmp	rdx, rax
	jne	.L5
	mov	rdx, QWORD PTR -8[rax]
	lea	r8, 1[rdx]
	cmp	r8d, 8
	jnb	.L6
	test	r8b, 4
	jne	.L23
	test	r8d, r8d
	je	.L12
	movzx	edx, BYTE PTR [rax]
	mov	BYTE PTR [rcx], dl
	test	r8b, 2
	je	.L22
	mov	r8d, r8d
	movzx	edx, WORD PTR -2[rax+r8]
	mov	WORD PTR -2[rcx+r8], dx
	mov	rdx, QWORD PTR -8[rax]
	jmp	.L12
.L3:
	ret
.L6:
	mov	rdx, QWORD PTR [rax]
	mov	r10, rax
	mov	QWORD PTR [rcx], rdx
	mov	edx, r8d
	mov	r9, QWORD PTR -8[rax+rdx]
	mov	QWORD PTR -8[rcx+rdx], r9
	lea	rdx, 24[rdi]
	and	rdx, -8
	sub	rcx, rdx
	add	r8d, ecx
	sub	r10, rcx
	and	r8d, -8
	cmp	r8d, 8
	jb	.L22
	and	r8d, -8
	xor	ecx, ecx
.L10:
	mov	r9d, ecx
	add	ecx, 8
	mov	r11, QWORD PTR [r10+r9]
	mov	QWORD PTR [rdx+r9], r11
... (truncated)
```

`relocate_trivial` is **two instructions**: scale `n` by `sizeof(Point)` (`sal rdx, 4`) and tail-jump to `memcpy`. `relocate_string` is a loop that, per element, tests whether the string is in SSO mode (the `cmp rdx, rax` against the local buffer, as in Chapter 6) and either steals the pointer or copies the inline bytes. The trait chose the algorithm at compile time with **no runtime check**; the discarded `if constexpr` branch does not exist in the binary.

Notice the cast to `void*`: GCC's `-Wclass-memaccess` warns on `memcpy` into a non-trivial type; with `if constexpr` guarding it, the cast makes the *intent* explicit. The standard library does exactly this inside `std::vector` growth and `std::uninitialized_move` (libstdc++'s `__is_bitwise_relocatable`/`__relocate_a`), and **C++26's `std::is_trivially_relocatable`** (P2786) lets *you* declare that a class such as `unique_ptr` can be relocated bitwise, which would make `vector<unique_ptr<T>>` growth a `memcpy`.

---

## 8. Assembly / runtime investigation

Traits themselves produce no code; their *consequences* do. To prove a trait changed the output, compile the same function with the trait forced both ways (add a `bool` template parameter and a `static_assert`) and diff:

```bash
g++-14 -std=c++23 -O2 -S -masm=intel -o - reloc.cpp | c++filt | grep -E 'memcpy|memmove|call'
```

To see what a trait costs the **compiler**:

```bash
g++-14 -std=c++23 -fsyntax-only -ftime-report big_template_user.cpp 2>&1 | head -20
clang++-18 -std=c++23 -fsyntax-only -ftime-trace big_template_user.cpp     # then open the JSON
```

To see **why a trait returned false** (the hardest question in practice), use a `static_assert` with a message *per requirement*:

```cpp
// @test fail -std=c++23 err=trivially
#include <string>
#include <type_traits>
static_assert(std::is_trivially_copyable_v<std::string>, "std::string must be trivially copyable for memcpy relocation");
int main() {}
```

When the compiler's message does not say *which* sub-property failed, ask the sub-traits one by one (`is_trivially_copy_constructible`, `is_trivially_destructible`, …), which is also the best exercise for learning what the umbrella traits mean.

---

## 9. Implementation exercise

Implement the following **without** using the standard's equivalent:

1. `my::is_base_of<Base, Derived>` using only overload resolution and `sizeof`/`decltype` (the classic trick: a conversion test between `Derived*` and `Base*` via a private helper). Check against `std::is_base_of` for: unrelated types, private inheritance, ambiguous inheritance, `Base == Derived`, non-class types.
2. `my::common_type<A, B>` as `decltype(true ? declval<A>() : declval<B>())` (with `decay_t`), plus the variadic recursion for `Ts...`.
3. `my::is_detected<Op, Args...>` and `my::detected_t`, then use them to implement `has_value_type<T>`, `has_begin<T>`, and `is_streamable<T>` (can `std::cout << declval<T>()` be formed?).
4. `my::type_list<Ts...>` with `size`, `at<I>`, `contains<T>` and `index_of<T>` (a seed of Chapter 17).

<details>
<summary><strong>Solution for 3, plus a tour of why it works</strong></summary>

```cpp
// @test run -std=c++23 -O0
#include <iostream>
#include <list>
#include <type_traits>
#include <utility>
#include <vector>

namespace my {
namespace detail {
    template <class Default, class AlwaysVoid, template <class...> class Op, class... Args>
    struct detector { using value_t = std::false_type; using type = Default; };

    template <class Default, template <class...> class Op, class... Args>
    struct detector<Default, std::void_t<Op<Args...>>, Op, Args...> {
        using value_t = std::true_type; using type = Op<Args...>;
    };
    struct nonesuch { nonesuch() = delete; ~nonesuch() = delete; nonesuch(const nonesuch&) = delete; void operator=(const nonesuch&) = delete; };
}
template <template <class...> class Op, class... Args>
inline constexpr bool is_detected_v = detail::detector<detail::nonesuch, void, Op, Args...>::value_t::value;
template <template <class...> class Op, class... Args>
using detected_t = typename detail::detector<detail::nonesuch, void, Op, Args...>::type;

// the "archetypes": alias templates whose validity IS the question
template <class T> using value_type_t = typename T::value_type;
template <class T> using begin_t      = decltype(std::declval<T&>().begin());
template <class T> using stream_t     = decltype(std::declval<std::ostream&>() << std::declval<const T&>());
}

struct Opaque {};

int main() {
    std::cout << std::boolalpha;
    std::cout << "has value_type: vector=" << my::is_detected_v<my::value_type_t, std::vector<int>>
              << " int=" << my::is_detected_v<my::value_type_t, int> << '\n';
    std::cout << "has begin():    list=" << my::is_detected_v<my::begin_t, std::list<int>>
              << " Opaque=" << my::is_detected_v<my::begin_t, Opaque> << '\n';
    std::cout << "streamable:     int=" << my::is_detected_v<my::stream_t, int>
              << " Opaque=" << my::is_detected_v<my::stream_t, Opaque> << '\n';
    static_assert(std::is_same_v<my::detected_t<my::value_type_t, std::vector<double>>, double>);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
has value_type: vector=true int=false
has begin():    list=true Opaque=false
streamable:     int=true Opaque=false
```

*Why it works.* `detector`'s primary template is the "no" answer. The partial specialization matches only if `std::void_t<Op<Args...>>` is a valid type, i.e. only if the *alias template* `Op<Args...>` substitutes without error. The alias is the **question** and `void_t` is the **question mark**. The 100-line Boost.TTI-style macro systems this replaced were, in effect, the same trick spelled differently.

</details>

---

## 10. Real-world example

| Where | Trait use |
|---|---|
| **`std::vector` / `std::uninitialized_copy`** | `is_trivially_copyable` → `memmove`; `is_nothrow_move_constructible` → `move_if_noexcept` |
| **`std::variant` / `std::optional`** | Whether copy/move/destructor are **trivial** is *propagated*: `optional<int>` is trivially copyable, `optional<string>` is not. Triviality flows from the parts to the whole, and with it ABI-level register passing |
| **`std::function` / `std::move_only_function`** | `is_nothrow_move_constructible` decides whether the object may live in the small buffer |
| **`std::hash`, `std::atomic<T>`** | `is_trivially_copyable_v<T>` is a *requirement* for `atomic<T>` |
| **`std::thread`, `std::bind`, `make_pair`** | `decay_t` of each argument = the stored type |
| **`std::chrono`** | `common_type<duration<R1,P1>, duration<R2,P2>>` finds the finest-grained common duration |
| **`std::format`** | Detects `formatter<T>` specializations to know whether `T` is formattable |
| **Qt** | `QTypeInfo<T>` is Qt's own trait set (`Q_PRIMITIVE_TYPE`, `Q_MOVABLE_TYPE`) that tells `QList`/`QVector` whether they can use `memmove` for relocation; the pre-`<type_traits>` design of the same idea ([Chapter 47](../part-18-qt/47-modern-cpp-in-qt.md)) |

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Specializing `std::is_*` for your type | UB (`[meta.rqmts]`); works until it doesn't | Wrap in your own trait; for `common_type`/`hash` customization, follow the documented points |
| Using a trait on an incomplete type | Wrong answer **cached** → ODR violation, or hard error | Include the full definition first |
| `conditional_t<C, A, B>` where one branch is invalid for some `T` | Hard error although "the other branch is chosen" | Defer: `typename conditional_t<C, X<A>, Y<B>>::type`, or `if constexpr` |
| `is_same_v<T, int>` when `T` may be `const int&` | False negatives | `is_same_v<std::remove_cvref_t<T>, int>` |
| Using `decay_t` when only qualifiers should go | Arrays/functions silently become pointers | `remove_cvref_t` |
| Checking `is_trivially_copyable` and assuming *copyable* | `NoCopy` is trivially copyable yet non-copyable | Check `is_copy_constructible` as well |
| `static_assert` inside a trait class used for SFINAE | Hard error (Experiment 4) | Make the failure absent (`enable_if`-style) or use a concept |
| `std::is_pod` / `std::result_of` / `std::is_literal_type` | Deprecated/removed | `is_trivially_copyable` + `is_standard_layout` / `invoke_result` / `is_constant_evaluated` |
| `enable_if` on a *default template argument* of two overloads | "redefinition of the template": the declarations are the same signature | Put it in the **return type**, or use a non-type parameter `std::enable_if_t<cond, int> = 0` |
| Detecting a member with `void_t` but it is *private* | Access is checked in the immediate context ⇒ `false`, not an error | Expect it; decide on public-API semantics |
| Deep trait recursion (`index_of` over 200 types) | `template instantiation depth exceeds maximum` and slow builds | Fold expressions, `std::index_sequence`, or constexpr arrays |
| Measuring `is_nothrow_move_constructible` of a template in a header before all specializations are visible | Different answers in different TUs | Declare all specializations first; check with `static_assert` next to the class |

---

## 12. Exercises

1. **Print the matrix.** Write a program that, for the types `int`, `const int&`, `int*`, `int[3]`, `void`, `std::string`, `Plain`, `NoCopy`, `std::unique_ptr<int>`, prints a table of 12 traits. Predict, then verify. Which three cells surprised you?
2. **`remove_cvref` vs `decay`.** Write a forwarding function `store(T&&)` that stores a `std::decay_t<T>`, then pass a string literal. What is stored? Change to `remove_cvref_t`. What now?
3. **Conditional trap.** Write `template <class T> using value_or_self = std::conditional_t<std::is_class_v<T>, typename T::value_type, T>;` and instantiate it with `int`. Explain the error and fix it with a lazily-evaluated trait.
4. **SFINAE-friendly `common_type`.** Implement `my::common_type<A, B>` so that `my::common_type_t<int, std::string>` is *softly* ill-formed (no `type` member) and use that to build a function `maybe_min(a, b)` that is removed from overload resolution for unrelated types.
5. **Rewrite three.** Take three `enable_if` uses from any codebase (or from your own earlier code) and rewrite each as a concept. Compare the error message you get when each fails.
6. **Padding detector.** Write `has_padding<T>` as `!has_unique_object_representations_v<T> && is_trivially_copyable_v<T>` and apply it to `std::pair<int, char>`, `std::array<char, 7>`, `struct {char a; char b;}`; compare with `sizeof` arithmetic from Chapter 2. Where would hashing the raw bytes be a bug?
7. **A `to_string` dispatcher.** `to_str(x)` must choose, **in this order**: `x.to_string()`, `std::to_string(x)` for arithmetic types, `std::string(x)` for string-like, else a `static_assert` with a message listing the supported options. Write it with detection + `if constexpr`; then with concepts; then compare compile errors for an unsupported type.
8. **Trivial relocation experiment.** Benchmark growing a `std::vector<Point>` vs `std::vector<std::string>` by `push_back` of 10⁷ elements; then replace the elements' type with a wrapper that has a non-trivial but semantically trivial copy constructor. How much of the cost is "relocation by `memcpy` lost"? (Use `perf stat`; see Chapter 40.)

---

## 13. Challenge: a compile-time "protocol" checker

Build `static_interface<T, Spec>` where `Spec` is a **type** describing required members, e.g.

```cpp
using Drawable = Protocol<
    Method<"draw",   void(Canvas&) const>,
    Method<"bounds", Rect() const>,
    TypeAlias<"id_type">>;
static_assert(satisfies<Circle, Drawable>);
```

Requirements: names as `FixedString` NTTPs (Chapter 8); detection via the detection idiom and member-pointer decltype tricks (no macros); a failure must produce a **message that names which member is missing or has the wrong signature**, using a `static_assert` in a helper that reports one requirement at a time. Provide a *concept-based* variant using `requires` expressions, and compare: which is shorter, which gives better diagnostics, and what can the type-list approach do that a hand-written concept cannot (answer: *generate* it from a data structure)?

---

## 14. Knowledge check

1. What are the three trait kinds and how do you spot each from its interface?
2. How does `is_same<T, T>` (partial specialization) know two types are identical?
3. What is the difference between `decay_t` and `remove_cvref_t`? When do they differ?
4. What exactly does "the immediate context" mean for SFINAE, and why does it make `static_assert` in a class body a hard error?
5. Why does `std::enable_if` *lack* a `type` member when the condition is false, rather than being an error?
6. Explain `void_t` and why the primary template needs a `class = void` parameter.
7. Why is `std::is_trivially_copyable_v<NoCopy>` true, and what does that imply for `memcpy`-based code?
8. What is the difference between `is_constructible` and `is_convertible`, with an example?
9. Why is it undefined behavior to specialize `std::is_integral<MyInt>`?
10. When is `if constexpr` + a trait better than SFINAE, and when are concepts better than both?
11. Why is `has_unique_object_representations_v<float>` false?
12. What does `std::declval` do, and why can't you call it at runtime?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. *Unary* (`is_x<T>::value`: property of one type), *binary* (`is_x<A, B>::value`: relation), *transformation* (`x<T>::type`: yields a type).
2. The partial specialization `is_same<T, T>` names the *same* parameter twice, so it only matches when both arguments are the identical type; otherwise the primary (`false_type`) is chosen.
3. `remove_cvref_t` strips only `const`/`volatile` and references. `decay_t` additionally converts arrays to pointers and functions to function pointers (the by-value argument type). They differ for arrays and functions.
4. Only errors in the *declaration* (signature, return type, default template arguments) while substituting are soft. Errors that occur while *instantiating a definition* (a class body, a function body) are outside it and are hard errors: the overload set is already decided.
5. So the *nested typename lookup* `enable_if<false>::type` fails softly in the immediate context and the overload disappears. A static_assert or hard error would stop compilation.
6. `void_t<...>` is `void` if all arguments are valid types and a substitution failure otherwise. The primary template has a defaulted `= void` second parameter so that `has_x<T>` means `has_x<T, void>`, which then matches the specialization `has_x<T, void_t<...>>` whenever the `void_t` is valid.
7. "Trivially copyable" means *if* a copy happens, bytewise copy is a valid implementation; it is about the *representation*, and it doesn't require that copying be allowed. Deleted copy operations don't make the class non-trivially-copyable provided at least one of the copy/move operations is non-deleted and trivial (rules of `[class.prop]`). For `memcpy` code, also check `is_copy_constructible`/the semantics you need.
8. `is_constructible<T, Args...>` tests direct-initialization (`T(args...)`) and so allows `explicit`; `is_convertible<From, To>` tests *implicit* conversion. `std::vector<int>` from `size_t` is constructible but not convertible.
9. The standard reserves the meaning of the standard traits for the implementation, and `[meta.rqmts]` makes specializing them ill-formed (UB); the compiler intrinsics won't see your specialization anyway.
10. `if constexpr` + a trait is better than SFINAE when you pick *an implementation inside one function* (no overloads). Concepts are better than both for *selecting/rejecting overloads and classes* because they are named, composable, ordered by subsumption, and give readable diagnostics.
11. `+0.0` and `-0.0` are equal but have different bit patterns, and NaNs have many representations; equal values do not always have equal bytes, so hashing or `memcmp` by bytes is wrong.
12. It returns `T&&` for use in unevaluated contexts, to get "an expression of type `T`" without constructing one. It is declared but **not defined**, so odr-using it at runtime is a link error (and `static_assert` in the implementation catches evaluated use).

</details>

---

[← Previous: Chapter 8](08-templates-deep-dive.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 10 — Concepts →](10-concepts.md)
