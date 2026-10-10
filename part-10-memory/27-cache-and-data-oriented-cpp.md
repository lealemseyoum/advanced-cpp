# Chapter 27 — Cache and Data-Oriented C++

> **Part X · Memory** &nbsp;|&nbsp; **Level 5** (systems/hardware) &nbsp;|&nbsp; **≈ 8 hours**
> **Prerequisites:** [Chapter 11](../part-05-standard-library/11-modern-containers.md), [Chapter 24](24-dynamic-memory.md), [Chapter 26](26-allocators-and-memory-resources.md) &nbsp;|&nbsp; **Standards:** C++17 (`hardware_destructive_interference_size`), C++20 (`std::assume_aligned`, `[[likely]]`/`[[unlikely]]`), C++23 (`std::mdspan`, `std::flat_map`, `[[assume]]`), C++26 (`std::inplace_vector`, SIMD library) &nbsp;|&nbsp; **Tools:** `g++-14`, `lscpu`, `perf` (not installed in this sandbox; commands are shown for your machine)

[← Previous: Chapter 26](26-allocators-and-memory-resources.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 28 — Undefined behavior →](../part-11-undefined-behavior/28-undefined-behavior.md)

---

**In one sentence:** on a modern CPU the cost of a program is dominated not by instruction count but by **how far the data is from the core**, so the layout of your data structures is a performance decision that language-level abstractions can hide but not remove.

**By the end of this chapter you can:**

- describe the memory hierarchy with real latency and bandwidth numbers, and **measure them on your machine**
- predict whether a data layout will be cache-friendly and verify it with an experiment
- convert AoS to SoA (and know when *not* to), eliminate false sharing, and write branch-predictor-friendly code
- explain what hardware prefetchers do and when `__builtin_prefetch` helps
- use the resulting rules as design constraints without falling into "optimise everything" folklore

---

## 1. Problem

Chapters 11 and 26 asked "which container?" and "which allocator?". Both were really asking *where do the bytes sit, and in what order do we touch them?* This chapter makes the underlying hardware explicit, because this is where C++ abstractions stop being free:

```cpp
std::vector<Particle> ps(1'000'000);
for (auto& p : ps) p.x += p.vx * dt;        // only touches 2 of the 12 fields... yet drags all 12 through the cache
```

The code is correct, clean, "zero-cost", and several times slower than it needs to be. The standard says nothing about this; the hardware is indifferent to your type system. You need a mental model of the machine.

---

## 2. Historical context

| Era | What happened |
|---|---|
| 1980s | CPUs and DRAM run at similar speeds; a memory access costs about one instruction |
| 1990s | CPU speed grows ~50 %/year, DRAM latency ~7 %/year: the **memory wall** (Wulf & McKee, 1995). Caches become essential |
| 2000s | Multi-level caches, hardware prefetchers, out-of-order execution hide *some* latency. Game developers (Mike Acton, Noel Llopis) formulate **data-oriented design**: "the data is the problem" |
| 2005 | Multi-core: caches must be kept **coherent** (MESI); sharing costs appear: *false sharing* |
| 2014–18 | Spectre/Meltdown: microarchitectural state (caches, predictors) becomes a security concern. Mainstream awareness of cache effects rises |
| C++17 | `std::hardware_destructive_interference_size` / `constructive` standardise the cache-line constants |
| C++20/23 | `std::assume_aligned`, `[[likely]]`, `mdspan`, `flat_map`: library and language support for cache-conscious design |
| 2020s | DRAM bandwidth per core is the bottleneck for many workloads; chiplet CPUs make NUMA effects ordinary |

---

## 3. Modern solution

Not a language feature, but a **design discipline** supported by standard tools:

```cpp
alignas(64) struct Counter { std::atomic<long> v; };          // one cache line each: no false sharing
std::vector<float> xs, ys, zs;                                  // SoA: hot loops stream only what they use
std::flat_map<K, V>   /   std::vector + sorted lookup           // contiguous instead of node-based
std::span / std::mdspan                                         // views that preserve contiguous, strided access
[[likely]] / branchless arithmetic / sorting before filtering   // help the branch predictor
```

The design rules, ordered by how much they usually matter:

1. **Fewer cache lines touched** (smaller types, SoA, hot/cold splitting, no pointer chasing).
2. **Sequential access** (arrays and vectors over lists and trees; hardware prefetch works).
3. **No sharing of written lines between threads** (padding, per-thread data).
4. **Predictable branches** (sort or partition data; branchless where measured).
5. **Alignment and vectorisation** (the compiler can use SIMD on contiguous, aligned, non-aliasing data).

---

## 4. Mental model

### The hierarchy (typical x86-64 server core, this sandbox's numbers in parentheses)

```text
   registers          ~ 0 cycles      ~1 KB
   L1 data cache      ~ 4–5 cycles    32–48 KiB per core      (32 KiB, 64 B lines, 8-way)
   L2 cache           ~ 12–16 cycles  0.5–2 MiB per core      (1 MiB per core)
   L3 cache           ~ 40–70 cycles  tens of MiB, shared     (33 MiB shared)
   DRAM               ~ 200–400 cycles (60–100 ns)            GiB
   other NUMA node    ~ 1.5–3× DRAM
   SSD / network      µs–ms
```

At 2.8 GHz one cycle is 0.36 ns, so a DRAM miss costs about **a hundred or more** simple instructions of lost work. Everything below follows from that ratio.

### Cache lines: the unit of transfer

Memory moves between levels in **64-byte lines**. Touching one byte loads 64. A struct that is 12 floats (48 bytes) but of which you read 2 wastes ~96 % of the bandwidth you paid for. A line is also the unit of **coherence**: when one core writes, other cores' copies of that line are invalidated, regardless of which bytes they use (false sharing).

```text
   array of struct {x,y,z,vx,vy,vz,mass,id,...}  (48-64 B)      struct of arrays
   ┌──────────────── one line ────────────────┐                 xs: [x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 ... ]  ← 16 floats per line
   │ x0 y0 z0 vx0 vy0 vz0 m0 id0 ...          │  you use 2/12    vxs:[vx0 vx1 ...                         ]
   └──────────────────────────────────────────┘                 a hot loop over x and vx touches 2 lines per 16 particles
```

### What the hardware does to help you (and what it can't)

- **Spatial locality:** the rest of the line comes along for free. Use it or waste it.
- **Hardware prefetchers** detect sequential and constant-stride streams and fetch ahead; pointer chasing (`p = p->next`) has no pattern to detect and no memory-level parallelism, so each hop pays full latency.
- **Out-of-order execution + memory-level parallelism:** independent loads overlap (10+ outstanding misses per core). A loop that *indexes* an array can overlap misses; a loop that *chases pointers* cannot — the next address depends on the previous load.
- **Branch predictors:** speculate past branches; a misprediction costs ~15–20 cycles, plus the work discarded.
- **Store buffers and coherence (MESI):** a write needs exclusive ownership of the line. Two cores alternately writing the same line pass it back and forth at cache-to-cache latency (~50–100+ cycles each time).

> **Latency vs bandwidth.** Latency is "how long does *one* dependent access take?" Bandwidth is "how many bytes per second can a streaming access sustain?" A sequential scan is bandwidth-bound (~10–20+ GB/s per core); a pointer chase is latency-bound (one miss per ~80 ns ≈ 50 MB/s of useful data). Same memory, 200× difference.

---

## 5. Language rules

The C++ standard says almost nothing about caches. The relevant guarantees are about **layout and alignment**:

| Rule | Where | Why it matters here |
|---|---|---|
| Members are laid out in declaration order with increasing addresses (within one access-control section, in practice always); padding is implementation-defined | `[class.mem]` | Field order controls struct size. `struct {char a; double b; char c;}` is 24 bytes; `{double b; char a, c;}` is 16 (🧩 SysV x86-64) |
| `alignof`/`alignas` | `[dcl.align]` | `alignas(64)` places an object at a line boundary; over-aligned `new` is honoured since C++17 (Chapter 24) |
| `std::vector`, `std::array`, arrays are contiguous | `[vector.overview]` | The basis for streaming access and SIMD |
| `std::hardware_destructive_interference_size` / `hardware_constructive_interference_size` | `<new>` (C++17) | Portable names for "keep apart to avoid false sharing" / "keep together for locality". **GCC warns (`-Winterference-size`) that the value may vary across `-mtune` and ABI** — it is baked into your ABI. Many projects hard-code 64 (128 on Apple M-series and some ARM/POWER) |
| `std::assume_aligned<N>(p)` | `<memory>` (C++20) | Promises the compiler the pointer is `N`-aligned; UB if false |
| Data races on a non-atomic object are UB | `[intro.races]` | Padding to avoid false sharing is a **performance** fix; it does not make a race correct (Chapter 30) |
| `volatile` does not prevent caching or reordering across threads | | Not a synchronisation tool |
| Strict aliasing, `restrict`-like assumptions | `[basic.lval]` | Aliasing blocks vectorisation; `__restrict` (extension) and `std::span` of distinct arrays help |

### Layer check

| Layer | Question | Answer |
|---|---|---|
| **C++ standard** | What does it promise about layout? | Declaration-order layout, contiguity of arrays/vectors, `alignas`; nothing about cache sizes, line sizes, prefetching |
| **Compiler** | What can it do for you? | Auto-vectorise contiguous loops (`-O3`/`-O2` in GCC ≥ 12), reorder fields? **No**: C++ layout order is observable, so GCC/Clang will not reorder members for you; interchange loops (rarely); prefetch (with `-fprefetch-loop-arrays`, rarely helpful) |
| **ABI** | What fixes sizes/alignments? | SysV x86-64 alignment of fundamental types; `std::max_align_t` = 16; `__STDCPP_DEFAULT_NEW_ALIGNMENT__` = 16 |
| **OS** | What is below? | Virtual memory: 4 KiB pages, TLB (typically 64 L1 entries, ~1.5k L2); transparent huge pages (2 MiB); NUMA placement policy (first touch) |
| **CPU** | What decides performance? | Line size (64 B), associativity (8/16-way), prefetchers, MESI coherence, branch predictors, ROB size/MLP, SIMD width (SSE 16 B, AVX2 32 B, AVX-512 64 B) |

---

## 6. Implementation model

### Why associativity and strides matter

An 8-way, 32 KiB, 64-byte-line L1D has 64 sets. The set index comes from address bits 6–11. If you access addresses that are multiples of 4096 apart (e.g. column access in a 1024-int-wide matrix), they all map to the **same set** and evict each other after 8 lines — thrashing with a tiny fraction of the cache in use. A padded row length (e.g. 1024+16 ints) breaks the conflict. Experiments 2 and 6 show the cost of large power-of-two strides.

### TLB

Each access needs a virtual-to-physical translation. A TLB miss costs a page-table walk (tens to hundreds of cycles). Strided or random access over gigabytes misses the TLB on nearly every access, which is part of why very large pointer-chases degrade further. Huge pages (`madvise(MADV_HUGEPAGE)`, or `THP` set to `always`) reduce TLB pressure for big arrays and arenas (Chapter 26).

### MESI in one table

| State | Meaning | Reading | Writing |
|---|---|---|---|
| **M**odified | only this core has it, dirty | hit | hit |
| **E**xclusive | only this core, clean | hit | hit (→ M) |
| **S**hared | several cores hold it clean | hit | must invalidate others first (**RFO**, tens of ns) |
| **I**nvalid | not usable | miss | miss |

A line two cores write in alternation ping-pongs M → I → M across the interconnect: that is the entire mechanism of false sharing (Experiment 4) and of the `shared_ptr` count contention in Chapter 25.

### AoS, SoA, AoSoA

| Layout | Pros | Cons |
|---|---|---|
| **AoS** (`vector<Particle>`) | Natural; one object = one place; good when you touch most fields of one element at a time (random access by id) | Streams unused fields through the cache; hard to vectorise |
| **SoA** (parallel arrays / struct of vectors) | Hot loops stream only needed fields; trivially SIMD-friendly | Awkward ownership/API; adding/removing elements touches several arrays; per-element operations touch several lines |
| **AoSoA** (blocks of 8/16 lanes: `struct Block {float x[8], y[8]...}`) | SIMD-friendly with element locality | Complex indexing |
| **Hot/cold split** | Keep the fields touched in the hot loop in a small struct; move the rest elsewhere | Extra indirection to reach cold data |

---

## 7. Experiments

All timing experiments here report what *this sandbox* measured (2 cores, 32 KiB L1D, 1 MiB L2, 33 MiB L3). The **shape** of each result is the lesson; absolute numbers will differ on your machine. Run them on yours, with the CPU governor set to `performance` and turbo/frequency noise minimised where possible (Chapter 40).

### Experiment 1 🔧: Measure the memory hierarchy: pointer chasing

A random cyclic permutation forces every load to depend on the previous one, so there is no overlap and no prefetch: the time per hop is the **latency** of whichever level holds the working set.

```cpp
// @test run -std=c++23 -O2 timeout=180
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <random>
#include <vector>

// Sattolo's algorithm: a uniformly random single cycle through all n slots.
static void make_cycle(std::vector<std::size_t>& next, std::mt19937_64& rng) {
    std::size_t n = next.size();
    std::iota(next.begin(), next.end(), 0);
    for (std::size_t i = n - 1; i > 0; --i) { std::size_t j = rng() % i; std::swap(next[i], next[j]); }
}

int main() {
    std::mt19937_64 rng(1);
    std::puts("working set -> ns per dependent load (random pointer chase, 64-bit indices)");
    for (std::size_t kib : {4, 16, 32, 64, 256, 1024, 2048, 8192, 32768, 131072, 524288}) {
        std::size_t n = kib * 1024 / sizeof(std::size_t);
        std::vector<std::size_t> next(n);
        make_cycle(next, rng);
        std::size_t steps = 20'000'000, idx = 0;
        // warm up the working set
        for (std::size_t i = 0; i < n; ++i) idx = next[idx];
        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < steps; ++i) idx = next[idx];                  // each load depends on the previous one
        double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / steps;
        std::printf("  %9zu KiB  %7.2f ns   %s\n", kib, ns, idx == 12345678 ? "(!)" : "");
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
working set -> ns per dependent load (random pointer chase, 64-bit indices)
          4 KiB     1.53 ns   
         16 KiB     1.52 ns   
         32 KiB     1.54 ns   
         64 KiB     3.07 ns   
        256 KiB     4.00 ns   
       1024 KiB    10.32 ns   
       2048 KiB    18.58 ns   
       8192 KiB   124.68 ns   
      32768 KiB   148.34 ns   
     131072 KiB   207.04 ns   
     524288 KiB   227.96 ns   
```

What this sandbox measured: **~1.6 ns (≈ 4–5 cycles) while the set fits in L1** (≤ 32 KiB), then a climb through L2 (5.5 ns at 256 KiB, 10 ns at 1 MiB, 19 ns at 2 MiB as lines start to spill), and then a cliff: **135 ns already at 8 MiB**, rising to **255 ns at 512 MiB**. Two honest caveats:

- The nominal 33 MiB L3 does not show up as a clean plateau. This is a **cloud VM** on a shared host: the L3 is shared with other tenants, and every access past the L1 DTLB reach also pays for nested (guest + host) page walks. On bare metal you would normally see a distinct L3 step of ~15–25 ns. *Your* machine's curve is the one that matters, so measure it.
- The shape, not the numbers, is the lesson: this is how tools like `lmbench` and `tinymembench` find cache sizes without a datasheet. **The ratio between the first and last values (here ≈ 160×) is what a cache miss costs you per dependent access.**

### Experiment 2 🔧: The same bytes, different order: stride and the cache line

```cpp
// @test run -std=c++23 -O2 timeout=120
#include <chrono>
#include <cstdio>
#include <vector>

int main() {
    constexpr std::size_t N = std::size_t(1) << 26;             // 256 MiB of int: far larger than L3
    std::vector<int> a(N, 1);
    long sink = 0;
    std::puts("stride (ints) -> ns per int touched, total bytes loaded constant (every int visited once)");
    for (std::size_t stride : {1, 2, 4, 8, 16, 32, 64, 1024}) {
        auto t0 = std::chrono::steady_clock::now();
        for (std::size_t s = 0; s < stride; ++s)                // visit all elements, in stride-sized passes
            for (std::size_t i = s; i < N; i += stride) sink += a[i];
        double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N;
        std::printf("  stride %5zu (%5zu bytes): %6.3f ns/int\n", stride, stride * sizeof(int), ns);
    }
    return sink == 7 ? 1 : 0;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
stride (ints) -> ns per int touched, total bytes loaded constant (every int visited once)
  stride     1 (    4 bytes):  0.699 ns/int
  stride     2 (    8 bytes):  1.162 ns/int
  stride     4 (   16 bytes):  2.002 ns/int
  stride     8 (   32 bytes):  3.694 ns/int
  stride    16 (   64 bytes):  7.833 ns/int
  stride    32 (  128 bytes): 14.841 ns/int
  stride    64 (  256 bytes): 13.640 ns/int
  stride  1024 ( 4096 bytes): 12.653 ns/int
```

Every row reads the *same* 256 MiB and the same number of ints. With stride 1 each line serves 16 ints, and the prefetcher streams ahead. As the stride grows to a full line (16 ints) and beyond, each access needs its own line, so you pay for 64 bytes to use 4, and the per-int cost climbs towards the per-line cost. At very large strides (1024 ints = 4096 bytes = a page) you also pay TLB misses and set conflicts. This is the quantitative argument for SoA and for compact types.

### Experiment 3 ✅: AoS vs SoA: a particle update

```cpp
// @test run -std=c++23 -O2 timeout=120
#include <chrono>
#include <cstdio>
#include <vector>

struct ParticleAoS {                 // 64 bytes: exactly one cache line per particle
    float x, y, z, vx, vy, vz;
    float mass, charge;
    float color[4];
    int id, flags;
    double pad;
};
static_assert(sizeof(ParticleAoS) == 56 || sizeof(ParticleAoS) == 64);

struct ParticlesSoA {
    std::vector<float> x, y, z, vx, vy, vz;      // the hot fields
    std::vector<float> mass, charge;             // cold in this loop
};

template <class F> double best_ms(F&& f, int reps = 7) {
    double best = 1e9;
    for (int r = 0; r < reps; ++r) {
        auto t0 = std::chrono::steady_clock::now();
        f();
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    return best;
}

int main() {
    constexpr std::size_t N = 4'000'000;         // ~224 MB as AoS; well beyond cache
    std::vector<ParticleAoS> aos(N);
    ParticlesSoA soa;
    for (auto* v : {&soa.x, &soa.y, &soa.z, &soa.vx, &soa.vy, &soa.vz, &soa.mass, &soa.charge}) v->assign(N, 1.0f);
    for (auto& p : aos) p = {1, 1, 1, 1, 1, 1, 1, 1, {}, 0, 0, 0};
    const float dt = 0.01f;

    // Hot loop 1: update x from vx only
    double a1 = best_ms([&] { for (auto& p : aos) p.x += p.vx * dt; });
    double s1 = best_ms([&] { for (std::size_t i = 0; i < N; ++i) soa.x[i] += soa.vx[i] * dt; });
    // Hot loop 2: update all three coordinates
    double a2 = best_ms([&] { for (auto& p : aos) { p.x += p.vx * dt; p.y += p.vy * dt; p.z += p.vz * dt; } });
    double s2 = best_ms([&] { for (std::size_t i = 0; i < N; ++i) { soa.x[i] += soa.vx[i]*dt; soa.y[i] += soa.vy[i]*dt; soa.z[i] += soa.vz[i]*dt; } });
    // "Random access by id, touching all fields of one particle": the case where AoS should win
    std::vector<std::size_t> ids(N); for (std::size_t i = 0; i < N; ++i) ids[i] = (i * 2654435761u) % N;
    float sa = 0, ss = 0;
    double a3 = best_ms([&] { for (auto i : ids) { auto& p = aos[i]; sa += p.x + p.y + p.z + p.vx + p.vy + p.vz; } });
    double s3 = best_ms([&] { for (auto i : ids) { ss += soa.x[i] + soa.y[i] + soa.z[i] + soa.vx[i] + soa.vy[i] + soa.vz[i]; } });

    std::printf("sizeof(ParticleAoS) = %zu bytes\n", sizeof(ParticleAoS));
    std::printf("update x only (2 fields):       AoS %7.2f ms   SoA %7.2f ms   SoA is %.2fx\n", a1, s1, a1 / s1);
    std::printf("update x,y,z (6 fields):        AoS %7.2f ms   SoA %7.2f ms   SoA is %.2fx\n", a2, s2, a2 / s2);
    std::printf("random access, read 6 fields:   AoS %7.2f ms   SoA %7.2f ms   SoA is %.2fx  (<1 means AoS wins)\n", a3, s3, a3 / s3);
    return (sa + ss) < 0 ? 1 : 0;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sizeof(ParticleAoS) = 64 bytes
update x only (2 fields):       AoS   27.42 ms   SoA    6.30 ms   SoA is 4.35x
update x,y,z (6 fields):        AoS   29.69 ms   SoA   10.74 ms   SoA is 2.76x
random access, read 6 fields:   AoS  131.00 ms   SoA  294.97 ms   SoA is 0.44x  (<1 means AoS wins)
```

Three rows, three lessons. **Row 1:** when the loop touches 2 of ~14 fields, SoA streams 2 arrays (8 bytes/particle) while AoS streams the whole 56–64 bytes: AoS is bandwidth-bound by a factor of ≈ 7–8 in bytes moved. **Row 2:** touching 6 fields shrinks the gap (the AoS line is now 40–60 % useful). **Row 3:** a random-access pattern that uses most fields of one element at a time flips the result: AoS needs **one** line per particle, SoA needs **six**. *There is no universally better layout; the access pattern decides.*

### Experiment 4 ✅: False sharing

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=120
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

constexpr int kLine = 64;                                       // std::hardware_destructive_interference_size is 64 here, but GCC warns that it is ABI-fragile

struct Packed { std::atomic<long> v{0}; };                      // 8 bytes: 8 counters share one line
struct alignas(kLine) Padded { std::atomic<long> v{0}; };       // 64 bytes: one counter per line

template <class C> double run(int threads) {
    std::vector<C> counters(threads);
    constexpr long iters = 50'000'000;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&, t] { for (long i = 0; i < iters; ++i) counters[t].v.fetch_add(1, std::memory_order_relaxed); });
    for (auto& t : ts) t.join();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int main() {
    std::printf("sizeof(Packed)=%zu  sizeof(Padded)=%zu\n", sizeof(Packed), sizeof(Padded));
    for (int threads : {1, 2}) {
        double p = run<Packed>(threads), q = run<Padded>(threads);
        std::printf("%d thread(s): adjacent counters %7.1f ms   padded counters %7.1f ms   padding speed-up %.2fx\n", threads, p, q, p / q);
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sizeof(Packed)=8  sizeof(Padded)=64
1 thread(s): adjacent counters   294.8 ms   padded counters   449.3 ms   padding speed-up 0.66x
2 thread(s): adjacent counters   675.6 ms   padded counters   432.0 ms   padding speed-up 1.56x
```

Each thread increments **its own** counter: there is no logical sharing at all. In `Packed`, the counters sit in the same 64-byte line, so every increment by one core invalidates the other's copy: the line bounces between cores. Padding gives each counter its own line and removes the traffic. Notice the single-thread row: padding costs nothing there, since the effect is purely a multi-core one. (This sandbox has 2 cores; on a larger machine with more threads the penalty grows.)

### Experiment 5 🔧: Branch prediction: sorted vs unsorted (and the compiler that removes the branch)

```cpp
// @test run -std=c++23 -O2 -fno-tree-vectorize -fno-if-conversion -fno-if-conversion2 -fno-tree-loop-if-convert timeout=120
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

template <class F> double best_ms(F&& f, int reps = 7) {
    double best = 1e9;
    for (int r = 0; r < reps; ++r) {
        auto t0 = std::chrono::steady_clock::now(); f();
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    return best;
}

[[gnu::noinline]] long branchy(const std::vector<int>& v) { long s = 0; for (int x : v) if (x >= 128) s += x; return s; }
[[gnu::noinline]] long branchless(const std::vector<int>& v) { long s = 0; for (int x : v) s += (x >= 128) ? x : 0; return s; }   // compilers emit cmov / SIMD here
[[gnu::noinline]] long arithmetic(const std::vector<int>& v) { long s = 0; for (int x : v) s += x & -(x >= 128); return s; }

int main() {
    std::mt19937 rng(3);
    std::vector<int> data(1 << 22);
    for (int& x : data) x = rng() % 256;                  // ~50 % of the values pass the test: worst case for the predictor
    std::vector<int> sorted = data; std::sort(sorted.begin(), sorted.end());

    long r[6];
    double u1 = best_ms([&] { r[0] = branchy(data); }),    s1 = best_ms([&] { r[1] = branchy(sorted); });
    double u2 = best_ms([&] { r[2] = branchless(data); }), s2 = best_ms([&] { r[3] = branchless(sorted); });
    double u3 = best_ms([&] { r[4] = arithmetic(data); }), s3 = best_ms([&] { r[5] = arithmetic(sorted); });
    std::printf("                    unsorted   sorted\n");
    std::printf("if (x>=128) s+=x    %7.2f ms %7.2f ms\n", u1, s1);
    std::printf("ternary             %7.2f ms %7.2f ms\n", u2, s2);
    std::printf("mask arithmetic     %7.2f ms %7.2f ms\n", u3, s3);
    return (r[0] == r[1] && r[2] == r[3] && r[0] == r[2] && r[4] == r[5] && r[0] == r[4]) ? 0 : 1;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
                    unsorted   sorted
if (x>=128) s+=x      18.10 ms    3.83 ms
ternary               18.20 ms    3.92 ms
mask arithmetic        4.25 ms    3.34 ms
```

**Why the odd flags?** With plain `-O2`, GCC 14 *removes the branch*: it if-converts the loop to a conditional move or vectorises it with SIMD compares and blends. We measured exactly that (hand-run, not auto-verified, same source, `g++-14 -std=c++23 -O2`):

```text
                    unsorted   sorted
if (x>=128) s+=x       3.34 ms    3.41 ms
ternary                3.55 ms    3.25 ms
mask arithmetic        3.36 ms    3.29 ms
```

Every row is the same: no branch means nothing to mispredict. The flags above (`-fno-tree-vectorize -fno-if-conversion -fno-if-conversion2 -fno-tree-loop-if-convert`) force the compiler to keep the data-dependent branch so the hardware effect is visible: on random data about half of the 4M branches mispredict: the unsorted run is ≈ 3.4 ns per element slower than the sorted run, i.e. ≈ 7 ns (≈ 19 cycles at 2.8 GHz) per misprediction, matching the textbook 15–20 cycle penalty. On sorted data the predictor is almost perfect. The ternary row degrades as well because, with if-conversion disabled, the compiler lowers it to a branch too; only the arithmetic-mask version is immune by construction.

**Lessons.** (1) Branch misprediction is real and large: a factor of ~5 here. (2) A modern optimiser often eliminates the problem for simple loops, so **look at the assembly before "optimising" a branch**. (3) The robust fixes are data-side: sort/partition so branches become predictable, or restructure so the decision is arithmetic. (4) ⚖️ `[[likely]]`/`[[unlikely]]` (C++20) only influence code layout and static hints; they do not make a data-dependent branch predictable.

### Experiment 6 ✅: Row-major traversal: the loop order that matches the layout

```cpp
// @test run -std=c++23 -O2 timeout=120
#include <chrono>
#include <cstdio>
#include <vector>

int main() {
    constexpr std::size_t N = 4096;                               // 4096x4096 ints = 64 MiB; rows of exactly 16 KiB
    std::vector<int> m(N * N, 1);
    long sink = 0;
    auto time = [&](auto f) { auto t0 = std::chrono::steady_clock::now(); f(); return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };

    double row = time([&] { for (std::size_t i = 0; i < N; ++i) for (std::size_t j = 0; j < N; ++j) sink += m[i * N + j]; });
    double col = time([&] { for (std::size_t j = 0; j < N; ++j) for (std::size_t i = 0; i < N; ++i) sink += m[i * N + j]; });

    constexpr std::size_t P = N + 16;                             // padded row length breaks power-of-two set conflicts
    std::vector<int> padded(N * P, 1);
    double colp = time([&] { for (std::size_t j = 0; j < N; ++j) for (std::size_t i = 0; i < N; ++i) sink += padded[i * P + j]; });

    std::printf("row-major traversal (contiguous):       %8.2f ms\n", row);
    std::printf("column traversal, rows %zu ints (pow2): %8.2f ms   %.1fx slower\n", N, col, col / row);
    std::printf("column traversal, rows %zu ints (padded): %6.2f ms   %.1fx slower\n", P, colp, colp / row);
    return sink == 3 ? 1 : 0;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
row-major traversal (contiguous):          10.59 ms
column traversal, rows 4096 ints (pow2):   196.47 ms   18.6x slower
column traversal, rows 4112 ints (padded): 150.75 ms   14.2x slower
```

Reading the result: the column walk is **about 15–20× slower** than the row walk for identical work. Two effects stack. Every access uses 4 of 64 bytes of its line (a 16× waste), and with a 16 KiB row stride consecutive column elements map to the same few L1 sets (conflict misses) and to a different page each time (TLB misses). Padding the row by 16 ints removes the set conflicts and recovers only about a third of the loss here (222 → 150 ms): the *line-per-element* waste and the page-per-element TLB cost remain. The real fix is to traverse in memory order, or to **tile** the loops so that a block of lines is reused while it is still in L1 (Exercise 7).

`std::mdspan` (C++23) names this explicitly: `layout_right` (row-major, C++ default), `layout_left` (column-major, Fortran/BLAS), `layout_stride`. Choosing the layout to match the dominant access pattern is the same decision as AoS/SoA at a different scale.

### Experiment 7 🔧: Software prefetch: when it helps (and when it is noise)

```cpp
// @test run -std=c++23 -O2 timeout=120
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <random>
#include <vector>

template <class F> double best_ms(F&& f, int reps = 5) {
    double best = 1e9;
    for (int r = 0; r < reps; ++r) {
        auto t0 = std::chrono::steady_clock::now(); f();
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    return best;
}

int main() {
    constexpr std::size_t N = std::size_t(1) << 25;                   // 128 MiB of ints: DRAM-resident table
    std::vector<int> table(N); std::iota(table.begin(), table.end(), 0);
    std::vector<std::uint32_t> idx(1 << 22);                          // random gather indices (known in advance)
    std::mt19937 rng(5); for (auto& i : idx) i = rng() % N;

    long a = 0, b = 0;
    double plain = best_ms([&] { long s = 0; for (auto i : idx) s += table[i]; a = s; });
    double pf = best_ms([&] {
        long s = 0;
        constexpr std::size_t D = 16;                                 // prefetch distance: tune! too small = late, too large = evicted
        for (std::size_t k = 0; k < idx.size(); ++k) {
            if (k + D < idx.size()) __builtin_prefetch(&table[idx[k + D]], /*rw=*/0, /*locality=*/1);
            s += table[idx[k]];
        }
        b = s;
    });
    std::printf("random gather, 4M lookups into a 128 MiB table:\n  no prefetch   %7.2f ms\n  prefetch +16  %7.2f ms   (%.2fx)\n", plain, pf, plain / pf);
    return a == b ? 0 : 1;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
random gather, 4M lookups into a 128 MiB table:
  no prefetch     62.84 ms
  prefetch +16    59.57 ms   (1.05x)
```

**Result here: no gain** (0.96x, within noise). That is the honest, common outcome. The indices are **independent**, so the out-of-order core already has many misses in flight (memory-level parallelism); explicit prefetch only helps when the lookups are too far apart for the reorder window to see, or when you can prefetch a *batch* ahead of heavier per-element work. For a *dependent* chain (linked list, tree descent) you can't prefetch the next node until you know its address, which is the real argument for arrays and B-trees. Treat `__builtin_prefetch` as a last resort: gains are workload- and CPU-specific, and it can hurt. Measure with hardware counters, not intuition.

---

## 8. Assembly / runtime investigation

`perf` is not installed in this sandbox, but this is the investigation you run on your machine:

```bash
# (1) Count the thing you are claiming: cache misses and branch misses
perf stat -e cycles,instructions,cache-references,cache-misses,branches,branch-misses,L1-dcache-load-misses ./prog

# (2) Where do the cycles go?  function level, then instruction level
perf record -g ./prog && perf report --stdio | head -40
perf annotate --stdio -s <symbol> | head -60

# (3) False sharing detection (Linux, Intel/AMD): record cache-to-cache transfers
perf c2c record ./prog && perf c2c report --stdio | head -60       # shows lines with HITM (hit modified in another core)

# (4) Layout inspection
pahole -C Particle ./prog          # struct layout with holes and cache-line boundaries  (dwarves package)
g++-14 -std=c++23 -O0 -fdump-lang-class prog.cpp                    # GCC's own class layout dump

# (5) Hardware facts for this machine
lscpu | grep -i cache; getconf -a | grep -i CACHE; cat /sys/devices/system/cpu/cpu0/cache/index*/{level,size,ways_of_associativity}

# (6) Vectorisation: did the compiler vectorise my SoA loop?
g++-14 -std=c++23 -O3 -fopt-info-vec-optimized -c prog.cpp          # "loop vectorized using 32 byte vectors"
g++-14 -std=c++23 -O3 -fopt-info-vec-missed -c prog.cpp             # why not (aliasing? dependences? non-contiguous?)
```

Interpretation guide:

| Counter | Large value means |
|---|---|
| `cache-misses / cache-references` | Working set doesn't fit; poor locality |
| `L1-dcache-load-misses` per instruction | Streaming a large footprint or strided access |
| `branch-misses / branches` > 2–5 % | Data-dependent branches; try sorting/partitioning/branchless |
| IPC (`instructions/cycles`) < 1 on a 4-wide core | Stalled on memory or mispredicts |
| `perf c2c` HITM entries on a line | False or true sharing: padding or restructure |

---

## 9. Implementation exercise

Build a **cache-aware particle system** and a measurement harness around it:

1. Start with an AoS `std::vector<Particle>` and a simulation step (integrate positions, apply a boundary collision, accumulate kinetic energy).
2. Convert to **SoA** with a templated `SoA<Fields...>` helper that stores a tuple of vectors, offers `field<I>()` spans and `size()`, and supports `push_back`, swap-remove (O(1) delete preserving density).
3. Add a **hot/cold split**: hot (`x,y,z,vx,vy,vz`) in SoA; cold metadata (`name`, `owner`) in a separate AoS addressed by index.
4. Add **AoSoA with 8 lanes** and write the step with `std::experimental::simd` or compiler vector extensions; compare with the auto-vectorised SoA loop (`-O3 -march=native`).
5. Parallelise with two threads over disjoint index ranges; first with accumulators in an array of `double` (watch false sharing), then padded or thread-local.
6. Produce a table: bytes moved per particle per step, measured time, `perf stat` counters, for each layout.

<details>
<summary><strong>Solution sketch: a compact SoA container</strong></summary>

```cpp
// @test run -std=c++23 -O2
#include <cstdio>
#include <span>
#include <tuple>
#include <vector>

template <class... Ts>
class SoA {
    std::tuple<std::vector<Ts>...> cols_;
public:
    void push_back(const Ts&... vals) {
        std::apply([&](auto&... col) { (col.push_back(vals), ...); }, cols_);
    }
    template <std::size_t I> std::span<std::tuple_element_t<I, std::tuple<Ts...>>> field() {
        return std::get<I>(cols_);
    }
    std::size_t size() const { return std::get<0>(cols_).size(); }
    void swap_remove(std::size_t i) {                             // O(1) delete: move the last element into the hole
        std::apply([&](auto&... col) { ((col[i] = std::move(col.back()), col.pop_back()), ...); }, cols_);
    }
};

int main() {
    SoA<float, float, int> ps;                                    // x, vx, id
    for (int i = 0; i < 5; ++i) ps.push_back(float(i), 1.0f, i);
    ps.swap_remove(1);                                            // remove id 1
    auto x = ps.field<0>(); auto vx = ps.field<1>(); auto id = ps.field<2>();
    for (std::size_t i = 0; i < ps.size(); ++i) x[i] += vx[i] * 0.5f;
    for (std::size_t i = 0; i < ps.size(); ++i) std::printf("id=%d x=%.1f\n", id[i], x[i]);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
id=0 x=0.5
id=4 x=4.5
id=2 x=2.5
id=3 x=3.5
```

`field<I>()` returns a `span` so hot loops see plain contiguous arrays that the compiler can vectorise. The swap-remove keeps all columns dense but reorders elements: any external *index* referring to the moved element must be fixed, which is the standard cost of SoA that stable-ID schemes (generational indices) address.

</details>

---

## 10. Real-world example

| Where | Technique |
|---|---|
| **Game engines (ECS)** | Entity-Component-Systems store each component type in its own dense array (SoA), systems stream one or two arrays: Unity DOTS, Bevy, EnTT. The textbook data-oriented design |
| **Databases** | Columnar storage (ClickHouse, DuckDB, Parquet/Arrow): SoA at the disk-page level, vectorised execution over contiguous column chunks |
| **Linux kernel** | `____cacheline_aligned_in_smp`, per-CPU variables, `struct page` packing, RCU to avoid shared writes |
| **Folly / Abseil** | `F14` and Swiss tables: metadata bytes grouped for SIMD probing, probes within a cache line or two. `absl::flat_hash_map` beats `std::unordered_map` by 2–5× largely on locality |
| **LMAX Disruptor, SPSC rings** | Padded head/tail indices to avoid false sharing; single-writer principle (Chapters 31–32) |
| **Qt** | `QVector`/`QList` are contiguous; implicit sharing copies on write; QGraphicsView and model/view code suffer if per-item `QObject`s (≈ 100+ bytes + heap) are used for millions of rows: the usual fix is a model backed by a contiguous `std::vector` of PODs and `QAbstractItemModel` that indexes it (Chapter 48) |
| **Python + NumPy** | NumPy arrays are SoA/contiguous by design — it is *the* reason NumPy loops over arrays beat lists of Python objects by 10–100×; understanding strides is what makes `np.ascontiguousarray` and views make sense (Chapter 47) |
| **Compilers** | Arena-allocated, contiguous IR (Zig, Carbon use index-based "struct-of-arrays" ASTs; LLVM's `SmallVector`, `DenseMap`) |

> **Opinion.** Data-oriented design is **not** an aesthetic and not a reason to destroy readable code everywhere. The order of operations is: **measure → find the hot loop → compute the bytes it moves per element → fix the layout of that data only.** Doing this *everywhere* gives you unmaintainable code with no measurable benefit; ignoring it where it matters gives you 5–50× slowdowns that no micro-optimisation can recover. Prefer, in order: smaller types (`uint32_t` ids instead of pointers, `float` instead of `double` when sufficient); contiguous containers (`vector`, `flat_map`, `inplace_vector`) over node-based ones; index/handle-based graphs over pointer graphs; SoA for bulk numeric loops; per-thread data and padding for write-shared state. Treat `[[likely]]`, `__builtin_prefetch` and manual SIMD as scalpels used after profiling, not defaults. And remember **false sharing is a performance bug, not a correctness bug**: if the code is racy, padding does not fix it.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Optimising instruction count when memory-bound | No change in runtime after "clever" arithmetic | `perf stat`: look at cache-misses, IPC; reduce bytes moved |
| `std::list`/`std::map`/`unordered_map` for bulk data | Cache-miss-dominated profile; each node a separate allocation | `vector`, `flat_map`, sorted vector + binary search; node containers on a pool/arena (Chapter 26) |
| Large structs in hot arrays | Bandwidth wasted on unused fields | Hot/cold split, SoA, smaller types, reorder fields (large → small) to cut padding |
| Atomic counters/flags of different threads in one line | Scaling collapses with thread count | `alignas(64)`, per-thread accumulators merged at the end, `perf c2c` |
| "Padding" a non-atomic shared variable | Still a data race (UB) | Use atomics or synchronisation; padding is only the performance fix |
| Power-of-two row/stride sizes | Cache-set conflicts: unexpectedly slow with e.g. 1024 or 4096 columns | Pad rows by a line; use `mdspan` with explicit strides |
| Microbenchmark with a working set that fits in L1/L2 | Layout "doesn't matter" locally but does in production | Benchmark with realistic sizes and cold caches (Chapter 40) |
| SoA applied to data accessed one whole element at a time | Slower than AoS (more lines per element) | Match layout to access pattern (Experiment 3, row 3) |
| Relying on `hardware_destructive_interference_size` across a binary boundary | GCC warns `-Winterference-size`; ABI differs per `-mtune` | Hard-code a constant in public headers (64, or 128 on ARM/POWER targets) |
| Manual prefetch with a wrong distance | No gain or regression | Measure with counters; treat as per-CPU tuning |
| Vectorisation blocked by aliasing/unaligned/gather access | `-fopt-info-vec-missed` reports why | `__restrict`, contiguous SoA, `std::assume_aligned`, simplify the loop |
| NUMA first-touch on the wrong thread | One node's memory used by all threads, remote access ~2× latency | Initialise (first-touch) memory on the thread/node that will use it; `numactl` / `libnuma` |
| Ignoring TLB for huge heaps | Random access slower than cache sizes predict | Huge pages (`madvise`), arenas of 2 MiB |

---

## 12. Exercises

1. **Your hierarchy.** Run Experiment 1 on your machine and plot ns vs size on a log axis. Identify L1/L2/L3 capacities and compare with `lscpu`. Repeat with 4 KiB vs 2 MiB pages (`MADV_HUGEPAGE` via `madvise` on an aligned allocation) for the largest sizes.
2. **Field order.** For a struct of 10 mixed fields find the ordering that minimises `sizeof`. Verify with `static_assert(sizeof...)` and `pahole`. Write a constexpr helper that computes the optimal order for a list of `{size, align}` pairs.
3. **Bandwidth.** Write a STREAM-triad kernel (`a[i] = b[i] + s*c[i]`) over 1 GiB. Measure GB/s single-threaded and with 2 threads. Compare with your CPU's theoretical bandwidth; explain the gap.
4. **False sharing, three ways.** Reproduce Experiment 4 with (a) a shared array of `long` indexed by thread id, (b) padded, (c) thread-local accumulators merged at the end. Add a mutex-protected shared counter as a fourth variant and rank all four by throughput and by scalability.
5. **Hash tables.** Benchmark `std::unordered_map<int,int>`, `std::map<int,int>`, a sorted `vector<pair>` with binary search, and an open-addressing table you write (linear probing, power-of-two capacity) for 1k, 100k and 10M keys. Explain the crossover points with cache sizes.
6. **Branch or not.** Take a loop with an unpredictable branch; write four versions (branchy, ternary, mask arithmetic, partition-first); check the assembly for `cmov`/SIMD at `-O2` and `-O3`; time all four on random, sorted and 90 %-biased data.
7. **mdspan.** Implement a blocked (tiled) matrix transpose using `std::mdspan` with `layout_right` and `layout_left`; find the tile size that is fastest and relate it to L1 size.
8. **Prefetch distance.** For a linked-list traversal where nodes were allocated contiguously in an arena vs scattered by the allocator, measure traversal time; then try to speed up the scattered case with prefetching of `next->next`. Why does it fail for dependent chains but work for array-of-pointers gather?

---

## 13. Challenge: a cache-conscious order book

Implement a limit order book (price levels with FIFO queues of orders; operations: add, cancel by order id, match) in two designs and compare:

- **Design A (textbook):** `std::map<Price, std::list<Order>>` + `unordered_map<Id, iterator>`.
- **Design B (data-oriented):** contiguous arrays — price levels in a sorted `vector` or direct-indexed by tick, orders in a slab/pool with generational ids and intrusive index links, per-thread state, no per-order heap allocation, orders ≤ 32 bytes.

Measure operations/second and the p50/p99/p99.9 latency for a replayed synthetic message stream (90 % cancel/replace at the top of book is typical). Use `perf stat`/`perf c2c` to explain *why* B wins (or doesn't), count cache misses per operation, and report the layout of `Order` with `pahole`. Discuss what you would **not** sacrifice for speed (readability? invariants? API?).

---

## 14. Knowledge check

1. Roughly what are the latencies of L1, L2, L3 and DRAM in cycles, and why does the ratio matter more than the absolute values?
2. What is a cache line, and how does it make a "read 4 bytes" access cost 64?
3. Why is a pointer chase latency-bound while an array index loop can be bandwidth-bound, with the same data size?
4. What is false sharing? Why is padding a performance fix and not a correctness fix?
5. When is AoS faster than SoA? Give a concrete access pattern.
6. What does `hardware_destructive_interference_size` do, and why does GCC warn when you use it in a header?
7. Why can a power-of-two matrix width make column traversal dramatically slower?
8. Why does sorting data before a filtering loop speed it up? Which hardware unit is responsible?
9. What can a compiler do for your layout? Why can't it reorder struct fields?
10. When does `__builtin_prefetch` help and when is it useless?
11. Give two reasons `std::unordered_map` is usually slower than a flat open-addressing table.
12. What do `perf stat`'s `cache-misses`, `branch-misses` and IPC tell you, and what question does each answer?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Roughly 4–5, 12–16, 40–70, and 200–400 cycles. The *ratio* (a miss costs 50–100 instructions of lost work) determines whether a program is compute-bound or memory-bound; the absolute numbers differ per CPU.
2. The hardware transfers memory in 64-byte lines. Loading one int brings in the whole line from the next level; if you use only 4 of 64 bytes you waste 94 % of the transfer.
3. A pointer chase's next address depends on the previous load, so misses cannot overlap or be prefetched: one full latency per element. An index loop has independent addresses: many misses are in flight, and the prefetcher recognises the stream, so throughput approaches memory bandwidth.
4. Two cores write different variables in the same cache line, forcing the line to bounce between cores (coherence traffic). Padding separates the lines; it doesn't synchronise anything, so a data race on non-atomics remains UB.
5. Random access by id where most fields of one element are used together: AoS touches one line per element, SoA touches one line per field.
6. It gives a compile-time constant for the cache-line size to avoid false sharing. The value may depend on `-mtune`/CPU, so using it in a public header bakes an ABI-dependent number into interfaces; GCC warns with `-Winterference-size`.
7. Elements down a column are exactly a multiple of the page/set stride apart, so they map to the same few cache sets and evict each other (conflict misses) and also the TLB.
8. The branch predictor: sorted data makes the data-dependent branch perfectly predictable, avoiding ~15–20-cycle mispredictions.
9. Auto-vectorise contiguous, non-aliasing loops, inline, unroll, sometimes interchange loops. It cannot reorder members because C++ declaration-order layout is observable (offsets, `memcpy`, ABI, standard-layout rules).
10. Helps for independent but far-apart future addresses that the hardware can't predict and the OoO window can't reach (gathers, hash probes in a batch). Useless or harmful for sequential streams (hardware already prefetches), dependent chains (address unknown), and when data is already in cache.
11. Node-based chaining (pointer chase per bucket, separate allocation per element) and poor locality/metadata; open addressing keeps keys/values and probe metadata contiguous and probes within one or two lines.
12. `cache-misses`: is the working set/locality the problem? `branch-misses`: are data-dependent branches mispredicted? IPC: is the core stalled (low) or busy (high)? Together they separate memory-bound, branch-bound and compute-bound code.

</details>

---

[← Previous: Chapter 26](26-allocators-and-memory-resources.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 28 — Undefined behavior →](../part-11-undefined-behavior/28-undefined-behavior.md)
