# Chapter 18 — The Reflection Landscape

> **Part VII · Compile-Time C++** &nbsp;|&nbsp; **Level 4** &nbsp;|&nbsp; **≈ 5 hours**
> **Prerequisites:** [Chapter 16](16-constexpr.md), [Chapter 17](17-template-metaprogramming.md) &nbsp;|&nbsp; **Standards:** none before C++26 · **C++26 (standardized, March 2026): static reflection (P2996) + expansion statements** &nbsp;|&nbsp; **Tools:** `g++-14`, `clang++-18` (today); **GCC 16** for the C++26 sections

[← Previous: Chapter 17](17-template-metaprogramming.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 19 — Runtime polymorphism →](../part-08-polymorphism/19-runtime-polymorphism.md)

---

**In one sentence:** C++ programs cannot ask *"what are the members of this struct?"* or *"what is this enumerator called?"*, so for thirty years we have worked around the gap with macros, code generators and compiler-specific tricks; C++26 finally closes it with **static reflection**, and this chapter shows both the workarounds you will still maintain and the mechanism that replaces them.

> **Status legend used in this chapter.**
> ✅ **Standardized and verified here** (compiled with GCC 14.2 / Clang 18 in this repository)
> 🟡 **Standardized in C++26, not verifiable on this course's toolchain** (needs GCC 16, which is not installed here). Code is written from the adopted design, is **marked `@test skip`**, and **has not been compiled by me**. Treat API spellings as "check against your compiler's headers".
> 🔧 **Compiler extension** (works today, not portable)
> 🧪 **Library hack** (works today by exploiting language corners; fragile)

**By the end of this chapter you can:**

- list exactly what C++ can and cannot introspect today
- apply the four standard workarounds (macros, external codegen, compiler-name tricks, aggregate decomposition) and say when each breaks
- read the C++26 reflection model: reflection values, the `^^` and `[: :]` operators, the `<meta>` library, expansion statements
- sketch how enum-to-string, struct serialization and CLI-parsing look with reflection
- judge *when to wait for C++26* and when the workaround is the right call

---

## 1. Problem

C++ has **compile-time type information** (`sizeof`, `decltype`, `is_class`, concepts) but **no way to enumerate** the parts of a declaration:

```cpp
enum class Color { Red, Green, Blue };
struct Point { int x; double y; std::string label; };

std::string_view to_string(Color c);                   // must list every enumerator, by hand, forever
std::string to_json(const Point& p);                   // must list every member, by hand, forever
Color from_string(std::string_view s);                 // ... and keep three lists in sync
```

Everything programmers want to automate is a variation of the same question: *given a declaration, enumerate its pieces (members, enumerators, bases, parameters, attributes) as ordinary compile-time data.*

| Use case | Needs |
|---|---|
| `enum` ⟷ string | enumerator names and values |
| Serialization (JSON, binary, protobuf-like), ORM, RPC | member names, types, offsets |
| Command-line / config parsing into a struct | member names, types, default values |
| `std::formatter` / `operator<<` / `operator==` / `hash` for aggregates | the member list |
| GUI property editors; Qt's meta-object system | names, types, signals/slots |
| Python / Lua bindings (pybind11, sol2) | everything above, plus functions |
| Mock / test-double generation | member functions and signatures |

---

## 2. Historical context

| Period | Approach | Representative |
|---|---|---|
| 1990s | Macros; hand-written lists | MFC `DECLARE_MESSAGE_MAP`, COM IDL |
| 1990s–today | **External code generators**: parse headers or an IDL, emit C++ | Qt **moc** (Chapter 47), protobuf, flatbuffers, SWIG, Cap'n Proto |
| 2000s | **Intrusive registration macros** | `BOOST_FUSION_ADAPT_STRUCT`, `BOOST_DESCRIBE_STRUCT`, `BOOST_HANA_DEFINE_STRUCT` |
| 2000s | Run-time reflection via RTTI/`typeid` | *Only* the type's name and `dynamic_cast`; no members |
| 2010s | **Compiler-name tricks** (🔧): parse `__PRETTY_FUNCTION__` / `__FUNCSIG__` | `magic_enum`, `nameof`, `ctti` |
| 2016–2020 | **Aggregate decomposition** (🧪): count fields via brace-initialization probing and decompose with structured bindings | Boost.PFR ("magic get") |
| 2014–2018 | Reflection TS / `reflexpr` (type-based, template-heavy) | Clang fork; abandoned |
| 2019–2023 | **Value-based reflection** (constexpr-first) redesign: P1240 (Sutton, Vali, Childers) → P2996 (Childers, Katz, Revzin, Sutton, Vali, Vandevoorde, …) | Bloomberg's Clang fork; EDG implementation |
| **2025** | **P2996 adopted** for C++26 (June 2025, Sofia); expansion statements (P1306) adopted | — |
| **March 2026** | **C++26 technically complete** (London/Croydon meeting): reflection is in the standard; contracts, `std::execution`, hardening also | Herb Sutter's trip report calls reflection "by far the biggest upgrade … since the invention of templates" |
| 2026 | **GCC 16** ships reflection (`-freflection`) and expansion statements; Clang/MSVC not yet (cppreference compiler-support table at time of writing) | — |

**The design pivot that made it work:** earlier proposals tried to express reflection *as types and templates* (`reflexpr(T)` returned a type-level object manipulated by metafunctions). That forced all of Chapter 17's recursion onto every reflection user. P2996 instead makes reflections **values of a single type, `std::meta::info`**, manipulated with **ordinary `constexpr` functions** (Chapter 16): loops, `std::vector`, `std::ranges`.

---

## 3. Modern solution

Reflection in C++26, in three moves:

```cpp
#include <meta>                                   // 🟡 C++26

constexpr std::meta::info r = ^^Color;           // 1. REFLECT: ^^ turns a declaration into a value of type std::meta::info
constexpr auto names = std::meta::enumerators_of(r);   // 2. QUERY: ordinary constexpr functions on info values
template for (constexpr auto e : names) { ... [:e:] ... }  // 3. SPLICE: [: info :] turns a value back into code
```

| Operator / facility | Role |
|---|---|
| `^^ entity` | **Reflect**: produce a `std::meta::info` for a type, namespace, variable, member, enumerator, template … |
| `[: r :]` | **Splice**: turn a reflection back into the expression / type / namespace it denotes |
| `std::meta::*` functions (`members_of`, `nonstatic_data_members_of`, `enumerators_of`, `identifier_of`, `type_of`, `offset_of`, `is_public`, `substitute`, `extract<T>`, …) | **Query and compute** at compile time |
| `template for` (expansion statement) | **Iterate** a `constexpr` range of heterogeneous elements, one expansion per element |
| `std::meta::define_aggregate`, `std::define_static_array` | **Generate** types and static data from compile-time values |
| Annotations (`[[=value]]`) | Attach compile-time data to declarations, readable by reflection |

---

## 4. Mental model

```text
                       source declaration                      "the compiler knows this"
                              │
                 ^^  (reflect: lift into the constexpr world)
                              ▼
        ┌─────────────────────────────────────────────┐
        │   std::meta::info  (an opaque value)        │      ordinary constexpr C++:
        │   members_of(r) → vector<info>              │ ◄──  loops, vectors, ranges, strings,
        │   identifier_of(m) → string_view            │      if/else, your own functions
        │   type_of(m) → info                         │
        └─────────────────────────────────────────────┘
                              │
                 [: :]  (splice: lower back into code)
                              ▼
                  generated expressions / types / members
```

Contrast with Chapter 17: *there*, the compile-time values were types and the "program" was recursive specialization. *Here*, the values are `info` objects and the program is a `constexpr` function with a `for` loop. Reflection moves *types and declarations* into the world Chapter 16 already made pleasant.

> **Compile-time vs. run-time.** All reflection happens **at compile time**. A `std::meta::info` cannot exist at run time (it is a *consteval-only type*); the *results* (strings, sizes, generated code) can. So reflection cannot discover types at run time (no dynamic loading, no plug-in discovery): that remains the domain of registration, RTTI and type erasure.

---

## 5. Language rules

### 5.1 What exists in standard C++ today ✅

| Capability | Mechanism | Limits |
|---|---|---|
| `sizeof`, `alignof`, `offsetof` (standard-layout only), `decltype`, `typeid(T).name()` | core language | no member list; `name()` is implementation-defined (often mangled) |
| Category queries | `<type_traits>`: `is_class`, `is_enum`, `is_aggregate`, `is_trivially_copyable`, `is_standard_layout`, `underlying_type` | yes/no questions only |
| Tuple-like protocol | `std::tuple_size`/`tuple_element`/`get`, **structured bindings** | You must already know the arity; works for aggregates *if you write the binding* |
| Concepts / `requires` | check for the *presence* of a named operation | cannot *enumerate* operations |
| `std::source_location` | file/line/**function name** of the call site | the name of the *enclosing function*, not of a type |
| Attributes | `[[nodiscard]]`, etc. | not user-readable at compile time |

### 5.2 The workarounds, and how each breaks

**(a) X-macros** ✅ **portable, everything is a macro.** One master list expands into the enum, the string table and the parser:

```cpp
#define COLORS(X) X(Red) X(Green) X(Blue)
```

Robust, zero-magic, works since C. Cost: the macro list *is* the declaration, so IDEs and refactoring tools see macro soup; no per-enumerator values or attributes without more macro arguments.

**(b) External code generators** ✅ **portable, build-system complexity.** Parse headers with libclang or a small script and emit C++. This is moc, protobuf, and every serious ORM. Robust and fully flexible; cost: a second tool in the build, a second language for the generator, error messages that point at generated code.

**(c) Compiler-name parsing** 🔧 `__PRETTY_FUNCTION__` (GCC/Clang) and `__FUNCSIG__` (MSVC) contain the text of *template arguments*, including a **non-type template argument that is an enumerator**. `magic_enum` instantiates `f<(Color)0>()`, `f<(Color)1>()`, … and reads the name out of the string. Works for enums in a limited value range; breaks on flag enums, large ranges, out-of-range values, different compilers (format differs), and costs *compile time* for every probe (Experiment 2).

**(d) Aggregate decomposition** 🧪 `Boost.PFR` counts fields by testing whether `T{any, any, ...}` is constructible with *N* arguments convertible to anything; then binds with a structured binding of that arity (`auto& [a, b, c] = obj;`). Gives *positions and types*, not **names** (names come only through a further compiler-specific hack, or not at all). Breaks on aggregates with base classes, C arrays, reference members, non-aggregates (any user constructor), and arity limits.

**(e) Intrusive registration** ✅ `BOOST_DESCRIBE_STRUCT(Point, (), (x, y, label))`: you list the members once, the macro generates the metadata. Honest, portable, **and a second list that can drift** from the struct.

### 5.3 C++26 static reflection (adopted, 🟡)

**Reflection values.** `^^X` yields a `std::meta::info`. `info` is a *scalar, structural* but **consteval-only** type: it can be a `constexpr` variable or a template argument, never a run-time value.

**Splicing** `[: r :]` is permitted wherever the thing it denotes would be: an expression (`obj.[:member:]`), a type (`typename [:type_of(m):]`), a template name, a namespace. It is a **constant expression context**: `r` must be a constant. Where a dependent splice would be ambiguous, `typename` / `template` disambiguators apply as usual.

**The `<meta>` library** (representative functions; the shapes below follow P2996 as adopted, but exact names and signatures, especially the **`access_context`** parameter added late, must be checked against your implementation's header):

| Function | Returns |
|---|---|
| `members_of(r, ctx)`, `nonstatic_data_members_of(r, ctx)`, `enumerators_of(r)`, `bases_of(r, ctx)`, `static_data_members_of(r, ctx)` | `std::vector<info>` (compile-time-only vector) |
| `identifier_of(r)`, `display_string_of(r)`, `has_identifier(r)` | `std::string_view` of the name (a `consteval` string) |
| `type_of(r)`, `parent_of(r)`, `template_of(r)`, `template_arguments_of(r)` | `info` |
| `is_public(r)`, `is_static_member(r)`, `is_class_member(r)`, `is_enumerator(r)`, `is_type(r)`, … | `bool` |
| `offset_of(r)`, `size_of(r)`, `alignment_of(r)`, `bit_size_of(r)` | layout information |
| `extract<T>(r)` | the *value* a constant reflection represents |
| `reflect_constant(v)`, `reflect_object(o)` | lift a value/object into an `info` |
| `substitute(template_info, args)` | instantiate a template from reflections |
| `define_aggregate(type_info, member_specs)` | **inject** a new class definition (code generation) |

**Access control.** Reflection queries take an *access context*: by default only members accessible from the point of the query are visible, so reflection does **not** silently bypass `private` (`std::meta::access_context::unchecked()` is the explicit, auditable escape hatch). This answers the long-standing "reflection breaks encapsulation" objection: it respects it unless you say otherwise.

**Expansion statements** (`template for`, P1306, adopted). A `for` loop over a *compile-time* sequence whose body may differ per element (different types), because it is **unrolled at translation time**:

```cpp
template for (constexpr auto m : std::define_static_array(std::meta::nonstatic_data_members_of(^^T, ctx)))
    out += std::format("{}={}, ", std::meta::identifier_of(m), obj.[:m:]);
```

`std::define_static_array` converts a *transient* compile-time vector (Chapter 16's transient-allocation rule) into a static array that outlives the evaluation: exactly the gap Chapter 16 left open.

**What reflection is not.**

- Not run-time: no `info` after compilation.
- Not (in C++26) a way to generate *arbitrary* code: injection is limited to `define_aggregate` and a few related facilities; **statement/function-body injection was not adopted**.
- Not a replacement for virtual dispatch, RTTI or type erasure.
- Not free: every query and splice costs compile time (§6).

### Layer check

| Layer | Decides |
|---|---|
| **Standard (C++26)** | `^^`, `[: :]`, `std::meta::info`, `<meta>` library, expansion statements, access-context rules. Names such as `identifier_of` are **implementation-defined for some entities** (e.g. unnamed or special members) and *encoding of identifiers* follows the literal/execution character-set rules |
| **Compiler** | Whether it exists at all (GCC 16: yes behind `-freflection`; Clang/MSVC: not at time of writing); the speed of queries; the quality of diagnostics. Today's 🔧 tricks (`__PRETTY_FUNCTION__`) are entirely compiler behaviour |
| **ABI** | Reflection does not change object layout or mangling. It can *generate* a type, and that generated type mangles like any other (`define_aggregate` types get a compiler-chosen name) |
| **OS / CPU** | Nothing: all of it is gone before code generation; the generated code is the code you would have written by hand |

---

## 6. Implementation model

### What the compiler does for each query

A reflection query forces the front end to **materialize** declarations it might otherwise have skipped (all members of a class, including implicitly-declared special members). A splice then instantiates whatever it names. Consequences you should expect, based on how template instantiation behaves (Chapter 17), though precise numbers need measurement on a real GCC 16:

- Each `members_of` on a large class costs time proportional to the number of members.
- `template for` expands to **N copies** of the body; compile time and code size scale with N.
- Generated code is ordinary: no run-time reflection data is emitted unless *you* emit it (e.g. a `static constexpr` table of names).

### What the 🔧/🧪 workarounds do (verified here, GCC 14 and Clang 18)

- **`__PRETTY_FUNCTION__`** is a string literal the compiler formats, including template arguments. Its exact format is **not specified** and differs between GCC, Clang and MSVC.
- **Aggregate decomposition** relies on the fact that `T{x1, …, xN}` is well-formed iff `N ≤` the number of aggregate elements (with brace elision caveats), probed by a type `any` with a templated conversion operator.

---

## 7. Experiments

### Experiment 1 ✅: Type names from the compiler (🔧)

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <string_view>
#include <vector>
#include <map>
#include <string>

// Parse the enclosing function's name. The exact text format is compiler-specific:
// GCC:   "constexpr std::string_view type_name() [with T = int; std::string_view = std::basic_string_view<char>]"
// Clang: "std::string_view type_name() [T = int]"
template <class T> constexpr std::string_view type_name() {
    std::string_view p = __PRETTY_FUNCTION__;
#if defined(__clang__)
    constexpr std::string_view key = "[T = ";
    auto start = p.find(key) + key.size();
    return p.substr(start, p.rfind(']') - start);
#elif defined(__GNUC__)
    constexpr std::string_view key = "with T = ";
    auto start = p.find(key) + key.size();
    return p.substr(start, p.find(';', start) - start);
#endif
}

struct Widget {};
enum class Color { Red, Green, Blue };

int main() {
    auto show = [](std::string_view s) { std::printf("  %.*s\n", int(s.size()), s.data()); };
    show(type_name<int>());
    show(type_name<const volatile unsigned long&>());
    show(type_name<Widget>());
    show(type_name<std::map<std::string, std::vector<int>>>());
    show(type_name<Color>());
    show(type_name<decltype([] {})>());
    static_assert(type_name<int>() == "int");              // usable in constant expressions
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
  int
  const volatile long unsigned int&
  Widget
  std::map<std::__cxx11::basic_string<char>, std::vector<int> >
  Color
  main()::<lambda()>
```

I ran the same file with `clang++-18`. The differences are exactly the ones the standard leaves open:

| Type | GCC 14.2 | Clang 18 |
|---|---|---|
| `const volatile unsigned long&` | `const volatile long unsigned int&` | `const volatile unsigned long &` |
| `std::map<std::string, std::vector<int>>` | `std::map<std::__cxx11::basic_string<char>, std::vector<int> >` | `std::map<std::basic_string<char>, std::vector<int>>` |
| a lambda | `main()::<lambda()>` | `(lambda at c1.cpp:33:29)` (includes the *file name*) |

Neither spells `std::string`, and the lambda's name even embeds a file path. That variance is the whole argument against building a library API on top of this trick, even though `type_name` is perfect for **debug output and error messages**. (Experiments 2 and 3 produced identical output on both compilers.)

### Experiment 2 🔧: Enum-to-string by probing with `__PRETTY_FUNCTION__` (the `magic_enum` technique)

```cpp
// @test run -std=c++23 -O0
#include <array>
#include <cstdio>
#include <string_view>
#include <utility>

// For an enumerator value V, GCC prints  "... name() [with E V = Color; ... = Color::Red]"  vs.  an out-of-range cast "(Color)7".
template <auto V> constexpr std::string_view raw_name() {
#if defined(__clang__) || defined(__GNUC__)
    std::string_view p = __PRETTY_FUNCTION__;
    #if defined(__clang__)
        auto end = p.rfind(']');
        auto start = p.rfind("V = ", end) + 4;
    #else
        auto end = p.rfind(';') == std::string_view::npos ? p.rfind(']') : p.rfind(';');
        auto start = p.rfind("V = ", end) + 4;
    #endif
    return p.substr(start, end - start);
#endif
}

// A name is valid iff the probed text looks like  "Color::Red"/"Red", NOT like "(Color)5".
template <auto V> constexpr bool is_valid_name() { return raw_name<V>().find('(') == std::string_view::npos; }

template <auto V> constexpr std::string_view enum_name_one() {
    auto n = raw_name<V>();
    auto pos = n.rfind("::");
    return pos == std::string_view::npos ? n : n.substr(pos + 2);
}

template <class E, int Lo = 0, int Hi = 16, std::size_t... Is>
constexpr std::string_view enum_name_impl(E e, std::index_sequence<Is...>) {
    std::string_view result{};
    ((static_cast<int>(e) == Lo + int(Is) && is_valid_name<static_cast<E>(Lo + int(Is))>()
          ? (result = enum_name_one<static_cast<E>(Lo + int(Is))>(), true) : false), ...);
    return result;
}
template <class E> constexpr std::string_view enum_name(E e) { return enum_name_impl<E>(e, std::make_index_sequence<16>{}); }

enum class Color { Red, Green, Blue };
enum Level { Debug = 3, Info = 4, Error = 9 };            // unscoped; sparse values

int main() {
    static_assert(enum_name(Color::Green) == "Green");
    static_assert(enum_name(Level::Error) == "Error");
    static_assert(enum_name(static_cast<Color>(7)).empty());              // not an enumerator: empty
    for (Color c : {Color::Red, Color::Green, Color::Blue}) std::printf("%.*s ", int(enum_name(c).size()), enum_name(c).data());
    std::puts("");
    std::printf("Level 9 -> %.*s;  Level 5 (invalid) -> '%.*s'\n", int(enum_name(Error).size()), enum_name(Error).data(),
                int(enum_name(static_cast<Level>(5)).size()), enum_name(static_cast<Level>(5)).data());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
Red Green Blue 
Level 9 -> Error;  Level 5 (invalid) -> ''
```

How it works: for each candidate integer `i` in `[0,16)`, the compiler *instantiates* `raw_name<(Color)i>()`; its `__PRETTY_FUNCTION__` prints the **enumerator's name** when `i` is a real enumerator and `(Color)i` when not. That is a **probe over a fixed numeric range**: sixteen instantiations *per enum type*, which is why real libraries cap the range (`MAGIC_ENUM_RANGE_MAX`) and why enums with large values (flags such as `1 << 20`) don't work. It is a clever hack and a good illustration of what the language *cannot* do directly.

> **Opinion.** Fine for a debug helper or a private tool. **Don't ship it in a public API**: the range cap, the compile-time cost per enum, and the dependence on unspecified string formats are all failure modes you would be signing for. Use the X-macro, a generator, or wait for C++26.

### Experiment 3 🧪: Aggregate decomposition: counting fields and visiting them (Boost.PFR in 40 lines)

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

// A type convertible to anything, used to probe "can T be brace-initialized with N arguments?"
struct any { template <class T> constexpr operator T() const; };

template <class T, class... Args> constexpr bool brace_constructible = requires { T{std::declval<Args>()...}; };

template <class T, class... Args>
constexpr std::size_t count_fields() {
    if constexpr (brace_constructible<T, Args..., any>) return count_fields<T, Args..., any>();   // one more still works: try more
    else return sizeof...(Args);
}

// Decompose with a structured binding of the right arity (needs one overload per arity: the unavoidable boilerplate).
template <class T> auto as_tuple(T& t) {
    constexpr std::size_t N = count_fields<std::remove_cvref_t<T>>();
    if constexpr (N == 0) return std::tie();
    else if constexpr (N == 1) { auto& [a] = t; return std::tie(a); }
    else if constexpr (N == 2) { auto& [a, b] = t; return std::tie(a, b); }
    else if constexpr (N == 3) { auto& [a, b, c] = t; return std::tie(a, b, c); }
    else if constexpr (N == 4) { auto& [a, b, c, d] = t; return std::tie(a, b, c, d); }
    else static_assert(N <= 4, "extend the table");
}

template <class T, class F> void for_each_field(T& obj, F&& f) {
    std::apply([&](auto&... fields) { (f(fields), ...); }, as_tuple(obj));
}

struct Point  { int x; double y; };
struct Person { std::string name; int age; Point home; bool admin; };
struct NotAgg { NotAgg() {} int a; };                              // user constructor: not an aggregate

int main() {
    static_assert(count_fields<Point>() == 2);
    static_assert(count_fields<Person>() == 4);
    static_assert(!std::is_aggregate_v<NotAgg>);

    Person p{"ada", 36, {1, 2.5}, true};
    std::printf("Person has %zu fields. Types seen by a generic visitor:\n", count_fields<Person>());
    for_each_field(p, [](auto& field) {
        using F = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::is_same_v<F, std::string>) std::printf("  string: %s\n", field.c_str());
        else if constexpr (std::is_same_v<F, int>)    std::printf("  int   : %d\n", field);
        else if constexpr (std::is_same_v<F, bool>)   std::printf("  bool  : %d\n", field);
        else                                          std::printf("  other (%zu bytes)\n", sizeof(F));
    });
    for_each_field(p, [](auto& field) { if constexpr (std::is_same_v<std::remove_cvref_t<decltype(field)>, int>) field += 1; });
    std::printf("age after increment: %d\n", p.age);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
Person has 4 fields. Types seen by a generic visitor:
  string: ada
  int   : 36
  other (16 bytes)
  bool  : 1
age after increment: 37
```

What you get: *positions and types, no names, mutable access*, enough for `==`, hashing, binary serialization and a generic debug dump. What you do not get: names (you cannot print `{"age": 36}`), nested-aggregate flattening without extra care, or **any** support for types with constructors, base classes (before C++17's relaxation, and with complications after), references, or `const` members. This is *the* standard example of 🧪: clever, widely used (Boost.PFR ships it), and **dependent on corner behaviour of brace elision** (a compiler change or an added private member silently changes `count_fields`).

### Experiment 4 ✅: The portable baseline: an X-macro that stays in sync

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

// ONE list. Everything else is generated from it, so nothing can drift.
#define COLORS(X) \
    X(Red,   0xFF0000) \
    X(Green, 0x00FF00) \
    X(Blue,  0x0000FF)

enum class Color {
#define X(name, rgb) name,
    COLORS(X)
#undef X
};

constexpr std::string_view to_string(Color c) {
    switch (c) {
#define X(name, rgb) case Color::name: return #name;
        COLORS(X)
#undef X
    }
    return "?";
}

constexpr unsigned rgb(Color c) {
    switch (c) {
#define X(name, rgb_) case Color::name: return rgb_;
        COLORS(X)
#undef X
    }
    return 0;
}

constexpr std::optional<Color> from_string(std::string_view s) {
#define X(name, rgb) if (s == #name) return Color::name;
    COLORS(X)
#undef X
    return std::nullopt;
}

inline constexpr std::size_t color_count = 0
#define X(name, rgb) + 1
    COLORS(X)
#undef X
    ;

static_assert(to_string(Color::Green) == "Green");
static_assert(rgb(Color::Blue) == 0x0000FF);
static_assert(*from_string("Red") == Color::Red && !from_string("Purple"));
static_assert(color_count == 3);

int main() {
    for (auto s : {"Red", "Blue", "Purple"}) {
        auto c = from_string(s);
        std::printf("%-7s -> %s\n", s, c ? std::string(to_string(*c)).c_str() : "(none)");
    }
    std::printf("count=%zu  rgb(Green)=%06X\n", color_count, rgb(Color::Green));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
Red     -> Red
Blue    -> Blue
Purple  -> (none)
count=3  rgb(Green)=00FF00
```

The X-macro is **ugly but honest**: no extra tools, no compiler dependence, it works from C, and it compiles *faster* than any template trick. For an enum you own, it is still a perfectly good answer in 2026.

### Experiment 5 🟡 (not compiled here): Enum to string with C++26 reflection

```cpp
// @test skip   (requires GCC 16 -freflection; not verifiable on this course's toolchain)
#include <meta>
#include <string_view>
#include <optional>

template <class E> requires std::is_enum_v<E>
constexpr std::string_view to_string(E value) {
    template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^E))) {
        if (value == [:e:]) return std::meta::identifier_of(e);
    }
    return "?";
}

template <class E> requires std::is_enum_v<E>
constexpr std::optional<E> from_string(std::string_view s) {
    template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^E))) {
        if (s == std::meta::identifier_of(e)) return [:e:];
    }
    return std::nullopt;
}

enum class Color { Red, Green, Blue };
static_assert(to_string(Color::Green) == "Green");          // the enum is declared normally; nothing else to maintain
static_assert(from_string<Color>("Blue") == Color::Blue);
```

Compare with Experiments 2 and 4: no macro list, no `__PRETTY_FUNCTION__`, no numeric range cap, works for sparse and large values, flag enums, scoped and unscoped, and **the enum is declared in the ordinary way**. The loop is an ordinary loop over a `constexpr` range; `template for` is needed (instead of `for`) because each iteration splices a *different* enumerator, so the body is instantiated per element.

### Experiment 6 🟡 (not compiled here): A generic struct printer and a CLI parser

```cpp
// @test skip   (requires GCC 16 -freflection; not verifiable on this course's toolchain)
#include <meta>
#include <format>
#include <string>

// All data members (public ones, from this context), as a compile-time array.
template <class T> consteval auto members() {
    return std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
}

template <class T>
std::string describe(const T& obj) {
    std::string out = std::string(std::meta::identifier_of(^^T)) + "{";
    bool first = true;
    template for (constexpr auto m : members<T>()) {
        if (!first) out += ", ";
        first = false;
        out += std::format("{}={}", std::meta::identifier_of(m), obj.[:m:]);       // member NAME and VALUE, both known at compile time
    }
    return out + "}";
}

struct Point { int x; double y; };
// describe(Point{1, 2.5})  ==  "Point{x=1, y=2.5}"
```

This is the use case that justifies the whole feature: Experiment 3 could not print member *names*; here names and values come from the declaration itself. The same loop can drive JSON/binary serialization, `operator==`, hashing, ORM column mapping, or the registration code that pybind11 users write by hand (Chapter 46). The same loop can also drive a CLI parser (one option per member, with annotations carrying help text); I have deliberately **not** shown annotation syntax because I could not verify it.

### Experiment 7 ✅: What an external generator looks like (moc in miniature)

```bash
# gen_enum.py: read a tiny description, emit a header.  Build systems run it as a custom command.
cat > colors.def <<'EOF'
Color: Red Green Blue
EOF

python3 - <<'EOF'
name, rest = open("colors.def").read().strip().split(":")
items = rest.split()
print(f"enum class {name} {{ {', '.join(items)} }};")
print(f"constexpr const char* to_string({name} v) {{ switch (v) {{")
for i in items: print(f'  case {name}::{i}: return "{i}";')
print('} return "?"; }')
EOF
```

```text
# output (python 3, run once by hand; this block is not auto-verified)
enum class Color { Red, Green, Blue };
constexpr const char* to_string(Color v) { switch (v) {
  case Color::Red: return "Red";
  case Color::Green: return "Green";
  case Color::Blue: return "Blue";
} return "?"; }
```

This is, line for line, what Qt's moc and every protobuf compiler do, at larger scale: **a second program reads a description and writes C++**. CMake integration is `add_custom_command(OUTPUT … COMMAND … DEPENDS …)`. The generator can be as smart as you like, **and** it runs on every compiler today. Its drawback is not power but friction: a second language, a second place to debug, a build step, and generated code that nobody reads.

---

## 8. Assembly / runtime investigation

There is **no runtime** to investigate for reflection itself, which is the point. What is worth verifying:

```bash
# (1) Prove the X-macro / enum_name results are compile-time: no string comparison, no call, at -O2.
g++-14 -std=c++23 -O2 -S -masm=intel -o - prog.cpp | grep -E "call|cmp.*Red" | head     # expect nothing for static_assert-only use

# (2) What do the probes cost the compiler?  (the numbers that decide whether the hack is acceptable)
g++-14 -std=c++23 -ftime-report -c prog.cpp -o /dev/null 2>&1 | grep -E "template instantiation|TOTAL"
clang++-18 -std=c++23 -ftime-trace -c prog.cpp -o /dev/null           # open prog.json in chrome://tracing / Perfetto

# (3) Experiment 3's field count is compile-time too; verify the generated code is just loads and stores:
g++-14 -std=c++23 -O2 -S -masm=intel -o - prog.cpp | awk '/^main:/,/ret/' | head -40

# (4) With GCC 16 (when you have it): compile Experiment 5 and see that `to_string` becomes a lookup/jump table,
#     identical to the hand-written switch.
g++-16 -std=c++26 -freflection -O2 -S -masm=intel -o - enum.cpp
```

Experiment 2's cost grows linearly with the probed range per enum type; measure it on your own codebase before adopting it (Exercise 3).

---

## 9. Implementation exercise

1. **Flag enums.** Extend Experiment 2 (or the X-macro) so a *bit-flag* enum prints as `Read|Write`. Which approach copes with `1u << 20`? Which does not?
2. **`==`, `<`, `hash` from Experiment 3.** Generate `operator==` and a `std::hash` for any aggregate with `for_each_field`. Show where it silently gives wrong answers (a `const char*` member, a padding-sensitive memcmp shortcut).
3. **An X-macro struct.** Make a single `FIELDS(X)` list that generates a struct, `operator==`, a JSON writer, and a `static constexpr` table of `{name, offset, size}`. Verify `offsetof` consistency with `static_assert`.
4. **A header generator.** Write a ~50-line Python script that reads a minimal IDL (struct name + typed fields) and emits a header with the struct, `operator==`, and `to_json`. Integrate via CMake `add_custom_command` and make the build rebuild only when the IDL changes.
5. **On paper (C++26).** Using only the API table in §5.3, write the pseudocode of: a struct-to-JSON function; a `Variant`-like tagged union generated from a list of types; a "visitor interface" generated from a class's virtual functions. Mark every API point you're unsure about. When GCC 16 is available, compile them and record every correction you had to make.
6. **Compare.** Write the same `enum ⟷ string` facility with all four approaches (X-macro, `__PRETTY_FUNCTION__`, generator, C++26 reflection), then rank them for: compile time, portability, readability, error messages, ability to add per-enumerator metadata.

---

## 10. Real-world example

| Where | Today | With C++26 reflection |
|---|---|---|
| **Qt moc** | Separate preprocessor generates `moc_*.cpp` for signals, slots, properties (Chapter 47) | Much of what moc extracts could be done with reflection + annotations; Qt has not committed to replacing moc; the build and tooling story is larger than the feature gap |
| **protobuf / flatbuffers / Cap'n Proto** | IDL → code generator | Their *schema* is not C++, so they keep generators; reflection helps **C++-native** schemas |
| **nlohmann::json, cereal, Boost.Serialization** | Macros (`NLOHMANN_DEFINE_TYPE_INTRUSIVE`) or per-type `serialize` functions | Zero-boilerplate `to_json` for any aggregate/class |
| **pybind11 / nanobind / sol2** | Hand-written binding lists | Auto-generated bindings from class declarations (Chapter 46) |
| **spdlog / fmt** | Per-type `formatter` | `formatter` for any aggregate, member names included |
| **gtest / Catch2** | `operator<<` or printers for failure messages | Automatic printing of arbitrary structs |
| **Game engines** | Custom code generators (Unreal's UHT, Unity-like editors), reflection macros | Property systems and editors from plain structs |
| **Embedded / HPC** | Often cannot afford RTTI; use X-macros and generators | Zero-overhead compile-time metadata |

> **Opinion.** Reflection is the most important C++ feature since lambdas, and I expect it to delete three categories of code: hand-written `to_string`/`from_string` for enums, per-type boilerplate serializers, and a large share of project-specific code generators. **But** (1) it isn't available on Clang or MSVC yet; (2) it adds *compile time*, which will matter in big codebases; (3) it works at compile time only, so run-time plug-in discovery still needs registration. **Recommendation today:** write **new** code against a small *internal interface* (`to_string(E)`, `for_each_member(obj, f)`) implemented with the X-macro/generator/PFR now, so that switching the implementation to C++26 reflection later is a one-file change. Do not scatter `__PRETTY_FUNCTION__` hacks through a codebase.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Public API built on `__PRETTY_FUNCTION__` | Different output on GCC/Clang/MSVC and across versions | Debug-only use; or a generator |
| `magic_enum` range too small/large | Missing names / huge compile times | Explicit range per enum; or X-macro |
| Aggregate decomposition on a type that gained a constructor or base | Silent wrong field count or a compile error far away | `static_assert(std::is_aggregate_v<T>)` and a field-count assertion in a test |
| Relying on field **names** from Boost.PFR-style tricks | Not portable | Intrusive description macro, or C++26 |
| Two parallel lists (struct and description macro) | Drift: new field not serialized | X-macro that *generates* the struct; or a test that compares `sizeof`/field count |
| Using C++26 reflection in a library that must build on Clang/MSVC | Build failure for downstream users | Feature-test macro (`__cpp_impl_reflection`/`__cpp_lib_reflection`: check the exact names) with fallback |
| Expecting reflection to find types at run time | Not possible: `info` is consteval-only | Registration, RTTI, `std::type_info`, type erasure |
| Reflecting private members through `unchecked` access context | Breaks encapsulation of third-party types | Use the default access context; reserve `unchecked` for types you own |
| Large `template for` over hundreds of members | Slow build, large object code | Table-driven approach: build a `constexpr` table of function pointers |
| Generated code not under version control or CI | "Works on my machine" builds | Generate in the build tree; make the generator a first-class target; check generated output in code review during development |
| Treating `std::meta::info` as a runtime value | Compile error ("consteval-only type") | Keep it in `constexpr`/`consteval` contexts; copy results into ordinary types |

---

## 12. Exercises

1. **Status check.** Look up the current state of reflection in GCC, Clang, MSVC and EDG, using cppreference's compiler-support page and the compilers' own release notes. Record the compiler version and the exact flag needed. (This chapter's table will be stale in a year; the *habit* of checking is the deliverable.)
2. **Clang vs GCC.** Run Experiments 1–3 with `clang++-18`. Diff the output. Explain each difference in terms of "specified by the standard" versus "chosen by the compiler".
3. **Probe cost.** Measure compile time of Experiment 2 for 1, 10, 100 enums, with a range of 16 and 256. Plot compile time against enum count × range.
4. **Break PFR.** For Experiment 3, construct (a) an aggregate with a base class, (b) one with a `std::array<int,3>` member, (c) one with a reference member, (d) one with a private member added later. For each, describe what `count_fields` returns, whether the program compiles, and whether the result is *wrong without a diagnostic*.
5. **Moc-lite.** Write a generator (Python) that reads a header with `Q_PROPERTY`-like comments (`// @property int width`) and emits getter/setter/`property(name)` tables. Compare to what moc does for the same class.
6. **Reflection wish list.** Write the five things you most want to automate in your own codebase, then for each, say whether C++26 *standard* reflection can do it (name the facility), or whether it needs something not adopted (statement injection, run-time reflection).

---

## 13. Challenge: a `describe()` facility with three back ends

Define a small interface used across a codebase:

```cpp
template <class T> constexpr auto fields(T&);   // returns a tuple of (name, reference) pairs
```

and implement it **three ways** behind one header selected by a feature macro:

1. an **X-macro/description-macro** back end (portable; the struct is declared through the macro)
2. an **aggregate-decomposition** back end plus a parallel `constexpr` name array (portable, positions only, names supplied separately and cross-checked with `static_assert`)
3. a **C++26 reflection** back end (🟡: compile on GCC 16 if you can; otherwise write it and mark it unverified)

Then build on `fields()`: `to_json`, `operator==`, `hash`, a CLI parser filling the struct from `--name=value` arguments, and a `diff(a, b)` that reports which fields differ by name. Provide a test suite that runs against every available back end, measure compile time per back end for 10, 50, 200 structs, and write a one-page recommendation for your team.

---

## 14. Knowledge check

1. What is the one kind of information that C++ (before C++26) cannot extract from a type, and why do all workarounds exist?
2. Name the four categories of workaround and one weakness of each.
3. Why is `__PRETTY_FUNCTION__`-based enum-to-string limited to a numeric range?
4. What does Boost.PFR's field-count trick rely on? What kinds of types defeat it?
5. In P2996, what *is* a reflection, and why can it not exist at run time?
6. What do `^^` and `[: :]` do? Where is a splice allowed?
7. Why does C++26 need `template for` (expansion statements) rather than a plain `for`?
8. What does `std::define_static_array` solve? (Connect to Chapter 16.)
9. How does reflection interact with `private` members?
10. Why did the committee move from type-based reflection (`reflexpr`) to value-based reflection?
11. List three things C++26 reflection cannot do.
12. What should a library author do today to be ready for C++26 reflection without depending on it?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. The *members/enumerators/parameters* of a declaration and their *names*, i.e. enumeration of the declaration's parts. `sizeof`/traits give properties of the whole type only. Everything else (macros, generators, name-parsing, PFR) simulates that missing enumeration.
2. Macros/X-macros (a second list or macro soup; declarations hidden in macros); external generators (extra tool, build complexity); compiler-name parsing (unspecified, compiler-dependent, range-limited); aggregate decomposition (positions only, breaks on non-aggregates/bases/references; fragile brace-elision behaviour). Intrusive description macros (a fifth, hybrid) drift from the struct.
3. It must *instantiate a function template per candidate value* and read its name; the candidates are a fixed numeric range, because the compiler cannot enumerate "all values of the enum". Big or sparse values fall outside it.
4. That `T{x1..xN}` is well-formed exactly when N ≤ number of aggregate elements (probed with a type convertible to anything), plus structured bindings of the matching arity. Defeated by user-declared constructors (non-aggregates), reference/`const` members, C arrays (flattening quirks), private members, and, depending on language version, base classes.
5. A value of the opaque type `std::meta::info` identifying a declaration or entity. It's a *consteval-only* type: values only exist during constant evaluation, so they can't be stored or passed at run time (their *results* can).
6. `^^X` produces the reflection of `X`; `[: r :]` splices a constant reflection back into code (expression, type, template, namespace). A splice must have a constant operand and appears wherever the denoted construct would be valid.
7. Each iteration may splice a *different* member of a *different type*; the body must therefore be instantiated per element at translation time, which a run-time `for` (one body with one set of types) cannot do.
8. It turns a *transient* compile-time container (e.g. the `std::vector<info>` returned by a query) into a static array that persists past the constant evaluation: the escape route from Chapter 16's transient-allocation rule.
9. By default reflection respects access: queries run in an access context and only report accessible members; `access_context::unchecked()` is an explicit opt-in to bypass it.
10. The type-based design required all manipulation through template metaprogramming (recursion, instantiation cost, unreadable code); value-based reflection uses ordinary `constexpr` code (loops, containers, algorithms), which is cheaper and far more readable.
11. Any three of: exist at run time (no run-time reflection or plug-in discovery); inject arbitrary statements/function bodies (only limited class definition is adopted); change access rules silently; find types not visible in the translation unit; run on compilers that haven't implemented it yet.
12. Hide the metadata behind a small internal interface (`to_string`, `for_each_member`, `fields`) with one portable implementation now, add a feature-test-guarded reflection implementation later, and test both.

</details>

---

**Sources used for the C++26 status in this chapter:** Herb Sutter, *C++26 is done! Trip report: March 2026 ISO C++ standards meeting (London Croydon, UK)* ([herbsutter.com](https://herbsutter.com/2026/03/)); cppreference *C++26 compiler support* ([en.cppreference.com/w/cpp/compiler_support/26](https://en.cppreference.com/w/cpp/compiler_support/26)); the P2996 paper *Reflection for C++26* ([isocpp.org/files/papers/P2996R4.html](https://isocpp.org/files/papers/P2996R4.html); this is **revision 4**: later revisions changed details such as the `access_context` parameter, so check the final text before relying on a signature).

[← Previous: Chapter 17](17-template-metaprogramming.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 19 — Runtime polymorphism →](../part-08-polymorphism/19-runtime-polymorphism.md)
