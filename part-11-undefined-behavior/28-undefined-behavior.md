# Chapter 28 — Undefined Behavior

> **Part XI · Undefined Behavior** &nbsp;|&nbsp; **Level 4** (compiler/runtime) &nbsp;|&nbsp; **≈ 8 hours**
> **Prerequisites:** [Chapter 3](../part-02-object-model-and-lifetime/03-lifetime-and-storage.md), [Chapter 24](../part-10-memory/24-dynamic-memory.md), [Chapter 27](../part-10-memory/27-cache-and-data-oriented-cpp.md) &nbsp;|&nbsp; **Standards:** all; **C++20** (signed integers are two's complement), **C++23** (`std::unreachable`, `[[assume]]`), **C++26** (*erroneous behaviour* for uninitialised reads, hardened standard library; see §5.7 for status) &nbsp;|&nbsp; **Tools:** `g++-14`, `clang++-18`, ASan, UBSan, TSan, Valgrind, `clang-tidy`

[← Previous: Chapter 27](../part-10-memory/27-cache-and-data-oriented-cpp.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 29 — C++ threading →](../part-12-concurrency/29-threading.md)

---

**In one sentence:** undefined behaviour is not "a bug that crashes"; it is a **contract between you and the optimiser**, in which you promise certain things never happen and the compiler is entitled to generate code *assuming it is true*, so a violated promise can corrupt code far from the violation, in a way that varies with compiler, flags and unrelated edits.

**By the end of this chapter you can:**

- distinguish *undefined*, *unspecified*, *implementation-defined* and (C++26) *erroneous* behaviour, and cite which applies
- explain, with generated code, how and why compilers exploit UB
- recognise the ~15 UB patterns that account for most real bugs, and the defined alternative to each
- run ASan, UBSan, TSan, Valgrind, hardened-library checks and compiler diagnostics, and read their reports
- design code and CI so UB is detected mechanically instead of by code review

---

## 1. Problem

C and C++ were designed to run on every machine from microcontrollers to supercomputers with maximum efficiency and no mandatory runtime checks. A check costs time; most programs never need it. So the language says, in effect: *"if you do X, we make no promises about what happens next, and we won't spend cycles guarding against it."*

```cpp
int f(int* p) {
    int v = *p;          // (1) dereference
    if (p == nullptr)    // (2) the programmer's "defensive" check
        return -1;
    return v;
}
```

Written by a careful programmer, this looks safe. Compiled with `-O2`, the null check is **deleted**: (1) already promised `p` is non-null, otherwise the program would have undefined behaviour, and the compiler may assume that never happens. The "bug" is not at the dereference (which might even succeed on your OS); it is that the *check* silently vanished. This is what separates UB from other bugs: **you cannot reason about what the program does by reading the source alone**.

Why does the language accept this? Because the same rule lets the compiler remove bounds checks it can prove redundant, keep loop counters in registers, vectorise loops, and reorder memory accesses. UB is the price of the speed, and understanding it precisely is how you keep the speed without paying the price.

---

## 2. Historical context

| Year | Event |
|---|---|
| 1972–89 | C: "UB" as the standard's escape hatch for non-portable and error-prone constructs; compilers are mostly literal |
| 1989 | ANSI C defines the categories (undefined, unspecified, implementation-defined) that C++98 inherits |
| 1990s | Optimisers get aggressive: strict aliasing (type-based alias analysis) allows reordering loads/stores of different types |
| 2000s | Signed-overflow and null-check optimisations start breaking old code. The Linux kernel adds `-fno-strict-aliasing`, `-fwrapv`, `-fno-delete-null-pointer-checks` |
| 2008 | Wang et al., *Towards Optimization-Safe Systems* (STACK). Regehr's blog popularises "UB as optimisation licence" |
| 2011 | C++11 memory model: data races are UB; threads get a formal basis (Chapter 30) |
| 2011–14 | ASan (Google), UBSan, TSan, MSan become practical; GCC/Clang ship them |
| 2017 | C++17 fixes many evaluation-order cases (`a[i] = i++` is no longer UB; function-call argument order stays *unspecified*) |
| 2020 | C++20: signed integers are two's complement (no more "representation" UB), but **overflow remains UB**; `std::bit_cast`, `std::is_constant_evaluated` |
| 2023 | C++23 `std::unreachable`, `[[assume]]` expose the optimiser contract explicitly; "Profiles" and safety discussions begin in earnest |
| **2024–26** | **Erroneous behaviour** (P2795) and **hardened standard library** (P3471) adopted for C++26; contracts (P2900) too. Compiler/library support is still arriving (GCC 14.2 and libstdc++ 14 do not implement erroneous behaviour; use `-ftrivial-auto-var-init`). Check current status before relying on it |

---

## 3. Modern solution

There is no single mechanism; the modern answer is **layered defence**:

```text
   1. Design out UB:     value types, RAII, span/string_view carefully, std::variant, checked arithmetic, no raw new/delete
   2. Make it compile-time error:   constexpr evaluation, `consteval`, concepts, [[nodiscard]], -Werror=…
   3. Warn statically:   -Wall -Wextra -Wconversion -Wnull-dereference -Wdangling-pointer, clang-tidy, -fanalyzer
   4. Check dynamically in testing:   ASan, UBSan, TSan, MSan, Valgrind, hardened library (_GLIBCXX_ASSERTIONS), fuzzing
   5. Harden in production:   -D_GLIBCXX_ASSERTIONS, -fstack-protector-strong, -fsanitize=… minimal runtime, CFI, -ftrivial-auto-var-init
```

> The correct stance is not "avoid UB by being careful" (nobody is that careful over a million lines) but "make UB *detectable by machines*, and *rare by construction*."

---

## 4. Mental model

### The optimiser's contract

```text
   You write           ┌───────────────────────────────┐        The compiler may assume
   source code    ───► │ the "abstract machine" program │ ───►   no UB path is ever executed
                       └───────────────────────────────┘
        valid executions = {all paths that never hit UB}
        the compiler picks ANY machine code that behaves correctly on valid executions
        on an invalid execution it owes you nothing: not a crash, not a message, not a consistent result
```

Two consequences people find surprising:

1. **UB is retroactive ("time travel").** If a path *will* reach UB, the compiler may transform code *before* that point, because executions that reach the UB are not "valid", so nothing constrains their earlier behaviour either. (The standard allows this: UB is permitted "to have unpredictable results … even affecting behaviour before the operation.")
2. **UB is compile-time knowledge, not run-time events.** The compiler does not *detect* UB; it *assumes its absence* to derive facts: "`p` is non-null", "`x + 1 > x`", "this loop terminates", "this pointer doesn't alias that one".

### The four kinds of "not fully specified"

| Category | The standard says | Example | Program valid? |
|---|---|---|---|
| **Well-defined** | One result | `unsigned` overflow wraps | yes |
| **Implementation-defined** | Result varies; **implementation must document** it | `sizeof(long)`, `char` signedness, `>>` of a negative `int` (arithmetic shift in practice; defined since C++20) | yes, not portable |
| **Unspecified** | A set of allowed results; need not be documented or consistent | Order of evaluation of function arguments; layout of padding | yes, but don't depend on it |
| **Undefined** | **No requirements** at all | signed overflow, out-of-bounds access, data race, null dereference, dangling reference | **no** — the program has no meaning |
| *(C++26)* **Erroneous** | A defined-but-wrong result, with permission for the implementation to diagnose | Reading an uninitialised automatic variable | yes, but it is a bug, and tools may flag it |

Memorise the test: *"Could two correct compilers legitimately print different things?"* → unspecified/implementation-defined. *"Could one of them legitimately do anything at all, including nothing?"* → undefined.

### Where UB comes from, by family

```text
   MEMORY            out-of-bounds, use-after-free, double free, dangling reference, uninitialised read, misaligned access,
                     object-lifetime violations (use of an object before its lifetime begins or after it ends)
   TYPE / ALIASING   accessing an object through an incompatible glvalue (strict aliasing), invalid downcast,
                     union type-punning (UB in C++; defined in C), calling a function through the wrong pointer type
   ARITHMETIC        signed overflow, division by zero, shift by ≥ width or negative, INT_MIN / -1, float→int out of range
   CONCURRENCY       data races (unsynchronised conflicting accesses to a non-atomic memory location)
   PROGRAM STRUCTURE ODR violations, infinite loops without side effects (C++ forward-progress), missing return in a non-void function,
                     modifying a const object, calling virtual functions during construction (defined, but surprising)
   LIBRARY           violated preconditions: dereferencing end(), invalid iterator use, std::move'd-from usage assumptions,
                     comparator that isn't a strict weak ordering, mismatched new/delete
```

---

## 5. Language rules

### 5.1 What the standard says  `[intro.abstract]`, `[defns.undefined]`

- *Undefined behavior*: "behavior for which this document imposes no requirements."
- A conforming program may not execute UB for **any input it is given**. A program that has UB on *some* input is still ill-behaved *for that input* — but the optimiser may exploit it everywhere that input is possible.
- **Constant evaluation is UB-free by rule**: a constant expression that would invoke UB is *not* a constant expression ("ill-formed", diagnosed). This makes `constexpr` the best UB detector available (Experiment 7).

### 5.2 Catalogue: the UB you will actually meet, and its defined alternative

| UB | Example | Why compilers exploit it | Defined alternative |
|---|---|---|---|
| Signed integer overflow | `x + 1 > x` | Loop counters and indices kept un-wrapped; strength reduction | `unsigned`, `__builtin_*_overflow`, C++26 `std::add_sat`, wider type, `-fwrapv` (extension) |
| Out-of-bounds access | `v[v.size()]`, `a[10]` of `int[10]` | Bounds are assumed; loads can be hoisted | `.at()`, `std::span` with checks, hardened library, ranges |
| Null dereference | `*p` then `if (!p)` | Dereference proves non-null | `T&`, `not_null`, check *before* use |
| Use after free / dangling | `string_view` of a temporary | Storage reused | Ownership types (Chapter 25), sanitizers |
| Uninitialised read | `int x; return x;` | Value can be anything, or *different at each use* | Initialise; `= {}`; `-ftrivial-auto-var-init=zero` |
| Strict-aliasing violation | `*(int*)&f` | Accesses of different types don't alias, so loads/stores reorder | `std::memcpy`, `std::bit_cast` |
| Shift ≥ width, negative shift | `1 << 32` | Maps to hardware shift whose result varies | Mask or `if`, `std::rotl` |
| Division by zero / `INT_MIN / -1` | `a / b` | Hardware traps (`idiv`) or the compiler assumes `b ≠ 0` | Check; use unsigned |
| Data race | two threads write `int` | Values cached in registers; stores reordered | `std::atomic`, `std::mutex` (Chapters 29–31) |
| Invalid iterator / pointer arithmetic | `*it` after `push_back`; `p + n` beyond one-past-end | Iterators are assumed valid | Index-based access, `reserve`, re-acquire iterators |
| Missing `return` in a non-void function | `int f(int x){ if (x) return 1; }` | Falling off the end is UB, so the compiler removes the path | `-Wreturn-type -Werror` |
| Infinite loop with no side effects | `while (1) {}` in a thread | Forward progress guarantee (C++ only; C has an exception for constant conditions); loop removed | atomic/IO/volatile in the loop, `std::this_thread::yield` |
| Modifying a `const` object | `const_cast` and write | Constants live in read-only memory or are folded | Don't |
| Invalid downcast | `static_cast<Derived*>(basePtr)` to wrong type | Vtable assumed | `dynamic_cast`, `std::variant`, `visit` |
| `memcpy`/`memset` on non-trivial type | `memcpy(&string, …)` | Lifetime/invariants violated | Copy constructors, `std::copy`, `trivially_copyable` constraint |
| Overlapping `memcpy`; null to `memcpy` | `memcpy(p, nullptr, 0)` | `nonnull` attributes | `memmove`; check |
| Comparator violating strict weak ordering | `std::sort` with `<=` | Algorithms assume irreflexivity | `<`, test comparators |
| ODR violation | Two different definitions of `inline` function in different TUs | Inlining picks one | One definition; `-Wodr` with LTO |

### 5.3 Pointer provenance and lifetime  (C++ object model, Chapter 3)

A pointer is not just an address: it carries *provenance* — the object it was derived from. Arithmetic past one-past-the-end, or converting an address back into a pointer to a *different* object, is UB even if the numeric address is right. Likewise, an object's lifetime must begin (construction) before access and has not ended (destruction or storage reuse). `std::launder`, `std::start_lifetime_as` (C++23, Chapter 3/24) and `std::construct_at` exist to state these facts to the compiler.

### 5.4 Unspecified and implementation-defined: what you may rely on

| Question | Status | What you may rely on |
|---|---|---|
| Argument evaluation order in `f(a(), b())` | unspecified (indeterminately sequenced since C++17) | Each is evaluated completely before the next starts; order may vary between compilers (Experiment 1) |
| `a[i] = i++` | UB before C++17; **defined in C++17** (the right operand of `=` is sequenced before the left) | `[expr.ass]` |
| `i = i++ + 1` | Defined since C++11 (assignment is sequenced after both value computations) | — |
| `f(i++, i++)`, `i++ + i++` | C++14: UB (unsequenced side effects). C++17: argument evaluations are indeterminately sequenced (no UB for `f(i++, i++)`), but `i++ + i++` remains **UB** (operands of `+` are unsequenced) | GCC 14 still warns with `-Wsequence-point` for `f(i++, i++)` even in C++17 (conservative) |
| `char` signedness, `sizeof(long)`, size of `int` | implementation-defined | Document it; use `<cstdint>` |
| Right shift of negative `int` | implementation-defined **before** C++20; **defined (arithmetic)** since C++20 | — |
| Padding bytes and their values | unspecified | Don't compare structs with `memcmp` |
| `std::sort` of equal elements | unspecified order | Use `stable_sort` when order matters |
| Address of two unrelated objects compared with `<` | unspecified (use `std::less`) | `std::less<T*>` gives a total order |

### 5.5 Erroneous behaviour (C++26)  🟡

P2795 ("Erroneous behaviour for uninitialized reads") makes reading an uninitialised automatic variable **not UB** but *erroneous*: the object holds a defined (implementation-chosen, "erroneous") value, the implementation is *encouraged to diagnose*, and the optimiser can no longer assume the read never happens. `[[indeterminate]]` opts out for performance (`int buf[4096] [[indeterminate]];`). **Status:** adopted into the C++26 draft; library/compiler support is arriving. GCC 14.2 has no support; the existing flag `-ftrivial-auto-var-init=zero|pattern` (GCC ≥ 12, Clang ≥ 8) gives similar behaviour today. Treat this as 🟡 until your compiler's documentation says otherwise.

### 5.6 What UB is *not*

- **Not** "what happens on this machine". "It works on x86" is the classic way UB survives until a compiler upgrade.
- **Not** necessarily a crash. The most dangerous UB produces plausible wrong output.
- **Not** limited to the instruction that triggers it (time travel, §4).
- **Not** the same as a bug that the sanitizer can't see: sanitizers detect *some* UB at *runtime on executed paths*.

### 5.7 Standard-library hardening  🟡 / 🔧

| Mechanism | What it checks | Cost | Availability |
|---|---|---|---|
| `-D_GLIBCXX_ASSERTIONS` (libstdc++) | `operator[]` on `vector`/`array`/`string`/`span`, `front()/back()` on empty, `optional`/`expected` access, `unique_ptr[]`... aborts on violation | Small (a compare per access) | ✅ GCC ≥ 4.x; cheap enough for production |
| `-D_GLIBCXX_DEBUG` | Full checked containers (iterator validity) | Large; changes ABI | ✅ debug builds only |
| `_LIBCPP_HARDENING_MODE` (libc++) | `fast`, `extensive`, `debug` hardening levels | Varies | ✅ Clang/libc++ ≥ 18 |
| C++26 standard hardening (P3471) | Standardises "hardened implementation" preconditions as contract violations | — | 🟡 adopted for C++26; implementations in progress |

### Layer check

| Layer | Question | Answer |
|---|---|---|
| **C++ standard** | What is UB? | Exactly the constructs the standard marks undefined; no guarantees whatsoever |
| **Compiler** | What does GCC/Clang *do* with it? | Assume absence; GCC and Clang differ in which UB they exploit and how aggressively (`-fno-delete-null-pointer-checks`, `-fwrapv`, `-fno-strict-aliasing`, `-fno-strict-overflow` are the opt-outs); optimisation level changes behaviour |
| **ABI** | Does the platform define anything? | The ABI can *define* some things the language doesn't (e.g. `int` representation, `sizeof`), but ABI-defined ≠ language-defined: the compiler still assumes the language rule |
| **OS** | What happens when it traps? | Linux delivers `SIGSEGV`/`SIGFPE`/`SIGILL`; page protections make null dereference (page 0) fault, but a *constant-folded* null dereference may never reach hardware |
| **CPU** | What does the hardware do? | x86 `idiv` traps on overflow/zero; shifts mask the count (mod 32/64); loads from unmapped addresses fault; but compilers don't generate the instruction you imagined once UB is assumed absent |

---

## 6. Implementation model

### How optimisers use UB (three mechanisms)

1. **Fact derivation (value-range and nullness analysis).** From `*p`, derive `p != 0`; from `x + 1` on `int`, derive `x ≠ INT_MAX`; from `a[i]` with `int a[10]`, derive `0 ≤ i < 10`. Later branches that contradict these facts are dead code and removed (§1; Experiment 2).
2. **Alias analysis.** Strict aliasing says an `int` lvalue and a `float` lvalue never refer to the same object, so a store through `float*` cannot change what an `int*` reads. The compiler reorders and caches loads accordingly (Experiment 3).
3. **Path pruning (`__builtin_unreachable`).** A path that definitely reaches UB (falling off a non-void function, `std::unreachable()`, a `[[assume(false)]]`) is treated as never taken; the code that leads *only* there is deleted, which can make a function with a missing `return` fall into the next function's body.

### What the machine code looks like when it goes wrong

- A `switch` whose `default` was deleted jumps through a jump table with an out-of-range index.
- A function missing a `return` runs on into whatever code follows it in the binary.
- A loop `for (int i = 0; i <= n; ++i)` with `n = INT_MAX` is compiled as infinite or as a wider-index loop.
- A "defensive" null check disappears; the dereference faults (or doesn't) in a different place.

### How sanitizers work

| Tool | Mechanism | Detects | Typical slowdown | Memory |
|---|---|---|---|---|
| **ASan** (`-fsanitize=address`) | Shadow memory (1 shadow byte per 8 app bytes), red zones around heap/stack/globals, quarantine of freed memory, compiler-inserted checks before loads/stores | Heap/stack/global buffer overflow, use-after-free, use-after-scope, double free, leaks (LSan); stack-use-after-return (opt-in) | ~2× | ~2–3× |
| **UBSan** (`-fsanitize=undefined`) | Compiler inserts checks at specific operations | Signed overflow, shifts, bad enum/bool values, misaligned/null access, `vptr` bad downcast, `unreachable` reached, VLA bounds, float-cast overflow | ~1.2× | — |
| **TSan** (`-fsanitize=thread`) | Happens-before tracking with vector clocks and shadow memory | Data races, lock-order inversions, some atomic misuse | 5–15× | 5–10× |
| **MSan** (Clang `-fsanitize=memory`) | Shadow bits for "initialised?" | Reads of uninitialised memory (needs everything instrumented) | ~3× | ~2× |
| **Valgrind/Memcheck** | Dynamic binary translation; tracks every byte | Uninitialised reads, invalid reads/writes, leaks; no recompilation | 10–50× | 2× |

Rules of the road: ASan and TSan **cannot be combined**; ASan+UBSan can. MSan and ASan cannot be combined. Sanitizers see only executed paths: they need **tests that reach the bug**, which is why fuzzing (`-fsanitize=fuzzer`, AFL++) pairs with them.

---

## 7. Experiments

### Experiment 1 ✅: Unspecified and implementation-defined are *not* UB, but they differ per compiler

```cpp
// @test run -std=c++23 -O0
#include <climits>
#include <cstdio>
#include <type_traits>

int a() { std::puts("  a() evaluated"); return 1; }
int b() { std::puts("  b() evaluated"); return 2; }
int f(int x, int y) { return x * 10 + y; }

int main() {
    std::puts("unspecified: the order of evaluating arguments of f(a(), b())");
    std::printf("  result %d\n", f(a(), b()));

    std::puts("implementation-defined (documented by the compiler, varies by platform):");
    std::printf("  sizeof(long) = %zu, sizeof(int) = %zu, char is %s, wchar_t is %zu bytes\n", sizeof(long), sizeof(int),
                std::is_signed_v<char> ? "signed" : "unsigned", sizeof(wchar_t));
    std::printf("  -8 >> 1 = %d  (arithmetic shift; defined for negative values since C++20)\n", -8 >> 1);
    std::printf("  CHAR_BIT = %d, INT_MAX = %d\n", CHAR_BIT, INT_MAX);

    std::puts("well-defined: unsigned wraparound");
    unsigned u = 0u; --u;
    std::printf("  0u - 1 = %u\n", u);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
unspecified: the order of evaluating arguments of f(a(), b())
  b() evaluated
  a() evaluated
  result 12
implementation-defined (documented by the compiler, varies by platform):
  sizeof(long) = 8, sizeof(int) = 4, char is signed, wchar_t is 4 bytes
  -8 >> 1 = -4  (arithmetic shift; defined for negative values since C++20)
  CHAR_BIT = 8, INT_MAX = 2147483647
well-defined: unsigned wraparound
  0u - 1 = 4294967295
```

On this machine GCC evaluates `b()` before `a()`. **Clang 18 on the same code prints `a()` first** (hand-run: `clang++-18 -std=c++23`). Both are correct. Code that depends on either is a latent bug; `-Wsequence-point` and clang-tidy's `bugprone-*` checks catch the worst cases, but not this one (it is not UB, merely unspecified).

### Experiment 2 🔧: Watch the optimiser use UB: assembly

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fcf-protection=none filter=signed_always_true,unsigned_check,null_after_deref,first_or_zero
// Signed overflow is UB: the comparison folds to "true".
bool signed_always_true(int x)   { return x + 1 > x; }
// Unsigned overflow wraps: the comparison is genuinely evaluated.
bool unsigned_check(unsigned x)  { return x + 1 > x; }
// Dereference proves non-null: the later null check is dead code.
int null_after_deref(int* p) {
    int v = *p;
    if (p == nullptr) return -1;
    return v;
}
// Control: a correctly written bound check is contradicted by nothing, so it survives.
int table[4] = {10, 20, 30, 40};
int first_or_zero(int i) { return i < 4 ? table[i] : 0; }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
signed_always_true(int):
	mov	eax, 1
	ret

unsigned_check(unsigned int):
	cmp	edi, -1
	setne	al
	ret

null_after_deref(int*):
	mov	eax, DWORD PTR [rdi]
	ret

first_or_zero(int):
	xor	eax, eax
	cmp	edi, 3
	jg	.L5
	movsx	rdi, edi
	lea	rax, table[rip]
	mov	eax, DWORD PTR [rax+rdi*4]
.L5:
	ret
```

Read the first two functions side by side. `signed_always_true` is `mov eax, 1; ret`: **the comparison no longer exists**. `unsigned_check` still compares (`x + 1 > x` is false exactly when `x == UINT_MAX`). The same source with a different type is a different program, because one type has defined overflow and the other does not. In `null_after_deref` the `-1` branch is gone: the function loads `*p` and returns it. (`first_or_zero` is the control: the compare-and-branch is still there, because nothing in the function contradicts it. The optimiser only deletes checks that UB has already "answered".)

Now the same signed function with the opt-out that tells the compiler overflow *is* defined:

```cpp
// @test asm -std=c++23 -O2 -fwrapv -fno-stack-protector -fcf-protection=none filter=signed_wraps
bool signed_wraps(int x) { return x + 1 > x; }     // identical source, compiled with -fwrapv
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
signed_wraps(int):
	cmp	edi, 2147483647
	setne	al
	ret
```

With `-fwrapv` (a GCC/Clang extension, **not** standard C++), the check is real again. This is why the Linux kernel builds with it. The standard-conforming fixes are: use `unsigned`, use `__builtin_add_overflow`/`std::add_sat`, or test *before* the operation (`x < INT_MAX`).

### Experiment 3 🔧: Strict aliasing: the optimiser reorders what you wrote

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fcf-protection=none filter=alias_int_float,punned_memcpy,punned_bitcast
#include <bit>
#include <cstdint>
#include <cstring>

// UB-based: stores through float* are assumed not to modify an int. Result can be the constant 1.
int alias_int_float(int* i, float* f) {
    *i = 1;
    *f = 2.0f;            // cannot alias *i under strict aliasing
    return *i;            // compiler returns 1 without reloading
}

// Defined ways to look at a float's bytes:
std::uint32_t punned_memcpy(float f) { std::uint32_t u; std::memcpy(&u, &f, sizeof u); return u; }
std::uint32_t punned_bitcast(float f) { return std::bit_cast<std::uint32_t>(f); }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
alias_int_float(int*, float*):
	mov	DWORD PTR [rdi], 1
	mov	eax, 1
	mov	DWORD PTR [rsi], 0x40000000
	ret

punned_memcpy(float):
	movd	eax, xmm0
	ret

punned_bitcast(float):
	movd	eax, xmm0
	ret
```

`alias_int_float` returns the literal `1` after both stores, so if a caller passes pointers to the same storage (`reinterpret_cast<float*>(&some_int)`), the program prints a stale value: UB made visible. `memcpy` and `std::bit_cast` both compile to a single `movd` (the register move); the "cast the pointer" version buys nothing and costs correctness. **Rule: to reinterpret bytes, `bit_cast` (C++20) or `memcpy`; never a pointer cast.** (`char*`/`unsigned char*`/`std::byte*` may alias anything; that is the one standard exception, and it is for *inspecting* bytes.)

### Experiment 4 ✅: Memory errors under AddressSanitizer

Each snippet is compiled with `-fsanitize=address -g` and **must** abort. The report below each is real output (paths/addresses normalised).

```cpp
// @test crash -std=c++23 -O0 -g -fsanitize=address err=heap-buffer-overflow
#include <vector>
int main() {
    std::vector<int> v(3);
    return v[5];              // operator[] has no bounds check: reads 8 bytes past the 12-byte buffer
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
=================================================================
==PID==ERROR: AddressSanitizer: heap-buffer-overflow on address 0xADDR at pc 0xADDR bp 0xADDR sp 0xADDR
READ of size 4 at 0xADDR thread T0
    #0 0xADDR in main snippet.cpp:5

0xADDR is located 8 bytes after 12-byte region [0xADDR,0xADDR)
allocated by thread T0 here:
    #0 0xADDR in operator new(unsigned long) ../../../../src/libsanitizer/asan/asan_new_delete.cpp:95
    [... libstdc++ internal frames elided ...]
    #8 0xADDR in main snippet.cpp:4

SUMMARY: AddressSanitizer: heap-buffer-overflow snippet.cpp:5 in main
[ASan shadow-memory dump and legend omitted]
==PID==ABORTING
```

```cpp
// @test crash -std=c++23 -O0 -g -fsanitize=address err=heap-use-after-free
#include <cstdio>
#include <vector>
int main() {
    std::vector<int> v = {1, 2, 3};
    int& first = v[0];        // reference into the buffer
    v.push_back(4);           // reallocation: the buffer is freed, `first` dangles
    std::printf("%d\n", first);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
=================================================================
==PID==ERROR: AddressSanitizer: heap-use-after-free on address 0xADDR at pc 0xADDR bp 0xADDR sp 0xADDR
READ of size 4 at 0xADDR thread T0
    #0 0xADDR in main snippet.cpp:8

0xADDR is located 0 bytes inside of 12-byte region [0xADDR,0xADDR)
freed by thread T0 here:
    #0 0xADDR in operator delete(void*, unsigned long) ../../../../src/libsanitizer/asan/asan_new_delete.cpp:164
    [... libstdc++ internal frames elided ...]
    #8 0xADDR in main snippet.cpp:7

previously allocated by thread T0 here:
    #0 0xADDR in operator new(unsigned long) ../../../../src/libsanitizer/asan/asan_new_delete.cpp:95
    [... libstdc++ internal frames elided ...]
    #7 0xADDR in main snippet.cpp:5

SUMMARY: AddressSanitizer: heap-use-after-free snippet.cpp:8 in main
[ASan shadow-memory dump and legend omitted]
==PID==ABORTING
```

```cpp
// @test crash -std=c++23 -O0 -g -fsanitize=address err=(heap|stack)-use-after
#include <cstdio>
#include <string>
#include <string_view>

std::string make() { return std::string(64, 'x'); }     // long enough to need the heap
int main() {
    std::string_view sv = make();    // the temporary string is destroyed at the end of the full expression
    std::printf("%c\n", sv[0]);      // sv dangles
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
=================================================================
==PID==ERROR: AddressSanitizer: heap-use-after-free on address 0xADDR at pc 0xADDR bp 0xADDR sp 0xADDR
READ of size 1 at 0xADDR thread T0
    #0 0xADDR in main snippet.cpp:9

0xADDR is located 0 bytes inside of 65-byte region [0xADDR,0xADDR)
freed by thread T0 here:
    #0 0xADDR in operator delete(void*, unsigned long) ../../../../src/libsanitizer/asan/asan_new_delete.cpp:164
    [... libstdc++ internal frames elided ...]
    #7 0xADDR in main snippet.cpp:8

previously allocated by thread T0 here:
    #0 0xADDR in operator new(unsigned long) ../../../../src/libsanitizer/asan/asan_new_delete.cpp:95
    [... libstdc++ internal frames elided ...]
    #8 0xADDR in make[abi:cxx11]() snippet.cpp:6
    #9 0xADDR in main snippet.cpp:8

SUMMARY: AddressSanitizer: heap-use-after-free snippet.cpp:9 in main
[ASan shadow-memory dump and legend omitted]
==PID==ABORTING
```

```cpp
// @test crash -std=c++23 -O0 -g -fsanitize=address err=stack-use-after-scope
#include <cstdio>
int main() {
    int* p;
    {
        int x = 42;
        p = &x;
    }                          // x's lifetime ends here, though the stack slot is still there
    std::printf("%d\n", *p);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
=================================================================
==PID==ERROR: AddressSanitizer: stack-use-after-scope on address 0xADDR at pc 0xADDR bp 0xADDR sp 0xADDR
READ of size 4 at 0xADDR thread T0
    #0 0xADDR in main snippet.cpp:9

Address 0xADDR is located in stack of thread T0 at offset 32 in frame
    #0 0xADDR in main snippet.cpp:3

  This frame has 1 object(s):
    [32, 36) 'x' (line 6) <== Memory access at offset 32 is inside this variable
HINT: this may be a false positive if your program uses some custom stack unwind mechanism, swapcontext or vfork
      (longjmp and C++ exceptions *are* supported)
SUMMARY: AddressSanitizer: stack-use-after-scope snippet.cpp:9 in main
[ASan shadow-memory dump and legend omitted]
==PID==ABORTING
```

These four cover most memory-safety bugs. The reports tell you **what** (read/write, size, kind), **where** (stack trace at the access), **and who freed/allocated it** (second and third stacks). That causality is why ASan is the first tool to reach for. Note what it *cannot* do: it finds only accesses that actually execute, and misses reads inside the same object's padding, or out-of-bounds within one allocation (`a.x[10]` landing in `a.y`).

### Experiment 5 ✅: Arithmetic and type UB under UBSan

```cpp
// @test crash -std=c++23 -O0 -g -fsanitize=undefined err=runtime.error
#include <climits>
#include <cstdio>
#include <cstdlib>

struct Base { virtual ~Base() = default; };
struct Derived : Base { int extra = 7; };

int main(int argc, char**) {
    int big = INT_MAX;
    int a = big + argc;                         // signed overflow (argc is 1)
    std::printf("overflow     -> %d\n", a);

    int sh = 33;
    int b = 1 << (sh - 1 + argc);               // shift by 33 >= width of int
    std::printf("shift        -> %d\n", b);

    int zero = argc - 1;
    // (division by zero would trap; shown in the report if enabled)  int c = 10 / zero;
    (void)zero;

    int* np = argc > 5 ? &a : nullptr;
    int d = np ? *np : 0;                       // guarded, not UB
    (void)d;

    alignas(8) char buf[16] = {};
    int* mis = reinterpret_cast<int*>(buf + 1); // misaligned int*
    *mis = 5;                                   // misaligned store

    Base base;
    Derived* bad = static_cast<Derived*>(&base);   // invalid downcast (the object is not a Derived)
    std::printf("bad downcast -> %d\n", bad->extra);

    int arr[3] = {1, 2, 3};
    std::printf("oob          -> %d\n", arr[argc + 2]);   // index 3 of int[3]

    std::abort();                               // UBSan recovers by default and continues: make the demo end with a non-zero exit
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
snippet.cpp:11:17: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'
snippet.cpp:15:15: runtime error: shift exponent 33 is too large for 32-bit type 'int'
snippet.cpp:28:10: runtime error: store to misaligned address 0xADDR for type 'int', which requires 4 byte alignment
0xADDR: note: pointer points here
 00 00 00  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  f0 7a 9a ae fe 7f 00 00  00 2f 8c b4 4f
snippet.cpp:31:20: runtime error: downcast of address 0xADDR which does not point to an object of type 'Derived'
0xADDR: note: object is of type 'Base'
 00 00 00 00  f8 ac 65 58 0e 56 00 00  00 00 00 00 00 00 00 00  41 7a 9a ae fe 7f 00 00  00 00 00 00
              vptr for 'Base'
snippet.cpp:32:46: runtime error: member access within address 0xADDR which does not point to an object of type 'Derived'
0xADDR: note: object is of type 'Base'
 00 00 00 00  f8 ac 65 58 0e 56 00 00  00 00 00 00 00 00 00 00  41 7a 9a ae fe 7f 00 00  10 7a 9a ae
              vptr for 'Base'
snippet.cpp:35:53: runtime error: index 3 out of bounds for type 'int [3]'
snippet.cpp:35:16: runtime error: load of address 0xADDR with insufficient space for an object of type 'int'
0xADDR: note: pointer points here
 03 00 00 00  00 05 00 00 00 00 00 00  00 00 00 00 00 00 00 00  f0 7a 9a ae fe 7f 00 00  00 2f 8c b4
```

UBSan prints one line per violation and **continues** by default (`-fno-sanitize-recover=all` aborts at the first). The checks you most want in CI: `signed-integer-overflow`, `shift`, `null`, `alignment`, `bounds`, `vptr`, `unreachable`, `return` (missing return), `float-cast-overflow`, `enum`, `bool`. GCC's `-fsanitize=undefined` enables most; Clang adds `-fsanitize=integer` (including *unsigned* overflow, which is defined but often a bug) and `-fsanitize=nullability`.

### Experiment 6 ✅: Data race under ThreadSanitizer

```cpp
// @test crash -std=c++23 -O1 -g -fsanitize=thread link=-pthread err=data.race
#include <cstdio>
#include <thread>

int counter = 0;                          // plain int: not atomic, not protected

int main() {
    std::thread t([] { for (int i = 0; i < 1000; ++i) ++counter; });
    for (int i = 0; i < 1000; ++i) ++counter;   // concurrent unsynchronised increments: a data race => UB
    t.join();
    std::printf("counter = %d (never trust this number)\n", counter);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
counter = 2000 (never trust this number)
==================
WARNING: ThreadSanitizer: data race (pid=N)
  Read of size 4 at 0xADDR by thread T1:
    #0 operator() snippet.cpp:8
    [... libstdc++ internal frames elided ...]
    #6 <null> <null>

  Previous write of size 4 at 0xADDR by main thread:
    #0 main snippet.cpp:9

  Location is global 'counter' of size 4 at 0xADDR

  Thread T1 (tid=N, running) created by main thread at:
    #0 pthread_create ../../../../src/libsanitizer/tsan/tsan_interceptors_posix.cpp:1022
    #1 std::thread::_M_start_thread(std::unique_ptr<std::thread::_State, std::default_delete<std::thread::_State> >, void (*)()) <null>

SUMMARY: ThreadSanitizer: data race snippet.cpp:8 in operator()
==================
ThreadSanitizer: reported 1 warnings
```

The program "works" on many runs, and prints a plausible count: that is exactly why races survive code review. TSan reports both conflicting accesses with their stacks, the thread creation site, and the memory location. Fix with `std::atomic<int>` or a mutex (Chapters 29–31). **TSan reports a race it *observes* under the happens-before analysis, even if the final value happened to be right; the absence of a report on a run proves nothing about paths not executed.**

### Experiment 7 ✅: Let the compiler prove UB: `constexpr`

```cpp
// @test fail -std=c++23 err=overflow|not.a.constant|constant.expression|out.of.range|array.subscript
#include <climits>

constexpr int add_one(int x) { return x + 1; }
constexpr int overflow = add_one(INT_MAX);            // UB in a constant expression => hard error

constexpr int oob() { int a[3] = {1, 2, 3}; return a[3]; }
constexpr int bad_index = oob();                      // also an error

int main() {}
```

Constant evaluation is the only place the language *requires* UB detection, and compilers implement it thoroughly (bounds, overflow, null, use-after-lifetime-end, signed shifts, even uninitialised reads). Practical technique: **write small utility functions `constexpr` and `static_assert` their edge cases**; the compiler becomes a free, exhaustive UB checker for that input. (Chapter 16.)

### Experiment 8 ✅: Hardened library: one flag, no recompile of the world

```cpp
// @test crash -std=c++23 -O2 -D_GLIBCXX_ASSERTIONS err=Assertion|assert
#include <cstdio>
#include <vector>

int main(int argc, char**) {
    std::vector<int> v = {1, 2, 3};
    std::printf("%d\n", v[argc + 4]);          // out of range: UB normally, an abort with _GLIBCXX_ASSERTIONS
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
/usr/include/c++/14/bits/stl_vector.h:1130: constexpr std::vector<_Tp, _Alloc>::reference std::vector<_Tp, _Alloc>::operator[](size_type) [with _Tp = int; _Alloc = std::allocator<int>; reference = int&; size_type = long unsigned int]: Assertion '__n < this->size()' failed.
```

`_GLIBCXX_ASSERTIONS` turns precondition violations of the standard library (`operator[]`, `front()` on an empty container, `optional::operator*`, `span::operator[]`...) into immediate, diagnosable aborts. The cost is a compare and a never-taken branch per access: typically a few percent, and recommended for production builds. (libc++: `-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE`.) Compare with `.at()`, which throws an exception: `at()` is for inputs you expect to be wrong; hardening is for conditions that are *bugs*.

### Experiment 9 🔧: Valgrind: no recompilation, finds uninitialised reads

```cpp
// @test run -std=c++23 -O0 -g
#include <cstdio>
#include <cstdlib>
int main(int argc, char**) {
    int* p = static_cast<int*>(std::malloc(4 * sizeof(int)));
    p[0] = 1;                       // p[1..3] never written
    int sum = p[0] + p[2];          // reads uninitialised heap memory
    if (sum > 0) std::puts("positive"); else std::puts("non-positive");
    std::free(p);
    return argc - 1;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
positive
```

*Hand-run under Valgrind (not auto-verified by the snippet checker):*

```text
$ g++ -std=c++23 -g -O0 u.cpp -o u && valgrind --track-origins=yes ./u
==PID== Conditional jump or move depends on uninitialised value(s)
==PID==    at 0x1091CD: main (u.cpp:7)
positive
==PID== ERROR SUMMARY: 1 errors from 1 contexts (suppressed: 0 from 0)
```

ASan does *not* find this (the memory is allocated, just never written). Valgrind and Clang's MSan do, because they track an "initialised?" bit for every byte. Note the report is at the *use* (`if (sum > 0)`), not at the read; `--track-origins=yes` adds where the uninitialised value originated.

### Experiment 10 ✅: Defined alternatives: checked arithmetic and saturation

```cpp
// @test run -std=c++26 -O0
#include <climits>
#include <cstdio>
#include <numeric>
#include <optional>

// Checked add that reports overflow instead of invoking UB (GCC/Clang builtin; the portable form is a pre-check)
std::optional<int> checked_add(int a, int b) {
    int r;
    if (__builtin_add_overflow(a, b, &r)) return std::nullopt;
    return r;
}
// Portable (standard-only) pre-check:
bool add_would_overflow(int a, int b) { return (b > 0 && a > INT_MAX - b) || (b < 0 && a < INT_MIN - b); }

int main() {
    std::printf("checked_add(INT_MAX, 1)  -> %s\n", checked_add(INT_MAX, 1) ? "value" : "overflow detected");
    std::printf("checked_add(2, 3)        -> %d\n", *checked_add(2, 3));
    std::printf("portable pre-check       -> %d\n", add_would_overflow(INT_MAX, 1));
    std::printf("C++26 std::add_sat(INT_MAX, 1)  = %d  (saturates)\n", std::add_sat(INT_MAX, 1));
    std::printf("C++26 std::sub_sat(INT_MIN, 1)  = %d\n", std::sub_sat(INT_MIN, 1));
    unsigned u = UINT_MAX; ++u;
    std::printf("unsigned wraparound is defined: %u\n", u);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
checked_add(INT_MAX, 1)  -> overflow detected
checked_add(2, 3)        -> 5
portable pre-check       -> 1
C++26 std::add_sat(INT_MAX, 1)  = 2147483647  (saturates)
C++26 std::sub_sat(INT_MIN, 1)  = -2147483648
unsigned wraparound is defined: 0
```

⚖️ `<numeric>` saturation functions (`std::add_sat`, `sub_sat`, `mul_sat`, `div_sat`, `saturate_cast`) are **C++26**; GCC 14 already provides them in `-std=c++26` mode. `__builtin_*_overflow` is a GCC/Clang extension that compiles to one add plus a flag test and is the fastest checked form today.

---

## 8. Assembly / runtime investigation

Questions to answer when you suspect UB, and the tools that answer them:

```bash
# (1) Does the bug disappear at -O0?  (classic sign of UB or a race)
g++-14 -std=c++23 -O0 prog.cpp && ./a.out ; g++-14 -std=c++23 -O2 prog.cpp && ./a.out

# (2) Look at what the optimiser assumed:  compare -O0 and -O2 assembly of the suspicious function
g++-14 -std=c++23 -O2 -S -masm=intel -o - prog.cpp | c++filt | awk '/^suspect_function/,/ret/'

# (3) Turn on the opt-outs to confirm the diagnosis (one at a time!)
-fwrapv                        # signed overflow wraps
-fno-strict-aliasing           # type-based alias analysis off
-fno-delete-null-pointer-checks
-fno-strict-overflow           # pointer/signed overflow assumptions off
-fsanitize=undefined -fno-sanitize-recover=all   # abort at the first violation, with a stack trace

# (4) The standard diagnostic stack for CI
-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnull-dereference -Wdangling-pointer=2 \
-Wuninitialized -Wmaybe-uninitialized -Wstrict-aliasing=2 -Wreturn-type -Wformat=2 -Wcast-align -Wold-style-cast -Werror
clang-tidy -checks='bugprone-*,cert-*,clang-analyzer-*,cppcoreguidelines-*' prog.cpp -- -std=c++23

# (5) Sanitizer matrix (each is a separate build; ASan and TSan cannot be combined)
g++-14 -fsanitize=address,undefined -fno-omit-frame-pointer -g
g++-14 -fsanitize=thread -g
clang++-18 -fsanitize=memory -fsanitize-memory-track-origins -g      # all code, including libc++, must be instrumented
valgrind --leak-check=full --track-origins=yes ./prog

# (6) Core dumps and post-mortem
ulimit -c unlimited; ./prog; gdb ./prog core    # bt, info registers, x/20i $pc

# (7) Fuzz to reach the paths your tests don't: libFuzzer / AFL++ with ASan+UBSan on
clang++-18 -fsanitize=fuzzer,address,undefined harness.cpp
```

---

## 9. Implementation exercise

Build a tiny **UB-hardening toolkit** you can use in every project:

1. `checked<Int>`: a wrapper whose `+ - * / << >>` use `__builtin_*_overflow` (or portable pre-checks) and either abort with a message, throw, or return `std::expected` (policy template parameter).
2. `not_null<T*>`: constructor aborts on null; `operator*` never needs a check.
3. `bounded_span<T>`: like `std::span` but with checked `operator[]` that reports file/line via `std::source_location`.
4. A CMake preset set (`debug-asan`, `debug-tsan`, `release-hardened`) enabling the flags above, plus a test target that deliberately triggers each bug class to **prove the CI configuration catches it** (a "canary test": the build must fail if a deliberately-buggy program does *not* produce a sanitizer report).
5. `-Werror` on a warning set, and a `clang-tidy` config suppressing only what you justify in writing.

<details>
<summary><strong>Solution sketch: `checked<int>` with a policy</strong></summary>

```cpp
// @test run -std=c++26 -O0
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <source_location>

struct AbortPolicy {
    [[noreturn]] static void fail(const char* op, std::source_location where) {
        std::fprintf(stderr, "checked<>: %s overflow at %s:%u\n", op, where.file_name(), where.line());
        std::abort();
    }
};

template <class Int, class OnError = AbortPolicy>
class checked {
    Int v_;
public:
    constexpr checked(Int v = 0) noexcept : v_(v) {}
    constexpr Int value() const noexcept { return v_; }

    friend constexpr checked operator+(checked a, checked b) { return add(a, b); }
    static constexpr checked add(checked a, checked b, std::source_location w = std::source_location::current()) {
        Int r;
        if (__builtin_add_overflow(a.v_, b.v_, &r)) OnError::fail("addition", w);
        return r;
    }
    static constexpr checked mul(checked a, checked b, std::source_location w = std::source_location::current()) {
        Int r;
        if (__builtin_mul_overflow(a.v_, b.v_, &r)) OnError::fail("multiplication", w);
        return r;
    }
};

int main() {
    checked<int> a = 40, b = 2;
    std::printf("40 + 2 = %d\n", (a + b).value());
    static_assert((checked<int>(40) + checked<int>(2)).value() == 42);      // usable in constant expressions
    std::printf("INT_MAX * 2 would abort here; skipping the call in this demo\n");
    // checked<int>::mul(std::numeric_limits<int>::max(), 2);               // uncomment to see the abort
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
40 + 2 = 42
INT_MAX * 2 would abort here; skipping the call in this demo
```

Because the `fail` path is `[[noreturn]]` and `constexpr`-reachable, the same type is a compile-time overflow detector inside `static_assert` and a run-time one outside it.

</details>

---

## 10. Real-world example

| Where | Story |
|---|---|
| **Linux kernel** | Built with `-fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks` precisely because kernel code relies on behaviours the standard leaves undefined; UBSan (`CONFIG_UBSAN`) and KASAN find the rest |
| **Chromium / Firefox / Android** | ASan/UBSan/MSan/TSan in CI; libFuzzer corpora run continuously; a majority (~65–70 % by Google/Microsoft's published statistics) of serious security vulnerabilities in their C/C++ code are **memory-safety** bugs |
| **CVEs** | Heartbleed (OpenSSL, 2014): out-of-bounds read. Many "compiler removed my overflow check" bugs, e.g. `if (len + x < len)` style checks, deleted for signed types (CERT INT32-C) |
| **Microsoft, Apple, Google hardening** | Bounds-safe libraries (`std::span` + hardened modes), `-fbounds-safety` (Clang), MTE/pointer authentication on ARM, Chromium's `raw_ptr<T>` and `MiraclePtr` (quarantine + poison to turn UAF into a safe crash) |
| **Rust / safe-language pressure** | Government agencies (CISA, NSA) recommend memory-safe languages. WG21's response: hardened library, erroneous behaviour, contracts, and the "profiles" work — C++26 and beyond. Realistic stance: **C++ stays unsafe by default; you opt into safety** |
| **Qt** | `QObject` lifetime bugs (dangling `QObject*` after parent deletion), `QPointer`/`QSharedPointer`, `Q_ASSERT` (debug only) and `qt_assert` hardening; deleting objects from a different thread; signal/slot connections to destroyed receivers are auto-disconnected (Chapter 47) |
| **Python extensions** | A C++ extension that frees memory Python still references crashes the interpreter far from the bug; ASan with `LD_PRELOAD=libasan.so python …` finds it (Chapter 46) |

> **Opinion.** The folk advice "just compile with `-O0`" or "just add `-fno-strict-aliasing`" is wrong in both directions: it hides bugs that will resurface, and it forks you from the language. My policy for any serious C++ project: **(1)** build with the full warning set as errors; **(2)** run the *entire* test suite under ASan+UBSan on every commit and under TSan nightly; **(3)** enable `_GLIBCXX_ASSERTIONS` (or libc++ extensive hardening) in production builds — a few percent buys you a deterministic abort instead of silent memory corruption; **(4)** treat sanitizer reports as P0 bugs, not noise; **(5)** forbid, by lint, the high-risk constructs outright: C-style casts, `reinterpret_cast` outside a reviewed utility header, raw `new`/`delete`, `std::move`-then-use, `memcpy` of non-trivial types. And don't "fix" UB by *adding the opt-out flag* unless you can state, in a code comment, exactly which defined behaviour the code is relying on and why that is the right trade.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| "It works in debug and fails in release" | UB or a race exploited only by the optimiser | Run sanitizers at `-O1`/`-O2`; compare assembly; do not assume the optimiser is buggy first |
| Checking for overflow *after* it happened (`if (a + b < a)` for `int`) | Check removed by the optimiser | Pre-check (`a > INT_MAX - b`) or builtin |
| Null check after dereference | Check removed; crash or silent corruption | Check first; use references for non-null |
| Reading an uninitialised variable "just to seed randomness" | Different values, or both branches of an `if` taken | Initialise; `std::random_device` |
| `reinterpret_cast<float*>(&u32)` | Wrong values at `-O2` | `std::bit_cast` / `memcpy` |
| Returning a reference/pointer/`string_view` to a local or temporary | Garbage, crash, or "works" until the stack frame is reused | Return by value; ownership types; `-Wdangling-pointer`, `-Wreturn-local-addr`, ASan |
| Keeping an iterator/reference across `push_back`/`insert`/`erase` | Use-after-free | Index; reserve; reacquire |
| Writing a racy "stop flag" as `bool` | Loop never sees the update (hoisted into a register) | `std::atomic<bool>` / `std::stop_token` |
| `volatile` used for thread synchronisation | Still a data race | Atomics (Chapter 31) |
| Relying on evaluation order of arguments or operands | Different results per compiler | Separate statements |
| Assuming `sizeof(long) == 8` or `char` signed | Breaks on another ABI | `<cstdint>`, `static_assert` |
| `memcmp` on structs with padding or on non-trivial types | Spurious inequality, UB | Member-wise `==` / `operator<=>` |
| Sanitizer build treated as optional | Bugs found by users, not CI | CI matrix with ASan+UBSan, TSan; canary tests |
| Fixing UB with `-fno-*` flags | Behaviour forked from the standard; other compilers differ | Fix the code; document any deliberate dependency |
| Suppressing sanitizer reports in third-party code and forgetting | Real bugs hidden | Narrow suppressions with comments and an expiry |
| Believing "no sanitizer report" means "no UB" | False confidence | Only executed paths are checked: add tests, fuzzers |

---

## 12. Exercises

1. **Classify.** For each, state defined / unspecified / implementation-defined / undefined (cite the rule): `INT_MAX + 1`; `UINT_MAX + 1u`; `1 << 31` (C++17 vs C++20); `-1 >> 1`; `f(i++, i++)`; `*(int*)&x` where `x` is `float`; `p + 5` for `int a[3]`; `(char)200`; `delete p; delete p;`; reading a member of an inactive union alternative; `std::vector<int>::front()` of an empty vector.
2. **Time travel.** Write a function where UB later in the function changes behaviour *before* the UB. Show the assembly at `-O2` and the observable output difference vs `-O0`.
3. **The missing return.** Write `int f(int x) { if (x > 0) return 1; }`. Find what GCC 14 and Clang 18 generate at `-O2` for `f(-1)`, and what `-Wreturn-type` and `-fsanitize=return` say.
4. **Hunt.** Take a project of yours (or an open-source one) and build it with `-fsanitize=address,undefined`; run its tests; triage every report: real bug / false positive / needs a suppression.
5. **Provenance.** Explain why `int a[2], b[2]; if (a + 2 == b) *(a + 2) = 1;` is UB even when `a + 2 == b` is true. Which standard rule? What does `std::launder` change (and not change)?
6. **Opt-out matrix.** Pick three UB-dependent snippets from this chapter. For each, determine which of `-fwrapv`, `-fno-strict-aliasing`, `-fno-delete-null-pointer-checks` changes the generated code, and explain the pairing.
7. **Race anatomy.** Write the racy `bool done` busy-wait. Compile at `-O0`, `-O2`; show the assembly where the load is hoisted out of the loop; fix with `atomic<bool>` and compare.
8. **Fuzz.** Write a parser with a subtle off-by-one. Build a libFuzzer harness with ASan; let it find the crash; minimise the input; write the regression test.

---

## 13. Challenge: a UB-hunting regression suite

Build a repository of 25 small programs, one per row of the catalogue in §5.2, each demonstrating that UB and its fix. For each program the harness must:

- confirm the **buggy** version is flagged by the appropriate sanitizer/warning/hardening (record which tool and the first line of the report);
- confirm the **fixed** version is clean under all tools;
- record whether the buggy version's *behaviour differs* across `-O0/-O2/-O3` and GCC/Clang (a table: output, exit code);
- for those that no tool catches at all (find at least three), explain why and what other process (review, fuzzing, `constexpr`) would;
- run in CI and fail if any buggy version stops being detected (canary).

The deliverable is the table, with your conclusion on which classes of UB are best caught by *design* (types), *compile time*, *tests + sanitizers*, or *only by review*.

---

## 14. Knowledge check

1. State the difference between undefined, unspecified and implementation-defined behaviour with one example each.
2. Why can a compiler delete `if (p == nullptr)` after `*p`? Does the deletion violate the standard?
3. Why is `x + 1 > x` compiled to `true` for `int` but not for `unsigned`?
4. What is "time travel" in UB discussions?
5. How do you legally reinterpret the bytes of a `float` as a `uint32_t`? Which two ways, and which is preferable?
6. What does ASan find that Valgrind/MSan don't, and vice versa?
7. Why can't ASan and TSan be combined? Which pair of sanitizers *can* be?
8. What does a TSan report not tell you when it prints nothing?
9. Why is `constexpr` a powerful UB detector?
10. What does `_GLIBCXX_ASSERTIONS` do, and why is it reasonable for production but `_GLIBCXX_DEBUG` is not?
11. What is erroneous behaviour (C++26)? How does it differ from UB for an uninitialised read?
12. Why is "it works on my machine / at -O0" meaningless for UB?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Undefined: no requirements at all (`INT_MAX + 1`). Unspecified: one of several valid results, not documented (order of argument evaluation). Implementation-defined: implementation chooses and must document (`sizeof(long)`, `char` signedness).
2. The dereference means the program has UB if `p` is null, and the compiler may assume UB never occurs, so `p` is non-null and the check is dead code. The standard allows it: after UB there are no requirements.
3. Signed overflow is UB, so `x + 1` can be assumed not to overflow and is therefore always greater than `x`. Unsigned arithmetic wraps and is well defined, so the result is false when `x == UINT_MAX`.
4. Because executions reaching UB are not constrained, the compiler may transform code that comes *before* the UB point (e.g. remove checks or computations) when it can prove the UB path will be taken.
5. `std::memcpy` into a `uint32_t`, or `std::bit_cast<uint32_t>(f)` (C++20, `constexpr`, type-checked). `bit_cast` is preferable: it states intent, checks sizes, works in constant expressions. A pointer cast violates strict aliasing.
6. ASan finds out-of-bounds, use-after-free/scope, double free using red zones and shadow memory; it misses uninitialised reads. Valgrind/MSan track initialisedness per byte and find uninitialised reads; MSan needs full instrumentation, Valgrind is slow; ASan's overflow detection is generally faster and finds stack/global overflows Valgrind can't.
7. Both reserve conflicting shadow-memory layouts and interpose on allocation/runtime. ASan+UBSan can be combined (UBSan has no shadow memory).
8. It tells you only that no race occurred in the executed interleavings and paths of that run; races on unexecuted code or other schedules are not covered.
9. UB in constant evaluation makes the expression non-constant (a diagnosed error), so the compiler must detect overflow, out-of-bounds, null, lifetime errors etc. for the evaluated input.
10. It enables cheap precondition checks in the library (`operator[]`, `front()`, etc.) that abort on violation; low overhead. `_GLIBCXX_DEBUG` replaces containers with checked versions (changing ABI/layout) and is too heavy and incompatible for production.
11. A defined-but-wrong value is read (the implementation picks it) rather than UB; the implementation may diagnose; the optimiser can't assume it doesn't happen, so no time-travel. `[[indeterminate]]` opts out. (Adopted for C++26; support is still arriving.)
12. UB makes no promise of consistency; an optimiser change, flag, platform or unrelated code edit can change behaviour; absence of observed misbehaviour isn't evidence of correctness.

</details>

---

[← Previous: Chapter 27](../part-10-memory/27-cache-and-data-oriented-cpp.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 29 — C++ threading →](../part-12-concurrency/29-threading.md)
