# Chapter 30 — The C++ Memory Model

> **Part XII · Concurrency** &nbsp;|&nbsp; **Level 5** (systems/hardware) &nbsp;|&nbsp; **≈ 10 hours**
> **Prerequisites:** [Chapter 27](../part-10-memory/27-cache-and-data-oriented-cpp.md), [Chapter 28](../part-11-undefined-behavior/28-undefined-behavior.md), [Chapter 29](29-threading.md) &nbsp;|&nbsp; **Standards:** C++11 (the model), C++17 (`memory_order_consume` discouraged), C++20 (`atomic_ref`, `atomic::wait`, relaxed the release-sequence rules and fixed `seq_cst` fences, P0668), C++26 (`memory_order_consume` still deprecated 🟡; `std::atomic` fetch_max/min) &nbsp;|&nbsp; **Tools:** `g++-14`, `clang++-18` (AArch64 cross-assembly), TSan

[← Previous: Chapter 29](29-threading.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 31 — Atomics →](31-atomics.md)

---

**In one sentence:** the memory model is the contract that says *which writes a read may see* in a multithreaded program; it is expressed as a partial order called *happens-before*, it lets you write portable code against hardware that reorders freely, and it forbids you, on pain of undefined behaviour, from ever having two unordered conflicting accesses to the same location.

> **This is deliberately not simplified.** It is the hardest chapter in the course. Read §4 and §5 twice; run every litmus test; do the exercises on paper before running them.

**By the end of this chapter you can:**

- define *memory location*, *conflict*, *data race*, *sequenced-before*, *synchronizes-with*, *happens-before*, *modification order* and *release sequence* precisely
- explain the six memory orders by what each **promises** and what each **permits** (and why `relaxed` is not “no ordering at all”)
- predict, for a litmus test, which outcomes are allowed by C++, which are observed on x86-64 TSO, and which on ARMv8/POWER
- translate a memory order into the instructions GCC and Clang emit on x86-64 and AArch64
- reason about a lock-free handoff with a *proof* (an argument via happens-before), not by testing alone

---

## 1. Problem

Chapter 29 told you data races are undefined. This chapter explains *why the rule is what it is* and how to write correct code that nevertheless doesn't use a mutex for everything. Consider the “obviously correct” message-passing code:

```cpp
int data = 0;
bool ready = false;

void producer() { data = 42; ready = true; }
void consumer() { while (!ready) {} use(data); }
```

Three independent mechanisms can make `consumer` see `ready == true` and `data == 0`, or never terminate:

| Who | What it does | Result |
|---|---|---|
| **Compiler** | Hoists `ready` out of the loop (it assumes no other thread writes it); or reorders `data = 42` after `ready = true` (they are independent in a single thread) | consumer spins forever, or reads stale `data` |
| **CPU, store side** | Store buffer: the write to `data` and to `ready` leave the core's buffer in an order other cores may observe differently (on weak ISAs; x86 keeps store-store order) | other cores see `ready` first |
| **CPU, load side** | Out-of-order and speculative loads: `data` may be read before `ready` is confirmed (ARM/POWER) | stale `data` |

Each of these is a *legal* optimisation for single-threaded semantics. We need a contract that (a) constrains all three at once, (b) is **portable** across ISAs with very different guarantees (x86 TSO, ARMv8, POWER, RISC-V RVWMO), and (c) leaves the compiler enough freedom to be fast where no communication happens.

---

## 2. Historical context

| Year | Event |
|---|---|
| 1979 | Lamport defines **sequential consistency (SC)**: the result is as if all operations executed in some single order consistent with each thread's program order |
| 1990s | Real hardware abandons SC for speed: write buffers (TSO on x86/SPARC), then fully relaxed (ARM, POWER, Alpha: even dependent loads can be reordered) |
| 1995 | POSIX threads: the *rules* (locks make memory visible) are specified informally, the C/C++ language says nothing |
| 2000 | **Java Memory Model** (JSR-133, 2004 revision): data-race-free ⇒ SC (DRF-SC) as the backbone, with a hard story for racy programs |
| 2005 | Boehm: *Threads Cannot Be Implemented as a Library*. Compiler transformations legal in C++98 break threaded code |
| 2008 | Boehm & Adve, *Foundations of the C++ Concurrency Memory Model* (PLDI); **N2429/N2800** adopted: the C++11 model with `std::atomic` and orders |
| 2011 | **C++11**: `memory_order_relaxed / consume / acquire / release / acq_rel / seq_cst`. Formalised axiomatically |
| 2013–15 | Batty et al. formalise C11 in HOL/Isabelle; discover *out-of-thin-air* problem for `relaxed` and the weaknesses of `consume` |
| 2016 | P0371/P0735: `memory_order_consume` implementation-wise "broken"; all compilers promote it to `acquire`; committee recommends not using it |
| 2018 | **P0668R5** (C++20): strengthens `seq_cst` fences vs x86/POWER/ARM mappings and weakens release sequence definition |
| 2020 | C++20: `atomic::wait/notify`, `atomic_ref`, `atomic<shared_ptr>` |
| 2024–26 | Work continues on out-of-thin-air (P1217), `consume` replacement (RCU / `std::rcu` and `hazard_pointer` adopted for C++26 🟡) |

The big idea survives 45 years: **give programmers SC for data-race-free programs, and make races UB so the compiler and CPU can reorder everywhere else.** The atomics with weaker orders are an escape hatch for experts, with a formal model that tells you exactly what you lose.

---

## 3. Modern solution

```cpp
std::atomic<bool> ready{false};
int data = 0;                                   // plain, non-atomic: protected by the ready/acquire-release protocol

void producer() {
    data = 42;                                  // (1)
    ready.store(true, std::memory_order_release);   // (2) publishes everything sequenced before it
}
void consumer() {
    while (!ready.load(std::memory_order_acquire)) {}   // (3) observes (2) ⇒ synchronizes-with it
    use(data);                                  // (4) happens-after (1): guaranteed to read 42
}
```

The reasoning is a *proof*, not a hope:

```text
   (1) is sequenced-before (2)                       [program order in the producer]
   (3) reads the value written by (2)  ⇒  (2) synchronizes-with (3)    [release/acquire pair]
   (3) is sequenced-before (4)                       [program order in the consumer]
   ⇒  (1) happens-before (4)                         [transitivity]
   ⇒  (4) reads 42, and there is no data race on `data`
```

That is the whole model in one example. The rest of the chapter is the machinery that makes each arrow precise.

---

## 4. Mental model

### Operational intuition: store buffers and caches (hardware view)

```text
        core 0                          core 1
   ┌───────────────┐              ┌───────────────┐
   │ registers     │              │ registers     │
   │ store buffer  │ ──┐      ┌── │ store buffer  │      each core: stores enter a private buffer, retire to cache later
   │ L1/L2 cache   │   ▼      ▼   │ L1/L2 cache   │      loads may be satisfied from the store buffer (forwarding),
   └───────┬───────┘  coherent     └───────┬───────┘      or from cache, in a different order than program order
           └──────────  shared L3 / memory ──────────┘
```

- Caches are *coherent* (MESI): there is one agreed order of writes **per location** (the *modification order*). This is the one property every ISA provides.
- Reordering comes from **store buffers** (a later load can complete before an earlier store becomes visible: *StoreLoad* reordering, the only one x86 allows) and from **out-of-order / speculative execution and non-multi-copy-atomic interconnects** on ARM/POWER (*StoreStore*, *LoadLoad*, *LoadStore* too).

| ISA | Allowed reorderings (hardware, between different locations) |
|---|---|
| **x86-64 (TSO)** | StoreLoad only. Stores are seen by all cores in one order (multi-copy atomic) |
| **ARMv8 (AArch64)** | All four (StoreLoad, StoreStore, LoadLoad, LoadStore) unless dependencies or acquire/release instructions forbid it. ARMv8 is *multi-copy atomic*; older ARMv7 and POWER are not |
| **RISC-V RVWMO** | Like ARMv8: relaxed, multi-copy atomic |
| **POWER** | Weakest: not multi-copy atomic (IRIW can be observed) |

A correct portable program must be correct on the **weakest** one, which is what the C++ model describes.

### Axiomatic view: the C++ model is a set of relations over operations

C++ does not specify a machine; it specifies **which executions are allowed**, by defining relations among the memory operations of a candidate execution and demanding that they satisfy axioms:

```text
   sb   ("sequenced-before")     program order inside one thread (a partial order within an expression, total across statements)
   rf   ("reads-from")           pairs each load with the store whose value it returned
   mo   ("modification order")   for EACH atomic object, one total order of all writes to it (coherence)
   sw   ("synchronizes-with")    rf edge from a release (or stronger) write to an acquire (or stronger) read that read it
   hb   ("happens-before")       the transitive closure of  sb ∪ sw  (plus a few details)
   S    ("seq_cst total order")  one total order of all seq_cst operations, consistent with hb and mo
```

An execution is **allowed** if: a read returns a value from a write that is not “hidden” by a more recent write that *happens-before* the read (the *visible side effect* rule); and `mo`, `hb` and `S` are consistent. A program is **valid** if no allowed execution has a data race; if any does, *all* executions of the program have UB.

### Data race, defined

Two memory operations **conflict** if they access the same *memory location* and at least one is a write. A **data race** exists if two conflicting accesses, at least one **non-atomic**, are **not ordered by happens-before**. (Two atomic operations never race with each other; ordering matters only for what they let *other* accesses see.)

> **Memory location** = a scalar object (`int`, pointer, …) or a maximal sequence of adjacent bit-fields. Separate scalars never alias for this purpose, even if they share a cache line: false sharing is a performance problem, **not** a race. Two bit-fields in the same word *are* the same location, which is why updating adjacent bit-fields from two threads is a race.

### DRF-SC: the theorem that makes this tractable

> **If every execution of a program (under SC) is data-race-free, then the program's behaviour is sequentially consistent.**

You never have to reason about store buffers if you only use mutexes/locks and `seq_cst` atomics correctly. The weaker orders are for when you can prove (and need) less.

### The six orders, by intent

| Order | One-line promise | Use for |
|---|---|---|
| `relaxed` | Atomicity and per-object modification order only. **No ordering with respect to other locations** | Counters, statistics, flags that carry no data |
| `consume` | Like acquire but only for data-*dependent* reads. **Do not use**: no compiler implements it; treated as `acquire` | — |
| `acquire` (loads) | Nothing sequenced **after** this load can move before it; if it reads a release-store's value, everything before that store is visible | Taking a lock; reading a “published” flag |
| `release` (stores) | Nothing sequenced **before** this store can move after it | Releasing a lock; publishing data |
| `acq_rel` (RMW) | Both, for a read-modify-write | Reference-count decrement; CAS on a lock-free node |
| `seq_cst` | acquire/release **plus** a single global order `S` of all `seq_cst` operations | When you can't prove less; the default |

**Why `relaxed` isn't “nothing”:** every atomic access, even relaxed, is atomic and respects *coherence*: you can never see writes to one atomic object go backwards; all threads agree on the modification order of each atomic. What you give up is **cross-object ordering** (and hence happens-before edges).

### Fences

`std::atomic_thread_fence(order)` is an ordering operation not tied to a location. A release fence followed (in program order) by a relaxed store, together with a relaxed load followed by an acquire fence on another thread, synchronizes if the load reads the store's value. Fences are stronger than the equivalent per-operation order (they order *all* prior/later accesses), which is why they are costlier on some ISAs.

---

## 5. Language rules

The normative text is `[intro.races]` and `[atomics.order]` in the C++ standard (C++20/23 numbering). The definitions that matter:

### 5.1 Sequenced-before  `[intro.execution]`

Within a thread, evaluations are ordered by *sequenced-before*: a strict partial order (the full expression's end is a sequence point; `;` sequences statements; the built-in `&&`, `||`, `,` and `?:` sequence their operands; function arguments are *indeterminately* sequenced; C++17 sequences the right operand of `=` before the left). Sequenced-before is the only ordering that exists inside one thread, and **the compiler may reorder anything not constrained by it plus data dependencies** (the as-if rule).

### 5.2 Release sequence and synchronizes-with  `[atomics.order]`

- An atomic *release* operation A on object M **synchronizes-with** an atomic *acquire* operation B on M if B reads (a) the value written by A, or (b) a value written by a later operation in the **release sequence** headed by A.
- The **release sequence** headed by A is A followed by a maximal contiguous subsequence of *read-modify-writes* to M (any order, any thread) in the modification order. (C++20 narrowed this to RMWs only; same-thread plain relaxed stores no longer continue the sequence.)
- Other synchronisation edges: `thread` creation (the constructor call synchronizes with the start of the new thread's function), `join` (end of thread synchronizes with the return of `join`), mutex `unlock` → next `lock`, `call_once`, `latch::count_down` → `wait`, `barrier` phases, `promise::set_value` → `future::get`, `semaphore::release` → `acquire`, `condition_variable` through its mutex.

### 5.3 Happens-before  `[intro.races]`

*Inter-thread happens-before* is built from synchronizes-with and sequenced-before (plus carries-a-dependency-to for `consume`). *Happens-before* = sequenced-before ∪ inter-thread happens-before, transitively closed. A has to *happen before* B for B to be guaranteed to see A's effects (for non-atomic data). If neither happens-before the other and they conflict and one is non-atomic: **data race = UB**.

### 5.4 Visible side effect and coherence rules

A load of atomic `M` returns the value of: the **visible side effect** (the latest write that happens-before the load, with no other write between them in happens-before) — or, for atomics, any write that is not happens-before-hidden (this is where `relaxed` gets its latitude). Four coherence requirements (write-write, read-read, read-write, write-read) say that if A happens-before B on the same object then B observes A's effect or something later in `mo`. They are why **relaxed counters never go backwards**.

### 5.5 The sequentially consistent total order `S`

All `seq_cst` operations (and `seq_cst` fences) are in one total order `S` that is consistent with happens-before and with the modification order of each object. A `seq_cst` load returns either the last preceding `seq_cst` store to that object in `S` or a non-`seq_cst` write that is later in `mo`. **`seq_cst` is stronger than `acq_rel`**: it forbids Store-Buffering outcomes (`r1 == r2 == 0` in Experiment 1), which `acq_rel` permits.

### 5.6 What the standard does *not* promise

- It does not say how long a store takes to become visible (only that it “should” in a reasonable time, `[atomics.order]/12`); no progress guarantee beyond the forward-progress rules.
- It does not give `volatile` any inter-thread meaning (`volatile` is for memory-mapped I/O; Java's `volatile` is a different thing).
- It does not guarantee atomics are lock-free (`is_lock_free()`, `is_always_lock_free`); on x86-64 `atomic<T>` for `sizeof(T) ≤ 8` is lock-free; 16 bytes needs `cmpxchg16b` and libatomic (not guaranteed, `-latomic`).
- `memory_order_consume` is specified but all major compilers promote it to `acquire`; avoid.
- The “out-of-thin-air” problem: the model deliberately leaves `relaxed` loads/stores slightly under-specified (a load could theoretically return a value no thread wrote when there are circular dependencies). No real compiler/CPU does this; the standard recommends against it ([atomics.order]/9). You can ignore it, but you should know it exists when reading papers.

### Layer check

| Layer | Question | Answer |
|---|---|---|
| **C++ standard** | What is guaranteed? | The axioms above; DRF-SC; `relaxed` coherence; release/acquire pairs create happens-before; races are UB. Nothing else |
| **Compiler** | What does it do with the order? | (1) Restricts its *own* reordering/hoisting/CSE across atomic operations according to the order; (2) emits instructions or fences for the **target** ISA. GCC/Clang are conservative: most `relaxed` ops are treated as `volatile`-ish (no merging), but this is not guaranteed |
| **ABI** | What is fixed? | The mapping from C++ orders to instructions is a **psABI convention** (the C/C++11 “atomics mappings” for x86-64, ARM, POWER, RISC-V) so that code compiled by different compilers interoperates (e.g. seq_cst store = `xchg` or `mov+mfence` must be consistent between compilers sharing atomics across a shared library boundary) |
| **OS** | Does it matter? | Kernel synchronisation (futex, scheduler IPIs, `membarrier(2)`) provides ordering across context switches; `mmap`'d shared memory between processes follows the same hardware rules, and atomics on it work if lock-free |
| **CPU** | What does the hardware really do? | x86: all stores are release, all loads are acquire (TSO); only StoreLoad needs a fence (`mfence`/locked op). ARMv8: dedicated `ldar`/`stlr` (acquire/release), `ldadd{a,l,al}` for RMW, `dmb` barriers. POWER: `lwsync`/`sync`. RISC-V: `fence rw,rw`, `.aq/.rl` |

---

## 6. Implementation model

### What GCC/Clang emit

| C++ operation | x86-64 | AArch64 (ARMv8.0 LL/SC or v8.1 LSE) |
|---|---|---|
| `load(relaxed / acquire)` | `mov` | `ldr` / `ldar` (`ldapr` in v8.3) |
| `load(seq_cst)` | `mov` | `ldar` |
| `store(relaxed / release)` | `mov` | `str` / `stlr` |
| `store(seq_cst)` | `xchg` (or `mov` + `mfence`) | `stlr` |
| `fetch_add(any)` | `lock xadd` | `ldadd{,a,l,al}` (LSE) or `ldxr/stxr` loop |
| `compare_exchange_*` | `lock cmpxchg` | `cas{,a,l,al}` (LSE) or `ldaxr/stlxr` loop |
| `atomic_thread_fence(acquire/release/acq_rel)` | *no instruction* (compiler barrier only) | `dmb ishld` / `dmb ish` |
| `atomic_thread_fence(seq_cst)` | `mfence` (Clang) / `lock or [rsp], 0` (GCC 14) | `dmb ish` |

Two observations that drive practice: on x86, acquire loads and release stores are **free** (a plain `mov`) because TSO already provides them: the cost shows up for `seq_cst` stores (a locked instruction or `mfence`: ~20 cycles) and all RMWs (a `lock` prefix: ~20 cycles). On AArch64, acquire/release are dedicated instructions, and relaxed accesses are genuinely cheaper.

### Why a compiler barrier is not enough

A compiler barrier (`asm volatile("" ::: "memory")`) stops the **compiler** from moving accesses across it; it emits no instruction and does nothing about the **CPU**'s reordering. On x86 it suffices to prevent compiler-induced reordering for TSO-ordered operations, which is why broken code “works on x86” — and fails on ARM.

### How the hardware enforces release/acquire (x86 TSO)

TSO already guarantees that stores become visible in program order and loads are not reordered with older loads. So a release store is just a store, an acquire load is just a load. The *only* new instruction needed is for SC: a store followed by a load of a different location (Dekker/Peterson/SB) needs a barrier (`mfence` or an `xchg`) to drain the store buffer before the load executes.

---

## 7. Experiments

### Experiment 1 ✅: Store Buffering: the litmus test that separates `seq_cst` from everything else

Two threads, two atomics, initial `x = y = 0`:

```text
   Thread A:  x = 1;  r1 = y;        Thread B:  y = 1;  r2 = x;
   Question:  can we end with r1 == 0 && r2 == 0 ?
```

Under SC the answer is **no** (one of the two stores must come first in the global order, so the other thread's load sees it). With a store buffer the answer is **yes**: each core's store sits in its buffer while its load reads stale memory.

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=120
#include <atomic>
#include <cstdio>
#include <thread>

// A tiny two-thread spin barrier so each iteration starts both threads at (nearly) the same instant.
struct SpinBarrier {
    std::atomic<int> count{0}, generation{0};
    void wait() {
        int g = generation.load(std::memory_order_acquire);
        if (count.fetch_add(1) == 1) { count.store(0); generation.fetch_add(1, std::memory_order_release); }
        else while (generation.load(std::memory_order_acquire) == g) {}
    }
};

template <std::memory_order StoreMO, std::memory_order LoadMO>
long run(int iterations) {
    std::atomic<int> x{0}, y{0};
    int r1 = 0, r2 = 0; long both_zero = 0;
    SpinBarrier start, end;
    std::thread other([&] {
        for (int i = 0; i < iterations; ++i) { start.wait(); y.store(1, StoreMO); r2 = x.load(LoadMO); end.wait(); }
    });
    for (int i = 0; i < iterations; ++i) {
        x.store(0, std::memory_order_relaxed); y.store(0, std::memory_order_relaxed);
        start.wait();
        x.store(1, StoreMO); r1 = y.load(LoadMO);
        end.wait();
        if (r1 == 0 && r2 == 0) ++both_zero;
    }
    other.join();
    return both_zero;
}

int main() {
    constexpr int N = 300'000;
    using enum std::memory_order;
    std::printf("outcome (r1==0 && r2==0) in %d trials -- forbidden by sequential consistency\n", N);
    std::printf("  relaxed store, relaxed load         : %7ld\n", run<relaxed, relaxed>(N));
    std::printf("  release store, acquire load         : %7ld   (acq/rel does NOT forbid it)\n", run<release, acquire>(N));
    std::printf("  seq_cst store, seq_cst load         : %7ld   (forbidden)\n", run<seq_cst, seq_cst>(N));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
outcome (r1==0 && r2==0) in 300000 trials -- forbidden by sequential consistency
  relaxed store, relaxed load         :   32887
  release store, acquire load         :   27610   (acq/rel does NOT forbid it)
  seq_cst store, seq_cst load         :       0   (forbidden)
```

This is the single most important experiment in the chapter. Three things it shows:

1. **Hardware reordering is observable, not theoretical**: on x86 (the *strongest* mainstream ISA) the “impossible” result appeared thousands of times in 300,000 trials for `relaxed` and tens of thousands of times for release/acquire (exact counts vary from run to run and with the timing of the barrier; one run on this machine gave about 2,400 for relaxed and 73,000 for release/acquire). With `seq_cst`: **never**.
2. **`release`/`acquire` is not SC.** They order a thread's accesses *relative to a synchronising partner*, but they do not make the two independent stores visible in a single global order. Only `seq_cst` (which on x86 compiles the store to `xchg`) drains the store buffer before the load.
3. This is exactly why Dekker's and Peterson's algorithms break on real hardware without `seq_cst` or explicit fences (Experiment 5).

### Experiment 2 ✅: Message passing: the correct release/acquire protocol

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=120
#include <atomic>
#include <cstdio>
#include <thread>

struct Payload { long a, b, c, d; };               // multiple plain, non-atomic fields: the point of publication

int main() {
    constexpr int N = 200'000;
    long violations = 0;
    Payload data;
    std::atomic<int> flag{0};
    std::atomic<int> turn{0};                      // round handshake: avoid reusing `data` while the reader is still looking at it

    std::thread reader([&] {
        for (int i = 1; i <= N; ++i) {
            while (flag.load(std::memory_order_acquire) != i) {}          // (3) acquire: sees the value written by (2)
            // (4) everything written before (2) is visible: data.a..d are all == i
            if (data.a != i || data.b != i || data.c != i || data.d != i) ++violations;
            turn.store(i, std::memory_order_release);                     // tell the writer we're done with this round
        }
    });
    for (int i = 1; i <= N; ++i) {
        data = {i, i, i, i};                                              // (1) plain stores
        flag.store(i, std::memory_order_release);                         // (2) release: publishes (1)
        while (turn.load(std::memory_order_acquire) != i) {}
    }
    reader.join();
    std::printf("rounds: %d, violations of the publication guarantee: %ld\n", N, violations);
    return violations == 0 ? 0 : 1;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
rounds: 200000, violations of the publication guarantee: 0
```

Zero violations is *guaranteed* here by the happens-before proof in §3, on every conforming implementation and ISA. Contrast with Experiment 1: the same hardware that allows “impossible” outcomes for SB cannot violate this one, because there *is* a synchronising edge. A TSan run of this program (`-fsanitize=thread`) is clean; replace both orders with `relaxed` and TSan reports a data race on `data` even though x86 would still “work”.

### Experiment 3 🧩: The instruction each order compiles to (x86-64)

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fcf-protection=none filter=load_relaxed,load_acquire,load_seqcst,store_relaxed,store_release,store_seqcst,add_relaxed,add_seqcst,cas_acqrel,fence_acquire,fence_seqcst
#include <atomic>

std::atomic<int> a;
int  load_relaxed()   { return a.load(std::memory_order_relaxed); }
int  load_acquire()   { return a.load(std::memory_order_acquire); }
int  load_seqcst()    { return a.load(std::memory_order_seq_cst); }
void store_relaxed()  { a.store(1, std::memory_order_relaxed); }
void store_release()  { a.store(1, std::memory_order_release); }
void store_seqcst()   { a.store(1, std::memory_order_seq_cst); }
int  add_relaxed()    { return a.fetch_add(1, std::memory_order_relaxed); }
int  add_seqcst()     { return a.fetch_add(1, std::memory_order_seq_cst); }
bool cas_acqrel(int& e) { return a.compare_exchange_strong(e, 7, std::memory_order_acq_rel); }
void fence_acquire()  { std::atomic_thread_fence(std::memory_order_acquire); }
void fence_seqcst()   { std::atomic_thread_fence(std::memory_order_seq_cst); }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
load_relaxed():
	mov	eax, DWORD PTR a[rip]
	ret

load_acquire():
	mov	eax, DWORD PTR a[rip]
	ret

load_seqcst():
	mov	eax, DWORD PTR a[rip]
	ret

store_relaxed():
	mov	DWORD PTR a[rip], 1
	ret

store_release():
	mov	DWORD PTR a[rip], 1
	ret

store_seqcst():
	mov	eax, 1
	xchg	eax, DWORD PTR a[rip]
	ret

add_relaxed():
	mov	eax, 1
	lock xadd	DWORD PTR a[rip], eax
	ret

add_seqcst():
	mov	eax, 1
	lock xadd	DWORD PTR a[rip], eax
	ret

cas_acqrel(int&):
	mov	eax, DWORD PTR [rdi]
	mov	edx, 7
	lock cmpxchg	DWORD PTR a[rip], edx
	sete	dl
	je	.L10
	mov	DWORD PTR [rdi], eax
.L10:
	mov	eax, edx
	ret

fence_acquire():
	ret

fence_seqcst():
	lock or	QWORD PTR [rsp], 0
	ret
```

Read the table: **loads are all `mov`**, `release` stores are `mov`, only the `seq_cst` store pays (`xchg`), and the RMWs are `lock`-prefixed whatever order you ask for (x86 gives them full-barrier semantics; asking for `relaxed` saves nothing at the instruction level, though it frees the *compiler* to move other code around the operation). Acquire/release fences emit **nothing** (they only constrain the compiler); `seq_cst` fence is a full barrier: GCC 14 emits `lock or QWORD PTR [rsp], 0` (a locked no-op on the stack, usually faster than `mfence`), Clang emits `mfence`. *On x86, `acquire`, `release` and `acq_rel` cost nothing; that is also why you cannot test their correctness on x86.*

### Experiment 4 🔧: The same source on AArch64 (cross-assembled with Clang)

```cpp
// @test asm -std=c++23 -O2 --target=aarch64-linux-gnu -march=armv8-a filter=load_relaxed,load_acquire,store_relaxed,store_release,store_seqcst,add_relaxed,add_acqrel,fence_acquire,fence_seqcst cxx=clang++-18
// No #include: the target's libstdc++ headers are not installed for the cross target, so we use the compiler builtins
// that std::atomic<int> itself is implemented with (__atomic_*). The memory-order constants are the same six orders.
int a;
int  load_relaxed()   { return __atomic_load_n(&a, __ATOMIC_RELAXED); }
int  load_acquire()   { return __atomic_load_n(&a, __ATOMIC_ACQUIRE); }
void store_relaxed()  { __atomic_store_n(&a, 1, __ATOMIC_RELAXED); }
void store_release()  { __atomic_store_n(&a, 1, __ATOMIC_RELEASE); }
void store_seqcst()   { __atomic_store_n(&a, 1, __ATOMIC_SEQ_CST); }
int  add_relaxed()    { return __atomic_fetch_add(&a, 1, __ATOMIC_RELAXED); }
int  add_acqrel()     { return __atomic_fetch_add(&a, 1, __ATOMIC_ACQ_REL); }
void fence_acquire()  { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
void fence_seqcst()   { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
```

```asm
; asm (clang 18.1.3, -O2, AArch64, GNU syntax)
load_relaxed():
	adrp	x8, a
	ldr	w0, [x8, :lo12:a]
	ret

load_acquire():
	adrp	x8, a
	add	x8, x8, :lo12:a
	ldar	w0, [x8]
	ret

store_relaxed():
	adrp	x8, a
	mov	w9, #1
	str	w9, [x8, :lo12:a]
	ret

store_release():
	mov	w8, #1
	adrp	x9, a
	add	x9, x9, :lo12:a
	stlr	w8, [x9]
	ret

store_seqcst():
	mov	w8, #1
	adrp	x9, a
	add	x9, x9, :lo12:a
	stlr	w8, [x9]
	ret

add_relaxed():
	adrp	x8, a
	add	x8, x8, :lo12:a
.LBB5_1:
	ldxr	w0, [x8]
	add	w9, w0, #1
	stxr	w10, w9, [x8]
	cbnz	w10, .LBB5_1
	ret

add_acqrel():
	adrp	x8, a
	add	x8, x8, :lo12:a
.LBB6_1:
	ldaxr	w0, [x8]
	add	w9, w0, #1
	stlxr	w10, w9, [x8]
	cbnz	w10, .LBB6_1
	ret

fence_acquire():
	dmb	ishld
	ret

fence_seqcst():
	dmb	ish
	ret
```

Here the orders are real: relaxed accesses are `ldr`/`str`; acquire loads are `ldar`; release/seq_cst stores are `stlr`; the RMWs are `ldxr/stxr` retry loops (LL/SC, ARMv8.0; with `-march=armv8.1-a` you get single-instruction `ldadd`/`cas` with `a`/`l`/`al` suffixes), with `ldaxr`/`stlxr` for acquire/release; fences are `dmb`. **A program with a missing `release`/`acquire` that works on x86 can fail on AArch64**: that is what the model protects you from, and why testing on x86 alone proves nothing about relaxed-order code.

### Experiment 5 ✅: Peterson's lock: correct only with `seq_cst`

Peterson's algorithm is mutual exclusion from plain atomic flags; it relies on **store→load ordering** (each thread stores its flag, then loads the other's). That is precisely the Store-Buffering pattern.

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=120
#include <atomic>
#include <cstdio>
#include <thread>

template <std::memory_order MO>
long run(int iterations) {
    std::atomic<int> flag[2] = {0, 0};
    std::atomic<int> turn{0};
    std::atomic<int> in_cs{0};                  // how many threads are inside the critical section right now
    long violations = 0;                        // incremented only inside the CS (protected, if the lock is correct)

    auto worker = [&](int me) {
        int other = 1 - me;
        for (int i = 0; i < iterations; ++i) {
            flag[me].store(1, MO);              // I want to enter
            turn.store(other, MO);              // but you go first, if you also want to
            while (flag[other].load(MO) == 1 && turn.load(MO) == other) {}   // wait
            // ---- critical section ----
            if (in_cs.fetch_add(1, std::memory_order_relaxed) != 0) ++violations;   // someone else is already in: mutual exclusion broken
            in_cs.fetch_sub(1, std::memory_order_relaxed);
            // ---- end ----
            flag[me].store(0, MO);
        }
    };
    std::thread t(worker, 1);
    worker(0);
    t.join();
    return violations;
}

int main() {
    constexpr int N = 400'000;
    std::printf("mutual-exclusion violations in %d iterations per thread:\n", N);
    std::printf("  seq_cst  : %ld\n", run<std::memory_order_seq_cst>(N));
    std::printf("  relaxed  : %ld\n", run<std::memory_order_relaxed>(N));
    return 0;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
mutual-exclusion violations in 400000 iterations per thread:
  seq_cst  : 0
  relaxed  : 1
```

*(The relaxed variant is broken twice over: it lacks StoreLoad ordering **and** has no acquire/release; on x86 the first failure is the visible one. The seq_cst version is the standard-correct Peterson.)* If your run shows 0 violations for `relaxed`, run longer, or on a machine with more cores: it is a probabilistic race; one observed violation proves the lock is broken, zero observations prove nothing.

### Experiment 6 ✅: `relaxed` is still atomic and coherent

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=60
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    std::atomic<long> counter{0};
    constexpr int T = 2, N = 2'000'000;
    {
        std::vector<std::jthread> ts;
        for (int t = 0; t < T; ++t) ts.emplace_back([&] { for (int i = 0; i < N; ++i) counter.fetch_add(1, std::memory_order_relaxed); });
    }
    std::printf("relaxed fetch_add: no lost updates -> %ld (expected %d)\n", counter.load(), T * N);

    // Coherence: a reader that polls a monotonically increasing atomic never sees it go backwards,
    // even with relaxed loads (read-read coherence on a single object).
    std::atomic<long> v{0};
    std::atomic<bool> stop{false};
    long went_backwards = 0;
    std::jthread writer([&] { for (long i = 1; !stop.load(std::memory_order_relaxed); ++i) v.store(i, std::memory_order_relaxed); });
    long last = 0;
    for (int i = 0; i < 5'000'000; ++i) {
        long cur = v.load(std::memory_order_relaxed);
        if (cur < last) ++went_backwards;
        last = cur;
    }
    stop.store(true, std::memory_order_relaxed);
    std::printf("relaxed loads observed a decrease %ld times (coherence forbids it)\n", went_backwards);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
relaxed fetch_add: no lost updates -> 4000000 (expected 4000000)
relaxed loads observed a decrease 0 times (coherence forbids it)
```

### Experiment 7 🔧: A data race lets the compiler remove your loop

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fcf-protection=none filter=spin_plain,spin_atomic
#include <atomic>

bool done_plain;
std::atomic<bool> done_atomic;

void spin_plain()  { while (!done_plain) {} }                                         // data race when another thread sets it: UB
void spin_atomic() { while (!done_atomic.load(std::memory_order_relaxed)) {} }       // fine: atomic, even relaxed
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
spin_plain():
	ret

spin_atomic():
.L4:
	movzx	eax, BYTE PTR done_atomic[rip]
	test	al, al
	je	.L4
	ret
```

`spin_atomic` reloads the flag on every iteration (`movzx … ; test ; je`). `spin_plain` is **the whole function reduced to `ret`**: GCC 14 treated the load as loop-invariant (no one else may write it without a race, and a race is UB), and then, since a loop that can never change state and performs no I/O or atomic operation has no observable behaviour, the *forward-progress rule* lets it assume the loop terminates, so the loop is deleted. Other compiler versions emit a one-instruction self-jump (`jmp .L`) instead; either way **the code never observes the other thread's write**. Note that `spin_atomic` is correct even with `relaxed` **for a flag that carries no data**; the moment the flag publishes other data, you need `release`/`acquire`.

### Experiment 8 ✅: Fence-based synchronisation

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=60
#include <atomic>
#include <cstdio>
#include <thread>

int main() {
    constexpr int N = 200'000;
    long violations = 0;
    long data = 0;
    std::atomic<int> flag{0}, turn{0};

    std::thread reader([&] {
        for (int i = 1; i <= N; ++i) {
            while (flag.load(std::memory_order_relaxed) != i) {}       // relaxed load...
            std::atomic_thread_fence(std::memory_order_acquire);       // ...followed by an acquire fence: acts as an acquire on `flag`
            if (data != i) ++violations;
            turn.store(i, std::memory_order_release);
        }
    });
    for (int i = 1; i <= N; ++i) {
        data = i;
        std::atomic_thread_fence(std::memory_order_release);           // release fence before a relaxed store: acts as a release on `flag`
        flag.store(i, std::memory_order_relaxed);
        while (turn.load(std::memory_order_acquire) != i) {}
    }
    reader.join();
    std::printf("fence-based publication: %ld violations in %d rounds\n", violations, N);
    return violations == 0 ? 0 : 1;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
fence-based publication: 0 violations in 200000 rounds
```

Fences are useful when one fence can cover many relaxed operations (batching), or when the ordering must apply to accesses that are not themselves atomic. In day-to-day code prefer the per-operation orders: they are easier to read, and on AArch64 they map to cheaper instructions (`ldar/stlr`) than a standalone `dmb`.

---

## 8. Assembly / runtime investigation

```bash
# (1) For any atomic operation, see the exact instruction on every ISA you care about
g++-14      -std=c++23 -O2 -S -masm=intel -o - op.cpp | c++filt | grep -E "mov|xchg|lock|mfence"
clang++-18  -std=c++23 -O2 --target=aarch64-linux-gnu -S -o - op.cpp | grep -E "ldar|stlr|ldadd|cas|dmb|ldxr|stxr"
clang++-18  -std=c++23 -O2 --target=riscv64-linux-gnu -S -o - op.cpp | grep -E "fence|amo|lr\.|sc\."
clang++-18  -std=c++23 -O2 --target=powerpc64le-linux-gnu -S -o - op.cpp | grep -E "sync|lwsync|lwarx|stwcx"

# (2) Find the *weakest* correct order empirically: run litmus tests with herd7 / litmus7 (diy suite)
#     herd7 -model c11.cat sb.litmus    ← exhaustively enumerates allowed outcomes under the C11 model
#     litmus7 -r 1000 sb.litmus          ← runs on real hardware, counts outcomes

# (3) Detect races and wrong-order bugs dynamically (TSan models the C++ memory model; it understands acquire/release)
g++-14 -std=c++23 -O1 -g -fsanitize=thread prog.cpp -pthread && ./a.out

# (4) Cross-check on weak hardware: run the same test on an ARM machine (Graviton / Apple silicon / Raspberry Pi); bugs invisible on x86 appear within seconds

# (5) How much does ordering cost on your machine?  microbenchmark mov vs xchg vs lock xadd
perf stat -e cycles,instructions ./store_release_loop ; perf stat -e cycles,instructions ./store_seqcst_loop
```

---

## 9. Implementation exercise

Prove (on paper) and then test:

1. **Spinlock**: `lock()` = `while (flag.exchange(true, memory_order_acquire))`; `unlock()` = `flag.store(false, memory_order_release)`. Draw the happens-before graph for two threads’ critical sections. Show that critical section 1 *happens-before* critical section 2. Which accesses would race if you weakened `unlock` to `relaxed`?
2. **SPSC ring buffer** (head/tail indices as atomics). For each load/store, pick the weakest order and justify it with an edge in your proof. Then test it under TSan and on an ARM machine.
3. **Seqlock** (reader retries if the sequence number changed): write it with atomics and fences. Why do the data words need to be atomic (relaxed) even though readers “discard torn reads”? (Answer: a torn read of a non-atomic is a data race, i.e. UB.)
4. **Reference counting**: show why `fetch_sub(1, release)` plus an `acquire` fence on the last decrement is sufficient for the destructor to see all previous writes; why `relaxed` would be wrong; why `acq_rel` on every decrement is correct but over-strong.
5. **Litmus zoo**: write MP, LB (load buffering), IRIW, CoRR, CoWR, 2+2W as run-and-count tests; record which outcomes you ever observe on x86.

<details>
<summary><strong>Solution sketch: a spinlock with a proof</strong></summary>

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=60
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

class SpinLock {
    std::atomic<bool> locked_{false};
public:
    void lock() noexcept {
        // acquire: on success, everything the previous holder did before unlock() is visible to us
        while (locked_.exchange(true, std::memory_order_acquire)) {
            while (locked_.load(std::memory_order_relaxed)) {}      // test-and-test-and-set: spin on a read, not on an RMW
        }
    }
    void unlock() noexcept {
        locked_.store(false, std::memory_order_release);           // release: publishes everything done inside the section
    }
};

int main() {
    SpinLock lock;
    long counter = 0;                                               // plain long: protected by the lock => no data race
    constexpr int N = 500'000;
    {
        std::vector<std::jthread> ts;
        for (int t = 0; t < 2; ++t) ts.emplace_back([&] { for (int i = 0; i < N; ++i) { lock.lock(); ++counter; lock.unlock(); } });
    }
    std::printf("counter = %ld (expected %d)\n", counter, 2 * N);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
counter = 1000000 (expected 1000000)
```

**Proof sketch.** Let S₁ be the section of the thread that took the lock first, S₂ the other. S₂'s successful `exchange` reads the `false` written by S₁'s `unlock` (the only write of `false` after the initial value that S₂ can read: the lock was held until then). That `store(release)` therefore *synchronizes-with* S₂'s `exchange(acquire)`. By sequenced-before inside each thread and transitivity, every access in S₁ happens-before every access in S₂: no data race on `counter`. Weaken `unlock` to `relaxed` and there is no synchronizes-with edge: `counter` races (UB) even though x86 hardware would likely still produce the right number.

</details>

---

## 10. Real-world example

| Where | How the model matters |
|---|---|
| **Linux kernel** | Has its own memory model (`tools/memory-model`, `smp_mb()`, `READ_ONCE/WRITE_ONCE`, `smp_load_acquire/smp_store_release`), formalised in `herd7` ("LKMM"). The release/acquire idea is identical |
| **`std::shared_ptr`** | The reference count decrement is `fetch_sub(acq_rel)` (with libstdc++ using release + acquire fence on the final decrement) so that the thread running the deleter sees all writes made through any other owner |
| **`std::mutex`, `std::thread::join`, `std::future`** | Specified entirely in terms of *synchronizes-with* edges; their implementations contain exactly the acquire/release RMWs and futex calls of the previous experiments |
| **Lock-free queues** (Chapter 32) | Every `head`/`tail` access has a justified order; Dmitry Vyukov's MPMC and the folly/Boost lock-free queues document each ordering |
| **Rust** | Uses the *same* model (`Ordering::{Relaxed, Acquire, Release, AcqRel, SeqCst}`): ideas transfer one-to-one |
| **Java / Go** | Java `volatile` ≈ C++ `seq_cst`; Go's memory model is DRF-SC-style with channel and sync edges |
| **Qt** | `QAtomicInteger` exposes `Relaxed/Acquire/Release/Ordered` operations mapping to C++ orders; `QSharedPointer`/`QObject` reference counts use them (Chapter 48) |
| **Database / OS kernels** | RCU (`rcu_dereference` is `consume`-like), seqlocks, hazard pointers: all built on the release/acquire pattern |

> **Opinion.** Default to **`seq_cst`** (which is what `std::atomic<T>::operator=`, `++`, and the unadorned `load()/store()` use) and to **mutexes**. They are correct by DRF-SC, cheap on x86 (the `xchg` costs ~20 cycles), and nobody ever needed to debug an `acquire` that should have been `seq_cst` at 3 a.m. Move to `acquire`/`release` only for a **measured** hot path, only on a **textbook pattern** (publication, spinlock, SPSC queue, refcount) and **with a happens-before argument in a comment above each non-seq_cst operation**. Use `relaxed` only for values that carry no other data (statistics counters, ids, “progress” hints). Never use `consume`, never invent your own ordering mixture, and never conclude that code is correct because it passed on x86. If you can't write the proof, use a stronger order or a lock. This is *engineering judgment*, not timidity: the cost difference on x86 is one instruction, while the debugging cost of a subtle order bug is days.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Non-atomic shared flag/counter | UB: hoisted loads, torn values, lost updates | `std::atomic`; or a lock |
| `volatile` used as a thread flag | Still a data race; no ordering with other data | `std::atomic<bool>` |
| `relaxed` flag that *publishes* data | Reader sees flag but stale data (on ARM, or after compiler reordering) | `release` store / `acquire` load |
| `acquire` on a store, `release` on a load | Compile error or unspecified meaning (the standard forbids it: stores can't be acquire, loads can't be release) | Pair the correct sides |
| SB pattern with release/acquire (Dekker, Peterson, “announce then check”) | Both threads enter the critical region | `seq_cst` on all four operations, or a fence between store and load |
| Checking `x.load(relaxed) == 1` then reading other data | Missing ordering | Acquire the flag, or use a mutex |
| Fence/acquire mismatch: acquire fence *before* the load | Does nothing useful | Acquire fence goes *after* the relaxed load; release fence *before* the relaxed store |
| Assuming `seq_cst` makes *non-atomic* accesses ordered with respect to *other threads' relaxed* atomics | Subtle races remain | Order matters only along happens-before edges you actually built |
| ABA / reuse of memory in lock-free code | Rare corruption | Hazard pointers, epochs, tagged pointers (Chapter 32) |
| Relying on x86 testing | Passes locally, fails on ARM/POWER, or at higher optimisation | Cross-test on ARM; TSan; herd7 |
| Using `consume` | Treated as `acquire` (fine) but flagged as deprecated; no one verified the dependency chain | Use `acquire` |
| Mixed atomic and non-atomic access to one location | UB (`atomic_ref` exists for atomics on plain objects during phases) | One access mode; `std::atomic_ref` with all accessors using it |
| Huge `is_lock_free() == false` types (`atomic<BigStruct>`) | Hidden mutex (libatomic), different perf, signal-handler unsafe | Pointers, 8/16-byte types, or a lock |
| Over-weakening an order after a benchmark win without a proof | Latent bugs | Write the happens-before argument in a comment; litmus-test it; TSan |

---

## 12. Exercises

1. **By hand.** For the code in §1, add `release`/`acquire` and write the happens-before chain. Then remove the `release`: which edge disappears and what exactly is the data race (which two accesses)?
2. **Predict.** For the litmus tests MP (`x=1; y=1` vs `r1=y; r2=x`), LB (`r1=x; y=1` vs `r2=y; x=1`), and IRIW, list the outcomes that SC forbids, that `acq_rel` forbids, and that real x86 forbids. Run MP and LB as experiments and compare.
3. **Mapping.** Predict the AArch64 instruction for `compare_exchange_weak(e, d, acquire, relaxed)` (success/failure orders differ!). Check with Clang. Then do POWER and RISC-V.
4. **Counter-example hunt.** Take a lock-free stack (push/pop with CAS on `head`) and try `relaxed` for the CAS success order. Argue that a popped node's fields may be unseen; run on ARM if you can.
5. **Refcount.** Implement an intrusive refcount with `fetch_sub(release)` + `acquire` fence and run it under TSan with many threads and a destructor that reads the object's fields.
6. **`atomic_ref`.** Use `std::atomic_ref<int>` to update elements of a plain `std::vector<int>` from several threads during a parallel phase, then read them non-atomically after `join`. Explain why the post-join read is safe.
7. **Seqlock.** Implement it; run readers and writers under TSan; find out which accesses need to be atomic and why.
8. **Read the proof.** Read the happens-before reasoning in the libstdc++ `shared_ptr` control-block (`_M_release`) and in `std::mutex::unlock`/`lock` documentation. Write the graph for `~shared_ptr` on thread A and B racing to drop the last two references.

---

## 13. Challenge: verify a lock-free handoff

Write a single-producer/single-consumer ring buffer of `T` (power-of-two capacity, indices in two separate cache lines). Then:

1. Specify its correctness: FIFO, no loss, no duplication, no torn `T`.
2. Choose the weakest correct memory order for each atomic access and write the happens-before argument for (a) “consumer sees the element written by the producer”, and (b) “producer does not overwrite an element the consumer is still reading”.
3. Prove it with a model checker: encode the algorithm as a `herd7`/CDSChecker/GenMC test (or write a bounded exhaustive scheduler that enumerates SC interleavings, and argue why SC testing is insufficient for relaxed orders).
4. Run it on x86 and on an ARM machine for 10⁹ operations under TSan and without; run a mutation test that weakens each order by one level and show which mutations are caught (and by which tool) and which survive.
5. Benchmark against a mutex+condvar queue; explain where the lock-free win comes from (and the false-sharing effect of putting both indices in one line).

---

## 14. Knowledge check

1. Define *data race* in C++. Why are two atomic operations on the same object never a data race?
2. What are the *sequenced-before*, *synchronizes-with* and *happens-before* relations, and how do they relate?
3. State the DRF-SC theorem and why it matters.
4. What does `memory_order_relaxed` still guarantee?
5. What is the difference between `acq_rel` and `seq_cst`? Give a litmus test that distinguishes them.
6. On x86-64, what instruction does each of: `relaxed` load, `acquire` load, `release` store, `seq_cst` store, `seq_cst` fetch_add, `acquire` fence, `seq_cst` fence compile to?
7. Why can code with a missing `release`/`acquire` pair work on x86 and fail on ARM?
8. Why is a compiler barrier (`asm volatile("" ::: "memory")`) not a substitute for atomics?
9. When does a relaxed load followed by an acquire fence synchronise with a release store?
10. What is a release sequence in C++20?
11. Why is `consume` discouraged?
12. You find `flag.store(true, std::memory_order_relaxed)` publishing a pointer to an initialised object. What's wrong and what is the minimal fix?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Two conflicting accesses (same location, at least one write), at least one of which is non-atomic, not ordered by happens-before. Atomic operations are by definition indivisible and totally ordered per object (modification order), so two atomics on the same object cannot race.
2. Sequenced-before: program order within a thread. Synchronizes-with: a release (or stronger) store read by an acquire (or stronger) load (and the other library synchronisation points). Happens-before: the transitive closure of sequenced-before and synchronizes-with (the partial order that decides visibility and races).
3. If a program has no data race in any sequentially consistent execution, then all its executions are sequentially consistent. So code that uses locks and seq_cst atomics correctly can be reasoned about as simple interleavings.
4. Atomicity (no torn read/write, RMW atomic), and per-object coherence (all threads agree on the modification order; no going backwards; a thread sees its own writes in order). No ordering with other objects.
5. `acq_rel` orders accesses relative to a synchronising partner but does not impose a single total order over all operations; `seq_cst` does. The Store-Buffering test (Experiment 1) distinguishes them: `acq_rel` allows `r1 == r2 == 0`, `seq_cst` forbids it. IRIW likewise.
6. `mov`; `mov`; `mov`; `xchg` (or `mov` + `mfence`); `lock xadd`; nothing (compiler barrier only); a full barrier (`mfence`, or GCC's `lock or [rsp], 0`).
7. x86 TSO hardware provides acquire/release semantics for all plain loads and stores (and only reorders StoreLoad), so a missing pair is masked unless the compiler reorders. ARM may reorder all four kinds, so the missing edge becomes an observable race.
8. It only stops the compiler from moving accesses; it emits no instruction, so the CPU may still reorder (on weak ISAs and StoreLoad on x86), and it creates no happens-before edge and no atomicity.
9. When the relaxed load reads the value written by the release store (or a later value in its release sequence), and the acquire fence follows the load in program order; likewise a release fence before the relaxed store. The fences then synchronise.
10. A release operation A followed by the maximal contiguous run of read-modify-write operations on the same object (in modification order). An acquire load that reads any value in that sequence synchronizes with A.
11. It was meant to give acquire-only ordering for data-dependent reads, but compilers cannot track dependency chains through optimisation, so all implementations treat it as `acquire`; the standard discourages it. RCU/hazard-pointer facilities (C++26) are the intended replacement.
12. A relaxed store creates no happens-before edge, so the reader that sees `true` may see an uninitialised object (or the compiler may reorder the initialisation after the store). Minimal fix: `store(true, std::memory_order_release)` with an `acquire` load on the reader side.

</details>

---

[← Previous: Chapter 29](29-threading.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 31 — Atomics →](31-atomics.md)
