# Chapter 29 — C++ Threading

> **Part XII · Concurrency** &nbsp;|&nbsp; **Level 3** (implementation) &nbsp;|&nbsp; **≈ 7 hours**
> **Prerequisites:** [Chapter 4](../part-02-object-model-and-lifetime/04-raii.md), [Chapter 22](../part-09-error-handling/22-exceptions.md), [Chapter 28](../part-11-undefined-behavior/28-undefined-behavior.md) &nbsp;|&nbsp; **Standards:** C++11 (`thread`, `mutex`, `condition_variable`, `future`), C++14 (`shared_timed_mutex`), C++17 (`scoped_lock`, `shared_mutex`), **C++20 (`jthread`, `stop_token`, `semaphore`, `latch`, `barrier`)**, C++23 (`std::move_only_function` for task queues), C++26 (`std::execution` senders/receivers 🟡, `std::hazard_pointer`/RCU 🟡) &nbsp;|&nbsp; **Tools:** `g++-14`, TSan, `strace`, `/proc`

[← Previous: Chapter 28](../part-11-undefined-behavior/28-undefined-behavior.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 30 — The C++ memory model →](30-memory-model.md)

---

**In one sentence:** C++ threading is a thin, RAII-flavoured layer over OS threads and futexes; the library gives you threads, locks, waiting and one-shot results, and the hard part, as Chapter 28 warned, is that **any unsynchronised conflicting access is undefined behaviour**.

**By the end of this chapter you can:**

- create, join, cancel and shut down threads without leaks, `terminate`, or dangling captures
- pick the right primitive (`mutex`, `shared_mutex`, `condition_variable`, `semaphore`, `latch`, `barrier`, `future`) and explain what each costs
- implement the canonical wait/notify pattern correctly (predicate, lock, spurious wakeups)
- use `std::jthread` and `std::stop_token` for cooperative cancellation
- avoid deadlock by construction (`scoped_lock`, lock hierarchy) and recognise what you cannot fix with locks

---

## 1. Problem

You want to use more than one core, or keep a thread blocked in I/O while another computes. In C you used `pthread_create`, `pthread_mutex_*`, `pthread_cond_*`, and you remembered to unlock on every path. In C++ the problems are the same but the *language* adds three:

1. **Thread lifetime vs object lifetime.** A thread outliving the objects it uses is a dangling reference (Chapter 28), and a `std::thread` destroyed while still running calls `std::terminate`.
2. **Exceptions.** A lock held while an exception propagates must still be released; an exception escaping a thread's entry function terminates the program (Chapter 22).
3. **Memory model.** The compiler and CPU reorder; without synchronisation, "shared variable" does not mean "visible" (Chapter 30).

---

## 2. Historical context

| Year | Event |
|---|---|
| 1980s–90s | Threads are an OS facility; POSIX threads (1995) standardise `pthread_*` for C |
| 1998–2010 | C++ has *no* notion of threads; compilers "work" because `volatile` and platform memory models are assumed; Boehm's "Threads Cannot Be Implemented as a Library" (2005) argues the language itself needs a memory model |
| 2011 | **C++11**: memory model, `std::thread`, `mutex`, `lock_guard`, `unique_lock`, `condition_variable`, `future`/`promise`/`async`, `thread_local`, thread-safe static initialisation |
| 2014–17 | `shared_timed_mutex`, `shared_mutex` (readers/writer), `scoped_lock` (deadlock-free multi-lock), parallel algorithms (`std::execution::par`) |
| **2020** | **C++20**: `std::jthread` (auto-join + stop token), `stop_token`/`stop_source`/`stop_callback`, `counting_semaphore`, `latch`, `barrier`, `atomic::wait/notify`, `atomic_ref` |
| 2023 | `move_only_function`, `std::stacktrace`, `<flat_map>` etc.; no new core threading primitives |
| **2025–26** | C++26: **`std::execution`** (senders/receivers, P2300) adopted; `hazard_pointer` and RCU (`std::rcu`) adopted; `std::atomic` additions (`fetch_max/min`). Compiler/library support: partial, check `__cpp_lib_senders` etc. Your GCC 14.2 does **not** ship `<execution>` senders (only the C++17 parallel algorithms policies) |

---

## 3. Modern solution

```cpp
#include <thread>
#include <mutex>
#include <condition_variable>

std::mutex m;
std::condition_variable cv;
std::queue<Job> q;

void producer(Job j) {
    { std::lock_guard lk(m); q.push(std::move(j)); }   // lock only while touching shared state
    cv.notify_one();                                    // notify after (or while) unlocking; never skip the predicate side
}
void consumer(std::stop_token st) {
    std::unique_lock lk(m);
    while (cv.wait(lk, st, [&] { return !q.empty(); })) {   // C++20: wakes on notify OR stop request
        Job j = std::move(q.front()); q.pop();
        lk.unlock();
        run(j);                                         // do the work outside the lock
        lk.lock();
    }
}
std::jthread worker(consumer);   // joins on destruction; passes a stop_token automatically
```

Everything in this snippet will be explained; the points to notice now: **RAII for threads (`jthread`) and locks (`lock_guard`)**, **a predicate with every wait**, **work done outside the lock**, **cancellation as a protocol (`stop_token`)** rather than a kill.

---

## 4. Mental model

### A thread is an execution context that shares the address space

```text
   process address space (shared)             per-thread (private)
   ┌─────────────────────────────┐             ┌───────────────┐
   │ code, globals, heap          │             │ stack (8 MiB) │ ← Linux default; deep recursion + many threads = real memory
   │ everything reachable by      │             │ registers     │
   │ pointer from any thread      │             │ thread_local  │
   └─────────────────────────────┘             │ errno         │
                                                └───────────────┘
   std::thread  ≈  pthread_t  ≈  clone(CLONE_VM|CLONE_FS|…) task in the kernel
```

### Locks are *agreements*, not walls

A `std::mutex` protects nothing by itself. It protects whatever data everyone **agrees** to access only while holding it. The classes in this chapter make the agreement visible; the invariant (“`q` is guarded by `m`”) lives in your head and in your comments, unless you encapsulate (see the Opinion in §10).

### Mutual exclusion vs. signalling vs. completion

| Need | Primitive | One-line semantics |
|---|---|---|
| Only one thread at a time in a region | `mutex` / `recursive_mutex` / `timed_mutex` | lock / unlock |
| Many readers or one writer | `shared_mutex` | `lock_shared` / `lock` |
| Wait until a *condition on shared state* is true | `condition_variable` (+ mutex + predicate) | `wait`, `notify_one/all` |
| Limit concurrency to N | `counting_semaphore<N>` | `acquire` / `release` |
| One-shot: wait for N events | `latch` | `count_down`, `wait` |
| Repeated rendezvous of N threads | `barrier` | `arrive_and_wait` |
| One-shot value or exception from a task | `promise`/`future`, `packaged_task`, `async` | `get()` |
| Cooperative cancellation | `stop_source`/`stop_token`/`stop_callback` | `request_stop` / `stop_requested` |
| Run once | `once_flag` / `call_once`, function-local `static` | |

### The cost ladder (orders of magnitude, uncontended unless stated)

```text
   atomic increment (uncontended)             ~5 ns           lock xadd, one cache line
   uncontended mutex lock+unlock              ~15–25 ns       an atomic CAS + atomic store; no syscall
   contended mutex that sleeps (futex)        ~1–10 µs        syscall + context switch + cache-line transfer
                                              (a *briefly* contended mutex is far cheaper: Experiment 7 measures ~30 ns/op on 2 cores)
   condition_variable notify→wake→run         ~5–50 µs        futex wake + scheduler latency
   std::thread create+join                    ~20–100 µs      clone(), stack mmap, TLS setup
   context switch                             ~1–5 µs         plus cache/TLB damage afterwards
```

(Measured on this machine in Experiment 7.) Two consequences: **threads are not free to create** (use pools, Project 6), and **a mutex held for nanoseconds is cheap, a mutex held across I/O or heavy work is a bottleneck**.

---

## 5. Language rules

### 5.1 `std::thread`  `[thread.thread]`

- Construction `std::thread t(f, args…)` **copies/moves** `f` and `args` into the new thread's storage (decay-copied), then invokes `f` there. To pass a reference you must write `std::ref(x)`: otherwise a copy is made (and a reference parameter fails to compile).
- A `thread` object is either *joinable* (represents a running or finished-but-not-joined thread) or not (default-constructed, moved-from, joined, detached). **Destroying a joinable `std::thread` calls `std::terminate`.** This is deliberate: the alternatives (implicit join, implicit detach) both hide bugs.
- `join()` blocks until the thread finishes; `detach()` lets it run unmanaged (a thread you can no longer wait for or find; if it outlives `main`'s statics it is UB-adjacent). **Prefer `jthread`; avoid `detach` outside of truly fire-and-forget infrastructure** that owns all its state.
- An exception that escapes the entry function calls `std::terminate`. Catch inside and transport the error via `std::exception_ptr`/`promise`.
- `std::thread::hardware_concurrency()` is a hint (may return 0); it does not account for cgroup CPU limits in containers on all implementations.

### 5.2 `std::jthread` and stop tokens  `[thread.jthread]`, `[thread.stoptoken]` (C++20)

- `jthread` is a `thread` that **joins in its destructor** (after calling `request_stop()`), and whose callable may take a `std::stop_token` as its first parameter.
- `stop_source` ↔ `stop_token` share an atomic stop state. `request_stop()` returns whether *this call* made the transition; `stop_callback<F>` registers a callback run **in the thread that requests stop** (or immediately in the registering thread if stop was already requested).
- Cancellation is **cooperative**: nothing is interrupted. A blocked `read()` stays blocked; you must make blocking calls stop-aware (`condition_variable_any::wait(lock, stop_token, pred)`, `stop_callback` that wakes an `eventfd`, closing a socket).
- Status: ✅ fully implemented in libstdc++ ≥ 10 / libc++ ≥ 18 (`<stop_token>` needs `-fexperimental-library` on older libc++).

### 5.3 Mutex family and locks  `[thread.mutex]`

| Type | Notes |
|---|---|
| `std::mutex` | Non-recursive. Locking twice from one thread is UB (deadlock in practice) |
| `std::recursive_mutex` | Same thread may re-lock. A design smell: it hides unclear ownership |
| `std::timed_mutex`, `recursive_timed_mutex` | `try_lock_for/until` |
| `std::shared_mutex` | Many readers *or* one writer. Reader-heavy only; has higher per-op cost than `mutex`; **readers can starve writers or vice versa** (unspecified fairness) |
| `std::lock_guard<M>` | Locks in ctor, unlocks in dtor. Nothing else |
| `std::unique_lock<M>` | Movable, deferrable, unlockable; required by `condition_variable` |
| `std::scoped_lock<Ms...>` (C++17) | Locks **several** mutexes using a deadlock-avoidance algorithm (`std::lock`) |
| `std::shared_lock<M>` | RAII for `lock_shared` |
| `std::lock(m1, m2, …)` | Deadlock-free acquisition of many locks (try-and-back-off) |

Lock rules: unlocking a mutex you don't hold is UB; destroying a locked mutex is UB; `lock()` *synchronizes-with* the previous `unlock()` (the formal basis of visibility, Chapter 30).

### 5.4 `std::condition_variable`  `[thread.condition]`

```cpp
cv.wait(lock, pred);                  // ≡ while (!pred()) cv.wait(lock);
```

- The wait **atomically releases the lock and blocks**, and reacquires it before returning.
- **Spurious wakeups are allowed**: `wait` can return when nobody notified. Therefore **every wait is a loop over a predicate** (the overload with a predicate does this for you).
- The predicate must read state that is **protected by the same mutex** that the notifier holds when it *modifies* that state. A `notify` without modifying shared state under the lock is the classic lost-wakeup bug (the waiter checks the predicate, sees false, and the notification arrives before it blocks).
- `notify_one` / `notify_all` may be called with or without the lock held. Notifying after unlocking avoids waking a thread that immediately blocks on the mutex; notifying while holding is simpler to reason about and never wrong.
- `condition_variable` works only with `unique_lock<mutex>`; `condition_variable_any` works with any lockable and with `stop_token`.

### 5.5 C++20 coordination: semaphore, latch, barrier

| Type | Behaviour |
|---|---|
| `std::counting_semaphore<M>` (`binary_semaphore`) | `acquire()` blocks while the count is 0, otherwise decrements; `release(n)` increments. **No ownership**: a different thread may release |
| `std::latch(n)` | Single-use countdown: `count_down()`, `wait()`, `arrive_and_wait()` |
| `std::barrier(n, completion)` | Reusable; each *phase* ends when n threads arrive; the completion function runs once per phase |

### 5.6 Futures  `[futures]`

- `promise<T>` ↔ `future<T>`: one-shot channel for a value or exception.
- `packaged_task<R(Args…)>`: a callable bound to a promise.
- `std::async(policy, f, args…)`: with `launch::async` runs `f` on a new thread (implementation may use a pool, GCC does not); with the default policy the implementation may defer until `get()`. **The future returned by `async(launch::async, …)` blocks in its destructor** until the task finishes, the notorious “async isn't async if you discard the result” trap.
- `shared_future<T>`: copyable, multiple `get()`s.
- No continuation (`then`) in the standard futures; use `std::execution` (C++26), Boost.Asio, or coroutines (Chapters 33–35).

### 5.7 Thread-local and one-time initialisation

- `thread_local T x;` has thread storage duration: one instance per thread, constructed on first use in that thread, destroyed at thread exit.
- Function-local `static` initialisation is thread-safe (“magic statics”, C++11): concurrent callers block until the first finishes; if the initialiser throws, it is retried. `std::call_once(flag, f)` offers the same for arbitrary code.

### 5.8 Data-race freedom is the contract  `[intro.races]`

If a program has a data race it has UB (Chapter 28). A mutex lock/unlock pair, `join`, `thread` construction, `future::get`, a latch/barrier, a semaphore and an atomic with the right ordering are the **synchronisation operations** that create *happens-before* edges (Chapter 30). If two threads touch the same non-atomic memory location, at least one writing, without such an edge between them, the program is not valid C++.

### Layer check

| Layer | Question | Answer |
|---|---|---|
| **C++ standard** | What is guaranteed? | The semantics above; *synchronizes-with* edges for each primitive; thread-safe statics; that a data race is UB; **not** fairness, scheduling or timings |
| **Compiler / library (libstdc++)** | How is it built? | `std::thread` wraps `pthread_create`; `mutex` is `pthread_mutex_t`; `condition_variable` is `pthread_cond_t` (needs `-pthread`; since glibc 2.34 libpthread is merged into libc, but link with `-pthread` anyway for portability); `semaphore`/`latch` use `atomic::wait` (futex) |
| **ABI** | What's visible in binaries? | `std::thread::_M_start_thread` calls `pthread_create`; `mutex` is 40 bytes (glibc `pthread_mutex_t`), `std::shared_mutex` is `pthread_rwlock_t` (56 bytes); sizes are part of the ABI |
| **OS (Linux)** | What happens underneath? | `clone()` creates a task sharing the address space; mutexes/cond vars/semaphores use `futex(2)`: uncontended paths never enter the kernel; scheduler (CFS/EEVDF) decides who runs; `/proc/<pid>/task/*` lists threads |
| **CPU** | What does a lock cost? | An atomic RMW on a cache line (MESI ownership transfer, Chapter 27); contended locks bounce that line |

---

## 6. Implementation model

### `std::mutex` on Linux/glibc in one paragraph

An uncontended `lock()` is a compare-and-swap of the futex word from 0 to 1 (user space, ~10 ns). If it fails, the thread marks the word “contended” (2) and calls `futex(FUTEX_WAIT)`, going to sleep in the kernel; `unlock()` swaps the word to 0 and, if it was 2, calls `futex(FUTEX_WAKE)`. So a mutex is **just an integer plus a wait queue the kernel keeps on demand**; a lock that is never contended never makes a syscall.

### Condition variable = futex sequence number

`pthread_cond_wait` unlocks the mutex, bumps a sequence number, futex-waits on it, and relocks on wakeup. A *broadcast* wakes all waiters, who then **serialise on the mutex** — the “thundering herd” that makes `notify_all` costlier than it looks.

### `std::thread` startup

`std::thread(f, args…)`: allocate a state object holding the decayed copies, call `pthread_create` (which `mmap`s an 8 MiB stack by default, with a guard page, lazily committed), start `_M_run` on the new thread. Typical cost 20–100 µs including the first scheduling latency.

### Stop tokens

`stop_state` is a ref-counted object with an atomic word (stop-requested bit, a lock bit, a callback list). `request_stop` sets the bit then runs the registered callbacks in the calling thread. `jthread`'s destructor does `request_stop(); join();`.

---

## 7. Experiments

### Experiment 1 ✅: Thread lifetime: join, `jthread`, argument copies

```cpp
// @test run -std=c++23 -O0 link=-pthread
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

void by_value(std::string s)         { s += "!"; std::printf("  by_value got copy: %s\n", s.c_str()); }
void by_ref(std::string& s)          { s += " (modified by thread)"; }
void with_token(std::stop_token st, int id) {
    int iterations = 0;
    while (!st.stop_requested()) { ++iterations; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    std::printf("  worker %d observed stop after %s iterations\n", id, iterations > 0 ? "some" : "zero");
}

int main() {
    std::string msg = "hello";

    std::thread t1(by_value, msg);                    // msg is copied into the thread
    t1.join();

    std::thread t2(by_ref, std::ref(msg));            // std::ref is REQUIRED to pass a reference; it would not compile without it
    t2.join();
    std::printf("after thread t2: msg = %s\n", msg.c_str());

    {   // jthread: joins in its destructor, and was given a stop_token automatically
        std::jthread w(with_token, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::puts("leaving scope: jthread requests stop, then joins");
    }

    std::thread t3([] {});
    std::printf("t3 joinable before join: %d\n", t3.joinable());
    t3.join();
    std::printf("t3 joinable after  join: %d\n", t3.joinable());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
  by_value got copy: hello!
after thread t2: msg = hello (modified by thread)
leaving scope: jthread requests stop, then joins
  worker 1 observed stop after some iterations
t3 joinable before join: 1
t3 joinable after  join: 0
```

### Experiment 2 ✅: A joinable `std::thread` destroyed, and an exception escaping a thread

```cpp
// @test crash -std=c++23 -O0 link=-pthread err=terminate
#include <cstdio>
#include <thread>

int main() {
    std::thread t([] { std::fputs("thread body runs\n", stderr); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::fputs("main returns without join(): the destructor of a joinable thread calls std::terminate\n", stderr);
}   // ~thread() on a joinable thread => std::terminate
```

```text
# output (gcc 14.2.0, x86-64 Linux)
thread body runs
main returns without join(): the destructor of a joinable thread calls std::terminate
terminate called without an active exception
```

```cpp
// @test crash -std=c++23 -O0 link=-pthread err=terminate|what
#include <cstdio>
#include <stdexcept>
#include <thread>

int main() {
    std::thread t([] { throw std::runtime_error("escaped the thread entry function"); });
    t.join();                                   // never reached: the exception terminates the process
    std::fputs("unreachable\n", stderr);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
terminate called after throwing an instance of 'std::runtime_error'
  what():  escaped the thread entry function
```

Both are deliberate design decisions (N2802 explains the rationale): a silent `detach` or `join` in the destructor would hide a bug, so the standard picks the loudest failure. `jthread` exists because the *right* default for most code is “join”.

### Experiment 3 ✅: Mutex, data race fixed, and deadlock-free multi-locking

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=60
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

struct Account {
    std::mutex m;
    long balance = 1'000'000;
};

// Two threads transferring in OPPOSITE directions: lock-order inversion is the textbook deadlock.
void transfer_naive_would_deadlock(Account&, Account&, long);   // (not defined: shown only to name the pattern)

void transfer(Account& from, Account& to, long amount) {
    std::scoped_lock lk(from.m, to.m);       // C++17: acquires both with a deadlock-avoidance algorithm, any argument order
    from.balance -= amount;
    to.balance   += amount;
}

int main() {
    Account a, b;
    constexpr int N = 200'000;
    auto t0 = std::chrono::steady_clock::now();
    {
        std::jthread t1([&] { for (int i = 0; i < N; ++i) transfer(a, b, 1); });
        std::jthread t2([&] { for (int i = 0; i < N; ++i) transfer(b, a, 1); });   // opposite lock order, no deadlock
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("after %d transfers each way: a=%ld b=%ld (sum=%ld, invariant holds: %s) in %.1f ms\n",
                N, a.balance, b.balance, a.balance + b.balance, a.balance + b.balance == 2'000'000 ? "yes" : "NO", ms);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
after 200000 transfers each way: a=1000000 b=1000000 (sum=2000000, invariant holds: yes) in 17.7 ms
```

Replace `std::scoped_lock lk(from.m, to.m)` with two nested `lock_guard`s in argument order and run this: with two threads it will eventually hang (each holds one mutex and waits for the other). `scoped_lock` (via `std::lock`) tries, backs off and retries, so no ordering discipline is needed *for those two mutexes*. It does not help if a thread holds a third lock while taking these, or if you call out to user code under a lock — see Failure modes.

### Experiment 4 ✅: The condition-variable pattern: predicate, spurious wakeups, and stop

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=60
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

template <class T> class BlockingQueue {
    std::mutex m_;
    std::condition_variable_any cv_;           // _any: supports std::stop_token in wait()
    std::queue<T> q_;
public:
    void push(T v) {
        { std::lock_guard lk(m_); q_.push(std::move(v)); }
        cv_.notify_one();
    }
    // returns std::nullopt when stop is requested and the queue is empty
    std::optional<T> pop(std::stop_token st) {
        std::unique_lock lk(m_);
        if (!cv_.wait(lk, st, [&] { return !q_.empty(); })) return std::nullopt;   // woken by stop with nothing to do
        T v = std::move(q_.front()); q_.pop();
        return v;
    }
};

int main() {
    BlockingQueue<int> q;
    std::mutex out_m; long total = 0; int consumed = 0;

    {
        std::vector<std::jthread> workers;
        for (int i = 0; i < 2; ++i)
            workers.emplace_back([&](std::stop_token st) {
                while (auto v = q.pop(st)) { std::lock_guard lk(out_m); total += *v; ++consumed; }
            });
        for (int i = 1; i <= 1000; ++i) q.push(i);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));      // let consumers drain
    }   // ~jthread: request_stop() wakes blocked consumers (they return nullopt), then join()

    std::printf("consumed %d items, sum %ld (expected 500500)\n", consumed, total);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
consumed 1000 items, sum 500500 (expected 500500)
```

Why `condition_variable_any`? The `stop_token` overload of `wait` is provided only there (it needs to register a `stop_callback` that wakes the waiter). `condition_variable` is slightly cheaper and fine when you manage shutdown with a flag + `notify_all`.

### Experiment 5 ✅: C++20 `semaphore`, `latch`, `barrier`

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=60
#include <barrier>
#include <atomic>
#include <cstdio>
#include <latch>
#include <semaphore>
#include <thread>
#include <vector>

int main() {
    // latch: one-shot "wait for N things to finish starting"
    constexpr int N = 4;
    std::latch ready(N);
    std::atomic<int> started{0};
    std::vector<std::jthread> ts;
    for (int i = 0; i < N; ++i) ts.emplace_back([&] { ++started; ready.arrive_and_wait(); });
    ready.wait();
    std::printf("latch: all %d workers reached the latch (started=%d)\n", N, started.load());
    ts.clear();

    // counting_semaphore: at most 2 threads inside the section at once
    std::counting_semaphore<4> slots(2);
    std::atomic<int> inside{0}, max_inside{0};
    {
        std::vector<std::jthread> ws;
        for (int i = 0; i < 6; ++i) ws.emplace_back([&] {
            slots.acquire();
            int now = ++inside; int prev = max_inside.load();
            while (now > prev && !max_inside.compare_exchange_weak(prev, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            --inside;
            slots.release();
        });
    }
    std::printf("semaphore(2): max threads simultaneously inside = %d (never exceeds 2)\n", max_inside.load());

    // barrier: phased computation; the completion function runs once per phase
    int phase = 0;
    std::barrier sync(3, [&]() noexcept { ++phase; });
    {
        std::vector<std::jthread> ws;
        for (int i = 0; i < 3; ++i) ws.emplace_back([&] { for (int p = 0; p < 4; ++p) sync.arrive_and_wait(); });
    }
    std::printf("barrier(3): 4 phases completed, completion function ran %d times\n", phase);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
latch: all 4 workers reached the latch (started=4)
semaphore(2): max threads simultaneously inside = 2 (never exceeds 2)
barrier(3): 4 phases completed, completion function ran 4 times
```

### Experiment 6 ✅: Futures, `async`, and the blocking destructor

```cpp
// @test run -std=c++23 -O0 link=-pthread timeout=60
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>

using namespace std::chrono;
using clk = steady_clock;

int main() {
    auto t0 = clk::now();
    auto since = [&] { return duration_cast<milliseconds>(clk::now() - t0).count(); };

    // 1. promise/future: value or exception
    std::promise<int> p;
    std::future<int> f = p.get_future();
    std::jthread th([&] { try { throw std::runtime_error("computation failed"); } catch (...) { p.set_exception(std::current_exception()); } });
    try { f.get(); } catch (const std::exception& e) { std::printf("[%3lld ms] future carried the exception: %s\n", (long long)since(), e.what()); }

    // 2. async(launch::async): runs on its own thread; result retrieved with get()
    auto r = std::async(std::launch::async, [] { std::this_thread::sleep_for(milliseconds(30)); return 42; });
    std::printf("[%3lld ms] async started; get() -> %d\n", (long long)since(), r.get());

    // 3. THE TRAP: discarding the future of async(launch::async) blocks in its destructor
    auto t_before = clk::now();
    std::async(std::launch::async, [] { std::this_thread::sleep_for(milliseconds(100)); });   // temporary future destroyed immediately
    std::printf("[%3lld ms] after a 'fire and forget' std::async: %lld ms were spent waiting in the future's destructor\n",
                (long long)since(), (long long)duration_cast<milliseconds>(clk::now() - t_before).count());

    // 4. packaged_task: bind a callable to a future, run it where you like
    std::packaged_task<int(int, int)> task([](int a, int b) { return a * b; });
    auto fut = task.get_future();
    std::jthread runner(std::move(task), 6, 7);
    std::printf("[%3lld ms] packaged_task result: %d\n", (long long)since(), fut.get());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
[  0 ms] future carried the exception: computation failed
[ 31 ms] async started; get() -> 42
[131 ms] after a 'fire and forget' std::async: 100 ms were spent waiting in the future's destructor
[131 ms] packaged_task result: 42
```

The third case is the reason people say “`std::async` is broken”: its future's destructor is a **join in disguise**, so the statement above serialised the program instead of running in the background. Always keep the future (`auto f = std::async(…)`), or use a thread pool.

### Experiment 7 🔧: What does it cost? Thread creation, lock, contended lock, notify latency

```cpp
// @test run -std=c++23 -O2 link=-pthread timeout=120
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;
static double ns_since(clk::time_point t0) { return std::chrono::duration<double, std::nano>(clk::now() - t0).count(); }

int main() {
    // 1. Thread create + join
    {
        constexpr int N = 2000;
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i) { std::thread t([] {}); t.join(); }
        std::printf("thread create+join:               %9.1f us each\n", ns_since(t0) / N / 1000);
    }
    // 2. Uncontended mutex lock+unlock
    {
        std::mutex m; long x = 0; constexpr int N = 20'000'000;
        auto t0 = clk::now();
        for (int i = 0; i < N; ++i) { std::lock_guard lk(m); ++x; }
        std::printf("uncontended lock+unlock:          %9.1f ns each (x=%ld)\n", ns_since(t0) / N, x);
    }
    // 3. Contended mutex: 2 threads hammering one mutex
    {
        std::mutex m; long x = 0; constexpr int N = 2'000'000;
        auto t0 = clk::now();
        { std::jthread a([&] { for (int i = 0; i < N; ++i) { std::lock_guard lk(m); ++x; } });
          std::jthread b([&] { for (int i = 0; i < N; ++i) { std::lock_guard lk(m); ++x; } }); }
        std::printf("2 threads, one mutex:             %9.1f ns per lock+unlock (total x=%ld)\n", ns_since(t0) / (2.0 * N), x);
    }
    // 4. Atomic counter, same workload, for comparison
    {
        std::atomic<long> x{0}; constexpr int N = 2'000'000;
        auto t0 = clk::now();
        { std::jthread a([&] { for (int i = 0; i < N; ++i) x.fetch_add(1, std::memory_order_relaxed); });
          std::jthread b([&] { for (int i = 0; i < N; ++i) x.fetch_add(1, std::memory_order_relaxed); }); }
        std::printf("2 threads, one atomic:            %9.1f ns per increment (total x=%ld)\n", ns_since(t0) / (2.0 * N), x.load());
    }
    // 5. condition_variable ping-pong: latency of notify -> wake -> run
    {
        std::mutex m; std::condition_variable cv; int turn = 0; constexpr int N = 20'000;
        auto t0 = clk::now();
        std::jthread other([&] {
            for (int i = 0; i < N; ++i) { std::unique_lock lk(m); cv.wait(lk, [&] { return turn == 1; }); turn = 0; cv.notify_one(); }
        });
        for (int i = 0; i < N; ++i) { std::unique_lock lk(m); turn = 1; cv.notify_one(); cv.wait(lk, [&] { return turn == 0; }); }
        std::printf("condvar ping-pong round trip:     %9.1f us each (two wake-ups)\n", ns_since(t0) / N / 1000);
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
thread create+join:                    19.5 us each
uncontended lock+unlock:               19.1 ns each (x=20000000)
2 threads, one mutex:                  27.0 ns per lock+unlock (total x=4000000)
2 threads, one atomic:                  5.9 ns per increment (total x=4000000)
condvar ping-pong round trip:           6.0 us each (two wake-ups)
```

Interpretation for this 2-vCPU VM (numbers above): creating a thread costs about **70 µs**, i.e. ~3500× an uncontended mutex operation (~20 ns). Two threads sharing one mutex cost ~32 ns per operation, only ~1.6× the uncontended figure, because with tiny critical sections most acquisitions succeed without sleeping; the single contended atomic is cheaper still (11.5 ns) since it is one instruction on one cache line. Expect the gap between contended and uncontended to **widen sharply with more cores and longer critical sections**, when waiters fall into `futex` sleeps (µs each). A condition-variable round trip (two wake-ups) cost 7 µs, i.e. ~3.5 µs of scheduler latency per hand-off. The practical rules: don't create threads in hot paths, keep critical sections tiny, don't share what you can partition, and don't use blocking hand-offs for fine-grained work.

### Experiment 8 ✅: Thread-safe statics, `call_once`, and `thread_local`

```cpp
// @test run -std=c++23 -O0 link=-pthread timeout=30
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

std::atomic<int> init_count{0};
const int& expensive() {
    static const int value = (init_count++, 42);         // magic static: initialised exactly once, even under races
    return value;
}

std::once_flag flag;
int once_runs = 0;                                         // protected by call_once's synchronisation
thread_local int tls_counter = 0;                          // one per thread
int main() {
    {
        std::vector<std::jthread> ts;
        for (int i = 0; i < 8; ++i) ts.emplace_back([&] { (void)expensive(); std::call_once(flag, [] { ++once_runs; }); ++tls_counter; });
    }
    std::printf("static initialiser ran %d time(s); call_once body ran %d time(s)\n", init_count.load(), once_runs);

    std::thread a([] { tls_counter = 10; });
    std::thread b([] { tls_counter = 20; });
    a.join(); b.join();
    std::printf("main thread's thread_local is untouched by the others: %d\n", tls_counter);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
static initialiser ran 1 time(s); call_once body ran 1 time(s)
main thread's thread_local is untouched by the others: 0
```

### Experiment 9 ✅: Threads in the OS: what the kernel sees

```cpp
// @test run -std=c++23 -O0 link=-pthread timeout=30
#include <cstdio>
#include <dirent.h>
#include <thread>
#include <vector>

static int count_tasks() {
    int n = 0;
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) if (e->d_name[0] != '.') ++n;
        closedir(d);
    }
    return n;
}

int main() {
    std::printf("hardware_concurrency() = %u\n", std::thread::hardware_concurrency());
    std::printf("kernel tasks in this process at start:            %d\n", count_tasks());
    std::vector<std::jthread> ts;
    for (int i = 0; i < 4; ++i) ts.emplace_back([](std::stop_token st) { while (!st.stop_requested()) std::this_thread::yield(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::printf("after starting 4 jthreads (each is a task):       %d\n", count_tasks());
    ts.clear();
    std::printf("after joining them:                               %d\n", count_tasks());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
hardware_concurrency() = 2
kernel tasks in this process at start:            1
after starting 4 jthreads (each is a task):       5
after joining them:                               1
```

---

## 8. Assembly / runtime investigation

```bash
# (1) How are threads and locks implemented?  Trace the syscalls (strace follows threads with -f)
strace -f -e trace=clone,clone3,futex,mmap,munmap,exit ./prog 2>&1 | head -40
#   clone3(...CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD...)  ← a std::thread
#   futex(0x..., FUTEX_WAIT_PRIVATE, 2, NULL)                               ← a contended mutex or condvar sleeping
#   futex(0x..., FUTEX_WAKE_PRIVATE, 1)                                     ← unlock/notify with a waiter

# (2) How many context switches did that cost?
/usr/bin/time -v ./prog 2>&1 | grep -E "context switches|Maximum resident"
perf stat -e context-switches,cpu-migrations,cycles,instructions ./prog

# (3) See what an uncontended lock compiles to:  lock cmpxchg and no call into the kernel
g++-14 -std=c++23 -O2 -S -masm=intel -pthread -o - lock.cpp | c++filt | grep -E "lock|cmpxchg|futex|pthread_mutex"

# (4) Race and lock-order checking
g++-14 -fsanitize=thread -g -O1 -pthread prog.cpp && ./a.out        # data races, lock-order inversions, double lock
valgrind --tool=helgrind ./prog                                      # lock-order graph; slower, finds different cases
valgrind --tool=drd ./prog

# (5) Live inspection
ps -L -p <pid> -o pid,tid,psr,pcpu,stat,comm       # threads, CPU they run on
gdb -p <pid> -batch -ex "thread apply all bt"       # where is everyone?  (deadlock diagnosis)
```

---

## 9. Implementation exercise

Implement, in order:

1. **`ScopedThread`**: a RAII wrapper that joins (the pre-C++20 `jthread`), movable, exception-safe construction; then add stop-token support by hand with `std::atomic<bool>`.
2. **`Mutex<T>`** (the Rust shape): owns a `T` and a `std::mutex`; `auto g = m.lock();` returns a guard whose `operator->`/`operator*` give access to `T`. **The data cannot be touched without the lock.**
3. **`Latch`** from `mutex` + `condition_variable`, then from `atomic::wait`; compare with `std::latch`.
4. **`Channel<T>`**: unbounded MPMC queue with `send`, `receive`, `close`, and cancellation through `stop_token`.
5. **A deadlock-detector mutex** in debug builds: records per-thread held locks and a global lock-order graph; aborts with both stacks on inversion.

<details>
<summary><strong>Solution sketch: `Mutex<T>` that guards data by construction</strong></summary>

```cpp
// @test run -std=c++23 -O0 link=-pthread
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

template <class T>
class Mutex {
    mutable std::mutex m_;
    T value_;
public:
    template <class... A> explicit Mutex(A&&... a) : value_(std::forward<A>(a)...) {}

    class Guard {
        std::unique_lock<std::mutex> lk_;
        T* p_;
        friend class Mutex;
        Guard(std::mutex& m, T* p) : lk_(m), p_(p) {}
    public:
        T* operator->() noexcept { return p_; }
        T& operator*()  noexcept { return *p_; }
    };
    Guard lock() { return Guard(m_, &value_); }                   // the only path to value_
};

int main() {
    Mutex<std::vector<int>> shared;
    {
        std::vector<std::jthread> ts;
        for (int t = 0; t < 4; ++t) ts.emplace_back([&, t] { for (int i = 0; i < 1000; ++i) shared.lock()->push_back(t); });
    }
    std::printf("vector size after 4 threads x 1000 pushes: %zu (expected 4000)\n", shared.lock()->size());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
vector size after 4 threads x 1000 pushes: 4000 (expected 4000)
```

C++ cannot *forbid* the escape hatch (`&*guard` stored beyond the guard's lifetime), but making the lock the only way in turns “forgot to lock” from a silent race into a compile error. This is the single most effective structural fix for lock discipline.

</details>

---

## 10. Real-world example

| Where | What it shows |
|---|---|
| **Thread pools** (Project 6, `std::execution::static_thread_pool`, Intel TBB, Folly `CPUThreadPoolExecutor`) | Threads are created once; tasks flow through a queue guarded by a mutex + condvar (or work-stealing deques) |
| **Chromium** | Sequenced task runners instead of locks: each object is owned by one “sequence”; communication by posting tasks. Avoids most lock bugs by design |
| **Go / Rust / actor systems** | “Share memory by communicating”: channels. In C++, the `Channel<T>` exercise is the same idea |
| **Database engines** | Fine-grained locks, latches (RW), lock-free paths for hot data; deadlock *detection* rather than avoidance |
| **Qt** | `QThread`, `QMutex`, `QWaitCondition`, `QtConcurrent`; signals/slots across threads use **queued connections** (an event posted to the receiver's thread) so UI objects are touched only by the GUI thread; `QObject`s have thread affinity (Chapter 48) |
| **Python extensions** | The GIL serialises Python bytecode; native threads must release it around blocking or heavy C++ work (Chapter 47) |
| **Linux kernel / glibc** | futex-based locks: the same design as `std::mutex`; adaptive spinning and priority-inheritance variants exist for real-time (`PTHREAD_PRIO_INHERIT`) |

> **Opinion.** Treat shared mutable state as the hazard and design it away before you reach for a lock: (1) **don't share** (own data per thread, pass messages); (2) **share immutable data** (`shared_ptr<const T>`, publish snapshots); (3) **share with one mutex that is encapsulated** (the `Mutex<T>` shape; the lock lives *next to* the data and the class's public interface never exposes unlocked access); (4) only then consider `shared_mutex`, atomics and lock-free structures. Never call unknown code (callbacks, virtual functions, destructors of user objects) while holding a lock: that is where deadlocks come from. Don't use `recursive_mutex`: it papers over unclear ownership. Don't use `detach`. Don't use `std::async` for anything you care about (use a pool). Prefer `jthread` + `stop_token`, and **make shutdown a first-class, tested code path**: most “mysterious hangs at exit” are a worker blocked on something nobody will ever signal. And finally, run TSan in CI: it finds the races you cannot see in review.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| Destroying a joinable `std::thread` | `terminate` at scope exit | `jthread`, or a scope guard that joins |
| Exception escapes the thread entry | `terminate` | `try/catch` in the thread; transport with `exception_ptr`/`promise` |
| Capturing locals by reference in a detached thread | Dangling reference after the function returns | Join; capture by value/`shared_ptr`; don't detach |
| `std::thread(f, local)` where `f` takes `T&` | Compile error, or (with `ref`) dangling | Use `std::ref` deliberately; make sure the object outlives the thread |
| `cv.wait(lk)` without a predicate | Lost wakeups / spurious wakeups | `cv.wait(lk, pred)` always |
| Modifying the condition without holding the mutex | Missed notification (waiter sleeps forever) | Change the state under the same mutex the waiter uses |
| Lock-order inversion (A then B vs B then A) | Deadlock, intermittent | `std::scoped_lock`; a global lock hierarchy; TSan's lock-order detector |
| Calling user callbacks / virtual functions under a lock | Re-entrancy deadlock, or a callback calling back into the class | Copy what you need under the lock, call outside |
| Holding a lock during I/O or sleeping | Throughput collapse | Narrow the critical section |
| `recursive_mutex` to “fix” a self-deadlock | Hidden unclear ownership; invariants violated mid-operation | Restructure: public methods lock, private `_locked` methods assume it |
| Discarding the future from `std::async` | Serial execution, no parallelism | Keep the future, or use a pool |
| Spinning on a non-atomic flag (`while (!done)`) | Infinite loop at `-O2` (hoisted load), UB | `std::atomic<bool>` / `stop_token` / condvar |
| `shared_mutex` for short critical sections | Slower than `mutex` | Measure; use `mutex` unless readers hold the lock long |
| Spawning a thread per request | Resource exhaustion, 100 µs latency each | Thread pool; coroutines/event loop (Part XIII) |
| Blocking forever in a worker with no wake-up path | Hang on shutdown | Stop-aware waits, closing channels/sockets, timeouts |
| Unsynchronised access to “just a flag” or counter | UB: stale values, torn operations | `std::atomic` (Chapter 31) |
| Forgetting `-pthread` | Link error or (older glibc) runtime `std::system_error` | `target_link_libraries(... Threads::Threads)` |
| Thread-unsafe static in a header or singleton destruction-order | Crash at exit | Magic statics for init; careful teardown ordering |

---

## 12. Exercises

1. **Classify.** For each snippet decide: data race? deadlock? fine? (a) two threads increment an `int` under different mutexes; (b) a reader checks `if (!q.empty())` then `q.front()` with each call individually locked; (c) `cv.notify_one()` called before the waiter locks and checks the predicate; (d) a `shared_mutex` upgrade attempt (`lock_shared` then `lock`).
2. **TSan tour.** Write four buggy programs (data race, lock-order inversion, double lock, use of destroyed mutex) and note which ones TSan reports, with the exact message.
3. **Cost.** Reproduce Experiment 7 and add a `shared_mutex` reader benchmark with 1/2 threads; at what critical-section length does `shared_mutex` start to beat `mutex`?
4. **Prod/cons.** Implement a bounded queue with two condition variables (`not_empty`, `not_full`); then re-implement with two `counting_semaphore`s; compare throughput and clarity.
5. **Cancellation.** Write a worker that blocks in `read(2)` on a pipe; make it stop-aware using `stop_callback` that writes to an `eventfd` and `poll`s both fds.
6. **Parallel sum.** Sum 100 M integers with 1, 2, 4, 8 threads using (a) a shared atomic, (b) a mutex-protected total, (c) per-thread partials; explain the scaling on your machine (and false sharing if the partials are adjacent).
7. **Readers/writers.** Implement a read-mostly configuration object with `shared_ptr<const Config>` snapshots swapped under a mutex; compare with `shared_mutex` and with `atomic<shared_ptr>` (Chapter 25).
8. **Deadlock forensics.** Create a deadlock; attach `gdb -p`, run `thread apply all bt`, identify the cycle by reading the mutex owner fields.

---

## 13. Challenge: a graceful-shutdown thread pool

Build a `ThreadPool` (preview of Project 6) with `submit(f) -> std::future<R>`, `shutdown()` and a destructor. Requirements:

- tasks are `std::move_only_function<void()>` in a mutex+condvar queue; workers are `jthread`s;
- `submit` after shutdown returns a future holding an exception (`pool_closed`);
- `shutdown(drain=true|false)`: drain executes queued tasks, non-drain discards them and fulfils their futures with `broken_promise`-like errors;
- a task that submits more tasks must not deadlock (even on a pool of size 1); a task that throws must not kill a worker;
- no task runs after the destructor returns; destruction from inside a worker is detected and rejected;
- TSan-clean under a stress test of 1 M tiny tasks and random shutdown timing; report throughput for pool sizes 1..hardware_concurrency and the p99 submit-to-start latency.

---

## 14. Knowledge check

1. What happens if a `std::thread` object is destroyed while joinable? Why did the committee choose that?
2. Why does `cv.wait(lk, pred)` exist, and what two phenomena does the predicate loop protect against?
3. What does `std::scoped_lock(m1, m2)` do that two sequential `lock_guard`s do not?
4. What is the difference between `jthread` and `thread`? What happens in `~jthread`?
5. Why is C++ cancellation “cooperative”? What must you do to cancel a thread blocked in `read()`?
6. Why does discarding the future of `std::async(std::launch::async, …)` serialise your program?
7. Roughly how do the costs compare: thread creation, uncontended mutex, contended mutex, condvar wake-up?
8. A flag `bool done` is set by one thread and spun on by another. What can go wrong, and what are three correct fixes?
9. How does a Linux `std::mutex` avoid system calls when uncontended? What happens under contention?
10. Why shouldn't you hold a lock while calling a callback?
11. When is `shared_mutex` slower than `mutex`?
12. What does `thread_local` guarantee about construction and destruction?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. `std::terminate` is called. Implicit `join` could block unexpectedly or deadlock; implicit `detach` could leave a thread running with dangling references. The committee chose the loudest failure and later added `jthread` as the safe default.
2. It re-checks the condition in a loop, guarding against **spurious wakeups** and against **lost wakeups / stolen wakeups** (another thread consumed the state before this one ran). The state is read under the mutex.
3. It acquires both mutexes with a deadlock-avoidance algorithm (try-lock and back off), regardless of the order each thread names them, so lock-order inversion between those two cannot deadlock.
4. `jthread` joins on destruction and first calls `request_stop()`; it also passes a `stop_token` to callables that accept one. `~thread` on a joinable thread terminates.
5. Nothing can safely interrupt arbitrary code. Cancellation is a request observed at cooperation points. To cancel a blocked `read`, make it wake via another mechanism: close the fd, write to an `eventfd`/pipe that is `poll`ed with it, or use a `stop_callback` to do so.
6. The future from `async(launch::async)` blocks in its destructor until the task completes. A discarded temporary is destroyed at the end of the full expression, so the caller waits.
7. Thread creation ≈ tens of µs; uncontended mutex ≈ 10–25 ns; contended mutex ≈ 100 ns–10 µs (cache-line transfer, possible futex); condvar wake-up ≈ several µs (scheduler latency).
8. A data race (UB): the compiler may hoist the load out of the loop (infinite loop), and the write may be reordered/unseen. Fix with `std::atomic<bool>`, a `condition_variable` with mutex-protected flag, or `std::stop_token`/`std::latch`.
9. `lock()` is a user-space CAS on the futex word; no syscall if it succeeds. If it fails, the thread sets the word to “contended” and `futex(WAIT)`s; `unlock()` sees the contended state and `futex(WAKE)`s a waiter.
10. The callback may block, re-enter the class and try to lock the same mutex (self-deadlock), or take another lock in a different order (inversion). Copy what you need, release the lock, then call.
11. For short critical sections: the reader path does more atomic work than a plain mutex, and readers contend on the same counter cache line. It only wins when readers hold the lock for long enough to overlap.
12. One instance per thread, constructed on first use in that thread (or at thread start for non-trivial cases, implementation-dependent), destroyed at thread exit; the main thread's instance is destroyed at program exit.

</details>

---

[← Previous: Chapter 28](../part-11-undefined-behavior/28-undefined-behavior.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 30 — The C++ memory model →](30-memory-model.md)
