# Chapter 40 — Profiling: Find the Cost Before You Fix It

> **Part XV · Performance engineering** &nbsp;|&nbsp; **Level 5** (systems/hardware) &nbsp;|&nbsp; **≈ 8 hours**
> **Prerequisites:** [Chapter 39 (benchmarking)](39-benchmarking.md), [Chapter 24 (dynamic memory)](../part-10-memory/24-dynamic-memory.md), [Chapter 27 (cache and data-oriented C++)](../part-10-memory/27-cache-and-data-oriented-cpp.md) &nbsp;|&nbsp; **Standards:** none; this chapter is about tools and the OS/CPU layers &nbsp;|&nbsp; **Tools:** `perf` 6.8, FlameGraph, Valgrind (`callgrind`, `cachegrind`, `dhat`), `g++-14`

[← Previous: Chapter 39](39-benchmarking.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 41 — Compiler optimization →](41-compiler-optimization.md)

---

**In one sentence:** a profiler answers the question *"where does the time (or the memory, or the cache-missing) actually go?"* so that you can spend your effort on the 5 % of the code that owns 80 % of the cost, and stop when the profile says the rest is not worth touching.

**By the end of this chapter you can:**

- explain how a sampling profiler works, what *skid*, *inlining* and *frame pointers* do to its answers
- record, read and compare `perf` profiles (self vs inclusive time, call graphs, flame graphs)
- use Valgrind to get **exact** instruction counts, simulated cache misses and allocation counts when hardware counters are not available
- run the loop *measure → profile → change one thing → measure*, and know when to stop (Amdahl)
- tell apart CPU-bound, memory-bound, allocation-bound and waiting-bound programs

> [!IMPORTANT]
> **Environment honesty.** This course's machine is a VM with **no hardware performance counters**: `perf stat -e cycles` prints `<not supported>`. All sampling below uses the software clock (`-e cpu-clock`), and cache behaviour is *simulated* by Valgrind, not measured by the CPU. Both are legitimate and I say which one is in use at each step. On bare metal, repeat the experiments with `-e cycles` and the real counters: that is Exercise 1.

---

## 1. Problem

Every programmer's intuition about where time goes is wrong often enough to be dangerous. The usual failure sequence:

```text
"This is slow."  →  guess the culprit  →  rewrite it carefully  →  gain 2 %  →  repeat
```

The guess is typically a *visible* suspect (the clever algorithm, the virtual call, the `shared_ptr`), while the real cost is in something nobody looked at (an `istringstream`, a `std::map` of strings, three heap allocations per input line, a page-fault storm during setup). A profile replaces the guess with evidence:

| Question | Tool family |
|---|---|
| Which functions take the CPU time? | Sampling profiler (`perf record`) |
| Who *calls* the expensive function, and how often? | Call-graph profile, flame graph, `callgrind` |
| How many instructions / allocations / cache misses, exactly? | Instrumenting/simulating tools (`callgrind`, `dhat`, `cachegrind`) |
| Why is a given instruction slow? | Hardware counters (`perf stat`, `perf annotate`): bare metal only |
| Where does the program *wait* rather than compute? | Off-CPU analysis, `strace -c`, `ltrace`, tracing |

The discipline this chapter teaches is one sentence: **measure the whole program first, profile it second, change one thing third, measure again fourth.**

---

## 2. Historical context

| Era | Tool / idea |
|---|---|
| 1982 | **gprof**: compile-time instrumentation (`-pg`) plus periodic sampling; call counts exact, times statistical, accuracy limited by its attribution model |
| 1990s–2000s | **Valgrind** (2002+, `callgrind`/`cachegrind`): dynamic binary translation; exact counts, 20–100× slowdown. **OProfile**, then **perf_events** (Linux 2.6.31, 2009) |
| 2009– | `perf`: sampling driven by the CPU's performance-monitoring unit (PMU): cycles, instructions, cache misses, branch misses, with low overhead (~1–5 %) |
| 2011 | **Flame graphs** (Brendan Gregg): the whole sampled call tree as one picture, widths proportional to time |
| 2010s | Intel LBR/PEBS, `perf` with DWARF unwinding, eBPF-based profilers, continuous profiling in production (Google-Wide Profiling lineage) |
| 2020s | Frame pointers are back in distributions (Fedora 38, Ubuntu 24.04 rebuilt core libraries with them) because *unwinding is the profiler's weakest link*; heap profilers (`heaptrack`, `dhat`, jemalloc/tcmalloc profilers) are routine |

---

## 3. Modern solution

There is no C++ feature involved. The modern method is:

```text
 1. Define a workload   (representative input, fixed seed, runs ≥ 1 s)
 2. Time the whole thing, with a spread          ← Chapter 39
 3. Profile it:   perf record -g     → flame graph / report
 4. Read the top of the profile; ask "why is this here?"
 5. Confirm with a second tool (exact counts / allocations) before changing code
 6. Change ONE thing. Re-time. Re-profile.
 7. Stop when the profile is flat or the remaining cost is not yours to reduce.
```

Build for profiling like this:

```bash
g++ -O2 -g -fno-omit-frame-pointer app.cpp -o app        # same optimisation as production, plus symbols and frame pointers
perf record -e cpu-clock -F 5000 --call-graph fp ./app   # -e cycles on bare metal
perf report --no-children --stdio                        # self time per function
```

`-O2 -g` is the key: **profile the optimised build you ship**, with debug info added (debug info does not change the generated code). A `-O0` profile profiles the wrong program.

---

## 4. Mental model

### A sampling profiler is a statistical camera

```text
  time ──────────────────────────────────────────────────────────────────►
  program:   [ parse ][ parse ][ lookup ][ lookup ][ lookup ][ alloc ][ sort ]...
  timer:        ▲         ▲        ▲         ▲        ▲         ▲        ▲
                │         │        │         │        │         │        │
  samples:   parse     parse    lookup    lookup   lookup     alloc    sort
              each ▲ = "interrupt; record the program counter (and the call stack)"
  report:    lookup 3/7 = 43 %,  parse 2/7,  alloc 1/7,  sort 1/7
```

Every ~200 µs (at `-F 5000`) the kernel interrupts the program and records where it is. After thousands of samples, the *fraction* of samples that landed in a function approximates the fraction of time spent there. Consequences:

- **It measures time, not calls.** A function called once for 3 seconds and a function called 10⁹ times for 3 seconds look the same.
- **It is statistical.** With 1 000 samples, a function showing 1 % is `10 ± 3` samples; do not chase 1 % differences.
- **Overhead is low and constant**, unlike instrumentation, so it does not distort what it measures much.
- **It needs the stack.** To attribute time to *callers*, it must walk the stack at each sample; how it does that matters (below).

### Self time vs inclusive time

```text
   main ─┬─ aggregate ── split ── malloc     self(aggregate) = time in aggregate's own instructions
         │                └─ ...             inclusive(aggregate) = self + everything it calls (split, malloc, memcmp...)
         └─ make_log
```

**Self** tells you where cycles are *burned*; **inclusive** tells you which *subtree* to look in. Start with inclusive to find the subtree, then self to find the instruction-level culprit. The two are the `--children` / `--no-children` switches of `perf report`.

### How stacks are recovered (and why that goes wrong)

| Method | How | Cost | Weakness |
|---|---|---|---|
| **Frame pointers** (`--call-graph fp`) | Follow the chain of saved `rbp` registers | Cheap, small data | Needs `-fno-omit-frame-pointer` in *all* code; **leaf functions that don't set up a frame are skipped** (Experiment 2) |
| **DWARF** (`--call-graph dwarf`) | Copy a slice of the stack (e.g. 16 KB) per sample, unwind offline using `.eh_frame` | **~150× more data per sample** (Experiment 2), slower | Needs debug/unwind info; truncated deep stacks |
| **LBR** (`--call-graph lbr`) | Hardware last-branch records | Cheap | Intel-only, bare metal, shallow |

### Skid and inlining: the two ways a profile misattributes cost

- **Skid.** The sample records the PC *after* the interrupt is taken, which may be several instructions past the one that was slow; `perf annotate` blames the *next* instruction. Precise events (PEBS on Intel, IBS on AMD) fix this but need the PMU; with `cpu-clock` assume skid.
- **Inlining.** The profiler sees *machine code*. After inlining, `aggregate` and `split` are literally part of `main`'s instructions. Debug info lets `perf` map them back (`-g`), but a profile of an inlined build is a profile of **the compiler's output, not of your functions**. Mark things you want to see as `[[gnu::noinline]]` while profiling, or read inline frames (`perf report --inline`).

### Amdahl's law is the stopping rule

If a part takes fraction *p* of the total and you make it *s* times faster, the whole becomes faster by `1 / ((1 − p) + p/s)`. Even infinitely fast code for a 25 % part yields at most **1.33×** overall. The profile tells you *p*; it tells you when to stop.

### Four kinds of "slow"

```text
  CPU-bound         high IPC, time in your own loops           → better algorithm / vectorisation / fewer instructions
  Memory-bound      low IPC, time stalled on loads              → better layout, fewer cache misses (Ch. 27)
  Allocation-bound  time in malloc/free/new/delete/page faults  → fewer allocations; arenas (Ch. 26)
  Waiting-bound     CPU idle: I/O, locks, sleeps                → off-CPU analysis, strace, tracing
```

A CPU profile only sees the first three, and only when the program is *running*.

---

## 5. Language rules

The language says nothing about profiling, but several language and ABI facts determine what a profiler can see:

| Fact | Layer | Why it matters for profiling |
|---|---|---|
| **As-if rule**: functions may be inlined, merged, split (`.cold` clones), tail-called | ⚖️ standard | Your source functions need not exist at run time (Chapter 39) |
| **Inlining/cloning is a compiler decision** (`.constprop`, `.isra`, `.part`, `.cold` suffixes) | 🔧 compiler | Profiles show `split(...) [clone .constprop.0] [clone .isra.0]`: it is still your function, specialised |
| **Frame pointer** is optional in the SysV x86-64 ABI | 🧩 ABI | Omitted at `-O2` by default → broken `fp` call graphs unless `-fno-omit-frame-pointer` |
| **Red zone / leaf functions** need no frame | 🧩 ABI | Hand-written assembly such as libc's `memcmp` pushes no `rbp`, so its *caller* is lost under fp unwinding (Experiment 2) |
| **Unwind tables (`.eh_frame`)** are emitted by default on x86-64 Linux | 🧩 ABI | They are what DWARF unwinding uses; stripped binaries still unwind |
| **Symbols**: C++ names are mangled | 🧩 ABI | `perf report` demangles (needs `-g` or symtab); `c++filt` otherwise |
| **PMU access** (`perf_event_open`) | OS + CPU | Hypervisors often hide the PMU; `kernel.perf_event_paranoid` limits non-root use |

### Layer check

| Layer | Question for a profile |
|---|---|
| **C++ standard** | Nothing is promised about code shape; the profile reflects the compiler's output, not your source |
| **Compiler** | Which functions survived inlining? Are the hot loops vectorised? (Chapter 41) |
| **ABI** | Can the profiler find the callers (frame pointers, `.eh_frame`)? |
| **OS** | Timer/PMU interrupts, page faults (visible as `clear_page_erms`, `do_user_addr_fault`), scheduling |
| **CPU** | Stalls on cache/TLB misses and branch mispredictions: visible only with PMU counters |

---

## 6. Implementation model

### What `perf record` actually does

1. Opens an event with `perf_event_open(2)` (here the software event `cpu-clock`, firing on a timer at 5 kHz per CPU; on bare metal usually `cycles`, firing every N CPU cycles).
2. On each overflow, the kernel's interrupt handler writes a record into a ring buffer mapped into `perf`: PC, PID/TID, timestamp, and the call chain (by one of the methods above).
3. `perf record` drains the ring buffer into `perf.data`. `perf report` symbolises addresses with the ELF symbol tables and DWARF line info.

### Valgrind is a different animal

`callgrind`, `cachegrind` and `dhat` **execute your program on a software CPU** (dynamic binary translation). They count *every* instruction and *every* modelled memory access, so results are exact and deterministic (no sampling noise), but:

| | `perf` sampling | Valgrind tools |
|---|---|---|
| Overhead | 1–5 % | 20–100× |
| Result | Statistical **time** | Exact **counts** (instructions, simulated cache refs/misses, allocations) |
| Cache information | Real hardware events (bare metal) | A *model*: simple LRU caches with configured sizes; no prefetcher, no out-of-order overlap |
| Works in a VM | Software events only | Yes |

Use `perf` to find *where*; use Valgrind to find *how many* and as a **proxy for the counters you can't read here**. Treat its cache misses as a precise description of an idealised machine, not a measurement of yours.

### The allocation view: `dhat`

`dhat` interposes `malloc`/`free` and records every block with its size, lifetime and the call stack that allocated it: total blocks and bytes, peak (`t-gmax`), and what was still live at exit (`t-end`). That is the number to look at when `malloc`, `_int_free` and `operator new` appear in the CPU profile.

---

## 7. Experiments

Everything is in [`code/ch40/`](code/ch40/); `bash run.sh` reproduces it. The subject is `app.cpp`: a log aggregator that sums the byte counts per user over generated log lines (`user123,GET,/item/456,789`). Three versions, chosen with `-DV`:

- **v1**: the obvious code: `istringstream` + `getline`, `split` into `vector<string>`, `std::stol`, `std::map<std::string,long>`.
- **v2**: removes the copies and the stream (`string_view`, `from_chars`), still `std::map`.
- **v3**: replaces the `map` with `unordered_map<string_view, long>`.

All three print identical results (`5000 users, top=user2226 (7192534 bytes)`). Hardware: 2 vCPUs of a VM, GCC 14.2 `-O2 -g -fno-omit-frame-pointer`. The timed region is `aggregate` + `sort`; the input is generated first (`make_log`), which turns out to matter.

### Experiment 1 🔧: Time first (Chapter 39 applied)

Three runs each, 500 000 lines:

```text
v1: aggregate+sort 291 ms   318 ms   355 ms        <- ±10 %: noisy VM
v2:                101 ms   103 ms   111 ms
v3:                 39 ms    40 ms    36 ms
```

The ratios (~3× then ~2.7×) are far outside the noise. Keep them in mind: this is the answer the profile has to *explain*.

### Experiment 2 🔧: Profile v1; see why call graphs lie

```bash
perf record -e cpu-clock -F 5000 --call-graph fp -o v1.data ./app1 2000000
perf report -i v1.data --no-children --stdio -g none --percent-limit 3      # SELF time
perf report -i v1.data --children    --stdio -g none --percent-limit 4      # INCLUSIVE time
```

```text
INCLUSIVE                                                                                 SELF
 99.84%  main                                                                              0.02%
 51.92%  aggregate(std::string const&)                                                     17.43%
 27.06%  make_log(int)                                                                     7.65%
 19.46%  split(std::string const&, char) [clone .constprop.0] [clone .isra.0]             13.51%
 15.54%  __memmove_evex_unaligned_erms                                                     6.98%
 12.22%  __memcmp_evex_movbe                                                              12.22%
  8.63%  kernel: asm_exc_page_fault  (page faults: first touch of freshly allocated memory)     0.00%
  6.92%  cfree / free                                                                      3.34%
  6.28%  std::to_string(unsigned long)                                                     6.28%
```

How to read it, in the order a professional would:

1. **Inclusive first.** `aggregate` owns ~52 %, `make_log` ~27 % (this is *test setup*, not the thing we time: **a whole-program profile includes everything**; filter to the region you care about or you will optimise the generator), `split` ~19 %.
2. **Self next.** Inside `aggregate`, `memcmp` has 12 % *self*. Nothing in the source says `memcmp`: it is the string comparison inside `std::map`'s tree search.
3. **Who calls `memcmp`?** The frame-pointer call graph says:

```text
 main
  └─ __memcmp_evex_movbe              <-- WRONG: aggregate and std::map are missing
```

`memcmp` is a hand-written assembly leaf; it does not push `rbp`, so the fp unwinder skips its immediate caller. Switch to DWARF unwinding on a smaller run:

```bash
perf record -e cpu-clock -F 2000 --call-graph dwarf,16384 -o v1d.data ./app1 500000
```

```text
 main → aggregate → std::map::operator[] → lower_bound → _M_lower_bound → std::less → operator<=> → basic_string::compare → char_traits::compare → memcmp
```

Now it is exact: **the comparison inside `std::map<std::string,…>::operator[]`**. The price: the fp profile was 0.9 MB for 8 573 samples (~106 bytes/sample); the DWARF one 11.5 MB for 723 samples (~16 000 bytes/sample), **about 150× more data per sample** because each sample copies 16 KB of stack.

> [!TIP]
> **Rule of thumb.** Build with `-fno-omit-frame-pointer`, use `--call-graph fp` by default, and when a leaf function (libc `mem*`/`str*`, `malloc`) shows a suspicious caller, repeat that one run with `--call-graph dwarf`.

The `main` frame is also instructive. In my *first* profile, before I marked `aggregate`/`split` `noinline`, the report said `main` had 32 % self time and 83 % inclusive: the compiler had inlined both into `main`, so the profile described one big function. **A flat or lumpy profile often means "inlined", not "unlucky".**

### Experiment 3 ✅: Flame graph

```bash
perf script -i v1.data | FlameGraph/stackcollapse-perf.pl > v1.folded
FlameGraph/flamegraph.pl --title "app v1" v1.folded > v1.svg
```

[`code/ch40/v1.svg`](code/ch40/v1.svg) is the result (open it in a browser; it is interactive). The folded file's heaviest stacks (units are ns of sampled time):

```text
298.8M   main;aggregate                      <- aggregate's own code
231.2M   main;aggregate;split                <- the split copies
209.4M   main;__memcmp_evex_movbe            <- (fp lost aggregate here, see Experiment 2)
131.2M   main;make_log
105.8M   main;make_log;to_string
```

Reading a flame graph: **width = time, height = stack depth, left-to-right order is alphabetical, not chronological.** Look for wide *plateaus* (self time) and wide *towers* (a hot subtree). A graph is only as good as its stacks: the missing `aggregate` frame above shows up as a `memcmp` tower hanging directly under `main`.

### Experiment 4 ✅: Count allocations exactly (`dhat`)

The CPU profile shows `malloc`, `free`, `operator new`, `_M_mutate`, `memmove`, page faults. Confirm with exact counts on 200 000 lines:

```text
v1: Total: 81,365,746 bytes in 934,344 blocks
v2: Total: 30,454,973 bytes in 334,342 blocks
v3: Total: 30,365,237 bytes in 334,343 blocks
```

`make_log` accounts for ~334 000 blocks in every version (string concatenation in the generator). So `aggregate` in v1 performs **(934 344 − 334 342) / 200 000 = 3.0 allocations per input line** and ~51 MB of allocated bytes, and in v2/v3 **essentially none** (the keys are small enough for the small-string optimisation, and `string_view` copies nothing). Where do v1's three per line come from? From `split`: `vector<string>` growing 1 → 2 → 4 elements as the four fields are pushed. The individual strings (`user123`, `GET`, `/item/456`, …) are ≤ 15 characters and fit in the SSO buffer, which is why *only* the vector's growth allocates. **Without the count I would have guessed "one string allocation per field", and been wrong.**

### Experiment 5 🔧: Exact instruction counts (`callgrind`)

```text
v1: I refs: 766,425,922
v3: I refs: 316,803,678          (whole program, 200 000 lines, including make_log, which is identical)
```

The whole-program instruction count falls **2.4×**, while the timed region (excluding `make_log`) got ~8× faster. Those two numbers are not directly comparable (different scope), but the gap is suggestive: instruction count alone does not explain the speed-up; the removed work (`std::map`'s pointer-chasing comparisons, allocations, `memmove`) was also *slow* work, more likely stalled on memory than a typical instruction. **I can't confirm the stall hypothesis without hardware counters**; I'm flagging it as an inference, not a result.

### Experiment 6 ✅: The profile after the fix (when to stop)

```text
v3 (self time):   make_log 19.6 %   aggregate 19.6 %   to_string 12.7 %   memmove 8 %   memcmp 5.8 %   page faults ~10 %
```

The thing we set out to fix (`aggregate`) is now the same size as the *test data generator*. That is the stopping signal: another 2× on `aggregate` would improve the program by about 10 %, and `make_log` is not part of the product. (Amdahl: `p ≈ 0.2`, even `s = ∞` gives ≤ 1.25×.)

### Experiment 7 🔧: Simulated cache misses: array-of-structs vs struct-of-arrays

`layout.cpp` sums one `double` field out of 1M particles, once stored as 64-byte structs (AoS) and once as a separate `std::vector<double>` (SoA).

```cpp
// @test run -std=c++20 -O2
#include <cstdio>
#include <vector>
struct P { double x, y, z, vx, vy, vz, mass, charge; };            // 64 bytes: one cache line per particle
struct SoA { std::vector<double> x, y, z, vx, vy, vz, mass, charge; };

[[gnu::noinline]] double sum_aos(const std::vector<P>& v) { double s = 0; for (auto& p : v) s += p.x; return s; }
[[gnu::noinline]] double sum_soa(const SoA& v)            { double s = 0; for (double x : v.x) s += x;  return s; }

int main() {
    const int n = 1 << 20;                                            // 1M particles: 64 MiB as AoS, 8 MiB for x alone
    std::vector<P> a(n, P{1, 2, 3, 4, 5, 6, 7, 8});
    SoA b; b.x.assign(n, 1.0);
    std::printf("%.0f %.0f\n", sum_aos(a), sum_soa(b));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
1048576 1048576
```

Under `valgrind --tool=cachegrind --cache-sim=yes` the two functions give:

| Function | Data read | D1 read misses (simulated) |
|---|---:|---:|
| `sum_aos` | 1 048 577 loads | **1 048 577** (one per particle) |
| `sum_soa` | 1 048 577 loads | **131 074** (one per 8 doubles) |

Exactly the arithmetic of a 64-byte line: AoS touches one line per element (it needs 8 bytes of a 64-byte line); SoA gets 8 elements per line. **The misses are 8.0× fewer, deterministically.** The wall-clock effect depends on whether the data fits in cache. Same total work (16 M element reads), best of 15, `asm volatile` barrier per call:

| Particles | AoS | SoA | Ratio | Footprint (AoS / SoA) |
|---:|---:|---:|---:|---|
| 1 024 | 19.5 ms | 19.4 ms | 1.0× | 64 KiB / 8 KiB (L2 / L1) |
| 16 384 | 21.1 ms | 20.5 ms | 1.0× | 1 MiB / 128 KiB (L2) |
| 262 144 | 47.7–51.6 ms | 20.8–21.0 ms | **2.3–2.5×** | 16 MiB / 2 MiB (L3 / L2) |
| 4 194 304 | 110–113 ms | 23–24 ms | **4.6–4.9×** | 256 MiB / 32 MiB (RAM / L3) |

The layout costs nothing while everything fits in the cache and 5× once it spills. The SoA time is flat at ~21 ms (compute-bound: 16 M adds), the AoS time climbs with the size (memory-bound). *This table is the whole argument of Chapter 27, produced by one profiler run and one timing loop.*

> [!WARNING]
> **A trap I fell into while measuring this.** My first version of the timing loop called `sum_aos(a)` repeatedly with the same argument. `sum_aos` is `noinline` but *pure*, so GCC computed it **once** and reused the result: the first run reported 0.02 ms for 16 M reads (a thousand times too fast). I only noticed because the number was impossible. The fix is the barrier in the timing loop (`asm volatile("" : : "r"(a.data()) : "memory")`). The lesson is Chapter 39's: *a number that is too good is a bug until proven otherwise.*

### Experiment 8 ✅: What the VM cannot do

```text
$ perf stat -e cycles,instructions,cache-misses ./spin
     <not supported>      cycles
     <not supported>      instructions
     <not supported>      cache-misses
```

So here: no IPC, no hardware cache-miss count, no branch-miss count, and no precise (PEBS) sampling. On bare metal the next step of Experiment 7 would be `perf stat -e cycles,instructions,L1-dcache-load-misses,LLC-load-misses ./layout`, and the instructions-per-cycle number would confirm the memory-bound diagnosis directly (IPC well below 1 for AoS, near the compute limit for SoA). This chapter's cache claims are therefore: **exact for the simulated cache, plausible and consistent with the timings for the real one, not counter-verified.**

---

## 8. Assembly / runtime investigation

```bash
# Annotate: which instructions in a function carry the samples? (skid: blame is on the instruction AFTER the slow one)
perf annotate -i v1.data --stdio -s app1 'aggregate(std::__cxx11::basic_string<char> const&)' | less

# Resolve inlined frames in the report
perf report -i v1.data --stdio --inline --no-children

# Per-callee view / callers of a symbol
perf report -i v1.data --stdio --no-children -G -S malloc

# System-call and library-call summaries (waiting-bound programs)
strace -c -f ./app1 200000          # time and count per syscall
ltrace -c ./app1 200000             # per library call (slow)

# Callgrind: interactive view
callgrind_annotate cg1.out | head -30            # exact instruction counts per function / source line
kcachegrind cg1.out                                  # (GUI) call graph with costs

# Exact allocation hot-spots
valgrind --tool=dhat ./app1 200000                   # then open dh_view.html
valgrind --tool=massif ./app1 200000 && ms_print massif.out.*     # heap over time

# Bare metal only
perf stat -e cycles,instructions,branches,branch-misses,L1-dcache-load-misses,LLC-load-misses ./app1
perf record -e cycles:pp -c 100003 --call-graph lbr ./app1        # precise sampling + hardware call stacks
perf c2c record ./app1; perf c2c report                           # false-sharing detector (Chapter 32)
```

**Reading assembly with samples.** For a hot loop, `perf annotate` shows each instruction with the percentage of samples that landed on it. A single instruction with 30 % of the loop's samples is where the pipeline waits; look one instruction *earlier* for the cause (skid), usually a load that missed or a long-latency divide.

---

## 9. Implementation exercise

**A poor man's sampling profiler.** Build one in ~80 lines for your own programs, to understand the mechanism:

1. Install a `SIGPROF` handler with `setitimer(ITIMER_PROF, …)` at 1 kHz.
2. In the handler, record the interrupted PC from the `ucontext_t` (`uc->uc_mcontext.gregs[REG_RIP]`). Store it in a pre-allocated lock-free array (a handler must be **async-signal-safe**: no `malloc`, no `printf`, no locks).
3. At exit, symbolise the PCs with `dladdr`, count per function, print the top 10.
4. Run it on `app1` and compare to `perf report`.

<details>
<summary><strong>Solution sketch</strong></summary>

```cpp
#include <csignal>
#include <cstdio>
#include <dlfcn.h>
#include <sys/time.h>
#include <ucontext.h>
#include <atomic>
#include <map>
#include <string>
#include <cxxabi.h>

namespace {
constexpr int kMax = 1 << 16;
void*              pcs[kMax];
std::atomic<int>   n{0};

void on_prof(int, siginfo_t*, void* ctx) {                       // async-signal-safe: only stores
    auto* uc = static_cast<ucontext_t*>(ctx);
    int i = n.load(std::memory_order_relaxed);
    if (i < kMax) { pcs[i] = reinterpret_cast<void*>(uc->uc_mcontext.gregs[REG_RIP]); n.store(i + 1, std::memory_order_relaxed); }
}
}

void profiler_start() {
    struct sigaction sa{}; sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    itimerval it{{0, 1000}, {0, 1000}};                          // 1 kHz of CPU time
    setitimer(ITIMER_PROF, &it, nullptr);
}

void profiler_report() {
    itimerval off{}; setitimer(ITIMER_PROF, &off, nullptr);
    std::map<std::string, int> hist;
    for (int i = 0; i < n.load(); ++i) {
        Dl_info info;
        std::string name = dladdr(pcs[i], &info) && info.dli_sname ? info.dli_sname : "?";
        int st; char* d = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &st);
        if (d) { name = d; free(d); }
        ++hist[name];
    }
    // print the top entries by count...
}
```

Link with `-rdynamic` (so `dladdr` sees the executable's symbols). Compare with `perf`: you will find the same top functions but **no call graph** and worse attribution inside inlined code, which is precisely what `perf`'s unwinder and debug-info machinery buy you.

</details>

---

## 10. Real-world example

| Where | What the profile revealed |
|---|---|
| **Google-Wide Profiling / continuous profiling** | Always-on sampling of production fleets found that a handful of library functions (`memcpy`, hashing, allocation, protobuf parsing, compression) account for a large share of all CPU cycles: the "datacenter tax" papers |
| **Compilers** (GCC, Clang) | `-ftime-trace` / `-ftime-report` profile the *compiler* itself: header parsing and template instantiation dominate; this is what drove modules (Chapter 38) |
| **Browsers** | Flame graphs of layout and style recalculation; most startup time is page faults and relocation, not computation |
| **Databases** | Instruction-count and cache-miss profiling (`perf stat`, `callgrind`) turned "row at a time" into vectorised/columnar execution (data layout, Chapter 27) |
| **Qt applications** | `perf` on a GUI app shows time in painting, text layout and `QString` allocations rather than in application logic; Qt Creator ships a `perf` and `valgrind` integration. Attribute cost to the **event handler** first (slot → paint → layout), not to individual `QString` operations |

> **Opinion.** **Never optimise without a profile, and never stop optimising without one.** The profile of v1 pointed at `split` and `std::map<std::string,…>`, not at the "clever" parts; the fixes were *deleting work* (copies, allocations, a stream) and *choosing a better container*, not micro-tuning. Notice also what did **not** happen: no `shared_ptr` to remove, no virtual call to devirtualise, no hand-vectorised loop. Most real wins come from the boring layers: allocations, copies, data structures, I/O. And keep a **performance test** (Chapter 39) next to every fix so it can't silently regress: a profile is a snapshot, a benchmark is a guard rail.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| **Profiling `-O0`** | Time is in `std::vector::operator[]`, iterators, `std::move` | Profile the optimised build you ship: `-O2 -g` |
| **No frame pointers / no debug info** | Flat profile; `[unknown]`; broken callers | `-g -fno-omit-frame-pointer`; or `--call-graph dwarf` |
| **Trusting the caller of a leaf function** | `memcmp`/`malloc` appears directly under `main` (Experiment 2) | Re-run with DWARF for that case |
| **Profiling setup/teardown** | You optimise the test-data generator (Experiment 2: 27 % `make_log`) | Profile only the region of interest (`perf record -D`, `--control`), or run it long enough that setup is negligible |
| **Optimising self time of a function nobody can call less** | You speed up a callee while its *caller* calls it 1000× too often | Check inclusive time and call counts (`callgrind`); fix the caller's algorithm |
| **Chasing < 3 % items** | Statistical noise (a 1 % function in a 1 000-sample profile is ±3 samples) | More samples (`-F`, longer run), or ignore |
| **Reading "skidded" annotations literally** | Blames an innocent instruction | Look at the instructions just before; use precise events on bare metal |
| **Profile of a different workload** | Fast in the lab, slow in production | Capture production-shaped input; profile in production if you can (low overhead) |
| **Assuming CPU-bound** | Time is in `read`, `futex`, `poll`, page faults | `strace -c`, off-CPU profiling, check `sys` vs `user` time |
| **Valgrind's cache numbers as hardware truth** | "0 L2 misses!" on a machine with prefetchers | Valgrind models a simple cache; use as a *relative* comparison and confirm with counters when available |
| **Comparing profiles across builds with different inlining** | Functions "disappear" | Compare totals and inclusive time, not individual function self time |
| **Changing three things at once** | You don't know which helped | One change → measure → profile → commit |
| **Stopping at the first win** | Leaves the next 3× on the table | Re-profile after each fix; stop when the profile is flat or Amdahl says no |
| **Optimising allocation-heavy code by tuning `malloc`** | Marginal gains | Remove allocations (SSO, reserve, `string_view`, arenas: Chapter 26) |
| **Instrumentation overhead distorting a profile** (`gprof -pg`, `callgrind` on timing-sensitive code) | Hot spots shift | Use sampling for time, instrumentation for counts |

---

## 12. Exercises

1. **Counters.** On a machine with a PMU, redo Experiment 7 with `perf stat -e cycles,instructions,L1-dcache-load-misses,LLC-load-misses`. Compute IPC and misses per element for AoS and SoA at 16 K and 4 M particles. Do they agree with the simulated cache?
2. **Flame graph reading.** Open `v1.svg` and name the three widest plateaus. Predict the effect of each possible fix *before* looking at v2/v3.
3. **fp vs dwarf.** Record the same run with `--call-graph fp`, `dwarf` and (if available) `lbr`. Find a function whose caller is wrong under one method. Measure the size and the run-time overhead of each recording.
4. **Inlining vs profile.** Rebuild v1 without `[[gnu::noinline]]` on `aggregate`/`split` and profile it. Use `--inline` to recover the function names. How many frames are lost? How does `-fno-inline` change the *speed* of the program?
5. **Allocation budget.** Use `dhat` to find the three allocation sites responsible for most blocks in v1. Eliminate them one at a time and record the block count after each.
6. **Amdahl.** For the v1 profile compute the best possible overall speed-up if (a) `aggregate` became free, (b) `make_log` became free. Compare with what v3 achieved.
7. **The 80-line profiler.** Finish §9's sampler, then explain two differences between its output and `perf report` for the same run.
8. **Off-CPU.** Write a program that spends 90 % of its wall time in `read()` on a pipe. Show that `perf record -e cpu-clock` hardly sees it, and that `strace -c -f` or `/usr/bin/time -v` does.

---

## 13. Challenge: profile-guided investigation

Take a C++ program you didn't write (an open-source tool of ~10–50 kLOC; a JSON parser, a regex engine, a ray-tracer). Define a 5-second representative workload. Produce a written report with: the whole-program timing with spread; a flame graph and the top-10 self-time functions; an allocation profile (`dhat`/`heaptrack`); a cache simulation or counter profile of the hottest function; **three** concrete changes ranked by expected gain via Amdahl; and then the *measured* result of the first change, plus the new profile. If your estimate and the measurement disagree, explain why. The best reports include a change you tried that did **not** help.

---

## 14. Knowledge check

1. What does a sampling profiler measure, and what does it not measure?
2. Why does the inclusive time of `main` read ~100 % while its self time is ~0 %?
3. What goes wrong with frame-pointer unwinding in libc's `memcmp`, and how do you get the real caller?
4. Why can a profile of an `-O2` build show `main` with large self time?
5. What does `dhat` give you that `perf` does not? What did it tell us about v1?
6. What is *skid*, and what avoids it on real hardware?
7. In Experiment 7, why is the number of AoS read misses *exactly* the number of elements, and the SoA number *exactly* one-eighth?
8. Why was the AoS/SoA ratio 1.0× at 1 024 particles and 4.6–4.9× at 4 M?
9. When is Valgrind's cache simulation misleading?
10. State Amdahl's law and apply it to Experiment 6.

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. It samples the program counter (and stack) at timer/PMU interrupts, so it estimates the fraction of *on-CPU time* spent in each place. It doesn't measure call counts, exact timing of short events, or time spent *off* the CPU (blocking, waiting).
2. Inclusive time counts everything below `main`; self time counts only `main`'s own instructions. `main` calls everything but does little itself.
3. `memcmp` is assembly that sets up no frame pointer, so the unwinder skips its immediate caller and shows `main` as the parent. Use `--call-graph dwarf` (copy the stack and unwind with `.eh_frame`) or LBR on supporting CPUs.
4. The compiler inlined the callees into `main`; their instructions are `main`'s. Use `-g` and `perf --inline`, or mark functions `noinline` for profiling.
5. Exact allocation counts, sizes, lifetimes and allocation call stacks. v1 performed 3.0 allocations per input line, all from `vector<string>` growth in `split` (the strings themselves were SSO), vs ~0 in v2/v3.
6. The recorded PC is that of an instruction after the one whose latency caused the sample. Precise event sampling (PEBS/IBS) attributes to the right instruction.
7. A 64-byte line holds one 64-byte `P` (so each element's read touches a new line) and eight 8-byte doubles in the SoA array (so one miss serves eight elements); the simulated cache is deterministic.
8. At 1 024 particles both layouts fit in L1/L2 (64 KiB / 8 KiB) and the loop is compute-bound; at 4 M the AoS footprint (256 MiB) streams from RAM and fetches 8× more cache lines, while SoA (32 MiB) is mostly L3 with perfect sequential prefetching.
9. When behaviour depends on hardware features it doesn't model: prefetchers, out-of-order overlap of misses, TLBs, shared/associative-cache details, SMT, or when sizes differ from your real CPU. It is a good *relative* tool, not an absolute one.
10. Speed-up = 1 / ((1 − p) + p/s). With `aggregate` ≈ 20 % of runtime (p = 0.2), even s = ∞ gives at most 1 / 0.8 = 1.25×; stop optimising it.

</details>

---

[← Previous: Chapter 39](39-benchmarking.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 41 — Compiler optimization →](41-compiler-optimization.md)
