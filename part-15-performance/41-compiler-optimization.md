# Chapter 41 — Compiler Optimization: What the Compiler Does, and What It Refuses To

> **Part XV · Performance engineering** &nbsp;|&nbsp; **Level 4** (compiler/runtime) &nbsp;|&nbsp; **≈ 8 hours**
> **Prerequisites:** [Chapter 39 (benchmarking)](39-benchmarking.md), [Chapter 40 (profiling)](40-profiling.md), [Chapter 28 (undefined behavior)](../part-11-undefined-behavior/28-undefined-behavior.md), [Chapter 36 (compilation model)](../part-14-compilation-and-linking/36-compilation-model.md) &nbsp;|&nbsp; **Standards:** the as-if rule ⚖️; everything else is 🔧 compiler-specific &nbsp;|&nbsp; **Tools:** `g++-14`, `clang++-18`, `objdump`, `-fopt-info`, `-Rpass`

[← Previous: Chapter 40](40-profiling.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 42 — API design →](../part-16-library-design/42-api-design.md)

---

**In one sentence:** the optimiser is a pipeline of transformations (inlining, constant propagation, devirtualisation, loop transformations, vectorisation, scheduling) that may rewrite your program in any way that preserves its *observable behaviour*; knowing which transformations exist, what they need in order to fire, and what blocks them is what lets you write code the compiler can make fast, and read the assembly to check that it did.

**By the end of this chapter you can:**

- say what `-O0 … -O3`, `-Os`, `-Ofast`, `-march=`, LTO and PGO actually change, and measure each on your code
- predict whether a loop will be vectorised and, if not, read the compiler's reason
- explain why inlining is "the mother of all optimisations" and how LTO extends it across translation units
- explain when a virtual call becomes a direct call (and when it can't)
- name the optimisations that *depend on undefined behaviour* and the flags that trade them away
- choose a build configuration for a program with evidence instead of folklore

> [!NOTE]
> Everything in this chapter is a **compiler** matter, not a language one (🔧): the standard allows these transformations (through the as-if rule) and requires none of them. Where GCC 14.2 and Clang 18.1 differ I show both. Numbers are from this course's 2-vCPU VM: ratios matter, digits don't.

---

## 1. Problem

You write clear, abstract code, and then you read three contradictory claims:

> "The compiler will inline that for free." &nbsp;&nbsp; "Never trust the compiler; write it by hand." &nbsp;&nbsp; "Just use `-O3 -ffast-math`."

All three are sometimes right and often costly. The concrete questions are:

| Question | Where the answer comes from |
|---|---|
| Is my abstraction (iterator, lambda, `std::function`, virtual call) actually free? | Reading the assembly |
| Why is this loop not vectorised? | `-fopt-info-vec-missed` / `-Rpass-analysis` |
| Does `-O3` beat `-O2`? Does `-Ofast`? Does `-march=native`? | Measuring *your* code |
| Should I use LTO? PGO? | Measuring, plus knowing the build cost |
| Why did the compiler delete my check, or reorder my floating-point math? | Knowing which optimisations rely on UB and on "fast-math" assumptions |

---

## 2. Historical context

| Era | Development |
|---|---|
| 1957–1970s | FORTRAN's optimiser sets the pattern: register allocation, common subexpression elimination, loop-invariant motion |
| 1980s–90s | C compilers add inlining, strength reduction, instruction scheduling; `register` and `inline` are *hints* the optimiser increasingly ignores |
| 2000 | **SSA form** becomes the standard IR (GCC's Tree-SSA 2005; LLVM from 2003) enabling sparse, scalable dataflow optimisations |
| 2003–10 | **LLVM** and the modularised pass pipeline; **LTO** (link-time optimisation) arrives in GCC 4.5 (2010) and LLVM |
| 2008 | **ThinLTO** (LLVM): scalable parallel whole-program optimisation |
| 2000s | **Auto-vectorisation** matures (SSE/AVX); **PGO** becomes practical (`-fprofile-use`, Chrome/Firefox adopt it) |
| 2010s | UB-driven optimisations make the news ("Why did my null check disappear?"); `-fsanitize=undefined` and `-fwrapv` become standard tools |
| 2020s | GCC 12 enables vectorisation at `-O2` (very-cheap cost model); LLVM 17+ improves loop and memory optimisation; `-fprofile-sample-use` (AutoFDO/BOLT) optimise from *production* profiles |

---

## 3. Modern solution

There is no new C++ feature. The modern approach is a **toolbox with a method**:

```text
1. Start with  -O2  (portable default)         → measure
2. Add         -march=<baseline>               → only if you control the deployment CPU
3. Try         -O3                              → only keep if it wins in YOUR benchmark
4. Try         -flto  (or -flto=thin)           → bigger win for code split across many TUs
5. Try         PGO    (-fprofile-generate/use)  → for hot, branchy, steady-state code
6. Never       -Ofast globally                  → opt in per file, with tests
7. Always      read the assembly or the optimisation report for the code that matters
```

The tools that make step 7 practical:

```bash
g++-14 -O2 -fopt-info-vec-optimized -c a.cpp        # which loops did GCC vectorise?
g++-14 -O2 -fopt-info-vec-missed    -c a.cpp        # and which did it refuse, and why
g++-14 -O2 -fopt-info-inline-optimized -c a.cpp     # inlining decisions
clang++-18 -O2 -Rpass=inline -Rpass-missed=loop-vectorize -Rpass-analysis=loop-vectorize -c a.cpp
g++-14 -O2 -S -masm=intel -o - a.cpp | less         # the assembly (or use Compiler Explorer)
```

---

## 4. Mental model

### The pipeline

```text
 source ─► parse ─► AST ─► (lowering) ─► SSA IR ─┬─► [ inliner ]──┐
                                                 │                │  repeated, in rounds:
                                                 ├─► constant propagation / folding
                                                 ├─► dead-code & dead-store elimination
                                                 ├─► common-subexpression & loop-invariant motion
                                                 ├─► loop unroll / interchange / vectorise
                                                 ├─► devirtualisation (when the type is known)
                                                 └─► ... ◄───────────┘
                                          ─► instruction selection ─► register allocation ─► scheduling ─► machine code
```

The single most important fact: **inlining is the enabler.** Most other optimisations work only on the code the optimiser can *see in one function*. After a call is inlined, the callee's arguments become known constants, its pointer arguments can be proven not to alias, its loop can merge with the caller's, its virtual call can have a known target. Everything you were told about "zero-cost abstractions" (Chapter 1) is actually: *cost removed by inlining followed by everything else*.

```text
   before inlining                         after inlining + constant propagation + DCE
   ────────────────                        ──────────────────────────────────────────
   int f() { return sq(7); }               int f() { return 49; }
   int sq(int x) { return x*x; }
```

(Experiment 3 shows exactly this: `local_known()` compiles to `mov eax, 49`.)

### What blocks an optimisation: the "can it prove it?" question

The compiler transforms only when it can **prove** the result is observably identical. Every missed optimisation is a failed proof:

| The compiler can't prove… | Typical cause | What helps |
|---|---|---|
| …that two pointers don't alias | `void f(float* y, const float* x)` | `__restrict`, local copies, `std::span` of distinct arrays |
| …that a floating-point reordering is value-preserving | FP addition is not associative | `-ffast-math` (dangerous), or write the reduction in lanes yourself |
| …that the callee is visible | Defined in another TU | LTO; define small functions in the header (`inline`) |
| …the dynamic type of an object | Pointer from a factory, plugin or `virtual` interface | `final`, templates, LTO with whole-program visibility, PGO |
| …that a loop trip count is safe | Signed `int` induction variables are fine; `unsigned` wraparound and `size_t` conversions can block | Use `int`/`ptrdiff_t`, `ranges`, simple loops |
| …that memory isn't modified by a call | An opaque call (`printf`, a virtual call, an `asm volatile("" ::: "memory")`) | Keep calls out of hot loops; cache values in locals |

### What the optimiser may assume: undefined behaviour is a *licence*

Because UB "can't happen" in a correct program, the optimiser is allowed to assume it doesn't, and use that to simplify code (Chapter 28):

| Assumption | Enables | Flag to switch off |
|---|---|---|
| Signed overflow doesn't occur | `x + 1 > x` ⇒ `true`; loop induction analysis; wider-register tricks | `-fwrapv` |
| Distinct types don't alias (**strict aliasing**) | Keeping values in registers across stores of another type | `-fno-strict-aliasing` |
| Null isn't dereferenced then compared | Deleting a null check *after* a dereference | `-fno-delete-null-pointer-checks` |
| Loops without side effects terminate (⚖️ C++11 forward-progress) | Removing empty loops | (none; write a `volatile`/atomic access) |

### Costs of the optimiser itself

More aggressive optimisation costs **compile time**, **code size** (inlining and unrolling), and **debuggability** (variables "optimised out", reordered statements). `-O3` can be slower than `-O2` through bloated code that no longer fits the instruction cache. Optimisation level is a trade-off, not a dial that goes to "better".

---

## 5. Language rules

| Rule | Text | Consequence for optimisation |
|---|---|---|
| **As-if rule** ⚖️ [intro.abstract]/1 | The implementation need only reproduce the *observable behaviour*: volatile accesses, I/O, and the final data of the abstract machine's calls to library I/O functions | Almost any transformation is legal, including deleting, reordering and merging |
| **Copy elision** ⚖️ [class.copy.elision] | Permitted (C++11–14) or required (C++17 prvalue) *even if it changes observable behaviour* (constructor side effects) | The one place the compiler may break "as-if"; see Chapter 6 |
| **Allocation elision** ⚖️ [expr.new]/10 | A new-expression's allocation may be omitted or merged if unobservable | `new int[100]` followed by `delete[]` can vanish (Chapter 1) |
| **Forward progress** ⚖️ [intro.progress] | A thread of execution must eventually do something observable, unless it is trivially infinite in the C++26 wording | Compilers may delete loops with no side effects |
| **UB** ⚖️ | No requirements on programs with UB | Licence for the assumptions above |
| **Floating point** ⚖️ [basic.fundamental] | The standard does *not* require IEEE 754; Annex F is for C. GCC/Clang on x86-64 default to IEEE semantics, with `-ffp-contract=fast` (GCC) allowing fused multiply-add | `a*b+c` may or may not be fused; results can differ between compilers/flags |
| **`inline`** ⚖️ | A linkage keyword (multiple definitions allowed), *not* a command to inline | The optimiser inlines by cost model, not by keyword; `[[gnu::always_inline]]`/`noinline` are extensions 🔧 |
| **`[[likely]]` / `[[unlikely]]`** ⚖️ C++20 | Hints for branch layout | Can move cold code out of line; PGO is better |
| **`std::assume_aligned`, `[[assume(...)]]`** ⚖️ C++20/23 | Tell the optimiser a fact you vouch for | Wrong assumptions are UB (use sparingly, verify with a sanitizer build) |
| **`__restrict`** 🔧 (C99 `restrict` is not C++) | Promise of non-aliasing | Enables vectorisation of loops with two pointer arguments |

### Layer check

| Layer | What it decides |
|---|---|
| **C++ standard** | What transformations are *allowed* (as-if) and which are *assumed safe* (no UB) |
| **Compiler** | Which transformations fire, in what order, with what cost model (`-O` level, `--param`s). **GCC and Clang differ**, and so do versions |
| **ABI** | Calling convention, which registers hold arguments; whether `noinline` calls are cheap; what must be preserved across calls |
| **OS** | Page-fault and dynamic-linking costs the optimiser cannot see (`-fno-plt`, `-Wl,-z,now`, `prelink`) |
| **CPU** | The target: SSE2 by default on x86-64, AVX2/AVX-512 only with `-march`; instruction latencies for scheduling |

---

## 6. Implementation model

### What each flag family does (GCC 14 / Clang 18)

| Flag | What it enables (summary) | Risk |
|---|---|---|
| **`-O0`** | No optimisation; every variable lives in memory; fastest compile; best debugging | Abstractions are real calls (Chapters 14, 39) |
| **`-Og`** | Optimise but keep debuggability | Slower than `-O1` |
| **`-O1`** | Basic scalar optimisations: constant folding, DCE, simple inlining of `static`/tiny functions, register allocation | None |
| **`-O2`** | The production default: adds most interprocedural and scalar passes, instruction scheduling, jump threading, GVN, **GCC 12+: vectorisation with a very-cheap cost model** (only when it doesn't need runtime checks or peeling) | Larger code than `-O1`, compile time ↑ |
| **`-O3`** | More inlining, loop unrolling, loop interchange, **aggressive vectorisation** (GCC) | Code bloat; sometimes slower |
| **`-Os` / `-Oz`** | Optimise for size | Slower code |
| **`-Ofast`** | `-O3 -ffast-math` (+ `-fno-math-errno`, `-ffinite-math-only`, `-fassociative-math`, …) and, with GCC, `-fallow-store-data-races` | **Breaks IEEE: NaN/inf checks, associativity, error handling** (Experiment 5) |
| **`-march=x86-64-v3` / `-march=native`** | Allows AVX2/FMA/BMI… | Binary doesn't run on older CPUs; `native` is non-reproducible across machines |
| **`-flto`** | Defers optimisation to link time; the whole program is one IR | Slower link, more memory; Clang's `-flto=thin` parallelises |
| **`-fprofile-generate` / `-fprofile-use`** | Instrumented build → run → optimise using branch/call frequencies | Needs a representative training run; build complexity |
| **`-fno-semantic-interposition`** (shared libs) | Lets the compiler inline across exported functions inside a `.so` | Changes symbol-interposition semantics |

### Vectorisation: what the loop vectoriser needs

```text
   for (int i = 0; i < n; ++i)  s += p[i];
```

1. **Countable loop**: the trip count `n` is computable before the loop starts.
2. **No loop-carried dependencies** except recognised *reductions* (sum, min, max, or/and).
3. **Alias-free or checkable**: otherwise the compiler emits a runtime check and two versions of the loop ("loop versioned for vectorization because of possible aliasing" was reported for `saxpy` in this chapter's build).
4. **A profitable cost model** (it must beat scalar code including setup and remainder handling).
5. For **floating-point reductions**: reordering additions changes the result, so it is only allowed under `-ffast-math`/`-fassociative-math` (Experiment 1).

### Devirtualisation (GCC and Clang, no LTO)

The compiler can turn `p->f()` into a direct call when it can prove the dynamic type: (a) the object is constructed in the same function; (b) the static type is `final` (or the method is); (c) the class has a single known implementation visible under whole-program/LTO visibility; (d) GCC's **speculative devirtualisation** guesses the most likely target and checks the vtable slot at run time (Experiment 3).

### LTO and PGO in one paragraph each

**LTO** puts the compiler's intermediate representation (GIMPLE / LLVM bitcode) into the object files, and at link time runs the optimiser over the whole program. It turns every function into an inlining candidate for every other TU and lets it discard unused code and propagate constants across files. **PGO** adds *data*: it instruments the binary, runs it on a training workload, writes edge and call counts, and recompiles using them to decide inlining, branch layout, and hot/cold splitting. It is the only approach that tells the compiler how your program actually behaves.

---

## 7. Experiments

All code in [`code/ch41/`](code/ch41/); `bash run.sh` runs them. **Timings: 2 vCPUs of a VM; GCC 14.2 and Clang 18.1.3; best-of-9 loops with an `asm` barrier so results aren't hoisted (Chapter 39).**

### Experiment 1 🔧: Four kernels, every optimisation level

`levels.cpp` times four `noinline` functions on 4 096 elements (16 KiB, in L1): a `float` sum, an `int` sum, `saxpy` (`y[i] += a*x[i]`), and a conditional count. Time per call, ns (lower is better):

| Build | `sum_f` | `sum_i` | `saxpy` | `count_even` |
|---|---:|---:|---:|---:|
| **g++ `-O0`** | 11 300 | 10 040 | 11 172 | 6 616 |
| g++ `-O1` | 4 974 | 2 321 | 3 039 | 2 223 |
| g++ `-O2` | 4 963 | 1 624 | 2 542 | 2 351 |
| g++ `-O3` | 5 006 | 659 | 644 | 934 |
| g++ `-Ofast` | **1 195** | 446 | 656 | 940 |
| g++ `-O3 -march=native` | 4 960 | **208** | 561 | **510** |
| g++ `-O2 -ftree-vectorize` | 4 936 | 446 | 640 | 940 |
| clang `-O1` | 5 009 | 1 650 | 2 626 | 2 455 |
| clang `-O2` | 4 960 | **247** | 561 | 788 |
| clang `-O3` | 4 947 | 245 | 533 | 791 |
| clang `-Ofast` | **627** | 245 | 568 | 788 |

Read this table slowly; it contains most of the chapter.

1. **`-O0` → `-O1` is 2–4×**, the biggest single step: registers instead of memory. Never judge abstraction costs at `-O0`.
2. **`-O2` → `-O3` is 2.5–4× for the integer and `saxpy` kernels with GCC**, because GCC 14's `-O2` vectorises only conservatively ("very cheap" model), and `-O3` does the full job. Adding just `-ftree-vectorize` to `-O2` gives about the same speedup: it is the vectoriser, not unrolling, that matters.
3. **Clang vectorises already at `-O2`**: its `-O2` numbers equal or beat GCC's `-O3`. The same flag name means different things in different compilers, so a "-O2 vs -O3" belief carried from GCC to Clang is wrong.
4. **`sum_f` doesn't move from `-O1` to `-O3` (≈ 5 000 ns) in either compiler, then drops 4× at `-Ofast`** (GCC 1 195, Clang 627). A floating-point sum is a serial dependency chain (`s = s + p[i]`); vectorising it changes the order of additions and therefore, possibly, the result, so the compiler refuses unless allowed to (`-fassociative-math`, implied by `-ffast-math`). The assembly says it directly: at `-O3`, `sum_f` is a chain of scalar `addss`; at `-Ofast` it uses `addps` on four lanes. Clang's reason message is explicit: *"loop not vectorized: cannot prove it is safe to reorder floating-point operations"*.
5. **`-march=native` buys another 2–3× on `sum_i`/`count_even`** (AVX2: 8 ints per instruction instead of 4), for free, at the price of portability.

```cpp
// @test asm -std=c++20 -O3 -fno-stack-protector filter=sum_f
float sum_f(const float* p, int n) { float s = 0; for (int i = 0; i < n; ++i) s += p[i]; return s; }
```

```asm
; asm (gcc 14.2.0, -O3, x86-64, Intel syntax)
sum_f(float const*, int):
	mov	ecx, esi
	test	esi, esi
	jle	.L7
	lea	eax, -1[rsi]
	cmp	eax, 2
	jbe	.L8
	mov	edx, esi
	mov	rax, rdi
	pxor	xmm0, xmm0
	shr	edx, 2
	sal	rdx, 4
	add	rdx, rdi
.L4:
	addss	xmm0, DWORD PTR [rax]
	add	rax, 16
	addss	xmm0, DWORD PTR -12[rax]
	addss	xmm0, DWORD PTR -8[rax]
	addss	xmm0, DWORD PTR -4[rax]
	cmp	rax, rdx
	jne	.L4
	mov	eax, ecx
	and	eax, -4
	test	cl, 3
	je	.L11
.L3:
	movsx	rdx, eax
	addss	xmm0, DWORD PTR [rdi+rdx*4]
	lea	rsi, 0[0+rdx*4]
	lea	edx, 1[rax]
	cmp	ecx, edx
	jle	.L1
	add	eax, 2
	addss	xmm0, DWORD PTR 4[rdi+rsi]
	cmp	ecx, eax
	jle	.L1
	addss	xmm0, DWORD PTR 8[rdi+rsi]
	ret
.L7:
	pxor	xmm0, xmm0
.L1:
	ret
.L11:
	ret
.L8:
	xor	eax, eax
	pxor	xmm0, xmm0
	jmp	.L3
```

The `-O3` code is unrolled four times but still a chain of scalar `addss` instructions, each depending on the previous one; with `-Ofast` the same source becomes:

```cpp
// @test asm -std=c++20 -Ofast -fno-stack-protector filter=sum_f
float sum_f(const float* p, int n) { float s = 0; for (int i = 0; i < n; ++i) s += p[i]; return s; }
```

```asm
; asm (gcc 14.2.0, -Ofast, x86-64, Intel syntax)
sum_f(float const*, int):
	mov	rcx, rdi
	test	esi, esi
	jle	.L7
	lea	eax, -1[rsi]
	cmp	eax, 2
	jbe	.L8
	mov	edx, esi
	mov	rax, rdi
	pxor	xmm0, xmm0
	shr	edx, 2
	sal	rdx, 4
	add	rdx, rdi
.L4:
	movups	xmm2, XMMWORD PTR [rax]
	add	rax, 16
	addps	xmm0, xmm2
	cmp	rdx, rax
	jne	.L4
	movaps	xmm1, xmm0
	mov	eax, esi
	movhlps	xmm1, xmm0
	and	eax, -4
	addps	xmm1, xmm0
	movaps	xmm0, xmm1
	shufps	xmm0, xmm1, 85
	addps	xmm0, xmm1
	test	sil, 3
	je	.L11
.L3:
	movsx	rdx, eax
	addss	xmm0, DWORD PTR [rcx+rdx*4]
	lea	rdi, 0[0+rdx*4]
	lea	edx, 1[rax]
	cmp	esi, edx
	jle	.L1
	add	eax, 2
	addss	xmm0, DWORD PTR 4[rcx+rdi]
	cmp	esi, eax
	jle	.L1
	addss	xmm0, DWORD PTR 8[rcx+rdi]
	ret
.L7:
	pxor	xmm0, xmm0
.L1:
	ret
.L11:
	ret
.L8:
	xor	eax, eax
	pxor	xmm0, xmm0
	jmp	.L3
```

(Look for `addps`: four additions per instruction, in an order that differs from the source's.)

### Experiment 2 ✅: LTO, the one-line fix for cross-TU calls

`weight()` is a tiny function defined in another translation unit; `main` calls it 2·10⁸ times.

```text
GCC   -O2        :  444 ms   362 ms   377 ms        call weight in main: 1
GCC   -O2 -flto  :   64 ms    66 ms    73 ms        call weight in main: 0     → 5.5-6× faster
Clang -O2        :  320 ms
Clang -O2 -flto  :   79 ms                                                     → 4× faster
```

Without LTO, the compiler compiling `main` sees only a declaration, so it must emit a real call (with the ABI's register moves and a function prologue) 2·10⁸ times. With LTO, the call is inlined, the loop is simplified, and the compiler then vectorises it; most of the 6× comes from the *second* step. (`objdump` confirms that `main` contains one `call weight` without LTO and none with.) For a 1-line function the right fix is to define it in the header as `inline`; LTO is the fix when you can't or when the call graph is too tangled.

### Experiment 3 🔧: Devirtualisation: four calls, three answers

```cpp
// @test asm -std=c++20 -O2 -fno-stack-protector filter=via_base,via_final,via_rect,local_known
struct Shape { virtual ~Shape() = default; virtual int area() const = 0; };
struct Sq final : Shape { int s; int area() const override { return s * s; } };
struct Rect : Shape       { int w, h; int area() const override { return w * h; } };

int via_base(const Shape& s)      { return s.area(); }                 // dynamic type unknown: must dispatch
int via_final(const Sq& s)        { return s.area(); }                 // Sq is final: no override possible
int via_rect(const Rect& r)       { return r.area(); }                 // Rect is not final: a subclass could override
int local_known() { Sq q; q.s = 7; const Shape& b = q; return b.area(); }   // dynamic type visible
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
via_base(Shape const&):
	mov	rax, QWORD PTR [rdi]
	jmp	[QWORD PTR 16[rax]]

via_final(Sq const&):
	mov	eax, DWORD PTR 8[rdi]
	imul	eax, eax
	ret

via_rect(Rect const&):
	mov	rax, QWORD PTR [rdi]
	lea	rdx, Rect::area() const[rip]
	mov	rax, QWORD PTR 16[rax]
	cmp	rax, rdx
	jne	.L13
	mov	eax, DWORD PTR 8[rdi]
	imul	eax, DWORD PTR 12[rdi]
	ret
.L13:
	jmp	rax

local_known():
	mov	eax, 49
	ret
```

| Function | GCC 14 `-O2` | Clang 18 `-O2` |
|---|---|---|
| `via_base` | indirect tail call through the vtable (`jmp [rax+16]`) | same |
| `via_final` | **inlined**: `imul eax, eax` | **inlined** |
| `via_rect` | **speculative devirtualisation**: loads the vtable slot, compares with `&Rect::area`, runs the inlined body if equal, else jumps | indirect `jmp` (no speculation) |
| `local_known` | `mov eax, 49` | `mov eax, 49` |

This is the quantitative content of "mark leaf classes `final`": it turns a virtual call into straight-line arithmetic that the optimiser then folds. GCC's speculation helps when the static type is "usually" exact; Clang at `-O2` doesn't do it without extra flags/LTO/PGO, so a performance claim about virtual calls must name the compiler.

### Experiment 4 🔧: PGO on a skewed workload

`pgo.cpp` is a tiny bytecode interpreter: 4 M instructions, 55 % `ADD`, 39 % `INC`, the rest rare; run 40 times (`-O2`, 3 runs each, ms):

```text
baseline            1117   1108   1077
-fprofile-use       1042   1008   1020          → about 6-8 % faster
```

The gain is modest and real (outside the ~3 % noise), not dramatic: this workload is dominated by *unpredictable* branches (random opcodes), which PGO can lay out better but cannot make predictable. PGO shines on code with strongly skewed branch and call distributions (compilers, database engines, interpreters with realistic programs, servers). The build cost is also real: instrumented build → representative run → rebuild, and the profile must be regenerated when the code changes (a stale `.gcda` is silently ignored; I first got no speed-up at all because the profile file name did not match the binary name: `-Wmissing-profile` warned about it).

### Experiment 5 ✅: `-Ofast` changes the meaning of the program

```cpp
// @test run -std=c++20 -O2
#include <cmath>
#include <cstdio>
#include <limits>
int main(int argc, char**) {
    double x = argc > 5 ? 1.0 : std::numeric_limits<double>::quiet_NaN();
    std::printf("isnan(x) = %d,  x != x = %d\n", std::isnan(x), x != x);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
isnan(x) = 1,  x != x = 1
```

With `-O2` both are `1`. The very same program built with `-Ofast` prints:

```text
GCC   -Ofast:  isnan(x) = 0,  x != x = 0
Clang -Ofast:  isnan(x) = 0,  x != x = 0       (with warning: "use of NaN is undefined behavior due to the currently enabled floating-point options")
```

A NaN that `isnan` doesn't recognise. `-ffast-math` includes `-ffinite-math-only`, a promise that no NaN or infinity ever occurs; the compiler is entitled to fold `isnan(x)` to `false`. The promise is yours; the failure is silent. **That is why `-Ofast` is not "a faster `-O3`".**

### Experiment 6 ✅: Optimisations that depend on UB

```cpp
// @test asm -std=c++20 -O2 -fno-stack-protector filter=always_true,still_checks
bool always_true(int x)  { return x + 1 > x; }                  // UB if x == INT_MAX, so "always true"
bool still_checks(unsigned x) { return x + 1 > x; }              // unsigned wraps: a real comparison
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
always_true(int):
	mov	eax, 1
	ret

still_checks(unsigned int):
	cmp	edi, -1
	setne	al
	ret
```

`always_true` becomes `mov eax, 1`: the compiler assumed signed overflow never happens. Compile the same function with `-fwrapv` and it performs the comparison. A program that "worked at `-O0`" and "broke at `-O2`" usually contains exactly this kind of UB, and the sanitizer (Chapter 28: `-fsanitize=undefined`) is the diagnostic, not "compiler bug".

---

## 8. Assembly / runtime investigation

```bash
# See what the optimiser decided
g++-14 -O2 -fopt-info-inline-optimized-missed -c a.cpp 2>&1 | less      # inlining: done / missed
g++-14 -O3 -fopt-info-vec-all -c a.cpp 2>&1 | grep 'a.cpp:LINE:'        # vectorisation, with reasons
clang++-18 -O3 -Rpass-analysis=loop-vectorize -c a.cpp                   # Clang's reasons for refusal
clang++-18 -O2 -Rpass=inline -c a.cpp                                    # which calls were inlined

# Compare two optimisation levels function-by-function
g++-14 -O2 -S -masm=intel -o O2.s a.cpp; g++-14 -O3 -S -masm=intel -o O3.s a.cpp; diff -u O2.s O3.s | less
size a_O2.o a_O3.o                                                       # text size: -O3 may be much bigger

# Which passes ran, and what did each do?
g++-14 -O2 -fdump-tree-all -c a.cpp          # one dump per GIMPLE pass (a.cpp.NNNt.passname)
clang++-18 -O2 -mllvm -print-after-all -c a.cpp 2> passes.txt
clang++-18 -O2 -emit-llvm -S a.cpp -o -      # LLVM IR before codegen
opt -O2 -print-pipeline-passes < a.ll        # the actual pass pipeline

# LTO and PGO plumbing
g++-14 -O2 -flto -fno-fat-lto-objects -c a.cpp; nm a.o | head           # object has GIMPLE, not code
gcov-dump pgo_x-pgo.gcda | head                                          # raw profile data (GCC)
g++-14 -O2 -fprofile-use -Wmissing-profile ...                           # warns when the profile does not match

# What does the CPU have, and what would -march=native turn on?
g++-14 -march=native -dM -E - < /dev/null | grep -E 'AVX|FMA|BMI' | head
lscpu | grep -E 'Flags' | tr ' ' '\n' | grep -E 'avx|fma|bmi' | sort | tr '\n' ' '
```

**A habit worth forming.** For any loop you care about: (1) `-fopt-info-vec-all` and look for your line; (2) if it says "missed", read the *reason*; (3) fix the reason (aliasing → `__restrict`; FP reduction → decide whether precision permits `-fassociative-math` on that file or restructure; control flow → branchless form; non-countable → simplify the loop). Do not paste intrinsics until the compiler's own explanation tells you it can't do the job.

---

## 9. Implementation exercise

**Write a compiler-friendly kernel, then prove the compiler agrees.** Implement `dot(span<const float>, span<const float>)` three ways:

1. A naive loop (`s += a[i]*b[i]`).
2. Four independent accumulators (manual reduction in lanes), combined at the end.
3. `std::transform_reduce` with `std::execution::unseq` (if your standard library supports it), or an `#pragma omp simd reduction(+:s)` loop.

For each: record the assembly (`addss` chain, `addps`/`vfmadd`, or both), time it at 4 K and 16 M elements, and explain the difference between (1) at `-O3` and (2) at `-O3` in terms of the *dependency chain* and floating-point associativity.

<details>
<summary><strong>Solution sketch</strong></summary>

```cpp
#include <cstddef>
#include <span>

float dot_naive(std::span<const float> a, std::span<const float> b) {
    float s = 0;
    for (std::size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];       // one serial chain: latency-bound
    return s;
}

float dot_lanes(std::span<const float> a, std::span<const float> b) {
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;                                // four independent chains
    std::size_t i = 0, n = a.size() & ~std::size_t{3};
    for (; i < n; i += 4) {
        s0 += a[i + 0] * b[i + 0]; s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2]; s3 += a[i + 3] * b[i + 3];
    }
    for (; i < a.size(); ++i) s0 += a[i] * b[i];
    return (s0 + s1) + (s2 + s3);                                          // result differs from naive in the last bits!
}
```

Why it works: a single-accumulator loop is bounded by the *latency* of one add (~4 cycles) per element, whatever the vector width. Four chains run in parallel; the compiler can also map the four lanes onto one SSE register (`addps`) because *you* have declared the reordering is acceptable. The two results differ in the low bits: **that is the price of changing the order**, and why the compiler won't do it for you.

</details>

---

## 10. Real-world example

| Where | Technique |
|---|---|
| **Chrome, Firefox, GCC, Clang itself** | Built with LTO + PGO (and sometimes BOLT for post-link layout): 5-20 % end-to-end speed-ups on browsers/compilers; the work is in the build system and the training workload |
| **Linux distributions** | Build packages with `-O2` plus hardening (`-D_FORTIFY_SOURCE=2`, `-fstack-protector-strong`, `-fcf-protection`: the `endbr64` you saw in the assembly); Ubuntu 24.04 enables frame pointers |
| **Game engines** | `-O2`/`-O3` per-file, `-ffast-math` per-file for physics, `restrict`-qualified data-oriented loops, SIMD intrinsics for hot loops, and unity builds to substitute for cross-TU inlining |
| **Numerical libraries** | Use `-march=` dispatch: ship several code paths (SSE2/AVX2/AVX-512), pick at run time with `__builtin_cpu_supports` or function multiversioning (`target_clones`) |
| **Embedded** | `-Os`/`-Oz`, `-ffunction-sections -Wl,--gc-sections`, LTO to shrink |
| **Qt** | Qt's own binaries use `-O2` with `-fvisibility=hidden` and `-fno-semantic-interposition`-like options to allow inlining inside the shared library; applications benefit from LTO of their own code but cannot inline across the Qt `.so` boundary (the ABI boundary of Chapter 37) |

> **Opinion.** **The best optimisation advice is five lines.** (1) Ship `-O2` unless a measurement says otherwise. (2) Turn on LTO for applications you build as a whole; it is cheap and often the biggest free win. (3) Use `-O3`, `-march=`, PGO per target **only after** a benchmark on your workload shows a gain bigger than its noise. (4) Never put `-ffast-math`/`-Ofast` in a global flag; opt in on the one translation unit where you've tested for it and where NaN/inf can't occur. (5) When the compiler seems to be doing something stupid, read *its explanation* (`-fopt-info`, `-Rpass`) and fix the code it was unable to prove things about. Most "compiler bugs" are UB; most "slow abstractions" are missed inlining.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| **Believing `-O3` is always faster** | Slower, bigger binaries (i-cache pressure) | Measure; keep `-O2` as default |
| **Carrying "O2 vs O3" folklore between compilers** | Clang at `-O2` already vectorises; GCC's `-O2` is conservative (Experiment 1) | Re-measure per compiler |
| **`-Ofast` / `-ffast-math` globally** | `isnan` false, inconsistent results, non-IEEE behaviour in libraries that include your headers | Opt in per TU; unit-test numerics; prefer targeted flags (`-fno-math-errno`, `-fno-trapping-math`) |
| **`-march=native` in a distributed binary** | `Illegal instruction` on customer machines; non-reproducible builds | Use a fixed baseline (`x86-64-v2/v3`), or function multiversioning with run-time dispatch |
| **UB hidden by `-O0`** | "Works in debug, breaks in release" | `-fsanitize=undefined,address` in CI; fix the UB rather than adding `-fwrapv` unless you want those semantics |
| **Expecting `inline` to inline** | Hot call stays a call | Look at `-fopt-info-inline-missed`; `[[gnu::always_inline]]` as a last resort |
| **Cross-TU hot calls without LTO** | 4-6× slower tiny functions (Experiment 2) | Define in header or `-flto` |
| **PGO with unrepresentative training** | Regressions on real workloads | Train on production-like data; refresh the profile; verify with a benchmark |
| **Stale/mismatched profile** | No gain, only a `-Wmissing-profile` warning | Same binary name/object paths; check warnings; automate in CI |
| **Mixing `-flto` objects from different compiler versions** | Link-time errors / ICEs / silent non-optimisation | Use the same compiler for all objects; don't ship LTO static libs |
| **Debugging optimised code** | "variable optimised out", jumpy stepping | `-Og` or a separate `-O0` build; `-g3`; don't judge behaviour by single-stepping |
| **`restrict`-style promises that are false** | Silent wrong results | `__restrict` only when proven; run under sanitizers/valgrind in tests |
| **Hand-written intrinsics before checking the compiler** | Unmaintainable code that's no faster | Read the vectoriser's reason first; try `-march`, `__restrict`, loop restructuring |
| **Assuming FP results are identical between builds** | Different last digits with `-O3`, `-march=native` (FMA), or Clang/GCC | `-ffp-contract=off` for reproducibility; compare with tolerances |
| **Huge `constexpr`/template-heavy TUs at `-O3`** | Compile time and memory blow-up | Optimise only the TUs that matter (`#pragma GCC optimize` / per-file flags) |

---

## 12. Exercises

1. **Reproduce Experiment 1** on your machine for both compilers, adding `-O3 -march=x86-64-v3` and `-Os`. Plot time vs level for each kernel.
2. **Make `sum_f` fast without `-Ofast`.** Restructure it (four lanes, `std::reduce` with `unseq`, `#pragma omp simd reduction`) and compare to the `-Ofast` build. What numerical difference do you observe on random data? Quantify it.
3. **Why wasn't it vectorised?** Take three loops from your own code and collect the compiler's missed-vectorisation reasons. Fix each reason in turn.
4. **Devirtualisation lab.** Extend Experiment 3: add a factory in another TU returning `Shape*`; measure a loop calling `area()` with (a) no LTO, (b) `-flto`, (c) `-flto -fwhole-program`, (d) PGO. Which combination removes the indirect call?
5. **LTO cost/benefit.** Measure build time, link time, peak memory and runtime of a 50-file project with `-flto`, `-flto=auto`, and Clang `-flto=thin`.
6. **UB vs optimisation.** For each assumption in the table of §4, write a 5-line function whose optimised code differs depending on `-fwrapv` / `-fno-strict-aliasing` / `-fno-delete-null-pointer-checks`. Show the assembly diff.
7. **Aliasing.** Write `saxpy(float*, const float*, float, int)`; check whether GCC emits an alias check (`-fopt-info-vec`), then add `__restrict` and compare the assembly and timings.
8. **PGO for real.** Pick a program with several modes of operation (e.g. a compiler, an interpreter). Train on workload A, then measure on workloads A and B. Report both gains.

---

## 13. Challenge: tune a build with evidence

Take a C++ project of your choice (≥ 20 files, with a benchmark or realistic workload). Produce a **build tuning report**: baseline `-O2`; then, one at a time, `-O3`, `-march=x86-64-v3`, `-flto`, `-fno-semantic-interposition` (if shared libs), PGO, and (if applicable) per-file `-ffast-math`. For each: speed-up with CV (Chapter 39), binary size, build time, and any behavioural change (tests, numerical diffs). Use the profiler (Chapter 40) to explain at least one gain and one non-gain at the instruction level (assembly or optimisation report). The deliverable is a recommended flag set and a justification for each flag **not** adopted.

---

## 14. Knowledge check

1. Why is inlining called the "enabler" of other optimisations?
2. Which standard rule gives the compiler its freedom, and which three things does it *not* license?
3. Why does `sum_f` not speed up from `-O1` to `-O3` but does at `-Ofast`?
4. What is the difference between GCC's and Clang's `-O2` regarding vectorisation (on these versions)?
5. What does LTO change at link time that a per-TU compile cannot do?
6. Under what four conditions can a virtual call be devirtualised?
7. What is speculative devirtualisation and which compiler did it in Experiment 3?
8. Why did `always_true(int)` compile to `mov eax, 1` and `still_checks(unsigned)` not?
9. What does `-Ofast` add on top of `-O3`, and what silent failure did Experiment 5 show?
10. Why is `-march=native` unsuitable for a distributed binary?
11. What can make a PGO build give no improvement at all?
12. Give three reasons `-O3` can be slower than `-O2`.

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. After inlining, the callee's body is in the caller's scope: arguments become constants, aliasing and types become known, loops can merge, dead code appears; most optimisations are intra-procedural and need that visibility.
2. The as-if rule. It does not license changing observable behaviour (volatile accesses, I/O, results), ignoring required behaviour on well-defined programs, or (outside copy/allocation elision) skipping side effects of constructors; and it does not license anything on programs relying on UB being "defined".
3. FP addition isn't associative; vectorising a sum reorders the additions and could change the result. Only `-fassociative-math` (part of `-ffast-math`/`-Ofast`) permits it.
4. GCC 14's `-O2` uses only a very-cheap vectorisation cost model (no runtime checks/peeling), so most loops stay scalar until `-O3` or `-ftree-vectorize`; Clang 18's `-O2` already runs the full loop vectoriser.
5. Whole-program view: cross-TU inlining, constant propagation across files, dead function removal, devirtualisation with global knowledge, better layout.
6. The object's type is known in the function; the class/method is `final`; whole-program/LTO visibility shows a single implementation; or the compiler speculates on the likely target and guards it with a vtable check (GCC).
7. Compare the vtable slot to the expected function pointer and run an inlined direct body if equal; otherwise take the indirect call. GCC 14 did this for the non-final `Rect`; Clang 18 `-O2` did not.
8. Signed overflow is UB, so `x + 1 > x` is assumed true; unsigned arithmetic wraps, so the comparison has real meaning (false when `x == UINT_MAX`).
9. `-ffast-math` and related flags (finite-math-only, associative-math, no math errno…). `std::isnan(NaN)` returned 0 because the compiler assumed no NaNs exist.
10. It enables whatever instruction sets the build machine has (AVX2/AVX-512, BMI…) and the binary crashes with an illegal-instruction fault on older CPUs; it also makes builds non-reproducible.
11. A training run that doesn't match the final workload; a profile file that doesn't match the binary (ignored with `-Wmissing-profile`); code that changed since the profile; branch behaviour that is inherently unpredictable (Experiment 4).
12. Code bloat from inlining/unrolling (i-cache and iTLB pressure), over-aggressive vectorisation with expensive setup/remainder code on short trip counts, and different register pressure/scheduling choices that happen to be worse for the particular kernel.

</details>

---

[← Previous: Chapter 40](40-profiling.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 42 — API design →](../part-16-library-design/42-api-design.md)
