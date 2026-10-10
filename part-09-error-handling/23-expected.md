# Chapter 23 — `std::expected` and Errors as Values

> **Part IX · Error Handling** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 5 hours**
> **Prerequisites:** [Chapter 13](../part-05-standard-library/13-optional-variant-any-expected.md), [Chapter 22](22-exceptions.md) &nbsp;|&nbsp; **Standards:** C++11 (`error_code`), C++17 (`optional`), **C++23 (`std::expected`, monadic operations)**, C++26 (`optional` range support; `expected` unchanged) &nbsp;|&nbsp; **Tools:** `g++-14`

[← Previous: Chapter 22](22-exceptions.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 24 — Dynamic memory →](../part-10-memory/24-dynamic-memory.md)

---

**In one sentence:** `std::expected<T, E>` makes failure an ordinary return value that the type system forces you to look at, trading the exception mechanism's rare-case cost (microseconds) for a small, uniform cost on every call (a few bytes and a branch).

**By the end of this chapter you can:**

- use `std::expected` and its monadic operations (`and_then`, `transform`, `or_else`, `transform_error`) to build error-propagating pipelines
- explain its layout, its calling-convention behaviour and what the compiler does with it
- design an error type (`enum`, `std::error_code`, rich struct, `variant`) deliberately
- measure the **failure-rate crossover** between exceptions and `expected` on your machine
- choose between *exception / `expected` / `optional` / error code* and defend the choice

---

## 1. Problem

Chapter 22 left a gap. Exceptions are excellent when failure is rare and the caller can't handle it locally; they are poor when failure is an **ordinary outcome**:

```cpp
// Parsing a stream of untrusted input: 30% of lines are malformed. That's normal, not exceptional.
for (auto& line : lines) {
    try { out.push_back(parse(line)); }          // ~2-3 µs per bad line
    catch (const parse_error&) { ++bad; }
}
```

The older alternatives each lose something:

| Alternative | What's wrong |
|---|---|
| Return a sentinel (`-1`, `nullptr`, `""`) | Can be ignored, may collide with a valid value, carries no reason |
| `bool f(T& out)` | Out-parameter; the value is not constructed in place; easy to ignore the `bool` |
| `int` error code + out-parameter (C style) | Same, plus codes mean different things in different libraries |
| `std::optional<T>` | Tells you *that* it failed, not **why** |
| `std::pair<T, error_code>` | Both members always exist; nothing stops you reading the value of a failed result |
| Exceptions | Microseconds per failure; invisible in the signature; some projects forbid them |

What we want: a return type that **either holds the value or holds the reason**, cannot be silently mistaken for a success, composes without boilerplate, and costs nothing when you don't fail.

---

## 2. Historical context

| Year | Event |
|---|---|
| 1970s–90s | Error codes everywhere in C (`errno`, `HRESULT`, Win32 `GetLastError`) |
| 1990s | ML/Haskell `Either`/`Result`: sum types as the error mechanism (Haskell `Either a b`, Rust later) |
| 2011 | `std::error_code` / `std::error_category` / `std::system_error` (C++11): a standard error vocabulary, for use with **either** error codes or exceptions |
| 2012 | Andrei Alexandrescu, *Systematic Error Handling in C++* (`Expected<T>`) |
| 2013–15 | Boost.Outcome precursors, Vicente Botet's `expected` proposals (N4015 → P0323) |
| 2015 | Rust 1.0: `Result<T, E>` and `?` make errors-as-values mainstream |
| 2017 | `std::optional` (C++17) |
| 2018–21 | LLVM `Expected<T>`/`ErrorOr<T>`, Abseil `StatusOr<T>`, Boost.Outcome v2 widely deployed |
| **2022–23** | **`std::expected` accepted into C++23** (P0323R12); monadic operations (`and_then`, `transform`, `or_else`) added to `optional` and `expected` (P2505, P2549) |
| 2024 | libstdc++ 12+ / libc++ 16+ / MSVC 19.33 implement it; GCC 14 has the full monadic set |
| 2025–26 | Ongoing: no language-level `?`/`try` operator was adopted for C++26 (P2561 "try" operator remains a proposal) |

---

## 3. Modern solution

```cpp
enum class ParseError { empty, not_a_number, out_of_range };

std::expected<int, ParseError> parse_int(std::string_view s);        // success: an int; failure: why
std::expected<int, ParseError> checked_double(std::string_view s) {
    return parse_int(s)
        .and_then([](int v) -> std::expected<int, ParseError> {      // runs only on success; may itself fail
            if (v > 1'000'000) return std::unexpected(ParseError::out_of_range);
            return v;
        })
        .transform([](int v) { return v * 2; });                     // runs only on success; wraps the result
}

if (auto r = checked_double(line)) use(*r);                          // test, then dereference
else                                report(r.error());               // the reason is right there
```

| Operation | Meaning |
|---|---|
| `std::expected<T,E>` | Holds either a `T` (the *expected* value) or an `E` (the *unexpected* error): a tagged union |
| `std::unexpected(e)` | Wrapper to construct the error alternative unambiguously |
| `r.has_value()` / `explicit operator bool` | Test |
| `*r`, `r->` | Access the value (**undefined** if it holds an error) |
| `r.value()` | Access the value, **throws `std::bad_expected_access<E>`** if error |
| `r.error()` | Access the error (undefined if it holds a value) |
| `r.value_or(x)` | Value, or `x` on error |
| `r.and_then(f)` | If value: `f(value)` (must return an `expected` with the same `E`); else pass the error through |
| `r.transform(f)` | If value: wrap `f(value)` in a new `expected`; else pass the error through |
| `r.or_else(f)` | If error: `f(error)` (may recover); else pass the value through |
| `r.transform_error(f)` | Map the error type |

---

## 4. Mental model

### A sum type with a branch you can see

```text
   expected<int, Err>                            optional<int>              variant<int, Err>
   ┌─────────────┬───────────────┐               ┌──────────┬─────────┐     ┌─────────────┬───────────┐
   │ union {     │ bool has_val  │               │ int      │ engaged │     │ union{int,Err}│ index     │
   │   int val;  │               │               └──────────┴─────────┘     └─────────────┴───────────┘
   │   Err unex; │               │               "value or nothing"         "one of N alternatives, any N"
   │ }           │               │
   └─────────────┴───────────────┘
   "value or reason"      same storage as variant<int, Err> with a fixed 2-alternative, value-biased API
```

### Railway-oriented programming

Think of each step as a two-track railway: a **success track** and a **failure track**. `and_then`/`transform` run only while on the success track; once a step fails the train switches to the failure track and every remaining step is skipped, with the error arriving unchanged at the end.

```text
   input ─► parse ──ok──► validate ──ok──► compute ──ok──► result      (success track)
              │              │               │
              └─error────────┴─error─────────┴────────────► error      (failure track: later steps skipped)
```

Compare the same flow with exceptions: the failure track is the **stack unwinding** itself (invisible in the types, expensive, automatic). With `expected` the failure track is **explicit data flow** (visible in every signature, cheap, written out or composed with combinators).

### The three costs of `expected`, and the one it removes

| | Exceptions | `expected` |
|---|---|---|
| Cost of the **happy** path | none (tables) | a branch per call on the caller side + the discriminant bit stored and returned |
| Cost of a **failure** | µs (Chapter 22) | ~ns (a return and a branch per frame) |
| Failure visible in the signature | no | **yes** |
| Can be silently ignored | yes (by not catching: it propagates) | **yes, the other way: a discarded `expected` is not diagnosed** (see §5.5 and Experiment 5) |
| Works in constructors | yes | no (use a factory returning `expected<T,E>`) |
| Propagation | automatic | manual or via `and_then` chains (no language `?` operator) |

---

## 5. Language rules

### 5.1 Class template `[expected]` (C++23)

`std::expected<T, E>` requires `E` to be a valid *unexpected* type (a non-array, non-cv object type, not `unexpected<>`) and `T` to be a destructible type or `void`. The object holds exactly one of a `T` or an `E`, in union storage plus a `bool`. Defining properties:

- **Value-oriented API.** `expected` mimics `T`: `operator*`/`operator->` give the value; comparisons between `expected`s and with values compare values.
- **Constructibility.** `expected<T,E>` is copy-/move-constructible only if both `T` and `E` are; it is **trivially destructible** iff both are; it is **trivially copy/move constructible** when both are (the standard requires this; this lets it be passed in registers; Experiment 4).
- **`expected<void, E>`** is "success with no value or an error" (replaces `bool` + error).
- `std::unexpected<E>` is a thin wrapper that exists so the error constructor can't be confused with the value constructor when `T` and `E` are convertible from each other (e.g. `expected<std::string, std::string>`).
- `value()` throws `std::bad_expected_access<E>` (carrying a copy of the error); `error()` and `operator*` on the wrong alternative are **undefined behaviour** (hardened libstdc++ asserts).

### 5.2 Monadic operations  `[expected.object.monadic]` (C++23)

```cpp
r.and_then (f)   // f: T  -> expected<U, E>   (same E!)   chain a step that can fail
r.transform(f)   // f: T  -> U                             map the value
r.or_else  (f)   // f: E  -> expected<T, G>                recover or re-fail with another error
r.transform_error(f) // f: E -> G                          map the error
```

All have lvalue/rvalue and const overloads, so they move values through the chain. Mixing error types requires an explicit `transform_error` (or `or_else`) so conversions are visible.

### 5.3 The error type is a design decision

| Error type | Strengths | Weaknesses | Use when |
|---|---|---|---|
| `enum class` | Tiny (fits in a register), trivial, exhaustive `switch` | No message, no context, no payload; every library invents its own | Closed, local error set (a parser, a codec) |
| `std::error_code` | 16 bytes (value + category pointer), standard vocabulary, compares across libraries (`ec == std::errc::no_such_file_or_directory`), interop with `system_error` exceptions | Needs an `error_category` for your own codes; no free-form context | Interop with OS / `<filesystem>` / `<system_error>`; library boundaries |
| `std::string` | Any message | Allocates; not comparable; not machine-readable; expensive to propagate | Prototypes, scripts, user-facing diagnostics at the top level |
| Rich struct `{code, message, context, location}` | Full diagnostics | Larger → `expected` returned via memory not registers | Application code where diagnostics matter more than nanoseconds |
| `std::variant<E1, E2, …>` / a sum of module errors | Exact; exhaustive handling | Verbose; conversions between layers | Cross-module APIs with strongly typed error sets |
| `std::exception_ptr` | Bridges to exceptions; type-erased | Heap; defeats the point | Wrapping legacy throwing code |
| `std::unique_ptr<Error>` / shared | Cheap to move, polymorphic payloads | Allocates; ownership | Deep error hierarchies |

> **Guideline.** For **library boundaries** use `std::error_code` (or an `enum` with an `error_category`). Inside a module use a small `enum class`. Add **context** when translating between layers (`transform_error`), not by growing every error with strings at the point of failure.

### 5.4 `std::error_code` and `error_category`  `[syserr]` (C++11)

An `error_code` is `{int value, const error_category* category}`. A category (a singleton) names the domain (`"generic"`, `"system"`, `"filesystem"`, yours) and turns values into messages. `std::error_condition` is the *portable* equivalence class (`std::errc::*`): `ec == std::errc::permission_denied` compares through categories' `equivalent()`. Enabling `is_error_code_enum<MyEnum>` and providing `make_error_code(MyEnum)` makes `std::error_code ec = MyEnum::foo;` work. Experiment 3 builds one.

### 5.5 What `expected` does not do

- **No propagation operator.** C++ has no Rust `?`. You write `if (!r) return std::unexpected(r.error());` or use `and_then` chains (or, as a **GNU extension**, a statement-expression macro: Experiment 2; portable code shouldn't).
- **Not `[[nodiscard]]`.** In libstdc++ 14 (verified) discarding an `expected` return value compiles **without any warning** at `-Wall -Wextra`. Errors-as-values only protect you if you look at them: mark your own functions `[[nodiscard]]` (or return a `[[nodiscard]] struct`-wrapper).
- **No automatic conversion between error types.** `expected<T, A>` → `expected<U, B>` needs `transform_error`.
- **Constructors can't return it.** Use named factories: `static std::expected<Socket, Error> connect(...)`.
- **It doesn't stop exceptions.** `T`'s constructors, allocation, and the callbacks passed to `and_then` may still throw; decide the policy (`noexcept` + `expected` for I/O-like layers).

### Layer check

| Layer | Decides |
|---|---|
| **Standard** | The interface and semantics of `expected`, `unexpected`, `bad_expected_access`; triviality requirements; the monadic operations; the `error_code` machinery |
| **Compiler / library** | Whether it is `[[nodiscard]]` (libstdc++: no); constexpr-ness (C++23: constexpr); inlining of the monadic lambdas (excellent at `-O2`); whether it is hardened (`_GLIBCXX_ASSERTIONS` checks `operator*` on the wrong alternative) |
| **ABI** | Layout: union of `T` and `E` plus a `bool`, padded; **passing and returning in registers** if the type is "trivial for the purposes of calls" (no non-trivial copy/move constructor or destructor): `expected<int, enum>` returns in `rax` on x86-64 SysV; `expected<std::string, E>` is returned through a hidden pointer (Experiment 4) |
| **CPU** | A predictable branch on each return; for the success path with small types it is a flag test and a conditional move; no table lookups, no unwinding |

---

## 6. Implementation model

### Storage

```cpp
template <class T, class E> class expected {
    union { T val_; E unex_; };      // anonymous union: only one alive
    bool has_val_;                   // the discriminant
};
```

`sizeof(expected<T,E>)` = max(sizeof T, sizeof E) + the bool, rounded up to alignment (Experiment 4 prints the numbers). libstdc++ stores a separate `bool` (padded to the alignment of the union); it does not exploit spare bit patterns (niches) in `T` or `E`.

### Calling convention (🧩 System V x86-64)

A class is returned **in registers** if it is at most 16 bytes and "trivial for the purposes of calls" (its copy/move constructors and destructor are trivial). `expected<int, Err>` (8 bytes) qualifies: the compiler packs the value and the flag into `rax` (Experiment 4 shows `value | (flag << 32)`). `expected<std::string, Err>` has a non-trivial destructor, so the caller passes a hidden pointer to the return slot. This is why **small, trivially-destructible success and error types make `expected` essentially free**, and why a `std::string` error costs a hidden-pointer return plus a destructor on every path.

### Monadic ops compile to branches

`r.and_then(f).transform(g)` on small types inlines into: test the flag; if error, forward; else compute; test again. With `-O2` the lambdas disappear and the chain is the same branches you would write by hand (Experiment 4's `g`).

---

## 7. Experiments

### Experiment 1 ✅: A tour of `expected` and its pipelines

```cpp
// @test run -std=c++23 -O0
#include <charconv>
#include <cstdio>
#include <expected>
#include <string>
#include <string_view>

enum class Err { empty, not_a_number, out_of_range, negative };

constexpr const char* to_string(Err e) {
    switch (e) { case Err::empty: return "empty"; case Err::not_a_number: return "not a number";
                 case Err::out_of_range: return "out of range"; case Err::negative: return "negative"; }
    return "?";
}

std::expected<int, Err> parse_int(std::string_view s) {
    if (s.empty()) return std::unexpected(Err::empty);
    int v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec == std::errc::result_out_of_range) return std::unexpected(Err::out_of_range);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return std::unexpected(Err::not_a_number);
    return v;
}
std::expected<int, Err> non_negative(int v) {
    if (v < 0) return std::unexpected(Err::negative);
    return v;
}

// One pipeline: parse -> validate -> map. The first failure short-circuits everything after it.
std::expected<int, Err> pipeline(std::string_view s) {
    return parse_int(s)
        .and_then(non_negative)                       // can fail (same error type)
        .transform([](int v) { return v * 2; });      // cannot fail
}

int main() {
    for (std::string_view in : {"21", "", "abc", "-5", "99999999999", "12x"}) {
        auto r = pipeline(in);
        if (r) std::printf("%-14s -> value %d\n", std::string(in).c_str(), *r);
        else   std::printf("%-14s -> error: %s\n", std::string(in).c_str(), to_string(r.error()));
    }

    // value_or, or_else (recovery), value() throwing, transform_error (changing the error type)
    std::printf("value_or:        %d\n", pipeline("oops").value_or(-1));
    auto recovered = pipeline("-3").or_else([](Err e) -> std::expected<int, Err> {
        if (e == Err::negative) return 0;               // recover from this specific error
        return std::unexpected(e);                      // pass the others through
    });
    std::printf("or_else recover: %d\n", *recovered);

    auto as_text = pipeline("abc").transform_error([](Err e) { return std::string("parse failed: ") + to_string(e); });
    std::printf("transform_error: %s\n", as_text.error().c_str());

    try { (void)pipeline("").value(); }
    catch (const std::bad_expected_access<Err>& ex) { std::printf("value() on error throws bad_expected_access, error = %s\n", to_string(ex.error())); }

    // expected<void, E>: success without a value
    auto check = [](int x) -> std::expected<void, Err> { if (x < 0) return std::unexpected(Err::negative); return {}; };
    std::printf("expected<void>: check(1) ok=%d, check(-1) ok=%d\n", check(1).has_value(), check(-1).has_value());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
21             -> value 42
               -> error: empty
abc            -> error: not a number
-5             -> error: negative
99999999999    -> error: out of range
12x            -> error: not a number
value_or:        -1
or_else recover: 0
transform_error: parse failed: not a number
value() on error throws bad_expected_access, error = empty
expected<void>: check(1) ok=1, check(-1) ok=0
```

### Experiment 2 🔧: Propagation without the `?` operator (and a GNU-only shortcut)

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <expected>
#include <string>

enum class E { bad_input, too_big };
std::expected<int, E> step1(int x) { if (x < 0) return std::unexpected(E::bad_input); return x + 1; }
std::expected<int, E> step2(int x) { if (x > 100) return std::unexpected(E::too_big); return x * 2; }

// (a) Portable, explicit: early return on the failure track.
std::expected<int, E> manual(int x) {
    auto a = step1(x);
    if (!a) return std::unexpected(a.error());
    auto b = step2(*a);
    if (!b) return std::unexpected(b.error());
    return *b + 1;
}

// (b) Portable, combinators.
std::expected<int, E> chained(int x) {
    return step1(x).and_then(step2).transform([](int v) { return v + 1; });
}

// (c) GNU statement-expression macro: a COMPILER EXTENSION (GCC/Clang), not standard C++. It mimics Rust's `?`.
//     Widely used in LLVM-style and system code, but it makes your code non-portable to MSVC: use judiciously.
#define TRY(expr)                                                       \
    ({ auto _r = (expr); if (!_r) return std::unexpected(_r.error()); std::move(*_r); })

std::expected<int, E> with_try(int x) {
    int a = TRY(step1(x));
    int b = TRY(step2(a));
    return b + 1;
}

int main() {
    for (int in : {5, -1, 200}) {
        auto m = manual(in), c = chained(in), t = with_try(in);
        std::printf("in=%4d  manual=%s  chained=%s  TRY=%s   (all equal: %d)\n", in,
                    m ? std::to_string(*m).c_str() : "error", c ? std::to_string(*c).c_str() : "error", t ? std::to_string(*t).c_str() : "error",
                    m == c && c == t);
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
in=   5  manual=13  chained=13  TRY=13   (all equal: 1)
in=  -1  manual=error  chained=error  TRY=error   (all equal: 1)
in= 200  manual=error  chained=error  TRY=error   (all equal: 1)
```

Pick (a) for clarity in short functions, (b) for linear pipelines, and be aware of (c)'s portability cost. The language-level fix (a `try` operator) is a proposal, **not** in C++26.

### Experiment 3 ✅: `std::error_code` with your own category

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <expected>
#include <string>
#include <system_error>

// 1. The domain's error enum
enum class ConfigError { ok = 0, missing_key, bad_type, out_of_range };

// 2. Its category: names the domain and maps values to messages
class ConfigCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "config"; }
    std::string message(int v) const override {
        switch (static_cast<ConfigError>(v)) {
            case ConfigError::ok: return "ok";
            case ConfigError::missing_key: return "required key is missing";
            case ConfigError::bad_type: return "value has the wrong type";
            case ConfigError::out_of_range: return "value out of range";
        }
        return "unknown config error";
    }
    // Make some of our errors compare equal to portable std::errc conditions.
    bool equivalent(const std::error_code& ec, int cond) const noexcept override { (void)ec; (void)cond; return false; }
};
const std::error_category& config_category() { static const ConfigCategory c; return c; }

// 3. Opt in so ConfigError converts to std::error_code implicitly
namespace std { template <> struct is_error_code_enum<ConfigError> : true_type {}; }
std::error_code make_error_code(ConfigError e) { return {static_cast<int>(e), config_category()}; }

// 4. Use it
std::expected<int, std::error_code> get_port(bool present, bool numeric, int value) {
    if (!present)  return std::unexpected(ConfigError::missing_key);              // implicit conversion to error_code
    if (!numeric)  return std::unexpected(ConfigError::bad_type);
    if (value < 1 || value > 65535) return std::unexpected(ConfigError::out_of_range);
    return value;
}

int main() {
    struct Case { bool p, n; int v; };
    for (Case c : {Case{true, true, 8080}, Case{false, true, 0}, Case{true, false, 0}, Case{true, true, 70000}}) {
        auto r = get_port(c.p, c.n, c.v);
        if (r) std::printf("port = %d\n", *r);
        else   std::printf("error [%s:%d] %s\n", r.error().category().name(), r.error().value(), r.error().message().c_str());
    }

    // Interop: the same type carries OS errors, and compares against portable conditions.
    std::error_code os = std::make_error_code(std::errc::no_such_file_or_directory);
    std::printf("os error: [%s] %s; == errc::no_such_file_or_directory? %d\n", os.category().name(), os.message().c_str(),
                os == std::errc::no_such_file_or_directory);
    std::printf("sizeof(std::error_code) = %zu (int + category pointer)\n", sizeof(std::error_code));

    // Bridge to exceptions at a boundary where throwing is appropriate:
    try { throw std::system_error(make_error_code(ConfigError::missing_key), "while loading config"); }
    catch (const std::system_error& e) { std::printf("as exception: %s (code %d in '%s')\n", e.what(), e.code().value(), e.code().category().name()); }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
port = 8080
error [config:1] required key is missing
error [config:2] value has the wrong type
error [config:3] value out of range
os error: [generic] No such file or directory; == errc::no_such_file_or_directory? 1
sizeof(std::error_code) = 16 (int + category pointer)
as exception: while loading config: required key is missing (code 1 in 'config')
```

`std::error_code` is the **vocabulary type for library errors**: it can hold a POSIX `errno`, a Windows error, an `<filesystem>` failure, or your own domain code, and it compares against the portable `std::errc` enumeration. The 16-byte size matters (§6): `expected<int, std::error_code>` is 24 bytes and is returned through memory, not registers.

### Experiment 4 🧩: Layout, size, and what the compiler does with it

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <variant>

enum class Err : int { a = 1, b };

#define SZ(...) std::printf("  sizeof(%-44s) = %2zu\n", #__VA_ARGS__, sizeof(__VA_ARGS__))

int main() {
    std::puts("sizes:");
    SZ(std::optional<int>);
    SZ(std::expected<int, Err>);
    SZ(std::expected<void, Err>);
    SZ(std::expected<int, std::error_code>);
    SZ(std::expected<std::string, Err>);
    SZ(std::expected<std::string, std::string>);
    SZ(std::expected<std::unique_ptr<int>, Err>);
    SZ(std::variant<int, Err>);

    std::puts("properties:");
    using EI = std::expected<int, Err>;
    std::printf("  trivially destructible       expected<int,Err>      = %d\n", std::is_trivially_destructible_v<EI>);
    std::printf("  trivially copy constructible expected<int,Err>      = %d\n", std::is_trivially_copy_constructible_v<EI>);
    std::printf("  trivially copyable           expected<int,Err>      = %d  (assignment is not trivial in libstdc++ 14)\n", std::is_trivially_copyable_v<EI>);
    std::printf("  trivially destructible       expected<string,Err>   = %d\n", std::is_trivially_destructible_v<std::expected<std::string, Err>>);
    std::printf("  nothrow move constructible   expected<string,Err>   = %d\n", std::is_nothrow_move_constructible_v<std::expected<std::string, Err>>);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sizes:
  sizeof(std::optional<int>                          ) =  8
  sizeof(std::expected<int, Err>                     ) =  8
  sizeof(std::expected<void, Err>                    ) =  8
  sizeof(std::expected<int, std::error_code>         ) = 24
  sizeof(std::expected<std::string, Err>             ) = 40
  sizeof(std::expected<std::string, std::string>     ) = 40
  sizeof(std::expected<std::unique_ptr<int>, Err>    ) = 16
  sizeof(std::variant<int, Err>                      ) =  8
properties:
  trivially destructible       expected<int,Err>      = 1
  trivially copy constructible expected<int,Err>      = 1
  trivially copyable           expected<int,Err>      = 0  (assignment is not trivial in libstdc++ 14)
  trivially destructible       expected<string,Err>   = 0
  nothrow move constructible   expected<string,Err>   = 1
```

The assembly shows how the layout is *used*: with `expected<int,int>` (8 bytes, trivially copy-constructible and destructible), GCC returns the whole object in **one register**.

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fcf-protection=none filter=checked,chained,ret_string
#include <expected>
#include <string>

std::expected<int, int> checked(int x) {
    if (x < 0) return std::unexpected(x);       // error alternative
    return x * 2;                               // value alternative
}
std::expected<int, int> chained(int x) {
    return checked(x).and_then([](int v) -> std::expected<int, int> { return v + 1; }).transform([](int v) { return v * 3; });
}
std::expected<std::string, int> ret_string(int x) {
    if (x < 0) return std::unexpected(x);
    return std::string("ok");
}
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
checked(int):
	mov	eax, edi
	xor	edx, edx
	test	edi, edi
	js	.L3
	lea	eax, [rdi+rdi]
	mov	edx, 1
.L3:
	sal	rdx, 32
	or	rax, rdx
	ret

chained(int):
	xor	eax, eax
	test	edi, edi
	js	.L6
	lea	eax, 1[rdi+rdi]
	lea	edi, [rax+rax*2]
	mov	eax, 1
.L6:
	movzx	edx, al
	mov	eax, edi
	sal	rdx, 32
	or	rax, rdx
	ret

ret_string[abi:cxx11](int):
	mov	rax, rdi
	test	esi, esi
	jns	.L9
	mov	DWORD PTR [rdi], esi
	mov	BYTE PTR 32[rdi], 0
	ret
.L9:
	lea	rdx, 16[rdi]
	mov	BYTE PTR 18[rdi], 0
	mov	QWORD PTR [rdi], rdx
	mov	edx, 27503
	mov	WORD PTR 16[rdi], dx
	mov	QWORD PTR 8[rdi], 2
	mov	BYTE PTR 32[rdi], 1
	ret
```

Read `checked`: there is **one `test`/`js` branch**, the value goes in `eax`, the error flag is shifted into the upper half (`sal rdx, 32; or rax, rdx`), and everything returns in `rax`: no memory traffic. `chained` shows the monadic operations vanishing: GCC inlines both lambdas and `checked` itself into the same few instructions, so the entire `and_then`/`transform` chain costs *one branch*, as the hand-written version would. `ret_string` is different: `expected<std::string, int>` is not trivially destructible, so the caller passes a hidden result pointer and the function stores the discriminant in memory. **Design implication: keep the success and error types small and trivially destructible on hot paths.**

### Experiment 5 ✅/⚠️: Silent discard: `expected` is not `[[nodiscard]]`

```cpp
// @test run -std=c++23 -O0 -Wall -Wextra
#include <cstdio>
#include <expected>

std::expected<int, int> may_fail(int x) { if (x < 0) return std::unexpected(x); return x; }

[[nodiscard]] std::expected<int, int> careful(int x) { return may_fail(x); }

int main() {
    may_fail(-1);       // compiles with NO warning on libstdc++ 14: the failure vanishes
    // careful(-1);     // would warn: ignoring return value of ... declared with attribute 'nodiscard'
    std::puts("may_fail(-1) discarded silently; mark your own factory functions [[nodiscard]]");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
may_fail(-1) discarded silently; mark your own factory functions [[nodiscard]]
```

*Compare with exceptions:* a failure you forgot to handle still **stops the program** (it propagates to `terminate`); a forgotten `expected` quietly continues in a wrong state. The type forces you to *look* only when you want the value. Mitigations: `[[nodiscard]]` on your functions, an in-house `[[nodiscard]] Result<T,E>` alias or wrapper, and `-Werror=unused-result`.

### Experiment 6 🔧: The failure-rate crossover: exceptions vs `expected` vs error code

The same three-step parsing pipeline written with each mechanism, run over inputs with a controlled failure probability. This is the measurement behind the rule "exceptions for rare failures, values for common ones".

```cpp
// @test run -std=c++23 -O2
#include <chrono>
#include <cstdio>
#include <expected>
#include <random>
#include <stdexcept>
#include <vector>

// Three small steps; a negative input fails at step 1, a multiple of 1000 fails at step 3.
struct BadInput : std::runtime_error { BadInput() : std::runtime_error("bad") {} };

// --- exceptions ---
[[gnu::noinline]] int s1_x(int v) { if (v < 0) throw BadInput(); return v + 1; }
[[gnu::noinline]] int s2_x(int v) { return s1_x(v) * 2; }
[[gnu::noinline]] int s3_x(int v) { return s2_x(v) + 3; }
// --- expected ---
using R = std::expected<int, int>;
[[gnu::noinline]] R s1_e(int v) { if (v < 0) return std::unexpected(v); return v + 1; }
[[gnu::noinline]] R s2_e(int v) { return s1_e(v).transform([](int x) { return x * 2; }); }
[[gnu::noinline]] R s3_e(int v) { return s2_e(v).transform([](int x) { return x + 3; }); }
// --- error codes with out-parameters ---
[[gnu::noinline]] int s1_c(int v, int& out) { if (v < 0) return -1; out = v + 1; return 0; }
[[gnu::noinline]] int s2_c(int v, int& out) { int t; if (int rc = s1_c(v, t)) return rc; out = t * 2; return 0; }
[[gnu::noinline]] int s3_c(int v, int& out) { int t; if (int rc = s2_c(v, t)) return rc; out = t + 3; return 0; }

template <class F> double ns_per(const std::vector<int>& in, F&& f, long& sink) {
    auto t0 = std::chrono::steady_clock::now();
    for (int v : in) sink += f(v);
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / in.size();
}

int main() {
    constexpr int N = 200'000;
    std::mt19937 rng(11);
    long sink = 0;
    std::printf("ns per call (lower is better), N=%d, 3-step pipeline\n", N);
    std::printf("  %-12s %12s %12s %12s\n", "failure rate", "exceptions", "expected", "error code");
    for (double p : {0.0, 0.001, 0.01, 0.1, 0.5}) {
        std::vector<int> in(N);
        for (auto& v : in) v = (std::uniform_real_distribution<>(0, 1)(rng) < p) ? -1 : 5;
        double tx = ns_per(in, [](int v) { try { return s3_x(v); } catch (const BadInput&) { return -1; } }, sink);
        double te = ns_per(in, [](int v) { return s3_e(v).value_or(-1); }, sink);
        double tc = ns_per(in, [](int v) { int o; return s3_c(v, o) ? -1 : o; }, sink);
        std::printf("  %-12.3f %12.1f %12.1f %12.1f\n", p, tx, te, tc);
    }
    return sink == 42 ? 1 : 0;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
ns per call (lower is better), N=200000, 3-step pipeline
  failure rate   exceptions     expected   error code
  0.000                 4.5          4.1          5.8
  0.001                 6.8          4.0          6.2
  0.010                27.7          4.1          5.9
  0.100               230.9          5.1          7.5
  0.500              1210.3          7.8         10.3
```

Read the table directly (measured on this machine, GCC 14.2): at **0 %** failures the three mechanisms are within about a nanosecond of each other (4.5 / 4.1 / 5.8 ns), because the exception path costs nothing until a throw happens, and here the inlined `expected` chain is just as cheap. From there the exception column grows linearly with the failure rate: 230 ns/call at 10 % and 1210 ns/call at 50 %, i.e. **≈ 2.3-2.4 µs per failure** for a three-frame stack. `expected` and the error code stay in the single-digit nanoseconds. So in this benchmark there is no failure rate at which exceptions beat `expected`; the happy-path penalty of `expected` (a register-sized return plus a branch) is below the noise. The trade-off appears elsewhere: with **large** `T`/`E` (memory returns, destructors, `std::string` errors) the per-call overhead of `expected` is real, and then exceptions win for genuinely rare failures. Measure your own types and call depths: deeper stacks make each throw more expensive (≈ 1 µs per frame, Chapter 22), which moves the balance further toward `expected`.

---

## 8. Assembly / runtime investigation

Experiment 4's assembly is the core. Additional checks:

```bash
# (1) Is the success path really branch-light?  Compare   expected<int,int>   vs plain int  for a hot function
g++-14 -std=c++23 -O2 -S -masm=intel -o - prog.cpp | c++filt | awk '/^hot_function/,/ret/'

# (2) Which functions return via memory (hidden pointer) instead of registers?  Look for 'rdi' stores in the prologue
#     and the absence of values in rax/rdx at ret.   expected<std::string,E> is the usual culprit.

# (3) Does -fno-exceptions + expected give a smaller binary?  (Chapter 22's size table)
g++-14 -std=c++23 -O2 -fno-exceptions -c prog.cpp -o noexc.o && size -A noexc.o | grep -E "text|eh_frame"

# (4) Hardened-library check: does *r on an error abort?
g++-14 -std=c++23 -O0 -D_GLIBCXX_ASSERTIONS prog.cpp && ./a.out    # 'expected::operator*(): has_value()' assertion
```

---

## 9. Implementation exercise

Implement your own `Expected<T, E>` from scratch:

1. Storage: an anonymous union plus a `bool`; constructors from `T`, from `Unexpected<E>`; correct copy/move/destroy of the active member (placement `new`, explicit destructor call); `noexcept` propagation.
2. `has_value`, `operator*`, `operator->`, `value()`, `error()`, `value_or`, comparison.
3. The monadic operations with correct value-category forwarding (`&`, `const&`, `&&`, `const&&` overloads).
4. A `static_assert` suite showing triviality is preserved when `T` and `E` are trivial.
5. `Expected<void, E>` as a specialization.
6. Then re-implement Experiment 1's pipeline with it and compare assembly with `std::expected`.

<details>
<summary><strong>Solution sketch: storage and copy</strong></summary>

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

template <class E> struct Unexpected { E error; };
template <class E> Unexpected(E) -> Unexpected<E>;

template <class T, class E>
class Expected {
    union { T val_; E err_; };
    bool has_;
public:
    Expected(T v) noexcept(std::is_nothrow_move_constructible_v<T>) : val_(std::move(v)), has_(true) {}
    Expected(Unexpected<E> u) noexcept(std::is_nothrow_move_constructible_v<E>) : err_(std::move(u.error)), has_(false) {}

    Expected(const Expected& o) : has_(o.has_) {
        if (has_) ::new (&val_) T(o.val_); else ::new (&err_) E(o.err_);
    }
    Expected(Expected&& o) noexcept(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E>) : has_(o.has_) {
        if (has_) ::new (&val_) T(std::move(o.val_)); else ::new (&err_) E(std::move(o.err_));
    }
    Expected& operator=(Expected o) noexcept(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E>) {
        this->~Expected();                             // destroy the active member
        ::new (this) Expected(std::move(o));           // re-construct in place from the temporary
        return *this;
    }
    ~Expected() { if (has_) val_.~T(); else err_.~E(); }

    bool has_value() const noexcept { return has_; }
    explicit operator bool() const noexcept { return has_; }
    T& operator*() noexcept { return val_; }
    const T& operator*() const noexcept { return val_; }
    const E& error() const noexcept { return err_; }

    template <class F> auto and_then(F&& f) & -> decltype(f(val_)) {
        using R = decltype(f(val_));
        if (has_) return f(val_);
        return R(Unexpected<E>{err_});
    }
    template <class F> auto transform(F&& f) & -> Expected<decltype(f(val_)), E> {
        using R = Expected<decltype(f(val_)), E>;
        if (has_) return R(f(val_));
        return R(Unexpected<E>{err_});
    }
};

int main() {
    Expected<std::string, int> a(std::string("hello"));
    Expected<std::string, int> b = Unexpected<int>{404};
    auto c = a;                                               // copy of the value alternative
    auto len = a.transform([](std::string& s) { return s.size(); });
    auto lb = b.transform([](std::string& s) { return s.size(); });
    std::printf("a=%s copy=%s len=%zu ; b has_value=%d error=%d ; transform on error keeps error=%d\n",
                (*a).c_str(), (*c).c_str(), *len, b.has_value(), b.error(), lb.error());
    static_assert(std::is_trivially_destructible_v<Expected<int, int>> == false, "this simple version isn't trivially destructible: how does std::expected achieve it?");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
a=hello copy=hello len=5 ; b has_value=0 error=404 ; transform on error keeps error=404
```

The final `static_assert` is the lesson: a user-written destructor makes the type non-trivial for calls and forces memory returns. `std::expected` provides a *conditionally trivial* destructor (a defaulted one when `T` and `E` are trivially destructible, a user-provided one otherwise), via constrained special members (C++20 `requires`-overloaded destructors, Chapter 10) or, in libstdc++, a base-class split. Re-implement that and watch the assembly of your `checked()` change from memory returns to a register return.

</details>

---

## 10. Real-world example

| Where | Errors-as-values in practice |
|---|---|
| **LLVM** | `llvm::Expected<T>`, `llvm::Error`, `ErrorOr<T>`: checked results with *runtime enforcement* that errors are inspected (it aborts in debug builds if you drop one) |
| **Abseil / Google** | `absl::Status`, `absl::StatusOr<T>` across all APIs (exceptions banned) |
| **Chromium, Firefox** | `base::expected`, `Result<T, E>` (Mozilla's `mozilla::Result`) |
| **Boost.Outcome** | `result<T, E>` / `outcome<T, E, P>` with exception interop; used in high-performance finance code |
| **Rust** | `Result<T, E>` plus the `?` operator: the design `expected` was modelled on, and the benchmark for ergonomics |
| **Linux kernel** | `ERR_PTR` / negative `errno` returns: errors-as-values at the C level |
| **Qt** | `QFileDevice::error()`, `QJsonParseError`, `QIODevice` return codes / `bool`: pre-`expected` code-style; Qt 6 code increasingly uses `std::optional` and `QStringView`; `QtConcurrent`/`QFuture` use exceptions via `QException` (Chapter 48) |
| **`<filesystem>`, `<charconv>`, `<system_error>`** | Overload pairs: a throwing version and a `std::error_code&` version; `from_chars` returns `{ptr, errc}`: a hand-rolled `expected` |
| **Embedded / game engines** | `-fno-exceptions` with `expected`-style returns throughout |

> **Opinion.** Use `std::expected<T, E>` for **recoverable errors that happen as part of normal operation** (parsing, validation, I/O that routinely fails, protocol decoding), for libraries that must work without exceptions, and at layers where you want the caller *forced to think* about failure. Keep exceptions for **truly exceptional** conditions (`bad_alloc`, broken invariants), for constructors, and for deep call chains where handling happens far away. The most useful split in practice: **a layered design**: low-level and I/O layers return `expected<T, std::error_code>` (cheap, uniform, library-boundary friendly); the application layer converts at a few well-chosen points with `value()` (throwing) or by translating into domain exceptions where unwinding to a top-level handler is the right behaviour. Make the error type small and trivially destructible on hot paths; make all your result-returning functions `[[nodiscard]]` yourself; avoid `std::string` as an error type on hot paths; and do not build ad-hoc `variant<T, std::string>` or `pair<T, bool>` when `expected` exists. Finally: **don't be dogmatic**. `expected` makes error handling *visible*, which is a benefit and also a source of repetition; if a function can fail in a dozen ways that every caller will merely propagate, an exception is the honest tool.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Discarding the result | Failure silently ignored (no warning in libstdc++ 14) | `[[nodiscard]]` on your functions; `-Werror=unused-result` |
| `*r` / `r.error()` on the wrong alternative | **UB** (garbage or crash) | Test first, or use `value()`; build with `-D_GLIBCXX_ASSERTIONS` in debug/CI |
| `expected<T, E>` where `T` and `E` are convertible from each other | Ambiguous construction, wrong alternative chosen | Always wrap errors in `std::unexpected(...)` |
| `std::string` as the error type everywhere | Allocation on every failure, hidden-pointer returns, no machine-readable code | `enum class` or `error_code` internally; strings only at the edges |
| Different error types per layer without conversion | Compile errors (`and_then` requires the same `E`) or ad-hoc conversions | `transform_error` at layer boundaries; a common error type per module |
| Treating `value()` as the "default" accessor | Exceptions reintroduced unexpectedly; `bad_expected_access` where you promised `noexcept` | Use `value()` deliberately at the boundary where throwing is wanted |
| Large `T`/`E` returned by value on hot paths | Memory returns and copies | Return in place, or `expected<std::reference_wrapper<T>, E>`/pointer |
| `and_then` callback returning a plain `T` | Compile error (it must return an `expected`) | Use `transform` for non-failing steps |
| Callbacks that throw | Exceptions still escape from a supposedly exception-free pipeline | `noexcept` lambdas; policy decision |
| Using `expected` for programmer errors (violated preconditions) | Callers write error handling for bugs | Assertions / contracts / `terminate`; reserve `expected` for *environmental* failures |
| Losing context while propagating | "invalid argument" with no idea where | Wrap with context in `transform_error` at layer crossings; `std::source_location` in rich error types |
| Mixing both styles carelessly in the same API | Callers can't know which failures are thrown vs returned | Document and keep a single rule per layer (e.g. "I/O returns expected; invariants throw") |
| Assuming `expected<T,E>` is trivially copyable | Surprises in `memcpy`/ABI assumptions | Check `std::is_trivially_*` per operation (Experiment 4) |

---

## 12. Exercises

1. **Port.** Take an exception-based parsing function from your code (or the `Config` parser from Project 1) and port it to `expected<T, std::error_code>`. Count lines, branches and measure failure and success timings.
2. **Three error types.** For the same API implement `enum class` errors, `std::error_code` with a category, and a rich struct `{code, message, location}`. Compare `sizeof`, assembly of the returning function (register vs memory), and ergonomics at call sites.
3. **Conditional triviality.** Implement the `Expected` of §9 with a conditionally trivial destructor and confirm with the assembly that `checked()` returns in registers.
4. **Crossover on your workload.** Rerun Experiment 6 with (a) deeper call chains (10, 50 frames), (b) an error type of `std::string`, (c) `-O3` and `-O0`. Plot the crossover failure rate.
5. **A `TRY` that is portable.** Build a `TRY` helper for MSVC-compatible code using `if (auto&& r = (e); !r) return unexpected(...)` and compare the readability with the GNU statement-expression macro and with `and_then`.
6. **Bridge.** Write `unwrap(expected<T,E>) -> T` (throwing a domain exception carrying `E`) and `catch_to_expected(F)` (invoking a throwing function and capturing known exception types as `E`). Use them to wrap a legacy throwing library behind an `expected` API.
7. **Error-code categories.** Write a category for HTTP status codes; make `ec == std::errc::...` and `ec == HttpClass::client_error` conditions work through `equivalent()` and `default_error_condition()`.
8. **Compare** `optional`, `expected`, `variant<T, E>`, `pair<T, error_code>` and exceptions for a function `find_user(id)`: produce a table of: signature clarity, ability to ignore, cost of miss, cost of hit, composability, and what a reviewer would say.

---

## 13. Challenge: a layered error architecture

Design the error handling of a small file-server library with three layers:

- a **transport** layer (sockets/files) returning `expected<T, std::error_code>` with `errno`-style codes and no exceptions
- a **protocol** layer (parsing requests) returning `expected<T, ProtocolError>` where `ProtocolError` is an `enum` plus a source location, converting transport errors with `transform_error` (keeping the cause chain, bounded in size)
- an **application** layer that uses exceptions *at one place only*: a top-level handler that maps any `ProtocolError`/`error_code` to an HTTP response
- show the **same operation** compiled twice: with `-fno-exceptions` (all layers value-based, failures at the top mapped by a visitor) and with exceptions allowed (the application layer calling `.value()`)
- tests: fault injection at every failure point (Chapter 22's harness adapted to values); a benchmark of success and failure paths; a document listing the policy: which failures are values, which are exceptions, and why

---

## 14. Knowledge check

1. What problem does `std::expected` solve that `std::optional` does not?
2. Why does `std::unexpected` exist?
3. What happens when you call `*r` on an `expected` that holds an error? `r.value()`?
4. What do `and_then`, `transform`, `or_else` and `transform_error` each do? Which requires the same error type?
5. Explain why `expected<int, Err>` can be returned in a single register but `expected<std::string, Err>` cannot.
6. Why does a discarded `expected` not warn in libstdc++, and what should you do about it?
7. What did the failure-rate sweep show about exceptions vs `expected`, and what could change the picture?
8. When is `std::error_code` a better error type than an `enum class`? When worse?
9. Why can't constructors return `expected`, and what is the workaround?
10. What is the language-level feature `expected` lacks compared with Rust's `Result`?
11. Name two situations where exceptions remain the better choice despite `expected`.
12. How would you layer exceptions and `expected` in one application?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. `optional` records only the absence of a value; `expected` records **why** it is absent (the error object).
2. To disambiguate constructing the error alternative from the value alternative, especially when `T` and `E` are mutually convertible (`expected<std::string, std::string>`).
3. `*r` on an error is undefined behaviour (the library may assert in hardened mode). `r.value()` throws `std::bad_expected_access<E>` containing the error.
4. `and_then`: if value, call `f(value)` returning an `expected` (chain a fallible step); `transform`: if value, map it with a function returning a plain `U` and re-wrap; `or_else`: if error, call `f(error)` to recover or re-fail; `transform_error`: map the error to another type. `and_then` and `or_else` (for the value side) require compatible `expected` types (same `E` for `and_then`); `transform_error` changes `E`.
5. In the SysV ABI, a class is returned in registers if ≤ 16 bytes and trivial for the purposes of calls (trivial copy/move constructors and destructor). `expected<int,Err>` is; `expected<string,Err>` has a non-trivial destructor and is returned via a hidden pointer.
6. `std::expected` is not declared `[[nodiscard]]` (in libstdc++ 14), so dropping the return value compiles silently. Mark your functions `[[nodiscard]]` (or return a `[[nodiscard]]` wrapper) and use `-Werror=unused-result`.
7. A throw costs ≈ 2-3 µs (plus ≈ 1 µs per extra frame), so exception cost grows linearly with the failure rate while `expected` stays flat at a few ns; at 0 % failures they are equal for small types. Heavy `T`/`E` types (memory returns, `std::string` errors) add real per-call overhead to `expected` and can make exceptions cheaper when failures are truly rare.
8. Better at library boundaries, for interop with OS/`<filesystem>` errors and for portable comparison against `std::errc`; worse when you want a register-sized trivial type (it's 16 bytes) or a closed set with exhaustive `switch` and no category machinery.
9. A constructor has no return value; the only failure signal is an exception. The workaround is a named factory (static function) returning `expected<T, E>`, with a private constructor.
10. A built-in propagation operator (`?`/`try`) that returns early on the failure track; C++ needs manual `if`/`return`, `and_then` chains or non-portable macros.
11. Failures from constructors and operators; truly exceptional/rare conditions (`bad_alloc`, invariant violations); deep stacks where intermediate layers have nothing to do with the error (propagation by hand would be pure noise).
12. Low-level layers return `expected<T, error_code>` (no exceptions, uniform and cheap); upper layers convert at a few chosen boundaries (`value()` or `unwrap`) so that a top-level handler can handle the failure; constructors and invariant violations throw.

</details>

---

[← Previous: Chapter 22](22-exceptions.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 24 — Dynamic memory →](../part-10-memory/24-dynamic-memory.md)
