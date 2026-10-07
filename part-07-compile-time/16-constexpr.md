# Chapter 16 — `constexpr`, `consteval`, `constinit`

> **Part VII · Compile-Time C++** &nbsp;|&nbsp; **Level 3–4** &nbsp;|&nbsp; **≈ 6 hours**
> **Prerequisites:** [Chapter 3](../part-02-object-model-and-lifetime/03-lifetime-and-storage.md), [Chapter 8](../part-04-generic-programming/08-templates-deep-dive.md) &nbsp;|&nbsp; **Standards:** C++11 → C++26 (each revision relaxed the rules) &nbsp;|&nbsp; **Tools:** `g++-14`, `nm`, `objdump`

[← Previous: Chapter 15](../part-06-ranges/15-algorithms-and-customization.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 17 — Template metaprogramming →](17-template-metaprogramming.md)

---

**In one sentence:** `constexpr` lets the *ordinary language* run inside the compiler, so computation, tables and validation move from run time to build time without a second "template language".

**By the end of this chapter you can:**

- say exactly when an expression is *required* to be evaluated at compile time versus merely *allowed*
- distinguish `constexpr`, `consteval`, `constinit` and `const` by what each actually guarantees
- write compile-time algorithms and tables using `std::array`, `std::string_view`, and (C++20) `std::vector`/`std::string` with transient allocation
- use `if consteval` / `std::is_constant_evaluated` correctly
- recognise what the compiler *cannot* do at compile time, and the cost of doing too much of it
- decide when `constexpr` is worth it, and when it is cargo cult

---

## 1. Problem

Some values are known before the program runs: lookup tables, hash seeds, parsed configuration, a CRC table, the size of a buffer. C++98 had three ways to get them, all bad:

```cpp
// (a) Macros: no types, no scope, no debugger
#define BUFFER_SIZE (4 * 1024)

// (b) Template metaprogramming: a second, functional language written in template syntax
template <unsigned N> struct Factorial { static const unsigned value = N * Factorial<N-1>::value; };
template <> struct Factorial<0> { static const unsigned value = 1; };

// (c) Compute at start-up: runs every launch, order-of-initialization hazards, not usable as an array bound
static unsigned crc_table[256];
static bool init = (fill_crc_table(crc_table), true);
```

| Approach | Problem |
|---|---|
| Macros | Textual, untyped, no debugging |
| Template recursion | Unreadable; each step is a class instantiation; no loops, no local variables, no `if` |
| Run-time init | Cost on every start; static-initialization-order fiasco; cannot be used where a constant is required (array bound, template argument, `static_assert`) |
| Code generation scripts | Another build step and file to keep in sync |

The real wish: *write the normal function once, and let the compiler run it when its arguments are constants.*

---

## 2. Historical context

| Standard | What `constexpr` could do |
|---|---|
| C++03 | Only "integral constant expressions": literals, `sizeof`, template recursion |
| **C++11** | `constexpr` functions: a **single `return` statement**, no loops, no mutation; recursion instead; literal types only |
| **C++14** | Loops, local variables, mutation of locals, multiple statements; `constexpr` member functions are no longer implicitly `const` |
| **C++17** | `constexpr` lambdas (implicitly constexpr when possible), `if constexpr` (Chapter 8), `constexpr` `std::array` accessors, `std::string_view` |
| **C++20** | `consteval`, `constinit`; `constexpr` **virtual** functions, `try` blocks (no `throw` executed), **`new`/`delete` during constant evaluation** (transient allocation), `std::is_constant_evaluated`, `constexpr` `std::vector`/`std::string`/`<algorithm>`, changing the active member of a `union`, `dynamic_cast`/`typeid` |
| **C++23** | `if consteval`; `constexpr` for non-literal variables and `goto`-free static locals relaxations (P2647, P2448), `constexpr` `std::unique_ptr`, `<cmath>` partially (GCC extension earlier), `static constexpr` locals |
| **C++26** | `static_assert` with a computed message, `constexpr` placement `new`, `constexpr` exceptions (P3068, adopted), `constexpr` `<cmath>`, `std::inplace_vector`; compile-time **reflection** (Chapter 18) builds on all of this |

The trend is monotone: **every revision removes a restriction**, moving the language toward "any function can run at compile time unless it does I/O or touches mutable global state."

---

## 3. Modern solution

Three keywords, three different promises:

```cpp
constexpr int  square(int x) { return x * x; }   // CAN run at compile time (if called in a constant context)
consteval int  cube(int x)   { return x * x * x; } // MUST run at compile time (immediate function)
constinit int  counter = square(7);               // initialization MUST be constant; the variable stays mutable
const int      c = std::rand();                   // runtime-initialized const: NOT a constant
```

| Keyword | On a… | Guarantee |
|---|---|---|
| `constexpr` | variable | The variable is `const` **and** its initializer is a constant expression; usable in constant expressions |
| `constexpr` | function | The function **may** be evaluated at compile time; it may *also* be called at run time |
| `consteval` | function | Every call **must** produce a constant (an *immediate function*); never emitted for run-time use |
| `constinit` | variable with static/thread storage | Initialization is **constant initialization** (no dynamic init, so no init-order fiasco). The variable is **not** const |
| `const` | variable | Read-only after initialization; the initializer can be a run-time value |

---

## 4. Mental model

### The compiler contains an interpreter

```text
        source ──► parser ──► AST ──► ┌──────────────────────────────┐
                                      │  constant evaluator          │  ← an *abstract machine* that executes
                                      │  (an interpreter in the       │    the C++ semantics: stack frames,
                                      │   compiler front end)         │    objects, lifetimes, UB detection
                                      └──────────────┬───────────────┘
                                                     │ results folded into the AST
                                                     ▼
                                   optimizer ──► code generator ──► machine code
```

Because the evaluator tracks **object lifetimes and bounds**, *undefined behaviour is a compile error* in a constant expression. This is an underrated feature: running your function inside `static_assert` is a **UB detector** for that input (Chapter 28).

### "Can" versus "must": the constant-evaluated context

```text
   constexpr int f(int);

   int a = f(3);                 // NOT a constant context → compiler MAY fold it (optimizer), or call f at run time
   constexpr int b = f(3);       // constant context → MUST be evaluated now (or error)
   int arr[f(3)];                // constant context (array bound)
   static_assert(f(3) == 9);     // constant context
   template <int N> struct S;  S<f(3)> s;   // constant context (template argument)
```

> **Crucial misconception.** `constexpr` on a *function* does **not** mean "evaluated at compile time". It means *eligible*. The call site decides. If you want a *guarantee*, put the call in a constant context (`constexpr` variable) or make the function `consteval`.

### What is allowed to happen "in the evaluator"

```text
   Allowed                                       Not allowed (until relaxed)
   ─────────────────────────────────────────     ────────────────────────────────────────
   arithmetic, loops, branches, recursion        I/O, system calls, `std::cout`
   local variables, mutation of locals           reading/writing non-constexpr globals
   objects of literal types, constexpr ctors     `reinterpret_cast`, pointer ↔ integer casts
   new/delete — if everything is freed           `throw` that is *executed* (C++26 relaxes)
   `std::vector`, `std::string` (transient)      `goto` into scope, `asm`
   virtual calls, dynamic_cast, typeid           uninitialized reads, out-of-range access, signed overflow (= UB → error)
   union member switching (C++20)                memory that *escapes* the evaluation (no non-transient allocation)
```

**Transient allocation** is the key C++20 rule: memory allocated *during* a constant evaluation must be released *before it ends*. So `std::vector<int>` works inside a `constexpr` function, but a `constexpr std::vector<int> v = {1,2,3};` *variable* does not (its storage would have to survive into run time). The standard workaround is to compute with a vector and **copy the result into a `std::array`** whose size you also computed at compile time (§7, Experiment 3).

---

## 5. Language rules

### 5.1 Constant expressions  `[expr.const]`

An expression is a **core constant expression** if evaluating it by the abstract machine does not hit any of: UB, an uninitialized read, a call to a non-`constexpr` function, a `throw`, an `asm`, a modification of an object whose lifetime began outside the evaluation, a `reinterpret_cast`, a comparison of unrelated pointers, `typeid` on an incomplete/unknown dynamic type, etc.

A **constant expression** is a core constant expression whose *result* is also acceptable: it must be a *permitted result* (no pointers/references to temporaries or to objects with automatic storage; for a pointer, it must point to an object of static storage, a function, or one-past-the-end).

### 5.2 `constexpr` functions  `[dcl.constexpr]`

A function may be `constexpr` if (C++23 wording, abridged):

- return type and parameter types are *literal types* (C++23 dropped this: the *call* just must not need a non-literal object in a constant evaluation)
- it contains no `goto`, no non-literal-typed variable definition that is *evaluated* (since C++23 even these are allowed if not evaluated), no `asm`
- **at least one set of arguments** exists for which a call is a constant expression: otherwise it is *ill-formed, no diagnostic required* (until C++23, which relaxed this: P2448R2)
- **`constexpr` is implicit** for: lambdas that qualify, defaulted special members that qualify, and functions that are *immediate*; **not** implicit for ordinary functions or member functions (you must write it)
- a `constexpr` function is implicitly `inline`

### 5.3 `consteval` (immediate functions)  `[expr.const]/13`

- Every *potentially-evaluated* call must be a constant expression, else a compile error
- Not emitted as code (no symbol), so you cannot take its address *outside* a constant expression
- It **propagates**: a `constexpr` function cannot call a `consteval` function with a non-constant argument, which is why generic helper templates can hit surprising errors
- Constructors may be `consteval`: that forces *every construction* to be compile-time (the standard `std::format` uses it to **check format strings at compile time**, `std::format_string`)

### 5.4 `constinit`  `[dcl.constinit]`

Applies only to variables with **static or thread** storage duration. It asserts "this is constant-initialized" *without* making the variable `const`. Combine with `thread_local` to avoid the hidden "initialized-yet?" guard that dynamic thread-local initialization needs (a **measurable** saving: see Experiment 6).

### 5.5 `if consteval` and `std::is_constant_evaluated()`

```cpp
constexpr double my_sqrt(double x) {
    if consteval { return newton_sqrt(x); }         // C++23: taken only during constant evaluation
    else         { return __builtin_sqrt(x); }      // run-time path: may use hardware / non-constexpr code
}
```

| | `std::is_constant_evaluated()` (C++20) | `if consteval` (C++23) |
|---|---|---|
| Is a… | function returning `bool` | statement form |
| Pitfall | `if constexpr (std::is_constant_evaluated())` is **always true** (the condition is itself a constant context) | None: designed to fix that |
| Can call `consteval` functions in the taken branch | no | **yes** |

Use the two-path pattern *only* when the constant-time and run-time implementations really must differ (e.g. a hardware intrinsic). The two paths **must compute the same result**; the compiler cannot check that.

### 5.6 Literal types and `constexpr` classes

A *literal type* can be created in a constant expression: scalar, reference, array of literals, or a class with a `constexpr` constructor, a trivial (C++20: constexpr) destructor, and literal members. A `constexpr` destructor (C++20) means classes such as `std::vector` can be used transiently.

### 5.7 `static_assert` and `constexpr` as testing

A `static_assert(expr)` is evaluated by the constant evaluator, so a *test* of a `constexpr` function becomes **part of the build**, and the evaluator additionally checks for UB on the exercised paths.

### Layer check

| Layer | Decides |
|---|---|
| **Standard** | What counts as a constant expression; the can/must rules; transient allocation; which library functions are `constexpr` |
| **Compiler** | Evaluator limits (`-fconstexpr-ops-limit`, `-fconstexpr-loop-limit`, `-fconstexpr-depth` in GCC; `-fconstexpr-steps` in Clang); **compile time and memory** of evaluation; whether the *optimizer* folds a non-constant-context call (always allowed, never required) |
| **ABI** | A `constexpr` function is `inline`: **weak symbol** in each TU that odr-uses it; a `consteval` function has **no symbol**; a `constexpr` *variable* with internal linkage by default (`const` implies internal linkage in C++; `inline constexpr` for headers) |
| **CPU** | Nothing at run time: a table computed at compile time sits in `.rodata`; the *run-time* cost is the same loads you would write by hand |

---

## 6. Implementation model

### How GCC and Clang evaluate

Both compilers walk the AST (GCC's `constexpr.cc`; Clang's `ExprConstant.cpp`) with a **tree-walking interpreter**. Consequences:

- It is **orders of magnitude slower than compiled code** (Experiment 5 measured about 5.6 µs per simple loop iteration), and *memory grows with call depth and live objects*.
- Loops are bounded by counters (GCC: `-fconstexpr-loop-limit`, `-fconstexpr-ops-limit`, `-fconstexpr-depth`; Clang: `-fconstexpr-steps`; defaults vary by version, so check `--help` for yours). Exceeding them is a hard error.
- GCC memoizes some `constexpr` call results (`-fconstexpr-cache-depth`); Clang does not do so in the same way. Don't rely on either: naive exponential recursion is still a build-time hazard (Experiment 5).

### Where do compile-time values live?

```text
   constexpr std::array<int,256> table = make_table();
        │
        ▼  evaluator result is a constant aggregate
   .rodata  (read-only data section), emitted as raw bytes → no code, no start-up cost
```

For an array like this, `objdump -s -j .rodata` shows the table's bytes **in the object file**. A use such as `table[i]` is a plain load. If the optimizer can see `i` is constant it folds the load too.

> **Caveat (compiler behaviour, not standard).** If a `constexpr` variable is used only as a *value* (`table[3]`), the compiler may never emit the array at all. If it is *odr-used* (its address taken, indexed with a non-constant), it is emitted exactly once for a `static constexpr` / `inline constexpr`, or once **per translation unit** for a namespace-scope `constexpr` (internal linkage): a hidden code-size cost for big tables in headers: use `inline constexpr`.

---

## 7. Experiments

### Experiment 1: "Can" versus "must": observing *when* a `constexpr` function runs

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <type_traits>

constexpr int where_am_i(int x) {
    if consteval { return x * 1000; }      // marker: constant evaluation
    else         { return x; }              // marker: run time
}

consteval int must_be_now(int x) { return x + 1; }

int main() {
    constexpr int a = where_am_i(1);        // constant context  -> 1000
    int           b = where_am_i(2);        // NOT a constant context. -O0: not folded.
    const int     c = where_am_i(3);        // const int: the compiler MAY treat this initializer as a constant expression
    int           n = 4;
    int           d = where_am_i(n);        // non-constant argument -> run time

    std::printf("constexpr var : %d   (forced compile time)\n", a);
    std::printf("plain int     : %d   (ran at run time under -O0)\n", b);
    std::printf("const int     : %d   (GCC evaluated this one at compile time: 3000 = the consteval branch)\n", c);
    std::printf("non-const arg : %d\n", d);

    std::printf("consteval     : %d\n", must_be_now(41));
    // int m = 5; must_be_now(m);       // ERROR: argument is not a constant expression

    // std::is_constant_evaluated in a declaration with a const int:
    int arr[where_am_i(0) + 3];             // array bound: constant context -> where_am_i(0) = 0
    std::printf("array of %zu\n", sizeof(arr) / sizeof(arr[0]));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
constexpr var : 1000   (forced compile time)
plain int     : 2   (ran at run time under -O0)
const int     : 3000   (the call is run time; `c` is a constant only if the initializer *is* a constant expression)
non-const arg : 4
consteval     : 42
array of 3
```

Read the second and third lines side by side. `int b = where_am_i(2);` took the **run-time** branch (2), while `const int c = where_am_i(3);` took the **compile-time** branch (3000). Both are the same call shape; the difference is that a `const` integer variable *whose initializer is a constant expression* is itself usable in constant expressions, so the standard makes the initializer a *manifestly constant-evaluated* context, whereas a plain `int` initializer is only *eligible* for folding by the optimizer (which at `-O0` doesn't happen). The practical lesson is the one from §4: don't memorize such corners; write `constexpr int c = ...;` when you need the guarantee, and `consteval` when you need it for every call.

### Experiment 2: The evaluator as a UB detector

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <array>

constexpr int sum_first(const std::array<int, 4>& a, int n) {
    int s = 0;
    for (int i = 0; i < n; ++i) s += a[i];      // a[4] would be out of bounds
    return s;
}

constexpr int signed_wrap(int x) { return x + 1; }

int main() {
    constexpr std::array<int, 4> a{1, 2, 3, 4};
    static_assert(sum_first(a, 4) == 10);            // fine
    // static_assert(sum_first(a, 5) == 0);          // ERROR: array subscript out of bounds (reads a[4])
    // static_assert(signed_wrap(2147483647) == 0);  // ERROR: signed integer overflow
    // constexpr int z = 1 / 0;                      // ERROR: division by zero
    // constexpr int u = []{ int x; return x; }();   // ERROR: read of an uninitialized variable (C++20: even indeterminate)

    std::printf("sum_first(a,4) = %d (verified at compile time, and UB-free on this input)\n", sum_first(a, 4));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sum_first(a,4) = 10 (verified at compile time, and UB-free on this input)
```

Uncomment each commented line **one at a time** and read the diagnostic: this is the cheapest UB detector in the toolchain, with *zero* false positives (the compiler is executing your code, not guessing). Its limitation is that it only checks the inputs you feed it. We return to this in Chapter 28.

### Experiment 3: Compile-time tables: from a `std::vector` to a `std::array`

The classic C++20 pattern: compute with ordinary containers (transient allocation), then **freeze the result** into a fixed-size array so it survives into run time.

```cpp
// @test run -std=c++23 -O0
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

// Sieve of Eratosthenes using std::vector<bool>-free logic, at compile time.
constexpr std::vector<int> primes_below(int n) {
    std::vector<char> composite(n + 1, 0);
    std::vector<int> out;
    for (int i = 2; i <= n; ++i) {
        if (composite[i]) continue;
        out.push_back(i);
        for (long long j = 1LL * i * i; j <= n; j += i) composite[j] = 1;
    }
    return out;                                  // transient: must not escape as a constexpr *variable*
}

constexpr std::size_t prime_count(int n) { return primes_below(n).size(); }      // run the algorithm just to measure

template <int N>
constexpr auto make_prime_table() {
    std::array<int, prime_count(N)> t{};          // size is itself computed at compile time
    auto v = primes_below(N);
    std::copy(v.begin(), v.end(), t.begin());
    return t;
}

// CRC-32 table computed by an ordinary loop: the same code you would run at start-up.
constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}
inline constexpr auto crc_table = make_crc_table();

constexpr std::uint32_t crc32(const char* s, std::size_t n) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) c = crc_table[(c ^ static_cast<unsigned char>(s[i])) & 0xFF] ^ (c >> 8);
    return ~c;
}

int main() {
    constexpr auto primes = make_prime_table<50>();
    static_assert(primes.size() == 15 && primes.back() == 47);
    std::printf("primes below 50 (%zu): ", primes.size());
    for (int p : primes) std::printf("%d ", p);
    std::puts("");

    // The classic check value: CRC-32("123456789") == 0xCBF43926
    static_assert(crc32("123456789", 9) == 0xCBF43926u);
    std::printf("crc32(\"123456789\") = %08X  (checked by static_assert)\n", crc32("123456789", 9));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
primes below 50 (15): 2 3 5 7 11 13 17 19 23 29 31 37 41 43 47 
crc32("123456789") = CBF43926  (checked by static_assert)
```

Two things to note. First, `constexpr std::vector<int> v = primes_below(50);` as a **variable** would be an error (non-transient allocation): the table is `std::array`, whose size was computed by calling the algorithm *twice* (once to count, once to fill). That "run it twice" pattern is the standard workaround until `std::define_static_array` (C++26, reflection-era, not in GCC 14) can promote a transient range into static storage. Second, the CRC table is the *same code* you'd have run at program start; the only change is `constexpr` and `inline`.

### Experiment 4: Where does the table live? (assembly and object file)

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fcf-protection=none filter=crc_byte,use_runtime
#include <array>
#include <cstdint>

constexpr std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}
constexpr auto table = make_table();

std::uint32_t crc_byte(std::uint32_t crc, unsigned char b) { return table[(crc ^ b) & 0xFF] ^ (crc >> 8); }
std::uint32_t const_idx() { return table[3]; }                  // constant index: should fold to an immediate
std::uint32_t use_runtime(std::uint32_t c, unsigned char b) { return crc_byte(c, b); }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
crc_byte(unsigned int, unsigned char):
	xor	esi, edi
	lea	rax, table[rip]
	shr	edi, 8
	movzx	esi, sil
	xor	edi, DWORD PTR [rax+rsi*4]
	mov	eax, edi
	ret

use_runtime(unsigned int, unsigned char):
	xor	esi, edi
	lea	rax, table[rip]
	shr	edi, 8
	movzx	esi, sil
	xor	edi, DWORD PTR [rax+rsi*4]
	mov	eax, edi
	ret
```

`crc_byte` is a load from a table symbol plus a shift and an xor: **no loop computing the table, no initialization code**. Verify the data really is in the object file:

```bash
g++-14 -std=c++23 -O2 -c crc.cpp -o crc.o
objdump -s -j .rodata crc.o | head -5         # first bytes: 00000000 96300777 2c610eee ... (little-endian 0x00000000, 0x77073096, 0xEE0E612C); .rodata is 0x400 = 1024 bytes
nm -C crc.o | grep -i table                   # prints `r table`: a local read-only symbol, internal linkage because `constexpr` at namespace scope is `const`
```

### Experiment 5: Compile-time cost is real

Two probes (shell, not auto-verified; numbers measured on one machine with GCC 14.2, wall-clock, `-c`):

```bash
# (a) a simple counting loop evaluated in static_assert
cat > lp.cpp <<'EOF'
constexpr unsigned long long loop(unsigned n){ unsigned long long s=0; for(unsigned i=0;i<n;++i) s+=i^(s>>3); return s; }
static_assert(loop(NN) > 0);
int main(){}
EOF
for n in 100000 1000000 10000000; do time g++-14 -std=c++23 -fconstexpr-loop-limit=2000000000 -fconstexpr-ops-limit=2000000000 -DNN=$n -c lp.cpp -o /dev/null; done

# (b) naive recursive Fibonacci
cat > ct.cpp <<'EOF'
constexpr unsigned long long fib(unsigned n) { return n < 2 ? n : fib(n-1) + fib(n-2); }
static_assert(fib(NN) > 0);
int main() {}
EOF
for n in 20 29 32 36; do time g++-14 -std=c++23 -DNN=$n -c ct.cpp -o /dev/null; done
```

| Probe | Result (GCC 14.2) |
|---|---|
| loop, n = 10⁵ | 0.34 s |
| loop, n = 10⁶ | 5.6 s (≈ **5.6 µs per iteration**, roughly three orders of magnitude slower than compiled code) |
| loop, n = 10⁷ | 66 s |
| loop, n = 10⁶ with **default** limits | **error**: "non-constant condition" (the ops limit was hit) |
| fib(20) / fib(29) / fib(32) | 0.02 s / 0.18 s / 0.74 s |
| fib(36), default limits | **error** (limit hit), though fib(36) is only ≈ 4·10⁷ calls |

The shape matters more than the constants: the evaluator is an interpreter, an iteration costs *microseconds*, and the compiler's safety limits are **tight on purpose**. Raising them (`-fconstexpr-ops-limit`, `-fconstexpr-loop-limit`) trades a build error for a build that takes minutes. GCC also caches `constexpr` call results (`-fconstexpr-cache-depth`), which is why the naive Fibonacci grows more slowly than its run-time counterpart would; do not count on that in other compilers. Rule: *write constexpr code with the same algorithmic care as run-time code, then budget a factor of roughly 1000 over native speed for anything with a hot loop.*

### Experiment 6: `constinit` and the hidden thread-local guard

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fcf-protection=none filter=get_dyn,get_ci
int compute() noexcept;                         // not constexpr: forces dynamic initialization

thread_local int tl_dynamic = compute();        // needs a "has this thread initialized it?" guard on every access
constinit thread_local int tl_constinit = 7;    // constant-initialized: plain TLS load, no guard

int get_dyn() { return tl_dynamic; }
int get_ci()  { return tl_constinit; }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
get_dyn():
	cmp	BYTE PTR fs:__tls_guard@tpoff, 0
	je	.L2
	mov	eax, DWORD PTR fs:tl_dynamic@tpoff
	ret
.L2:
	sub	rsp, 8
	mov	BYTE PTR fs:__tls_guard@tpoff, 1
	call	compute()@PLT
	mov	DWORD PTR fs:tl_dynamic@tpoff, eax
	add	rsp, 8
	ret

get_ci():
	mov	eax, DWORD PTR fs:tl_constinit@tpoff
	ret
```

Compare the two functions. Reading the *dynamically initialized* thread-local compiles to a **guard check** (`cmp BYTE PTR fs:__tls_guard, 0`), a branch, and an out-of-line initialization path that calls `compute()` on first use; the `constinit` one is a single `fs:`-relative load. (Within the defining TU GCC inlines the guard; from another TU, GCC reaches such variables through a TLS wrapper function.) This is the single most practical use of `constinit`: *telling the compiler there is no dynamic initialization, so it need not check*. (The guard mechanism is an implementation detail of the Itanium ABI / compiler, not mandated by the standard.)

### Experiment 7: `consteval` for compile-time validation (a checked format string)

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <string_view>

// A tiny "format string" type whose constructor is consteval: every use is checked at build time.
struct CheckedFmt {
    std::string_view s;
    consteval CheckedFmt(const char* str) : s(str) {
        int open = 0;
        for (char c : s) {
            if (c == '{') ++open;
            else if (c == '}') { if (open == 0) throw "unmatched }"; --open; }
        }
        if (open != 0) throw "unmatched {";                 // `throw` in a consteval ctor = a compile error with this text
    }
};

void log(CheckedFmt f, int v) { std::printf("[%.*s] %d\n", int(f.s.size()), f.s.data(), v); }

int main() {
    log("value {x}", 42);               // OK, checked at compile time
    // log("value {x", 42);             // ERROR at build time: "unmatched {" (throw evaluated in a constant expression)
    // const char* bad = "{"; log(bad, 1);  // ERROR: not a constant expression at all
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
[value {x}] 42
```

This is how `std::format` rejects `std::format("{:d}", "text")` *during compilation*. The technique (*a `consteval` constructor taking a string literal*) is the portable replacement for macros that parsed strings. In C++26 the error message can be a computed string via `static_assert(cond, message_expr)`.

### Experiment 8: Where `constexpr` quietly stops: a catalogue of failures

```cpp
// @test run -std=c++23 -O0
#include <cstdio>
#include <string>
#include <vector>

constexpr std::size_t transient_ok() {            // OK: vector and string die before the evaluation ends
    std::vector<std::string> v;
    v.emplace_back("alpha"); v.emplace_back("beta"); v.emplace_back("gamma");
    // NOTE: `std::vector<std::string> v{"alpha","beta","gamma"};` is rejected by GCC 14.2 in a constant
    // expression (copying strings out of the initializer_list trips its SSO union handling). A library/compiler
    // limitation, not a language rule; emplace_back works. Re-test on newer GCC/Clang.
    std::size_t total = 0;
    for (auto& s : v) total += s.size();
    return total;
}
static_assert(transient_ok() == 14);

// constexpr std::vector<int> bad = {1, 2, 3};      // ERROR: "is not a constant expression because it refers to a non-transient allocation"
// constexpr int *p = new int(3);                    // ERROR: same (allocation outlives the evaluation)
// constexpr int g(const int *p) { return *reinterpret_cast<const char*>(p); }   // ERROR: reinterpret_cast
// int global = 3; constexpr int h() { return global; }     // function OK, but any constant-context call is an ERROR: reads non-const global

int main() {
    std::printf("transient total = %zu (computed at compile time)\n", transient_ok());
    std::puts("Uncomment each commented line to see the exact diagnostic.");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
transient total = 14 (computed at compile time)
Uncomment each commented line to see the exact diagnostic.
```

---

## 8. Assembly / runtime investigation

Experiments 4 and 6 are the investigation. Three more checks to run on your own code:

```bash
# (1) Was my "compile-time" call actually folded?  A call instruction to the function means no.
g++-14 -std=c++23 -O0 -S -masm=intel -o - prog.cpp | grep -E "call.*my_func"     # at -O0 a non-constant-context call stays
g++-14 -std=c++23 -O2 -S -masm=intel -o - prog.cpp | grep -E "call.*my_func"     # at -O2 the optimizer usually folds it

# (2) Which symbols exist?   consteval functions have none; constexpr functions that are odr-used have weak ones (W)
nm -C prog.o | grep my_func

# (3) How big did my table make the object?
size prog.o ; objdump -h prog.o | grep -E "rodata|data"
```

**Interpretation rule.** `constexpr` guarantees nothing about codegen unless the call is in a *constant context*. Whether `int x = f(3);` becomes `mov eax, 9` is the **optimizer's** decision (it almost always folds at `-O1+`, rarely at `-O0`). If you *need* the guarantee, make it `constexpr`/`consteval`.

---

## 9. Implementation exercise

Implement these as `constexpr`/`consteval` code and verify with `static_assert`:

1. **`constexpr_string_view_find`**, **`constexpr_split`** (return a `std::array<std::string_view, N>` where `N` is computed by a first pass), and a **compile-time INI/CSV header parser** that turns `"id,name,age"` into a `std::array<std::string_view, 3>`.
2. **`constexpr_sort`** (a constexpr quicksort or heapsort on `std::array`; compare with `std::sort`, which is `constexpr` since C++20), then a **perfect-hash** builder for a fixed keyword set (search a seed such that all keys map to distinct slots).
3. **A `consteval` literal-checker**: `"2024-02-30"_date` fails to compile because February has no 30th; return a `constexpr` `Date` struct (user-defined literal with a `consteval` operator).
4. **A compile-time state machine**: describe a regex-like pattern as a string, compile it into a `constexpr` transition table in `std::array<std::array<int,256>,N>`, and match input at run time with a table-driven loop. State the limits that make this impractical for large patterns (evaluator step limits, compile time).
5. **Compile-time unit test harness**: a `constexpr` `Test` that returns `bool`, collected with `static_assert(all(test1(), test2(), …))`. Show it catching a UB case (out-of-range subscript) that a run-time test would miss.

<details>
<summary><strong>Solution sketch for item 3 (checked date literal)</strong></summary>

```cpp
// @test run -std=c++23 -O0
#include <cstdio>

struct Date { int y, m, d; };

consteval Date make_date(int y, int m, int d) {
    constexpr int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (m < 1 || m > 12) throw "month out of range";
    int max = dim[m - 1] + (m == 2 && leap);
    if (d < 1 || d > max) throw "day out of range";
    return {y, m, d};
}

consteval Date operator""_date(const char* s, unsigned long) {
    auto num = [&](int from, int len) { int v = 0; for (int i = 0; i < len; ++i) v = v * 10 + (s[from + i] - '0'); return v; };
    return make_date(num(0, 4), num(5, 2), num(8, 2));              // "YYYY-MM-DD"
}

int main() {
    constexpr Date ok = "2024-02-29"_date;                          // leap day: accepted
    std::printf("%04d-%02d-%02d\n", ok.y, ok.m, ok.d);
    // constexpr Date bad = "2023-02-29"_date;                     // ERROR at build time: day out of range
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
2024-02-29
```

The `throw` is never *executed* in a valid program; in a constant evaluation, **reaching it makes the expression non-constant**, which for a `consteval` call is a hard error, with the string visible in the diagnostic on GCC and Clang.

</details>

---

## 10. Real-world example

| Where | Use |
|---|---|
| **`std::format`** | `consteval` `basic_format_string` constructor validates format specifiers at compile time |
| **fmt, spdlog** | Compile-time format checking (`FMT_STRING`, `FMT_CONSTEVAL`) |
| **CRC / hash tables, lookup tables** | Replace generated `.inc` files and start-up initializers |
| **Embedded** | Pin tables to `.rodata`; `constinit` globals avoid static-init order and run-time init code |
| **Parsers / DSLs** | CTRE (compile-time regular expressions), Boost.Hana-style tuples, `frozen` (constexpr containers: perfect-hash `unordered_map`) |
| **Qt** | `QLatin1StringView`, `QStringLiteral`, `qHash` constexpr; the `Q_DECLARE_*` families use `constexpr` queries; moc output is still generated code (until reflection) |
| **Compiler/OS code** | `constinit` for per-thread and global singletons, e.g. `thread_local` allocator caches |

> **Opinion.** `constexpr` *by default* on small pure functions and types is cheap and good: it costs nothing, lets the function be used in constant contexts, and enables `static_assert` tests. **Do not** bend a design around "everything at compile time": heavy compile-time computation moves cost from run time into *every developer's every build*. A 20 ms run-time init is usually cheaper than a 2 s increase of a header that 400 TUs include. Use compile-time computation for **tables and validation**, not for **business logic**. And **always prefer `consteval` or a `constexpr` variable if you actually need the guarantee**: a `constexpr` function that "happens to be folded" is a performance *hope*, not a contract.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Assuming `constexpr f(x)` runs at compile time | Run-time cost at `-O0`, or inconsistent folding | `constexpr` variable or `consteval` |
| `if constexpr (std::is_constant_evaluated())` | Always true | `if consteval` (C++23) or a plain `if` |
| Two branches of `if consteval` computing different results | Behaviour differs between compile time and run time | Test both with the same inputs: `static_assert(f(x) == runtime_f(x))` |
| `constexpr std::vector`/`string` **variable** | "non-transient allocation" | Compute with vectors, store in `std::array` or use `std::string_view` to literals |
| Namespace-scope `constexpr` big table in a header | A copy per TU (internal linkage) | `inline constexpr` |
| Exponential constexpr recursion | Build takes minutes, evaluator limit error | Iterate; memoize; raise limits only as a last resort |
| Forgetting `constexpr` on a member used in a constant context | Error "call to non-constexpr function" | Mark member `constexpr` (C++14+: not implicitly `const`) |
| Reading a non-`constexpr` global | "not usable in a constant expression" | Pass as a parameter |
| `constexpr` function that can *never* be constant (pre-C++23) | Ill-formed NDR | Remove `constexpr`, or restructure |
| Depending on a compiler extension in constant evaluation (`__builtin_*`, `<cmath>` before C++26) | Works on GCC, fails on Clang/MSVC | Standard facilities, or `if consteval` fallbacks |
| `consteval` helpers called from generic code with run-time arguments | Hard compile error, far from the cause | Provide a `constexpr` version and a thin `consteval` wrapper |
| Using `constinit` on a non-static variable | Ill-formed | Applies to static/thread storage only |
| Believing `const int x = f()` is a constant | Only if `f()` is a constant expression *and* the type is const integral | `constexpr int x = f();` |

---

## 12. Exercises

1. **Experiment 1 across compilers.** Run it with GCC and Clang at `-O0` and `-O2`. Tabulate which lines are "run time" vs "compile time". Explain the differences using the standard text: what is *required* versus *allowed*?
2. **Evaluator limits.** Find the exact loop-iteration limit in GCC by writing a counting loop in `constexpr`; raise it with `-fconstexpr-loop-limit` and `-fconstexpr-ops-limit`. Do the same in Clang with `-fconstexpr-steps`. What are the defaults?
3. **UB zoo.** Write ten `constexpr` functions that each commit one kind of UB (overflow, out-of-bounds, null dereference, uninitialized read, dangling reference, shift too large, division by zero, accessing inactive union member, use after destroy, modifying a `const` object). For each, record whether `static_assert` catches it on GCC and on Clang.
4. **Table placement.** Compare object size and `nm` output for `constexpr` vs `inline constexpr` vs `static constexpr` tables of 64 KiB, included from 3 TUs. Which gives 1 copy and which gives 3?
5. **`constinit`.** Measure the cost of `thread_local` with and without `constinit` in a hot loop (Experiment 6, plus a micro-benchmark). Is the difference visible?
6. **Compile-time vs run-time.** Compute a 65 536-entry sine table with a `constexpr` series (Taylor/CORDIC) and with a start-up loop. Measure build time, start-up time and binary size.
7. **Two paths, one answer.** Implement a `constexpr` `popcount` with an `if consteval` branch (portable loop) and a run-time branch (`__builtin_popcountll`). Write an exhaustive-for-32-bit property test proving equal results.
8. **Frozen map.** Implement a `constexpr` perfect-hash `Map<K, V, N>` whose construction sorts keys at compile time and whose lookup is a binary search; compare with `std::map` and `std::unordered_map` for N = 8, 64, 1024.

---

## 13. Challenge: a compile-time command-line parser

Design `constexpr_cli<Spec>`: given a *specification* string (or a `constexpr` array of option descriptors) such as `"-v,--verbose:flag; -o,--output:string; -n:int=4"`, produce:

- a `constexpr` option table, with errors in the spec reported at **build time** (duplicate short options, malformed default values) via a `consteval` constructor
- a run-time `parse(argc, argv)` that is **table-driven** and allocation-free, filling a generated `struct` of typed fields (use `std::tuple` of field types computed from the spec)
- a `constexpr` `usage()` generator producing the help text as a `std::array<char, N>` where `N` is computed in a first pass
- `static_assert`-based unit tests for both the *spec* and *parse* logic (parse can be tested at compile time by passing a `constexpr` argument array)
- a measurement of compile-time cost against the number of options (2, 16, 128) and a comparison to a macro/code-generation approach

---

## 14. Knowledge check

1. Is a `constexpr` function guaranteed to be evaluated at compile time? What gives a guarantee?
2. Name the four contexts that *force* constant evaluation.
3. What does `constinit` guarantee, and why is the variable not `const`?
4. Why can a `constexpr` function use `std::vector` but a `constexpr` variable not be a `std::vector`?
5. What problem does `if consteval` solve that `std::is_constant_evaluated()` has?
6. Why does a `consteval` function have no symbol in the object file?
7. What does it mean that the constant evaluator "detects UB", and what are its limits?
8. What is the linkage of `constexpr int table[N]` at namespace scope, and why does it matter for headers?
9. Give two cases in which making something `constexpr` is a bad idea.
10. In `consteval Date operator""_date(...)`, why is `throw` acceptable, and what happens if it is reached?
11. Which layer decides evaluator step limits: the standard, the compiler, or the ABI?
12. What is the difference between `const int x = f();` and `constexpr int x = f();`?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. No: only *eligible*. A call is guaranteed compile-time if it is in a constant context (a `constexpr`/`constinit` initializer, array bound, template argument, `static_assert`, `if constexpr` condition) or the function is `consteval`.
2. `constexpr` variable initializers, template non-type arguments, array bounds (and `static_assert`/`if constexpr` conditions, `case` labels, `noexcept(...)`/`explicit(...)` operands, bit-field widths); anything required to be a *constant expression*.
3. That the variable has **constant initialization** (no dynamic initializer, no init-order issues, no TLS guard). It is not `const` because it is meant to be modified at run time.
4. Memory allocated during constant evaluation must be freed before it ends (transient allocation); a variable's storage would persist into run time, so the allocation would escape.
5. `if constexpr (is_constant_evaluated())` is always true because the condition is itself a constant context; `if consteval` is a real statement that selects the branch correctly and also lets the taken branch call `consteval` functions.
6. It is an *immediate function*: all calls must be constant-evaluated and are replaced by their value; nothing needs to exist at run time.
7. The evaluator follows the abstract machine and rejects any operation with UB (out-of-bounds, overflow, uninitialized read, …) as "not a constant expression". Limits: it checks only the executed paths for the supplied inputs, and compilers may differ in diagnostics.
8. Internal (it's implicitly `const`); each TU that odr-uses it gets its own copy. Use `inline constexpr` (C++17) for one shared definition.
9. Heavy computation that slows every build (business logic, large regex compilers); designs bent to keep everything `constexpr` (e.g. avoiding I/O logging or necessary dynamic state); also when the "compile-time" and run-time branches of `if consteval` would diverge.
10. A `throw` that is never *evaluated* is fine in constant evaluation; if evaluation reaches it, the expression is not a constant, which makes a `consteval` call ill-formed, so the compiler reports an error (usually quoting the thrown text).
11. The compiler (a quality-of-implementation limit; the standard only recommends minimum limits in an annex).
12. `constexpr` *requires* a constant expression and the variable is usable in constant expressions; `const int x = f();` may be initialized at run time and is usable in constant expressions only when its initializer *was* a constant expression and the type is const-qualified integral/enumeration.

</details>

---

[← Previous: Chapter 15](../part-06-ranges/15-algorithms-and-customization.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 17 — Template metaprogramming →](17-template-metaprogramming.md)
