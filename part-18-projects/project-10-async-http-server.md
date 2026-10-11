# Project 10 — Async HTTP Server

[← Project 9](project-09-event-loop.md) &nbsp;|&nbsp; [Projects overview](README.md) &nbsp;|&nbsp; [Next: Project 11 — Python extension →](project-11-python-extension.md)

> **Level 4 · ≈ 12 hours · Builds on:** Chapters 34–35 (coroutines and networking), Projects 8 and 9 (`Task`, `EventLoop`)
> **Reference solution:** [`code/p10/http.hpp`](code/p10/http.hpp), [`server_main.cpp`](code/p10/server_main.cpp), [`test.cpp`](code/p10/test.cpp), [`loadgen.cpp`](code/p10/loadgen.cpp)

## Goal

```text
Linux → epoll → event loop → coroutines → HTTP parser → router → handlers
```

A small HTTP/1.1 server with **no framework**, running on a single thread, where each connection is a coroutine, each handler is a coroutine, and none of them blocks. You will meet the parts of HTTP that make real servers hard: partial reads, pipelining, size limits, keep-alive, and a parser that must treat every prefix of a valid request as "need more" rather than "error".

## Specification

1. **Net layer** (awaitables over Project 9's loop): `accept_one`, `connect_to`, `read_some`, `write_all`. All sockets are non-blocking; `write_all` uses `send(..., MSG_NOSIGNAL)`.
2. **Parser**, incremental and allocation-conscious:
   - `parse_request(buffer)` → `need_more | done | bad_request | header_too_large | body_too_large | not_implemented`, plus the number of bytes consumed.
   - **Every proper prefix of a valid request is `need_more`**, never an error (tested by feeding all prefixes).
   - Request line `METHOD SP target SP HTTP/1.0|1.1`; headers case-insensitive (stored lower-case); body by `Content-Length` only.
   - Limits: header block ≤ 16 KiB, body ≤ 1 MiB (configurable).
   - **Reject** (as `400`): whitespace before the header colon, conflicting duplicate `Content-Length`, non-numeric `Content-Length`, unknown HTTP version, lower-case method, a target that does not start with `/`.
   - `Transfer-Encoding` present → `501` (chunked bodies are a stretch goal).
3. **Router:** `add(method, "/users/:id", handler)`; matches by path segments; unknown path → `404`; known path but wrong method → `405` with an `Allow` header.
4. **Handlers** are coroutines `Task<Response>(Request&)`. A handler that throws yields `500` and **the server keeps running**.
5. **Connections:** one detached task each. Keep-alive by default for HTTP/1.1 (`Connection: close` ends it). **Pipelined requests** in one TCP segment are answered in order, in one connection. A protocol error closes the connection after the error response.
6. **Graceful shutdown:** `shutdown()` stops accepting, lets existing connections finish, and `loop.run()` returns when every task has completed. Nothing is leaked (ASan/LSan clean).

## Milestones

1. **The parser alone**, with unit tests and no sockets. Property test: for every valid request string, every prefix is `need_more` and the full string is `done` with `consumed == size()`.
2. A listener and an accept loop that prints each peer connecting.
3. One connection coroutine that reads a request and writes a fixed `200`.
4. Router and handlers.
5. Keep-alive and pipelining.
6. Error mapping (`400/404/405/413/431/500/501`).
7. Graceful shutdown.
8. Load test; sanitizers; compare behaviour with `curl`.

## Testing it for real

The in-process test (`test.cpp`) runs the server and its own client coroutines on **one** loop, so there are no threads, no races, and no ports to collide with (it listens on port 0). It covers every status code, split packets (headers, a pause, then the body), keep-alive over one TCP connection, three pipelined requests in one `send`, a throwing handler, and 200 concurrent clients hitting a 100 ms handler:

```text
200 concurrent /slow requests (100 ms each): 130 ms wall
requests served: 214, connections: 213
all tests passed
```

**200 requests at 100 ms each took 130 ms of wall time on one thread.** That is the point of the exercise: waiting is free; only CPU work is serial.

The same tests pass with GCC 13.3 (`-fsanitize=address,undefined`, `-fsanitize=thread`), GCC 14.2 and Clang 18.

And with the real tool (server in one terminal, `curl` in another):

```text
$ ./server 8080 &
$ curl -i localhost:8080/hello
HTTP/1.1 200 OK
content-type: text/plain; charset=utf-8
content-length: 13
connection: keep-alive

Hello, world
$ curl localhost:8080/users/alice        → user alice
$ curl "localhost:8080/sum?a=20&b=22"     → 42
$ curl -X POST -d 'posted body' localhost:8080/echo   → posted body
$ curl -i -X DELETE localhost:8080/hello  → 405 Method Not Allowed, "allow: GET"
$ curl localhost:8080/nothing             → 404
```

## Measurements

A separate load generator ([`loadgen.cpp`](code/p10/loadgen.cpp), also coroutines on the same event loop) drives **keep-alive** `GET /hello` over loopback. Server and generator run as two processes on the same 2-vCPU VM, `-O2`, GCC 13.3, 200 000 requests each:

```text
connections   requests/connection   result
          1               200 000   17 600 req/s   (11.4 s)   ≈ 57 us per request-response round trip
         10                20 000   86 500 req/s
        100                 2 000   81 200 req/s
      1 000                   200   88 300 req/s
```

- **One connection measures latency, not throughput.** A request, a response, and two `epoll` wake-ups across two processes cost about 57 µs. Nothing is being processed concurrently, so the server is idle most of that time.
- **With 10 or more connections, throughput plateaus around 85 000 requests/s** on a single server thread, sharing two virtual CPUs with the load generator. The curve is flat from 10 to 1000 connections, which is what an event loop should do: the cost per connection is a descriptor and a small buffer, not a thread.
- **These numbers do not compare with any other server.** The load generator competes with the server for the same two cores, loopback is not a network, and the server does the minimum (no TLS, no logging, a 13-byte body). They are useful for *your* before-and-after comparisons, e.g. when you remove the `epoll_ctl` calls described in Project 9.

## Tests that find the bugs

| Bug | Symptom |
|---|---|
| Treating a partial request as an error | A client that writes headers and body in two packets gets `400`. The "every prefix is `need_more`" test catches it |
| Not keeping bytes after `consumed` | Pipelined requests lose the 2nd and 3rd |
| Stopping at the first `\r\n\r\n` without checking `Content-Length` | The body is parsed as the next request |
| Accepting `Content-Length: 5` then `Content-Length: 6` | **Request smuggling** between the proxy and server; reject |
| Whitespace before the colon allowed | Another smuggling vector (`Transfer-Encoding : chunked` seen by one parser and not the other) |
| No limits | One connection sending an endless header consumes all memory |
| A connection closed in the middle of `write_all` | `SIGPIPE` kills the process: use `MSG_NOSIGNAL` and ignore `EPIPE` |
| Handler exception propagates out of the connection task | `terminate` in the detached coroutine; one bad request kills the server |
| `shutdown()` that closes the listener while the accept coroutine sits in `epoll` | The coroutine never wakes: the reference solution makes a throwaway connection to wake it |
| Forgetting `TCP_NODELAY` | 40 ms stalls on request/response patterns (Nagle's algorithm meets delayed ACK) |

## Think about (and write answers)

1. **Slowloris.** A client that sends one byte per second holds a connection forever. This server has no timeouts. What would you add, where, and how does `co_await loop.sleep()` racing a read let you do it (a `with_timeout(task, duration)` combinator)?
2. **Back-pressure.** `write_all` suspends until the socket buffer drains, so a slow reader slows only its own connection. What happens to *memory* if a handler builds a 1 GB response before writing it? How would a streaming response body change `Response`?
3. **Why one coroutine per connection?** Compare with a state-machine-per-connection callback design. What does the coroutine give you (locals survive suspension) and cost (a heap frame per connection, per awaited task)?
4. **Parser safety.** The parser uses `std::string_view` into the connection buffer, then copies into `Request` strings. Where would you stop copying? What dangling-view hazard appears (Chapter 12) if the buffer is reallocated while a view is alive?
5. **Multi-core.** One loop per core with `SO_REUSEPORT` (the kernel balances accepts), versus one accept thread handing connections to workers. What does each do to cache locality and to the "no locks" property you currently have?
6. **Security checklist.** Which of these are not handled here: percent-decoding, path traversal if you serve files, header injection in responses, request line limits, `Expect: 100-continue`, HTTP/1.0 `Host` rules? For each: what is the attack?
7. **Why 405 and 404 differ, and what `Allow` is for.** Read RFC 9110 section 15.5.6.

## Stretch goals

- **Chunked request and response bodies**; `Expect: 100-continue`.
- **Timeouts:** header-read timeout, idle keep-alive timeout, write timeout.
- **Static file serving** with `sendfile` and path-traversal protection (`..`, symlinks, encoded `%2e%2e`).
- **Middleware:** `Task<Response>(Request&, Next)`: logging, auth, timing.
- **Streaming responses** with `co_yield` (an async generator) and `Transfer-Encoding: chunked`.
- **Multi-threaded**: N loops, `SO_REUSEPORT`; measure scaling on a machine with more cores than this VM.
- **TLS** via OpenSSL's memory BIOs, driven by the loop.
- **HTTP/1.1 conformance check** against `h2spec`-style tests or a fuzzer (`-fsanitize=fuzzer` on `parse_request` is an excellent first fuzz target).

## Reference solution notes

- **Layering:** `net` (awaitables) → `parser` (pure function of a buffer) → `router` (pure) → `Server` (coroutines). The parser and router have no I/O, so they are tested without sockets.
- **The connection loop parses before it reads**, so leftover pipelined bytes from the previous iteration are processed first.
- **Handlers are `std::function<Task<Response>(Request&)>`.** Stored lambdas are alive for as long as the router is, so coroutine lambdas with captures are safe *here*; the same lambda as a temporary would not be (Project 9). The test's `/slow` handler captures a pointer to the loop, not a reference to a local.
- **Not covered, deliberately:** TLS, HTTP/2, percent-decoding, timeouts, `Expect`, and multi-threading. Each is a stretch goal.

Run it:

```bash
cd code/p10
g++ -std=c++23 -O2 -Wall -Wextra -fsanitize=address,undefined test.cpp -o test_asan -pthread && ./test_asan
g++ -std=c++23 -O2 server_main.cpp -o server -pthread && ./server 8080
g++ -std=c++23 -O2 loadgen.cpp -o loadgen -pthread && ./loadgen 8080 100 2000
```

## What I learned (write it)

> Which part of HTTP/1.1 surprised you most when you had to parse it byte stream by byte stream?
