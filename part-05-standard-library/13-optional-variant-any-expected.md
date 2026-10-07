# Chapter 13 — `optional`, `variant`, `any`, `expected`

> **Part V · The modern standard library** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 6 hours**
> **Prerequisites:** [Chapter 2](../part-02-object-model-and-lifetime/02-object-model.md), [Chapter 6](../part-03-value-categories-and-move/06-move-semantics.md), [Chapter 10](../part-04-generic-programming/10-concepts.md) &nbsp;|&nbsp; **Standards:** C++17 (`optional`, `variant`, `any`), C++23 (`expected`, monadic `optional`) &nbsp;|&nbsp; **Tools:** `g++-14`, asm, `-fno-exceptions`

[← Previous: Chapter 12](12-views-and-non-owning-types.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 14 — Ranges →](../part-06-ranges/14-ranges.md)

---

**In one sentence:** the four **vocabulary sum types** (`optional`: *a value or nothing*, `variant`: *one of a fixed set of types*, `any`: *one of any type, checked at run time*, `expected`: *a value or an error*) move *"what can this be?"* from comments and conventions into the type system.

| Type | Says | Storage | Allocates? | Standard |
|---|---|---|---|---|
| `optional<T>` | "a `T`, or nothing" | `T` + `bool` (inline) | never | C++17 |
| `variant<Ts...>` | "exactly one of these types, known at compile time" | union of `Ts` + index (inline) | never | C++17 |
| `any` | "one value of *some* copyable type, discovered at run time" | small buffer **or** heap pointer + manager function | **sometimes** | C++17 |
| `expected<T, E>` | "a `T`, or an `E` explaining why not" | union of `T`/`E` + `bool` (inline) | never | **C++23** |

**By the end of this chapter you can:**

- describe each type's layout and say what its `sizeof` will be
- use them idiomatically (`visit`, monadic operations, `get_if`, `value_or`) and name each one's characteristic failure mode
- implement a `variant` (tagged union + visit by function table) and explain how `any` erases its type
- decide among `optional`, `expected`, exceptions and error codes with measured costs, not fashion

---

## 1. Problem

Every function returns *something that sometimes isn't there*, or *one of several things*. The pre-modern encodings:

| Situation | The old encoding | What goes wrong |
|---|---|---|
| "No result" | `-1`, `nullptr`, `INT_MAX`, `end()` | Every caller must remember the sentinel; arithmetic on it compiles |
| "Result or error" | `int f(T* out)` returning a status; `errno`; or an exception | Out-parameters; `errno` is global, **silently ignorable** |
| "One of several shapes" | `union` + a tag you maintain by hand; or a base class + `dynamic_cast` | Reading the wrong member is UB; the tag and the data can disagree; virtual hierarchies force heap allocation and openness |
| "Whatever the caller gives me" | `void*` | No type safety, no ownership, no destruction |

All four share one flaw: **the type does not tell you what states the value can be in**, so correct handling depends on discipline.

---

## 2. Historical context

| Year | Event |
|---|---|
| 1990s | `boost::variant` (Eric Friedman), `boost::any` (Kevlin Henney), `boost::optional` (Fernando Cacciola): hand-built, header-only, widely used |
| 2000s | Haskell's `Maybe`/`Either`, ML's `option`/`result`, Rust's `Option`/`Result` (2010+) show **algebraic data types** as a mainstream design |
| 2017 | `optional`, `variant`, `any` adopted from Boost into C++17. Controversy over `optional<T&>` (excluded) and `variant`'s *valueless-by-exception* state |
| 2018–2022 | `expected` (Vicente Botet, P0323; based on Alexandrescu's talk); `std::visit` return-type rules fixed (P2162); `std::variant` inheritance fixes |
| 2023 | **`std::expected`** (C++23) and **monadic operations** on `optional` (`and_then`, `transform`, `or_else`) (P0798) |
| 2025 | `std::optional<T&>` (P2988, adopted for C++26), `std::variant` pattern-matching proposals (*not* adopted), `std::optional` range support (`begin()/end()`, C++26) |

The shape of the progression: C++ took the ML/Haskell idea of *sum types* but kept two C++ constraints that make them different from the functional-language versions: **value semantics with in-place storage** (no mandatory heap), and **the destructor must be correct in every state**.

---

## 3. Modern solution

```cpp
std::optional<int>                  find_id(std::string_view name);
std::variant<int, double, std::string> v = 3.5;
std::any                            a = std::string("hello");
std::expected<Config, ParseError>   load(std::filesystem::path);

if (auto id = find_id("ada"))            use(*id);                          // optional: test, then access
std::visit(overloaded{ [](int){}, [](double){}, [](const std::string&){} }, v);   // variant: exhaustive dispatch
if (auto* s = std::any_cast<std::string>(&a)) use(*s);                      // any: checked cast
auto cfg = load(p).and_then(validate).transform(normalize);                 // expected: monadic chain
```

---

## 4. Mental model

### Each is a **tagged union** with a different tag

```text
 optional<T>        [ tag: has_value ]  [ T  (raw, uninitialized storage when empty) ]
                        bool                 ◄── sizeof(T), alignof(T) ──►

 variant<A,B,C>     [ tag: index 0|1|2 ]  [ union { A; B; C; }   sized & aligned for the LARGEST ]
                         1 byte                ◄── max(sizeof) ──►

 expected<T,E>      [ tag: has_value ]  [ union { T; E; } ]

 any                [ manager* ]  [ buffer: small object inline │ OR │ pointer to heap object ]
                       │
                       └─ a function pointer to a per-type routine:  copy / move / destroy / type-info / cast
                          (the TYPE is erased; the *behaviour* is kept in the pointer)
```

The union **member is constructed and destroyed manually** (Chapter 3's placement-new and explicit destruction), and the wrapper's special members dispatch on the tag. That is all four types are, mechanically.

### Closed vs open sets

| | Alternatives known at compile time | Alternatives open at run time |
|---|---|---|
| **No heap, value semantics** | **`variant`** ✅ | `any` (small ones) |
| **Heap, polymorphic** | `unique_ptr<Base>` with a closed hierarchy | `unique_ptr<Base>` / `any` / type erasure |

Choose `variant` when the set of alternatives is **closed and you control it** (the AST node kinds, the message types of one protocol, a JSON value). Choose a **virtual hierarchy** or **type erasure** (Chapter 21) when others must be able to *add* alternatives. `any` is a last resort, used when the types are truly unknown at the point of storage (a property bag, a plugin boundary).

### The expression problem, in one sentence

> `variant` + `visit` makes it easy to add a **new operation** over a fixed set of types (write another visitor) and hard to add a **new type** (edit every visitor). Virtual functions make the reverse trade. Neither is "more modern"; choose by which axis will change.

---

## 5. Language rules

### 5.1 `std::optional<T>`  `[optional]`

| Rule | Detail |
|---|---|
| Storage | Inline: `T` (in a union or equivalent) + a `bool`. `sizeof(optional<T>)` is `sizeof(T)` rounded up with the flag: usually `2×alignof(T)`-ish |
| `T` requirements | Not a reference (until C++26), not an array, not `in_place_t`/`nullopt_t`; must be destructible |
| Access | `*o` / `o->` : **UB if empty** (no check). `o.value()` throws `bad_optional_access`. `o.value_or(x)`: copies `x` |
| Construction | `optional<T>{}` / `nullopt` empty; `in_place` to construct without a copy: `optional<std::mutex>{std::in_place}` |
| Comparison | Empty is *less than* every value; `optional<int>{} == nullopt` is true |
| Moving | **Moving from an `optional` leaves it engaged** (contains a moved-from `T`): Chapter 6 |
| `noexcept` | Move is `noexcept` iff `T`'s is; **trivially copyable iff `T` is** (C++20 conditional triviality, Chapter 10 Exp 6) |
| **Monadic** (C++23) | `and_then(f)` (`f: T → optional<U>`), `transform(f)` (`f: T → U`), `or_else(f)` (`f: () → optional<T>`) |

`optional` is **not** a pointer: it has *value semantics* (copy copies the contained `T`; comparison compares values; `const optional<T>` makes the contained value `const`). Using `optional<T>` instead of `T*` to mean "maybe a borrowed object" is a **category error**: for borrowing, use `T*` (or C++26 `optional<T&>`).

### 5.2 `std::variant<Ts...>`  `[variant]`

| Rule | Detail |
|---|---|
| Storage | A union of the `Ts` + an index (`unsigned char` if < 256 alternatives) |
| Default constructor | Constructs the **first** alternative: it must be default-constructible, or use `std::monostate` first |
| Access | `std::get<I>(v)` / `std::get<T>(v)` (throws `bad_variant_access`); `std::get_if<T>(&v)` (returns pointer or null); `std::holds_alternative<T>(v)`; `v.index()` |
| **Visit** | `std::visit(visitor, v...)`: calls `visitor` with the active alternatives; **all alternatives must be handled**; **all calls must return the same type** (C++20: or be convertible to a common one) |
| Converting constructor | Picks the alternative by *overload resolution* on the argument: `variant<int, double> v = 3` → `int`; `variant<bool, std::string> v = "x"` → **`bool`** (a famous trap, fixed by P0608 in C++20 for narrowing and `bool`) |
| Duplicate types | `variant<int, int>` is allowed, but access only by index |
| **`valueless_by_exception`** | If constructing the *new* alternative throws during assignment/`emplace`, the old one is already destroyed: the variant has **no value** (`index() == variant_npos`). `visit` on it throws `bad_variant_access` |
| `noexcept` | Move is `noexcept` iff **all** alternatives' are |

**Why valueless exists.** `v = std::string(...)` must destroy the old alternative *then* construct the new one in the same storage. If construction throws, there is nothing to roll back into (the old value is gone, and the new one never existed). The library avoids this when it can (if the new alternative is `nothrow` constructible from the argument, or the alternative is trivially moveable it builds a temporary first), but it cannot in general. It is the price of in-place storage with no heap. (`std::expected` and `optional` do not have this problem: they have two states, not N.)

### 5.3 `std::any`  `[any]`

| Rule | Detail |
|---|---|
| Storage | **Small-buffer optimization**: if `T` is *nothrow-move-constructible* and fits the buffer (libstdc++: `sizeof(void*)` bytes; libc++: 3 pointers), stored inline; else **heap**. Which types are inline is **implementation-defined** |
| Contained type | Must be **copy-constructible** (the *type-erased copy* must exist). A `unique_ptr` cannot go into an `any`. (C++23 `std::move_only_function` is the move-only analogue; there is no standard move-only `any`.) |
| Access | `std::any_cast<T>(a)` (by value; throws `bad_any_cast`), `any_cast<T>(&a)` (pointer; null on mismatch), `a.type()` (`typeid`), `a.has_value()`, `a.reset()`, `a.emplace<T>(…)` |
| Type check | The check compares **`typeid`** (or the address of a per-type function), exact type match: `any_cast<long>` on an `int` fails, and `any_cast<const int>` does not match `int` |
| RTTI | Required in principle (`typeid`); libstdc++ implements the check without RTTI if `-fno-rtti` by comparing manager addresses (non-portable across shared-library boundaries: duplicates of the manager function break equality: see Chapter 36) |

### 5.4 `std::expected<T, E>`  `[expected]` (C++23)

| Rule | Detail |
|---|---|
| Storage | A union of `T` and `E` + `bool` |
| Access | `*e` / `e->` (UB if error); `e.value()` (throws `bad_expected_access<E>` carrying the error); `e.error()` (UB if value); `e.value_or(x)`; `has_value()` / `operator bool` |
| Construct error | `std::unexpected(err)` (a wrapper type): `return std::unexpected(Errc::NotFound);` |
| Monadic | `and_then`, `transform`, `or_else`, `transform_error` |
| `void` success | `expected<void, E>` is valid: "succeeded or failed with E" |
| Comparison | Compares values, then errors. **No** implicit conversion between `expected<T, E1>` and `expected<T, E2>` (you convert errors explicitly with `transform_error`) |
| Compiler support | **GCC 12+** (`<expected>`), Clang 16+ with libc++ 16 (`-fexperimental-library` earlier), MSVC 19.33. Available here |

### 5.5 Special-member propagation

All four types **propagate triviality and `noexcept`** from their contents using the constrained-special-member technique of Chapter 10: `variant<int, double>` is trivially copyable, `variant<int, std::string>` is not. This decides whether the type is passed in registers (Chapter 37) and whether `memcpy` relocation is legal (Chapter 9).

### Layer check

| Layer | Decides |
|---|---|
| **Standard** | Interface, exception behaviour, propagation of triviality/`noexcept`, valueless state, comparison semantics, `visit` rules |
| **Library implementation** | Layout; `any`'s small-buffer size and manager strategy; whether `optional<T>` uses a niche (no: *no standard library does* for general `T`; it spends a byte plus padding) |
| **ABI** | `optional<int>` is trivially copyable ⇒ passed in a register pair on x86-64 SysV, `optional<std::string>` by hidden pointer; **a library that changes the layout breaks the ABI of every public signature mentioning these types** |
| **CPU** | `visit` compiles to a **jump table** indexed by the tag, one indirect jump per dispatch |

---

## 6. Implementation model

### Optional, variant, expected: *tagged union + manual lifetime*

```cpp
template <class T>
class optional {
    union { T value_; };              // anonymous union: no constructor/destructor run automatically
    bool engaged_ = false;
public:
    ~optional() requires std::is_trivially_destructible_v<T> = default;
    ~optional() { if (engaged_) value_.~T(); }        // manual destruction
    template <class... A> T& emplace(A&&... a) {
        reset();
        ::new (&value_) T(std::forward<A>(a)...);     // manual construction
        engaged_ = true; return value_;
    }
    void reset() { if (engaged_) { value_.~T(); engaged_ = false; } }
};
```

That's the whole idea (Chapter 2's `MiniOptional`, Chapter 10's constrained destructors). For `variant`, the manual part is a **function table**:

```cpp
template <class... Ts> struct variant {
    alignas(Ts...) unsigned char storage_[std::max({sizeof(Ts)...})];
    std::size_t index_;
    void destroy() {                                                   // dispatch on the tag at run time
        static constexpr void (*table[])(void*) = { +[](void* p) { static_cast<Ts*>(p)->~Ts(); }... };
        table[index_](storage_);
    }
};
```

`std::visit` builds an *N-dimensional* version of that table (for *k* variants of sizes n₁…nₖ it generates n₁×…×nₖ entries): **visiting four 10-alternative variants generates 10⁴ function pointers**. That is a *compile-time and code-size* trap (Failure modes).

### `any`: the manager-function pattern

```cpp
class any {
    enum class Op { Copy, Move, Destroy, Type };
    using Manager = void (*)(Op, any&, any*);                 // ONE function pointer per stored type
    Manager mgr_ = nullptr;
    union { void* heap_; alignas(void*) unsigned char buf_[sizeof(void*)]; };
    // for T: `mgr_` points at an instantiation of  template<class T> void manage(Op, any&, any*)
};
```

The **type is erased, and its behaviours (copy, move, destroy, `typeid`) are captured in one function pointer** generated from the template at construction time. This is the same mechanism as `std::function`, `std::shared_ptr`'s deleter and every type-erased wrapper: Chapter 21 builds it fully.

---

## 7. Experiments

### Experiment 1: `sizeof` ledger

```cpp
// @test run -std=c++23 -O0
#include <any>
#include <cstdio>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#define SZ(...) std::printf("  %-48s sizeof=%-3zu alignof=%zu\n", #__VA_ARGS__, sizeof(__VA_ARGS__), alignof(__VA_ARGS__))

int main() {
    std::puts("optional: T + flag, rounded up to alignment");
    SZ(std::optional<char>);
    SZ(std::optional<int>);
    SZ(std::optional<double>);
    SZ(std::optional<std::string>);                 // no niche: 32 + 8
    SZ(std::optional<std::unique_ptr<int>>);        // a null-pointer niche exists, but the standard library does not use it
    SZ(std::optional<bool>);

    std::puts("variant: max(alternatives) + index, rounded up");
    SZ(std::variant<char, char>);
    SZ(std::variant<int, float>);
    SZ(std::variant<int, double>);
    SZ(std::variant<std::monostate, int>);
    SZ(std::variant<int, std::string>);
    SZ(std::variant<std::string, std::vector<int>, double>);

    std::puts("expected: max(T, E) + flag");
    SZ(std::expected<int, int>);
    SZ(std::expected<int, std::error_code>);
    SZ(std::expected<std::string, std::error_code>);
    SZ(std::expected<void, std::error_code>);

    std::puts("any: a manager pointer + a small buffer");
    SZ(std::any);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
optional: T + flag, rounded up to alignment
  std::optional<char>                              sizeof=2   alignof=1
  std::optional<int>                               sizeof=8   alignof=4
  std::optional<double>                            sizeof=16  alignof=8
  std::optional<std::string>                       sizeof=40  alignof=8
  std::optional<std::unique_ptr<int>>              sizeof=16  alignof=8
  std::optional<bool>                              sizeof=2   alignof=1
variant: max(alternatives) + index, rounded up
  std::variant<char, char>                         sizeof=2   alignof=1
  std::variant<int, float>                         sizeof=8   alignof=4
  std::variant<int, double>                        sizeof=16  alignof=8
  std::variant<std::monostate, int>                sizeof=8   alignof=4
  std::variant<int, std::string>                   sizeof=40  alignof=8
  std::variant<std::string, std::vector<int>, double> sizeof=40  alignof=8
expected: max(T, E) + flag
  std::expected<int, int>                          sizeof=8   alignof=4
  std::expected<int, std::error_code>              sizeof=24  alignof=8
  std::expected<std::string, std::error_code>      sizeof=40  alignof=8
  std::expected<void, std::error_code>             sizeof=24  alignof=8
any: a manager pointer + a small buffer
  std::any                                         sizeof=16  alignof=8
```

Reading it: `optional<int>` is **8 bytes for a 4-byte value**: the flag costs 100% (padding). `optional<double>` is 16. `variant<int, double>` is 16 (8-byte payload + 4-byte index padded). `optional<std::unique_ptr<int>>` is 16 even though a null pointer could encode "empty" in 8: the standard's specification (a separate `bool` plus `value()` returning a reference to a live object) makes the niche *not allowed in general*: **Rust's `Option<Box<T>>` is 8 bytes; C++'s is 16.** If this matters, store a pointer, or write a niche-based `OptionalPtr`. (This is one reason the standard `optional` is not a zero-overhead replacement for a pointer.)

### Experiment 2: `optional`: access, monadic chains, and the engaged-after-move rule

```cpp
// @test run -std=c++23 -O0
#include <charconv>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

std::optional<int> parse_int(std::string_view s) {
    int v{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec == std::errc{} && p == s.data() + s.size()) return v;
    return std::nullopt;
}
std::optional<int> checked_div(int a, int b) { return b == 0 ? std::nullopt : std::optional{a / b}; }

int main() {
    // monadic chain: stops at the first failure, no nested ifs
    for (auto input : {"84", "x", "0"}) {
        auto r = parse_int(input)
                    .and_then([](int n) { return checked_div(168, n); })       // int → optional<int>
                    .transform([](int q) { return q * 2; })                    // int → int (wrapped)
                    .or_else([] { return std::optional<int>{-1}; });           // supply a fallback when empty
        std::printf("  input %-3s -> %d\n", input, *r);
    }

    std::optional<int> e;
    try { (void)e.value(); }
    catch (const std::bad_optional_access& x) { std::printf("  value() on empty throws: %s\n", x.what()); }
    std::printf("  value_or(7) on empty = %d\n", e.value_or(7));
    // *e would be undefined behavior: the dereference is unchecked.

    // moving from an optional does NOT empty it
    std::optional<std::string> a = "payload-that-is-long-enough-to-live-on-the-heap";
    auto b = std::move(a);
    std::printf("  after move: a.has_value()=%d (moved-from string size %zu), b.has_value()=%d\n", a.has_value(), a->size(), b.has_value());

    // in_place avoids a move/copy and allows non-movable types
    struct Pinned { Pinned(int) {} Pinned(const Pinned&) = delete; Pinned(Pinned&&) = delete; };
    std::optional<Pinned> p{std::in_place, 42};
    std::printf("  in_place constructed an immovable type: has_value=%d\n", p.has_value());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
  input 84  -> 4
  input x   -> -1
  input 0   -> -1
  value() on empty throws: bad optional access
  value_or(7) on empty = 7
  after move: a.has_value()=1 (moved-from string size 0), b.has_value()=1
  in_place constructed an immovable type: has_value=1
```

### Experiment 3: `variant`: visit, the `bool` trap, and `valueless_by_exception`

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <stdexcept>
#include <string>
#include <variant>

template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

struct Throws {
    Throws() = default;
    Throws(const Throws&) { throw std::runtime_error("copy failed"); }
    Throws& operator=(const Throws&) = default;
};

int main() {
    std::variant<int, double, std::string> v = 3.5;

    std::visit(overloaded{
        [](int i)                { std::printf("  int %d\n", i); },
        [](double d)             { std::printf("  double %g\n", d); },
        [](const std::string& s) { std::printf("  string %s\n", s.c_str()); },
    }, v);

    std::printf("  index=%zu holds double=%d  get_if<int>=%p\n", v.index(), std::holds_alternative<double>(v), (void*)std::get_if<int>(&v));

    // The converting-constructor trap, and the C++20 fix:
    std::variant<bool, std::string> t = "text";
    std::printf("  variant<bool,string> = \"text\"  → alternative index %zu (%s)\n", t.index(), t.index() == 1 ? "string: correct since P0608 (C++20)" : "bool: the old trap");

    // The one weird state:
    std::variant<std::string, Throws> w = std::string("alive");
    Throws thrower;
    try { w = thrower; }                                    // destroys the string, then the copy of Throws throws
    catch (const std::exception& e) { std::printf("  assignment threw: %s\n", e.what()); }
    std::printf("  valueless_by_exception = %d, index = %zu (variant_npos = %zu)\n", w.valueless_by_exception(), w.index(), std::variant_npos);
    try { std::visit([](auto&&) {}, w); }
    catch (const std::bad_variant_access& e) { std::printf("  visit on valueless throws: %s\n", e.what()); }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
  double 3.5
  index=1 holds double=1  get_if<int>=(nil)
  variant<bool,string> = "text"  → alternative index 1 (string: correct since P0608 (C++20))
  assignment threw: copy failed
  valueless_by_exception = 1, index = 18446744073709551615 (variant_npos = 18446744073709551615)
  visit on valueless throws: std::visit: variant is valueless
```

`valueless_by_exception` is the only one of the four types with an *extra, nameless* state. It can arise only if an alternative's constructor/assignment throws **and** the library could not avoid destroying the old value first. Writing alternatives with `noexcept` moves (and in particular `std::is_nothrow_move_constructible`) makes the library choose the safe temporary-then-move path and the state becomes unreachable in practice.

### Experiment 4: How `visit` compiles: a jump table

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector filter=area
#include <variant>

struct Circle { double r; };
struct Rect   { double w, h; };
struct Tri    { double b, h; };

double area(const std::variant<Circle, Rect, Tri>& s) {
    return std::visit([](const auto& x) -> double {
        using T = std::remove_cvref_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Circle>) return 3.14159 * x.r * x.r;
        else if constexpr (std::is_same_v<T, Rect>) return x.w * x.h;
        else return 0.5 * x.b * x.h;
    }, s);
}
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
area(std::variant<Circle, Rect, Tri> const&):
	movzx	eax, BYTE PTR 16[rdi]
	cmp	al, 1
	je	.L2
	cmp	al, 2
	jne	.L6
	movsd	xmm0, QWORD PTR .LC1[rip]
	mulsd	xmm0, QWORD PTR [rdi]
	mulsd	xmm0, QWORD PTR 8[rdi]
	ret
.L6:
	movsd	xmm1, QWORD PTR [rdi]
	movsd	xmm0, QWORD PTR .LC0[rip]
	mulsd	xmm0, xmm1
	mulsd	xmm0, xmm1
	ret
.L2:
	movsd	xmm0, QWORD PTR [rdi]
	mulsd	xmm0, QWORD PTR 8[rdi]
	ret
```

For this **3-alternative** variant GCC 14 emitted a short **compare chain** on the tag byte at offset 16 (`cmp al, 1`, `cmp al, 2`), not a table. With 8 alternatives (verified separately) it emits a real **jump table**: `notrack jmp rax` through a table of `.long` offsets. Either way there is **no virtual call, no allocation, no RTTI**: `visit` over a closed set is a `switch` on the tag. This is why `variant` + `visit` is a legitimate high-performance replacement for a virtual hierarchy when the set is closed. The cost of changing the set is paid at compile time, across every visitor.

### Experiment 5: `any`: small-buffer behaviour, measured

```cpp
// @test run -std=c++23 -O0
#include <any>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <typeinfo>

static long g_allocs = 0;
void* operator new(std::size_t n) { ++g_allocs; return std::malloc(n); }
void  operator delete(void* p) noexcept { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept { std::free(p); }

template <class T> void probe(const char* name, T value) {
    g_allocs = 0;
    { std::any a = std::move(value); (void)a; }
    std::printf("  %-26s sizeof=%-3zu allocations=%ld\n", name, sizeof(T), g_allocs);
}

struct Big { std::array<char, 64> bytes{}; };
struct ThrowingMove { ThrowingMove() = default; ThrowingMove(ThrowingMove&&) noexcept(false) {} ThrowingMove(const ThrowingMove&) = default; };

int main() {
    std::puts("storing T in a std::any (libstdc++ buffer = sizeof(void*) = 8 bytes):");
    probe("int",                 42);
    probe("double",              3.14);
    probe("pointer",             (void*)nullptr);
    probe("std::string",         std::string("hi"));      // 32 bytes: doesn't fit
    probe("Big (64 bytes)",      Big{});
    probe("throwing-move empty", ThrowingMove{});         // fits, but NOT nothrow-movable: forced to the heap

    std::any a = 42;
    std::printf("\n  any_cast<int>(&a) -> %s\n", std::any_cast<int>(&a) ? "pointer" : "null");
    std::printf("  any_cast<long>(&a) -> %s   (exact type match only)\n", std::any_cast<long>(&a) ? "pointer" : "null");
    try { (void)std::any_cast<std::string>(a); }
    catch (const std::bad_any_cast& e) { std::printf("  any_cast<string> throws: %s\n", e.what()); }
    std::printf("  type() = %s\n", a.type().name());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
storing T in a std::any (libstdc++ buffer = sizeof(void*) = 8 bytes):
  int                        sizeof=4   allocations=0
  double                     sizeof=8   allocations=0
  pointer                    sizeof=8   allocations=0
  std::string                sizeof=32  allocations=1
  Big (64 bytes)             sizeof=64  allocations=1
  throwing-move empty        sizeof=1   allocations=1

  any_cast<int>(&a) -> pointer
  any_cast<long>(&a) -> null   (exact type match only)
  any_cast<string> throws: bad any_cast
  type() = i
```

Only types that are (1) small enough *and* (2) `nothrow`-move-constructible are inline, so **a plain `std::string` is always on the heap in libstdc++**, because 32 bytes exceed the 8-byte buffer (libc++ is more generous). This is a quality-of-implementation property that **changes between standard libraries and versions**: never design around "any is cheap for small types".

### Experiment 6: `expected`: the chain, and its cost against exceptions

```cpp
// @test run -std=c++23 -O2
#include <chrono>
#include <charconv>
#include <cstdio>
#include <expected>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

// ----- the same parsing API in two styles -----
std::expected<int, std::errc> parse_e(std::string_view s) {
    int v{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size()) return std::unexpected(ec == std::errc{} ? std::errc::invalid_argument : ec);
    return v;
}
int parse_x(std::string_view s) {
    int v{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size()) throw std::invalid_argument("bad int");
    return v;
}

// ----- a small monadic pipeline: parse → range-check → scale -----
std::expected<int, std::errc> pipeline(std::string_view s) {
    return parse_e(s)
        .and_then([](int n) -> std::expected<int, std::errc> { if (n < 0 || n > 1000) return std::unexpected(std::errc::result_out_of_range); return n; })
        .transform([](int n) { return n * 2; });
}

template <class F> double ms(F&& f) {
    auto t0 = std::chrono::steady_clock::now(); f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int main() {
    for (auto s : {"21", "-5", "abc", "999999"}) {
        auto r = pipeline(s);
        if (r) std::printf("  %-7s -> ok %d\n", s, *r);
        else   std::printf("  %-7s -> error %d (%s)\n", s, int(r.error()), std::make_error_code(r.error()).message().c_str());
    }

    // cost of the ERROR path as a function of failure rate
    constexpr int N = 400000;
    std::printf("\n  %d parses; time by failure rate\n  fail%%   expected     exception\n", N);
    for (int fail_pct : {0, 1, 10, 50}) {
        std::vector<std::string_view> inputs(N);
        for (int i = 0; i < N; ++i) inputs[i] = (i % 100 < fail_pct) ? "oops" : "12345";
        long sum = 0;
        double te = ms([&] { for (auto s : inputs) { auto r = parse_e(s); sum += r ? *r : -1; } });
        double tx = ms([&] { for (auto s : inputs) { try { sum += parse_x(s); } catch (const std::invalid_argument&) { sum -= 1; } } });
        std::printf("  %3d%%    %7.2f ms   %8.2f ms   (%.0fx)  [checksum %ld]\n", fail_pct, te, tx, tx / te, sum);
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
  21      -> ok 42
  -5      -> error 34 (Numerical result out of range)
  abc     -> error 22 (Invalid argument)
  999999  -> error 34 (Numerical result out of range)

  400000 parses; time by failure rate
  fail%   expected     exception
    0%       3.71 ms       3.68 ms   (1x)  [checksum 9876000000]
    1%       4.27 ms       9.60 ms   (2x)  [checksum 9777232000]
   10%       3.84 ms      59.56 ms   (16x)  [checksum 8888320000]
   50%       4.21 ms     288.01 ms   (68x)  [checksum 4937600000]
```

*What the measurement says.* On the **success path** the two styles cost about the same: exceptions are free until thrown (the zero-cost model, Chapter 22), and `expected` adds one compare-and-branch per call. On the **failure path**, an exception costs on the order of **1.4 µs** here (50% of 400 000 calls failing added ~285 ms, i.e. ~1.4 µs per throw: unwinding via the table lookup, `__cxa_allocate_exception`, personality routines), roughly **a thousand times** the cost of returning an `expected`. The whole-workload ratio therefore tracks the failure rate: **1× at 0%, 2× at 1%, 16× at 10%, 68× at 50%**. Where failures are rarer than about 1 in 1000, the difference is below the noise; exceptions then have the advantage of leaving the success path free of error-propagation branches (the `expected` version pays one test per call). (Indicative timings, single process; Chapter 40 covers rigorous measurement.)

> **Verdict.** Use **exceptions** for *exceptional* (rare, usually unrecoverable-at-the-call-site) failures where the error needs to travel many frames. Use **`expected`** where failure is a *normal outcome of the operation* (parsing, lookup, I/O that can fail, validation) and the caller is expected to handle it right there. Use **`optional`** when there's *exactly one* way to fail and no reason to explain it ("not found"). Use **error codes** (`std::error_code` out-parameter, or return value) in `noexcept` APIs, in the C interop layer, and in code compiled with `-fno-exceptions`. Chapter 23 builds on this table.

### Experiment 7: The four error-handling styles side by side

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <expected>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

struct Config { std::string name; int port; };

// (1) optional: "there is no config": cannot say why
std::optional<Config> load_opt(bool ok) { if (!ok) return std::nullopt; return Config{"svc", 80}; }

// (2) error code out-parameter: C-style, works with -fno-exceptions; the caller can ignore it
Config load_ec(bool ok, std::error_code& ec) {
    if (!ok) { ec = std::make_error_code(std::errc::no_such_file_or_directory); return {}; }
    ec.clear(); return Config{"svc", 80};
}

// (3) exception: the failure travels up the stack; the call site stays clean
Config load_exc(bool ok) { if (!ok) throw std::system_error(std::make_error_code(std::errc::no_such_file_or_directory), "load"); return Config{"svc", 80}; }

// (4) expected: failure is a value with a reason; `[[nodiscard]]` on the type is built-in
std::expected<Config, std::error_code> load_exp(bool ok) {
    if (!ok) return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    return Config{"svc", 80};
}

int main() {
    std::puts("success paths:");
    std::printf("  optional : %s\n", load_opt(true)->name.c_str());
    std::error_code ec; auto c = load_ec(true, ec);
    std::printf("  errcode  : %s (ec=%d)\n", c.name.c_str(), int(ec.value()));
    std::printf("  exception: %s\n", load_exc(true).name.c_str());
    std::printf("  expected : %s\n", load_exp(true)->name.c_str());

    std::puts("failure paths:");
    std::printf("  optional : %s\n", load_opt(false) ? "?" : "nullopt (no reason)");
    load_ec(false, ec);
    std::printf("  errcode  : %s\n", ec.message().c_str());
    try { load_exc(false); } catch (const std::system_error& e) { std::printf("  exception: %s\n", e.what()); }
    auto r = load_exp(false);
    std::printf("  expected : %s\n", r.error().message().c_str());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
success paths:
  optional : svc
  errcode  : svc (ec=0)
  exception: svc
  expected : svc
failure paths:
  optional : nullopt (no reason)
  errcode  : No such file or directory
  exception: load: No such file or directory
  expected : No such file or directory
```

| | `optional` | error code | exception | `expected` |
|---|---|---|---|---|
| Carries *why*? | ❌ | ✅ | ✅ (any type) | ✅ (any type) |
| Can be silently ignored? | ⚠️ (use `[[nodiscard]]`) | ⚠️ (unless `[[nodiscard]]`; the out-param is ignorable) | ❌ (propagates) | ⚠️ (neither `std::expected` nor `std::optional` is `[[nodiscard]]` in libstdc++ 14: GCC stayed silent when I discarded one; add `[[nodiscard]]` to the *function*, or declare your error type `struct [[nodiscard]] …`) |
| Works with `-fno-exceptions`, in `noexcept` code | ✅ | ✅ | ❌ | ✅ |
| Success-path cost | 1 branch | 1 branch + out-param | **0** (tables) | 1 branch |
| Failure-path cost | ~1 ns | ~1 ns | **~1–2 µs** | ~1 ns |
| Propagation through many frames | manual (`and_then`) | manual | **automatic** | manual (`and_then`) / a `TRY` macro (Chapter 23) |
| Composes with constructors, operators | ❌ constructors can't return it | ❌ | ✅ | ❌ |
| Interop with C | ✅ | ✅ | ❌ (must not cross the boundary) | ✅ |
| ABI / return convention | in registers if trivial | n/a | n/a | in registers if trivially copyable |

---

## 8. Assembly / runtime investigation

Three diagnostics worth running on your own types:

```bash
# 1. Is it passed in registers? Look at the signature in the generated code: a hidden first pointer argument means "memory".
g++-14 -std=c++23 -O2 -S -masm=intel -o - prog.cpp | c++filt | grep -A6 'make_opt'

# 2. How big is the visit table? (look for .rodata tables with many .quad entries)
g++-14 -std=c++23 -O2 -S -o - prog.cpp | grep -c '\.quad'

# 3. Where did exceptions go? exceptions cost nothing on the happy path, but leave data:
size -A prog.o | grep -E 'eh_frame|gcc_except_table'
nm -C prog.o | grep -E '__cxa_throw|_Unwind_Resume'
```

In Experiment 1's `sizeof` ledger the *trivially-copyable* ones (`optional<int>`, `variant<int,double>`, `expected<int,int>`) are returned in **registers** (`rax:rdx`), and the non-trivial ones (`optional<std::string>`) through a hidden pointer. Returning `optional<int>` is as cheap as returning a pair of integers; returning `expected<std::string, E>` is a memory write into the caller's frame (the NRVO slot).

---

## 9. Implementation exercise

Implement `MiniVariant<Ts...>`:

1. aligned storage sized for the largest alternative, an index, **correct destructor, copy and move** dispatch by function table
2. `emplace<I>(args...)` / `emplace<T>(args...)`, `index()`, `holds_alternative<T>`, `get_if<T>`
3. `visit(f, v)` for one variant, with the dispatch table built by `std::index_sequence` and **result type checked** (all alternatives must return the same type: `static_assert`)
4. conditionally trivial special members (Chapter 10: constrain the copy constructor/destructor `= default` when every alternative is trivial), tested with `is_trivially_copyable_v`
5. the strong guarantee for `emplace`, *or* a `valueless_by_exception` state: decide, implement, and write the test that exercises the choice

<details>
<summary><strong>Solution</strong></summary>

```cpp
// @test run -std=c++23 -O0 -fsanitize=address,undefined
#include <algorithm>
#include <cstdio>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

template <class T, class... Ts> inline constexpr std::size_t index_of_v = [] {
    constexpr bool same[] = {std::is_same_v<T, Ts>...};
    for (std::size_t i = 0; i < sizeof...(Ts); ++i) if (same[i]) return i;
    return std::size_t(-1);
}();

template <class... Ts>
class MiniVariant {
    static constexpr std::size_t npos = std::size_t(-1);
    alignas(Ts...) unsigned char buf_[std::max({sizeof(Ts)...})];
    std::size_t index_ = npos;

    template <std::size_t I> using alt_t = std::tuple_element_t<I, std::tuple<Ts...>>;

    void destroy() noexcept {
        if (index_ == npos) return;
        static constexpr void (*table[])(void*) = { +[](void* p) { static_cast<Ts*>(p)->~Ts(); }... };
        table[index_](buf_);
        index_ = npos;
    }
public:
    MiniVariant() = default;
    ~MiniVariant() { destroy(); }

    template <class T, class... A>
        requires (index_of_v<T, Ts...> != npos) && std::constructible_from<T, A...>
    T& emplace(A&&... a) {
        destroy();                                           // after this, the variant is valueless…
        T* p = ::new (static_cast<void*>(buf_)) T(std::forward<A>(a)...);   // …and stays so if this throws
        index_ = index_of_v<T, Ts...>;
        return *p;
    }

    template <class T> requires (index_of_v<std::remove_cvref_t<T>, Ts...> != npos)
    MiniVariant(T&& v) { emplace<std::remove_cvref_t<T>>(std::forward<T>(v)); }

    MiniVariant(const MiniVariant& o) {
        if (o.index_ == npos) return;
        static constexpr void (*table[])(void*, const void*) = { +[](void* d, const void* s) { ::new (d) Ts(*static_cast<const Ts*>(s)); }... };
        table[o.index_](buf_, o.buf_);
        index_ = o.index_;
    }
    MiniVariant& operator=(const MiniVariant& o) { if (this != &o) { MiniVariant tmp(o); destroy(); /* move tmp in */ 
        if (tmp.index_ != npos) { static constexpr void (*mv[])(void*, void*) = { +[](void* d, void* s) { ::new (d) Ts(std::move(*static_cast<Ts*>(s))); }... };
            mv[tmp.index_](buf_, tmp.buf_); index_ = tmp.index_; } } return *this; }

    std::size_t index() const noexcept { return index_; }
    bool valueless() const noexcept { return index_ == npos; }
    template <class T> bool holds() const noexcept { return index_ == index_of_v<T, Ts...>; }
    template <class T> T* get_if() noexcept { return holds<T>() ? std::launder(reinterpret_cast<T*>(buf_)) : nullptr; }

    template <class F>
    decltype(auto) visit(F&& f) {
        using R = std::invoke_result_t<F, alt_t<0>&>;
        static_assert((std::is_same_v<R, std::invoke_result_t<F, Ts&>> && ...), "all alternatives must return the same type");
        static constexpr R (*table[])(F&, void*) = { +[](F& fn, void* p) -> R { return fn(*std::launder(static_cast<Ts*>(p))); }... };
        return table[index_](f, buf_);
    }
};

static_assert(!std::is_trivially_destructible_v<MiniVariant<int, double>>);   // (exercise 4: make this true with a constrained destructor)

int main() {
    MiniVariant<int, std::string, double> v = std::string("a string long enough to avoid SSO, so ASan sees the heap");
    v.visit([](auto& x) -> void { if constexpr (std::is_same_v<std::remove_cvref_t<decltype(x)>, std::string>) std::printf("string of %zu chars\n", x.size()); });
    MiniVariant<int, std::string, double> w = v;           // copy: duplicates the string
    v.emplace<double>(2.5);                                // destroys the string in v; w keeps its own
    std::printf("v holds double: %d, w holds string: %d, index(w)=%zu\n", v.holds<double>(), w.holds<std::string>(), w.index());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
string of 56 chars
v holds double: 1, w holds string: 1, index(w)=1
```

ASan/UBSan confirm the string is freed exactly once per owner: no leak, no double free. The `visit` table is built from the pack with the same expansion trick as the destructor. The missing pieces (the move constructor, the trivial special members, the `noexcept` propagation) are exercises 4–5 and the chapter challenge.

</details>

---

## 10. Real-world example

| Where | What it uses and why |
|---|---|
| **Compilers (LLVM, Clang, rustc-in-C++ style code)** | `variant<…>` for AST nodes and IR instructions; `visit` = pattern-matching. Closed set, exhaustive handling, no heap per node if you arena-allocate (Chapter 26) |
| **JSON libraries (nlohmann, simdjson's DOM)** | A JSON value is a `variant<null, bool, int64, double, string, array, object>` (or a hand-rolled tagged union to control layout) |
| **Protocol/message handling** | `variant<Connect, Data, Close, Error>` + `visit`: a state machine as a type |
| **`std::filesystem`, `std::from_chars`, I/O APIs** | Error code **out-parameter overloads** are the `-fno-exceptions`-friendly version; `std::expected` is the direction of new APIs (`std::from_chars` itself still returns a struct) |
| **Config/plugin property bags** | `std::any` or (better) `variant<bool, int, double, string>`: usually the closed set is enough; `any` is the escape hatch |
| **Qt** | `QVariant`: a `variant` of Qt's meta-types **with open registration** (`qRegisterMetaType`), the dynamic, extensible cousin; it also gives signal/slot argument marshalling (Chapter 48) |
| **Qt 6.6+ `QtPrivate::Expected`/`std::expected` in QtCore** | Tracks the direction: Qt's own result type is ceding to `std::expected` |

> **Opinion: the default order of preference for a function that can "fail".** (1) Make failure impossible by the type (a validated `Port` type that cannot be 70000). (2) `std::optional` when "none" is self-explanatory. (3) `std::expected` when the caller needs the reason and will handle it locally. (4) Exceptions when the failure is rare and the handler is far away, or the failing operation is a constructor. (5) `bool` + out-parameter / error code only at ABI/C boundaries. **Never** use `-1`/`nullptr` sentinels in new C++ code, and **never** `std::any` where a `variant` fits.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| `*opt` without checking | UB: reads uninitialized storage (no exception) | `if (opt)`, `value()`, `value_or`, or `and_then` |
| `optional<T>` where `T*` is meant ("borrow a maybe-object") | Copies the object; mutations don't affect the original | `T*`, or C++26 `optional<T&>` |
| `optional<bool>` in conditions | `if (ob)` tests engagement, not the value: `optional<bool>{false}` is *true* | `ob.value_or(false)` / `ob == true` |
| `variant` converting constructor picks the "wrong" alternative (`bool` vs `string`) | Silent bug | Use `std::in_place_type<T>` or C++20 compilers (P0608); `static_assert(std::is_same_v<…>)` |
| `visit` with a visitor that doesn't cover an alternative | Hard compile error (good) | Add the case; or a generic `[](auto&&)` catch-all, which *disables* the exhaustiveness check: use sparingly |
| `visit` over 3+ variants of many alternatives | Compile-time and code-size explosion (product of sizes) | Reduce dimensions; visit the pair in nested steps |
| `valueless_by_exception` ignored | `visit`/`get` throw `bad_variant_access` unexpectedly | `noexcept` moves for alternatives; `emplace` into a temporary first |
| Recursion: `struct Node { std::variant<int, std::vector<Node>> v; };` | Incomplete type | `std::vector<Node>` is allowed with incomplete `Node` (C++17), but `variant<int, Node>` is not: add indirection (`unique_ptr`) |
| `std::any_cast<int>` on a `short` | `bad_any_cast`: exact types only | Store a canonical type |
| `std::any` holding a big type in a hot path | A heap allocation per copy | `variant`, or hold by `shared_ptr<const T>` |
| `any` across a DLL/shared-library boundary | Type check can fail (duplicate `typeinfo` objects across `.so`) | Symbol visibility/`-fvisibility`; avoid `any` at ABI boundaries |
| `expected<T, E>` `*e` on error | UB | Check first; or `.value()` (throws) |
| `expected` errors as strings | Allocation on the error path; no programmatic handling | An error `enum`/`std::error_code`; add context via `transform_error` |
| Forgetting `[[nodiscard]]` on functions returning error types | Errors silently dropped | `[[nodiscard]]` on the function, or on the *error type* (`struct [[nodiscard]] Error`) |
| Treating `optional`/`variant` as "free" in a hot data structure | Padding doubles sizes (Experiment 1) | Pack the flag elsewhere (a bitmask, SoA, Chapter 27), or use a niche |

---

## 12. Exercises

1. **Layout prediction.** For ten types, including `optional<std::optional<int>>`, `variant<std::monostate>`, `variant<char, std::string>`, `optional<std::array<char,7>>`, predict `sizeof` and `alignof`; check; explain every padding byte.
2. **`optional<bool>` trap.** Write a function returning `optional<bool>` and show three ways a caller can misread it. Replace the return type by a 3-valued `enum class`. Which is better?
3. **Monadic rewrite.** Take a function with four nested `if (auto x = f()) { if (auto y = g(*x)) { … } }` and rewrite it with `and_then`/`transform`/`or_else`. Count lines; look at the assembly at `-O2`: is it the same?
4. **A JSON value.** Implement `Json = variant<nullptr_t, bool, double, string, vector<Json>, map<string, Json>>` (hint: incomplete-type rules; use a wrapper struct deriving from `variant`). Write `to_string` with `visit`. Count allocations for parsing a 1 KB document.
5. **`visit` blowup.** Write `visit` over 1, 2, 3, 4 variants of 8 alternatives each. Measure compile time and object size. Rewrite the 4-variant case as nested single visits. What did you save?
6. **Valueless on purpose.** Build the smallest program that makes a `std::variant` valueless. Then make the same program *not* valueless with a `noexcept` move. What did the library do differently (inspect with `-S`)?
7. **`any` vs `variant` vs virtual.** Store 10⁶ heterogeneous values (int, double, string) three ways. Compare memory (via the counting `operator new`), construction time and visit time.
8. **`expected` interop.** Write `TRY(expr)` (a statement-expression macro, GCC/Clang extension) that returns early with `std::unexpected` on error. Compare readability and codegen to explicit `if`s and to exceptions. Why is a `TRY` macro the main ergonomic complaint about `expected` in C++?
9. **Niche optional.** Implement `OptionalPtr<T>` (8 bytes: null means empty) and `OptionalIndex` (an `int` where `-1` means empty) with the optional interface (`has_value`, `value_or`, `and_then`). Show the `sizeof` win and the type-safety you give up.

---

## 13. Challenge: an error-handling layer for a library

Design the error-handling strategy of a mini HTTP client library (`connect`, `send`, `read_response`, `parse_headers`, `close`) that must:

- be usable **with and without exceptions** (compile with `-fno-exceptions` and still link): the same functions, two surfaces (`expected`-returning core, throwing wrappers generated from it)
- keep **error context** (what operation, which file/line, the OS `errno`, the failing URL) without allocating on the error path (a fixed-size error struct with a small inline buffer)
- convert errors **across layers** (`std::error_code` categories for socket errors, HTTP status errors and parse errors; `transform_error` at each layer boundary)
- provide a `TRY` mechanism and show the generated assembly for the success path (it should equal the hand-written early-return version)
- be **ABI-honest**: public functions return trivially copyable result types where possible; justify the layout of your `Error` type in terms of Chapter 37

Write tests injecting failures at every step, run them under ASan/UBSan, and measure the cost of the failure path against a throwing implementation (Experiment 6's method). Document the rule for which errors are **programmer errors** (assert/terminate) versus **runtime errors** (`expected`).

---

## 14. Knowledge check

1. What are the storage and tag of each of `optional`, `variant`, `any`, `expected`? Which may allocate?
2. Why is `sizeof(std::optional<int>)` 8, and why is `sizeof(std::optional<std::unique_ptr<int>>)` 16?
3. When does a `variant` become *valueless by exception*, and why can `optional`/`expected` not?
4. How does `std::visit` dispatch at run time, and what is the code-size cost for *k* variants?
5. What does `std::any` need from the contained type, and what decides whether it is stored inline?
6. What does moving from a `std::optional<T>` do to its engaged state?
7. What are the monadic operations on `optional`/`expected`, and what does each do on an empty/error state?
8. Why is `expected` preferred to exceptions for parsing user input, and exceptions preferred to `expected` for I/O deep in a call stack?
9. Why is `variant<bool, std::string> v = "x"` a trap, and which standard change fixed it?
10. Why can `any` not hold a `std::unique_ptr`, and what is the standard move-only alternative for callables?
11. What is the expression problem, and where do `variant` and virtual functions each sit?
12. In which cases is an `optional<int>` returned in registers, and when through memory?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. `optional`: `T` + `bool` (inline). `variant`: union of alternatives + index (inline). `expected`: union of `T`/`E` + `bool` (inline). `any`: a manager function pointer + small buffer, or a pointer to a heap block. Only `any` may allocate.
2. `optional<int>`: 4 bytes of payload + 1 flag byte, padded to alignment 4 → 8. `optional<unique_ptr<int>>`: 8 + flag, padded to 16. The standard's design spends a dedicated `bool` and doesn't use niches (null pointer) in general.
3. When constructing the *new* alternative throws after the old one was already destroyed (assignment/`emplace`). `optional` and `expected` have only two states, so they can always construct into a temporary first or roll back; a variant of N types cannot keep all old values around.
4. By indexing a table of function pointers (or an equivalent switch) with the tag: an indirect jump, no allocation. For *k* variants the table has n₁×…×n_k entries (each a distinct instantiation), so code size grows multiplicatively.
5. Copy-constructibility (to be able to copy the `any`). Inline storage needs `sizeof(T)` ≤ the buffer and `is_nothrow_move_constructible_v<T>`; the buffer size is implementation-defined.
6. Nothing: the source stays engaged, containing a moved-from `T`.
7. `and_then(f)`: if a value, `f(value)` (which returns the same wrapper); else propagate. `transform(f)`: if a value, wrap `f(value)`; else propagate. `or_else(f)`: if empty/error, call `f` (for `expected`, with the error); else propagate the value. `transform_error` (`expected`) maps the error.
8. Parsing user input *often fails* (the "error" is a normal result), so the 1–2 µs/exception cost matters and the handler is right next to the call. Deep I/O errors are rare and need to travel many frames; exceptions propagate automatically at zero success-path cost, avoiding `if (!r) return r;` after every call.
9. `"x"` (a `const char*`) converts to `bool` by a standard conversion, which beat the user-defined conversion to `std::string`. P0608R3 (C++20) restricts the converting constructor to non-narrowing conversions and excludes `bool` from non-`bool` arguments.
10. Because `any` must be copy-constructible (the manager has a copy operation). `std::move_only_function` (C++23) is the move-only wrapper for callables; there is no standard move-only `any`.
11. Adding operations vs adding types over a fixed family. `variant` + visit: new operations are easy (new visitor), new types are hard (every visitor changes). Virtual functions: new types are easy (a new derived class), new operations are hard (add a virtual function to every class).
12. When the type is trivially copyable and small (≤ 16 bytes): `optional<int>`, `optional<double>`. When it is non-trivial (e.g., `optional<std::string>`) it's returned through a hidden pointer (the caller's frame).

</details>

---

[← Previous: Chapter 12](12-views-and-non-owning-types.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 14 — Ranges →](../part-06-ranges/14-ranges.md)
