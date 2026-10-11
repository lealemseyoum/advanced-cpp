# Project 8 — Coroutine `Task<T>`

[← Project 7](project-07-concurrent-queue.md) &nbsp;|&nbsp; [Projects overview](README.md) &nbsp;|&nbsp; [Next: Project 9 — Event loop →](project-09-event-loop.md)

> **Level 4 · ≈ 8 hours · Builds on:** Chapters 33 (coroutine machinery), 34 (building a `Task`)
> **Reference solution:** [`code/p08/task.hpp`](code/p08/task.hpp), [`test.cpp`](code/p08/test.cpp), [`bench.cpp`](code/p08/bench.cpp)

## Goal

Implement a **lazy, awaitable, exception-propagating, move-only `Task<T>`** and the tools to run one (`sync_wait`), and understand every part of the machinery: the promise, the frame, the handle, the awaiters, symmetric transfer, and who destroys what.

This project completes Chapter 34's exercise. Chapter 34 walked through the design; here you build it from a blank file and prove it correct.

## Specification

```cpp
edu::Task<int> add(int a, int b) { co_return a + b; }
edu::Task<int> run()             { co_return co_await add(1, 2) + co_await add(3, 4); }
int v = edu::sync_wait(run());   // 10
```

1. **Lazy:** calling a coroutine function runs nothing (`initial_suspend` is `suspend_always`). Destroying a never-started `Task` frees its frame and never runs its body.
2. **Owning, move-only:** the `Task` owns the coroutine frame and destroys it in its destructor.
3. **`co_await std::move(task)`** starts the task, suspends the awaiter, and resumes the awaiter when the task finishes, **using symmetric transfer** (`await_suspend` returns a `coroutine_handle<>`; `final_suspend`'s awaiter returns the continuation).
4. **Results:** `Task<T>` for move-only `T`; a `Task<void>` specialisation.
5. **Exceptions:** an exception thrown inside the task is captured (`unhandled_exception`) and **rethrown from the `co_await` expression**.
6. **`sync_wait(task)`** blocks the calling thread until the task completes, returning its value or rethrowing its exception. It must work when the task is *resumed on a different thread* than the one that started it.
7. **Frame accounting:** every coroutine frame that is allocated is freed (count them with a replaced `operator new`/`delete`).

## Milestones

1. `Task<int>` with a stored `int`, `initial_suspend = suspend_always`, `final_suspend = suspend_always`, `get_return_object`, `return_value`. Drive it by hand with `handle.resume()` and `handle.done()`. This is the machinery with the scaffolding removed.
2. `operator co_await`: record the continuation, resume the task. First version: `await_suspend` returns `void` and calls `h.resume()`.
3. **Final awaiter** that resumes the continuation. First version: `continuation.resume()` inside `await_suspend` → watch the stack grow in a 1 000 000-iteration loop.
4. **Symmetric transfer:** both `await_suspend`s return handles. Rerun the loop.
5. Exceptions; `Task<void>`.
6. `sync_wait` using a small detached helper coroutine and a latch.
7. Frame-accounting test and sanitizers.

## What the stack does: a finding that depends on the compiler

Symmetric transfer is *specified* to bound stack use: `await_suspend` returning a handle means "resume that handle after this coroutine has been suspended", so a loop of a million `co_await`s must not nest a million frames. In practice that guarantee is delivered by the compiler turning the resumption into a **tail call**. So I ran the million-await loop (`loop(1'000'000)`) and the 100 000-deep recursion (`depth(100'000)`) under every combination I could build:

```text
compiler            flags                          result
GCC 13.3            -O0                            segfault (stack overflow)
GCC 13.3            -O1                            segfault
GCC 13.3            -O2                            OK
GCC 13.3 / 14.2     -O2 -fsanitize=address         ASan stack-overflow
GCC 14.2            -O0, -O1                       segfault
GCC 14.2            -O2                            OK
GCC 14.2            -O2 -fsanitize=thread          crash (SEGV)
Clang 18.1.3        -O0                            OK
Clang 18.1.3        -O2                            OK
```

Read that table as three separate facts:

- **The standard** says symmetric transfer must not grow the stack.
- **Clang 18** implements this even at `-O0`. **GCC 13.3 and 14.2** deliver it only when optimisation is on and the sanitizers are off; on a debug build, a deep chain of awaits **overflows the stack**. (I did not test GCC 15/16; check before relying on either behaviour.)
- **The practical consequence:** a coroutine library that is correct in release can crash in a debug or sanitizer build with GCC. The reference tests therefore choose the depth of the deep tests based on whether tail calls are available (`kTailCalls`) and print which mode they ran in. Never write a test that "passes" by silently doing nothing; say what it covered.

## Tests that find the bugs

| Bug | Symptom |
|---|---|
| `initial_suspend` returns `suspend_never` | Body runs at call time; "lazy" test fails; if it throws, the exception escapes before anyone can await |
| `final_suspend` returns `suspend_never` | The frame is destroyed at the end of the coroutine, and the `Task` then destroys it again: double free / use-after-free |
| `final_suspend` is not `noexcept` | Compile error (it must not throw) |
| Reading `result` after the `Task` destroyed its handle | ASan use-after-free |
| Continuation resumed *inside* `final_suspend`'s `await_suspend` via `.resume()` | Works, but the stack grows by one frame per await (see above) |
| `await_ready` returns `true` for a not-yet-started task | Never started; `await_resume` reads an empty variant |
| Forgetting `rethrow_exception` | Errors silently swallowed |
| `return_value(T)` taking by value only | Extra moves; fails for non-movable results; use `U&&` |
| Task captured by reference in a lambda that outlives it | Dangling frame (see "Think about") |

## Think about (and write answers)

1. **What does `co_await std::move(task)` do, step by step?** Write the sequence of calls: `await_ready`, `await_suspend(caller)`, the task's body, `final_suspend`, the final awaiter's `await_suspend`, the caller resumes, `await_resume`. Which of these run on the awaiting thread?
2. Why must `final_suspend` suspend? Who is allowed to destroy the frame if it does, and what is the continuation's guarantee that the frame is still readable when it resumes?
3. Why does `operator co_await` require an **rvalue** (`&&`)? What would `co_await task` on an lvalue mean, and why is it a bug waiting to happen?
4. The **dangling lambda** problem: `auto t = [&x]() -> Task<int> { co_return x; }();` where the lambda is a temporary. What does the coroutine frame store, and what has been destroyed by the time it runs? Why do guidelines say "coroutine lambdas must not capture; pass state as parameters"?
5. A `Task` is lazy and the awaiter resumes on **whichever thread completes the task**. What does that mean for thread-affinity (a Qt-style GUI thread, an `epoll` loop)? How would you add a scheduler so that continuations resume on a chosen executor?
6. Why did the benchmark below see a heap allocation per task? Under what conditions may a compiler **elide** the frame allocation? Which property of your type blocks it?
7. What is wrong with `sync_wait` inside a coroutine that runs on the same event loop that must complete the awaited task?

## Measurements

`-O2`, 10 million iterations of a loop that either calls a plain function or `co_await`s a leaf `Task<int>`:

```text
compiler       plain call     co_await Task     frame allocations (awaited loop)
GCC 13.3       2.40 ns        28.8 ns           10 000 002
GCC 14.2       2.51 ns        21.1 ns           10 000 002
Clang 18.1.3   0.96 ns        18.3 ns           10 000 002
```

- A `co_await` of a freshly created leaf task costs **18-29 ns here: roughly 20 to 30 times the cost of a call**, and it performs **one heap allocation per task** (the frame) plus the transfers. That is not a reason to avoid coroutines (an I/O wait takes microseconds), but it is a reason not to use `Task<int>` for something that could be a function call.
- **The allocation was not elided** by any of the three compilers for this type (the count is one per iteration). Frame elision ("HALO") depends on the compiler seeing that the task's lifetime is nested inside the caller's, and on annotations (Clang has `[[clang::coro_await_elidable]]`, from version 19; I did not test it). I did not investigate beyond counting allocations: treat "coroutines allocate a frame per call unless proven otherwise" as the working assumption.
- These are microbenchmarks of the *machinery*. They say nothing about a coroutine that suspends on real I/O.

## Stretch goals

- **`when_all(tasks...)`** and **`when_any`**: start several tasks, resume the awaiter when all/one completes. This needs a shared counter and a thread-safe continuation hand-off: where is the race?
- **A thread-pool scheduler:** `co_await pool.schedule()` suspends and resumes on a pool thread (Project 6). Then `co_await` inside tasks hops between threads.
- **`Generator<T>`** with `co_yield` (a different promise type; a good contrast with `Task`).
- **Cancellation:** pass a `std::stop_token` through the promise and check it in your awaiters.
- **Custom frame allocator:** give `promise_type` an `operator new(size_t)` that takes memory from your Project 5 arena (and think about what `operator delete` must do).
- **Detect misuse:** make awaiting a task twice, or destroying a running task, a debug-mode error.

## Reference solution notes

- **`PromiseBase`** holds the continuation and the `FinalAwaiter`; `Task<T>::promise_type` adds a `std::variant<monostate, T, exception_ptr>` for the outcome. The `Task<void>` specialisation replaces the variant with a bare `exception_ptr`.
- **The `Task` owns the frame**, and `final_suspend` returns `suspend_always`-like behaviour (via the final awaiter that *transfers* control rather than leaving the coroutine suspended with no one to resume it); the task's destructor calls `handle.destroy()`.
- **`sync_wait`** starts a tiny self-destroying `Runner` coroutine (it uses `suspend_never` at the end, which is correct *because nobody holds its handle*) that awaits the task and signals a mutex/condition-variable latch. The calling thread blocks on the latch. This is the standard construction; contrast with a design that polls `done()`.
- **No allocator customisation, no cancellation, no scheduler:** those are stretch goals, and they are the difference between this and production libraries (cppcoro, libunifex, folly::coro, Boost.Asio's `awaitable`).

Run it:

```bash
cd code/p08
g++ -std=c++23 -O2 -Wall -Wextra test.cpp -o test -pthread && ./test                       # full depth
g++ -std=c++23 -Wall -Wextra -fsanitize=address,undefined test.cpp -o test_asan -pthread && ./test_asan   # reduced depth
g++ -std=c++23 -Wall -Wextra -g -fsanitize=thread test.cpp -o test_tsan -pthread && ./test_tsan
g++ -std=c++23 -O2 bench.cpp -o bench -pthread && ./bench
```

```text
frames allocated and freed: balanced
deep tests: full (1M awaits, depth 100k)          # -O2, no sanitizers (GCC 13.3, GCC 14.2, Clang 18)
deep tests: reduced (no guaranteed tail calls in this build)   # GCC with -O0/-O1 or sanitizers
all tests passed
```

## What I learned (write it)

> Where in this project did the standard, the compiler and the sanitizer disagree about what your program does? Which one was right?
