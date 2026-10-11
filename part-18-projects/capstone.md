# Capstone — A Small Asynchronous Runtime Library

[← Project 11](project-11-python-extension.md) &nbsp;|&nbsp; [Projects overview](README.md) &nbsp;|&nbsp; [Course map](../README.md)

> **Level 5 · ≈ 30 hours · Builds on:** everything. Uses the work of Projects 1 and 4–10.
> **Reference solution:** [`code/capstone/`](code/capstone/) (`include/aio/`, `tests/`, `bench/`, `examples/`, `Makefile`)

## Goal

Combine what you built into one coherent library, **`aio`**, and put a real workload through it:

```text
         ┌──────────────────────────── Runtime ─────────────────────────────┐
         │   Loop 0 (thread)      Loop 1 (thread)   …      ThreadPool       │
         │   epoll + timers        epoll + timers          (blocking / CPU) │
         └───────▲───────────────────────▲──────────────────────▲───────────┘
   post()/spawn() from any thread        │ co_await rt.blocking(f): run on the pool, resume on the same loop
                 │
   net::accept / read_some / write_all   → Result<T>  (std::expected<T, std::error_code>)
                 │
   http::Server  → per-connection Arena → parse_request → Router (concept-constrained handlers, coroutines)
```

This is a *design* exercise as much as a coding one. The reference solution is about 1 100 lines of library code (over a third of it derived from earlier projects), plus about 320 lines of tests, and each file is small enough to read in a sitting. What matters is where the **seams** are, and which of your earlier decisions turn out to be wrong when the pieces meet.

## Requirements and where the reference solution meets them

| Requirement (course brief) | Reference solution | Status |
|---|---|---|
| RAII | `net::Fd`, `Loop` (epoll and eventfd), `Runtime` (joins threads), `Server` | covered |
| Ownership | One thread owns each loop; `Task` owns its frame; `Server` owns listeners; the arena owns request memory | covered |
| Templates and **concepts** | `RequestHandler` concept constrains `Router::add/get/post` | covered |
| **Ranges** | `views::split` for path segments and query parameters; `ranges::transform/all_of` in the parser | covered (small) |
| **Type erasure** | `aio::Function` (small-buffer, move-only) holds posted work and route handlers | covered |
| **Custom allocators** | `Request` is built from `std::pmr` containers; each connection owns an `Arena` and resets it per request; `ServerConfig::use_arena` switches it off | covered |
| **Memory pool** | Not in the reference library: only the arena is. Project 5's `Pool`/`PoolResource` are the natural drop-in (see stretch goals) | **partial** |
| **Coroutines** | `Task<T>`; the loop's awaitables; handlers; `Runtime::blocking` | covered |
| **Concurrency** | N shared-nothing loop threads, cross-thread `post`, thread pool, atomics for stats | covered |
| **Event loop and timers** | `Loop`: epoll, eventfd wake-up, `sleep`, **deadlines on every I/O wait** | covered |
| **TCP sockets** | `net::` accept/connect/read/write, `SO_REUSEPORT` listener per loop | covered |
| **HTTP parser and routing** | `http::parse_request`, `Router` with `:param` | covered |
| **Structured error handling** | `Errc` + `std::error_category`; I/O returns `Result<T>`; handler exceptions become `500` | covered |
| **Unit tests** | `tests/test_loop.cpp`, `test_runtime.cpp`, `test_http.cpp` | covered |
| **Benchmarks** | `bench/bench_parse.cpp`, the load generator from Project 10 | covered, with the caveats below |
| Build system | A plain `Makefile`. (The course skips build-system chapters, so there is no CMake here.) | covered |

Unlisted but present: graceful shutdown, a header/idle/write timeout policy, and sanitizer-clean tests.

## Specification (what to build)

1. **`Loop`**
   - One thread runs `run()`; **any** thread may call `post(fn)` and `post_spawn(task)`, which wake the loop through an `eventfd`.
   - `co_await readable(fd, timeout?)` / `writable(fd, timeout?)` return `bool` (`false` = deadline). A fired deadline must unregister the fd; a fired event must cancel the deadline. (Both directions are tested: the second one is the bug that resumes a dead coroutine.)
   - `sleep(duration)`, `spawn(task)`, `Loop::current()`.
2. **`Runtime`**
   - `Runtime(io_threads, pool_threads)`; `spawn`/`spawn_on(i, …)` from any thread (round-robin); `loop(i)`.
   - `co_await rt.blocking(f)` runs `f` on the pool and **resumes on the loop thread that awaited it**, so the code after it may keep using loop-confined state without locks. Exceptions in `f` are rethrown in the awaiting coroutine.
3. **`net`**: `listen_tcp(port, reuse_port)`, `accept`, `connect`, `read_some`, `write_all`; all return `Result<…>`; EOF is `Errc::closed`, a missed deadline is `Errc::timeout`. No exception on an expected failure.
4. **`http`**: the parser and limits from Project 10, with `std::pmr` request fields; `Router` constrained by the `RequestHandler` concept; `Server` with one `SO_REUSEPORT` listener per loop, header/idle/write timeouts, and a graceful `stop()` that returns `true` only when every connection has closed.
5. **Everything passes** ASan+UBSan and TSan, in the plain and arena configurations, repeatedly.

## Milestones

1. **Port the loop** from Project 9 to be multi-thread-aware (`post`, `eventfd`, `stop`). Test: 4 producers post 20 000 closures each; none is lost.
2. **Deadlines.** Add the timeout to `readable`/`writable`. Write the two tests described above *before* the implementation; the second one is subtle.
3. **`Result`-based `net`**, replacing the exceptions from Project 10.
4. **`Runtime`**, then **`blocking()`**. Test it with a ticker coroutine on the same loop: the ticker must keep ticking while the pool sleeps.
5. **Server.** Start from Project 10's connection loop; add timeouts, stats, graceful stop. Test with real sockets from plain blocking client threads (no coroutines in the test client: it must not share a bug with the server).
6. **Arena** per connection. Make it switchable, and run the full test suite in both modes.
7. **Benchmark**, honestly.
8. **Write the one-page design note** (see "What to hand in").

## What the tests prove

`make test` (ASan+UBSan, then TSan) on GCC 13.3; `test_http` additionally under GCC 14.2 and repeated ten times under ASan:

```text
== loop (ASan+UBSan)     all tests passed
== runtime (ASan+UBSan)  all tests passed
== http (ASan+UBSan)     all tests passed
== loop / runtime / http (TSan)   all tests passed
with per-connection arena:
  64 concurrent /slow (100 ms each): 150 ms wall
  20 requests while a 400 ms blocking handler runs: 4 ms
with the default allocator:
  64 concurrent /slow (100 ms each): 128 ms wall
  20 requests while a 400 ms blocking handler runs: 6 ms
```

What those two lines say:

- **64 requests, each waiting 100 ms, finished in about 130-150 ms of wall time** on two loop threads: waiting does not occupy a thread.
- **20 requests were served in 4-6 ms while another handler was blocking a pool thread for 400 ms.** That is the reason for `blocking()`: the same handler written with `std::this_thread::sleep_for` on the loop thread would have stalled every other connection on that loop for 400 ms.

The test also checks: a half-sent request gets `408` after the header timeout; a silent connection is closed by the idle timeout; keep-alive and three pipelined requests share one TCP connection; both loop threads serve connections (60 requests hit exactly 2 distinct thread ids); `stop()` closes an idle keep-alive connection in well under a second and returns `true`; and no coroutine ended in an exception (`task_errors() == 0`).

## Measurements

### Parsing with and without the arena

One 335-byte request with nine headers, `-O2`, GCC 13.3, best of 7 (two runs each):

```text
new_delete resource    1098 - 1138 ns
Arena + reset           880 -  920 ns      →  about 20% less time
```

The arena helps, a little, because a request creates ~30 small `pmr::string`/map-node allocations that become pointer bumps plus one bulk `reset`. Note what it costs: a 16 KiB chunk per connection, and the discipline that no request-derived object may outlive the request. The parse is about a microsecond either way.

### End-to-end throughput

The Project 10 load generator (keep-alive `GET /hello`, 100 connections × 2000 requests) against the capstone server, on the **same 2-vCPU VM**, three runs per configuration:

```text
configuration            requests/s (three runs)
1 io thread, arena        104 900   71 700   88 600
1 io thread, no arena      92 100   80 800   86 900
2 io threads, arena       109 900   93 100   74 100
2 io threads, no arena     89 900   93 000   74 600
```

**These numbers show no difference between any two configurations.** The run-to-run spread (72 000 to 110 000) is larger than any gap between configurations. Two threads are not faster than one because the load generator is a second process competing with the server for the same two virtual CPUs. The honest conclusions are: (1) the runtime sustains roughly 70-110 thousand small keep-alive requests per second per core-pair on this VM; (2) the arena's ~20% parse saving is invisible at this level because parsing is a small part of a request's cost (system calls dominate); (3) **to see multi-thread scaling you need a machine with spare cores and a load generator on separate ones.** Do not quote the table as evidence for or against either setting.

## Design decisions you must be able to defend

| Decision | Alternatives | Why this one here |
|---|---|---|
| Loop per thread, shared-nothing | One loop + worker pool; work stealing | No locks on the hot path; connections stay on one cache-warm thread. Cost: load balancing is left to `SO_REUSEPORT`'s hash |
| `post()` through a mutex-protected queue and an `eventfd` | A lock-free MPSC queue (Project 7) | The queue is not the bottleneck (system calls are); a mutex is simpler to get right. Revisit if profiling says otherwise |
| `blocking()` resumes on the origin loop | Resume on the pool thread | Keeps loop-confined state unlocked; costs one extra `post` and wake-up per offload |
| `Result<T>` for I/O, exceptions for handlers | Exceptions everywhere; `Result` everywhere | A closed or timed-out connection is *expected* and frequent; a handler bug is not. Don't pay exception cost on the common failure path |
| Timeouts as part of every wait | A separate `with_timeout` combinator | One timer entry per waiting fd, cancelled by whichever event comes first. A combinator would need coroutine cancellation (a stretch goal) |
| Per-connection `Arena`, reset per request | `new` per field; a global pool | Request lifetime is exactly the arena's lifetime; ~20% parse saving; no per-field free |
| `aio::Function` for route handlers | `std::function` | Handlers may capture move-only state |
| Accept loop polls with a 50 ms timeout to observe `stop()` | Close the listener from another thread; self-connect to wake it | Simple and correct; costs a wake-up per loop per 50 ms. Project 10's self-connect trick is equally valid |

## Known limitations of the reference solution (deliberate)

- **GCC only.** It needs `std::expected`, and **Clang 18 with libstdc++ 13 does not provide it** (libstdc++'s `<expected>` requires `__cpp_concepts` ≥ 202002, which Clang 18 does not define). Use GCC 13.3+, or Clang with a newer libstdc++ or libc++.
- Linux-only (`epoll`, `eventfd`, `accept4`, `SO_REUSEPORT`).
- No TLS, no HTTP/2, no chunked bodies, no `Expect: 100-continue`, no percent-decoding, IPv4 loopback only.
- `Runtime::stop()` stops loops immediately: tasks still suspended at that moment are leaked (their frames are never resumed or destroyed). `Server::stop()` drains connections first, and the tests wait for `live_tasks() == 0` before stopping the runtime.
- Cancellation of an in-flight `blocking()` call is not supported.
- Unbounded `post` queue (no back-pressure).
- No logging or metrics beyond the atomic counters in `ServerStats`.

## Exercises (do at least three)

1. **Memory pool.** Replace the per-connection `Arena` for *coroutine frames* with a pool: give `Task<T>::promise_type` an `operator new`/`operator delete` that draw from a thread-local `PoolResource` (Project 5). Measure `co_await` cost before and after (Project 8's benchmark). Where is the size-class assumption wrong?
2. **Cancellation.** Implement `Task` cancellation via `std::stop_token` and make `with_timeout(task, duration)` a combinator. What must happen to a `read_some` that is waiting when the token fires?
3. **Back-pressure.** Bound the `post` queue and the number of in-flight connections. What should `accept` do when the limit is hit?
4. **Replace the posted-work queue** with the lock-free MPSC queue of Project 7 (you will need to write one: the MPMC ring is bounded). Does a benchmark of `post` throughput justify it?
5. **Persistent epoll registrations.** Remove the `epoll_ctl(ADD/DEL)` pair on every wait (Project 9, "Think about" #2). Measure with the load generator, using enough cores.
6. **`perf` it.** Profile the server under load (`perf record -g`, Chapter 40). What fraction of time is in system calls, in parsing, in the allocator, in coroutine resume? Does it match your expectation?
7. **Fuzz** `parse_request` with libFuzzer (`-fsanitize=fuzzer,address`). Fix what it finds.
8. **Port to `io_uring`**: replace readiness (`epoll`) with completion (`io_uring`) behind the same `read_some`/`write_all` interface.
9. **Add a client:** `http::Client` with connection pooling, built on the same loop; use it in a test against the server.
10. **Instrument** the runtime with per-loop metrics (tasks, wake-ups, timers, bytes) exported on a `/metrics` route.

## What to hand in (the part that proves mastery)

A design note of **one page**, in your own words:

1. A diagram of who owns what, and on which thread each object may be touched.
2. The ordering guarantees you rely on (what `happens-before` what) in `post`, `blocking`, and graceful stop; name each `memory_order` or lock that provides them.
3. Three bugs you hit, how the sanitizer or test exposed each, and what you changed.
4. One benchmark you ran, what you expected, what you measured, and your explanation. (If you cannot explain it, say so and say what you would measure next.)
5. The single decision you would reverse with hindsight.

## Rubric

Use the [self-grading rubric](README.md#grading-rubric-use-it-on-yourself). A capstone scoring 2 on every row means you understand the material; 3 on **ownership**, **sanitizers** and **explanation** means you can be trusted with someone else's event loop.

## Reference solution notes

- Files in `include/aio/` marked "Derived from Project N" are those projects' code with the namespace renamed, so the capstone has no cross-project includes. The *new* code is `error.hpp`, `loop.hpp` (cross-thread post, deadlines), `runtime.hpp`, `net.hpp` and `http.hpp`.
- **`Loop::run` handles the eventfd inside the epoll loop**, level-triggered and permanent, so a `post` from any thread is one `write` and one `read`.
- **A deadline and an fd wait are linked:** the `Wait` entry stores an iterator to its timer, and the timer entry stores the fd. Whichever fires first erases the other. Without that, a stale timer would resume a coroutine that had already moved on.
- **`Runtime::blocking`'s awaiter lives in the suspended coroutine's frame.** The pool thread writes into it and then posts the resume to the home loop; the `post` (mutex + eventfd) is the synchronisation that makes that write visible to the loop thread.
- **Lifetime rule of thumb used throughout:** objects touched by one loop thread are not synchronised; everything shared across threads is an atomic, a mutex-protected queue, or the pool.

Run it:

```bash
cd code/capstone
make test            # builds and runs all tests under ASan+UBSan, then TSan
make bench           # parse microbenchmark; builds the load generator
make build/server && build/server 8080 2     # then: curl -i localhost:8080/hello
build/loadgen 8080 100 2000
```

## What I learned (write it)

> If you had to explain this runtime to a colleague in five minutes using only the ownership diagram, what would you draw, and what would you say is the most fragile invariant?
