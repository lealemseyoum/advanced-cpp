# Chapter 8 — Templates Deep Dive

> **Part IV · Generic programming** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 6 hours**
> **Prerequisites:** [Chapter 7](../part-03-value-categories-and-move/07-perfect-forwarding.md) &nbsp;|&nbsp; **Standards:** C++11 → C++23 &nbsp;|&nbsp; **Tools:** `g++`, `clang++`, `nm`, `-fdump-tree`

[← Previous: Chapter 7](../part-03-value-categories-and-move/07-perfect-forwarding.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 9 — Type traits →](09-type-traits.md)

---

**In one sentence:** a template is not code, it is a **recipe the compiler follows each time it sees a new set of arguments**, and nearly every surprising rule (two-phase lookup, `typename`, lazy instantiation, deduction) follows from that one fact.

**By the end of this chapter you can:**

- say exactly *when* a template is parsed, checked, and instantiated, and what is checked at each point
- explain two-phase name lookup and predict whether a name is found at definition or instantiation
- use `typename` and `template` disambiguators correctly, and say why they exist
- predict template argument deduction for values, references, arrays, functions, braces and CTAD
- choose between full specialization, partial specialization and overloading (and know why function templates cannot be partially specialized)
- use non-type parameters, parameter packs, fold expressions and `if constexpr`
- find instantiations in an object file and reason about code bloat

---

## 1. Problem

You want `max`, `swap`, a `vector`, a `sort`, **once**, for every type, with no runtime penalty and full type checking.

| Approach | What it costs |
|---|---|
| Write it per type | Duplication, divergence, bugs |
| C macros (`#define MAX(a,b) ((a)>(b)?(a):(b))`) | No types, double evaluation, no scope, awful errors |
| `void*` + size + function pointers (`qsort`) | Type safety gone, an indirect call per comparison, nothing inlines |
| Base-class hierarchy (Java-style generics) | Requires every type to inherit; boxing; virtual dispatch per call |
| **Templates** | Compile time, code size, and diagnostics; no runtime cost |

---

## 2. Historical context

| Year | Step |
|---|---|
| 1990 | Templates enter C++ (ARM). Stroustrup's goal: containers as fast as hand-written C |
| 1994 | **Alexander Stepanov's STL** shows generic programming can be *zero-overhead* and algorithm/container independent: the reason templates were accepted into the standard |
| 1998 | Two-phase lookup, partial specialization, `typename` standardized |
| 2000 | Andrei Alexandrescu, *Modern C++ Design*; Boost.MPL: **accidental** Turing-complete template metaprogramming (Chapter 17) |
| 2011 | Variadic templates, `decltype`, `static_assert`, alias templates, `extern template`, default template arguments for functions |
| 2014 | Variable templates, generic lambdas |
| 2017 | **Fold expressions**, **`if constexpr`**, **CTAD**, `auto` NTTP |
| 2020 | **Concepts**, class-type NTTPs, template lambdas, `consteval`; **modules** |
| 2023 | Deducing `this`, `static operator()`; `if consteval` |

The pattern: C++98 templates were powerful but *unconstrained and untyped*: errors appeared deep inside the instantiation. Every later feature either made a template **do more** (variadics, folds, NTTP) or made it **easier to say what it requires** (concepts, `if constexpr`).

---

## 3. Modern solution

```cpp
template <class T, std::size_t N>              // type and non-type parameters
  requires std::is_trivially_copyable_v<T>     // C++20 constraint (Chapter 10)
struct Ring {
    T buf[N];
    template <class... A> void emplace(A&&... a);   // member template with a pack
};

template <class T> constexpr T pi = T(3.14159265358979L);     // variable template

template <class T> using Vec = std::vector<T, MyAlloc<T>>;    // alias template

Ring r{1, 2, 3};     // CTAD: deduces Ring<int, 3> via a deduction guide
```

---

## 4. Mental model

### A template is a *parameterized AST*, not code

```text
  template <class T> T twice(T x) { return x + x; }
          │
          │   parsed ONCE at the definition:  syntax + non-dependent names checked
          ▼
     [ template definition: a parse tree with holes (T) ]
          │
          │   twice(21)  → T = int          twice(2.0) → T = double
          ▼                                 ▼
     substitute T := int              substitute T := double
     check again (dependent names)    check again
          │                                 │
          ▼                                 ▼
     twice<int>  (real function)       twice<double>  (real function)
```

Three consequences you should be able to derive on your own:

1. **Syntax errors are found at definition; type errors that depend on `T` are found at instantiation.** (Two-phase lookup, §5.2.)
2. **Only what is *used* gets instantiated** (§5.1), which is why a class template may contain member functions that do not compile for some `T`.
3. **There is one real function per distinct argument list.** Code size is *your* problem (§6).

### The checklist for any template line

When you read a template, for every name ask: *is this name **dependent** (does its meaning depend on a template parameter)?* If yes, it is looked up and checked **later**, at instantiation. If no, it must already be valid **now**.

---

## 5. Language rules

### 5.1 Instantiation  `[temp.inst]`

| Kind | How | Effect |
|---|---|---|
| **Implicit** | Use the specialization where a *complete type* or definition is required | Instantiates the class declaration; **member function bodies and static data members only when used** |
| **Explicit instantiation definition** | `template class Box<int>;` | Instantiates *all* members here (emits symbols in this TU) |
| **Explicit instantiation declaration** | `extern template class Box<int>;` | "Do not instantiate here; another TU does." Cuts compile time and duplicate code |
| **Explicit (full) specialization** | `template <> struct Box<bool> { … };` | A *different* definition, not an instantiation |

**Lazy instantiation is a language rule, not an optimization.** `Box<int>` below is valid even though `never_called()` could not compile for `int` (Experiment 1).

**Point of instantiation.** For a function template the point is immediately after the namespace-scope declaration containing the use, *and also* the end of the translation unit; compilers are permitted to use either. Code that behaves differently depending on which is chosen is ill-formed, no diagnostic required (`[temp.point]`). In practice: define everything a template uses *before* the template is instantiated, or rely on ADL.

### 5.2 Two-phase name lookup  `[temp.res]`

| Phase | When | What is done |
|---|---|---|
| **1: definition** | The compiler parses the template | Syntax checked. **Non-dependent names** are looked up and bound **now** (ordinary lookup, at the definition context) |
| **2: instantiation** | The template is used with arguments | **Dependent names** are looked up now: *unqualified* dependent function calls use **ordinary lookup at the definition** *plus* **argument-dependent lookup (ADL) from both the definition and instantiation contexts**; qualified dependent names are looked up in the dependent type |

```cpp
void helper(int);                       // visible at the definition

template <class T> void f(T t) {
    helper(1);        // non-dependent: bound NOW to helper(int)
    helper(t);        // dependent: ordinary lookup (at definition) + ADL (at instantiation)
    undeclared_fn(1); // non-dependent and not declared: ERROR now (GCC and Clang), even if never instantiated
}
void helper(double);                    // declared AFTER f: invisible to ordinary lookup inside f…
                                        // …but found by ADL when T is a class type in an associated namespace
```

Practical rule: **a template calling a "customization" function (`swap`, `begin`, `operator<<`, `hash_value`) must make it findable by ADL**, which is exactly why `using std::swap; swap(a, b);` is the idiom and why customization point *objects* (Chapter 15) were invented.

### 5.3 Dependent names: `typename` and `template`  `[temp.names]`

Inside a template, `T::x` could be a type, a value or a template. The compiler can't know until `T` is known, so the **default is "a value"**. You must say otherwise:

```cpp
template <class C>
void g(const C& c) {
    typename C::value_type v = *c.begin();   // typename: "this dependent name is a TYPE"
    c.template get<0>();                     // template: "what follows < is a template argument list"
    typename C::template rebind<int>::other x;   // both
}
```

C++20 relaxed `typename` in some contexts (`P0634`): it is optional in alias declarations, return types of class members, parameter declarations of member functions out-of-class, etc., where only a type can appear. It is still required in the general case.

### 5.4 Template argument deduction  `[temp.deduct]`

For `template <class T> void f(P)` called as `f(arg)`, the compiler matches `P` against the **type of `arg`**:

| `P` | `arg` type | Deduced `T` | Rule |
|---|---|---|---|
| `T` | `const int` | `int` | By value: top-level `const`/`&` dropped; **arrays and functions decay to pointers** |
| `T` | `int[3]` | `int*` | Array-to-pointer decay |
| `T&` | `int[3]` | `int[3]` | No decay with a reference: **array size is deduced** |
| `const T&` | `int` | `int` | The `const` in `P` is stripped from `T` |
| `T&&` | lvalue `int` | `int&` | [Forwarding reference](../part-03-value-categories-and-move/07-perfect-forwarding.md) |
| `T*` | `const int*` | `const int` | |
| `std::vector<T>` | `std::vector<int>` | `int` | Structural match |
| `T(&)[N]` | `int[3]` | `T=int`, `N=3` | Array size is a deducible NTTP |
| `T` | `{1,2}` | **fails** | A braced list is not an expression: **non-deduced** |
| `std::initializer_list<T>` | `{1,2}` | `int` | The one exception |

**Non-deduced contexts** (`[temp.deduct.type]/5`): `T` is *not* deduced from:

- the nested name of a qualified type: `typename X<T>::type`
- the **expression** of a `decltype`
- a template argument that is a non-trivial expression involving a parameter (`array<int, N+1>`)
- a function **parameter that is a braced list**, or an overload set
- a **default argument** (if all else fails the default is used)

If `T` is deduced from several arguments and the results differ, deduction **fails**: `max(1, 2.0)` is an error, and not a conversion.

**Class template argument deduction (CTAD, C++17).** `std::pair p{1, 2.0};` works by synthesizing a set of *deduction guides* from the class's constructors, plus any guides you write:

```cpp
template <class T> struct Wrap { T v; Wrap(T v) : v(v) {} };
template <class U> Wrap(U) -> Wrap<U>;               // explicit deduction guide
```

CTAD is also the reason `std::vector v{1, 2, 3};` prefers `initializer_list`, while `std::vector v(3, 1)` doesn't. (C++20 also adds aggregate CTAD and alias-template CTAD.)

### 5.5 Specialization  `[temp.spec]`

| Form | Class templates | Function templates | Variable templates | Alias templates |
|---|:-:|:-:|:-:|:-:|
| **Primary** | ✅ | ✅ | ✅ | ✅ |
| **Full** (explicit) specialization | ✅ | ✅ | ✅ | ❌ |
| **Partial** specialization | ✅ | **❌** | ✅ | ❌ |

```cpp
template <class T>            struct Traits               { static constexpr const char* n = "other"; };
template <class T>            struct Traits<T*>           { static constexpr const char* n = "pointer"; };   // partial
template <>                   struct Traits<void>         { static constexpr const char* n = "void"; };      // full
```

**Why function templates cannot be partially specialized.** Function templates *overload*, so the language already has a more flexible tool: write another overload. A specialization of a function template is **not** a participant in overload resolution; resolution picks among the *primary templates and overloads* first, and only *then* looks for a specialization of the winner. That produces the classic trap in Experiment 4.

> **Opinion.** For functions: **overload, never specialize.** For classes/variables: specialize freely (partial specialization is how traits work, Chapter 9). Concepts (Chapter 10) make most remaining specializations unnecessary, because constrained overloads express the same selection more clearly.

**Choosing among specializations:** partial ordering selects the *most specialized* matching partial specialization; two equally specialized matches are ambiguous (an error at instantiation).

### 5.6 Non-type template parameters (NTTP)  `[temp.param]`

| Standard | Allowed NTTP types |
|---|---|
| C++98 | integral, enum, pointers/references to objects/functions with linkage, pointer-to-member |
| C++11 | + `std::nullptr_t` |
| C++17 | + `auto` as the type: `template <auto V>` |
| **C++20** | + **floating-point** and **literal class types** with *structural* equality (all bases/members public, non-`mutable`, structural types) |

The C++20 class-type NTTP is what lets a *string* be a template argument: `template <FixedString S> struct Tag;`. Two specializations are the same iff their arguments are *template-argument-equivalent* (memberwise).

### 5.7 Parameter packs and fold expressions  `[temp.variadic]`, `[expr.prim.fold]`

| Fold | Expands to |
|---|---|
| unary right `(args + ...)` | `a1 + (a2 + (a3 + a4))` |
| unary left `(... + args)` | `((a1 + a2) + a3) + a4` |
| binary right `(args + ... + init)` | `a1 + (a2 + (a3 + init))` |
| binary left `(init + ... + args)` | `((init + a1) + a2) + a3` |

Empty pack: allowed only for `&&` (→ `true`), `||` (→ `false`) and `,` (→ `void()`). For every other operator an empty unary fold is ill-formed; use a binary fold with an `init`. The `,` fold is the replacement for the C++11 "initializer-list expansion trick".

### 5.8 `if constexpr`  `[stmt.if]`

Inside a **template**, the condition is evaluated at instantiation and the **discarded branch is not instantiated**: it only has to be syntactically valid, and need not type-check for the given `T`. This is the key difference from a plain `if`.

```cpp
template <class T> auto describe(T t) {
    if constexpr (std::is_pointer_v<T>) return *t;      // not instantiated when T is not a pointer
    else                                return t;
}
```

> [!WARNING]
> The discard rule only applies when the condition is **value-dependent**. In a non-template, or if the condition does not depend on a template parameter, the discarded statement is still fully checked. And a `static_assert(false)` in a discarded branch used to be ill-formed even in a template ("no valid specialization can be generated"), fixed retroactively by P2593 (adopted for C++23, implemented in GCC 13, Clang 17).

### Layer check

| Layer | Decides |
|---|---|
| **Standard** | Two-phase lookup, deduction, specialization rules, lazy instantiation, SFINAE, where discarded statements apply |
| **Compiler** | Instantiation *order* and the point of instantiation; template caches; error-message quality; recursion limits (`-ftemplate-depth=`, default 900 for GCC, 1024 for Clang) |
| **ABI** | Itanium mangling encodes template arguments in symbol names (`_Z5twiceIiET_S0_`, with the return type, unlike plain functions!); instantiations are **weak/COMDAT** symbols so the linker keeps one copy |
| **Linker** | De-duplicates identical template instantiations across TUs; with `-ffunction-sections` + `--icf` can also merge *different-but-identical-code* instantiations (e.g. `vector<int*>` and `vector<long*>`) |

---

## 6. Implementation model

Compilers keep an **instantiation table** keyed by (template, arguments). When `twice<int>` is first needed:

1. substitute `T := int` into a *copy* of the template body
2. perform full semantic analysis (overload resolution, access, constexpr evaluation)
3. emit a function with **weak linkage** into its own section (`.text._Z5twiceIiET_S0_`)

At link time the linker keeps one copy of each weak symbol: this is how templates (and `inline` functions) avoid violating the One Definition Rule even though every translation unit defines them (Chapter 36).

### Where the cost goes

| Cost | Source | Mitigation |
|---|---|---|
| **Compile time** | Each instantiation re-checks the body; recursive templates (TMP) are expensive; header-only libraries re-parse in every TU | `extern template`, precompiled headers/modules, fewer distinct instantiations, concepts instead of SFINAE chains, `-ftime-trace` |
| **Code size** | One function per distinct argument list | Type-erase the non-generic parts (non-template base class), `-fipa-icf`, `--icf=safe`, merge `T*` instantiations by `void*` |
| **Error messages** | Backtrace through every instantiation | Concepts and `static_assert` with messages |

---

## 7. Experiments

### Experiment 1: Instantiation is lazy, and visible in the symbol table

```cpp
// @test run -std=c++23 -O0
#include <cstdio>

template <class T>
struct Box {
    T v;
    T get() const { return v; }
    int never_called() const { return v.no_such_member(); }   // does NOT exist for int…
};

template <class T> T twice(T x) { return x + x; }

int main() {
    Box<int> b{21};                                           // …and that is fine: never used
    std::printf("%d %.1f\n", twice(b.get()), twice(2.0));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
42 4.0
```

`Box<int>` compiled although `never_called()` is ill-formed for `int`. Now look at what the compiler emitted:

```bash
g++-14 -std=c++23 -O0 -c inst.cpp -o inst.o && nm -C inst.o
```

```text
0000000000000000 W double twice<double>(double)
0000000000000000 W int twice<int>(int)
0000000000000000 W Box<int>::get() const
                 U __stack_chk_fail
0000000000000000 T main
```

- Exactly **three** template symbols: `twice<int>`, `twice<double>` and `Box<int>::get`. **Nothing for `never_called`**: lazy instantiation, visible in the binary.
- The letter **`W`** is "weak": the linker may merge duplicates from other TUs. `main` is `T` (strong, defined here).
- Each lives in its own section (`.text._Z5twiceIiET_S0_` ...): the raw material for the linker's garbage collection (`--gc-sections`) and identical-code folding.

The mangled name `_Z5twiceIiET_S0_` reads: function `twice` (5 chars), template args `<int>` (`Ii…E`), **return type** `T_` (the first template parameter, a *template's return type is part of its mangled name*), parameter `S0_` (a substitution for the same type). An ordinary non-template function name does not encode its return type. This is ABI detail, and one of the reasons a template function's signature is more rigid than it looks (Chapter 37).

### Experiment 2: Two-phase lookup, observed

```cpp
// @test fail -std=c++23 err=declared
template <class T>
void f(T t) {
    undeclared_function(1);   // non-dependent and not declared: diagnosed at DEFINITION time
}
// note: f is never instantiated.
int main() {}
```

Now the dependent case that works only through ADL:

```cpp
// @test run -std=c++23 -O0
#include <cstdio>

namespace lib {
    struct Widget {};
}

template <class T>
void call_hook(T t) {
    hook(t);                  // dependent: looked up at instantiation, via ADL
}

namespace lib {
    void hook(Widget) { std::puts("lib::hook(Widget) found by ADL"); }   // declared AFTER call_hook
}

void hook(int) { std::puts("::hook(int)"); }      // also after the template definition

int main() {
    call_hook(lib::Widget{});     // ADL looks in namespace lib at the point of instantiation
    // call_hook(1);              // would NOT compile: ordinary lookup only sees what was declared before the template,
                                  // and `int` has no associated namespace for ADL to search
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
lib::hook(Widget) found by ADL
```

`hook` was not declared when `call_hook` was written, yet the call works because **ADL consults the namespaces associated with the argument type** (`lib`) at instantiation. The *same* mechanism for `int` finds nothing: fundamental types have no associated namespaces. This asymmetry is why library authors put customization functions in the **same namespace as the type they customize** and why `std::swap(a, b)` explicitly qualified would break user overloads.

### Experiment 3: `typename`, `template` and the dependent-name default

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <vector>

struct Registry {
    template <int N> static int slot() { return N * 10; }
    using id_type = unsigned;
    static constexpr int count = 3;
};

template <class R>
void probe() {
    typename R::id_type id = 7;                       // R::id_type is a TYPE → typename
    int c = R::count;                                 // R::count is a value (the default assumption)
    int s = R::template slot<4>();                    // R::slot is a template → template keyword
    std::printf("id=%u count=%d slot<4>=%d\n", id, c, s);
}

template <class C>
auto first_or(const C& c, typename C::value_type fallback) {   // typename in a parameter
    return c.empty() ? fallback : *c.begin();
}

int main() {
    probe<Registry>();
    std::printf("%d\n", first_or(std::vector<int>{}, 99));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
id=7 count=3 slot<4>=40
99
```

Remove `typename` or `template` and the compiler parses `R::id_type id` as a multiplication or `R::slot < 4 > ()` as comparisons. Try it (Exercise 2).

### Experiment 4: Deduction: values, references, arrays, braces

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstddef>
#include <initializer_list>
#include <type_traits>
#include <utility>

template <class T> constexpr const char* kind() {
    using U = std::remove_reference_t<T>;
    if constexpr (std::is_array_v<U>)                  return "array";
    else if constexpr (std::is_function_v<U>)          return "function";
    else if constexpr (std::is_pointer_v<U>)           return std::is_const_v<std::remove_pointer_t<U>> ? "pointer to const" : "pointer";
    else if constexpr (std::is_const_v<U>)             return "const value";
    else                                               return "value";
}

template <class T> void by_value(T)   { std::printf("  by value   : T is %-18s", kind<T>()); }
template <class T> void by_ref(T&)    { std::printf("  by ref     : T is %-18s", kind<T>()); }
template <class T, std::size_t N> void by_array_ref(T (&)[N]) { std::printf("  array ref  : T[N] with N=%zu\n", N); }
template <class T> void by_list(std::initializer_list<T>) { std::printf("  init-list  : T is %s\n", kind<T>()); }
void fn() {}

int main() {
    int arr[3] = {};
    const int ci = 0;
    std::puts("array argument");
    by_value(arr);    std::puts("  <- decays to int*");
    by_ref(arr);      std::puts("  <- stays an array (size kept in the type)");
    by_array_ref(arr);
    std::puts("function argument");
    by_value(fn);     std::puts("  <- decays to function pointer");
    by_ref(fn);       std::puts("  <- stays a function type");
    std::puts("const");
    by_value(ci);     std::puts("  <- top-level const dropped");
    by_ref(ci);       std::puts("  <- const kept");
    std::puts("string literal");
    by_value("abc");  std::puts("  <- const char*");
    by_ref("abc");    std::puts("  <- const char[4]");
    std::puts("braces");
    by_list({1, 2, 3});
    // by_value({1, 2, 3});   // error: cannot deduce T from a braced list
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
array argument
  by value   : T is pointer             <- decays to int*
  by ref     : T is array               <- stays an array (size kept in the type)
  array ref  : T[N] with N=3
function argument
  by value   : T is pointer             <- decays to function pointer
  by ref     : T is function            <- stays a function type
const
  by value   : T is value               <- top-level const dropped
  by ref     : T is const value         <- const kept
string literal
  by value   : T is pointer to const    <- const char*
  by ref     : T is array               <- const char[4]
braces
  init-list  : T is value
```

### Experiment 5: Specialize or overload? The function-specialization trap

```cpp
// @test run -std=c++23 -O0
#include <cstdio>

// A primary template and an overload of it:
template <class T> void f(T)    { std::puts("  f(T)        primary #1"); }
template <>        void f(int*) { std::puts("  f<int*>     specialization of #1"); }   // specializes #1
template <class T> void f(T*)   { std::puts("  f(T*)       primary #2 (an overload)"); }

// The same structure, but the specialization is declared after #2 and so it specializes #2:
template <class T> void g(T)    { std::puts("  g(T)        primary #1"); }
template <class T> void g(T*)   { std::puts("  g(T*)       primary #2"); }
template <>        void g(int*) { std::puts("  g<int*>     specialization of #2"); }

// Overloading (not specializing) is predictable:
template <class T> void h(T)    { std::puts("  h(T)"); }
template <class T> void h(T*)   { std::puts("  h(T*)"); }
void                    h(int*) { std::puts("  h(int*)     plain overload"); }

int main() {
    int x = 0;
    std::puts("f(&x):"); f(&x);     // overload resolution picks #2 (more specialized); #1's specialization is never considered
    std::puts("g(&x):"); g(&x);     // picks #2, and a specialization of #2 exists
    std::puts("h(&x):"); h(&x);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
f(&x):
  f(T*)       primary #2 (an overload)
g(&x):
  g<int*>     specialization of #2
h(&x):
  h(int*)     plain overload
```

`f(&x)` does **not** call the specialization `f<int*>`, even though its parameter matches exactly. Overload resolution runs first, over the *primary templates* `#1` and `#2`; `#2` is more specialized; and `#2` has no explicit specialization (the one written belongs to `#1`). Reordering two lines of source changed which function runs (`g`). Identical-looking code with different behavior based on declaration order is exactly why the advice is **overload, never specialize function templates**.

### Experiment 6: NTTP, folds and `if constexpr`

```cpp
// @test run -std=c++23 -O0
#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// 1. C++20 class-type NTTP: a string as a template argument
template <std::size_t N>
struct FixedString {
    char data[N]{};
    constexpr FixedString(const char (&s)[N]) { std::copy_n(s, N, data); }
    constexpr std::string_view view() const { return {data, N - 1}; }
};

template <FixedString Name>
struct Field {
    static constexpr std::string_view name = Name.view();
};

// 2. Fold expressions
template <class... Ts> constexpr auto sum(Ts... ts)        { return (... + ts); }          // unary left
template <class... Ts> constexpr bool all_positive(Ts... ts) { return ((ts > 0) && ...); } // && with empty pack = true
template <class... Ts> void print_all(const Ts&... ts)     { ((std::printf("[%s]", std::string_view(ts).data())), ...); std::puts(""); }

// 3. if constexpr: the discarded branch is never instantiated
template <class T>
auto stringify(const T& v) {
    if constexpr (std::is_arithmetic_v<T>)       return std::to_string(v);
    else if constexpr (std::is_convertible_v<T, std::string_view>) return std::string(std::string_view(v));
    else                                         return std::string("<?>");
}

// 4. A compile-time index: all fold + index_sequence
template <class F, std::size_t... I>
constexpr void for_each_index(F&& f, std::index_sequence<I...>) { (f(std::integral_constant<std::size_t, I>{}), ...); }

int main() {
    std::printf("Field<\"user_id\">::name = %.*s\n", int(Field<"user_id">::name.size()), Field<"user_id">::name.data());
    static_assert(!std::is_same_v<Field<"a">, Field<"b">>);          // distinct types from distinct strings
    static_assert(std::is_same_v<Field<"a">, Field<"a">>);           // same value → same type

    static_assert(sum(1, 2, 3, 4) == 10);
    static_assert(all_positive(1, 2, 3) && !all_positive(1, -2));
    static_assert(all_positive());                                   // empty && fold == true
    print_all("a", "bc", "def");

    std::printf("%s %s %s\n", stringify(42).c_str(), stringify("text").c_str(), stringify(std::array<int,1>{}).c_str());

    for_each_index([](auto I) { std::printf("%zu ", decltype(I)::value); }, std::make_index_sequence<4>{});
    std::puts("");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
Field<"user_id">::name = user_id
[a][bc][def]
42 text <?>
0 1 2 3 
```

Things worth pausing on:

- `Field<"a">` and `Field<"b">` are **different types**: a string literal became part of the type system. This is how compile-time-routed APIs (`route<"/users/{id}">`, a field named `"user_id"` in a schema, `std::format`'s compile-time format string checking) get built without macros.
- `sum()` with no arguments would be ill-formed (an empty `+` fold); `all_positive()` is fine, because an empty `&&` fold is defined as `true`.
- In `stringify`, if the check were a regular `if`, `std::to_string(v)` would be compiled for the `std::array` argument and fail. `if constexpr` removes it.

### Experiment 7: Code bloat and `extern template`

```bash
cat > bloat.cpp <<'EOF'
#include <vector>
#include <string>
int use_a() { std::vector<std::string> v; v.push_back("x"); return (int)v.size(); }
EOF
g++-14 -std=c++23 -O0 -c bloat.cpp -o a.o
# the same instantiations again in another TU:
cp bloat.cpp bloat2.cpp && sed -i 's/use_a/use_b/' bloat2.cpp && g++-14 -std=c++23 -O0 -c bloat2.cpp -o b.o
nm -C --defined-only a.o | grep -c ' W '      # weak symbols per TU
nm -C --defined-only b.o | grep -c ' W '
g++-14 a.o b.o -o /dev/null -shared 2>&1; echo "link ok: the linker merged the duplicate weak instantiations"
```

```text
102
102
link ok: the linker merged the duplicate weak instantiations
```

The experiment uses `-O0` on purpose: at `-O2` the tiny functions are inlined and **no weak symbols survive at all** (the same program yields 0), which is itself a lesson: instantiation count and emitted-symbol count are different things.

Each translation unit carries its own copy of `std::vector<std::string>::_M_realloc_insert` and friends. They are `W`eak, the linker keeps one. That is correct and **free at runtime**, but you paid **compile time** twice. The fix at scale:

```cpp
// widget_vec.h
extern template class std::vector<std::string>;       // "someone else instantiates this"
// widget_vec.cpp
template class std::vector<std::string>;              // the one place that does
```

Use it for heavy templates you instantiate with the same arguments in hundreds of TUs (your own `Matrix<double>`, a logger `fmt::format`). For standard containers, the savings are small; measure with `-ftime-trace` (Clang) or `-ftime-report` (GCC) before bothering.

---

## 8. Assembly / runtime investigation

Three questions to answer for any template:

```bash
# (1) which instantiations exist?
nm -C prog.o | grep ' W '

# (2) how big is each?
nm -C --size-sort -S prog.o | tail -20

# (3) how much compile time did each cost? (Clang)
clang++-18 -std=c++23 -O2 -ftime-trace -c prog.cpp      # open prog.json in chrome://tracing or speedscope.app
# GCC: -ftime-report prints a per-phase summary
```

A useful experiment: write `template <class T> int f(T x) { return x * 2; }` and call it with `int`, `unsigned`, `long`, `short`. Four symbols, four *nearly identical* function bodies. With `-O2` and `-ffunction-sections -Wl,--icf=all` (gold or lld) the linker can fold the ones that are byte-identical. The take-away: instantiation count is a *design* quantity; consider it when you write `template <class T>` around a body that does not depend on `T`.

---

## 9. Implementation exercise

Implement the following, each using the language feature named:

1. **`my::tuple<Ts...>`** (recursive inheritance or `std::index_sequence` storage) with `get<I>` and CTAD.
2. **`my::apply(f, tuple)`** with `std::index_sequence` and your `my::invoke` from Chapter 7.
3. **A `FixedString` NTTP-based `static_map`**: `template <FixedString... Keys> struct KeySet { static constexpr int index(std::string_view k); };` where lookup is `constexpr`.
4. **`overloaded{...}` helper**: the `struct overloaded : Ts... { using Ts::operator()...; }; template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;` pattern, then use it with `std::visit`. Identify which feature each line of the helper needs (pack expansion in base list, using-declaration pack, deduction guide).

<details>
<summary><strong>Solution for 4, with the dissection</strong></summary>

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <string>
#include <variant>

template <class... Ts>
struct overloaded : Ts... {              // pack expansion in the base-specifier list
    using Ts::operator()...;             // C++17: pack expansion in a using-declaration
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;  // deduction guide (needed pre-C++20; C++20 aggregate CTAD can synthesize it)

int main() {
    std::variant<int, std::string, double> v = std::string("hi");
    auto visitor = overloaded{
        [](int i)                { std::printf("int %d\n", i); },
        [](const std::string& s) { std::printf("string %s\n", s.c_str()); },
        [](double d)             { std::printf("double %g\n", d); },
    };
    std::visit(visitor, v);
    v = 2.5;
    std::visit(visitor, v);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
string hi
double 2.5
```

Each lambda is a distinct closure type with one `operator()`. Deriving from all of them and **`using`-ing each `operator()`** pulls the whole set into one class scope so overload resolution sees them together (names in different base classes would otherwise be hidden/ambiguous). Four features, one line each.

</details>

---

## 10. Real-world example

| Library | Template technique |
|---|---|
| **libstdc++ `std::vector<T>`** | Lazy instantiation of members; `if constexpr`/traits select `memmove` for trivially-relocatable-like paths |
| **`std::format`** | Class-type NTTP-style and `consteval` constructor validate the format string *at compile time*; `std::formatter<T>` is a **partial specialization** customization point |
| **`{fmt}`** | Same idea in C++11 era; shows what compile-time validation costs in build time |
| **Eigen** | *Expression templates*: `a + b * c` builds a type that encodes the expression and evaluates it in one loop at assignment, no temporaries |
| **Boost.Hana / Mp11** | Type-level metaprogramming (Chapter 17) |
| **Qt** | Mostly *avoids* templates in its core API: `QObject` signals/slots use the `moc` code generator and macros, partly for ABI reasons and compile times (Chapter 48) |

> **Verdict on template-heavy design.** Templates are the right tool when you need *zero-overhead, type-checked genericity*: containers, algorithms, numeric kernels, wrappers. They are the wrong default for **application and plugin boundaries**: those want a small, concrete, stable interface (ABI, compile time, error messages). The pragmatic shape is a **template inside, a concrete interface outside**: the public header declares a non-template class; the `.cpp` instantiates the templates it needs.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Forgetting `typename` | `error: need 'typename' before 'T::x' because 'T' is a dependent scope` | Add `typename`; in C++20 many contexts no longer need it |
| Forgetting `template` before `obj.f<3>()` | "expected primary-expression before ‘)’" or comparison parse | `obj.template f<3>()` |
| Specializing a function template | Specialization silently not chosen (Experiment 5) | Overload; or delegate to a class template specialization |
| Specialization after first use | `specialization of … after instantiation` (ill-formed NDR) | Declare specializations before any use, in the same header |
| ODR violation: two different definitions of the same template in different TUs | Silent wrong behaviour; the linker keeps one | One definition in a header; check with `-Wodr` + LTO |
| Template body depends on declaration order | Works in one TU, fails in another (point of instantiation) | Make everything the template needs visible before; use ADL |
| Unconstrained template with a greedy parameter | Hijacks overloads (Chapter 7, Experiment 3) | Concepts |
| Recursive instantiation too deep | `template instantiation depth exceeds maximum of 900` | Fold expressions, `std::index_sequence`, `constexpr` loops |
| `static_assert(false)` in a discarded branch | Fires, or is flagged pre-C++23 | Dependent false: `static_assert(sizeof(T) == 0)`; GCC 13+/Clang 17+ implement P2593 |
| Defining a template member in a `.cpp` and calling from another TU | Linker error `undefined reference to Box<int>::get() const` | Define in the header, or explicitly instantiate in the `.cpp` |
| `T` deduced from conflicting arguments (`max(1, 2.5)`) | No matching function | Explicit `max<double>(...)`, or `std::common_type_t` (Chapter 9) |
| Template parameter named like a macro (`min`, `max`, `ERROR`) | Preprocessor eats it (Windows headers) | Rename; `(std::max)(a,b)` |
| Large template in a public header | Build times balloon; every TU re-parses | Forward declare, `extern template`, pImpl, modules |

---

## 12. Exercises

1. **Lazy or not?** Build a class template with a member function whose *declaration* (not body) is ill-formed for some `T`, e.g. returns `typename T::x`. Is the class instantiable for `int`? What if the bad construct is in the *body* only? What if it is a data member?
2. **Break the parse.** In Experiment 3 remove `typename`, then `template`, then both. Record the exact error message of each on GCC and Clang. How do the two compilers differ?
3. **Two-phase.** Write a template that calls an overloaded function `log(int)` and `log(double)`, with `log(double)` declared *after* the template. For `f<double>` with a `double` argument, which `log` is called? Why does ADL not help here? Fix it two ways.
4. **Deduction table.** For `template <class T> void f(T, T)` predict `f(1, 2)`, `f(1, 2L)`, `f(1, 2.0)`, `f("a", "b")`, `f("a", "bc")`, `f(arr, arr2)`. Check. Which fail, and which succeed *surprisingly*?
5. **CTAD guides.** Write `Pair<A, B>` with a constructor `Pair(A, B)` and CTAD. Then add a `Pair(const char*, const char*) -> Pair<std::string, std::string>` deduction guide. Why does `Pair p{"a", "b"}` deduce `const char*` without it?
6. **Specialization ordering.** Write `Traits<T>` and four partial specializations (`T*`, `const T*`, `T[N]`, `T(*)(Args...)`), and a function that prints which was selected. Find an ambiguous call by adding `T* const`.
7. **Fold realities.** Implement `all_of(pred, args...)`, `any_of`, `count_if`, and a `print_sep(sep, args...)` that prints `a, b, c` with no trailing separator, using only fold expressions (hint: the comma fold with a lambda that tracks "first").
8. **Instantiation cost.** Write a recursive `Fib<N>` template and an equivalent `constexpr` function. Compile `Fib<30>` and `fib(30)` under `-ftime-report`; compare memory and time. (Chapter 17 explains the result.)
9. **`extern template`.** Take a 10-TU project that includes a heavy header-only template with the same arguments in all TUs. Measure build time before and after adding `extern template`, and `nm | grep ' W '` counts.

---

## 13. Challenge: a compile-time command-line parser

Using only templates and `constexpr`, build:

```cpp
using Cli = Parser<
    Flag<"verbose", 'v'>,
    Option<"output", 'o', std::string>,
    Option<"jobs",   'j', int>>;
auto args = Cli::parse(argc, argv);           // returns an object with typed accessors:
args.get<"jobs">()   // int
args.get<"verbose">() // bool
// args.get<"nonexistent">()   → compile error with a readable message
```

Requirements: names as `FixedString` NTTPs; types deduced from the declarations; unknown names diagnosed by `static_assert` with a custom message (not a deep error); no dynamic allocation for the option table; parse `--jobs=4`, `-j 4`, `-vj4`. Then answer: what did this design cost you in compile time and code size compared to a `std::map<std::string, std::any>`, and what did it buy?

---

## 14. Knowledge check

1. At which point is a template's syntax checked, and at which point are its `T`-dependent expressions checked?
2. What is a *dependent name*, and what are the defaults for how the compiler parses one? Which keywords override them?
3. `template <class T> void f(T t) { g(t); g(1); }`, with `g` declared **after** `f`. Which call can succeed and why?
4. What is the argument-dependent lookup set of a call `g(x)`, and why is it consulted at instantiation?
5. Deduce `T`: `template <class T> void f(T&); const int a[3]; f(a);`
6. Why can't function templates be partially specialized, and what should you do instead?
7. Why does `f(&x)` in Experiment 5 not call the explicit specialization written for `int*`?
8. What does the `W` in `nm` output mean, and why do templates need it?
9. Which empty-pack folds are well-formed, and what do they yield?
10. When is the discarded branch of `if constexpr` *not* ignored?
11. What changed for NTTPs in C++20, and what does it enable?
12. Why does an explicit instantiation declaration (`extern template`) help, and what does it *not* change at runtime?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Syntax and **non-dependent** names at the template *definition*; dependent expressions when the template is *instantiated* with concrete arguments (the second phase).
2. A name whose meaning depends on a template parameter (`T::x`, `f(t)`). The default is that a dependent qualified name is **a value** (not a type or template), so you must write `typename T::x` for types and `T::template f<..>` for member templates.
3. `g(t)` can succeed *if `T` is a class type and `g` is found by ADL* (in an associated namespace at the point of instantiation). `g(1)` is non-dependent, bound at definition, and `g` isn't yet declared: **error**, even if `f` is never instantiated.
4. The namespaces and classes associated with the argument types (the namespace of a class type, its bases, template arguments' namespaces). It is consulted at instantiation so that user types can supply their own overloads of functions called by generic code.
5. `T = const int[3]`; with `T&`, there is no decay, and `const` is kept; the parameter is `const int (&)[3]`.
6. Functions already overload, and specializations don't participate in overload resolution (Experiment 5), which makes them surprising. Overload the function (possibly with a more constrained template), or forward to a class template that you *can* specialize.
7. Overload resolution chooses between the primary templates (`#1` and `#2`) first; `#2` (`T*`) is more specialized and wins; the explicit specialization was of `#1`, so it is not considered.
8. **W**eak symbol: the object defines it but other objects may too, and the linker may pick any one. Templates and `inline` functions are defined in every TU that uses them; weak/COMDAT linkage makes this legal.
9. Only `&&` (`true`), `||` (`false`) and `,` (`void()`). All other operators require a binary fold with an initial value.
10. When the condition is **not value-dependent** (outside a template, or independent of template parameters), the discarded statement is still fully checked.
11. Floating-point types and **literal class types with structural equality** can be NTTPs; this enables strings as template arguments (`FixedString`) and compile-time configuration objects as types.
12. It prevents each TU from instantiating the template and emitting its own weak copy: **compile time and object size** go down. Runtime behaviour is unchanged (one copy ends up in the program either way); inlining is *less* likely across the boundary unless the definition remains visible (and it normally still is).

</details>

---

[← Previous: Chapter 7](../part-03-value-categories-and-move/07-perfect-forwarding.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 9 — Type traits →](09-type-traits.md)
