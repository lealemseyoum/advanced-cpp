# Project 6 — Thread Pool

[← Project 5](project-05-memory-arena.md) &nbsp;|&nbsp; [Projects overview](README.md) &nbsp;|&nbsp; [Next: Project 7 — Concurrent queue →](project-07-concurrent-queue.md)

> **Level 3 · ≈ 6 hours · Builds on:** Chapters 29 (threads, `jthread`, `stop_token`, condition variables), 21 (type erasure), Project 4
> **Reference solution:** [`code/p06/thread_pool.hpp`](code/p06/thread_pool.hpp), [`test.cpp`](code/p06/test.cpp), [`bench.cpp`](code/p06/bench.cpp), [`tsan.supp`](code/p06/tsan.supp)

## Goal

A fixed-size pool of worker threads that runs submitted callables and returns results through `std::future`, with **cancellation** and **graceful shutdown**. Small, but every line is a concurrency decision: who owns the queue, when a worker sleeps, what "shutdown" means to tasks that are queued, running, or not yet submitted.

## Specification

```cpp
edu::ThreadPool pool(4);
auto f = pool.submit([](int a, int b) { return a + b; }, 20, 22);   // std::future<int>
int answer = f.get();
```

1. `submit(f, args...)` returns `std::future<std::invoke_result_t<F, Args...>>`. It works with **move-only** arguments and results. It is `[[nodiscard]]` (dropping the future drops the only way to see an exception).
2. **Exceptions** thrown by a task are delivered through its future; the worker survives.
3. **Workers sleep on a condition variable** while the queue is empty. No polling, no `sleep` in the worker loop.
4. **Shutdown:**
   - `shutdown(Drain::yes)` (and the destructor): run every queued task, then join the workers.
   - `shutdown(Drain::no)`: discard queued tasks (their futures throw `std::future_error` with `broken_promise`), let running tasks finish.
   - `submit` after shutdown throws `std::runtime_error`.
5. **Cancellation:** `submit_cancellable(f, args...)` passes a `std::stop_token` as the first argument; `cancel_all()` requests stop for every cancellable task, drops queued work, and leaves the pool usable (new submissions get a fresh token).
6. Tasks run **outside** the queue lock.
7. Declaration order guarantees destruction order: the worker threads are declared **last**, so they are joined before the queue, mutex and condition variable they use are destroyed.

## Milestones

1. `submit` with `std::packaged_task` and a queue of `std::function<void()>`. It will fail to compile for move-only tasks: **that is the point.** (`std::function` requires copyable callables; `packaged_task` is move-only.)
2. Swap in your Project 4 `Function`.
3. Workers, condition variable and the shutdown protocol.
4. `Drain::no` and `broken_promise`.
5. `submit_cancellable`, `cancel_all`.
6. Stress test under TSan: four producers, four workers, 20 000 tasks, checksum the results.

## Tests that find the bugs

| Bug | Symptom |
|---|---|
| Notifying before the predicate changes, or checking the predicate without the lock | Lost wake-up: one task never runs, test hangs |
| Not checking `closed_` in the wait predicate | Destructor hangs forever |
| Running the task while holding the lock | No parallelism (the "4 × 100 ms sleeps finish in < 250 ms" test fails) |
| `workers_` declared before `q_`/`m_` | Use-after-destruction in worker threads during teardown |
| Exception escapes the worker loop | `std::terminate` |
| `cancel_all()` replaces the `stop_source` without the lock | TSan data race with `submit_cancellable` |
| A task waits for a task submitted to the same single-thread pool | Deadlock (see "Think about") |

## A TSan report that was *not* a bug in the pool

Run the reference test 5 times under ThreadSanitizer and one run may print two "data race" reports involving `std::runtime_error::~runtime_error` and `std::__exception_ptr::exception_ptr::_M_release` in the exception test. I did not want to write that off, so I reproduced it **without any pool code**:

```cpp
std::promise<int> p; auto f = p.get_future();
std::thread t([pr = std::move(p)]() mutable { try { throw std::runtime_error("boom"); }
                                              catch (...) { pr.set_exception(std::current_exception()); } });
try { f.get(); } catch (const std::runtime_error& e) { use(e.what()); }
t.join();
```

Three runs of 3000 iterations each produced the same two reports. The cause is that `libstdc++.so` is **not instrumented**, so TSan cannot see the atomic reference count inside `std::exception_ptr`, and it concludes that two threads touch the exception object without synchronisation. That is a limitation of the tool with this toolchain, not a race in the pool. I added a two-line suppression ([`tsan.supp`](code/p06/tsan.supp)) and the reference test then ran **25 of 25 clean**. Two lessons: (1) a sanitizer report needs an explanation, not a shrug, and (2) suppress the narrowest pattern you can justify, with a comment saying why.

Run with: `TSAN_OPTIONS="suppressions=tsan.supp" ./test_tsan`

## Think about (and write answers)

1. **Deadlock.** On a pool with *one* worker, a task submits another task and calls `.get()` on it. Why does that hang? What are the fixes: a larger pool (not a fix), running queued tasks while waiting ("helping"), continuations instead of blocking (`then`), or coroutines (Chapter 33)?
2. Why must the predicate be re-checked after `wait` returns? What is a spurious wake-up, and what does the predicate overload of `wait` do for you?
3. Notify **while holding** the lock or **after** releasing it? What does each cost? (The reference solution notifies after, in `submit`.)
4. One shared queue is a contention point. When do **per-worker queues with work stealing** win, and when do they lose? What do they do to task ordering and fairness?
5. `std::jthread` plus `stop_token` is available, but the workers here wait on a condition variable keyed on `closed_`, not on the token. How would you use `std::condition_variable_any::wait(lock, stop_token, pred)` instead? What does it buy?
6. What is the right number of threads for CPU-bound tasks? For tasks that block on I/O? Why is "one thread per request" different from a pool?
7. **Priorities and back-pressure:** an unbounded queue turns a slow consumer into out-of-memory. What do you do when `submit` is called faster than tasks complete?

## Measurements

`-O2`, GCC 13.3, **2-vCPU VM** (so two workers is the ceiling):

```text
empty tasks:               200000 in 285 ms   =   1.42 us/task
64 CPU tasks, 1 worker :   243 ms
64 CPU tasks, 2 workers:   121 ms     (2.0x)
64 CPU tasks, 4 workers:   118 ms     (no further gain: only two cores)
```

- **Scaling is what the hardware allows:** 1 → 2 workers halves the time; 4 workers on 2 cores add nothing (and, with more tasks, would add context switches).
- **1.4 µs per empty task is overhead**, not work: a heap allocation for the `packaged_task` shared state, the `Function` node, a mutex acquire and release on each side, and a condition-variable wake-up. If your tasks take less than ~10 µs, a thread pool will dominate your runtime; batch the work.

## Stretch goals

- **Parallel `for_each`/`transform`** on top of the pool: chunk the range, one task per chunk, wait for all. How do you choose the chunk size?
- **Bounded queue** with back-pressure: `submit` blocks (or fails) when the queue is full. Which is right for a server?
- **Work stealing:** per-worker deques, steal from the back of a random victim's deque. Measure on an unbalanced task set (a few long tasks plus many short ones).
- **`wait_idle()`** and a `Barrier`-style `parallel_invoke(f1, f2, f3)`.
- **Task priorities** with `std::priority_queue` (what happens to FIFO order within one priority?).
- Compare with `std::async`, which has no pool: `std::launch::async` creates a thread per call. Measure the cost.

## Reference solution notes

- **Type erasure:** the queue stores `edu::Function<void()>`; each entry wraps a `std::packaged_task` in a lambda that calls it. The `packaged_task` destructor is what turns a discarded task into a `broken_promise`: shutdown without drain needs no extra code.
- **Why a `std::tuple` of arguments:** `std::bind` copies and decays in surprising ways; a lambda capturing `fn` and a tuple, invoked with `std::apply` and `std::move`, forwards move-only arguments exactly once.
- **Cancellation is cooperative.** `cancel_all` cannot interrupt a running task; the task must poll its `stop_token`. This is the same contract as `std::jthread`, and for the same reason: forced cancellation cannot release the task's resources.
- **Known limitations** (all candidates for your own improvements): single queue and single lock; no priorities; no back-pressure; no `wait_idle`; tasks that wait on other tasks of the same pool can deadlock.

Run it:

```bash
cd code/p06
g++ -std=c++23 -Wall -Wextra -fsanitize=address,undefined test.cpp -o test_asan -pthread && ./test_asan
g++ -std=c++23 -Wall -Wextra -g -fsanitize=thread test.cpp -o test_tsan -pthread && TSAN_OPTIONS="suppressions=tsan.supp" ./test_tsan
g++ -std=c++23 -O2 bench.cpp -o bench -pthread && ./bench
```

```text
all tests passed
```

## What I learned (write it)

> Which tests would have passed with a subtly wrong implementation (a lost wake-up, say) most of the time? How many repetitions would you need before you believed it?
