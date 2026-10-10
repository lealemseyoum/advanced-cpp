# Chapter 33 — C++20 Coroutines: The Machinery

> **Part XIII · Coroutines** &nbsp;|&nbsp; **Level 4** (compiler/runtime) &nbsp;|&nbsp; **≈ 10 hours**
> **Prerequisites:** [Chapter 2–3 (lifetime)](../part-02-object-model-and-lifetime/), [Chapter 6 (move semantics)](../part-03-value-categories-and-move/), [Chapter 24 (dynamic memory)](../part-10-memory/24-dynamic-memory.md) &nbsp;|&nbsp; **Standards:** C++20 `<coroutine>` ⚖️; `std::generator` is C++23 ⚖️ (libstdc++ 14 ✅); `std::execution` / `std::task` are C++26 🟡 (not in GCC 14.2) &nbsp;|&nbsp; **Tools:** `g++-14`, `clang++-18`, ASan

[← Previous: Chapter 32](../part-12-concurrency/32-lock-free.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 34 — Building a coroutine type →](34-building-task.md)

---

**In one sentence:** a C++ coroutine is an ordinary function that the compiler rewrites around a heap-allocatable **frame** (the saved locals and a resume point), a **promise object** (your customisation hooks), and a **handle** (a pointer to the frame), and every `co_await` is just a call to three functions on an object you choose.

**By the end of this chapter you can:**

- say what a coroutine *is* at the machine level: what is in the frame, where it lives, and when it is allocated and freed
- expand any coroutine by hand into the promise/awaiter calls the compiler generates, in the right order
- write promise types, awaiters and the three forms of `await_suspend`
- predict when the frame allocation can be elided (HALO) and measure it
- recognise the four lifetime traps that cause most real coroutine bugs

> **Not "async functions".** Coroutines in C++ are a *mechanism*, not a feature with built-in scheduling, futures or threads. There is no `async` runtime in the standard (until `std::execution` arrives, 🟡). The language gives you suspension and resumption; **you** supply everything else. This chapter is the mechanism; Chapter 34 builds `Task<T>`; Chapter 35 adds `epoll`.

---

## 1. Problem

You want to write sequential-looking code that **pauses** in the middle and continues later, without dedicating a thread to it. Examples: a generator that yields values on demand, a parser that consumes a byte stream a chunk at a time, a server handler that waits for a socket to become readable.

Without language support, you hand-write a **state machine**:

```cpp
// A "read a length-prefixed message" step, as a callback state machine
struct Reader {
    enum { WantLen, WantBody } state = WantLen;
    uint32_t len; std::string body;
    void on_readable(int fd) {
        switch (state) {
        case WantLen:  if (!read_exact(fd, &len, 4)) return; state = WantBody; [[fallthrough]];
        case WantBody: if (!read_exact(fd, body.data(), len)) return; deliver(body); state = WantLen; }
    }
};
```

Every local that lives across a wait becomes a member; every wait becomes a `case`; loops and error handling turn inside out. The three standard alternatives all cost something:

| Approach | Cost |
|---|---|
| **Thread per task**, blocking calls | ~8 MiB virtual stack, ~70 µs to create (Chapter 29), OS scheduling; unusable for 100 000 connections |
| **Callbacks / continuations** | Control flow is fragmented; locals escape into heap closures; "callback hell"; error handling scattered |
| **Hand-written state machine** | Fast, but unreadable and hard to change (what you saw above) |

Coroutines let the **compiler** write the state machine from straight-line code.

---

## 2. Historical context

| Year | Event |
|---|---|
| 1958–63 | Conway coins "coroutine"; Simula 67 and later Modula-2 make them first-class |
| 1983 | Duff’s device and `switch`-based “protothreads” (C hackery) emulate stackless coroutines |
| 2000s | Boost.Coroutine / `ucontext` / Boost.Context: **stackful** coroutines by swapping stack pointers (each needs its own stack) |
| 2012 | C# `async`/`await` (stackless, compiler state machine) popularises the model |
| 2014–17 | C++ **Coroutines TS**: Gor Nishanov's stackless design, built on LLVM `coro` intrinsics; Microsoft's `resumable functions` merged in |
| **2020** | **C++20** standardises stackless coroutines: `co_await`, `co_yield`, `co_return`, `<coroutine>` |
| 2022 | C++23 adds `std::generator` (P2502) ⚖️ |
| **2024–26** | `std::execution` (senders/receivers, P2300) and `std::task` (P3552) adopted for **C++26** 🟡; GCC 14.2 / libstdc++ 14 ship neither |

**Stackful vs stackless** is the key design decision. A stackful coroutine (Boost.Context, Go goroutines, Lua) owns a whole stack and can suspend from any depth. A **stackless** one (C++20, C#, Rust, Python generators) saves only *its own* locals in a fixed-size frame and can suspend **only at its own `co_await` points**: callees must themselves be coroutines to suspend. The trade: tiny, predictable memory and the possibility of the compiler optimising the frame away, against the "function colouring" problem.

---

## 3. Modern solution

Three keywords and a library header:

```cpp
#include <coroutine>
#include <utility>

Gen<int> iota(int n) {            // return type Gen<int> => its nested promise_type controls this coroutine
    for (int i = 0; i < n; ++i)
        co_yield i;               // suspend, handing i to the caller
}                                 // falling off the end = co_return;

Task<int> add_later() {
    int x = co_await read_int();  // suspend until the awaitable completes
    co_return x + 1;              // store the result in the promise, finish
}
```

A function is a coroutine **iff its body contains `co_await`, `co_yield` or `co_return`**. Nothing in the signature says so; the return type must provide a `promise_type`, and the compiler wires everything else.

---

## 4. Mental model

### The three objects

```text
        caller's stack                                heap (usually)
   ┌───────────────────────┐             ┌──────────────────────────────────────────┐
   │ Gen<int> g            │             │              COROUTINE FRAME             │
   │   handle ─────────────┼───────────► │  resume fn ptr │ destroy fn ptr           │
   └───────────────────────┘             │  promise_type  (your hooks and results)   │
                                         │  copies of parameters                     │
                                         │  locals that live across a suspension     │
                                         │  suspend-point index (the "state")        │
                                         │  temporaries / awaiters alive across waits│
                                         └──────────────────────────────────────────┘
```

| Object | Who owns / writes it | Role |
|---|---|---|
| **Frame** | compiler-generated; allocated with `operator new` (or the promise's) | the saved function activation |
| **Promise** (`promise_type`) | *you* write it; lives inside the frame | policy: what happens at start/end, what `co_return`/`co_yield` do, what exceptions do |
| **Handle** (`std::coroutine_handle<P>`) | a *non-owning* pointer to the frame, one machine word | `resume()`, `destroy()`, `done()`, `promise()` |
| **Return object** (`Gen<int>`) | you write it; built by `promise.get_return_object()` and given to the caller | the RAII owner the *caller* holds (it usually calls `destroy()`) |
| **Awaiter** | any type with `await_ready/await_suspend/await_resume` | policy for one suspension point |

A `coroutine_handle` is a **raw, non-owning pointer** with the ownership problems of `T*`. All safety comes from the wrapper you build around it (Chapter 34).

### The compiler's rewrite (the model to memorise)

For `R f(Args...) { body }` with `P = coroutine_traits<R, Args...>::promise_type`:

```text
R f(args) {
    frame = operator new(sizeof(frame))             // (1) may be elided; may use P::operator new
    frame.params   = copies/moves of args           //     by-value params are COPIED into the frame; references stay references
    P& promise     = frame.promise (constructed)    // (2) P(args...) if such a ctor exists, else P()
    R  ret         = promise.get_return_object()    // (3) the object that will be returned to the caller
    try {
        co_await promise.initial_suspend();         // (4) suspend now (lazy start) or keep running (eager start)
        body ...                                    //     co_await / co_yield / co_return expand as below
    } catch (...) { promise.unhandled_exception(); }
  final_suspend_point:
    co_await promise.final_suspend();               // (5) noexcept; usually suspends so the owner can read the result
    // if we get here (final_suspend didn't suspend), the frame is destroyed automatically
    return ret;       // (conceptually: ret is returned to the caller at the FIRST suspension, not at the end)
}
```

and the three keywords:

```text
co_return v;   ≡  promise.return_value(v);   goto final_suspend_point;     // or return_void() for `co_return;` and falling off the end
co_yield  v;   ≡  co_await promise.yield_value(v);
co_await  e;   ≡  see the next diagram
```

### What `co_await e` does

```text
  awaiter = get_awaiter(e)         // e itself; or promise.await_transform(e); or e.operator co_await()
  if ( !awaiter.await_ready() ) {                          // "can I continue without waiting?"
        <save locals + resume point in the frame>          // the coroutine is now SUSPENDED
        r = awaiter.await_suspend(handle_to_this_coroutine);
        //   void   -> stay suspended, return to caller/resumer
        //   bool   -> true: stay suspended;  false: resume immediately
        //   handle -> stay suspended, then resume THAT coroutine (symmetric transfer, tail call)
        return to caller/resumer
     ── later: somebody calls handle.resume() ──
  }
  result = awaiter.await_resume();  // value of the whole `co_await` expression
```

**The whole library-level design of C++ coroutines lives in these three functions and in `await_suspend`'s freedom to do *anything*, including handing the handle to another thread or an `epoll` loop.** Note the one subtle point: by the time `await_suspend` runs, the coroutine is already counted as suspended, so another thread may legally resume (or even destroy) it *while `await_suspend` is still executing*.

---

## 5. Language rules

| Topic | Rule |
|---|---|
| **What makes a coroutine** | Body contains `co_await`, `co_yield` or `co_return` (outside unevaluated operands). Not allowed as coroutines: `constexpr`/`consteval` functions, constructors, destructors, `main`, functions with a deduced (`auto`) return type, functions with C-style `...` varargs; and a coroutine body may not contain a plain `return` statement (use `co_return`). |
| **Promise lookup** | `std::coroutine_traits<R, Args...>::promise_type`, which defaults to `R::promise_type`. Specialising `coroutine_traits` lets you make a coroutine out of a return type you don't own (`std::future<int>`). |
| **Required promise members** | `get_return_object()`, `initial_suspend()`, `final_suspend() noexcept`, `unhandled_exception()`, and **exactly one family** of `return_void()` *or* `return_value(T)`. |
| **Parameters** | Copied (moved) into the frame **by their declared type**: a `std::string` parameter is copied into the frame; a `const std::string&` parameter stays a *reference to the caller's object*, and dangles if the caller's object dies before the coroutine finishes (see §11). |
| **Allocation** | `operator new` is looked up first in the promise class (with the parameter list prepended by size, then without), else global. Compilers **may elide** the allocation if the frame's lifetime is provably nested within the caller. If allocation can fail non-throwingly, `P::get_return_object_on_allocation_failure()` is used. |
| **Frame destruction** | Via `handle.destroy()` (explicit), or automatically if the coroutine runs off the end of `final_suspend` *without* suspending. Calling `destroy()` on a coroutine suspended at a point where live locals exist destroys them in reverse order. |
| **Resuming** | `resume()` is UB if the coroutine is not suspended, or is suspended at the final suspend point (`done()` is true). |
| **Flowing off the end** | Without `return_void()` it is **undefined behaviour** (the compiler treats the end as `co_return;`). Forgetting `co_return` in a value-returning coroutine is a classic bug. |
| **Exceptions** | An exception escaping the body is caught and passed to `promise.unhandled_exception()` (which can `std::current_exception()`, rethrow, or terminate). An exception from `initial_suspend`'s `await_resume`/`await_ready` is *also* handled there; from `final_suspend` it is not allowed (`noexcept`). |
| **`await_suspend` return types** | `void`, `bool`, or `std::coroutine_handle<>`. The handle form is a guaranteed tail call (symmetric transfer): the stack does not grow. |
| **Symmetric transfer** | Mandated by the standard: resuming the returned handle must not consume stack space ("as if" by `noreturn`/tail call). Required for deep `co_await` chains. |
| **Where `co_await` is not allowed** | In unevaluated operands (`sizeof`, `decltype`, `noexcept`), in default arguments, and in a function that is not a coroutine. It *is* allowed in `catch` handlers (C++20) and inside nested expressions; `co_await` inside a lambda makes the *lambda's* call operator the coroutine, not the enclosing function. |

### Layer check

| Layer | Question | Answer |
|---|---|---|
| **C++ standard** | What's specified? | The transformation, the hooks, the lifetime of the frame, symmetric transfer, "may elide the allocation" |
| **Compiler** | Layout and optimisation | GCC and Clang each pick a frame layout (resume/destroy function pointers at the start, an index, promise, spilled locals); Clang runs LLVM's `CoroSplit`/`CoroElide` passes; GCC does the split in its front end and elides less aggressively |
| **ABI** | Is the frame layout standard? | **No.** Not in the Itanium ABI. A `coroutine_handle` is a `void*` but you cannot portably inspect or resume a frame built by another compiler. Don't ship coroutine frames across a shared-library boundary built by different compilers |
| **OS** | Memory | Plain heap `operator new`; no stack switching, no `mmap` per coroutine; frames are cache-friendly small objects |
| **CPU** | Cost of a switch | A suspend/resume pair is an indirect call plus a few spills: typically 2–10 ns, no kernel entry, no TLB or stack swap |

---

## 6. Implementation model

How GCC and Clang compile a coroutine:

1. **Front end** turns the body into a *ramp function* (what the caller calls: allocate frame, construct promise, call `get_return_object`, run to the first suspension point) and a *resume function* and a *destroy function* operating on the frame.
2. The frame starts with **two function pointers** (resume, destroy) followed by the promise and the spilled state. `handle.resume()` is, in effect, `frame->resume_fn(frame)`; `done()` tests whether `resume_fn` has been set to null at the final suspend point; `destroy()` is `frame->destroy_fn(frame)`. (GCC/Clang both use this scheme but it is not guaranteed by the standard.)
3. A **suspend index** (small integer) says which `co_await` the coroutine is parked at; the resume function is one big `switch` on it, which is why a coroutine is exactly the "state machine" of §1 written by the compiler.
4. **What is spilled into the frame:** parameters and any variables whose lifetime spans a suspension point. Locals that die before the next suspension stay in registers or on the real stack of whoever is currently running the coroutine. This is why the frame size is not simply "sum of locals".
5. **HALO** (Heap Allocation eLision Optimisation): if the compiler can see the coroutine is created, resumed and destroyed all within one caller and the frame doesn't escape, it replaces `operator new` with a stack allocation (an `alloca`-like slot in the caller's frame) and often inlines the whole thing. In our test Clang 18 elided the frame at `-O2` and GCC 14.2 did not (Experiment 3).
6. Each `co_await` is **inlined** to the three calls when the types are visible; trivial awaiters like `std::suspend_always` disappear completely.

> **Opinion.** Coroutine frames are *not free*: each live one is a heap object, so 1 million concurrent coroutines at 100 bytes is 100 MiB, and a custom frame allocator (arena or pool, Chapter 26 via `promise_type::operator new`) is the cheapest performance win you can add to a coroutine-heavy runtime. But they are *much* cheaper than 1 million threads (≈ 8 GiB of stack reservation), which is why they exist.

---

## 7. Experiments

### Experiment 1 ✅: Trace the whole lifecycle

Every hook prints, so you see the exact order the compiler uses.

```cpp
// @test run -std=c++23 -O0
#include <coroutine>
#include <utility>
#include <cstdio>
#include <cstdlib>

struct Co {
    struct promise_type {
        promise_type()  { std::puts("    promise ctor"); }
        ~promise_type() { std::puts("    promise dtor"); }
        static void* operator new(std::size_t n)               { std::printf("    operator new(%zu)\n", n); return std::malloc(n); }
        static void  operator delete(void* p, std::size_t n)   { std::printf("    operator delete(%zu)\n", n); std::free(p); }
        Co get_return_object() { std::puts("    get_return_object"); return Co{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { std::puts("    initial_suspend"); return {}; }
        std::suspend_always final_suspend()   noexcept { std::puts("    final_suspend"); return {}; }
        void return_void()           { std::puts("    return_void"); }
        void unhandled_exception()   { std::puts("    unhandled_exception"); }
    };
    explicit Co(std::coroutine_handle<promise_type> h) : h(h) {}
    Co(Co&& o) noexcept : h(std::exchange(o.h, {})) {}
    ~Co() { if (h) { std::puts("  ~Co: handle.destroy()"); h.destroy(); } }
    std::coroutine_handle<promise_type> h;
};

struct Noisy { Noisy() { std::puts("    Noisy ctor"); } ~Noisy() { std::puts("    Noisy dtor"); } };

Co f(int x) {
    std::puts("    body: start");
    Noisy n;
    co_await std::suspend_always{};
    std::printf("    body: resumed, x=%d\n", x);
}                                                   // falls off the end: return_void(), then final_suspend

int main() {
    std::puts("main: call f(7)");
    Co c = f(7);
    std::puts("main: back from f; resume #1");
    c.h.resume();
    std::printf("main: back from resume #1; done=%d; resume #2\n", c.h.done());
    c.h.resume();
    std::printf("main: back from resume #2; done=%d\n", c.h.done());
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
main: call f(7)
    operator new(48)
    promise ctor
    get_return_object
    initial_suspend
main: back from f; resume #1
    body: start
    Noisy ctor
main: back from resume #1; done=0; resume #2
    body: resumed, x=7
    Noisy dtor
    return_void
    final_suspend
main: back from resume #2; done=1
  ~Co: handle.destroy()
    promise dtor
    operator delete(48)
```

Read the output top to bottom against the §4 rewrite: `operator new` → promise constructed → `get_return_object` → `initial_suspend` (we suspend, so control returns to `main` **before the body runs**) → first `resume()` runs the body to the `co_await` → second `resume()` finishes the body, destroys `Noisy`, calls `return_void` and `final_suspend`, and parks at the final suspend point with `done() == true`. The frame is only freed when `main`'s `Co` destructor calls `destroy()`: the promise destructor and `operator delete` run *then*, not at `final_suspend`.

### Experiment 2 🔧: How big is a frame? What goes in it?

```cpp
// @test run -std=c++23 -O2 -fno-stack-protector
#include <coroutine>
#include <string_view>
#include <utility>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static std::size_t g_last = 0;

struct Co {
    struct promise_type {
        static void* operator new(std::size_t n) { g_last = n; return std::malloc(n); }
        static void  operator delete(void* p)    { std::free(p); }
        Co get_return_object() { return Co{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::abort(); }
    };
    explicit Co(std::coroutine_handle<promise_type> h) : h(h) {}
    Co(Co&& o) noexcept : h(std::exchange(o.h, {})) {}
    ~Co() { if (h) h.destroy(); }
    std::coroutine_handle<promise_type> h;
};

[[gnu::noinline]] void sink(const void* p) { asm volatile("" :: "r"(p) : "memory"); }

Co empty()                              { co_return; }
Co one_int(int x)                       { co_await std::suspend_always{}; sink(&x); }
Co five_ints(int a, int b, int c, int d, int e) { co_await std::suspend_always{}; sink(&a); sink(&b); sink(&c); sink(&d); sink(&e); }
Co big_live_local()                     { char buf[256]; std::memset(buf, 1, sizeof buf); co_await std::suspend_always{}; sink(buf); }
Co big_dead_local()                     { { char buf[256]; std::memset(buf, 1, sizeof buf); sink(buf); } co_await std::suspend_always{}; }
Co by_value_string(std::string_view s, long a[4]) { co_await std::suspend_always{}; sink(&s); sink(a); }

int main() {
    long arr[4] = {};
    empty();                          std::printf("no params, no locals ......... %zu bytes\n", g_last);
    one_int(1);                       std::printf("one int parameter ........... %zu bytes\n", g_last);
    five_ints(1, 2, 3, 4, 5);         std::printf("five int parameters ......... %zu bytes\n", g_last);
    big_live_local();                 std::printf("char[256] live across co_await  %zu bytes\n", g_last);
    big_dead_local();                 std::printf("char[256] dead before co_await  %zu bytes\n", g_last);
    by_value_string({}, arr);         std::printf("string_view + pointer params  %zu bytes\n", g_last);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
no params, no locals ......... 40 bytes
one int parameter ........... 48 bytes
five int parameters ......... 64 bytes
char[256] live across co_await  296 bytes
char[256] dead before co_await  296 bytes
string_view + pointer params  64 bytes
```

(The `Co` temporaries returned by each call are destroyed at the end of the full-expression, which frees the frame; `g_last` records the size requested.) The same file with other compilers (run by hand; not auto-verified):

```text
                                   g++-14 -O0..-O3   clang++-18 -O0   clang++-18 -O2
no params, no locals ........            40               24                 0 *
one int parameter ...........            48               32                 0 *
five int parameters .........            64               48                 0 *
char[256] live across co_await           296              288                 0 *
char[256] dead before co_await           296               24                 0 *
string_view + pointer params .            64               48                 0 *

* 0 = operator new was never called: Clang elided the frame (HALO, next experiment).
```

Lessons:

- Even an empty coroutine has a **fixed header**: two function pointers (resume, destroy), the promise, and a suspend index. That is 40 bytes on GCC 14 and 24 on Clang 18 here, so the header is a compiler choice (🔧), not a standard quantity.
- **Parameters are copied into the frame**: each `int` adds 4 to 8 bytes, a `string_view` 16.
- **Whether a dead local occupies the frame is up to the optimiser.** Clang keeps `char buf[256]` out of the frame when it is dead before the suspension (24 bytes); GCC 14 puts it in the frame regardless (296), even at `-O3`. Do not assume "locals not live across a suspension cost nothing"; measure.
- **Never hard-code frame sizes.** Look at them (or, better, give the promise an `operator new` that logs) when sizing a pool.

### Experiment 3 🔧: Frame allocation elision (HALO)

```cpp
// @test run -std=c++23 -O2 -fno-stack-protector
#include <coroutine>
#include <utility>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>

static int g_allocs = 0;
void* operator new(std::size_t n) { ++g_allocs; if (void* p = std::malloc(n)) return p; throw std::bad_alloc{}; }
void  operator delete(void* p) noexcept { std::free(p); }
void  operator delete(void* p, std::size_t) noexcept { std::free(p); }

struct Gen {
    struct promise_type {
        int current = 0;
        Gen get_return_object() { return Gen{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(int v) noexcept { current = v; return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
    explicit Gen(std::coroutine_handle<promise_type> h) : h(h) {}
    Gen(Gen&& o) noexcept : h(std::exchange(o.h, {})) {}
    ~Gen() { if (h) h.destroy(); }
    std::coroutine_handle<promise_type> h;
};

Gen iota(int n) { for (int i = 0; i < n; ++i) co_yield i; }

[[gnu::noinline]] long sum_gen(int n) {
    long s = 0;
    Gen g = iota(n);                         // created, resumed and destroyed entirely inside this function
    for (g.h.resume(); !g.h.done(); g.h.resume()) s += g.h.promise().current;
    return s;
}

int main() {
    long s = sum_gen(1000);
    std::printf("sum = %ld, heap allocations for the coroutine frame: %d\n", s, g_allocs);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
sum = 499500, heap allocations for the coroutine frame: 1
```

Same source, other flags and compilers (run by hand; not auto-verified):

```text
g++-14     -O0, -O1, -O3 : sum = 499500, heap allocations: 1     (GCC 14.2 never elided the frame in this test)
clang++-18 -O0           : sum = 499500, heap allocations: 1
clang++-18 -O2           : sum = 499500, heap allocations: 0     (frame lives in sum_gen's stack frame)
```

GCC 14.2 allocated the frame at every optimisation level, even though the coroutine is created, driven and destroyed inside one function; Clang 18 removed the allocation at `-O2` (its LLVM `CoroElide` pass). That is a real, current, compiler-specific difference (🔧). Elision is an **optimisation, not a guarantee**: the standard says "may". It disappears if the handle escapes (stored in a global, passed to another thread, put in a container), if the coroutine is not destroyed in the creating function, or at `-O0`. **Never write code that depends on it for correctness**; use it as free performance for generators and short helper coroutines, and verify with a counting `operator new` as above.

### Experiment 4 ✅: Awaiters and the three `await_suspend` forms

```cpp
// @test run -std=c++23 -O0
#include <coroutine>
#include <utility>
#include <cstdio>
#include <cstdlib>
#include <deque>

struct Fire {                                   // fire-and-forget coroutine: starts eagerly, destroys itself
    struct promise_type {
        Fire get_return_object() { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::abort(); }
    };
};

std::deque<std::coroutine_handle<>> ready_queue;           // a toy "scheduler"
void run_ready() { while (!ready_queue.empty()) { auto h = ready_queue.front(); ready_queue.pop_front(); h.resume(); } }

// (a) void: always suspend; hand the handle to the scheduler
struct Yield {
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const { ready_queue.push_back(h); }
    void await_resume() const noexcept {}
};
// (b) bool: decide at the last moment whether to really suspend
struct MaybeSuspend {
    bool really;
    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<>) const { return really; }          // false => resume immediately, no switch
    int  await_resume() const noexcept { return really ? 1 : 0; }
};
// (c) handle: transfer control directly to another coroutine (symmetric transfer).
//     Shown here for the shape only; Chapter 34 uses it to chain Task<T>s without growing the stack.
struct Transfer {
    std::coroutine_handle<> next;
    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<>) const noexcept { return next; }   // resume `next` as a tail call
    void await_resume() const noexcept {}
};

Fire worker(const char* name, int steps) {
    for (int i = 1; i <= steps; ++i) {
        std::printf("  %s step %d\n", name, i);
        co_await Yield{};                                                           // cooperative yield: round-robin
    }
    std::printf("  %s done\n", name);
}
Fire decide() {
    int a = co_await MaybeSuspend{false};
    std::printf("  MaybeSuspend{false} -> await_resume=%d (never left the coroutine)\n", a);
}

int main() {
    std::puts("round robin:");
    worker("A", 2); worker("B", 3);                     // each runs eagerly to its first co_await
    run_ready();
    std::puts("bool form:");
    decide();
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
round robin:
  A step 1
  B step 1
  A step 2
  B step 2
  A done
  B step 3
  B done
bool form:
  MaybeSuspend{false} -> await_resume=0 (never left the coroutine)
```

`worker("A")` and `worker("B")` interleave because each `co_await Yield{}` parks the handle in `ready_queue` and returns to the caller: **that queue is a scheduler**, and `run_ready` is an event loop. Chapter 35 replaces the queue with `epoll`. Note `MaybeSuspend{false}`: `await_suspend` returned `false`, so the coroutine continued without ever returning to its caller, which is the trick for "the I/O was already complete" fast paths. The third form, `Transfer`, returns the handle of the coroutine to run next; we put it to work in Chapter 34.

### Experiment 5 ✅: Coroutine lifetime trap: a reference parameter dangles

```cpp
// @test crash -std=c++23 -O0 -g -fsanitize=address err=stack-use-after-scope
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

struct Lazy {
    struct promise_type {
        Lazy get_return_object() { return Lazy{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::abort(); }
    };
    explicit Lazy(std::coroutine_handle<promise_type> h) : h(h) {}
    Lazy(Lazy&& o) noexcept : h(std::exchange(o.h, {})) {}
    ~Lazy() { if (h) h.destroy(); }
    std::coroutine_handle<promise_type> h;
};

Lazy greet(const std::string& name) {              // BUG: `name` is a reference stored in the frame, not a copy
    std::fprintf(stderr, "hello, %s\n", name.c_str());
    co_return;
}

int main() {
    Lazy l = greet(std::string(40, 'x'));          // the temporary string dies at the end of this full-expression ...
    l.h.resume();                                  // ... but the coroutine runs now and reads it
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
=================================================================
==PID==ERROR: AddressSanitizer: stack-use-after-scope on address 0xADDR at pc 0xADDR bp 0xADDR sp 0xADDR
READ of size 8 at 0xADDR thread T0
    [... libstdc++ internal frames elided ...]
    #2 0xADDR in greet snippet.cpp:23
    #4 0xADDR in main snippet.cpp:29

Address 0xADDR is located in stack of thread T0 at offset 96 in frame
    #0 0xADDR in main snippet.cpp:27

  This frame has 3 object(s):
    [48, 49) '<unknown>'
    [64, 72) 'l' (line 28)
    [96, 128) '<unknown>' <== Memory access at offset 96 is inside this variable
HINT: this may be a false positive if your program uses some custom stack unwind mechanism, swapcontext or vfork
      (longjmp and C++ exceptions *are* supported)
SUMMARY: AddressSanitizer: stack-use-after-scope /usr/include/c++/14/bits/basic_string.h:228 in std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >::_M_data() const
[ASan shadow-memory dump and legend omitted]
==PID==ABORTING
```

ASan reports `stack-use-after-scope`: the reference points at the temporary `std::string` object in `main`'s stack frame, not at a copy. The coroutine is **lazy** (`initial_suspend` suspends), so the body does not run until `resume()`, after the temporary `std::string` has been destroyed. The fix is to take the parameter **by value**: by-value parameters are copied into the frame. This is the single most common coroutine bug, and the C++ Core Guidelines (CP.51–CP.53) say it plainly: coroutines should not take reference parameters and lambda coroutines should not capture.

### Experiment 6 ✅: What does a suspension cost? Generator vs loop vs callback

```cpp
// @test run -std=c++23 -O2 -fno-stack-protector timeout=120
#include <chrono>
#include <coroutine>
#include <utility>
#include <cstdio>
#include <cstdlib>
#include <functional>

struct Gen {
    struct promise_type {
        int current = 0;
        Gen get_return_object() { return Gen{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(int v) noexcept { current = v; return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
    explicit Gen(std::coroutine_handle<promise_type> h) : h(h) {}
    Gen(Gen&& o) noexcept : h(std::exchange(o.h, {})) {}
    ~Gen() { if (h) h.destroy(); }
    std::coroutine_handle<promise_type> h;
};
Gen iota(int n) { for (int i = 0; i < n; ++i) co_yield i; }

[[gnu::noinline]] long sum_loop(int n)                    { long s = 0; for (int i = 0; i < n; ++i) { s += i; asm volatile("" ::: "memory"); } return s; }
[[gnu::noinline]] long sum_gen(int n)                     { long s = 0; Gen g = iota(n); for (g.h.resume(); !g.h.done(); g.h.resume()) s += g.h.promise().current; return s; }
[[gnu::noinline]] long sum_callback(int n, const std::function<void(int)>& f) { for (int i = 0; i < n; ++i) f(i); return 0; }

template <class F> double ns_per(int n, F f) {
    auto t0 = std::chrono::steady_clock::now();
    long r = f();
    double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
    if (r == -1) std::puts("");                     // keep r alive
    return ns;
}

int main() {
    constexpr int N = 50'000'000;
    long cb_sum = 0;
    std::function<void(int)> cb = [&](int i) { cb_sum += i; asm volatile("" ::: "memory"); };
    std::printf("ns per element over %d elements:\n", N);
    std::printf("  plain loop ............ %5.2f\n", ns_per(N, [&] { return sum_loop(N); }));
    std::printf("  coroutine generator ... %5.2f\n", ns_per(N, [&] { return sum_gen(N); }));
    std::printf("  std::function callback  %5.2f\n", ns_per(N, [&] { return sum_callback(N, cb); }));
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
ns per element over 50000000 elements:
  plain loop ............  0.31
  coroutine generator ...  2.28
  std::function callback   1.63
```

A suspend+resume pair costs a few nanoseconds: an indirect call to the resume function, a `switch` on the suspend index and a few spills. That is the same order as a `std::function` call (which is also an indirect call) and about 7x a plain loop iteration (2.2 ns against 0.3 ns here), so **coroutine generators are for structure, not for tight inner loops**. The one-time frame allocation is amortised over 50 million elements and is not what is measured; with Clang's frame elision the per-element cost may differ (Exercise 1).

---

## 8. Assembly / runtime investigation

```bash
# 1. See the three functions the compiler creates: ramp (f), resume (f.resume / _Z1f.actor), destroy (f.destroy)
g++-14 -std=c++23 -O1 -S -o - prog.cpp | c++filt | grep -E '^[_a-zA-Z0-9().]+:' | grep -i -E 'resume|destroy|actor'
nm -C prog.o | grep -E 'f\(|\.resume|\.destroy|\.actor'

# 2. Clang: dump the LLVM IR before and after coroutine lowering
clang++-18 -std=c++23 -O0 -S -emit-llvm -Xclang -disable-llvm-passes prog.cpp -o pre.ll    # contains llvm.coro.* intrinsics
clang++-18 -std=c++23 -O2 -S -emit-llvm prog.cpp -o post.ll                                 # CoroSplit/CoroElide have run
grep -E 'llvm\.coro\.(id|begin|suspend|save|end|free|alloc)' pre.ll | head -30

# 3. See the frame header in gdb (GCC): the handle address points at the resume-fn pointer
gdb -q ./a.out -ex 'break f' -ex run -ex 'finish' -ex 'x/4gx c.h.address()'
# word 0 = resume function address, word 1 = destroy function address, then the promise, then spilled locals

# 4. Count frame allocations, prove HALO happened or didn't
ltrace -e malloc ./a.out 2>&1 | wc -l          # or use the counting operator new from Experiment 3
valgrind --tool=massif ./a.out                  # frame sizes and peak live frames

# 5. Debug info: GCC emits the coroutine state as __coro_frame with named fields
gdb -q ./a.out -ex 'ptype struct _Z1fi.Frame' 2>&1 | head -20
```

---

## 9. Implementation exercise

Implement a **`Generator<T>`** step by step, the machinery tour in code. Requirements: lazy start; `co_yield` of lvalues and rvalues by reference (no copy of the yielded object); an iterator interface so `for (auto x : gen)` and `std::ranges` work; exception propagation through `unhandled_exception`; move-only RAII ownership of the handle.

<details>
<summary><strong>Solution</strong></summary>

```cpp
// @test run -std=c++23 -O0
#include <coroutine>
#include <cstdio>
#include <exception>
#include <iterator>
#include <memory>
#include <ranges>
#include <utility>

template <class T>
class Generator {
public:
    struct promise_type {
        std::add_pointer_t<std::conditional_t<std::is_reference_v<T>, T, T&>> value_ = nullptr;   // points at the yielded object: no copy
        std::exception_ptr error_;
        Generator get_return_object() noexcept { return Generator{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(std::remove_reference_t<T>& v) noexcept { value_ = std::addressof(v); return {}; }
        std::suspend_always yield_value(std::remove_reference_t<T>&& v) noexcept { value_ = std::addressof(v); return {}; }   // safe: the temporary lives until the next resume
        void return_void() noexcept {}
        void unhandled_exception() noexcept { error_ = std::current_exception(); }
        void await_transform() = delete;                    // generators may not co_await
    };

    class iterator {
        std::coroutine_handle<promise_type> h_;
    public:
        using value_type = std::remove_cvref_t<T>;
        using difference_type = std::ptrdiff_t;
        iterator() = default;
        explicit iterator(std::coroutine_handle<promise_type> h) : h_(h) {}
        iterator& operator++() { h_.resume(); if (h_.done()) { auto e = h_.promise().error_; h_ = nullptr; if (e) std::rethrow_exception(e); } return *this; }
        void operator++(int) { ++*this; }
        T& operator*() const noexcept { return *h_.promise().value_; }
        friend bool operator==(const iterator& it, std::default_sentinel_t) { return !it.h_ || it.h_.done(); }
    };

    Generator(Generator&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    Generator& operator=(Generator&& o) noexcept { if (this != &o) { if (h_) h_.destroy(); h_ = std::exchange(o.h_, {}); } return *this; }
    ~Generator() { if (h_) h_.destroy(); }

    iterator begin() { if (h_) { h_.resume(); if (h_.done() && h_.promise().error_) std::rethrow_exception(h_.promise().error_); } return iterator{h_}; }
    std::default_sentinel_t end() const noexcept { return {}; }

private:
    explicit Generator(std::coroutine_handle<promise_type> h) : h_(h) {}
    std::coroutine_handle<promise_type> h_;
};

Generator<int> fib() { int a = 0, b = 1; for (;;) { co_yield a; a = std::exchange(b, a + b); } }
Generator<int> take_while_small(Generator<int>& g, int limit) { for (int x : g) { if (x > limit) co_return; co_yield x; } }

int main() {
    auto g = fib();
    auto small = take_while_small(g, 100);
    for (int x : small) std::printf("%d ", x);
    std::puts("");
    for (int x : fib() | std::views::drop(5) | std::views::take(5)) std::printf("%d ", x);      // works with ranges
    std::puts("");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
0 1 1 2 3 5 8 13 21 34 55 89 
5 8 13 21 34 
```

C++23 ships `std::generator<T>` (header `<generator>`, libstdc++ 14 ✅) with the same design plus *nested generators* via `co_yield std::ranges::elements_of(inner)` which avoid the per-level resume chain. Use it when available.

</details>

---

## 10. Real-world example

| Where | How coroutines are used |
|---|---|
| **Asynchronous I/O (Boost.Asio `awaitable`, libunifex, cppcoro, folly::coro)** | Sequential-looking network code over a reactor or proactor; the awaiter registers a callback with the OS and `await_suspend` returns |
| **`std::generator` (C++23)** | Lazy sequences, tree traversals (`co_yield elements_of`), parsers that emit tokens |
| **Game engines (Unreal, Unity C#; custom C++ engines)** | Script-style coroutines that wait for frames or timers: `co_await wait_seconds(2)` |
| **Servers (Seastar, Meta's folly, Microsoft's cpp/WinRT)** | One coroutine per request; all concurrency from a handful of threads; frames pooled |
| **Parsers and state machines** | Network protocols written as straight-line reads: `auto hdr = co_await read(4);` |
| **Qt** | `QCoro` library adapts `QFuture`, `QNetworkReply`, signals to `co_await`; Qt 6.x itself ships `QFuture` continuations but has no coroutine core yet (Chapter 47) |

> **Opinion.** Treat `std::coroutine_handle` the way you treat `new`: a mechanism to wrap once, in a small audited class, and then never touch in application code. Application code should see `Task<T>`, `Generator<T>` and `co_await some_operation()`. Writing raw promise types ad hoc in a codebase produces the same class of bugs as raw `new`/`delete` did. Also: do not use coroutines where a plain function or a range pipeline would do, as the frame and the lifetime rules are real costs. They earn their place when the code **waits** (I/O, timers, other coroutines) or when you need a pull-style generator.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| **Reference / `string_view` / `span` parameters** to a coroutine | Use-after-free once the caller's object dies (Experiment 5) | Take parameters **by value**; for views, ensure the owner outlives the coroutine; Core Guidelines CP.53 |
| **Lambda coroutine with captures** | The lambda object (and thus the captures) dies while the coroutine is suspended; the frame holds a `this` pointer to it | Capture nothing; pass values as parameters (`[](int x) -> Task<> {...}(x)`); or keep the lambda alive for as long as the coroutine runs |
| **Forgetting `co_return`** in a coroutine whose promise has `return_value` | Falling off the end is UB (compiler assumes `return_void`) | Always `co_return` a value; compile with `-Wreturn-type`; give the promise only one of `return_void`/`return_value` |
| **Resuming a finished coroutine** (`done()` is true) | UB: a crash or a jump into the weeds | Check `done()`; wrap the handle in a type that tracks state |
| **Destroying a running coroutine** | Frame freed while its own code is executing | `destroy()` only when suspended; in `await_suspend`, remember another thread may destroy or resume *immediately* |
| **Double-resume race** in `await_suspend` (handle published, then more work) | Thread A resumes the coroutine while thread B is still inside `await_suspend` touching `this` (a member of the awaiter, which lives in the frame!) | Do all work with `this` **before** publishing the handle; after publishing, touch nothing |
| **Not destroying the frame** (handle leaked) | Memory leak; destructors of locals never run | RAII return type owning the handle; for fire-and-forget, `final_suspend` returns `suspend_never` |
| **Destroying at `final_suspend` and also in the owner** | Double free | One owner: either `final_suspend` auto-destroys (`suspend_never`) or the owner does, never both |
| **`final_suspend` that throws or doesn't suspend while the owner expects to read the promise** | UB / compile error (it must be `noexcept`) | `suspend_always`, read the result, then `destroy()` |
| **Deep `co_await` chain without symmetric transfer** | Stack overflow in `Task` chains of 100 000 awaits (each `resume()` nests) | `await_suspend` returns a `coroutine_handle<>` (Chapter 34) |
| **Eager coroutine started before its consumer is ready** | Work runs on the wrong thread, or exceptions fire before they can be caught | Prefer lazy start (`initial_suspend` returns `suspend_always`) |
| **Assuming HALO** | Surprise heap allocation in a hot path | Measure (Experiment 3); provide `promise_type::operator new` with a pool |
| **Shipping coroutine handles across binaries built with different compilers** | Crash: frame layout and resume ABI differ | Keep coroutines inside one toolchain; use C ABI callbacks at the boundary |

---

## 12. Exercises

1. **Compare compilers.** Run Experiments 2 and 3 with `clang++-18`. Record frame sizes and allocation counts for `-O0/-O1/-O2/-O3`. Which differences are surprising?
2. **Hand-expand.** Take Experiment 1's `f` and write the equivalent state machine by hand: a struct for the frame, a `resume` function with a `switch` on the state, a `destroy` function. Check that observable behaviour matches.
3. **Promise hooks.** Add `await_transform` to a promise so that `co_await 5` (an `int`) means "yield to the scheduler and resume with the value 5". Add `get_return_object_on_allocation_failure`.
4. **Frame allocator.** Give `promise_type` an `operator new`/`delete` that draws from a fixed-size free list (Chapter 26). Measure the allocation cost for 10 million short coroutines against the default.
5. **Exceptions.** Throw from inside a coroutine body, from `initial_suspend`'s awaiter, and from `await_suspend`. Document where each exception ends up and which hooks run.
6. **Detect the race.** Write an awaiter whose `await_suspend` posts the handle to another thread and then reads a member of itself; reproduce the use-after-free with ASan/TSan, then fix it.
7. **`std::generator`.** Port Experiment 6 to C++23 `std::generator` and compare. Then write a recursive tree traversal using `elements_of` and compare to the hand-written recursive generator (O(depth) resume chain vs O(1)).
8. **Read the source.** Read libstdc++ `<coroutine>` and `<generator>` (and LLVM's `CoroSplit.cpp` summary comments). List three decisions you now understand.

---

## 13. Challenge: a resumable tokenizer

Write a coroutine-based **incremental JSON tokenizer**: a `Generator<Token>` fed by a byte source that may deliver data in arbitrary chunks (`co_await next_chunk()`), so the caller can push 1 byte at a time or 1 MiB at a time and receive identical token streams. Constraints: no allocation per token (tokens are views into a ring buffer), correct handling of a token split across chunks, `Generator`-style lifetime safety. Test by feeding every JSON file in a corpus byte-by-byte and whole, and comparing; fuzz with AFL or libFuzzer. Then rewrite it as a hand-written state machine and compare speed and code size.

---

## 14. Knowledge check

1. What makes a function a coroutine? Where does its return type's `promise_type` come from?
2. What lives in a coroutine frame, and what does not?
3. List, in order, everything the compiler calls from the moment `f(args)` is invoked until the first suspension (for a lazy coroutine).
4. What is the difference between `initial_suspend` returning `suspend_always` and `suspend_never`?
5. What do `await_ready`, `await_suspend`, `await_resume` each do? What are the three return forms of `await_suspend` and what does each mean?
6. Why is it dangerous for `await_suspend` to touch `this` after publishing the handle?
7. Why does `final_suspend` need to be `noexcept`, and why do most promises suspend there?
8. A coroutine parameter is `const std::string&`. Where is the string stored and what can go wrong?
9. What is HALO? Under what conditions can it fail? Is it guaranteed?
10. Why does a stackless coroutine require callees to be coroutines to suspend? What do stackful coroutines trade away?
11. What is symmetric transfer and which problem does it solve?
12. Is the frame layout part of the C++ standard? Of the Itanium ABI? What follows?

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Containing `co_await`, `co_yield` or `co_return`. `std::coroutine_traits<R, Args...>::promise_type`, which defaults to the nested type `R::promise_type`.
2. A header (resume/destroy function pointers), the promise object, copies of parameters, the suspend-point index, and locals/temporaries that are live across a suspension. Locals that die before any suspension are not stored; they stay in registers or on the executing stack.
3. `operator new` (frame), copy parameters, construct the promise, `get_return_object()`, `initial_suspend()` (+ its awaiter's `await_ready`/`await_suspend`); with `suspend_always` control returns to the caller before any body code runs.
4. `suspend_always`: lazy; the body runs only when resumed. `suspend_never`: eager; the body runs immediately inside the call until the first real suspension.
5. `await_ready`: may we skip suspending? `await_suspend`: runs after the coroutine is suspended, receives its handle, arranges the resumption. `await_resume`: computes the value of the `co_await` expression after resumption. Forms: `void` = stay suspended; `bool` = `true` stay, `false` resume at once; `handle` = stay suspended and symmetric-transfer to that coroutine.
6. The coroutine is already suspended, so another thread may resume it (and even finish and destroy the frame, which owns the awaiter) concurrently. Touching `this` afterwards is a data race / use-after-free.
7. Throwing there would leave the coroutine in an undefined state with the exception having nowhere to go; suspending there lets the owner read the result and destroy the frame deliberately, with destructors of the promise run at a known time.
8. Only the reference is stored in the frame; the string lives with the caller. If the caller's string dies before the coroutine reads it (e.g. a temporary and a lazy start), use-after-free.
9. Heap-allocation elision: replacing the frame's `operator new` with storage in the caller's frame when the lifetime is provably nested. Fails if the handle escapes, the coroutine outlives the caller, or at low optimisation levels. Not guaranteed.
10. The frame holds only the coroutine's own locals; there is no separate stack for callees to suspend on. Stackful coroutines allocate a full stack (large, harder to optimise, needs context switching) in exchange for suspension from any depth.
11. `await_suspend` returning a handle makes the runtime resume that coroutine as a tail call, so a chain of awaits does not grow the stack; without it, long chains of already-ready awaits recurse and overflow.
12. No. Compiler-specific. Therefore handles/frames must not cross toolchain or shared-library boundaries built with different compilers.

</details>

---

[← Previous: Chapter 32](../part-12-concurrency/32-lock-free.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 34 — Building a coroutine type →](34-building-task.md)
