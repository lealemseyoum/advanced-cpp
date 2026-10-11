# Project 9 — `epoll` Event Loop

[← Project 8](project-08-coroutine-task.md) &nbsp;|&nbsp; [Projects overview](README.md) &nbsp;|&nbsp; [Next: Project 10 — Async HTTP server →](project-10-async-http-server.md)

> **Level 4 · ≈ 8 hours · Builds on:** Chapters 33–35 (coroutines, building a `Task`, coroutines and networking), and Project 8 (`Task<T>`)
> **Reference solution:** [`code/p09/event_loop.hpp`](code/p09/event_loop.hpp), [`test.cpp`](code/p09/test.cpp), [`bench.cpp`](code/p09/bench.cpp)

## Goal

Build a single-threaded reactor on Linux `epoll` and connect it to coroutines, so that this reads like blocking code and runs like an event loop:

```cpp
edu::Task<void> echo(edu::EventLoop& loop, int fd) {
    char buf[256];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) { ... }
        else if (errno == EAGAIN) co_await loop.readable(fd);   // yield to the loop until fd is readable
    }
}
```

The loop is about 100 lines. The learning is in the *contract*: who resumes whom, on which thread, and what happens to a coroutine that is waiting when the object it waits on goes away.

## Specification

1. `EventLoop` owns an `epoll` file descriptor (RAII, Project 1).
2. **Awaitables:** `co_await loop.readable(fd)`, `co_await loop.writable(fd)`, `co_await loop.sleep(duration)`. Each suspends the coroutine and registers *exactly one* wake-up source.
3. **Timers** are kept in an ordered container; the `epoll_wait` timeout is derived from the earliest deadline, **rounded up**, so no timer fires early.
4. **`loop.spawn(Task<void>)`** starts a detached task. The loop tracks the number of live tasks. `run()` returns when there are no live tasks left, `stop()` was called, or no task can ever be woken (nothing registered: this would otherwise hang forever).
5. **Readiness events are one-shot** (`EPOLLONESHOT`): the loop removes the waiter *before* resuming it, so the resumed coroutine may immediately wait on the same descriptor again.
6. **An exception in a detached task does not stop the loop**; it is counted (`task_errors()`), and your own logging policy decides what else to do.
7. Two coroutines waiting on the same descriptor and the same event is a programming error: throw `std::logic_error` rather than lose one of them silently.
8. **Single-threaded:** every resumption happens on the thread that calls `run()`. State this in the header comment; it is what makes the loop lock-free.

## Milestones

1. `epoll_create1`, an `FdAwaiter` that registers `EPOLLONESHOT | EPOLLIN`, and a `run()` that calls `epoll_wait` and resumes handles. Test with a `socketpair`.
2. Timers: `multimap<time_point, handle>`; compute the timeout; resume everything due. Test ordering with sleeps registered out of order.
3. `spawn` with a detached coroutine that decrements a live-task counter. Test: 1000 concurrent 50 ms sleeps finish in about 50 ms of wall time (not 50 seconds).
4. Echo ping-pong between two coroutines over a `socketpair`, 2000 round trips on one thread.
5. Error handling and the "nothing can wake me" exit.
6. Sanitizers and a benchmark.

## A bug ASan found in my own test

My first version of the exception test did this:

```cpp
loop.spawn([&]() -> Task<void> { co_await loop.sleep(10ms); other_ran = true; }());   // BUG
```

ASan:

```text
ERROR: AddressSanitizer: stack-use-after-scope
READ of size 8 ... in main::{lambda()#7}::operator()(...) [clone .actor]
```

The lambda object is a **temporary**; the captured references live in the closure, and the coroutine frame refers to the closure rather than copying the captures. The temporary is destroyed at the end of the full expression, long before the timer fires. This is the dangling-lambda trap from Project 8's "Think about" question #4, hit in practice. Fixes, best first:

1. Make the closure a **named variable** that outlives the coroutine (what the final test does).
2. Better: don't capture; **pass state as parameters** (`Task<void> f(EventLoop& loop, int& flag)`), because parameters are *copied into the frame*.
3. Or use a captureless lambda and pass the needed objects as arguments.

## Tests that find the bugs

| Bug | Symptom |
|---|---|
| Timeout rounded **down** | The loop wakes a few hundred microseconds early, runs nothing, and spins (busy loop) until the deadline |
| Waiter erased *after* `resume()` | The coroutine waits on the same fd again from inside `resume()` and the new registration is erased |
| `EPOLL_CTL_DEL` omitted for one-shot events | Next `ADD` of the same fd fails with `EEXIST` |
| Nothing registered, tasks alive, `epoll_wait(-1)` | `run()` hangs forever |
| Capturing lambda temporary | `stack-use-after-scope` (above) |
| `Task` destroyed while its handle is in the epoll set | The loop later resumes a freed frame: use-after-free |
| A closed fd still in the set | `EBADF` / the number is reused by a new connection and wakes the wrong coroutine |
| `EPOLLERR`/`EPOLLHUP` not requested or not handled | Waiters never wake when the peer closes; the coroutine re-checks with `read` and sees `0`/error |

## Think about (and write answers)

1. **Level- vs edge-triggered.** The reference uses one-shot level-triggered registrations. What changes with `EPOLLET`? Why must edge-triggered code read until `EAGAIN`? What bugs does it prevent and introduce?
2. **Cost per wait.** Each wait here is `epoll_ctl(ADD)` + `epoll_wait` + `epoll_ctl(DEL)`: three system calls. How would you register each descriptor *once* and only re-arm it (`EPOLL_CTL_MOD` or a persistent registration with a waiter slot per direction)? What changes in the data structure?
3. **Cancellation.** A coroutine waits on `readable(fd)` and another coroutine wants to cancel it (shutdown). What must happen to the registration, the waiter, and the frame? Who destroys the frame? Sketch an API.
4. **Timers.** `multimap` is `O(log n)` per timer. What do production loops use for hundreds of thousands of timers? (Timer wheels, hierarchical wheels, heaps.) What does `timerfd` give you that the `epoll_wait` timeout does not?
5. **Fairness.** One coroutine that never suspends starves the loop. Which of your tests would reveal that? How do you add a `yield()` (`co_await loop.post()`), and where does it re-queue the handle?
6. **Multi-threading.** To use several cores: one loop per thread (shared-nothing, `SO_REUSEPORT`), or a shared loop with a thread pool? What would you need to make `spawn` callable from another thread (an `eventfd` to wake the loop)?
7. **`io_uring` vs `epoll`.** `epoll` tells you a descriptor is *ready*; `io_uring` performs the operation and tells you it is *done*. How does that change the awaiter interface (`co_await loop.read(fd, buf)`)?

## Measurements

`-O2`, GCC 13.3, 2-vCPU VM, 100 000 round trips (4-byte message) over a `socketpair`:

```text
coroutines on one thread (this loop)   5.3 - 6.4 us per round trip
two threads with blocking read/write   38.8 - 38.9 us per round trip
100 000 concurrent sleepers (50-99 ms) 152 - 156 ms wall time, all complete
```

- **The single-threaded coroutine loop is about 6-7× faster per round trip than two blocking threads.** The difference is mostly thread wake-up latency: each blocking `read` must be woken by the scheduler on another CPU, while on the loop the "other side" is already running in the same thread and the data is ready on the next `epoll_wait`. This is a feature of the *ping-pong pattern* on a VM; do not generalise it to throughput with many connections.
- **100 000 timers cost ~100 ms of overhead beyond the 50-99 ms sleep**, i.e. about 1 µs per sleeper (frame allocation, `multimap` insertion, resumption). Fine for this scale; this is where a timer wheel would pay off at millions.
- **Each wait still makes three system calls.** That's the biggest obvious cost left, and it is the reference solution's deliberate simplification (see "Think about" #2).

## Stretch goals

- **Persistent registrations:** one `epoll_ctl(ADD)` per descriptor, `MOD` only when interest changes; support separate reader and writer waiters per fd.
- **`timerfd`** (or a timer heap) and **`eventfd`** for cross-thread `post(Task)`.
- **Cancellation:** `co_await loop.readable(fd, stop_token)` or a `cancel(fd)` that resumes the waiter with an error.
- **Accept loop:** `co_await listener.accept()` returning a connected socket (Project 10 builds on it).
- **Edge-triggered mode** with a read-until-`EAGAIN` helper.
- **Per-thread loops** with `SO_REUSEPORT` and a load test comparing 1 vs 2 threads.
- **`io_uring` backend** behind the same `loop.read()/write()` awaitable interface.

## Reference solution notes

- **The loop never owns task frames after `spawn`**: the `Detached` coroutine awaits the `Task` and frees itself when done; `active_` counts live detached coroutines.
- **`run_due_timers` is called after every `epoll_wait`**, whether it returned events or timed out, so a busy fd cannot starve timers.
- **Waiters are keyed by descriptor.** One waiter per fd keeps the map trivial; it is also why a second waiter on the same fd throws.
- **The "can never wake" check** (`waiters_.empty() && timers_.empty()` with tasks still alive) is a deadlock detector. In a real server, tasks waiting on something *external* (a cross-thread `post`, a signal) need an `eventfd` registered permanently so that the loop is never "empty".
- **Not thread-safe by design.**

Run it:

```bash
cd code/p09
g++ -std=c++23 -O2 -Wall -Wextra -fsanitize=address,undefined test.cpp -o test_asan -pthread && ./test_asan
g++ -std=c++23 -O2 -Wall -Wextra -g -fsanitize=thread test.cpp -o test_tsan -pthread && ./test_tsan
g++ -std=c++23 -O2 bench.cpp -o bench -pthread && ./bench
```

```text
all tests passed      (GCC 13.3 ASan+UBSan, TSan; GCC 14.2; Clang 18 -O2)
```

## What I learned (write it)

> Draw the sequence of calls from `epoll_wait` returning to your coroutine's next line. Which part is the loop's code, which is the awaiter's, and which is the compiler's?
