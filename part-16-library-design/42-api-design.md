# Chapter 42 — API Design: Signatures That Tell the Truth

> **Part XVI · Modern library design** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 8 hours**
> **Prerequisites:** [Chapter 6 (move semantics)](../part-03-value-categories-and-move/06-move-semantics.md), [Chapter 12 (views)](../part-05-standard-library/12-views-and-non-owning-types.md), [Chapter 22 (exceptions)](../part-09-error-handling/22-exceptions.md), [Chapter 23 (`expected`)](../part-09-error-handling/23-expected.md), [Chapter 25 (smart pointers)](../part-10-memory/25-smart-pointers.md), [Chapter 37 (ABI)](../part-14-compilation-and-linking/37-abi.md) &nbsp;|&nbsp; **Standards:** C++20 designated initializers and concepts ⚖️, `[[nodiscard]]` C++17 ⚖️ &nbsp;|&nbsp; **Tools:** `g++-14`, ASan, Google Benchmark

[← Previous: Chapter 41](../part-15-performance/41-compiler-optimization.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 43 — Header and library architecture →](43-header-and-library-architecture.md)

---

**In one sentence:** a function signature is a contract in the type system, so a good C++ API makes **ownership, lifetime, mutability, failure and cost** visible in the declaration, makes the wrong call fail to compile, and leaves room to evolve without breaking its users.

**By the end of this chapter you can:**

- choose a parameter type from the real decision table (read, sink, mutate, share, optional) and justify it with copy/move counts
- return values and errors in a way that cannot be silently ignored
- design against the common misuse paths: swapped arguments, dangling views, boolean traps, forgotten results
- constrain generic APIs with concepts so that errors appear at the call site, not 40 lines into a template
- evolve an API (add options, add overloads) without source or binary breakage
- decide, with numbers, when `shared_ptr` belongs in an interface (rarely)

---

## 1. Problem

An API is used far more often than it is written, by people who read the *declaration* and not the implementation. Poor signatures fail in four ways:

| Failure | Example |
|---|---|
| **They hide ownership** | `Widget* make();` Who deletes it? `void take(Widget*);` Does it keep the pointer? |
| **They hide lifetime** | `std::string_view name() const;` Valid until when? Returning a view of a temporary |
| **They invite misuse** | `void resize(int w, int h);` Called as `resize(h, w)`; `connect(host, true, false, 30)`: what is `true`? |
| **They can't evolve** | Adding a parameter breaks every caller and every function pointer; changing a class's members breaks every binary (Chapter 37) |

None of this needs fancy features. It needs the discipline of treating the declaration as the documentation the compiler checks.

---

## 2. Historical context

| Era | Style | Problem |
|---|---|---|
| C / C++98 | `T*` for everything: array, optional, owner, observer, out-parameter | Pointers say nothing; comments carry the contract |
| C++98 idioms | `const T&` for input, `T&` for output parameters, `std::auto_ptr` return for ownership, return codes or exceptions | `auto_ptr` was broken; out-params obscure data flow |
| C++11 | `unique_ptr`/`shared_ptr` make ownership visible; move semantics enable pass-by-value sinks; `override`, `= delete`, `explicit` operators | Guidance fragments: "pass by value", "pass by `&&`", "`const&`" argued over |
| C++14–17 | `string_view`, `optional`, `variant`, `[[nodiscard]]`, structured bindings, `std::filesystem` shows modern API style | New dangling risks (views) |
| C++20 | Concepts, `span`, ranges, designated initializers, `[[nodiscard("reason")]]`, `std::source_location` | Constrained, self-documenting generic APIs |
| C++23 | `std::expected`, `std::print`, deducing `this`, `std::move_only_function`, `static operator()` | Error-as-value in the standard vocabulary |
| Guidelines | C++ Core Guidelines (F.* function rules, I.* interfaces, R.* resource rules) consolidate the vocabulary | |

---

## 3. Modern solution

The modern vocabulary, as a single table. Read the right column as "what a reader may conclude from the signature alone".

| I want to… | Parameter type | The reader concludes |
|---|---|---|
| **read** a cheap value (`int`, `double`, a handle, an enum) | `T` | Copied; callee can't affect the caller |
| **read** an expensive object (`std::string`, `std::vector`, user class) | `const T&` | Not copied; not retained *after the call* |
| **read** text / a contiguous sequence without caring who owns it | `std::string_view` / `std::span<const T>` | Cheap to pass; **valid only for this call**; not nul-terminated |
| **modify** the caller's object | `T&` | The argument is changed; the caller must pass an lvalue |
| **store** (sink) a copy | `T` by value, then `std::move` into the member | Callee takes ownership of its own copy |
| **take over** a unique resource | `std::unique_ptr<T>` by value | Caller gives it up (`std::move`) |
| **maybe** pass one | `std::optional<T>` (by value or `const&`) or `const T*` | Absence is a valid input |
| **observe** an object that must outlive the call | `T&` or `T*` (non-owning) | Callee doesn't own, doesn't delete |
| **share** ownership | `std::shared_ptr<T>` by value | The callee keeps the object alive; **this is a design statement**, not a convenience |
| **use** the shared object during the call only | `T&` / `const T&` (not the `shared_ptr`) | Lifetime is the caller's problem |
| **generic** callable | `F&&` (constrained) or `std::function_ref` (C++26 🟡) / `std::move_only_function` (C++23) | Type-erased or template: Chapter 21 |
| **return** a new object | `T` by value | Moves/elides; the caller owns it |
| **return** a view into `*this` | `const T&` / `string_view` | Valid while `*this` is unchanged |
| **fail** | `[[nodiscard]] std::expected<T,E>`, `optional<T>`, or exception | Failure is visible (or declared exceptional) |

The rest of the chapter explains, with measurements, why each row is the right answer.

---

## 4. Mental model

### Every parameter answers four questions

```text
   void f( ????  x );
              │
              ├── 1. COPY or REFERENCE?        who pays for the transfer
              ├── 2. MUTABLE or const?         can the callee change the caller's data
              ├── 3. OWNING or OBSERVING?      who destroys the object
              └── 4. HOW LONG is it valid?     during the call / until X / forever
```

C++ has the machinery to express (1)–(3) in the type. Question (4) is the hard one: the language does **not** express lifetimes, so it must be encoded by convention and made hard to get wrong.

### The cost model is two numbers: copies and moves

A *sink* parameter is one the function keeps (a setter, a constructor argument, `push_back`). Let `c` be a copy and `m` a move. The callers are an lvalue, an `std::move`d lvalue, and a prvalue. Experiment 1 counts them for four designs:

```text
                        lvalue    std::move(lv)   temporary
  const T&                1c         1c              1c        always copies, never moves
  T (by value)           1c+1m       2m              1m        one extra move, one overload
  const T& + T&&          1c         1m              1m        optimal, two overloads (2^n for n params)
  template<U&&> forward   1c         1m              1m        optimal, but unconstrained template
```

By-value costs **one extra move** over the perfect forwarding design in exchange for **one function, one signature, one place to read**. Since moves are usually O(1) pointer swaps and the cost of a sink is typically dominated by what the callee does next, that is the right trade for ordinary sinks. It is the wrong trade when the type is expensive to move (a large `std::array`, no-heap types) or when you are in a hot generic path (`emplace`, `push_back`): the standard containers use the two-overload form precisely because they are the performance floor.

### Lifetime is the missing dimension

```text
   owning types:   std::string  std::vector  unique_ptr        ──►  the object lives as long as you hold it
   non-owning:     string_view  span  T&  T*  iterator  ranges::subrange   ──►  lifetime = someone else's problem
```

Every non-owning type in a signature creates a **lifetime responsibility** for the caller. Taking one as a *parameter* is safe (the callee returns before the caller's temporaries die). Returning one, or storing one in a member, is where bugs are made (Experiment 2). The API-level rule: **a function may return or store a non-owning type only when its documentation can say "valid as long as argument N / `*this` is alive and unmodified"**; otherwise return an owning type.

### Make illegal states unrepresentable

```text
   resize(int w, int h)      →   resize(Width, Height)           swapped arguments don't compile
   open(path, true, false)   →   open(path, {.create = true})    each setting is named
   Foo* or nullptr           →   optional<Foo> / reference       absent vs present is in the type
   int error                 →   enum class / expected<T, E>     can't be mixed up with data
```

### An API is a promise about *change*

Any signature you ship will be called by code you can't see. The promises you implicitly make: the function exists; its parameter types accept these arguments; its overload set stays unambiguous; its exceptions and noexcept-ness are what you said; and (for a shared library) its **symbol and class layout** are stable (Chapter 37). Design the shape so that *adding* capability never requires *changing* a signature.

---

## 5. Language rules

| Rule | Text | API consequence |
|---|---|---|
| **Value categories & binding** ⚖️ [dcl.init.ref] | A `const T&` binds to any expression and extends a temporary's lifetime *only when bound to a local variable*, **not** when returned or stored in a member | `const T& x = f();` is safe; `struct S { const T& r; S(const T& t) : r(t) {} };` dangles on temporaries |
| **`explicit`** ⚖️ [class.conv.ctor] | A single-argument constructor without `explicit` is also an implicit conversion | Always `explicit` unless conversion is intended (`string_view` from `string` is deliberately implicit) |
| **`[[nodiscard]]`** ⚖️ C++17/20 | Encourages a warning when the return value is discarded; may be placed on a type to cover every function returning it | Apply to error-carrying and resource-returning functions; never on a function whose result is routinely optional |
| **Designated initializers** ⚖️ C++20 | `T{.a = 1, .c = 3}` for aggregates; **must be in declaration order**; omitted members take their default member initializers | Options structs get named, extensible parameters |
| **Default arguments** ⚖️ | Evaluated at the call site; part of the declaration, not the function type | Adding one changes source-compatibility with function pointers and, in a library, the *callers' compiled code* |
| **Overload resolution** ⚖️ [over.match] | Ambiguities, conversions and templates compete; adding an overload can change which function an existing call selects | Adding overloads is not always compatible. `f(0)` with `f(int)` and `f(const char*)`… |
| **`noexcept` is part of the function type** ⚖️ (C++17) | Affects overloads, function pointers, and `std::is_nothrow_*` traits | Mark move operations, swap, destructors; do not mark functions that allocate or call user code unless you mean "terminate on failure" |
| **Concepts** ⚖️ C++20 | A constrained template is rejected at the call with a named-requirement diagnostic | Use concepts as the *documentation and the check* of generic parameters |
| **Class layout & ODR** ⚖️/🧩 | A class's members and virtual functions are part of its binary interface | Not a language rule, an ABI one: see Chapter 37 |
| **Exception specification** ⚖️ | Dynamic exception specs removed in C++17; `noexcept` only | Document throw behaviour in prose; encode only `noexcept` |

### Layer check

| Layer | Question |
|---|---|
| **C++ standard** | What can the signature express (const, ref, `noexcept`, `[[nodiscard]]`, concepts)? What is silently allowed (implicit conversions, dangling)? |
| **Compiler** | Which misuse produces a diagnostic (`-Wunused-result`, `-Wdangling-*`, `-Wuninitialized`), and which only a sanitizer sees (Experiment 2)? |
| **ABI** | Does this change alter mangled names, vtable slots, class size, calling convention (small-object passing)? (Chapter 37) |
| **OS / CPU** | By-value passing of a 16-byte struct is in registers; a 24-byte one goes via memory; a non-trivially-copyable type always goes via an address (Chapter 37) |

---

## 6. Implementation model

### How parameters really travel (SysV x86-64)

| Parameter | What the compiler does |
|---|---|
| `int`, `double`, pointers, references, `string_view` (16 B, trivially copyable) | In registers (`string_view` as two registers) |
| `std::span<T>` | Registers (two) |
| `std::string`, `std::vector`, anything with a non-trivial destructor/copy | Passed as a **hidden pointer to a caller-allocated temporary**; the caller destroys it (Chapter 37) |
| `std::shared_ptr<T>` | Same as above, and copying it performs an **atomic increment**, the destructor an atomic decrement (Experiment 5) |
| `std::unique_ptr<T>` by value | Hidden pointer too (non-trivial destructor), despite being "just a pointer" |

So "pass by value" isn't free for non-trivial types even when the object is moved from: you pay an address, a destructor call for the moved-from temporary, and the move itself.

### What a view-returning accessor compiles to

```cpp
const std::string& name() const { return name_; }       // returns a pointer to the member
std::string_view   name() const { return name_; }       // returns {ptr,len} in two registers
```

Both are a couple of loads; the choice is about **what you promise** (a `const std::string&` promises null-termination via `c_str()`, a `string_view` doesn't, but accepts any string-like backing store later).

### What the sink-by-value design compiles to

```text
set(T x):    caller constructs `x` in its own frame (copy if lvalue, elided if prvalue, move if xvalue)
             callee: h.t = std::move(x);  → one move-assignment
             callee epilogue: caller destroys x (moved-from → trivial)
```

The "extra move" is the move-assignment into the member; the construction of the parameter is the copy/move/elision you'd have paid anyway.

---

## 7. Experiments

Single-file experiments are checked by the course tools; the benchmark is in [`code/ch42/`](code/ch42/) (GCC 14.2 `-O2`, 2-vCPU VM; ratios, not digits).

### Experiment 1 ✅: Copies and moves of four sink designs

```cpp
// @test run -std=c++23 -O2
#include <cstdio>
#include <string>
#include <utility>

struct T {
    static inline int copies = 0, moves = 0;
    T() = default;
    T(const T&) { ++copies; }
    T(T&&) noexcept { ++moves; }
    T& operator=(const T&) { ++copies; return *this; }
    T& operator=(T&&) noexcept { ++moves; return *this; }
    static void reset() { copies = moves = 0; }
};

struct Holder { T t; };

// Four ways to write a "sink" setter (the function stores its argument)
struct ByConstRef { Holder h; void set(const T& x) { h.t = x; } };                            // always copies
struct ByValue    { Holder h; void set(T x) { h.t = std::move(x); } };                         // one move after the by-value init
struct TwoOverloads { Holder h; void set(const T& x) { h.t = x; } void set(T&& x) { h.t = std::move(x); } };
struct Forwarding { Holder h; template <class U> void set(U&& x) { h.t = std::forward<U>(x); } };

template <class S> void row(const char* name) {
    S s;
    T lv;
    T::reset(); s.set(lv);              int c1 = T::copies, m1 = T::moves;
    T::reset(); s.set(std::move(lv));   int c2 = T::copies, m2 = T::moves;
    T::reset(); s.set(T{});             int c3 = T::copies, m3 = T::moves;
    std::printf("%-14s lvalue: %dc %dm   std::move: %dc %dm   temporary: %dc %dm\n", name, c1, m1, c2, m2, c3, m3);
}

int main() {
    row<ByConstRef>("const T&");
    row<ByValue>("T (by value)");
    row<TwoOverloads>("const&, &&");
    row<Forwarding>("T&& (fwd ref)");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
const T&       lvalue: 1c 0m   std::move: 1c 0m   temporary: 1c 0m
T (by value)   lvalue: 1c 1m   std::move: 0c 2m   temporary: 0c 1m
const&, &&     lvalue: 1c 0m   std::move: 0c 1m   temporary: 0c 1m
T&& (fwd ref)  lvalue: 1c 0m   std::move: 0c 1m   temporary: 0c 1m
```

Reading (`c` = copies, `m` = moves):

- **`const T&` alone** copies every time, even from a temporary: right for *reading*, wrong for *sinks*.
- **By value** costs one move more than optimal in each case (lvalue: 1c + 1m; `std::move`: 2m; temporary: 1m with the temporary elided into the parameter). Everything else is the same, with a single overload.
- **`const&` + `&&`** and **a forwarding template** are optimal; the first doubles the overloads (2ⁿ for n sink parameters), the second is unconstrained (Chapter 7 failure modes) and accepts anything convertible to `T`.

> **Opinion.** Default to **by value + `std::move`** for sinks of types that are cheap to move (strings, vectors, smart pointers, most user types): one signature, correct, nearly optimal. Use the **overload pair** in containers, builders, and hot paths where the extra move is measurable; use a **constrained forwarding template** only for genuinely generic code (`emplace`-like). Never use `const T&` for a parameter you intend to copy into a member: it's the version that *looks* efficient and isn't.

### Experiment 2 ✅: A `string_view` that outlives its string

```cpp
// @test crash -std=c++23 -O0 -g -fsanitize=address err=heap-use-after-free
#include <cstdio>
#include <string>
#include <string_view>
std::string make() { return "a long enough string to defeat the small buffer optimisation"; }
std::string_view first_word(std::string_view s) { return s.substr(0, s.find(' ')); }
int main() {
    std::string_view w = first_word(make());      // the temporary std::string dies at the semicolon
    char c = w[0];
    std::printf("%c\n", c);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
=================================================================
==PID==ERROR: AddressSanitizer: heap-use-after-free on address 0xADDR at pc 0xADDR bp 0xADDR sp 0xADDR
READ of size 1 at 0xADDR thread T0
    #0 0xADDR in main snippet.cpp:9

0xADDR is located 0 bytes inside of 61-byte region [0xADDR,0xADDR)
freed by thread T0 here:
    #0 0xADDR in operator delete(void*, unsigned long) ../../../../src/libsanitizer/asan/asan_new_delete.cpp:164
    [... libstdc++ internal frames elided ...]
    #7 0xADDR in main snippet.cpp:8

previously allocated by thread T0 here:
    #0 0xADDR in operator new(unsigned long) ../../../../src/libsanitizer/asan/asan_new_delete.cpp:95
    [... libstdc++ internal frames elided ...]
    #8 0xADDR in make[abi:cxx11]() snippet.cpp:5
    #9 0xADDR in main snippet.cpp:8

SUMMARY: AddressSanitizer: heap-use-after-free snippet.cpp:9 in main
[ASan shadow-memory dump and legend omitted]
==PID==ABORTING
```

`first_word(make())` is a *correct* call (the view is only used inside `first_word` while the temporary is alive) and a *wrong* line: its **result** points into a temporary destroyed at the end of the full-expression. Nothing in the signature said so. Two remarks from the experiment:

- **The first version of this program printed `%.*s` of the dangling view and ASan said nothing.** Reading freed memory through the C library's `printf` was not diagnosed, while the direct `w[0]` read was. A sanitizer-clean run is evidence, not proof (Chapter 28).
- **Design response.** Don't return a `string_view` from a function whose argument is a `string_view` unless it is documented *and* named to say so (`substr`, `first_word_of`); accept `const std::string&` when you *need* the lifetime guarantee in the signature; and when you can, return `std::string` (SSO makes short results free).

### Experiment 3 ✅: Strong types make swapped arguments a compile error

```cpp
// @test fail -std=c++23 err=convert
struct Width  { int v; explicit Width(int x)  : v(x) {} };
struct Height { int v; explicit Height(int x) : v(x) {} };
int area_ints(int w, int h) { return w * h; }
int area(Width w, Height h) { return w.v * h.v; }
int main() {
    area_ints(5, 3);                // fine, and equally fine as area_ints(3, 5): the swap is invisible
    return area(Height{3}, Width{5});   // swapped: does not compile
}
```

GCC: `error: could not convert 'Height(3)' from 'Height' to 'Width'`. The cost is zero at run time (the wrappers are trivially copyable one-`int` structs, passed in registers), and one line of declaration each. `explicit` matters: without it, `area(5, 3)` would compile and the type would document nothing. Use this for units, IDs, indices, handles; reach for a generic `Strong<Tag, T>` template if you have many.

### Experiment 4 ✅: `[[nodiscard]]` and options structs

```cpp
// @test fail -std=c++23 -Werror=unused-result err=nodiscard
#include <optional>
[[nodiscard]] std::optional<int> parse(const char*);
void f() { parse("1"); }          // result silently dropped: the error is never seen
```

```cpp
// @test run -std=c++23
#include <cstdio>
#include <string>
struct ConnectOptions {
    std::string host;
    int         port        = 443;
    bool        tls         = true;
    int         timeout_ms  = 5000;
};
void connect(const ConnectOptions& o) { std::printf("%s:%d tls=%d timeout=%d\n", o.host.c_str(), o.port, o.tls, o.timeout_ms); }
int main() {
    connect({.host = "example.org"});
    connect({.host = "localhost", .port = 8080, .tls = false});
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
example.org:443 tls=1 timeout=5000
localhost:8080 tls=0 timeout=5000
```

`connect(host, 8080, false, 5000)` has two integers and a bool whose meaning must be remembered; the options version names each, takes defaults for the rest, and **adding `.retries = 3` later breaks no caller**, in contrast to a fifth defaulted parameter, which changes the function's type and all function pointers to it. (Designated initializers must follow declaration order; add new members **at the end**.)

### Experiment 5 ✅: What does a `shared_ptr` parameter cost?

`shared_ptr_cost.cpp`: call a `noinline` function that reads one field, passing the same `shared_ptr<Widget>` by value, by `const&`, as `const Widget&`, and as a raw pointer.

```text
BM_value (shared_ptr by value)         7.08 ns     one thread
BM_cref  (const shared_ptr&)           1.08 ns
BM_ref   (const Widget&)               0.92 ns
BM_ptr   (const Widget*)               1.08 ns

BM_value, 2 threads sharing the same shared_ptr      53.0 ns per iteration
BM_cref,  2 threads                                  0.56 ns
```

Passing by value costs **an atomic increment and decrement** per call: ~7× the cost of a reference single-threaded, and **~95× with two threads** hammering the same control block (cache-line ping-pong, Chapter 27). References and raw observers are the same price as each other. The contended figure is the point: shared ownership in an API means "every call crosses a cache line that every other thread is also writing".

> **Opinion.** **Don't put `shared_ptr` in an interface unless the callee genuinely extends the object's lifetime.** A function that merely *uses* the object takes `T&` (or `const T&`); the caller decides how the object is owned. A `std::shared_ptr<T>` parameter says "I may keep this alive", which becomes the unspoken reason for every later "just make it shared" and the cycles that follow (Chapter 25). Pass `const shared_ptr<T>&` only to code that *might* copy it (and then ask why it isn't a by-value sink).

### Experiment 6 ✅: Concepts move the error to the call site

```cpp
// @test fail -std=c++23 err=input_range
#include <ranges>
#include <vector>
template <std::ranges::input_range R>
auto total(R&& r) { long s = 0; for (auto&& x : r) s += x; return s; }

int main() {
    std::vector<int> v{1, 2, 3};
    total(v);
    total(42);               // not a range
}
```

GCC reports `constraints not satisfied` for `total<int>` naming `std::ranges::input_range<int>`, at the call line. Without the constraint the same mistake yields an error inside the `for` loop, pointing at `begin()` of an `int`. A concept is the generic equivalent of a parameter *type*: the signature states what is required.

---

## 8. Assembly / runtime investigation

```bash
# How is each parameter passed? (SysV x86-64) Compare the call sites.
g++-14 -O2 -S -masm=intel -o - shared_ptr_cost.cpp | grep -E 'by_value|by_cref|lock|xadd' | head
#   by_value: a `lock xadd` (atomic add) pair per call, or a call into libstdc++'s refcount helper
#   by_cref : no atomics at all

# Does the sink-by-value design really move?
g++-14 -O2 -fno-elide-constructors -S ...        # (C++17: prvalue elision is mandatory, this only affects the xvalue/pre-17 view)

# Layout and size of API types (Chapter 37)
pahole -C ConnectOptions a.out                   # padding in an options struct
g++-14 -fdump-lang-class -c opts.cpp             # class layout dump

# API breakage detection
abidiff old.so new.so                            # libabigail (Chapter 37)
nm -D --defined-only libfoo.so | c++filt | sort > api.txt   # diff exported symbols between releases

# Does the header compile on its own, with warnings on?
echo '#include "foo.hpp"' | g++-14 -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -fsyntax-only -x c++ -
```

---

## 9. Implementation exercise

Design and implement a small `Config` library with this surface, then attack it:

```cpp
class Config {
public:
    [[nodiscard]] static std::expected<Config, ParseError> parse(std::string_view text);
    [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const;     // valid while *this lives and is unmodified
    template <class T> [[nodiscard]] std::expected<T, ConvertError> get_as(std::string_view key) const;
    void set(std::string key, std::string value);                                       // sink: by value
    ...
};
```

Requirements: `get_as<T>` is constrained to types with `std::from_chars` support or a user `from_string` customisation point; `set` has strong-typed overloads for known keys; there's an options struct for `parse` (`strict`, `allow_comments`); nothing returns a raw owning pointer. Write the **misuse tests**: a file that must *not* compile for each misuse you prevented, and ASan tests for each lifetime hazard you couldn't prevent but documented.

<details>
<summary><strong>Solution sketch</strong></summary>

```cpp
#include <charconv>
#include <concepts>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

enum class ParseError  { missing_equals, empty_key };
enum class ConvertError { missing, bad_format, out_of_range };

struct ParseOptions { bool strict = false; bool allow_comments = true; };      // extensible, named

template <class T>
concept FromChars = requires(const char* b, const char* e, T& v) { std::from_chars(b, e, v); };

class Config {
    std::unordered_map<std::string, std::string> kv_;
public:
    [[nodiscard]] static std::expected<Config, ParseError> parse(std::string_view text, ParseOptions = {});

    [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const {
        auto it = kv_.find(std::string(key));                // (C++20: heterogeneous lookup avoids this allocation)
        if (it == kv_.end()) return std::nullopt;
        return it->second;                                   // view into *this: documented lifetime
    }

    template <FromChars T>
    [[nodiscard]] std::expected<T, ConvertError> get_as(std::string_view key) const {
        auto s = get(key);
        if (!s) return std::unexpected(ConvertError::missing);
        T v{};
        auto [p, ec] = std::from_chars(s->data(), s->data() + s->size(), v);
        if (ec == std::errc::result_out_of_range) return std::unexpected(ConvertError::out_of_range);
        if (ec != std::errc{} || p != s->data() + s->size()) return std::unexpected(ConvertError::bad_format);
        return v;
    }

    void set(std::string key, std::string value) { kv_[std::move(key)] = std::move(value); }   // sink by value
};
```

Misuse tests: `cfg.get_as<std::string>("a")` must fail to compile (constraint `FromChars`); `cfg.get("a");` alone must warn (`[[nodiscard]]`); `Config::parse(...)` result used as `Config` without checking must fail (`expected` is not implicitly convertible); `auto v = Config::parse(f()).value().get("k");` is a dangling-hazard test for ASan (the `Config` temporary dies).

</details>

---

## 10. Real-world example

| Where | Lesson |
|---|---|
| **`std::filesystem`** | `path` is a value; errors are available both ways (`throws` and `error_code&` overloads), a pattern now widely considered over-engineered: two APIs per function |
| **`std::string_view` in the standard** | `string::substr` returns an owning string; `string_view::substr` returns a view: the name is the same and the lifetime contract differs. Hidden in the *type*, not the name |
| **`std::vector<bool>`, `std::initializer_list`** | Cautionary tales: an API that looks like a container but doesn't behave like one; a view type with a lifetime trap (`initializer_list` backing array) |
| **`std::optional`/`expected` in new APIs** | `std::from_chars` returns a result struct instead of throwing or modifying a global (`errno`); `std::chrono` and `std::format` show strong types and compile-time-checked format strings |
| **Qt** | `QString` and `QByteArray` are implicitly shared value types (copy-on-write), so passing `const QString&` is the norm; the implicit conversion macros (`QT_NO_CAST_FROM_ASCII`, `QT_USE_QSTRINGBUILDER`) exist because implicit conversions and hidden allocations were API design mistakes; `QObject*` parameters mean "parent takes ownership" only by *convention*, which is the thing modern ownership types fix |
| **Abseil / Chromium style** | Strong preference for `absl::Span`/`string_view` params, `StatusOr` returns, `absl::Duration` instead of ints, and `Options` structs |

> **Opinion.** **The smallest set of rules that prevents most API bugs:** (1) read → `const T&`/`string_view`/`span`; (2) sink → by value + move; (3) own → `unique_ptr`/value; observe → reference; share → rarely; (4) `[[nodiscard]]` on every function that returns a status, a resource, or a value that is pointless to ignore; (5) `explicit` constructors; strong types for dimensions, IDs, units; (6) options structs instead of `bool` and `int` parameter lists; (7) never return a non-owning type whose lifetime you can't write in one sentence. And the meta-rule: **if the correct use needs a comment, try to put the comment into the type.**

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| **`const T&` for a sink** | Hidden copy per call | By value + `std::move`, or `const&`/`&&` pair |
| **By value for large, non-movable types** (`std::array<char,4096>`) | Copy per call | `const&` |
| **`T&&` parameter used as "efficient const&"** | Caller's object silently gutted, needs `std::move` | `&&` only for sinks with a matching `const&` overload |
| **Returning `string_view`/`span` of a local or temporary** | Use-after-free (Experiment 2) | Return `std::string`/`vector`, or document and test the lifetime |
| **Storing a `string_view` / `const T&` member** | Dangles when the source dies | Own it (`std::string`) unless the class is explicitly a view |
| **`shared_ptr` by value "to be safe"** | Atomic traffic, hidden sharing, cycles (Experiment 5) | `T&`; share only for real shared ownership |
| **Raw `T*` that sometimes owns** | Leaks / double deletes | `unique_ptr<T>` for owning; `T*` strictly non-owning and nullable; `T&` for non-null |
| **Out-parameters (`void f(Result& out)`)** | Callers can't use `const`/`auto`; uninitialized outs | Return a value, `std::pair`/struct, or `expected` |
| **`bool` parameters** | `f(true, false)` is unreadable | Enum class, options struct |
| **Many same-typed parameters** | Swapped-argument bugs | Strong types, options struct |
| **Implicit conversions** (non-`explicit` ctors, `operator bool`) | Surprising overload selection; `if (std::ostream)`-style traps | `explicit`; `explicit operator bool` |
| **Ignoring return values** | Errors lost | `[[nodiscard]]` (and a warning-as-error in CI) |
| **Overloading across numeric types and pointers** | `f(0)` picks the wrong one; `f(nullptr)` ambiguity | One overload set per concept; avoid `int`/`pointer` overload pairs |
| **Adding defaulted parameters to a published function** | Breaks function pointers/ABI; ambiguity with new overloads | Options struct; or add a new named function |
| **Overly generic `template<class T> void f(T)`** | Errors 30 lines deep, accepts anything | Constrain with a concept; `requires` clause |
| **Exceptions *and* error codes for the same failure** (two APIs per function) | Double the surface and tests | Pick one failure model per layer (Chapter 23) |
| **Default arguments in virtual functions** | The default comes from the *static* type | Overload instead |
| **Public data members with invariants** | Invariants cannot be enforced | Private data, member functions; `struct` only for plain aggregates |
| **API that requires call order** (`init()`, `start()`, `stop()`) | Misuse compiles | RAII: construct fully, or a builder that yields a valid object |

---

## 12. Exercises

1. **Counts.** Extend Experiment 1 with two sink parameters and the four designs; compute the overload count for the `const&`/`&&` pair strategy and the copy/move counts for each argument combination.
2. **Audit.** Take a header from a codebase you maintain: for each public function list the four parameter questions (copy/ref, mutable, owning, lifetime). Fix the five worst signatures.
3. **Dangling catalogue.** Write six functions that return or store a non-owning type in a way that dangles (view, span, iterator, reference to a temporary, lambda capturing a local by reference, `c_str()` of a temporary). Which does ASan detect? Which does `-Wdangling-gsl`/`-Wdangling-pointer` detect?
4. **Strong types.** Implement `Strong<Tag, T>` with explicit construction, comparison, and opt-in arithmetic. Show that it adds no overhead (assembly of an `area` function).
5. **Options evolution.** Add three fields to an options struct over three "releases". Show which source patterns keep compiling; which designated-initializer orders break.
6. **Concepts API.** Constrain a `serialize(const T&)` function with a `Serializable` concept and a custom `requires` with two different customization mechanisms (member function vs ADL free function). Compare diagnostics.
7. **Cost of sharing.** Reproduce Experiment 5; add `std::atomic<int>` increments directly and a `std::weak_ptr::lock()` loop, and explain the differences.
8. **Failure-model comparison.** Implement `parse_port(string_view)` with exceptions, `optional`, `expected`, and error codes; compare the call sites for readability and the object code (success path and failure path).

---

## 13. Challenge: design a public API and defend it

Design the public header of a **tiny HTTP client** (`Client`, `Request`, `Response`, headers, timeouts, redirects, TLS options, async completion). Requirements: no raw owning pointers; options structs for everything configurable; strong types for `Url`, `StatusCode`, `Timeout`; `expected`-based errors with a single error enum plus message; a view-returning accessor on `Response` whose lifetime rule you can state in one sentence; extension through concepts, not inheritance. Write the header **before** any implementation, then write ten "bad programs" that must not compile, three that must trigger a warning, and three ASan tests for hazards you accepted. Finally, explain which parts of the header would change if it had to be a stable shared-library ABI (Chapter 37).

---

## 14. Knowledge check

1. What are the four questions every parameter answers?
2. For a sink parameter, compare `const T&`, `T`, `const T&` + `T&&`, and `U&&`: copies and moves for lvalue and temporary arguments.
3. Why is a `string_view` parameter safe but a `string_view` return value risky?
4. What does a `shared_ptr<T>` by value parameter tell the reader, and what does it cost?
5. Why are options structs preferable to additional defaulted parameters?
6. Give three uses of `explicit`.
7. When is `[[nodiscard]]` wrong?
8. What is the lifetime rule for `const T&` bound to a temporary when it is (a) a local, (b) a returned value, (c) a member?
9. How does a concept change the *diagnostic*, not just the acceptance, of a bad call?
10. Which API change is source-compatible but ABI-breaking? Which is the reverse?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Copy or reference? Mutable or const? Owning or observing? How long is it valid?
2. `const T&`: always one copy. By value: lvalue 1c+1m, temporary 1m (elided into the parameter, then moved into the member). `const&`+`&&`: lvalue 1c, temporary 1m. `U&&` (forwarding): same as the pair, but unconstrained.
3. As a parameter it only has to live for the call, which the caller guarantees. As a return value it outlives the call and refers to storage with a lifetime the signature doesn't state.
4. "I may keep this alive/share ownership." It costs an atomic increment/decrement (≈7× a reference single-threaded here, ≈95× under two-thread contention).
5. They are named, can be extended by adding members without changing the function's type, and avoid `bool`/`int` ambiguity.
6. Single-argument constructors (no implicit conversion), conversion operators (`explicit operator bool`), and (C++20) `explicit(bool)` conditional constructors in generic wrappers; also deduction guides.
7. When ignoring the result is normal (e.g. a chained setter returning `*this`, `insert` returns of which most callers use none, functions with side effects where the return is bonus info).
8. (a) Lifetime extended to the reference's scope; (b) not extended: dangles; (c) not extended (except in a few aggregate-init cases): dangles when the constructor binds it.
9. The error is reported at the call, naming the unsatisfied concept, rather than deep inside the template body.
10. Source-compatible but ABI-breaking: adding a data member (or virtual function) to a class, or adding a defaulted parameter. The reverse (ABI-compatible, source-breaking): adding an overload that makes existing calls ambiguous, or changing a default argument: already-compiled callers keep working, recompiling them may fail or behave differently.

</details>

---

[← Previous: Chapter 41](../part-15-performance/41-compiler-optimization.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 43 — Header and library architecture →](43-header-and-library-architecture.md)
