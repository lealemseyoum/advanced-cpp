# Chapter 34 — Building a Coroutine Type: `Task<T>`

> **Part XIII · Coroutines** &nbsp;|&nbsp; **Level 4** (compiler/runtime) &nbsp;|&nbsp; **≈ 10 hours**
> **Prerequisites:** [Chapter 33](33-coroutines.md), [Chapter 25 (ownership)](../part-10-memory/25-smart-pointers.md), [Chapter 29 (threads)](../part-12-concurrency/29-threading.md) &nbsp;|&nbsp; **Standards:** C++20 ⚖️; `std::task` / `std::execution::task` (P3552) are C++26 🟡, not in GCC 14.2 &nbsp;|&nbsp; **Tools:** `g++-14`, `clang++-18`, ASan

[← Previous: Chapter 33](33-coroutines.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 35 — Coroutines and networking →](35-coroutines-and-networking.md)

---

**In one sentence:** a `Task<T>` is a lazy, move-only, single-owner handle to a coroutine whose promise stores the result and a **continuation**, and whose final suspension hands control straight to whoever awaited it, which is the whole recipe for composing asynchronous operations with `co_await`.

**By the end of this chapter you can:**

- build `Task<T>` from nothing: promise, result storage, exception transport, awaiter, symmetric transfer
- explain precisely how `co_await task` links two coroutine frames and returns control, with no scheduler involved
- write a run loop with timers, a thread-hopping scheduler, `sync_wait` and `when_all`, and say what each adds
- prove, with a measurement, why symmetric transfer is mandatory for deep await chains, and know which compilers need which flags
- list the design decisions behind `Task` (lazy vs eager, ownership, cancellation, allocator) and their trade-offs

---

## 1. Problem

[Chapter 33](33-coroutines.md) gave us the mechanism and a `Generator`, a coroutine that produces a *sequence*. Asynchronous code needs something else: a coroutine that produces **one result later** and can itself be awaited by another coroutine:

```cpp
Task<Config>  load_config(Path p);
Task<Reply>   handle(Request r)  {
    Config c  = co_await load_config("app.toml");      // suspend until the config is ready
    Row    row = co_await db.query(c, r.id);           // suspend until the database answers
    co_return render(row);
}
```

Requirements, each of which constrains the design:

| Requirement | Consequence |
|---|---|
| `co_await f()` must compose: a task awaiting a task awaiting a task | each task must record **who is waiting for it** (the *continuation*) |
| Results and **exceptions** must cross the suspension | the promise stores `variant<monostate, T, exception_ptr>` |
| 100 000-deep chains must not overflow the stack | completion must **transfer** control to the continuation without nesting (symmetric transfer) |
| Exactly one owner for the frame | move-only, destroys the frame in its destructor |
| The caller decides *when* and *where* it runs | **lazy** start; scheduling is a separate concern |
| It must be usable from `main()` | a blocking `sync_wait` bridge |

---

## 2. Historical context

| Era | How "async result" was composed |
|---|---|
| C++03 | Callbacks, `boost::bind`, hand-rolled state machines |
| C++11 | `std::future`/`std::promise`/`std::async`: a *blocking* `get()`, no continuation (`.then` never standardised); a thread per async call |
| 2010s | `boost::asio` completion handlers; Folly futures with `.then()`; "callback pyramids" |
| 2017–19 | Coroutines TS: **cppcoro** (Lewis Baker) defines `task<T>`, `sync_wait`, `when_all` and the symmetric-transfer design; Folly `coro::Task`; Asio `awaitable<T>` |
| 2020 | C++20 ships the *mechanism* only; no `Task` in the library |
| 2024–26 | **`std::execution::task`** (P3552) adopted for C++26 🟡 together with senders/receivers; libstdc++ 14.2 has neither |

Lewis Baker's cppcoro `task<T>` is the reference design that nearly every library follows, and the one we build (simplified).

---

## 3. Modern solution

```cpp
Task<int> leaf(int x)   { co_return x * 2; }
Task<int> mid(int x)    { co_return co_await leaf(x) + co_await leaf(x + 1); }
int main()              { return sync_wait(mid(10)); }          // 42
```

Four small pieces: `Task<T>` (this chapter's centrepiece), `sync_wait` (bridge from blocking code), a **scheduler** (run loop, timers, thread pool) that decides *where and when* handles are resumed, and combinators like `when_all`. The language is involved only through `co_await`/`co_return`; everything else is library, which is why different libraries can make different choices.

---

## 4. Mental model

### Two frames, one link

```text
   parent coroutine frame                      child Task frame
  ┌──────────────────────┐                   ┌──────────────────────────┐
  │ ... int a = co_await │                   │ promise                  │
  │      leaf(10);       │                   │   continuation ──────────┼──► parent's handle   (set by await_suspend)
  │ locals, suspend idx  │                   │   result: variant<...>   │
  └──────────────────────┘                   │ locals, suspend idx      │
          ▲                                  └──────────────────────────┘
          │  Task (owner) is a temporary in the PARENT's full-expression: it dies after await_resume
```

Control flow of `co_await child()`:

```text
1. parent evaluates child()       -> creates the child frame, suspended at initial_suspend (lazy)
2. parent: await_ready()          -> false (child hasn't run)
3. parent: await_suspend(parent_handle):
       child.promise.continuation = parent_handle
       return child_handle                          // symmetric transfer: "now run the child"
4. child runs ... co_return v     -> promise stores v
5. child: final_suspend -> FinalAwaiter::await_suspend(child_handle):
       return child.promise.continuation            // symmetric transfer back: "now run the parent"
6. parent resumes -> await_resume() -> promise.take() -> v (or rethrows the stored exception)
7. end of the full-expression: the Task temporary is destroyed -> child frame freed
```

Steps 3 and 5 are **tail calls**: neither `parent.resume()` nor `child.resume()` is called *by* the other, so the native stack depth stays constant however long the chain. Without this, step 5 would be `continuation.resume()` *inside* the child's frame, and a loop of one million awaited children would nest one million frames deep.

### Where "async" comes from

`Task` has **no scheduler**. It only chains frames. A task becomes asynchronous when some awaiter at the bottom of the chain parks the handle somewhere other than the current call stack: a timer queue, an `epoll` loop, another thread's queue. When that event fires, someone calls `h.resume()`; the leaf resumes, finishes, transfers to its parent, and so on up the chain. **The scheduler is whoever calls `resume()`.**

### Design axes (decide these once, document them)

| Axis | Choice here | Alternative and when |
|---|---|---|
| Start | **Lazy** (`initial_suspend` = `suspend_always`) | Eager: starts immediately; needed for fire-and-forget or "start now, await later" parallelism; risk: exceptions and lifetimes before you're ready |
| Ownership | **Unique, move-only**; destructor destroys the frame | Shared (`shared_task`): many awaiters; needs atomics |
| Result access | Stored in the promise, moved out by `await_resume` | `T&` for reference results; expected-style errors |
| Errors | Exceptions captured and rethrown at `co_await` | `Task<expected<T,E>>` for error codes; or both |
| Where it resumes | **Wherever the completing thread is** (the child's last resume) | Affinity: always resume on the awaiter's scheduler (adds a hop per await) |
| Cancellation | none (exercise) | `stop_token` through the promise; cancel-aware awaiters |
| Allocation | default `operator new` | Pool/arena via `promise_type::operator new` |

---

## 5. Language rules

| Rule | Why it matters here |
|---|---|
| **Temporaries live to the end of the full-expression**, and a full-expression containing `co_await` spans the suspension | `co_await child();` keeps the `Task` temporary (and so the child frame) alive until after `await_resume`. This is what makes `Task` safe without storing it in a variable |
| `operator co_await` may be **ref-qualified**; we provide `&&` only | `co_await some_lvalue_task;` fails to compile unless you write `std::move(t)`: ownership of a one-shot task is explicit |
| `await_suspend` may return `std::coroutine_handle<>` | the returned handle is resumed as if by `return h.resume();` and the standard requires this not to grow the stack (symmetric transfer) |
| `std::noop_coroutine()` | a handle that does nothing when resumed; the "no continuation" value for a root task (resuming it returns control to the resumer) |
| `final_suspend()` is `noexcept` and its awaiter may return a handle | this is where the continuation is picked up |
| Exceptions thrown in the body go to `unhandled_exception()`; `co_await` of an awaiter that throws in `await_resume` rethrows in the awaiting coroutine | how errors propagate up the chain |
| Parameters are copied into the frame; references are not (Chapter 33) | a `Task<void> worker(Loop& L)` taking a reference is fine **only** if `L` outlives the task |
| A coroutine that is never resumed to completion still has its locals destroyed by `handle.destroy()` | `Task`'s destructor can safely drop an unfinished (suspended) task; it can **not** destroy one that is currently running |

### Layer check

| Layer | Question | Answer |
|---|---|---|
| **C++ standard** | Is symmetric transfer guaranteed? | **Yes**: the standard requires `await_suspend`'s returned handle to be resumed without unbounded stack growth |
| **Compiler** | Does GCC/Clang actually do it? | **Clang: yes at every level. GCC 14.2: only when sibling-call optimisation is on** (`-O2`, or `-O0 -foptimize-sibling-calls`); at `-O0`/`-O1` it nests (Experiment 2). Known GCC bug 100897 🔧. Always test deep chains in your Debug build |
| **ABI** | Anything binary? | The `Task` type, its promise layout and `operator new` are your own ABI; keep coroutine types inside one toolchain |
| **OS** | What bounds the chain depth when symmetric transfer fails? | The thread's stack limit (`ulimit -s`, 8 MiB default); the process dies with `SIGSEGV` |
| **CPU** | Cost of a tail-call transfer | One indirect `jmp`: a few cycles, plus the frame's cache misses |

---

## 6. Implementation model

Here is the complete library, ~100 lines. It is a *shared header* (`task.hpp`) used by every experiment in this chapter; the checker writes it next to each snippet, so you can copy it into a file with that name and build all the experiments yourself. Read it slowly; each member answers one question from §4.

```cpp
// @test file task.hpp
#pragma once
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace co {

// Eager, self-destroying coroutine for internal plumbing (sync_wait, when_all). Never exposed to users.
struct Fire {
    struct promise_type {
        Fire get_return_object() const noexcept { return {}; }
        std::suspend_never initial_suspend() const noexcept { return {}; }
        std::suspend_never final_suspend() const noexcept { return {}; }      // frame is freed when the body ends
        void return_void() const noexcept {}
        void unhandled_exception() const noexcept { std::terminate(); }
    };
};

namespace detail {

struct PromiseBase {
    std::coroutine_handle<> continuation = std::noop_coroutine();            // who to run when we finish

    std::suspend_always initial_suspend() const noexcept { return {}; }       // lazy: nothing runs until awaited

    struct FinalAwaiter {
        bool await_ready() const noexcept { return false; }                   // always suspend: the owner reads the result
        template <class P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) const noexcept {
#ifdef CO_NO_SYMMETRIC_TRANSFER
            h.promise().continuation.resume();                                // WRONG: nests a resume() inside this frame
            return std::noop_coroutine();
#else
            return h.promise().continuation;                                  // tail call into the awaiting coroutine
#endif
        }
        void await_resume() const noexcept {}
    };
    FinalAwaiter final_suspend() const noexcept { return {}; }
};

template <class T>
struct ResultPromise : PromiseBase {
    std::variant<std::monostate, T, std::exception_ptr> result;
    template <class U = T> void return_value(U&& v) { result.template emplace<1>(std::forward<U>(v)); }
    void unhandled_exception() noexcept { result.template emplace<2>(std::current_exception()); }
    T take() {
        if (result.index() == 2) std::rethrow_exception(std::get<2>(result));
        return std::move(std::get<1>(result));
    }
};

template <>
struct ResultPromise<void> : PromiseBase {
    std::exception_ptr error;
    void return_void() noexcept {}
    void unhandled_exception() noexcept { error = std::current_exception(); }
    void take() { if (error) std::rethrow_exception(error); }
};

}  // namespace detail

template <class T = void>
class [[nodiscard]] Task {
public:
    struct promise_type : detail::ResultPromise<T> {
        Task get_return_object() noexcept { return Task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
    };
    using handle_type = std::coroutine_handle<promise_type>;

    Task() = default;
    Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task& operator=(Task&& o) noexcept { if (this != &o) { reset(); h_ = std::exchange(o.h_, {}); } return *this; }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { reset(); }

    bool done() const noexcept { return !h_ || h_.done(); }
    handle_type handle() const noexcept { return h_; }
    decltype(auto) get() { return h_.promise().take(); }                      // for a finished task (used by schedulers)

    auto operator co_await() && noexcept {
        struct Awaiter {
            handle_type h;
            bool await_ready() const noexcept { return !h || h.done(); }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) const noexcept {
                h.promise().continuation = awaiting;                          // 1. remember who is waiting
                return h;                                                     // 2. symmetric transfer: run the child now
            }
            T await_resume() const { return h.promise().take(); }             // result or rethrow
        };
        return Awaiter{h_};
    }

private:
    explicit Task(handle_type h) noexcept : h_(h) {}
    void reset() noexcept { if (h_) { h_.destroy(); h_ = {}; } }
    handle_type h_;
};

// Bridge from ordinary blocking code: run `task` to completion and return its result.
template <class T>
T sync_wait(Task<T> task) {
    using R = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
    std::optional<R> out;
    std::exception_ptr err;
    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    [](Task<T>& t, std::optional<R>& out, std::exception_ptr& err, std::mutex& m, std::condition_variable& cv, bool& ready) -> Fire {
        try {
            if constexpr (std::is_void_v<T>) { co_await std::move(t); out.emplace(); }
            else                              out.emplace(co_await std::move(t));
        } catch (...) { err = std::current_exception(); }
        std::lock_guard lk(m);                       // notify while holding the lock: the waiter cannot return (and destroy m/cv) before we unlock
        ready = true;
        cv.notify_one();
    }(task, out, err, m, cv, ready);                 // runs on this thread until the first real suspension
    std::unique_lock lk(m);
    cv.wait(lk, [&] { return ready; });
    if (err) std::rethrow_exception(err);
    if constexpr (!std::is_void_v<T>) return std::move(*out);
}

}  // namespace co
```

Every design decision from §4 is visible:

- **Lazy start:** `initial_suspend` is `suspend_always`; the frame is created by the call but nothing runs.
- **Continuation:** a single `std::coroutine_handle<>` in the promise, defaulting to `noop_coroutine()` so a root task has somewhere harmless to return.
- **`FinalAwaiter`:** always suspends (so the frame survives for the owner to read the result) and returns the continuation: step 5 of the control-flow diagram. The `#ifdef` branch is the *wrong* design, kept so you can measure it.
- **Result storage:** `variant<monostate, T, exception_ptr>`; `void` gets its own specialisation because a promise may declare `return_void` *or* `return_value`, not both.
- **Ownership:** `Task` is move-only and `reset()` destroys the frame. The awaiter holds only a copy of the handle (non-owning).
- **`sync_wait`:** wraps the task in an eager `Fire` coroutine that stores the result and signals a condition variable. Note the comment about notifying under the lock: a classic lifetime bug otherwise (the waiting thread wakes, returns, and destroys the condition variable the notifier is still touching).

### Frame allocation

One `operator new` per `Task`. For a `co_await` on a trivial child, that allocation dominates (Experiment 6). Giving `PromiseBase` its own `operator new` is the cheapest speed-up and is Exercise 3.

---

## 7. Experiments

### Experiment 1 ✅: Compose tasks; laziness; exceptions travel the chain

```cpp
// @test run -std=c++23 -O0
#include "task.hpp"
#include <cstdio>
#include <stdexcept>

using namespace co;

Task<int> leaf(int x) { std::printf("  leaf(%d) runs\n", x); co_return x * 2; }

Task<int> mid(int x) {
    std::printf("  mid(%d) starts\n", x);
    int a = co_await leaf(x);
    int b = co_await leaf(x + 1);
    co_return a + b;
}

Task<void> thrower() { co_await leaf(1); throw std::runtime_error("boom from thrower"); }

Task<int> catcher() {
    try { co_await thrower(); }
    catch (const std::exception& e) { std::printf("  catcher caught: %s\n", e.what()); co_return -1; }
    co_return 0;
}

int main() {
    std::puts("create mid(10) -- nothing should run yet:");
    Task<int> t = mid(10);
    std::printf("  created; done=%d\n", t.done());
    std::puts("sync_wait:");
    int v = sync_wait(std::move(t));
    std::printf("result: %d\n", v);

    std::puts("exception propagation:");
    std::printf("result: %d\n", sync_wait(catcher()));

    std::puts("uncaught at the root:");
    try { sync_wait(thrower()); } catch (const std::exception& e) { std::printf("  main caught: %s\n", e.what()); }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
create mid(10) -- nothing should run yet:
  created; done=0
sync_wait:
  mid(10) starts
  leaf(10) runs
  leaf(11) runs
result: 42
exception propagation:
  leaf(1) runs
  catcher caught: boom from thrower
result: -1
uncaught at the root:
  leaf(1) runs
  main caught: boom from thrower
```

Laziness: `mid(10)` creates a frame and runs *nothing*. `leaf` runs inside `mid`'s `co_await`, and the whole chain executes on the calling thread because no awaiter ever parked a handle. Exceptions: the body's `throw` is caught by the *promise's* `unhandled_exception`, stored as an `exception_ptr`, and rethrown by `await_resume` **inside the awaiting coroutine**, where an ordinary `try`/`catch` handles it.

### Experiment 2 ✅: Why symmetric transfer is not optional

One million sequential `co_await`s of an already-complete child, in a single loop.

```cpp
// @test run -std=c++23 -O2
#include "task.hpp"
#include <cstdio>

using namespace co;

Task<int> one() { co_return 1; }

Task<long> sum(int n) {
    long s = 0;
    for (int i = 0; i < n; ++i) s += co_await one();      // 1,000,000 awaits, each completes and transfers back
    co_return s;
}

int main() { std::printf("sum = %ld\n", sync_wait(sum(1'000'000))); }
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sum = 1000000
```

Now break the design on purpose. Compile the same program with `-DCO_NO_SYMMETRIC_TRANSFER`, which replaces the tail call with a nested `continuation.resume()`:

```cpp
// @test crash -std=c++23 -O2 -DCO_NO_SYMMETRIC_TRANSFER
#include "task.hpp"
#include <cstdio>

using namespace co;

Task<int> one() { co_return 1; }
Task<long> sum(int n) { long s = 0; for (int i = 0; i < n; ++i) s += co_await one(); co_return s; }

int main() { std::printf("sum = %ld\n", sync_wait(sum(1'000'000))); }
```

The program prints nothing and is killed by `SIGSEGV` (the checker verifies a non-zero exit; by hand it is status 139): every iteration's completion called `parent.resume()` from *inside* the child, which then ran the next iteration, whose child called `resume()` again, so the stack grew by a few frames per iteration until it hit the 8 MiB limit (`ulimit -s`). The `co_await` loop looks flat in the source, but without the tail call it is a recursion a million deep.

**Which compilers do the tail call?** Measured by hand on this machine with a minimal single-file version of the same test (not auto-verified):

```text
                                  symmetric version    NO-symmetric version
g++-14.2    -O0                   SIGSEGV              SIGSEGV
g++-14.2    -O1                   SIGSEGV              SIGSEGV
g++-14.2    -O2                   1000000              SIGSEGV
g++-14.2    -O0 -foptimize-sibling-calls   1000000     (n/a)
g++-14.2    -O1 -foptimize-sibling-calls   1000000     (n/a)
clang++-18  -O0                   1000000              SIGSEGV
clang++-18  -O2                   1000000              SIGSEGV
```

So: the standard **guarantees** the transfer; **GCC 14.2 delivers it only when its sibling-call optimisation is enabled** (so a GCC *Debug* build with deep chains can overflow where the *Release* build does not) while Clang always does. If you ship a library that uses long chains, add `-foptimize-sibling-calls` to your Debug flags for GCC, and keep a deep-chain test in CI at `-O0`.

### Experiment 3 ✅: A run loop with timers: where "async" comes from

Add a scheduler. This is a second shared header: a single-threaded **ready queue + timer heap**. `sleep_for` is an awaiter whose `await_suspend` parks the handle in the timer heap; `run()` resumes ready handles, then sleeps until the next timer is due.

```cpp
// @test file loop.hpp
#pragma once
#include "task.hpp"
#include <chrono>
#include <deque>
#include <queue>
#include <thread>
#include <vector>

namespace co {

class Loop {
public:
    using Clock = std::chrono::steady_clock;

    void post(std::coroutine_handle<> h) { ready_.push_back(h); }

    void spawn(Task<void> t) {                       // the loop owns root tasks and starts them
        auto h = t.handle();
        roots_.push_back(std::move(t));
        post(h);
    }

    auto sleep_for(Clock::duration d) {
        struct Awaiter {
            Loop& loop;
            Clock::time_point when;
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) const { loop.timers_.push({when, loop.seq_++, h}); }
            void await_resume() const noexcept {}
        };
        return Awaiter{*this, Clock::now() + d};
    }

    auto yield() {                                   // let every other ready coroutine run first
        struct Awaiter {
            Loop& loop;
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) const { loop.post(h); }
            void await_resume() const noexcept {}
        };
        return Awaiter{*this};
    }

    void run() {
        for (;;) {
            while (!ready_.empty()) {
                auto h = ready_.front();
                ready_.pop_front();
                h.resume();                          // runs until the coroutine (chain) suspends or finishes
            }
            if (timers_.empty()) break;
            if (timers_.top().when > Clock::now()) std::this_thread::sleep_until(timers_.top().when);
            while (!timers_.empty() && timers_.top().when <= Clock::now()) {
                ready_.push_back(timers_.top().h);
                timers_.pop();
            }
        }
        for (auto& r : roots_) r.get();              // rethrow the first exception that escaped a root task
        roots_.clear();
    }

private:
    struct Timer {
        Clock::time_point when;
        std::uint64_t seq;                           // FIFO among equal deadlines
        std::coroutine_handle<> h;
        bool operator>(const Timer& o) const { return when != o.when ? when > o.when : seq > o.seq; }
    };
    std::deque<std::coroutine_handle<>> ready_;
    std::priority_queue<Timer, std::vector<Timer>, std::greater<>> timers_;
    std::vector<Task<void>> roots_;
    std::uint64_t seq_ = 0;
};

}  // namespace co
```

Three workers with different periods share one thread. `fetch` is a *nested* task that sleeps, which shows resumption travelling up a chain.

```cpp
// @test run -std=c++23 -O0 timeout=60
#include "loop.hpp"
#include <chrono>
#include <cstdio>

using namespace co;
using namespace std::chrono_literals;

Task<int> fetch(Loop& L, int id, std::chrono::milliseconds latency) {      // child: sleeps, then returns
    co_await L.sleep_for(latency);
    co_return id * 10;
}

Task<void> worker(Loop& L, const char* name, std::chrono::milliseconds period, int rounds) {
    for (int i = 1; i <= rounds; ++i) {
        int v = co_await fetch(L, i, period);                              // suspends the whole chain: worker -> fetch -> timer
        std::printf("  %s round %d got %d\n", name, i, v);
    }
    std::printf("  %s finished\n", name);
}

int main() {
    Loop loop;
    loop.spawn(worker(loop, "A", 60ms, 3));            // fires at  60, 120, 180 ms
    loop.spawn(worker(loop, "B", 100ms, 2));           // fires at 100, 200 ms
    loop.spawn(worker(loop, "C", 140ms, 1));           // fires at 140 ms
    std::puts("running three coroutines on ONE thread:");
    loop.run();
    std::puts("all done");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
running three coroutines on ONE thread:
  A round 1 got 10
  B round 1 got 10
  A round 2 got 20
  C round 1 got 10
  C finished
  A round 3 got 30
  A finished
  B round 2 got 20
  B finished
all done
```

Three tasks, one thread, no locks. The interleaving is dictated entirely by the timer heap: when a deadline passes, `run()` resumes the **leaf** (`fetch`'s sleeping frame), which finishes and transfers (symmetric) to `worker`, which prints and starts the next `fetch`. There is no dispatcher inside `Task`; the loop's `h.resume()` is the only place control enters the system. (The workers take references to `Loop` and `const char*` literals: fine, because `loop` outlives them and string literals have static storage; see Chapter 33's reference-parameter rule.)

### Experiment 4 ✅: Hopping threads: `co_await pool.schedule()`

An awaiter can park the handle in *another thread's* queue. The coroutine then continues on that thread.

```cpp
// @test run -std=c++23 -O0 link=-pthread timeout=60
#include "task.hpp"
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

using namespace co;

class Pool {
public:
    Pool() : worker_([this](std::stop_token st) { loop(st); }) {}
    auto schedule() {
        struct Awaiter {
            Pool& pool;
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) const {          // after this, ANOTHER thread may resume h
                std::lock_guard lk(pool.m_);
                pool.q_.push_back(h);
                pool.cv_.notify_one();
            }
            void await_resume() const noexcept {}
        };
        return Awaiter{*this};
    }
    std::thread::id worker_id() const { return worker_.get_id(); }

private:
    void loop(std::stop_token st) {
        std::unique_lock lk(m_);
        while (cv_.wait(lk, st, [&] { return !q_.empty(); })) {
            auto h = q_.front(); q_.pop_front();
            lk.unlock();
            h.resume();                                                    // the coroutine runs on this worker thread
            lk.lock();
        }
    }
    std::mutex m_;
    std::condition_variable_any cv_;
    std::deque<std::coroutine_handle<>> q_;
    std::jthread worker_;                                                  // declared last: joined first on destruction
};

Task<int> compute(Pool& pool, std::thread::id main_id) {
    std::printf("  before hop: on the main thread? %s\n", std::this_thread::get_id() == main_id ? "yes" : "no");
    co_await pool.schedule();
    auto first = std::this_thread::get_id();
    std::printf("  after hop : on the main thread? %s\n", first == main_id ? "yes" : "no");
    co_await pool.schedule();
    std::printf("  second hop: same worker thread? %s\n", std::this_thread::get_id() == first ? "yes" : "no");
    co_return 42;
}

int main() {
    Pool pool;
    int r = sync_wait(compute(pool, std::this_thread::get_id()));
    std::printf("result %d, delivered back to main through sync_wait\n", r);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
  before hop: on the main thread? yes
  after hop : on the main thread? no
  second hop: same worker thread? yes
result 42, delivered back to main through sync_wait
```

`await_suspend` is the **only** place a thread switch happens. After `push_back(h)` returns, the worker may already be running the coroutine, which is why §11 of Chapter 33 says to touch nothing of the awaiter after publishing the handle. Here the Task chain's *final* resumption of `sync_wait`'s `Fire` therefore also happens on the worker thread; `sync_wait` hands the result back to `main` via the mutex and condition variable. This unstructured “continuation runs wherever the last event happened” behaviour is the default for most libraries, and the source of the question *“which thread am I on after `co_await`?”* Chapter 35 returns to it.

### Experiment 5 ✅: Concurrency by composition: `when_all`

```cpp
// @test file when_all.hpp
#pragma once
#include "loop.hpp"
#include <vector>

namespace co {

// Run all tasks concurrently (on the calling scheduler) and complete when every one has finished.
// Single-threaded variant: the counter is not atomic. Rethrows the first exception after ALL tasks finish.
inline Task<void> when_all(std::vector<Task<void>> tasks) {
    struct State {
        std::size_t remaining;
        std::coroutine_handle<> waiter{};
        std::exception_ptr first{};
    } st{tasks.size()};

    auto starter = [](Task<void>& t, State& st) -> Fire {                  // eager: starts the child right now
        try { co_await std::move(t); }
        catch (...) { if (!st.first) st.first = std::current_exception(); }
        if (--st.remaining == 0 && st.waiter) st.waiter.resume();          // last one out wakes the waiter
    };
    for (auto& t : tasks) starter(t, st);

    struct WaitAll {
        State& st;
        bool await_ready() const noexcept { return st.remaining == 0; }    // everything completed synchronously
        void await_suspend(std::coroutine_handle<> h) const noexcept { st.waiter = h; }
        void await_resume() const noexcept {}
    };
    co_await WaitAll{st};
    if (st.first) std::rethrow_exception(st.first);
}

}  // namespace co
```

```cpp
// @test run -std=c++23 -O0 timeout=60
#include "when_all.hpp"
#include <chrono>
#include <cstdio>

using namespace co;
using namespace std::chrono_literals;

Task<void> request(Loop& L, int id) {
    co_await L.sleep_for(80ms);                            // pretend network latency
    std::printf("  request %d completed\n", id);
}

Task<void> sequential(Loop& L) { for (int i = 1; i <= 4; ++i) co_await request(L, i); }

Task<void> concurrent(Loop& L) {
    std::vector<Task<void>> v;
    for (int i = 1; i <= 4; ++i) v.push_back(request(L, i));
    co_await when_all(std::move(v));
}

template <class F> double time_ms(F f) {
    Loop loop;
    auto t0 = Loop::Clock::now();
    loop.spawn(f(loop));
    loop.run();
    return std::chrono::duration<double, std::milli>(Loop::Clock::now() - t0).count();
}

int main() {
    std::puts("sequential:"); double seq = time_ms(sequential);
    std::puts("concurrent:"); double con = time_ms(concurrent);
    std::printf("4 requests of 80 ms: sequential about 4x (%s), concurrent about 1x (%s)\n",
                seq > 300 ? "yes" : "NO", con < 160 ? "yes" : "NO");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sequential:
  request 1 completed
  request 2 completed
  request 3 completed
  request 4 completed
concurrent:
  request 1 completed
  request 2 completed
  request 3 completed
  request 4 completed
4 requests of 80 ms: sequential about 4x (yes), concurrent about 1x (yes)
```

The four requests overlap on **one thread**: `when_all` starts each child with an eager `Fire`, each child suspends in the timer heap, and the *last* finisher resumes the waiter. Wall time falls from the sum to the maximum. This is concurrency without parallelism: the timer queue is the only thing running at the same time. (Honest limitations: the counter is not atomic, so use it only when all children run on the same scheduler thread; and `starter`'s `Fire` calls `waiter.resume()` nested inside the last child's completion, which is fine here but is exactly the kind of re-entrancy a production `when_all` avoids with symmetric transfer.)

### Experiment 6 🔧: What a Task costs: one frame allocation per await

```cpp
// @test run -std=c++23 -O2 -fno-stack-protector timeout=120
#include "task.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>

// A toggleable global allocator: a thread-local free list for blocks up to 128 bytes.
static bool g_pool = false;
static long g_allocs = 0;
struct FreeNode { FreeNode* next; };
static thread_local FreeNode* t_free = nullptr;
constexpr std::size_t kBlock = 128;

void* operator new(std::size_t n) {
    ++g_allocs;
    if (g_pool && n <= kBlock) {
        if (t_free) { auto* p = t_free; t_free = p->next; return p; }
        return std::malloc(kBlock);
    }
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept {
    // blocks from the pool path are kBlock bytes; we cannot tell them apart here, so the toggle is only flipped between phases
    if (g_pool && p) { auto* f = static_cast<FreeNode*>(p); f->next = t_free; t_free = f; }
    else std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

using namespace co;

Task<int> one() { co_return 1; }
Task<long> sum(int n) { long s = 0; for (int i = 0; i < n; ++i) s += co_await one(); co_return s; }

int main() {
    constexpr int N = 5'000'000;
    for (bool pool : {false, true}) {
        g_pool = pool; g_allocs = 0;
        auto t0 = std::chrono::steady_clock::now();
        long s = sync_wait(sum(N));
        double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N;
        std::printf("%-22s %6.1f ns per co_await   (sum=%ld, operator new calls: %ld)\n",
                    pool ? "free-list allocator" : "malloc", ns, s, g_allocs);
        g_pool = false;
    }
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
malloc                   28.6 ns per co_await   (sum=5000000, operator new calls: 5000002)
free-list allocator      15.2 ns per co_await   (sum=5000000, operator new calls: 5000002)
```

Every `co_await one()` creates, runs and destroys a child frame, so there is **one `operator new`/`delete` pair per await**: the counter reads N + 2 (the two extras are the `sum` frame and `sync_wait`'s `Fire`). On this VM an await of a trivial task costs about **42 ns with `malloc` and 29 ns with the free list**. Two lessons:

- The allocator is a real, removable cost (about 13 ns, roughly a third), and a promise-level `operator new` (Exercise 3) gets it without replacing the global allocator.
- But **most of the cost is not the allocation**. The remaining ~29 ns is the ramp function (frame setup, promise construction including the `variant`), two symmetric transfers, the result move, and destroying the frame. Compare Chapter 33's bare generator at about 2 ns per suspend/resume: creating and completing a *task* is roughly 15x more expensive than resuming an existing coroutine. So do not `co_await` trivially small tasks in a hot loop; await a coroutine that does real work, or call a plain function. (Clang's frame elision would remove both the allocation and much of this overhead when the child is visible and inlined; measure with `clang++-18`.)

The global-allocator toggle used here is only safe because the flag flips between phases while no frames are live; do not do this in real code.

---

## 8. Assembly / runtime investigation

```bash
# 1. Is symmetric transfer a real tail call?  Look at the final-suspend awaiter in the resume function
g++-14 -std=c++23 -O2 -S -o - prog.cpp | c++filt | grep -B2 -A12 'FinalAwaiter'
#    a tail call ends with `jmp *%rax` (indirect jump), a nested resume ends with `call *%rax` followed by `ret`
objdump -d --no-show-raw-insn -C a.out | grep -A40 'sum(int).*resume\|sum(int)\.resume' | grep -E 'jmp|call'

# 2. Compare -O0 and -O2 on GCC: the jmp becomes a call at -O0 (Experiment 2)
for o in -O0 -O2; do g++-14 -std=c++23 $o -c prog.cpp -o p$o.o && objdump -d --no-show-raw-insn -C p$o.o | grep -c 'call.*\*'; done

# 3. Stack depth at the crash: how deep did the recursion go?
gdb -q ./a.out -ex run -ex 'bt -5' -ex 'bt -frame-info short' 2>&1 | tail -12   # the same few frames repeated thousands of times
ulimit -s                                  # 8192 KiB by default; each nested level costs a few dozen bytes to a few hundred

# 4. Count frame allocations (HALO or not)
ltrace -c -e malloc+free ./a.out 2>&1 | tail -4
valgrind --tool=massif --pages-as-heap=no ./a.out && ms_print massif.out.* | head -30

# 5. What does await_suspend do on the CPU?  perf annotate the resume function of the leaf
perf record -e cycles ./a.out && perf annotate --stdio -s 'sum(int)'
```

---

## 9. Implementation exercise

Extend `Task` yourself, each step verified with its own test and ASan:

1. **`Task<T&>` and move-only `T`**: result storage for reference results (store a pointer) and for `std::unique_ptr<X>`.
2. **`when_any`**: complete with the index and result of the first finisher; what happens to the losers (cancellation)?
3. **`promise_type::operator new`/`delete`** backed by a thread-local size-class pool; measure with Experiment 6.
4. **`resume_on(Executor&)`**: an awaiter that makes the *continuation* run on a given executor, so `co_await task` always resumes on the awaiter's own executor (scheduler affinity).
5. **Cancellation**: thread a `std::stop_token` through the promise; make `sleep_for` and the pool awaiter cancel-aware.

<details>
<summary><strong>Solution sketch: a `Task` that can be cancelled through a stop token</strong></summary>

The smallest useful cancellation design: the awaiter *checks the token before suspending* and the timer wakes early when stop is requested. Cancelled tasks complete by throwing `Cancelled` from the awaiter's `await_resume`, so ordinary exception propagation unwinds the whole chain and runs every destructor.

```cpp
// @test run -std=c++23 -O0 timeout=60
#include "loop.hpp"
#include <cstdio>
#include <stop_token>

using namespace co;
using namespace std::chrono_literals;

struct Cancelled {};

// A sleep that can be interrupted: the loop's timer fires at the deadline; on resume we check the token.
// (A real implementation also removes the timer from the heap when stop is requested, via std::stop_callback.)
Task<void> sleep_cancellable(Loop& L, std::chrono::milliseconds d, std::stop_token st) {
    co_await L.sleep_for(d);
    if (st.stop_requested()) throw Cancelled{};
}

struct Guard { const char* n; ~Guard() { std::printf("  ~Guard(%s) runs during unwinding\n", n); } };

Task<void> job(Loop& L, std::stop_token st) {
    Guard g{"job"};
    try {
        for (int i = 1; i <= 5; ++i) {
            co_await sleep_cancellable(L, 40ms, st);
            std::printf("  job tick %d\n", i);
        }
    } catch (const Cancelled&) { std::puts("  job: cancelled"); }
}

Task<void> canceller(Loop& L, std::stop_source& src) {
    co_await L.sleep_for(100ms);
    std::puts("  canceller: requesting stop");
    src.request_stop();
}

int main() {
    Loop loop;
    std::stop_source src;
    loop.spawn(job(loop, src.get_token()));
    loop.spawn(canceller(loop, src));
    loop.run();
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
  job tick 1
  job tick 2
  canceller: requesting stop
  job: cancelled
  ~Guard(job) runs during unwinding
```

The job ticks at 40 ms and 80 ms; the stop request arrives at 100 ms; the pending 120 ms sleep ends, notices the token, and the exception unwinds `job`'s frame. Note the limitation: cancellation latency is bounded by the sleep that was already in flight (up to 40 ms here). Making the awaiter itself wake on `stop_requested()` needs a `std::stop_callback` that removes the timer and re-posts the handle (Exercise 5).

</details>

---

## 10. Real-world example

| Where | How its `Task` differs from ours |
|---|---|
| **cppcoro** (Lewis Baker) | The reference design: `task<T>`, `shared_task`, `sync_wait`, `when_all`, `static_thread_pool`, `cancellation_token`; ours is a simplified cppcoro |
| **folly::coro** | `Task<T>` with executors, cancellation tokens, and **scheduler affinity** (each `co_await` resumes on the awaiter's executor), plus a frame pool |
| **Boost.Asio `awaitable<T>`** | Tied to an `io_context`/executor; `co_spawn`, `use_awaitable`; completion tokens turn any callback API into an awaitable |
| **libunifex / stdexec** | The senders/receivers world; `task` is the coroutine adapter on top (the C++26 direction) |
| **Qt: QCoro** | `QCoro::Task<T>` over the Qt event loop; `co_await` a `QNetworkReply`, signal, `QTimer`; resumes on the Qt thread because the awaiter hooks the signal (Chapter 48) |
| **Seastar** | Futures first, coroutines layered on; shard-per-core so no atomics on task state |
| **Rust `async fn`** | The same stackless-state-machine idea, but the *poll* model (pull) rather than C++'s resume model (push); `Future::poll` returns `Pending` and a `Waker` re-schedules |

> **Opinion.** A task type is a *library design*, not a technique to scatter around a codebase: write it once (or take one from cppcoro / folly / Asio), test it hard, and make everyone else's `co_await` boring. If you roll your own, the three things to get right on day one are **symmetric transfer, exception transport and single ownership**; the three to get right before you scale are **cancellation, executor affinity and a frame allocator**. Prefer **lazy** tasks: eager tasks start before you can attach lifetime or error handling and are the usual source of "my coroutine ran on the wrong thread".

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| **Non-symmetric `final_suspend`** (calls `continuation.resume()`) | Stack overflow on long await loops (Experiment 2) | Return the continuation handle from `await_suspend` |
| **GCC Debug build, deep chain** | SIGSEGV at `-O0`/`-O1` although Release works | `-foptimize-sibling-calls`, or restructure to bound the depth; test at `-O0` |
| **Dropping a `Task` that is currently running** | `destroy()` on an executing frame | Never destroy a started task before it finishes; await it, or use an explicit cancellation protocol |
| **Starting a lazy `Task` and never awaiting it** | Nothing runs; the `[[nodiscard]]` warning is the only hint | Keep `[[nodiscard]]`; spawn on a loop or `sync_wait` |
| **Reference parameters / lambda captures** | Dangling after the caller returns (Chapter 33) | Pass by value; `Loop&`-style references only when the referent clearly outlives the task |
| **`co_await` on an lvalue `Task`** | Compile error (by design) | `co_await std::move(t)` |
| **Forgetting `co_return`** in a `Task<T>` | UB (falling off the end of a value-returning coroutine) | Always `co_return`; `-Wreturn-type`; `-Werror` |
| **Exception swallowed in a fire-and-forget** | `std::terminate` in `Fire::unhandled_exception` | Wrap with a handler that logs or forwards the error |
| **Racing `await_suspend`** (publish the handle, then touch the awaiter) | Use-after-free when another thread resumes and finishes the frame | Write everything first, publish last, touch nothing after |
| **Assuming resume-on-the-awaiter's-thread** | Data race: continuation runs on the worker that completed the child | Add an affinity awaiter, or make shared state thread-safe |
| **Non-atomic shared state in `when_all` with multithreaded children** | Lost updates, double resume | Atomic counter, and ensure exactly one resumer |
| **Unbounded memory: millions of in-flight tasks** | RSS = N × frame size | Bound concurrency (semaphore/limiter); frame pool |
| **Task stored in a container that moves while the loop holds its handle** | none for handles (frames don't move), but *use after destroy* if the container drops the Task | Let the loop own roots (Experiment 3) until completion |

---

## 12. Exercises

1. **Trace it.** Add a `printf` to every promise hook, `await_suspend`, `await_resume`, and destructor of `Task`, run Experiment 1, and annotate the output against the 7-step diagram.
2. **Prove the depth.** Measure the maximum chain depth that survives with `CO_NO_SYMMETRIC_TRANSFER` at `-O0` and `-O2` (binary search on `n`), and compute bytes per level from `ulimit -s`.
3. **Frame pool.** Implement `PromiseBase::operator new/delete` over a thread-local free list with size classes 32…256; re-run Experiment 6 and report ns per await.
4. **Eager variant.** Make `Task` eager (`suspend_never` initial) and add a synchronisation-safe result handoff (atomic state: *created / running / suspended-with-awaiter / done*). Show the race you must close when a child finishes on another thread at the same moment the parent calls `co_await`.
5. **`shared_task`.** Allow many awaiters on one result: store a list of continuations, resume all at completion, ref-count the frame.
6. **Executor affinity.** Write `Task` so that after `co_await x`, execution resumes on the executor that was current when `co_await` began. Measure the cost of the extra hop.
7. **Timers with cancellation.** Rework `Loop::sleep_for` to use a `std::stop_callback` that removes the timer and re-posts the handle immediately.
8. **Compare with `std::future`.** Write the same “4 requests” scenario with `std::async` and with Experiment 5; compare threads, memory, and code.

---

## 13. Challenge: structured concurrency

Implement a **nursery / `task_scope`** (Trio / `async_scope` style):

```cpp
Task<void> serve(Loop& L) {
    co_await with_scope([&](Scope& s) -> Task<void> {
        s.spawn(handle_connection(L, 1));
        s.spawn(handle_connection(L, 2));
        co_return;                    // the scope does NOT complete until both children have
    });                               // exceptions in a child cancel the siblings and propagate here
}
```

Requirements: (a) children can never outlive the scope (no dangling references); (b) the first child failure requests stop on a shared `stop_source` and the scope rethrows it after **all** children have finished; (c) no leaked frames (ASan); (d) a deterministic test harness using the `Loop` with fake time. Then write a short note on why “structured” beats `detach()` in review.

---

## 14. Knowledge check

1. Why is `Task` lazy here? What goes wrong with an eager task whose caller hasn't yet attached an error handler?
2. Draw the sequence of calls for `co_await child()` from the moment the parent evaluates `child()` until it receives the value. Where does the continuation get stored, and who runs it?
3. What does symmetric transfer change about `FinalAwaiter::await_suspend`, and what exactly would happen to the stack without it?
4. Why must `final_suspend` suspend rather than let the frame fall off the end? Who destroys the frame and when?
5. Why does `co_await some_task;` (lvalue) not compile, and why is that good?
6. Where does an exception thrown inside the child end up, and which function rethrows it?
7. Why can `co_await child();` keep the child frame alive without a named variable?
8. `Task` has no scheduler. Name two places “asynchrony” actually enters and what each does with the handle.
9. After `co_await pool.schedule()`, which thread executes the next statement? Which thread resumes the *parent* of this coroutine when it finishes?
10. What is wrong with touching `this` in `await_suspend` after pushing the handle to another thread's queue?
11. GCC 14.2 and Clang 18 behave differently at `-O0` in Experiment 2. How and why? What do you do about it?
12. List four properties of a production task type that our version lacks.

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Lazy start gives the caller control of when and where work begins; nothing runs until awaited or spawned. An eager task may run (and throw) before the caller has set up exception handling, cancellation, or the scheduler context, and may start on the wrong thread.
2. `child()` creates a suspended frame → parent's awaiter `await_ready` (false) → `await_suspend(parent_h)` stores `parent_h` in `child.promise.continuation` and returns the child's handle (tail call) → child runs → at its end `FinalAwaiter::await_suspend` returns the continuation (tail call) → parent's `await_resume` calls `take()` and returns the value/rethrows. The continuation is run by the child's final awaiter returning it.
3. Without it the final awaiter would call `continuation.resume()` while still inside the child's frame, so the parent's next iteration would run *on top of* the child's stack frame; in a loop this nests once per iteration → stack overflow. With symmetric transfer the returned handle is resumed by a tail call that replaces the current frame.
4. So the owner can read the result stored in the promise after completion; if the frame vanished at the end the result would be gone. `Task`'s destructor (`reset()`) calls `destroy()`; for `co_await child();` that is at the end of the full-expression, after `await_resume`.
5. `operator co_await` is `&&`-qualified: awaiting consumes the one-shot task. Requiring `std::move` makes ownership transfer visible and prevents awaiting the same task twice.
6. `unhandled_exception()` in the promise stores it as an `exception_ptr`; `await_resume()` → `take()` rethrows it in the awaiting coroutine.
7. The temporary `Task` lives until the end of the full-expression, which includes the whole suspension and `await_resume`, and its destructor destroys the frame afterwards.
8. In an awaiter's `await_suspend` that parks the handle: the loop's timer heap (resumed by `run()` when the deadline passes) and the pool's queue (resumed by the worker thread); in Chapter 35, an `epoll` registration (resumed when the fd is ready).
9. The pool's worker thread. The child's final awaiter returns the parent's handle, so the parent resumes on that same worker thread (the one that finished the child), unless something hops back.
10. After the handle is published another thread may resume the coroutine, which may complete and destroy the frame; the awaiter lives in the frame, so `this` can dangle (use-after-free / data race).
11. Clang transfers control with a real tail call even at `-O0`; GCC 14.2 only does so when sibling-call optimisation is on, so at `-O0`/`-O1` the "symmetric" version still nests and overflows on deep chains. Build Debug with `-foptimize-sibling-calls` (or avoid deep chains) and keep an `-O0` deep-chain test.
12. Cancellation, executor/scheduler affinity, a frame allocator/pool, thread-safe (atomic) state for eager or shared tasks, `Task<T&>`, `when_any`, better diagnostics (stack traces of async chains), and `noexcept`/error-code integration.

</details>

---

[← Previous: Chapter 33](33-coroutines.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 35 — Coroutines and networking →](35-coroutines-and-networking.md)
