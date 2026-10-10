# Chapter 31 — Atomics

> **Part XII · Concurrency** &nbsp;|&nbsp; **Level 5** (systems/hardware) &nbsp;|&nbsp; **≈ 8 hours**
> **Prerequisites:** [Chapter 27](../part-10-memory/27-cache-and-data-oriented-cpp.md), [Chapter 29](29-threading.md), [Chapter 30](30-memory-model.md) &nbsp;|&nbsp; **Standards:** C++11 (`std::atomic`, `atomic_flag`), C++17 (`is_always_lock_free`), **C++20** (`atomic_ref`, `atomic<shared_ptr>`, `atomic<float>::fetch_add`, `atomic::wait/notify`, `atomic_flag::test`), C++26 (`fetch_max`/`fetch_min` 🟡, `atomic` `std::atomic_ref` additions) &nbsp;|&nbsp; **Tools:** `g++-14`, `-latomic`, TSan, `perf`, `strace`

[← Previous: Chapter 30](30-memory-model.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 32 — Lock-free programming →](32-lock-free.md)

---

**In one sentence:** `std::atomic<T>` is a type whose every access is indivisible and carries a memory order, and its small set of operations (load, store, exchange, compare-exchange, fetch-op, wait/notify) is the vocabulary from which every lock, queue and counter that doesn't use the OS is built.

**By the end of this chapter you can:**

- choose among load/store, exchange, CAS (weak/strong), `fetch_*` and `wait/notify`, and write the CAS-loop idiom correctly
- tell whether an atomic type is lock-free, what it compiles to, and what it costs (measured)
- build a spinlock, ticket lock, SPSC ring buffer and a sharded counter, and give the memory-order argument for each
- use `atomic_ref`, `atomic<shared_ptr>` and `atomic::wait` appropriately
- explain why a contended atomic does not scale and what to do instead

---

## 1. Problem

Mutexes (Chapter 29) are correct and, for most code, fast enough. Three situations push you below them:

1. **A single word of shared state** (a counter, a flag, a pointer swap). A mutex round trip for `++count` is overkill and, under contention, a convoy of futex sleeps.
2. **No blocking allowed**: signal handlers, real-time threads, interrupt context, the inside of an allocator or a mutex implementation itself.
3. **Publication** of data to many readers without making them lock.

Chapter 30 gave you the rules. This chapter gives you the **operations** and the idioms, and then immediately shows you what a hardware atomic actually costs, since “lock-free” does not mean “fast”.

---

## 2. Historical context

| Year | Event |
|---|---|
| 1960s–70s | Test-and-set (IBM 360), compare-and-swap (IBM 370, 1970): the first universal synchronisation primitives |
| 1991 | Herlihy, *Wait-Free Synchronization*: CAS is a **universal** primitive (can implement any concurrent object wait-free); test-and-set and fetch-and-add are not |
| 1990s | x86 `lock cmpxchg`, SPARC `cas`, Alpha/POWER/ARM load-linked/store-conditional (LL/SC) |
| 2004 | Michael & Scott queue (1996) and hazard pointers (2004) make lock-free structures practical |
| 2005–11 | GCC `__sync_*` builtins, then `__atomic_*` with memory orders; C++11 `std::atomic` standardised on them |
| 2014 | ARMv8.1 **LSE** adds single-instruction `ldadd`, `cas`, `swp`, which scale far better under contention than LL/SC loops |
| 2017 | `is_always_lock_free`; C++17 sizes of lock-free atomics become queryable at compile time |
| **2020** | **C++20**: `atomic_ref<T>` (atomic operations on ordinary objects), `atomic<shared_ptr<T>>`/`weak_ptr`, `atomic<floating>::fetch_add`, `wait/notify_one/notify_all` (futex-backed), `atomic_flag::test`, default-initialised `atomic<T>` |
| 2024–26 | C++26 adds `fetch_max`/`fetch_min` (P0493) 🟡 and hazard pointer / RCU facilities (Chapter 32). GCC 14.2 does not yet provide `fetch_max`; use a CAS loop (Experiment 2) |

---

## 3. Modern solution

```cpp
std::atomic<int> counter{0};
counter.fetch_add(1, std::memory_order_relaxed);        // statistics counter: no ordering needed

std::atomic<Node*> head{nullptr};
Node* old = head.load(std::memory_order_relaxed);
do { n->next = old; }
while (!head.compare_exchange_weak(old, n,                // CAS loop: publish n if head is still `old`
         std::memory_order_release, std::memory_order_relaxed));

std::atomic<bool> ready{false};
ready.wait(false);                                       // C++20: block (futex) until it's no longer false
ready.store(true, std::memory_order_release); ready.notify_all();
```

Only a handful of operations exist; their power is in the **ordering argument** you attach to each.

---

## 4. Mental model

### An atomic object is a memory word with a *transaction interface*

```text
   std::atomic<T> a;            ≈   a cache line you can lock for the duration of one instruction

   load()                       read the current value (atomic: never torn)
   store(v)                     overwrite
   exchange(v)                  swap in v, return the old value                      (1 RMW)
   compare_exchange(e, d)       if (a == e) { a = d; return true; } else { e = a; return false; }   (1 RMW, "CAS")
   fetch_add / sub / and / or / xor   a = a op v; return the OLD value               (1 RMW)
   wait(old) / notify_one/all   block until a != old, wake waiters                   (futex)
```

Every **read-modify-write (RMW)** is a *single* indivisible step in the object's modification order: that is what makes `fetch_add` exact where `load; store` loses updates. All RMWs on x86 are `lock`-prefixed instructions that hold the cache line in the core in Modified state for the duration.

### CAS is the universal building block

`compare_exchange` turns “I computed a new state from the old one” into a *conditional commit*:

```text
   loop:   old = a.load()
           new = f(old)                  ← any pure function of old
           if a.compare_exchange(old, new)  → done      (nobody changed `a` since we read it)
           else                           → `old` was refreshed with the current value; retry
```

Retries mean the algorithm is **lock-free** (somebody always makes progress) but not **wait-free** (a particular thread can starve). `compare_exchange_weak` may fail *spuriously* (LL/SC machines) but is cheaper in loops; `_strong` retries internally; on x86 they compile to the same `lock cmpxchg`.

### The scaling model: a hot cache line is a serial resource

```text
   Throughput of N threads doing fetch_add on ONE atomic ≈ 1 / (cache-line transfer time), not N × single-thread throughput.
   Each op needs the line in Modified state → the line ping-pongs between cores (Chapter 27, MESI).
```

An atomic counter that every thread increments is a *global lock in disguise*. The standard cures: **shard** (per-thread/per-core counters summed on demand), **batch** (accumulate locally, flush every N), **avoid sharing** (partition the work).

### Lock-freedom vs wait-freedom vs obstruction-freedom

| Property | Meaning | Example |
|---|---|---|
| **Blocking** | A stalled thread can stop everyone | mutex |
| **Obstruction-free** | A thread running alone makes progress | simple optimistic schemes |
| **Lock-free** | *Some* thread always makes progress in a finite number of steps | CAS-loop push onto a stack |
| **Wait-free** | *Every* thread makes progress in a bounded number of steps | `fetch_add` on x86; SPSC ring buffer |

(Lock-freedom is a statement about progress guarantees, not speed; Chapter 32.)

---

## 5. Language rules

### 5.1 `std::atomic<T>` requirements  `[atomics.types]`

- `T` must be **trivially copyable** (and, since C++20, copy/move constructible and assignable): the object is `memcpy`-able. `std::string` is not allowed.
- Specialisations: integral types (all `fetch_*`: add, sub, and, or, xor, and since C++26 max/min), pointers (`fetch_add` with pointer arithmetic), `bool`, floating point (C++20: `fetch_add/sub`), `shared_ptr`/`weak_ptr` (C++20).
- **Lock-free or not** is a property of the type on the implementation: `a.is_lock_free()` (runtime), `std::atomic<T>::is_always_lock_free` (constexpr). If not lock-free, the library uses a hidden lock: a hash of the address indexes a table of mutexes (`libatomic`), which is *not* async-signal-safe and not wait-free. On x86-64/GCC: sizes 1, 2, 4, 8 are always lock-free; **16 bytes is not** (needs `cmpxchg16b`; GCC routes it through `libatomic` and reports `false`); larger types never.
- **Padding bits**: `compare_exchange` compares object representations. GCC ≥ 11 clears padding so a struct with padding compares equal by value; do not rely on it across compilers (Experiment 1).
- `std::atomic<T>` is neither copyable nor movable. `atomic<T>::operator=(T)` and `T()` conversions use `seq_cst`. **The defaults are the safest order.**
- C++20 constructs `std::atomic<T>` value-initialised (zero) by default; before C++20 `std::atomic<int> a;` was uninitialised.

### 5.2 Compare-exchange  `[atomics.ref.ops]`

```cpp
bool compare_exchange_weak  (T& expected, T desired, std::memory_order success, std::memory_order failure);
bool compare_exchange_strong(T& expected, T desired, std::memory_order success, std::memory_order failure);
```

- On success: RMW with order `success`. On failure: a plain **load** with order `failure` (so it cannot contain `release`), and `expected` receives the observed value.
- Pre-C++17 `failure` could not be stronger than `success`; that restriction was removed.
- The single-order overloads derive `failure` from `success` (`acq_rel` → `acquire`, `release` → `relaxed`).
- **`weak` may fail even when `*this == expected`** (LL/SC reservation lost). Always use `weak` in a loop; use `strong` for one-shot attempts.

### 5.3 `atomic_flag` and `wait/notify`

- `std::atomic_flag` is the only type guaranteed lock-free: `test_and_set`, `clear`, and since C++20 `test`, `wait`, `notify_*`. It is the classic spinlock building block.
- `a.wait(old, order)` blocks while `a.load() == old`; it can wake spuriously, so always re-check (the standard version loops internally comparing with `old`). `notify_one/all` wake waiters; on Linux they map to `futex`. **A notify without modifying the atomic does nothing useful.** Pattern: store, then notify.
- Under the hood libstdc++ keeps a small table of waiter counters (hash of the address) so `notify` on an atomic with no waiters is nearly free.

### 5.4 `std::atomic_ref<T>` (C++20)

A reference-like wrapper giving atomic access to a **non-atomic object** for the lifetime of the `atomic_ref`. While any `atomic_ref` to an object exists, *all* concurrent accesses to it must go through `atomic_ref`s. Requires suitable alignment (`atomic_ref<T>::required_alignment`). Use it for phases: parallel update of a big array/vector of plain integers (histogram), then plain reads after a `join`.

### 5.5 `std::atomic<std::shared_ptr<T>>` (C++20)

Provides atomic `load/store/exchange/compare_exchange` on a `shared_ptr`. **Not lock-free in libstdc++ (and most implementations)**: it uses an internal spinlock/mutex table, so it is a convenience for the read-copy-update-style “swap a config snapshot”, not a high-performance primitive (Experiment 7).

### 5.6 What `volatile` is not

`volatile std::atomic<T>` exists but `volatile` alone gives no atomicity, no ordering across threads and no happens-before. Use `volatile` only for memory-mapped I/O and `sig_atomic_t` signal flags.

### Layer check

| Layer | Question | Answer |
|---|---|---|
| **C++ standard** | What is promised? | Atomicity of every operation; RMW = one step in modification order; orders as in Chapter 30; `is_always_lock_free` accuracy; nothing about fairness or speed |
| **Compiler / library** | What does libstdc++ do? | Header-only wrappers over `__atomic_*` builtins (inline instructions when lock-free); non-lock-free sizes call `__atomic_load_16` etc. in `libatomic.so` (link `-latomic`); `wait/notify` use a futex-backed waiter pool |
| **ABI** | What's fixed? | Layout (`sizeof(atomic<T>) == sizeof(T)` for lock-free types), alignment (`alignof(atomic<T>) >= sizeof(T)` for power-of-two sizes ≤ 16), the `__atomic` symbol names in libatomic; `atomic<shared_ptr>` is 16 bytes in libstdc++ ≥ 12 |
| **OS** | What helps? | `futex(FUTEX_WAIT/WAKE)` for blocking waits; signals interrupt non-lock-free operations (UB to use them from a handler) |
| **CPU** | What executes? | x86: `lock`-prefixed RMWs (full barriers), plain `mov` for acquire/release; ARM: LL/SC or LSE atomics; each RMW needs exclusive ownership of the cache line (~20 cycles uncontended, ~100+ contended) |

---

## 6. Implementation model

### What a `lock cmpxchg` loop looks like

```asm
; head.compare_exchange_weak(old, n, release, relaxed)   (x86-64)
.retry:  mov  rax, [head]            ; old  (relaxed load)
         mov  [n+next], rax          ; n->next = old
         lock cmpxchg [head], rdx    ; if head == rax: head = rdx (ZF=1) else rax = head (ZF=0)
         jne  .retry
```

One locked instruction per attempt; failure refreshes `rax` for free, which is why the idiom `do { n->next = old; } while (!cas(old, n))` needs no reload.

### How `fetch_add` becomes a CAS loop when no instruction exists

`fetch_and`/`fetch_or` on x86 whose result is *used* cannot use `lock and` (it doesn't return the old value): the compiler emits a `cmpxchg` loop. `fetch_add` and `fetch_sub` have `lock xadd`. A general `atomic<T>` with an arbitrary function is always a CAS loop.

### `atomic::wait` internally

`wait(old)`: load; if `!= old` return; else spin briefly, then register in a hashed waiter table, `futex(WAIT, &counter)`. `notify_*` increments/inspects the table entry and `futex(WAKE)`s only if waiters exist. So a producer pays a few ns when nobody waits.

### Memory footprint and false sharing

An `atomic<int>` is 4 bytes; an array of them packs 16 per cache line, so adjacent per-thread counters **false-share** (Chapter 27). `alignas(std::hardware_destructive_interference_size)` (or a hard-coded 64/128) separates them.

---

## 7. Experiments

### Experiment 1 ✅: Which atomics are lock-free? Padding and `-latomic`

```cpp
// @test run -std=c++23 -O2 link=-latomic
#include <atomic>
#include <cstdio>
#include <memory>

struct P  { char c; int i; };            // 8 bytes with 3 padding bytes
struct S8 { int a, b; };
struct S16 { long a, b; };
struct S24 { long a, b, c; };

template <class T> void report(const char* name) {
    std::atomic<T> a{};
    std::printf("  %-28s sizeof=%2zu  is_lock_free=%d  is_always_lock_free=%d\n", name, sizeof(std::atomic<T>), a.is_lock_free(),
                std::atomic<T>::is_always_lock_free);
}

int main() {
    std::puts("lock-freedom on x86-64, GCC 14 (the library may use a hidden mutex otherwise):");
    report<char>("char");
    report<short>("short");
    report<int>("int");
    report<long>("long");
    report<void*>("void*");
    report<float>("float");
    report<double>("double");
    report<S8>("struct {int,int}");
    report<S16>("struct {long,long}  16 B");
    report<S24>("struct {long x3}     24 B");
    report<std::shared_ptr<int>>("shared_ptr<int>");

    // compare_exchange compares the object representation. GCC >= 11 clears padding so this works; don't rely on it elsewhere.
    std::atomic<P> p{P{1, 2}};
    P expected{1, 2};
    std::printf("CAS on a struct with padding succeeded: %d\n", p.compare_exchange_strong(expected, P{3, 4}));

    std::atomic<float> f{1.0f};
    f.fetch_add(2.5f);                                    // C++20: floating-point fetch_add (a CAS loop underneath)
    std::printf("atomic<float>::fetch_add -> %.1f\n", f.load());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
lock-freedom on x86-64, GCC 14 (the library may use a hidden mutex otherwise):
  char                         sizeof= 1  is_lock_free=1  is_always_lock_free=1
  short                        sizeof= 2  is_lock_free=1  is_always_lock_free=1
  int                          sizeof= 4  is_lock_free=1  is_always_lock_free=1
  long                         sizeof= 8  is_lock_free=1  is_always_lock_free=1
  void*                        sizeof= 8  is_lock_free=1  is_always_lock_free=1
  float                        sizeof= 4  is_lock_free=1  is_always_lock_free=1
  double                       sizeof= 8  is_lock_free=1  is_always_lock_free=1
  struct {int,int}             sizeof= 8  is_lock_free=1  is_always_lock_free=1
  struct {long,long}  16 B     sizeof=16  is_lock_free=0  is_always_lock_free=0
  struct {long x3}     24 B    sizeof=24  is_lock_free=0  is_always_lock_free=0
  shared_ptr<int>              sizeof=16  is_lock_free=0  is_always_lock_free=0
CAS on a struct with padding succeeded: 1
atomic<float>::fetch_add -> 3.5
```

Rules of thumb: **≤ 8 bytes and power-of-two: lock-free everywhere that matters.** 16 bytes: lock-free only with hardware double-width CAS, and GCC reports `false` regardless (it routes through `libatomic`, which may or may not use `cmpxchg16b` at run time). Larger: a hidden mutex. `atomic<shared_ptr>` is not lock-free. Anything non-lock-free is unsuitable for signal handlers, real-time paths and “lock-free” algorithms (it silently reintroduces locks). Note the link flag: without `-latomic` the 16- and 24-byte instantiations fail to link.

### Experiment 2 ✅: The CAS-loop idiom: atomic max and a read-modify-write of your own

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=60
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

// C++26 adds fetch_max; before that, this is the idiom for *any* custom RMW.
template <class T>
T atomic_fetch_max(std::atomic<T>& a, T v, std::memory_order mo = std::memory_order_relaxed) {
    T old = a.load(std::memory_order_relaxed);
    while (old < v && !a.compare_exchange_weak(old, v, mo, std::memory_order_relaxed)) {
        // on failure `old` now holds the current value; the loop re-tests `old < v`
    }
    return old;
}

// A custom RMW: saturating add (stops at a cap), impossible with fetch_add alone.
int saturating_add(std::atomic<int>& a, int delta, int cap) {
    int old = a.load(std::memory_order_relaxed), desired;
    do { desired = std::min(old + delta, cap); }
    while (!a.compare_exchange_weak(old, desired, std::memory_order_relaxed));
    return old;
}

int main() {
    std::atomic<int> mx{0}, sat{0};
    {
        std::vector<std::jthread> ts;
        for (int t = 0; t < 2; ++t)
            ts.emplace_back([&, t] {
                for (int i = 0; i < 200'000; ++i) { atomic_fetch_max(mx, i * 2 + t); saturating_add(sat, 1, 1'000'000); }
            });
    }
    std::printf("atomic max     = %d (expected %d)\n", mx.load(), 199'999 * 2 + 1);
    std::printf("saturating add = %d (400000 increments, cap 1000000 => 400000)\n", sat.load());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
atomic max     = 399999 (expected 399999)
saturating add = 400000 (400000 increments, cap 1000000 => 400000)
```

The two things to internalise: (1) the loop body must be a **pure function of `old`** (no side effects: it may run many times); (2) after a failed CAS `old` has already been refreshed, so never reload it inside the loop.

### Experiment 3 🧩: Weak vs strong CAS, and what the RMWs compile to

```cpp
// @test asm -std=c++23 -O2 -fno-stack-protector -fcf-protection=none filter=cas_weak_loop,cas_strong_once,fetch_and_used,fetch_and_unused,exchange_flag,test_and_set
#include <atomic>

std::atomic<int> a;
std::atomic<unsigned> bits;
std::atomic_flag fl;

int cas_weak_loop(int d) {
    int old = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(old, old + d, std::memory_order_acq_rel, std::memory_order_relaxed)) {}
    return old;
}
bool cas_strong_once(int& e) { return a.compare_exchange_strong(e, 7); }
unsigned fetch_and_used(unsigned m)   { return bits.fetch_and(m); }      // result used: needs the old value => cmpxchg loop
void     fetch_and_unused(unsigned m) { bits.fetch_and(m); }             // result discarded: a plain `lock and`
int  exchange_flag(int v)             { return a.exchange(v); }          // xchg (implicitly locked)
bool test_and_set()                   { return fl.test_and_set(std::memory_order_acquire); }
```

```asm
; asm (gcc 14.2.0, -O2, x86-64, Intel syntax)
cas_weak_loop(int):
	mov	eax, DWORD PTR a[rip]
.L2:
	lea	edx, [rdi+rax]
	lock cmpxchg	DWORD PTR a[rip], edx
	jne	.L2
	ret

cas_strong_once(int&):
	mov	eax, DWORD PTR [rdi]
	mov	edx, 7
	lock cmpxchg	DWORD PTR a[rip], edx
	sete	dl
	je	.L6
	mov	DWORD PTR [rdi], eax
.L6:
	mov	eax, edx
	ret

fetch_and_used(unsigned int):
	mov	eax, DWORD PTR bits[rip]
.L9:
	mov	ecx, eax
	mov	edx, eax
	and	ecx, edi
	lock cmpxchg	DWORD PTR bits[rip], ecx
	jne	.L9
	mov	eax, edx
	ret

fetch_and_unused(unsigned int):
	lock and	DWORD PTR bits[rip], edi
	ret

exchange_flag(int):
	mov	eax, edi
	xchg	eax, DWORD PTR a[rip]
	ret

test_and_set():
	mov	eax, 1
	xchg	al, BYTE PTR fl[rip]
	ret
```

On x86 `weak` and `strong` are the same `lock cmpxchg`. Note how `fetch_and` changes when its result is discarded: **the compiler picks a cheaper instruction if you don't use the return value**, one more reason to ignore results you don't need.

### Experiment 4 ✅: Spinlocks: `test_and_set`, test-and-test-and-set with backoff, and the ticket lock

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=120
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__x86_64__)
#  define CPU_PAUSE() __builtin_ia32_pause()
#else
#  define CPU_PAUSE() ((void)0)
#endif

// 1. Naive: every spin iteration is an RMW => hammers the cache line
class TasLock {
    std::atomic_flag f_ = ATOMIC_FLAG_INIT;
public:
    void lock() noexcept   { while (f_.test_and_set(std::memory_order_acquire)) {} }
    void unlock() noexcept { f_.clear(std::memory_order_release); }
};
// 2. Test-and-test-and-set: spin on a read-only load (shared state in cache), RMW only when it looks free; `pause` in the loop
class TtasLock {
    std::atomic<bool> b_{false};
public:
    void lock() noexcept {
        for (;;) {
            if (!b_.exchange(true, std::memory_order_acquire)) return;
            while (b_.load(std::memory_order_relaxed)) CPU_PAUSE();
        }
    }
    void unlock() noexcept { b_.store(false, std::memory_order_release); }
};
// 3. Ticket lock: FIFO fairness; each waiter spins on a shared "now serving" counter
class TicketLock {
    std::atomic<unsigned> next_{0}, serving_{0};
public:
    void lock() noexcept {
        unsigned my = next_.fetch_add(1, std::memory_order_relaxed);
        while (serving_.load(std::memory_order_acquire) != my) CPU_PAUSE();
    }
    void unlock() noexcept { serving_.store(serving_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }
};

template <class L> double bench(const char* name, int threads, long iters) {
    L lock; long counter = 0;
    auto t0 = std::chrono::steady_clock::now();
    {
        std::vector<std::jthread> ts;
        for (int t = 0; t < threads; ++t) ts.emplace_back([&] { for (long i = 0; i < iters; ++i) { lock.lock(); ++counter; lock.unlock(); } });
    }
    double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (double(iters) * threads);
    std::printf("  %-12s %d thread(s): %7.1f ns per lock/unlock   (counter %ld %s)\n", name, threads, ns, counter, counter == iters * threads ? "ok" : "WRONG");
    return ns;
}

int main() {
    constexpr long N = 1'000'000;
    for (int threads : {1, 2}) {
        bench<TasLock>("TAS", threads, N);
        bench<TtasLock>("TTAS+pause", threads, N);
        bench<TicketLock>("ticket", threads, N);
        bench<std::mutex>("std::mutex", threads, N);
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
  TAS          1 thread(s):     8.6 ns per lock/unlock   (counter 1000000 ok)
  TTAS+pause   1 thread(s):     8.3 ns per lock/unlock   (counter 1000000 ok)
  ticket       1 thread(s):     8.1 ns per lock/unlock   (counter 1000000 ok)
  std::mutex   1 thread(s):    19.1 ns per lock/unlock   (counter 1000000 ok)
  TAS          2 thread(s):    12.3 ns per lock/unlock   (counter 2000000 ok)
  TTAS+pause   2 thread(s):    12.7 ns per lock/unlock   (counter 2000000 ok)
  ticket       2 thread(s):   227.7 ns per lock/unlock   (counter 2000000 ok)
  std::mutex   2 thread(s):    30.5 ns per lock/unlock   (counter 2000000 ok)
```

What the numbers say (2 vCPUs): uncontended, all three spinlocks cost ~8 ns, less than half of `std::mutex` (19 ns), because they are one locked instruction plus one plain store. With two threads, TAS and TTAS stay at ~12 ns (still faster than the mutex's 30 ns), but the **ticket lock collapses to ~228 ns per operation**, 20× worse, even though there are only two threads on two cores. FIFO hand-off is the reason: the lock can only pass to the *specific* next ticket holder, and on a virtualised host the vCPU of that waiter may not be running at that moment, so everyone waits for the hypervisor to schedule it. A barging lock (TAS/TTAS) lets whichever thread is *currently running* take the lock. This is the lock-holder/next-waiter **preemption problem**: **spinlocks are only safe when the number of runnable threads ≤ cores and the scheduler cannot take a core away from a waiter or holder**. With more threads than cores, or under virtualisation, a thread holding the lock can be descheduled while the others burn their time slices spinning, which is why `std::mutex` sleeps in the kernel and why user-space spinlocks in general-purpose software are a mistake. Use them for critical sections of a few instructions in code that controls its own scheduling (kernels, real-time, runtime internals), and prefer a mutex with adaptive spinning (glibc's `PTHREAD_MUTEX_ADAPTIVE_NP`) otherwise.

### Experiment 5 ✅: `atomic::wait` / `notify`: a futex-backed event, verified with `strace`-free counting

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=60
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    std::atomic<int> state{0};
    std::atomic<long> spins{0};

    std::jthread waiter([&] {
        // blocks (futex) until state != 0; no busy loop, no condition_variable, no mutex
        state.wait(0, std::memory_order_acquire);
        std::printf("waiter: woke up, state = %d\n", state.load(std::memory_order_relaxed));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::puts("main: setting state and notifying");
    state.store(1, std::memory_order_release);
    state.notify_one();
    waiter.join();

    // Producer/consumer handoff with wait/notify (a binary event reused in a loop)
    std::atomic<int> turn{0};
    constexpr int N = 20'000;
    auto t0 = std::chrono::steady_clock::now();
    std::jthread pong([&] {
        for (int i = 0; i < N; ++i) { turn.wait(0); turn.store(0); turn.notify_one(); }
    });
    for (int i = 0; i < N; ++i) { turn.store(1); turn.notify_one(); turn.wait(1); }
    pong.join();
    double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / N;
    std::printf("wait/notify ping-pong round trip: %.2f us (compare condvar: Chapter 29, Experiment 7)\n", us);
    return (int)spins;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
main: setting state and notifying
waiter: woke up, state = 1
wait/notify ping-pong round trip: 2.97 us (compare condvar: Chapter 29, Experiment 7)
```

The round trip took about **3 µs** here, compared with about 7 µs for the `condition_variable` ping-pong of Chapter 29. `atomic::wait` removes the need for a mutex + condition variable + predicate when the *entire* shared state is one word. It is how `std::latch`, `std::counting_semaphore` and `std::barrier` are implemented in libstdc++. Remember the rule: the value must change (or be notified explicitly) for waiters to proceed; set it, then notify.

### Experiment 6 ✅: A single-producer single-consumer ring buffer: wait-free, with a happens-before argument

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=120
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <thread>
#include <new>

template <class T, std::size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "capacity must be a power of two");
    alignas(64) std::atomic<std::size_t> head_{0};     // consumer index (read position)  -- written only by the consumer
    alignas(64) std::atomic<std::size_t> tail_{0};     // producer index (write position) -- written only by the producer
    alignas(64) std::array<T, N> buf_{};
public:
    bool try_push(const T& v) {                         // producer thread only
        std::size_t t = tail_.load(std::memory_order_relaxed);          // own index: relaxed is enough
        if (t - head_.load(std::memory_order_acquire) == N) return false;   // full. acquire: the consumer's release of head_ means the slot is free to reuse
        buf_[t & (N - 1)] = v;                                           // (1) write the element (plain store)
        tail_.store(t + 1, std::memory_order_release);                   // (2) publish: element (1) happens-before the consumer's read
        return true;
    }
    bool try_pop(T& out) {                              // consumer thread only
        std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) return false;    // empty. acquire pairs with (2): element is visible
        out = buf_[h & (N - 1)];                                         // (3) read the element
        head_.store(h + 1, std::memory_order_release);                   // (4) free the slot: (3) happens-before the producer's reuse
        return true;
    }
};

int main() {
    constexpr std::size_t kItems = 5'000'000;
    SpscRing<long, 1024> ring;
    long errors = 0;
    auto t0 = std::chrono::steady_clock::now();
    std::jthread producer([&] { for (long i = 1; i <= (long)kItems; ++i) while (!ring.try_push(i)) {} });
    long expected = 1;
    while (expected <= (long)kItems) {
        long v;
        if (ring.try_pop(v)) { if (v != expected) ++errors; ++expected; }
    }
    producer.join();
    double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kItems;
    std::printf("%zu items through the ring, %ld ordering errors, %.1f ns per item\n", kItems, errors, ns);
    return errors == 0 ? 0 : 1;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
5000000 items through the ring, 0 ordering errors, 62.8 ns per item
```

**The argument, edge by edge.** (a) *Producer → consumer:* the consumer's acquire load of `tail_` reads the value stored by the producer's release store (2); (1) is sequenced before (2) and (3) after the load, so (1) happens-before (3): the element is fully written when read. (b) *Consumer → producer:* the producer's acquire load of `head_` reads the consumer's release store (4); (3) happens-before the producer's later overwrite of that slot (1′). Each index has exactly one writer, so plain `store` (not RMW) is enough: this is why it is **wait-free**: no CAS, no retry loop. (The measured ~63 ns per item is dominated by this two-vCPU VM: both threads spin on each other's cache lines and the hypervisor interleaves them; bare-metal figures of 5–20 ns are typical. Compare it with Chapter 29's ~7 µs condvar hand-off to see why rings are used for high-rate paths.) The `relaxed` loads of one's own index are fine because no other thread writes it. The `alignas(64)` separation of `head_`, `tail_` and the buffer prevents false sharing; remove it and re-run (Exercise 3).

### Experiment 7 🔧: Contended counters don't scale: shared atomic vs sharded vs local

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=180
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

constexpr int kLine = 64;
struct alignas(kLine) PaddedCounter { std::atomic<long> v{0}; };
struct PackedCounter { std::atomic<long> v{0}; };

template <class F> double run(int threads, long iters, F&& per_thread) {
    auto t0 = std::chrono::steady_clock::now();
    { std::vector<std::jthread> ts; for (int t = 0; t < threads; ++t) ts.emplace_back([&, t] { per_thread(t, iters); }); }
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (double(iters) * threads);
}

int main() {
    constexpr long N = 20'000'000;
    std::printf("ns per increment (lower is better); N = %ld increments per thread\n", N);
    std::printf("  %-34s %8s %8s\n", "strategy", "1 thread", "2 threads");
    for (int variant = 0; variant < 4; ++variant) {
        double res[2] = {};
        for (int ti = 0; ti < 2; ++ti) {
            int threads = ti + 1;
            std::atomic<long> shared{0};
            std::vector<PackedCounter> packed(threads);
            std::vector<PaddedCounter> padded(threads);
            switch (variant) {
                case 0: res[ti] = run(threads, N, [&](int, long n) { for (long i = 0; i < n; ++i) shared.fetch_add(1, std::memory_order_relaxed); }); break;
                case 1: res[ti] = run(threads, N, [&](int t, long n) { for (long i = 0; i < n; ++i) packed[t].v.fetch_add(1, std::memory_order_relaxed); }); break;
                case 2: res[ti] = run(threads, N, [&](int t, long n) { for (long i = 0; i < n; ++i) padded[t].v.fetch_add(1, std::memory_order_relaxed); }); break;
                case 3: res[ti] = run(threads, N, [&](int, long n) { long local = 0; for (long i = 0; i < n; ++i) { ++local; asm volatile("" : "+r"(local)); } shared.fetch_add(local, std::memory_order_relaxed); }); break;
            }
        }
        const char* names[] = {"one shared atomic", "per-thread, adjacent (false sharing)", "per-thread, padded to a cache line", "thread-local count, one flush at end"};
        std::printf("  %-34s %8.2f %8.2f\n", names[variant], res[0], res[1]);
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
ns per increment (lower is better); N = 20000000 increments per thread
  strategy                           1 thread 2 threads
  one shared atomic                      6.15     7.00
  per-thread, adjacent (false sharing)     6.01     7.28
  per-thread, padded to a cache line     7.14     4.69
  thread-local count, one flush at end     0.64     0.33
```

The numbers here (2 vCPUs): the shared atomic costs 6–7 ns per increment; the false-sharing layout (separate counters in one cache line) costs the same, 7 ns; padded counters drop to 4.7 ns with two threads; and a thread-local count with one flush at the end costs **0.3–0.6 ns**, i.e. an order of magnitude less. The ranking is the lesson: sharing one atomic and false-sharing are equally bad because the cache line bounces either way; padding fixes the bounce; not sharing at all removes the atomic from the hot path entirely. With only two cores the differences are modest; on 16–64 cores a shared atomic degrades by orders of magnitude (Exercise 7). Design rule: **make the common operation touch only memory that no other core is writing.**

### Experiment 8 ✅: `atomic_ref`: atomic updates to plain data, then plain reads after the phase

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=60
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    // A plain vector<int> histogram: no atomics in the storage type, so it can be memcpy'd, sorted, serialised, passed to C.
    std::vector<int> hist(16, 0);
    constexpr int N = 1'000'000;
    {
        std::vector<std::jthread> ts;
        for (int t = 0; t < 2; ++t)
            ts.emplace_back([&, t] {
                for (int i = 0; i < N; ++i) {
                    std::atomic_ref<int> bin(hist[(i * 7 + t) & 15]);            // atomic view of the plain object
                    bin.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }   // join => happens-before the plain reads below: the "atomic phase" is over
    long total = 0;
    for (int c : hist) total += c;                                              // plain reads are fine now
    std::printf("histogram total = %ld (expected %d); required alignment of atomic_ref<int> = %zu\n", total, 2 * N,
                std::atomic_ref<int>::required_alignment);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
histogram total = 2000000 (expected 2000000); required alignment of atomic_ref<int> = 4
```

The contract: while any `atomic_ref` exists for an object, every concurrent access to that object must use an `atomic_ref`. The phase boundary (`join`, a barrier, a latch) provides the happens-before that makes plain access legal again. It is the right tool when the data must stay a plain array (large, interop, GPU/MPI buffers), and the wrong one when a `std::atomic<int>` member would do.

### Experiment 9 ✅: `atomic<shared_ptr>`: config snapshots (and why not lock-free)

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=60
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

struct Config { int version; std::string name; };

int main() {
    std::atomic<std::shared_ptr<const Config>> current{std::make_shared<const Config>(Config{1, "initial"})};
    std::printf("atomic<shared_ptr>::is_lock_free() = %d (libstdc++ uses an internal lock-pool/spin protocol)\n", current.is_lock_free());

    std::atomic<bool> stop{false};
    long reads = 0, inconsistent = 0;
    std::jthread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            std::shared_ptr<const Config> c = current.load();                  // a consistent snapshot: version and name always pair up
            if (c->name != "v" + std::to_string(c->version) && c->version != 1) ++inconsistent;
            ++reads;
        }
    });
    for (int v = 2; v <= 2000; ++v)
        current.store(std::make_shared<const Config>(Config{v, "v" + std::to_string(v)}));   // writer publishes a whole new immutable object
    stop.store(true);
    reader.join();
    std::printf("reader performed %ld snapshot loads; inconsistent snapshots seen: %ld\n", reads, inconsistent);
    return inconsistent == 0 ? 0 : 1;
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
atomic<shared_ptr>::is_lock_free() = 0 (libstdc++ uses an internal lock-pool/spin protocol)
reader performed 1264 snapshot loads; inconsistent snapshots seen: 0
```

This is **read-copy-update (RCU) in miniature**: readers get an immutable snapshot, the writer builds a new object and swaps the pointer, old versions die when the last reader drops its `shared_ptr` (Chapter 25). It is correct, simple, and avoids reader/writer locks, but each `load` is a hidden lock or refcount RMW and is slower than a hazard-pointer or epoch scheme (Chapter 32); use it where reads are not the hottest path.

---

## 8. Assembly / runtime investigation

```bash
# (1) What do my atomics compile to, on every architecture?  (Chapter 30 table)
g++-14 -std=c++23 -O2 -S -masm=intel -o - a.cpp | c++filt | grep -E "lock|xchg|mfence|cmpxchg|xadd"
nm -C a.o | grep -i atomic                 # undefined __atomic_*_16 => hidden libatomic locks!

# (2) Who owns this cache line?  perf c2c shows HITM (hit-modified-in-another-core) traffic = contended atomics
perf c2c record ./prog && perf c2c report --stdio | head -50

# (3) How often is the futex path taken?  (atomic::wait, mutex, condvar)
strace -f -c -e trace=futex ./prog        # calls and time in futex

# (4) Verify lock-freedom claims at compile time and run time
static_assert(std::atomic<T>::is_always_lock_free);
# and in CI:   g++ -DNDEBUG … && nm -u prog | grep -c __atomic   (should be 0 for a lock-free design)

# (5) Race / order bugs: TSan understands acquire/release and atomic_ref
g++-14 -std=c++23 -O1 -g -fsanitize=thread prog.cpp -pthread

# (6) On ARM, see LL/SC vs LSE:  -march=armv8-a  →  ldaxr/stlxr loops;  -march=armv8.1-a  →  casal, ldaddal
clang++-18 --target=aarch64-linux-gnu -march=armv8.1-a -O2 -S -o - a.cpp | grep -E "cas|ldadd|swp"
```

---

## 9. Implementation exercise

Build a small **atomics toolkit**, each with an ordering justification in comments:

1. `Spinlock` (TTAS + pause + exponential backoff + `yield` after N spins) and a `std::mutex`-compatible adaptor so it works with `lock_guard`; benchmark against `std::mutex` for critical sections of 10 ns, 1 µs and 100 µs with 1/2/4+ threads.
2. `ShardedCounter`: N cache-line-padded slots indexed by a hashed thread id; `add(n)` relaxed, `read()` sums with acquire loads; relate precision to concurrency.
3. `OnceFlag`/`Latch` from `atomic<int>` + `wait/notify`.
4. `AtomicStack` (Treiber): `push` via CAS (release), `pop` via CAS (acquire). Run it with TSan; then identify the ABA hazard (Chapter 32).
5. `SeqLock<T>` for a small POD: single writer, many readers retry on version change; write the memory orders and *why the payload copy must use atomics (or `memcpy` between fences)* to avoid a formal data race.
6. `SpscRing` (Experiment 6) extended with batch push/pop (one release store per batch) and benchmark the batch effect.

<details>
<summary><strong>Solution sketch: a Treiber stack (push/pop with CAS)</strong></summary>

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=60
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

// NOTE: pop() here has the ABA problem and a use-after-free hazard (the popped node is read after another thread may free it).
// To keep the demonstration safe we never free nodes during the concurrent phase. Chapter 32 fixes this properly.
template <class T>
class TreiberStack {
    struct Node { T value; Node* next; };
    std::atomic<Node*> head_{nullptr};
    std::vector<Node*> graveyard_;            // popped nodes, freed at destruction (single-threaded)
    std::atomic<bool> grave_lock_{false};
public:
    ~TreiberStack() { for (Node* n : graveyard_) delete n; for (Node* n = head_.load(); n;) { Node* nx = n->next; delete n; n = nx; } }

    void push(T v) {
        Node* n = new Node{std::move(v), head_.load(std::memory_order_relaxed)};
        // release: everything written to *n happens-before any pop that acquires the new head
        while (!head_.compare_exchange_weak(n->next, n, std::memory_order_release, std::memory_order_relaxed)) {}
    }
    bool pop(T& out) {
        Node* n = head_.load(std::memory_order_acquire);
        while (n && !head_.compare_exchange_weak(n, n->next, std::memory_order_acquire, std::memory_order_acquire)) {}
        if (!n) return false;
        out = std::move(n->value);
        while (grave_lock_.exchange(true, std::memory_order_acquire)) {}
        graveyard_.push_back(n);                                   // not freed now: avoids use-after-free in this demo
        grave_lock_.store(false, std::memory_order_release);
        return true;
    }
};

int main() {
    TreiberStack<int> st;
    constexpr int N = 100'000;
    {
        std::vector<std::jthread> ts;
        for (int t = 0; t < 2; ++t) ts.emplace_back([&, t] { for (int i = 0; i < N; ++i) st.push(t * N + i); });
    }
    long sum = 0; int v, count = 0;
    while (st.pop(v)) { sum += v; ++count; }
    std::printf("pushed %d values, popped %d, sum %ld (expected %ld)\n", 2 * N, count, sum, (long)(2 * N) * (2 * N - 1) / 2);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
pushed 200000 values, popped 200000, sum 19999900000 (expected 19999900000)
```

The stack is correct for concurrent *pushes* followed by single-threaded pops; concurrent `pop`s additionally require protection against ABA and reclamation: the whole subject of the next chapter.

</details>

---

## 10. Real-world example

| Where | Atomics in practice |
|---|---|
| **`std::shared_ptr`** | Atomic strong/weak counts with `acq_rel` on the final decrement (Chapter 25) |
| **`std::mutex`, `condition_variable`, `latch`, `semaphore`** | A futex word updated with acquire/release RMWs; `atomic::wait` is the same mechanism exposed |
| **`std::call_once` / magic statics** | An atomic state word with acquire loads on the fast path |
| **Allocators (tcmalloc, jemalloc, mimalloc)** | Per-thread caches; central free lists as CAS-linked stacks; atomics for statistics |
| **Folly / Abseil** | `absl::Mutex` (spin + futex), `folly::MPMCQueue`, `folly::AtomicHashMap`, sharded counters (`ThreadCachedInt`) |
| **Linux kernel** | `atomic_t`, `refcount_t`, per-CPU counters, seqlocks, RCU, `cmpxchg` everywhere |
| **Disruptor, Aeron, LMAX** | SPSC/MPSC rings with padded sequence counters; throughput in the 10⁷–10⁸ msgs/s range |
| **Qt** | `QAtomicInt`/`QAtomicPointer` for `QObject` connection state and implicit-sharing reference counts; `QSemaphore` and `QMutex` have futex-based fast paths (Chapter 48) |
| **Python** | CPython's free-threaded build (3.13+) replaces the GIL with biased reference counting and per-object locks, atomics for hot counters |

> **Opinion.** An atomic is a **sharp tool**, not a performance feature. My ordered checklist: (1) can the data be *thread-local* or *partitioned* so that no atomic is needed? (2) if shared, can it be *immutable after publication* (`shared_ptr<const T>`, one release store)? (3) if it is a counter or flag with **no data attached**, a relaxed atomic is correct and idiomatic; (4) if it **guards other data**, use a mutex until measurement proves otherwise, then `release`/`acquire` on a textbook pattern with the happens-before argument in a comment; (5) CAS loops and lock-free structures only in infrastructure code you'd be willing to maintain with a model checker. And remember that *“lock-free” is a progress property, not a speed claim*: a CAS loop on a contended line is slower than an uncontended mutex. Don't use `atomic<shared_ptr>` where performance matters; don't use spinlocks in application code; and never write your own memory-reclamation scheme when hazard pointers or epochs (Chapter 32) exist.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| `if (a.load() == 0) a.store(1);` (check-then-act) | Two threads both pass: lost atomicity | `compare_exchange_strong(expected, 1)` or `exchange` |
| `a = a + 1;` / `a++` mixed with plain reads of derived state | Fine for the count, but invariants across several atomics are not atomic | One atomic word for the invariant, or a mutex |
| Two related atomics updated separately | Torn invariant seen by a reader (“count” and “sum” disagree) | Pack into one word / struct under CAS, or a lock |
| CAS loop with side effects in the body | Side effect repeated per retry | Compute purely; apply after success |
| Using `compare_exchange_weak` outside a loop | Spurious failure treated as real | Loop, or `strong` |
| Reloading `expected` inside the loop | Extra loads; races with the CAS result | Rely on the refreshed `expected` |
| Spinlock with more threads than cores | Lock-holder preemption; CPU burnt spinning | Sleep (mutex), or `yield` after N spins; don't spin in user code |
| `seq_cst` default left on a hot counter | `lock xadd` and compiler barriers where `relaxed` suffices (no data attached) | `relaxed` for pure counters, *after* confirming nothing is published through it |
| `relaxed` where data is published | Stale data on ARM/after reordering | `release`/`acquire` (Chapter 30) |
| False sharing between adjacent atomics | Scaling collapse | `alignas(64)`; per-thread slots |
| ABA in pointer CAS | Corruption with node reuse | Tagged pointers, hazard pointers, epochs (Chapter 32) |
| `atomic<struct>` > 8 bytes assumed lock-free | Hidden mutex; deadlock in signal handler; `-latomic` link error | Check `is_always_lock_free`; split into 8-byte atomics or lock |
| Non-trivially-copyable `T` in `atomic<T>` | Compile error | Pointers/handles/indices |
| Mixing `atomic_ref` and plain access concurrently | Data race (UB) | All concurrent accessors use `atomic_ref`; separate phases with synchronisation |
| Notify without changing state (`wait/notify`) | Lost wake-up or no effect | Store first, then `notify_*`; waiter re-checks |
| `volatile` as an atomic | No atomicity guarantees | `std::atomic` |
| Busy-wait on `atomic` without `pause`/yield | Hyperthread starvation, power use | `CPU_PAUSE`, backoff, or `atomic::wait` |

---

## 12. Exercises

1. **Lost update.** Write the `load; store` increment, show lost updates with 2 threads, then fix it three ways (`fetch_add`, CAS loop, mutex) and rank by speed.
2. **Fetch-op zoo.** For `fetch_add/sub/and/or/xor` with result used and discarded, predict the x86 instruction, then verify (Experiment 3). Which need a `cmpxchg` loop and why?
3. **False sharing in the ring.** Remove the `alignas(64)` from Experiment 6 and measure the slowdown; put only `head_` and `tail_` on one line, and then explain the result with `perf c2c`.
4. **Ticket vs TTAS fairness.** Measure per-thread acquisition counts under contention: the ticket lock should be fair, TTAS can starve a thread. Print the distribution.
5. **Waiting.** Implement a `Gate` (open/close) with `atomic<bool>::wait` and compare latency and CPU use with a spin loop and with `condition_variable`.
6. **LL/SC vs LSE.** Cross-assemble `fetch_add` and `compare_exchange` for `-march=armv8-a` and `-march=armv8.1-a`; if you have an ARM machine, measure contended `fetch_add` with and without LSE (`-moutline-atomics`).
7. **Scaling.** Run Experiment 7 on a machine with ≥ 8 cores for 1, 2, 4, 8 threads; plot ns/op vs threads for each variant.
8. **`atomic<shared_ptr>` cost.** Benchmark `load()` on `atomic<shared_ptr<T>>` vs a `shared_ptr` guarded by `shared_mutex` vs an `atomic<T*>` with epoch reclamation (preview of Chapter 32) with 1..N reader threads.

---

## 13. Challenge: a bounded MPMC queue

Implement a bounded multi-producer multi-consumer queue (Vyukov's sequence-number design): each cell holds a `std::atomic<size_t> seq` and a payload; producers CAS the `tail` index to claim a cell, write the payload, then publish by storing `seq`; consumers do the mirror image.

- Choose and justify every memory order with an explicit happens-before edge.
- Prove it linearizable (state the linearization point of `push` and `pop`).
- Handle wrap-around of the 64-bit indices and the full/empty conditions without ABA.
- Test with: a stress test checking per-producer FIFO and the multiset equality of pushed/popped values; TSan; a CDSChecker/GenMC model check for 2 producers + 2 consumers + capacity 2.
- Compare throughput and tail latency to a mutex+condvar queue for 1, 2, 4, 8 producers/consumers, and to your SPSC ring when the topology allows.

---

## 14. Knowledge check

1. Why is `x.load(); x.store(old + 1)` not an atomic increment, even though each half is atomic?
2. What is CAS, and what does `compare_exchange_weak` do on failure to its first argument?
3. When may `compare_exchange_weak` fail spuriously, and what does that imply for how you call it?
4. Which `std::atomic<T>` types are lock-free on x86-64 with GCC? What happens if one is not?
5. Why does a shared atomic counter not scale with threads? Name three fixes.
6. What is the difference between lock-free and wait-free? Is your SPSC ring wait-free? Is a CAS-loop stack?
7. How does `atomic::wait` avoid busy-waiting? What must the notifier do?
8. What is `std::atomic_ref` for? State the rule you must obey while an `atomic_ref` exists.
9. Why is test-and-test-and-set better than test-and-set under contention?
10. Why are user-space spinlocks dangerous when threads outnumber cores?
11. Why can `fetch_and` compile to a `cmpxchg` loop on x86 when its result is used?
12. Why is `atomic<shared_ptr<T>>` not suitable for the hottest read paths?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Another thread can modify the value between the load and the store, and that update is overwritten (lost update). The pair is not one indivisible step; `fetch_add` is.
2. CAS atomically compares the atomic's value with `expected` and, if equal, replaces it with `desired`. On failure `compare_exchange_*` writes the current observed value into `expected` (which you therefore reuse in the retry).
3. On LL/SC architectures (ARM, POWER, RISC-V) when the reservation is lost (interrupt, another store to the line) even though the value matched. You must call it in a loop (or use `strong` for one-shot use).
4. Sizes 1, 2, 4 and 8 bytes (and pointers); 16 bytes is reported not lock-free by GCC (goes through `libatomic`), larger sizes never. A non-lock-free atomic uses a hidden lock: slower, not async-signal-safe, may need `-latomic`.
5. The cache line holding it bounces between cores (each RMW needs exclusive ownership). Fixes: shard per thread/core (padded) and sum on read; accumulate locally and flush in batches; partition work so threads don't share.
6. Lock-free: some thread always completes in a finite number of steps. Wait-free: every thread completes in a bounded number of steps. The SPSC ring (one writer per index, no retries) is wait-free; the CAS-loop stack is lock-free but a given thread can starve.
7. It blocks the thread in the kernel (futex) when the value equals `old`. The notifier must change the value (a store/RMW) and then call `notify_one/all`.
8. Atomic access to an ordinary (non-atomic) object. While any `atomic_ref` to the object exists, all concurrent accesses to it must go through `atomic_ref`; plain access is legal only after a synchronising boundary (join, barrier).
9. TAS spins with RMWs, each stealing the cache line exclusively and generating coherence traffic. TTAS spins on a read-only load (line stays shared in each core's cache), and attempts an RMW only when the lock looks free.
10. A thread holding the lock may be descheduled; the others then burn their entire time slices spinning without any chance that the holder runs (lock-holder preemption), wasting CPU and increasing latency; spinlocks also cause priority inversion.
11. x86 has `lock and/or/xor` instructions but they don't return the old value. If the result is used the compiler must read the old value atomically, so it uses a `cmpxchg` retry loop; if unused it emits a single `lock and`.
12. `load()` involves a hidden lock or reference-count RMW (the reader acquires a `shared_ptr` copy, bumping a counter on a shared control block), so readers contend on cache lines. Hazard pointers/epoch/RCU readers can read with no shared writes.

</details>

---

[← Previous: Chapter 30](30-memory-model.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 32 — Lock-free programming →](32-lock-free.md)
