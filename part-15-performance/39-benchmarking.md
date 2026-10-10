# Chapter 39 — Benchmarking: Measuring Without Fooling Yourself

> **Part XV · Performance engineering** &nbsp;|&nbsp; **Level 4** (compiler/runtime) &nbsp;|&nbsp; **≈ 6 hours**
> **Prerequisites:** [Chapter 27 (cache and data-oriented C++)](../part-10-memory/27-cache-and-data-oriented-cpp.md), [Chapter 36 (compilation model)](../part-14-compilation-and-linking/36-compilation-model.md) &nbsp;|&nbsp; **Standards:** nothing here is in the standard; the *as-if rule* ⚖️ is what makes benchmarking hard &nbsp;|&nbsp; **Tools:** Google Benchmark 1.8.3 (`apt install libbenchmark-dev`), `g++-14`, `objdump`

[← Previous: Chapter 38](../part-14-compilation-and-linking/38-modules.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 40 — Profiling →](40-profiling.md)

---

**In one sentence:** a microbenchmark does not time *your abstraction*, it times *whatever the compiler turned your abstraction into*, on *that data*, on *that machine, at that moment*, so every benchmark is an experiment that needs a control, a check of the generated code, and an estimate of its own noise.

**By the end of this chapter you can:**

- recognise the four ways a benchmark silently measures nothing (dead code, constant folding, hoisting, wrong data)
- use Google Benchmark correctly (`DoNotOptimize`, `ClobberMemory`, `Arg`/`Range`, repetitions, aggregates) and write a minimal harness of your own
- report a result with its variance, and know which statistic (min, median, mean) answers which question
- sweep a parameter (data size, predictability) instead of measuring one point
- say what a microbenchmark can **not** tell you, and when to measure the whole program instead

---

## 1. Problem

You wrote two versions of a function and want to know which is faster. You write a loop, call each a million times, print the times. The numbers come out, one is "3× faster", you ship the change. What went wrong, in order of how often it happens:

| Failure | What you actually measured |
|---|---|
| **The work was deleted** (dead-code elimination) | An empty loop |
| **The work was done once** (hoisting / constant folding) | The cost of one call, divided by a million |
| **The inputs were unrepresentative** | A perfectly predicted, perfectly cached best case |
| **The machine was busy or changed state** | Your neighbour's compile job; a frequency step; a cold cache |
| **The two sides were not built the same way** | One side inlined into the benchmark, the other not |
| **One run, no error bar** | A sample of size one from a noisy distribution |

None of these produce an error message. All of them produce a plausible number. That is what makes benchmarking dangerous: **a wrong measurement looks exactly like a right one.**

> [!NOTE]
> Chapter 14 already contained one example: a ranges pipeline that appeared 3.5× slower than a hand loop, until both sides were isolated behind `noinline`. The "slowdown" was an inlining difference in the *harness*, not a property of ranges.

---

## 2. Historical context

| Era | Practice |
|---|---|
| 1990s | `clock()` around a loop. Resolution of milliseconds; compilers did little dead-code elimination, so naive loops mostly worked |
| 2000s | Optimising compilers (GCC 4.x+, MSVC) delete unused results; the "volatile sink" trick appears. `rdtsc` timing, with all its pitfalls (frequency scaling, out-of-order execution, VM trapping) |
| 2013 | **Google Benchmark** (`DoNotOptimize`, automatic iteration counts, statistics); Chandler Carruth's talks on benchmark pitfalls popularise `asm volatile("" : "+r"(x) : : "memory")` |
| 2015– | Statistical methods become mainstream: repetitions, median/CV reporting, Mann–Whitney tests (`benchmark/tools/compare.py`), "Stabilizer" (Curtsinger & Berger) showing that code and stack *layout* alone can swing results by double digits |
| 2020s | Continuous benchmarking in CI (noise is the main enemy), `perf stat` counters next to timings, cloud VMs as the default (and noisiest) measurement platform |

---

## 3. Modern solution

There is no language feature; there is a **method**:

```text
1. State the question           ("is A faster than B for N≈10⁴ ints in L1?")
2. Make the work un-deletable   (DoNotOptimize results, ClobberMemory writes)
3. Make the inputs un-foldable  (runtime values; realistic data)
4. Look at the code             (objdump / -S: is the thing you think you're timing there?)
5. Control the environment      (release build, quiet machine, same flags both sides)
6. Repeat and summarise         (≥ 10 repetitions; min for "capability", median for "typical")
7. Sweep a parameter            (size, predictability), never a single point
8. Cross-check at a higher level (does the whole program agree?)
```

Google Benchmark automates steps 2, 3 (partly), 6, 7:

```cpp
static void BM_Sum(benchmark::State& st) {
    std::vector<int> v(st.range(0), 1);                 // setup: not timed
    for (auto _ : st) {                                  // the library chooses the iteration count
        benchmark::DoNotOptimize(v.data());              // "v may have changed": forbids hoisting
        long s = std::accumulate(v.begin(), v.end(), 0L);
        benchmark::DoNotOptimize(s);                     // "s is used": forbids deleting the loop
    }
    st.SetItemsProcessed(st.iterations() * v.size());
}
BENCHMARK(BM_Sum)->RangeMultiplier(8)->Range(8, 1 << 20);
```

---

## 4. Mental model

### A benchmark is a *tiny program* the compiler optimises as a whole

```text
                  your source                              what the CPU runs
   ┌───────────────────────────────────┐       ┌────────────────────────────────────┐
   │ for (auto _ : st) {               │       │                                    │
   │     int s = work(v);              │  ───► │   (nothing: s is never observed,   │
   │ }                                 │  -O2  │    work() is pure, loop deleted)   │
   └───────────────────────────────────┘       └────────────────────────────────────┘
```

The compiler does not know it is looking at a benchmark. It applies the **as-if rule** (⚖️ [intro.abstract]): any transformation that preserves *observable behaviour* (I/O, volatile accesses, the final program result) is allowed. Time is not observable behaviour. So the compiler may delete, hoist, fold, vectorise or unroll the thing you meant to measure.

### The two ways to lie to the optimiser

| Tool | What it tells the compiler | Prevents |
|---|---|---|
| `DoNotOptimize(x)` | "`x` is read (and may be modified) by something you cannot see" | Deleting the computation of `x`; folding it into a constant |
| `ClobberMemory()` | "all memory may have been read or written" | Hoisting loads/stores out of the loop; keeping data in registers across iterations |
| `asm volatile("" : "+r"(x) : : "memory")` | The same, hand-rolled (this is what the library does) | The same |

Neither inserts instructions of its own (an empty `asm`), only **forces the value to exist in a register or memory at that point**. They are *barriers for the compiler, not for the CPU*: they cost almost nothing but also order nothing in hardware.

### Variance is data, not an annoyance

Run-to-run differences come from the OS scheduler, interrupts, CPU frequency, cache and TLB state, address-space layout (ASLR changes alignment), and, in a VM or a container, your neighbours. The right summary depends on the question:

| Question | Statistic | Why |
|---|---|---|
| "What is the best this code can do?" (an algorithm comparison) | **min** of N | Noise only adds time; the minimum is the least-disturbed run |
| "What will users typically see?" | **median** (+ spread) | Robust to outliers |
| "Did this commit make it slower?" | **distribution comparison** (e.g. Mann–Whitney on ≥ 10 repetitions each) | Differences of a few percent are indistinguishable from noise without it |
| "Is the system stable enough to measure?" | **coefficient of variation** (stddev/mean) | If CV is 10 %, a "5 % improvement" means nothing |

### What a microbenchmark can and cannot answer

A microbenchmark has a **warm cache, a trained branch predictor, a hot instruction cache, a private allocator state, and often a single thread**. That is a *capability* number. The production program has none of those luxuries. Use microbenchmarks to compare two implementations of the *same* small thing under *controlled* conditions, and profile the real program (Chapter 40) to learn what matters at all.

---

## 5. Language rules

| Topic | Rule |
|---|---|
| **As-if rule** ⚖️ | The implementation may perform any transformation that preserves the program's observable behaviour: reads/writes of `volatile` objects, calls to library I/O functions, and (since C++11) the termination behaviour. **Elapsed time is not observable behaviour**, so being faster is always conforming |
| **Allocation elision** ⚖️ (C++14 [expr.new]) | A *new-expression* may be merged or omitted if the allocation is unobservable. So `new`/`delete` pairs in a benchmark can vanish (Chapter 1, 5.2) |
| **`volatile`** ⚖️ | Accesses to `volatile` objects are observable and cannot be removed or merged. Therefore `volatile` *does* defeat dead-code elimination, but it also forces a memory store/load that your real code does not have: it distorts what you measure. Prefer `DoNotOptimize` |
| **`asm` statements** 🔧 | `asm` is conditionally-supported; GCC/Clang's extended `asm volatile` with a `"memory"` clobber is a compiler extension and is how benchmark libraries implement `DoNotOptimize`. MSVC has no inline asm on x64; its library version uses `_ReadWriteBarrier`-like intrinsics |
| **`std::chrono::steady_clock`** ⚖️ | Monotonic, but its *resolution* and *cost* are implementation-defined. On Linux/glibc it is `clock_gettime(CLOCK_MONOTONIC)` through the vDSO: tens of nanoseconds per call, which is why you time a *batch* of iterations, never one call |
| **`high_resolution_clock`** ⚖️ | Often an alias of `system_clock` (which can jump), implementation-defined. Do not use it for intervals |
| **Inlining** 🔧 | Not specified by the standard in any binding way (`inline` is a linkage hint). Whether the function under test is inlined into the loop is a compiler decision that changes what is measured |

### Layer check

| Layer | What it contributes to measurement noise or error |
|---|---|
| **C++ standard** | Permits deletion/hoisting (as-if); says nothing about time |
| **Compiler** | Decides inlining, vectorisation, if-conversion (branch → `cmov`), alignment; **differs by version and flags** (Experiment 3) |
| **ABI** | Calling convention decides whether arguments are in registers; `noinline` calls pay it, inlined ones don't |
| **OS** | Scheduler migrations, interrupts, page faults on first touch, ASLR, CPU-frequency governor, other processes |
| **CPU** | Frequency scaling and turbo, cache/TLB state, branch predictor training, SMT neighbours, thermal throttling. **Hardware counters are not available in this VM** (see §8), so the cache and branch claims below come from timings, not counter values |

---

## 6. Implementation model

### How `benchmark::State` works

```text
 for (auto _ : state)   ──►  begin():  start timer (lazily, after setup)
                              each iteration: ++counter; compare to the target
                              end():    stop timer
```

The library runs the function with iteration count 1, 10, 100, … until the total time exceeds `--benchmark_min_time`, then reports `time / iterations`. The loop itself costs about one increment and one compare per iteration, which is why it can only resolve work of a few cycles or more, and why a body that is "0.3 ns" (Experiment 1) is a warning, not an achievement: **that is about one cycle: you timed the loop counter.**

### `DoNotOptimize` in two lines

```cpp
template <class T> inline void escape(T& v) { asm volatile("" : "+r,m"(v) : : "memory"); }
inline void clobber()                       { asm volatile("" : : : "memory"); }
```

The empty `asm` emits no instruction. `"+r,m"(v)` means "this block reads and writes `v`, which must be in a register *or* memory"; `"memory"` means "it may touch any memory". The optimiser must therefore keep `v` alive and fully computed *before* the block and must assume it changed *after* it.

### What the library cannot fix

- **Layout effects**: moving a function by 16 bytes changes alignment relative to the fetch/decode boundary and cache lines; effects of ±10 % are documented in the literature for otherwise identical code. If a 3 % difference matters, you have to randomise layout (or just don't believe it).
- **Different inlining decisions on the two sides**. Check with `objdump`.
- **Allocator state**: a benchmark that allocates after thousands of identical allocations sees a perfectly warmed free list.

---

## 7. Experiments

All code is in [`code/ch39/`](code/ch39/); `bash run.sh` reproduces everything. Numbers are real runs on this machine: **2 vCPUs of a VM, GCC 14.2 `-O2`, Google Benchmark 1.8.3**. They show *shapes* and *ratios*; the digits will differ on yours.

### Experiment 1 ✅: Four benchmarks of "sum 1000 ints"

```cpp
// @test compile -std=c++20 -O2 link=-lbenchmark,-lpthread
#include <benchmark/benchmark.h>
#include <numeric>
#include <vector>

static void BM_Naive(benchmark::State& st) {            // BROKEN: result unused, input constant
    std::vector<int> v(1000, 1);
    for (auto _ : st) { int s = std::accumulate(v.begin(), v.end(), 0); (void)s; }
}
BENCHMARK(BM_Naive);

static void BM_Escaped(benchmark::State& st) {           // result escapes, memory possibly changed
    std::vector<int> v(1000, 1);
    for (auto _ : st) {
        benchmark::DoNotOptimize(v.data());
        int s = std::accumulate(v.begin(), v.end(), 0);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_Escaped);

static void BM_Constant(benchmark::State& st) {          // everything known at compile time
    for (auto _ : st) {
        int s = 0;
        for (int i = 0; i < 1000; ++i) s += i;
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_Constant);

static void BM_Runtime(benchmark::State& st) {           // size is only known at run time
    int n = static_cast<int>(st.range(0));
    for (auto _ : st) {
        benchmark::DoNotOptimize(n);
        int s = 0;
        for (int i = 0; i < n; ++i) s += i;
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_Runtime)->Arg(1000);
BENCHMARK_MAIN();
```

```text
BM_Naive             0.000 ns        0.000 ns   1000000000
BM_Escaped             126 ns          121 ns      2324639
BM_Constant          0.323 ns        0.321 ns    896712836
BM_Runtime/1000        440 ns          436 ns       622024
```

Read it as a detective:

- **`BM_Naive` = 0.000 ns, one billion iterations.** The library hit its iteration cap because the body takes no time at all: the loop is empty. The "result" `s` is discarded, `accumulate` is pure, so GCC deleted the call. *A benchmark that reports 0 ns has measured nothing.*
- **`BM_Constant` = 0.32 ns.** That is about one cycle. The sum `0+1+…+999` is a compile-time constant; the loop is replaced by `mov eax, 499500`. What remains is the cost of the harness's own loop counter.
- **`BM_Escaped` = 126 ns** and **`BM_Runtime` = 440 ns** are real work. Notice they differ by 3.5× for the "same" 1000 additions. `objdump` shows SSE vector instructions in *both* functions, so "vectorised versus scalar" is **not** the explanation, and I did not dissect the two loops further (a good exercise: compare their inner loops, unrolling and dependency chains). The lesson stands without the explanation: *two correct measurements of two different pieces of generated code are not interchangeable*, and the number you want is the one for the code your program actually compiles to.

### Experiment 2 ✅: The assembly agrees

```cpp
// @test asm -std=c++20 -O2 -fno-stack-protector filter=dead,escaped
#include <numeric>
#include <vector>

void dead(const std::vector<int>& v) {
    int s = std::accumulate(v.begin(), v.end(), 0);
    (void)s;
}

void escaped(const std::vector<int>& v) {
    int s = std::accumulate(v.begin(), v.end(), 0);
    asm volatile("" : "+r"(s));                          // what DoNotOptimize does
}
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
dead(std::vector<int, std::allocator<int> > const&):
	ret

escaped(std::vector<int, std::allocator<int> > const&):
	mov	rcx, QWORD PTR 8[rdi]
	mov	rax, QWORD PTR [rdi]
	xor	edx, edx
	cmp	rax, rcx
	je	.L4
.L5:
	add	edx, DWORD PTR [rax]
	add	rax, 4
	cmp	rax, rcx
	jne	.L5
.L4:
	ret
```

`dead` is a single `ret`; `escaped` still contains the loop (the barrier forces `s` to exist). **Whenever a number looks too good, read the assembly before believing it.** It takes one minute and is the single most effective benchmarking habit.

### Experiment 3 🔧: The compiler removed the effect you wanted to measure

Branch prediction: sum only elements `≥ 128` from 65 536 bytes of random values, once with random order and once sorted (identical instructions, identical data, different *predictability*).

```text
default  -O2:                                              random   42 470 ns    sorted   44 091 ns     <- no difference!
-O2 -fno-if-conversion -fno-if-conversion2 -fno-tree-vectorize:
                                                           random  282 533 ns    sorted   53 986 ns     <- 5.2x
```

The first row is the instructive one. At plain `-O2`, GCC 14 turned the branch into a conditional move (`cmovg`; confirmed with `objdump`), so there is **no branch to mispredict**, and the "famous sorted-array effect" is absent. The classic experiment only shows its effect when the compiler leaves a real branch, which here required switching off the optimisations that remove it. Neither number is wrong. They answer different questions: *"what does mispredicting cost?"* (second row) versus *"does it matter in my build?"* (first row: not for this code).

> [!WARNING]
> Textbook microbenchmark results are statements about a **compiler version and flags**, not about C++. Always run the experiment in *your* build configuration before acting on someone else's number.

### Experiment 4 🔧: Sweep, don't sample

A pointer-chase through a random cycle of `n` bytes (every load depends on the previous one, so nothing can be overlapped or prefetched) measures **memory latency** as the working set outgrows each cache level. 1000 dependent loads per iteration:

| Working set | Time per 1000 loads | per load | Fits in |
|---:|---:|---:|---|
| 4 KiB | 1.57 µs | 1.6 ns | L1 (32 KiB) |
| 32 KiB | 1.63 µs | 1.6 ns | L1 |
| 256 KiB | 4.9 µs | 4.9 ns | L2 (1 MiB) |
| 2 MiB | 18.7 µs | 19 ns | L3 |
| 16 MiB | 123 µs | 123 ns | L3/RAM edge |
| 128 MiB | 203 µs | 203 ns | RAM (+ TLB misses) |
| 256 MiB | 235 µs | 235 ns | RAM |

Eight data points tell a story one point never could: a **150× spread** for the *same code*, with plateaus at the cache boundaries (L1 32 KiB, L2 1 MiB, L3 33 MiB as reported by the library's own header: `L2 Unified 1024 KiB`, `L3 Unified 33792 KiB`). The jump between 16 MiB and 128 MiB is memory plus TLB misses; these are timings, and I cannot attribute them to the TLB without counters, which this VM does not expose. A benchmark of "how long does a lookup take" that used a 4 KiB table would report the first row and be wrong for any table that matters.

### Experiment 5 ✅: How noisy is *this* machine?

Ten repetitions of `BM_Escaped` (aggregates only):

```text
quiet machine:
BM_Escaped_mean          122 ns          121 ns           10
BM_Escaped_median        121 ns          120 ns           10
BM_Escaped_stddev       4.65 ns         3.56 ns           10
BM_Escaped_cv           3.81 %          2.95 %            10

two busy-loop processes competing for the 2 vCPUs:
BM_Escaped_mean          195 ns          125 ns           10
BM_Escaped_median        191 ns          121 ns           10
BM_Escaped_stddev       28.7 ns         8.41 ns           10
BM_Escaped_cv          14.71 %          6.74 %            10
```

Columns: the first is **wall time**, the second **CPU time**. Under contention the wall time rose **60 %** (122 → 195 ns) while the CPU time barely moved (121 → 125 ns): the benchmark was descheduled, not slower. Moral: (1) on a shared machine prefer CPU time for compute-bound code; (2) a CV of 4 % on a *quiet* machine already means that a "3 % speedup" is not distinguishable from noise; (3) **always print the spread.** Pinning with `taskset -c 1` did not help here (CV 4–8 % pinned, 4–5 % unpinned): inside a VM the host scheduler can still preempt the vCPU, so don't assume pinning cures noise.

### Experiment 6 ✅: A 40-line harness, and why *min* wins for comparisons

`harness.cpp` compares a hand loop with `std::accumulate` (21 interleaved rounds of 2000 reps each, 10 000 ints):

```text
run 1:  loop       min 4341 ns   median 4796 ns   max 6936 ns   (spread 59.8%)
        accumulate min 4343 ns   median 5077 ns   max 7656 ns   (spread 76.3%)
run 2:  loop       min 4390 ns   median 4490 ns   max 5045 ns   (spread 14.9%)
        accumulate min 4373 ns   median 4489 ns   max 5694 ns   (spread 30.2%)
```

The **medians** moved by up to 6 % between two runs of the same binary; the **minima** agree to under 1.5 % (4341 vs 4390; 4343 vs 4373), and show that the two functions are the same speed, which is plausible: both compile to a simple reduction loop (I did not diff the two functions' assembly). Two techniques worth stealing: **interleave** A and B rounds so that drift (thermal, neighbours) hits both equally, and use `min` to compare capabilities.

---

## 8. Assembly / runtime investigation

```bash
# Is the thing I'm timing in the binary?
objdump -d --no-show-raw-insn -C ./realistic | awk '/<branchy\(int const\*, unsigned long\)>:/,/ret/'
#    look for: cmov (branchless), vector registers (xmm/ymm), or a loop at all

g++ -O2 -S -fverbose-asm -o - bench.cpp | less            # the whole story for one TU
g++ -O2 -fopt-info-vec-optimized -c bench.cpp             # which loops did GCC vectorise?
clang++ -O2 -Rpass=loop-vectorize -c bench.cpp            # same, Clang

# Environment
lscpu | grep -E 'Model name|MHz|L1d|L2|L3'                # what machine, what caches
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor # "performance" is what you want (not present in this VM)
taskset -c 2 ./bench                                      # pin to a core (helps on bare metal; not here)
./bench --benchmark_repetitions=20 --benchmark_report_aggregates_only=true
./bench --benchmark_out=run1.json --benchmark_out_format=json     # then compare two runs:
python3 compare.py benchmarks run1.json run2.json                 # (Google Benchmark tools/): Mann-Whitney U-test p-values

# Hardware counters (bare metal / privileged only): this VM reports "<not supported>"
perf stat -e cycles,instructions,branch-misses,cache-misses ./bench
```

**Hardware counters were unavailable here** (`perf stat -e cycles` prints `<not supported>`), so every claim in this chapter about *why* a number moved is an inference from timings and assembly, not a counter reading. On bare metal, put `perf stat` next to every benchmark: it separates "more instructions" from "same instructions, more stalls" instantly. Chapter 40 returns to this.

---

## 9. Implementation exercise

Write `bench_util.hpp`: a header-only harness (no dependencies) providing

```cpp
template <class F> Result run(std::string_view name, F&& f, Options = {});   // auto-calibrates reps so one sample ≥ 1 ms
void escape(T&), void clobber();                                              // as in §6
struct Result { double min, median, mean, stddev; int samples; };
compare(Result a, Result b) -> "A is X% faster (min) / difference within noise"
```

Requirements: calibrate the repetition count automatically; interleave A/B in `compare`; report CV; refuse to print a verdict when the spread of either side exceeds the claimed difference.

<details>
<summary><strong>Solution sketch</strong></summary>

```cpp
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <vector>

struct Result { double min, median, mean, stddev; int samples; };

template <class F> double sample_ns(F& f, long reps) {
    auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < reps; ++i) f();
    auto dt = std::chrono::steady_clock::now() - t0;
    return std::chrono::duration<double, std::nano>(dt).count() / reps;
}

template <class F> long calibrate(F& f) {                 // find reps so that one sample takes >= 1 ms
    long reps = 1;
    while (sample_ns(f, reps) * reps < 1e6) reps *= 2;
    return reps;
}

inline Result summarise(std::vector<double> x) {
    std::sort(x.begin(), x.end());
    double mean = std::accumulate(x.begin(), x.end(), 0.0) / x.size(), var = 0;
    for (double v : x) var += (v - mean) * (v - mean);
    return { x.front(), x[x.size() / 2], mean, std::sqrt(var / x.size()), int(x.size()) };
}

template <class A, class B> void compare(A&& a, B&& b, int rounds = 21) {
    long ra = calibrate(a), rb = calibrate(b);
    std::vector<double> xa, xb;
    for (int i = 0; i < rounds; ++i) { xa.push_back(sample_ns(a, ra)); xb.push_back(sample_ns(b, rb)); }
    Result A = summarise(xa), B = summarise(xb);
    double diff = (B.min - A.min) / A.min * 100;
    double noise = std::max(A.stddev / A.mean, B.stddev / B.mean) * 100;
    if (std::abs(diff) < 2 * noise) std::printf("difference %.1f%% is within noise (CV %.1f%%)\n", diff, noise);
    else                            std::printf("B is %+.1f%% vs A (min), CV %.1f%%\n", diff, noise);
}
```

The "within 2 CV" rule is a crude significance test, adequate for deciding whether to *look further*; for decisions that matter use a rank test across ≥ 10 repetitions each (Google Benchmark's `compare.py` does this).

</details>

---

## 10. Real-world example

| Where | Benchmarking pitfall that bit |
|---|---|
| **`std::sort` vs hand-written sort** | A famous 2000s result "my quicksort beats `std::sort`" used already-sorted or constant-length data, so the library's introsort fallback and branch behaviour differed from what real data does |
| **Hash-map comparisons** | Benchmarks with 1000 keys and sequential integers fit in L1 and have no collisions; real workloads have millions of keys and clustered hashes (the cache-miss story of Experiment 4) |
| **Allocator benchmarks** | `malloc`/`free` of the same size in a tight loop is a free-list pop/push, which says nothing about fragmentation in a long-running server (Chapter 26) |
| **Compiler/library upgrades** | A 5 % regression "caused by the new compiler" that was an alignment change of one hot loop: reproducible with `-falign-functions`/`-falign-loops` |
| **Qt** | UI code is dominated by event dispatch, painting and allocation, not arithmetic: microbenchmarking a `QString` operation in isolation says little; profile the app's real frame (Chapter 40) |

> **Opinion.** **A microbenchmark is a unit test for performance: it protects a decision you already made on other grounds; it is rarely the right way to *make* the decision.** Profile first (Chapter 40), find the function that matters, then microbenchmark *alternatives to that function* on *data captured from production*. And report three numbers, never one: the result, its spread, and the build flags. A 5 % claim without a CV is not a claim.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| **Unused result** | 0 ns, or "faster than the CPU clock" | `DoNotOptimize(result)`; read the asm |
| **Constant input** | ≈ 0.3 ns per "operation"; huge items/s | Take sizes/values from `state.range` or a runtime source; `DoNotOptimize` the inputs |
| **Hoisting of loads** | Time independent of data size | `ClobberMemory()` each iteration; `DoNotOptimize(v.data())` |
| **`-O0` benchmark** | Everything slow, abstraction penalty exaggerated (ranges, iterators, CPOs are real calls at `-O0`) | Benchmark what you ship: `-O2`/`-O3`, `NDEBUG` |
| **Debug library** | Benchmark library warns "Library was built as DEBUG" | Link a release build of the benchmark library |
| **Sanitizers on** | 2–20× slower, distorted ratios | Never benchmark with ASan/TSan/UBSan |
| **Timing the setup** | Cost of building the input dominates | `st.PauseTiming()` (expensive itself, ~100s of ns: prefer building outside the loop) or `iterations` batch |
| **One sample** | A "10 % win" that disappears on re-run | `--benchmark_repetitions=10+`, report CV; interleave A/B |
| **Different inlining on A and B** | Surprise slowdowns that vanish with `noinline` (Chapter 14) | Same harness for both; `noinline` wrappers; compare asm |
| **Best-case data** | Great numbers in the lab, mediocre in production | Realistic sizes, distributions, working sets; sweeps (Experiment 4) |
| **Warm-only measurements** | Ignores first-call costs: page faults, cold i-cache, lazy binding | If cold start matters, measure it separately (`perf stat`, `ltrace`, first-iteration timing) |
| **Frequency/thermal drift** | Later runs faster/slower | Interleave; warm up; on bare metal fix governor to `performance` and disable turbo |
| **Noisy neighbour (VM/container)** | CV of 10 %+, wall ≠ CPU time (Experiment 5) | Use CPU time; repeat at different times; use a dedicated machine for decisions |
| **Believing a ratio of different code** | "`accumulate` is 3.5× faster than a loop" (Experiment 1) | Check what each compiled to; a different loop shape is a different comparison |
| **Mistaking layout luck for speed** | 3–5 % differences that follow code placement | Re-run after unrelated edits; require effects well above the noise floor |
| **Benchmarking with `volatile`** | Adds loads and stores that production code won't have | `DoNotOptimize` |
| **Multi-threaded benchmarks without care** | False sharing in the harness; threads contend on counters | `->Threads(n)`, per-thread state, cache-line padding (Chapters 27, 32) |

---

## 12. Exercises

1. **Detective work.** Write five benchmarks that each report ≈ 0 ns for a *different* reason (unused result, constant folded, loop-invariant hoisted, `new`/`delete` pair elided, dead store). Fix each and explain the fix in terms of the as-if rule.
2. **Reproduce and flip.** Re-run Experiment 3 with `-O3`, `-O2 -fno-if-conversion`, and Clang 18. Tabulate when the sorted/random effect exists. Which row of the table would a typical release build be?
3. **The cache staircase.** Re-draw Experiment 4 on your machine with a finer sweep (powers of 2). Overlay your CPU's cache sizes (`lscpu`). Add a *sequential* (prefetchable) traversal and explain the difference.
4. **Noise floor.** Measure the CV of an empty-ish benchmark on your machine at idle, with a compile job running, and pinned vs unpinned. Decide the smallest effect you can detect.
5. **`vector` vs `list` honestly.** Benchmark `push_back` then iterate for `vector<int>` and `list<int>` at 10, 10³, 10⁵, 10⁷ elements. What does each size measure? Where does the crossover/staircase appear?
6. **Allocator realism.** Benchmark `new`/`delete` of 64 B in a tight loop, then in a loop that keeps 10⁶ live objects of mixed sizes and frees them in random order. Explain the ratio (Chapter 26).
7. **A/B with significance.** Use Google Benchmark's `compare.py` on two JSON runs with 20 repetitions. Make a deliberate 1 % change and see whether the test detects it.
8. **Your own pitfall.** Find a benchmark in a codebase you know (or one blog post) and check whether the generated code does what the author assumed. Write up the finding.

---

## 13. Challenge: settle a design argument with data

Your team argues about `std::unordered_map<std::string, int>` versus a sorted `std::vector<std::pair<std::string,int>>` with binary search for a read-mostly dictionary of 1k, 100k and 10M keys. Build the benchmark so that it can survive review: realistic keys (lengths, shared prefixes), a lookup distribution (uniform vs Zipf), hit/miss ratio, cold and warm cache variants, and a size sweep. Report min/median/CV with build flags, check the assembly of the hot loop for both, and explain every crossover in terms of cache misses, branch behaviour and hashing cost. Finish with a one-paragraph recommendation that says **where your benchmark would stop being valid.**

---

## 14. Knowledge check

1. Why can a compiler legally delete the work in a benchmark loop? Which rule?
2. What does `DoNotOptimize(x)` do, and what instructions does it emit?
3. A benchmark reports 0.3 ns/iteration for a loop body that "adds 1000 numbers". What happened?
4. Experiment 3 showed no sorted/random difference at `-O2` but a 5× difference with `-fno-if-conversion`. What does each result tell you?
5. When do you report the minimum, and when the median?
6. Why is wall time a poor statistic on a shared machine, and what did Experiment 5 show?
7. Why measure at several data sizes?
8. Why interleave A and B rounds?
9. What does a microbenchmark *systematically* leave out compared with production?
10. Why is benchmarking at `-O0` misleading for C++ specifically?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Elapsed time is not observable behaviour; the as-if rule lets the compiler perform any transformation that preserves observable behaviour (I/O, volatile accesses, results), including removing unused pure computations.
2. It tells the compiler the value is read (and possibly modified) by code it cannot see, plus a memory clobber. It is an empty `asm volatile` and emits **no instructions**; it only forces the value to exist at that point.
3. The loop was constant-folded (or deleted): 0.3 ns is about one cycle: the cost of the harness's own loop counter.
4. At `-O2` the compiler removed the branch (a `cmov`), so the benchmark doesn't measure misprediction at all, and for that code in that build it doesn't matter. With branches forced, misprediction costs ~5× on random data. They answer "does it matter in my build?" vs "what does it cost when it happens?".
5. Minimum: to compare capabilities of two implementations (noise only adds time). Median (with spread): to describe typical behaviour.
6. Wall time includes time descheduled; Experiment 5 showed wall time +60 % while CPU time moved +3 %.
7. A single size selects one regime (L1, L2, DRAM…); the sweep reveals cache boundaries and crossovers (150× spread in Experiment 4).
8. Drift (thermal, frequency, noisy neighbours) then affects both sides equally instead of whichever ran later.
9. Cache, branch-predictor, i-cache and allocator state is warm; data is small and regular; one thread; no competing code. Production has none of these.
10. C++ abstractions (iterators, ranges, `std::move`, lambdas, CPOs) are only cheap *after* inlining; at `-O0` they are real calls, so abstraction cost is exaggerated.

</details>

---

[← Previous: Chapter 38](../part-14-compilation-and-linking/38-modules.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 40 — Profiling →](40-profiling.md)
