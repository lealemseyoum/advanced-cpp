# Chapter 35 — Coroutines and Networking: `epoll`, an Event Loop and an HTTP Server

> **Part XIII · Coroutines** &nbsp;|&nbsp; **Level 5** (systems/hardware) &nbsp;|&nbsp; **≈ 12 hours**
> **Prerequisites:** [Chapter 33](33-coroutines.md), [Chapter 34](34-building-task.md) (we reuse its `task.hpp` and `when_all.hpp`), [Chapter 4 (RAII)](../part-02-object-model-and-lifetime/) &nbsp;|&nbsp; **Standards:** C++20 coroutines ⚖️; **no networking in the C++ standard** (the Networking TS was not adopted; `std::execution` in C++26 🟡 defines schedulers but not sockets); sockets and `epoll` are **POSIX/Linux** &nbsp;|&nbsp; **Tools:** `g++-14`, ASan/UBSan, `strace`

[← Previous: Chapter 34](34-building-task.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 36 — The C++ compilation model →](../part-14-compilation-and-linking/36-compilation-model.md)

---

**In one sentence:** an asynchronous server is a loop that asks the kernel "which of these file descriptors are ready?" (`epoll_wait`) and resumes whichever coroutine was parked on each one, so each connection reads like blocking code while one thread serves thousands of them.

**By the end of this chapter you can:**

- explain `epoll` (level-triggered, edge-triggered, one-shot) and the readiness model, and contrast it with completion-based I/O (`io_uring`)
- build an `EventLoop` where `co_await loop.readable(fd)` parks a coroutine on a descriptor
- write non-blocking `accept`, `connect`, `read` and `write` as `Task`s, with correct handling of `EAGAIN`, `EINTR` and partial writes
- implement an incremental HTTP/1.1 parser, a router and a keep-alive connection handler on top
- measure the result (requests per second, syscalls per request), and state exactly what this design can and cannot do

> **Scope.** This is an educational server: HTTP/1.1 subset, no TLS, no chunked bodies, single thread. The objective is to understand the *machinery*, not to compete with nginx. Where production servers differ, the text says so.

---

## 1. Problem

A server must handle many connections whose data arrives at unpredictable times. A blocking `read()` parks the *thread* until data arrives:

| Strategy | Cost per connection | Limit |
|---|---|---|
| **Thread per connection** (blocking I/O) | ~8 MiB stack reserved, kernel thread, context switches | thousands; scheduler and memory pressure |
| **Process pool / pre-fork** | A whole process | hundreds |
| **One thread, non-blocking sockets + `select`/`poll`** | O(n) scan of all descriptors per wake-up | ~1000 (`FD_SETSIZE`, linear cost) |
| **One thread, `epoll` + callbacks** | a few hundred bytes of state; O(ready) per wake-up | 100 000+; but code is a callback state machine |
| **One thread, `epoll` + coroutines** | a coroutine frame (~100–300 bytes); O(ready) | 100 000+; code reads as blocking code |

The last row is the point of this chapter: we keep `epoll`'s scalability but get the *readability* of a thread per connection, as in:

```cpp
Task<void> serve_connection(EventLoop& L, Socket conn, const Router& routes) {
    for (;;) {
        Request req = co_await read_request(L, conn);       // suspends until a whole request arrived
        Response res = co_await routes.dispatch(req);       // a handler may itself suspend (database, timer)
        co_await write_all(L, conn.fd.get(), res.to_string());
    }
}
```

---

## 2. Historical context

| Year | Mechanism | Idea |
|---|---|---|
| 1983 | BSD `select()` | Pass a bitmap of descriptors; kernel returns ready subset. O(n) in both user and kernel, `FD_SETSIZE` = 1024 |
| 1986 | System V `poll()` | Array instead of bitmap; no fixed limit, still O(n) |
| 1999 | **The C10K problem** (Dan Kegel) | "Can one server handle 10 000 concurrent clients?" Documents why `select`/`poll` and thread-per-connection don't scale |
| 2000 | FreeBSD `kqueue` | Register interest once, fetch only ready events |
| 2002 | **Linux `epoll`** (2.5.44) | Same model on Linux: `epoll_ctl` to register, `epoll_wait` returns only ready fds. Level-triggered (default) and edge-triggered modes |
| 2003–10 | Reactor libraries: `libevent`, `libev`, `Boost.Asio` | Callback-based readiness loops |
| 2010s | Node.js, nginx | Event loop as a mainstream server architecture |
| 2019 | **Linux `io_uring`** (5.1) | *Completion*-based: submit read/write/accept operations, reap results from a ring; fewer syscalls |
| 2020 | C++20 coroutines | `co_await` gives the callback model sequential syntax |
| 2024–26 | Asio coroutine support, `std::execution` 🟡, `liburing` wrappers | The C++ ecosystem converges on coroutines over readiness or completion loops |

---

## 3. Modern solution

Three layers, each the answer to one question:

```text
   HTTP         Request / Response / Router / serve_connection        "what does a request mean?"
   ────────────────────────────────────────────────────────────────
   Awaitable    read_some / write_all / accept_one / connect_to       "how do I wait without blocking?"
   I/O          Task<T>  built on  co_await loop.readable(fd)
   ────────────────────────────────────────────────────────────────
   Reactor      EventLoop: epoll_wait + ready queue + timer heap       "who runs next?"
   ────────────────────────────────────────────────────────────────
   Kernel       sockets (non-blocking), epoll instance                 "which fds are ready?"
```

The only coupling between the loop and the coroutines is **one function**: `EventLoop::arm(fd, event, handle)`. A coroutine calls it (via `await_suspend`) to say "resume me when `fd` is readable". The loop calls `handle.resume()` when `epoll_wait` says so. Everything above that is ordinary coroutine code.

---

## 4. Mental model

### The readiness model

`epoll` reports **readiness**, not completion: “a `read()` on this socket would not block now”. You still perform the `read()` yourself, and it may still return `EAGAIN` (spurious wake-up) or fewer bytes than you asked for. That is why every I/O `Task` below is a loop:

```text
   read_some(fd):                                    write_all(fd, data):
     loop:                                             while data not empty:
        n = recv(fd, buf)            ── >= 0 ──► return n          n = send(fd, data)
        if error is EAGAIN:                                          if n >= 0: advance data; continue
             co_await loop.readable(fd)   (park)                     if EAGAIN:  co_await loop.writable(fd)
             └── retry                                               else throw
```

### What happens to a connection

```text
 client                    kernel                      EventLoop (one thread)                  coroutines
   │  connect  ──────────►  accept queue                                                       accept_loop: co_await readable(listener)
   │                         listener readable ──────► epoll_wait returns [listener]  ───────►  resume accept_loop
   │                                                                                            accept4() -> new fd; detach(serve_connection)
   │                                                                                            serve_connection: recv() = EAGAIN -> co_await readable(conn)
   │  "GET /hello ..." ───►  conn readable   ────────► epoll_wait returns [conn]      ───────►  resume serve_connection
   │                                                                                            recv() ok -> parse -> route -> send()
   │  ◄─ "HTTP/1.1 200 ..."                                                                     loop: recv() = EAGAIN -> park again
```

### The four moving parts of `EventLoop`

| Part | Data structure | Purpose |
|---|---|---|
| **epoll instance** | a kernel object, `Fd` | the interest list and the ready list, in the kernel |
| **Waiter table** | `unordered_map<int fd, {read_handle, write_handle}>` | which coroutine is parked on which fd and direction |
| **Ready queue** | `deque<coroutine_handle<>>` | coroutines posted by user code (`post`) |
| **Timer heap** | `priority_queue<Timer>` | `sleep_for`; also determines `epoll_wait`'s timeout |

`run()` is the classic reactor loop: drain ready queue → fire due timers → compute timeout → `epoll_wait` → resume woken coroutines → repeat; it returns when nothing is waiting.

### Level-triggered, edge-triggered, one-shot

| Mode | Reports | Pitfall |
|---|---|---|
| **Level-triggered** (default) | "the fd *is* ready" every `epoll_wait` until the condition clears | If nobody consumes the data, `epoll_wait` returns immediately forever (busy loop) |
| **Edge-triggered** (`EPOLLET`) | "the fd *became* ready" once | You **must** read until `EAGAIN`, or you lose wake-ups and hang |
| **One-shot** (`EPOLLONESHOT`) | Level-triggered but disabled after one event; re-arm with `EPOLL_CTL_MOD` | One `epoll_ctl` per wait; safest for multi-threaded loops |

We use **one-shot** because it matches the coroutine's "wait once, then act" shape: each `co_await readable(fd)` arms the fd, one event wakes exactly one coroutine, and the interest disappears until the next `co_await`. No busy loop, no stale interest. Its cost is an `epoll_ctl` syscall per wait (Experiment 3 measures the effect); edge-triggered with a persistent registration is the faster design and is Exercise 4.

---

## 5. Language rules and OS rules

| Topic | Rule |
|---|---|
| **C++ standard** | Says nothing about sockets or `epoll`. Only coroutine rules apply (Chapters 33–34). The Networking TS (`std::net`) was never adopted; C++26 `std::execution` provides *schedulers and senders*, not I/O 🟡 |
| **POSIX** | `socket`, `bind`, `listen`, `accept4`, `connect`, `recv`, `send`, `getsockopt` are POSIX; `epoll`, `accept4`, `SOCK_NONBLOCK` are **Linux-specific** (the BSDs use `kqueue`) |
| **Non-blocking I/O** | On a `O_NONBLOCK` socket, `recv`/`send`/`accept` return `-1` with `errno == EAGAIN` (`EWOULDBLOCK`) instead of blocking. `connect` returns `-1/EINPROGRESS`; completion is signalled by *writability*, and the result is read with `getsockopt(SO_ERROR)` |
| **Partial transfers** | `send` may write fewer bytes than asked; `recv` returns what is available; both can be interrupted (`EINTR`) |
| **`SIGPIPE`** | Writing to a closed peer raises `SIGPIPE` and kills the process by default; use `send(..., MSG_NOSIGNAL)` (Linux) |
| **Descriptor lifetime** | `close(fd)` removes the fd from the epoll set **only if no duplicate descriptors exist**. Closing an fd while a coroutine is parked on it leaves that coroutine forever suspended; and a *reused* fd number can alias a stale entry. Our `Socket` destructor calls `loop.forget(fd)` before closing |
| **Coroutine rules that bite here** | Reference parameters are safe **only because** each I/O `Task` is awaited in the same full-expression that created it (temporaries live until the end of the full-expression, which spans the suspension). Storing such a task and starting it later would dangle (Chapter 33). The parameters that must be by-value: anything a *detached* task keeps (`Socket`, strings) |

### Layer check

| Layer | Question | Answer |
|---|---|---|
| **C++ standard** | What helps? | Coroutine suspension; nothing for I/O |
| **Compiler** | Cost of the frames | One `operator new` per `Task` (Chapter 34): each `read_some` call costs a frame (~100 bytes) unless elided; a hot path can special-case "data already there" before creating the task |
| **ABI / libc** | Syscall wrappers | glibc `recv`/`send`/`epoll_*` are thin wrappers; errors via `errno` (thread-local) |
| **OS (Linux)** | What does the kernel do? | Per-socket receive/send buffers, the TCP state machine, an `epoll` red-black tree of interests and a ready list; readiness is signalled from the network softirq when data arrives |
| **CPU / NIC** | Where does time go? | At small request sizes, **syscall and TCP-stack cost dominate** (~1–3 µs per syscall on a VM, more with mitigations), not the coroutine machinery (~30 ns); loopback has no NIC or interrupt cost, a real network does |

---

## 6. Implementation model

### The event loop and I/O tasks (`net.hpp`)

A shared header used by every experiment. It requires `task.hpp` from Chapter 34 next to it (the checker supplies it).

```cpp
// @test file net.hpp
#pragma once
#include "task.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <queue>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace net {

[[noreturn]] inline void throw_errno(const char* what) { throw std::system_error(errno, std::generic_category(), what); }

// ---- Owning file descriptor (Project 1 builds this out properly) ----
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Fd& operator=(Fd&& o) noexcept { if (this != &o) { reset(); fd_ = std::exchange(o.fd_, -1); } return *this; }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }
    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    void reset() noexcept { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
private:
    int fd_ = -1;
};

// ---- The reactor ----
class EventLoop {
public:
    using Clock = std::chrono::steady_clock;

    EventLoop() : ep_(::epoll_create1(EPOLL_CLOEXEC)) { if (!ep_) throw_errno("epoll_create1"); }
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // `co_await loop.readable(fd)`: park the coroutine until fd is readable (or hung up / in error).
    struct Readiness {
        EventLoop& loop;
        int fd;
        std::uint32_t event;                                                  // EPOLLIN or EPOLLOUT
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) const { loop.arm(fd, event, h); }
        void await_resume() const noexcept {}
    };
    Readiness readable(int fd) { return {*this, fd, EPOLLIN}; }
    Readiness writable(int fd) { return {*this, fd, EPOLLOUT}; }

    auto sleep_for(Clock::duration d) {
        struct Awaiter {
            EventLoop& loop;
            Clock::time_point when;
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) const { loop.timers_.push({when, loop.seq_++, h}); }
            void await_resume() const noexcept {}
        };
        return Awaiter{*this, Clock::now() + d};
    }

    void post(std::coroutine_handle<> h) { ready_.push_back(h); }

    // Start a task now and let it clean itself up when it finishes. The wrapper owns the Task (by value).
    void detach(co::Task<void> task) {
        [](co::Task<void> t) -> co::Fire {
            try { co_await std::move(t); }
            catch (const std::exception& e) { std::fprintf(stderr, "detached task failed: %s\n", e.what()); }
            catch (...) { std::fprintf(stderr, "detached task failed\n"); }
        }(std::move(task));
    }

    // Must be called before closing an fd that this loop has seen, and only when no coroutine is parked on it.
    void forget(int fd) noexcept {
        auto it = entries_.find(fd);
        if (it == entries_.end()) return;
        if (it->second.registered) ::epoll_ctl(ep_.get(), EPOLL_CTL_DEL, fd, nullptr);
        waiting_ -= (it->second.rd ? 1 : 0) + (it->second.wr ? 1 : 0);
        entries_.erase(it);
    }

    // Run until there is nothing left to wait for.
    void run() {
        std::vector<epoll_event> events(256);
        std::vector<std::coroutine_handle<>> wake;
        for (;;) {
            while (!ready_.empty()) { auto h = ready_.front(); ready_.pop_front(); h.resume(); }
            fire_due_timers();
            if (!ready_.empty()) continue;
            if (waiting_ == 0 && timers_.empty()) return;                     // quiescent: nothing can ever wake us

            int timeout_ms = -1;
            if (!timers_.empty()) {
                auto d = std::chrono::ceil<std::chrono::milliseconds>(timers_.top().when - Clock::now());
                timeout_ms = d.count() < 0 ? 0 : static_cast<int>(d.count());
            }
            int n = ::epoll_wait(ep_.get(), events.data(), static_cast<int>(events.size()), timeout_ms);
            if (n < 0) { if (errno == EINTR) continue; throw_errno("epoll_wait"); }

            wake.clear();
            for (int i = 0; i < n; ++i) {
                auto it = entries_.find(events[i].data.fd);
                if (it == entries_.end()) continue;                            // forgotten while the event was in flight
                Entry& e = it->second;
                std::uint32_t ev = events[i].events;
                bool bad = ev & (EPOLLERR | EPOLLHUP);                         // wake both directions; the syscall reports the error
                if (e.rd && ((ev & EPOLLIN) || bad))  { wake.push_back(std::exchange(e.rd, {})); --waiting_; }
                if (e.wr && ((ev & EPOLLOUT) || bad)) { wake.push_back(std::exchange(e.wr, {})); --waiting_; }
                if (e.rd || e.wr) rearm(events[i].data.fd, e);                 // ONESHOT disabled the fd: re-enable the other direction
            }
            for (auto h : wake) h.resume();
        }
    }

private:
    struct Entry { std::coroutine_handle<> rd{}, wr{}; bool registered = false; };
    struct Timer {
        Clock::time_point when;
        std::uint64_t seq;
        std::coroutine_handle<> h;
        bool operator>(const Timer& o) const { return when != o.when ? when > o.when : seq > o.seq; }
    };

    void arm(int fd, std::uint32_t event, std::coroutine_handle<> h) {
        Entry& e = entries_[fd];
        (event == EPOLLIN ? e.rd : e.wr) = h;
        try { rearm(fd, e); }
        catch (...) { (event == EPOLLIN ? e.rd : e.wr) = {}; throw; }          // exception from await_suspend resumes the coroutine with it
        ++waiting_;
    }
    void rearm(int fd, Entry& e) {
        epoll_event ev{};
        ev.events = EPOLLONESHOT | (e.rd ? EPOLLIN : 0u) | (e.wr ? EPOLLOUT : 0u);
        ev.data.fd = fd;
        int op = e.registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        if (::epoll_ctl(ep_.get(), op, fd, &ev) < 0) {
            if (op == EPOLL_CTL_MOD && errno == ENOENT)      op = EPOLL_CTL_ADD;     // the fd number was closed and reused
            else if (op == EPOLL_CTL_ADD && errno == EEXIST) op = EPOLL_CTL_MOD;
            else throw_errno("epoll_ctl");
            if (::epoll_ctl(ep_.get(), op, fd, &ev) < 0) throw_errno("epoll_ctl");
        }
        e.registered = true;
    }
    void fire_due_timers() {
        auto now = Clock::now();
        while (!timers_.empty() && timers_.top().when <= now) { ready_.push_back(timers_.top().h); timers_.pop(); }
    }

    Fd ep_;
    std::unordered_map<int, Entry> entries_;
    std::deque<std::coroutine_handle<>> ready_;
    std::priority_queue<Timer, std::vector<Timer>, std::greater<>> timers_;
    std::uint64_t seq_ = 0;
    long waiting_ = 0;                                                         // number of parked fd waiters
};

// ---- A socket that unregisters itself from the loop before closing ----
struct Socket {
    EventLoop* loop = nullptr;
    Fd fd;
    Socket() = default;
    Socket(EventLoop& l, Fd f) noexcept : loop(&l), fd(std::move(f)) {}
    Socket(Socket&&) noexcept = default;
    Socket& operator=(Socket&& o) noexcept { if (this != &o) { release(); loop = o.loop; fd = std::move(o.fd); } return *this; }
    ~Socket() { release(); }
    void release() noexcept { if (fd && loop) loop->forget(fd.get()); fd.reset(); }
    int get() const noexcept { return fd.get(); }
};

inline void set_nodelay(int fd) { int one = 1; ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }

// ---- Awaitable socket operations: each is "try the syscall; on EAGAIN, park until ready; retry" ----
inline co::Task<ssize_t> read_some(EventLoop& L, int fd, void* buf, std::size_t n) {
    for (;;) {
        ssize_t r = ::recv(fd, buf, n, 0);
        if (r >= 0) co_return r;                                               // 0 means the peer closed
        if (errno == EINTR) continue;
        if (errno != EAGAIN) throw_errno("recv");
        co_await L.readable(fd);
    }
}

inline co::Task<void> write_all(EventLoop& L, int fd, std::string_view data) {
    while (!data.empty()) {
        ssize_t r = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (r >= 0) { data.remove_prefix(static_cast<std::size_t>(r)); continue; }   // partial write: keep going
        if (errno == EINTR) continue;
        if (errno != EAGAIN) throw_errno("send");
        co_await L.writable(fd);
    }
}

inline Socket listen_on(EventLoop& L, std::uint16_t port, std::uint16_t* bound = nullptr) {
    Fd fd{::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (!fd) throw_errno("socket");
    int one = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) throw_errno("bind");
    if (::listen(fd.get(), 128) < 0) throw_errno("listen");
    if (bound) { socklen_t len = sizeof a; ::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&a), &len); *bound = ntohs(a.sin_port); }
    return Socket{L, std::move(fd)};
}

inline co::Task<Socket> accept_one(EventLoop& L, Socket& listener) {
    for (;;) {
        int c = ::accept4(listener.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c >= 0) { set_nodelay(c); co_return Socket{L, Fd{c}}; }
        if (errno == EINTR || errno == ECONNABORTED) continue;
        if (errno != EAGAIN) throw_errno("accept4");
        co_await L.readable(listener.get());
    }
}

inline co::Task<Socket> connect_to(EventLoop& L, std::uint16_t port) {
    Fd fd{::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (!fd) throw_errno("socket");
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
        if (errno != EINPROGRESS) throw_errno("connect");
        co_await L.writable(fd.get());                                         // connect completes => writable
        int err = 0; socklen_t len = sizeof err;
        ::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &err, &len);
        if (err) { errno = err; throw_errno("connect"); }
    }
    set_nodelay(fd.get());
    co_return Socket{L, std::move(fd)};
}

}  // namespace net
```

Points worth reading closely:

- **One-shot re-arm logic.** After an event, the fd is *disabled* in the kernel. If the other direction still has a parked coroutine, `rearm` re-enables it; otherwise the entry stays registered-but-idle until the next `arm` uses `EPOLL_CTL_MOD`.
- **`wake` list, then resume.** We collect the handles first and resume afterwards, because a resumed coroutine may call `forget`, `arm`, or `detach`, mutating `entries_` while we iterate.
- **Fast path first.** `read_some` calls `recv` *before* parking, so data that is already buffered costs a syscall and no suspension (compare Chapter 33's `await_suspend` returning `false`). Only on `EAGAIN` do we pay for `epoll_ctl` + `epoll_wait`.
- **Ownership.** `Socket` forgets its fd before `Fd` closes it; `Task<Socket>` returns ownership through the promise by move.
- **Exceptions.** I/O errors become `std::system_error` thrown at the `co_await`, and `detach` logs any that escape a connection coroutine.

### The HTTP layer (`http.hpp`)

```cpp
// @test file http.hpp
#pragma once
#include "net.hpp"
#include <algorithm>
#include <charconv>
#include <functional>
#include <string>

namespace http {

struct Header { std::string name, value; };               // name stored lower-case

struct Request {
    std::string method, target, version;
    std::vector<Header> headers;
    std::string body;

    std::string_view header(std::string_view lower_name) const {
        for (const auto& h : headers) if (h.name == lower_name) return h.value;
        return {};
    }
    bool keep_alive() const {
        auto c = header("connection");
        if (version == "HTTP/1.0") return c == "keep-alive";
        return c != "close";                                // HTTP/1.1 defaults to keep-alive
    }
};

struct Response {
    int status = 200;
    std::string reason = "OK";
    std::string content_type = "text/plain";
    std::string body;

    std::string to_string(bool keep_alive) const {
        std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
        out += "Content-Type: " + content_type + "\r\n";
        out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        out += keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
        out += body;
        return out;
    }
};

enum class Parse { NeedMore, Done, Bad };
struct ParseResult { Parse status; std::size_t consumed = 0; };

inline std::string lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Parse one request from the front of `in`. Stateless and restartable: call again with more data after NeedMore.
// (Re-scanning the head each time is O(n^2) in pathological cases; fine for a teaching server -- see Exercise 2.)
inline ParseResult parse_request(std::string_view in, Request& out) {
    constexpr std::size_t kMaxHead = 16 * 1024, kMaxBody = 1 << 20;
    const auto npos = std::string_view::npos;
    auto end = in.find("\r\n\r\n");
    if (end == npos) return {in.size() > kMaxHead ? Parse::Bad : Parse::NeedMore, 0};
    if (end > kMaxHead) return {Parse::Bad, 0};
    std::string_view head = in.substr(0, end);

    auto eol = head.find("\r\n");
    std::string_view line = head.substr(0, eol == npos ? head.size() : eol);
    auto sp1 = line.find(' ');
    auto sp2 = sp1 == npos ? npos : line.find(' ', sp1 + 1);
    if (sp1 == npos || sp2 == npos) return {Parse::Bad, 0};
    out.method.assign(line.substr(0, sp1));
    out.target.assign(line.substr(sp1 + 1, sp2 - sp1 - 1));
    out.version.assign(line.substr(sp2 + 1));
    if (out.version != "HTTP/1.1" && out.version != "HTTP/1.0") return {Parse::Bad, 0};

    out.headers.clear();
    std::size_t pos = eol == npos ? head.size() : eol + 2;
    while (pos < head.size()) {
        auto e = head.find("\r\n", pos);
        if (e == npos) e = head.size();
        std::string_view ln = head.substr(pos, e - pos);
        auto colon = ln.find(':');
        if (colon == npos) return {Parse::Bad, 0};
        out.headers.push_back({lower(ln.substr(0, colon)), std::string(trim(ln.substr(colon + 1)))});
        pos = e + 2;
    }

    std::size_t len = 0;
    if (auto cl = out.header("content-length"); !cl.empty()) {
        auto [p, ec] = std::from_chars(cl.data(), cl.data() + cl.size(), len);
        if (ec != std::errc{} || p != cl.data() + cl.size() || len > kMaxBody) return {Parse::Bad, 0};
    }
    std::size_t total = end + 4 + len;
    if (in.size() < total) return {Parse::NeedMore, 0};                  // body not complete yet
    out.body.assign(in.substr(end + 4, len));
    return {Parse::Done, total};
}

// ---- Router: "METHOD target" -> coroutine handler ----
class Router {
public:
    using Handler = std::function<co::Task<Response>(const Request&)>;
    void add(std::string method, std::string target, Handler h) { routes_[std::move(method) + ' ' + std::move(target)] = std::move(h); }

    co::Task<Response> dispatch(const Request& req) const {
        auto it = routes_.find(req.method + ' ' + req.target);
        if (it == routes_.end()) co_return Response{404, "Not Found", "text/plain", "no route for " + req.method + " " + req.target + "\n"};
        co_return co_await it->second(req);                              // handlers may suspend (timers, I/O)
    }
private:
    std::unordered_map<std::string, Handler> routes_;
};

// ---- One connection = one coroutine: read request, route, write response, repeat while keep-alive ----
inline co::Task<void> serve_connection(net::EventLoop& L, net::Socket conn, const Router& router) {
    std::string buf;
    char tmp[4096];
    for (;;) {
        Request req;
        ParseResult pr;
        while ((pr = parse_request(buf, req)).status == Parse::NeedMore) {
            ssize_t n = co_await net::read_some(L, conn.get(), tmp, sizeof tmp);
            if (n == 0) co_return;                                       // peer closed between requests
            buf.append(tmp, static_cast<std::size_t>(n));
        }
        if (pr.status == Parse::Bad) {
            co_await net::write_all(L, conn.get(), Response{400, "Bad Request", "text/plain", "bad request\n"}.to_string(false));
            co_return;
        }
        buf.erase(0, pr.consumed);                                        // keep pipelined bytes of the next request
        bool ka = req.keep_alive();
        Response res = co_await router.dispatch(req);
        co_await net::write_all(L, conn.get(), res.to_string(ka));        // the temporary string lives until the write completes
        if (!ka) co_return;
    }
}

inline co::Task<void> accept_loop(net::EventLoop& L, net::Socket& listener, const Router& router, int connections) {
    for (int i = 0; i < connections; ++i) {
        net::Socket c = co_await net::accept_one(L, listener);
        L.detach(serve_connection(L, std::move(c), router));              // by-value Socket moves into the new frame
    }
}

}  // namespace http
```

The shape of `serve_connection` is the entire pitch: a loop with `co_await` where a thread-per-connection server would block. The state that a callback design spreads across a struct (`buf`, the half-parsed request, “am I writing or reading?”) is ordinary local variables.

---

## 7. Experiments

### Experiment 1 ✅: A real server, real clients, one thread

The server and four clients all run on **one thread, one `EventLoop`**. One client sends two keep-alive requests on a single connection; one hits `/slow`, a handler that `co_await`s a 100 ms timer; one posts a body; one asks for a missing route. `when_all` (Chapter 34) runs the clients concurrently.

```cpp
// @test run -std=c++23 -O1 -g -fsanitize=address,undefined timeout=60
#include "http.hpp"
#include "when_all.hpp"
#include <chrono>
#include <cstdio>

using namespace std::chrono_literals;
using http::Request;
using http::Response;

// ---- client helpers (they run on the same event loop) ----
static co::Task<std::string> exchange_until_close(net::EventLoop& L, std::uint16_t port, std::string request) {
    net::Socket s = co_await net::connect_to(L, port);
    co_await net::write_all(L, s.get(), request);
    std::string resp;
    char buf[4096];
    for (;;) {
        ssize_t n = co_await net::read_some(L, s.get(), buf, sizeof buf);
        if (n == 0) break;
        resp.append(buf, static_cast<std::size_t>(n));
    }
    co_return resp;
}

// Read one response whose Content-Length we find in the headers.
static co::Task<std::string> read_one_response(net::EventLoop& L, int fd, std::string& buf) {
    char tmp[4096];
    for (;;) {
        auto he = buf.find("\r\n\r\n");
        if (he != std::string::npos) {
            auto cl = buf.find("Content-Length: ");
            std::size_t len = cl == std::string::npos ? 0 : std::stoul(buf.substr(cl + 16));
            if (buf.size() >= he + 4 + len) { std::string r = buf.substr(0, he + 4 + len); buf.erase(0, he + 4 + len); co_return r; }
        }
        ssize_t n = co_await net::read_some(L, fd, tmp, sizeof tmp);
        if (n == 0) throw std::runtime_error("connection closed early");
        buf.append(tmp, static_cast<std::size_t>(n));
    }
}

static std::string summarise(const std::string& r) {          // "200 OK | body"
    auto sp = r.find(' '), eol = r.find("\r\n"), body = r.find("\r\n\r\n");
    std::string text = r.substr(body + 4);
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return r.substr(sp + 1, eol - sp - 1) + " | " + text;
}

int main() {
    net::EventLoop L;
    std::uint16_t port = 0;
    net::Socket listener = net::listen_on(L, 0, &port);        // port 0: the kernel picks a free port

    http::Router router;
    router.add("GET", "/hello", [](const Request&) -> co::Task<Response> { co_return Response{200, "OK", "text/plain", "Hello, coroutines!\n"}; });
    router.add("GET", "/slow", [&L](const Request&) -> co::Task<Response> {
        co_await L.sleep_for(100ms);                           // suspends only THIS connection; the loop keeps serving others
        co_return Response{200, "OK", "text/plain", "slow response\n"};
    });
    router.add("POST", "/echo", [](const Request& r) -> co::Task<Response> { co_return Response{200, "OK", "text/plain", "you sent: " + r.body}; });

    std::vector<std::string> results(5);
    auto t0 = net::EventLoop::Clock::now();

    auto keepalive_client = [&]() -> co::Task<void> {          // two requests on ONE connection
        net::Socket s = co_await net::connect_to(L, port);
        std::string buf;
        for (int i = 0; i < 2; ++i) {
            co_await net::write_all(L, s.get(), "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n");
            results[i] = summarise(co_await read_one_response(L, s.get(), buf));
        }
    };
    auto slow_client = [&]() -> co::Task<void> {
        results[2] = summarise(co_await exchange_until_close(L, port, "GET /slow HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
    };
    auto post_client = [&]() -> co::Task<void> {
        results[3] = summarise(co_await exchange_until_close(L, port, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello"));
    };
    auto missing_client = [&]() -> co::Task<void> {
        results[4] = summarise(co_await exchange_until_close(L, port, "GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
    };

    auto clients = [&]() -> co::Task<void> {
        std::vector<co::Task<void>> v;
        v.push_back(keepalive_client());
        v.push_back(slow_client());
        v.push_back(post_client());
        v.push_back(missing_client());
        co_await co::when_all(std::move(v));
    };

    L.detach(http::accept_loop(L, listener, router, 4));       // the server: accepts exactly 4 connections, then returns
    L.detach(clients());
    L.run();                                                   // returns when no fd and no timer is pending

    double ms = std::chrono::duration<double, std::milli>(net::EventLoop::Clock::now() - t0).count();
    for (int i = 0; i < 5; ++i) std::printf("response %d: %s\n", i, results[i].c_str());
    std::printf("total wall time under 250 ms (the 100 ms sleep overlapped the other requests): %s\n", ms < 250 ? "yes" : "NO");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
response 0: 200 OK | Hello, coroutines!
response 1: 200 OK | Hello, coroutines!
response 2: 200 OK | slow response
response 3: 200 OK | you sent: hello
response 4: 404 Not Found | no route for GET /nope
total wall time under 250 ms (the 100 ms sleep overlapped the other requests): yes
```

What just happened: **one thread** served four connections concurrently, one of which was parked in a 100 ms timer while the others completed. The clients are coroutines too (`connect_to`, `write_all`, `read_some` are the same awaitables the server uses), and the whole program runs under AddressSanitizer and UBSan with no complaints: every `Socket`, `Task` and detached `Fire` frame is released, and `forget` runs before every `close`. The server's `accept_loop` was told to accept exactly four connections so that the loop becomes quiescent and `run()` returns; a real server runs forever and shuts down by closing the listener.

### Experiment 2 ✅: The parser, tested the way TCP actually delivers data

TCP is a byte stream: a request can arrive in any fragmentation. The right test is *every* split of a request. This one feeds the request one byte at a time, then splits it at every position, and checks the result is identical to a single parse; it also tests pipelining and malformed input.

```cpp
// @test run -std=c++23 -O1 -g -fsanitize=address,undefined
#include "http.hpp"
#include <cassert>
#include <cstdio>

using namespace http;

int main() {
    const std::string req = "POST /echo?x=1 HTTP/1.1\r\nHost: example\r\nContent-Length: 11\r\nX-Mixed-Case:   padded  \r\n\r\nhello world";

    Request whole;
    auto r = parse_request(req, whole);
    assert(r.status == Parse::Done && r.consumed == req.size());
    assert(whole.method == "POST" && whole.target == "/echo?x=1" && whole.body == "hello world");
    assert(whole.header("x-mixed-case") == "padded");                       // names lower-cased, values trimmed

    // 1. Feed one byte at a time: NeedMore until the very last byte, then Done with the same result.
    int needmore = 0;
    for (std::size_t n = 1; n <= req.size(); ++n) {
        Request part;
        auto pr = parse_request(std::string_view(req).substr(0, n), part);
        if (n < req.size()) { assert(pr.status == Parse::NeedMore); ++needmore; }
        else { assert(pr.status == Parse::Done && part.body == whole.body); }
    }

    // 2. Pipelining: two requests in one buffer; `consumed` tells where the second starts.
    std::string two = "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n";
    Request a, b;
    auto ra = parse_request(two, a);
    auto rb = parse_request(std::string_view(two).substr(ra.consumed), b);
    assert(a.target == "/a" && b.target == "/b" && ra.consumed + rb.consumed == two.size());

    // 3. Malformed input is rejected, not crashed on.
    int bad = 0;
    for (std::string_view m : {"GARBAGE\r\n\r\n", "GET / HTTP/9.9\r\n\r\n", "GET / HTTP/1.1\r\nNoColonHere\r\n\r\n",
                               "POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n", "POST / HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n"}) {
        Request x;
        if (parse_request(m, x).status == Parse::Bad) ++bad;
    }
    std::string huge(20'000, 'a');                                          // no terminator, over the header limit
    Request x;
    if (parse_request(huge, x).status == Parse::Bad) ++bad;

    std::printf("byte-at-a-time: %d NeedMore results then Done\npipelined: consumed %zu + %zu = %zu bytes\nmalformed inputs rejected: %d of 6\n",
                needmore, ra.consumed, rb.consumed, two.size(), bad);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
byte-at-a-time: 98 NeedMore results then Done
pipelined: consumed 19 + 19 = 38 bytes
malformed inputs rejected: 6 of 6
```

This is the test that catches the common server bugs: assuming one `recv` equals one request, losing the second of two pipelined requests, and trusting `Content-Length`. Run it under a fuzzer (Exercise 3) before you trust the parser.

### Experiment 3 🔧: How fast is it? Requests per second and syscalls per request

20 keep-alive connections, 1 000 requests each, all on one thread, server and clients together (so the figure includes the *client's* work).

```cpp
// @test run -std=c++23 -O2 -DNDEBUG timeout=120
#include "http.hpp"
#include "when_all.hpp"
#include <chrono>
#include <cstdio>

using http::Request;
using http::Response;

static co::Task<void> client(net::EventLoop& L, std::uint16_t port, int requests, long& total) {
    net::Socket s = co_await net::connect_to(L, port);
    static const std::string req = "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n";
    char buf[512];
    for (int i = 0; i < requests; ++i) {
        co_await net::write_all(L, s.get(), req);
        std::string resp;
        while (resp.size() < 4 || resp.compare(resp.size() - 4, 4, "pong") != 0) {         // the response body is "pong"
            ssize_t n = co_await net::read_some(L, s.get(), buf, sizeof buf);
            if (n == 0) throw std::runtime_error("closed");
            resp.append(buf, static_cast<std::size_t>(n));
        }
        ++total;
    }
}

int main() {
    constexpr int kClients = 20, kRequests = 1000;
    net::EventLoop L;
    std::uint16_t port = 0;
    net::Socket listener = net::listen_on(L, 0, &port);
    http::Router router;
    router.add("GET", "/ping", [](const Request&) -> co::Task<Response> { co_return Response{200, "OK", "text/plain", "pong"}; });

    long total = 0;
    auto all = [&]() -> co::Task<void> {
        std::vector<co::Task<void>> v;
        for (int i = 0; i < kClients; ++i) v.push_back(client(L, port, kRequests, total));
        co_await co::when_all(std::move(v));
    };
    auto t0 = std::chrono::steady_clock::now();
    L.detach(http::accept_loop(L, listener, router, kClients));
    L.detach(all());
    L.run();
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%ld requests in %.2f s: %.0f requests/s, %.1f us per request (server and %d clients share one thread)\n",
                total, s, total / s, s / total * 1e6, kClients);
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
20000 requests in 0.21 s: 94129 requests/s, 10.6 us per request (server and 20 clients share one thread)
```

Now count what the *kernel* is asked to do per request (run by hand; not auto-verified; the program is the one above, built with `-O2`):

```bash
g++-14 -std=c++23 -O2 -DNDEBUG -I part-13-coroutines/code bench.cpp -o bench
strace -c -f ./bench 2>&1 | head -12
```

```text
% time     seconds  usecs/call     calls    errors syscall
 43.48    1.243091          15     80040     40020 recvfrom
 33.52    0.958486          23     40000           sendto
 21.22    0.606838          15     40082           epoll_ctl
  1.55    0.044186          22      2002           epoll_wait
  0.06    0.001609          34        47           close
  ...                                                (the remaining lines are setup: socket, connect, accept4, ...)
```

(The `seconds` column is inflated by `strace` itself; only the **call counts** are meaningful.) For 20 000 requests:

| Syscall | Calls | Per request | Why |
|---|---|---|---|
| `recvfrom` | 80 040 (of which **40 020 failed with `EAGAIN`**) | 4 (2 failed) | server and client each `recv` first, find nothing (`EAGAIN`), park, wake, and `recv` again |
| `sendto` | 40 000 | 2 | one response by the server, one request by the client |
| `epoll_ctl` | 40 082 | 2 | one-shot re-arm after every wake (`EPOLL_CTL_MOD`) |
| `epoll_wait` | 2 002 | **0.1** | each call returns about ten ready events: the loop *batches* wake-ups |
| **total** | | **about 8** | |

The structure of the cost is visible at once. Two things are worth noticing. First, **`epoll_wait` is the cheapest part**: one call serves many connections, which is the whole point of a reactor. Second, **half the `recv` calls are wasted**: the "try the syscall first" fast path misses on this ping-pong traffic, because each side asks for data right after sending and the peer has not answered yet. A connection that is idle between requests should *park first* and read after the wake-up, and a persistent edge-triggered registration removes the `epoll_ctl` calls too (Exercise 4); together those would cut about 8 syscalls to about 4. The coroutine machinery (tens of nanoseconds per `Task`, Chapter 34) is invisible next to this: the roughly 10 µs per request is almost entirely the kernel boundary and TCP stack (the measured rate varies run to run on this VM, about 70 000 to 100 000 requests/s). That is the lesson of this part of the course: **at network speeds the bottleneck is the number of syscalls**, so the optimisations that matter are fewer syscalls (edge-triggered persistent registration, park-first reads, batching with `io_uring`, `MSG_ZEROCOPY`), not faster coroutines. Production servers (nginx, Seastar) apply exactly these techniques and run on multiple cores.

### Experiment 4 ✅: The cardinal rule: one blocking call stalls the whole loop

```cpp
// @test run -std=c++23 -O1 timeout=60
#include "http.hpp"
#include "when_all.hpp"
#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono_literals;
using http::Request;
using http::Response;

static co::Task<void> fetch(net::EventLoop& L, std::uint16_t port, const char* path) {
    net::Socket s = co_await net::connect_to(L, port);
    std::string req = std::string("GET ") + path + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    co_await net::write_all(L, s.get(), req);
    char buf[512];
    while (co_await net::read_some(L, s.get(), buf, sizeof buf) > 0) {}
}

static double run_three(const char* path) {
    net::EventLoop L;
    std::uint16_t port = 0;
    net::Socket listener = net::listen_on(L, 0, &port);
    http::Router router;
    router.add("GET", "/async", [&L](const Request&) -> co::Task<Response> { co_await L.sleep_for(100ms); co_return Response{200, "OK", "text/plain", "ok"}; });
    router.add("GET", "/blocking", [](const Request&) -> co::Task<Response> { std::this_thread::sleep_for(100ms); co_return Response{200, "OK", "text/plain", "ok"}; });   // BUG
    auto clients = [&]() -> co::Task<void> {
        std::vector<co::Task<void>> v;
        for (int i = 0; i < 3; ++i) v.push_back(fetch(L, port, path));
        co_await co::when_all(std::move(v));
    };
    auto t0 = net::EventLoop::Clock::now();
    L.detach(http::accept_loop(L, listener, router, 3));
    L.detach(clients());
    L.run();
    return std::chrono::duration<double, std::milli>(net::EventLoop::Clock::now() - t0).count();
}

int main() {
    double a = run_three("/async"), b = run_three("/blocking");
    std::printf("3 concurrent requests, handler sleeps 100 ms\n  co_await sleep_for : about 1x (%s)\n  thread sleep       : about 3x (%s)\n",
                a < 200 ? "overlapped" : "NOT overlapped", b > 280 ? "serialized" : "NOT serialized");
}
```

```text
# output (gcc 14.2.0, x86-64 Linux)
3 concurrent requests, handler sleeps 100 ms
  co_await sleep_for : about 1x (overlapped)
  thread sleep       : about 3x (serialized)
```

Same handler, same load; the only difference is `co_await L.sleep_for` versus `std::this_thread::sleep_for`. The blocking call stops the **entire thread**, so every other connection on this loop stalls for 100 ms, and three requests take three times as long. The same happens with a slow database driver, `getaddrinfo`, a file read on a network filesystem, a mutex held by another thread, or a CPU-heavy handler. Rules for a single-threaded reactor: never block, never do long CPU work, and push the unavoidable to a thread pool (`co_await pool.schedule()`, Chapter 34 Experiment 4, then hop back).

---

## 8. Assembly / runtime investigation

```bash
# 1. Watch the syscall stream for one request (the picture in §4, for real)
strace -f -tt -e trace=network,epoll_wait,epoll_ctl,read,write,recvfrom,sendto ./server 2>&1 | head -60
#   epoll_ctl(ADD/MOD, fd, {EPOLLIN|EPOLLONESHOT})  -> epoll_wait -> recvfrom -> sendto -> epoll_ctl(MOD) ...

# 2. Per-syscall counts and time
strace -c -f ./server           # what dominates?  recvfrom/sendto/epoll_ctl/epoll_wait

# 3. Kernel-side state: sockets, queues, epoll set
ss -tnpi                        # per-connection send/recv queue sizes, retransmits, congestion window
cat /proc/$(pidof server)/fdinfo/<epoll-fd>      # the interest list the kernel holds (tfd: ... events: ...)
ls -l /proc/$(pidof server)/fd | wc -l          # open descriptors

# 4. Where does the user-space time go?
perf record -g ./server && perf report --stdio | head -50     # expect: syscall entry/exit, TCP stack in the kernel; little in user code
perf stat -e context-switches,cpu-migrations ./server        # a single-threaded reactor: context switches ~ blocking syscalls only

# 5. Frame allocations per connection (Chapter 34): count operator new calls under a load run
ltrace -c -e malloc ./server 2>&1 | tail -3

# 6. Limits that bite at scale
ulimit -n                       # max open fds per process (often 1024 by default!)
sysctl net.core.somaxconn net.ipv4.tcp_max_syn_backlog
# raise: ulimit -n 1048576 ; and check /proc/sys/fs/file-max
```

---

## 9. Implementation exercise

Turn the pieces into the standard server components, each with a test:

1. **Idle timeout.** Close connections that send nothing for 5 s. You need a `read_some` that races a timer: implement `with_timeout(Task<T>, duration)` (hint: a shared state flag; whichever finishes first resumes the awaiter, the other result is discarded; cancel the I/O by closing the fd or removing the epoll registration).
2. **Request limits and a streaming body.** Cap total in-flight bytes per connection and support `Transfer-Encoding: chunked` bodies without buffering them entirely.
3. **Graceful shutdown.** `stop_source`-driven: stop accepting, let in-flight requests finish, close idle keep-alive connections, then exit `run()`.
4. **Fewer syscalls.** Replace one-shot re-arm with a persistent `EPOLLET` registration, and make idle connections read *park-first*. Re-run Experiment 3's `strace -c`: the 8 syscalls per request should fall to about 4 with the `EAGAIN` misses gone. Write the “read until EAGAIN” invariant down as a comment.
5. **Multi-core.** One `EventLoop` per thread, each with its own listener on the same port via `SO_REUSEPORT` (kernel load-balances accepts); measure scaling on as many cores as you have.

<details>
<summary><strong>Solution sketch: `with_timeout`</strong></summary>

The core trick is a **first-finisher-wins** latch that both the operation and the timer try to claim; the loser's result is discarded. Two lifetime rules make it safe: the shared state lives on the heap (`shared_ptr`), because `with_timeout` may return *before* the loser finishes; and the operation `Task` is moved **into the runner coroutine's frame**, so the abandoned operation keeps its frame alive until it completes by itself (an earlier version of this sketch destroyed it while it was still parked in the timer queue, and AddressSanitizer reported the use-after-free).

```cpp
// @test run -std=c++23 -O1 -g -fsanitize=address,undefined timeout=60
#include "loop.hpp"
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>

using namespace co;
using namespace std::chrono_literals;

struct timeout_error : std::runtime_error { timeout_error() : std::runtime_error("timed out") {} };

template <class T>
struct RaceState {
    bool settled = false, timed_out = false;
    std::optional<T> value;
    std::exception_ptr error;
    std::coroutine_handle<> waiter{};
    void settle() { if (waiter) std::exchange(waiter, {}).resume(); }      // wake the awaiting coroutine, once
};

// Run `op` and a timer concurrently; return whichever finishes first. The loser is NOT cancelled: it runs to completion.
template <class T>
Task<T> with_timeout(Loop& L, Task<T> op, std::chrono::milliseconds d) {
    auto st = std::make_shared<RaceState<T>>();

    auto runner = [](Task<T> t, std::shared_ptr<RaceState<T>> s) -> Fire {      // owns `t` until it finishes, win or lose
        try {
            auto v = co_await std::move(t);
            if (!s->settled) { s->settled = true; s->value.emplace(std::move(v)); s->settle(); }
        } catch (...) {
            if (!s->settled) { s->settled = true; s->error = std::current_exception(); s->settle(); }
        }
    };
    auto timer = [](Loop& L, std::chrono::milliseconds d, std::shared_ptr<RaceState<T>> s) -> Fire {
        co_await L.sleep_for(d);
        if (!s->settled) { s->settled = true; s->timed_out = true; s->settle(); }
    };
    runner(std::move(op), st);
    timer(L, d, st);

    struct Wait {
        RaceState<T>& s;
        bool await_ready() const noexcept { return s.settled; }
        void await_suspend(std::coroutine_handle<> h) const noexcept { s.waiter = h; }
        void await_resume() const noexcept {}
    };
    co_await Wait{*st};
    if (st->timed_out) throw timeout_error{};
    if (st->error) std::rethrow_exception(st->error);
    co_return std::move(*st->value);
}

Task<int> slow(Loop& L, std::chrono::milliseconds d) { co_await L.sleep_for(d); co_return 7; }

Task<void> demo(Loop& L) {
    std::printf("fast op: %d\n", co_await with_timeout(L, slow(L, 20ms), 100ms));
    try { co_await with_timeout(L, slow(L, 200ms), 50ms); }
    catch (const timeout_error& e) { std::printf("slow op: %s (the abandoned operation still runs to completion in the background)\n", e.what()); }
}

int main() { Loop L; L.spawn(demo(L)); L.run(); }
```

```text
# output (gcc 14.2.0, x86-64 Linux)
fast op: 7
slow op: timed out (the abandoned operation still runs to completion in the background)
```

Caveats the sketch exposes: the timed-out operation is *abandoned, not cancelled*: it still consumes its resources until it finishes (here 150 ms more), and `Loop::run()` waits for it. For sockets this means the connection is in an unknown state and must be closed. This is why production designs integrate **cancellation** (Chapter 34 §9): the loser has to be told to stop and given a chance to unwind.

</details>

---

## 10. Real-world example

| System | Architecture | Relation to this chapter |
|---|---|---|
| **nginx** | One single-threaded event loop per worker process (`epoll`, edge-triggered), state machines in C, no coroutines | Same reactor, hand-written state machines; `SO_REUSEPORT`/accept-mutex across workers |
| **Seastar / ScyllaDB** | Shard-per-core, share-nothing; futures and C++20 coroutines over `epoll` or `io_uring`; own TCP stack optional | Same programming model, multi-core by sharding |
| **Boost.Asio / Beast** | `io_context` reactor (or `io_uring` backend); `awaitable<T>` coroutines; completion tokens | The mainstream C++ answer; our `net.hpp` is a tiny Asio |
| **libuv (Node.js)** | `epoll` event loop + thread pool for file I/O and DNS | Why Node offloads blocking work: the cardinal rule of Experiment 4 |
| **Tokio (Rust)** | `epoll` via `mio`, work-stealing multi-threaded executor of `Future`s | Same idea, poll-based and multi-threaded |
| **Qt** | The Qt event loop is a `select`/`poll`/`epoll` reactor over `QSocketNotifier`; `QTcpSocket` emits `readyRead`; QCoro adapts it to `co_await` | Chapter 47 |
| **Redis** | Single-threaded `epoll` event loop (+ I/O threads since 6.0) | A famous demonstration that one thread can serve 100k+ requests/s when handlers never block |

> **Opinion.** For **application** code, do not build this; use Asio (or the framework your platform ships) and spend your time on the protocol. Build your own once, as you just did, so you can *read* Asio's source, debug a stuck connection with `strace`, and know what a reactor does and costs. For **very high connection counts or low latency**, reach for a mature library (Seastar, Asio with `io_uring`) before writing syscalls yourself. In production the details you did not implement matter most: backpressure, timeouts on every phase, connection limits, TLS, graceful shutdown, `SO_REUSEPORT` across cores, HTTP request smuggling defences, and observability. Also note what coroutines buy here: *readability and safety of control flow*, not speed. The speed comes from `epoll` and from the number of syscalls you avoid.

---

## 11. Failure modes

| Mistake | Symptom | Fix |
|---|---|---|
| **A blocking call in a handler** (sleep, blocking DB driver, `getaddrinfo`, big CPU loop, contended mutex) | Every connection on that loop stalls (Experiment 4) | Never block the loop; offload to a pool and hop back, or use async clients |
| **Level-triggered fd never consumed** | `epoll_wait` returns instantly, 100 % CPU | Read until `EAGAIN`, or use one-shot/edge appropriately |
| **Edge-triggered, read only once** | Connection hangs with data in the buffer | Loop until `EAGAIN` (or stay level-triggered/one-shot) |
| **Closing an fd while a coroutine is parked on it** | The coroutine never resumes (leak); or a reused fd number aliases the stale entry | `Socket` destructor `forget`s first; cancel waiters before close; `epoll_ctl(DEL)` explicitly |
| **Ignoring partial writes** | Truncated responses under load | `write_all` loops; use `writable` on `EAGAIN` |
| **`SIGPIPE` on a closed peer** | Process killed silently | `MSG_NOSIGNAL` (or ignore `SIGPIPE`) |
| **Assuming `recv` returns one request / one line** | Works on localhost, fails on a real network | Parse from a buffer; test every split (Experiment 2) |
| **Trusting `Content-Length` / unbounded buffers** | Memory exhaustion, request smuggling | Limits on header and body size; reject ambiguous framing (both `Content-Length` and `Transfer-Encoding`) |
| **`EINTR` / `ECONNABORTED` treated as fatal** | Random accept/recv failures | Retry loops as written |
| **Reference parameters on a detached task** | Use-after-free after the caller returns (Chapter 33) | By-value `Socket` and strings into detached tasks; reference only when the task is awaited in the same full-expression |
| **`co_await` on an fd from a different loop/thread** | Data race on `entries_` | One loop per thread; cross-thread wake-ups through a thread-safe queue + `eventfd` |
| **Unbounded connections/fds** | `EMFILE` ("too many open files"), accept loop spins | `ulimit -n`, limit concurrent connections, stop accepting at the limit |
| **No timeouts** | Slow-loris clients pin connections forever | Per-phase timeouts (Exercise 1) |
| **Exceptions escaping a detached task** | `std::terminate` (or silent loss) | `detach` catches and logs (as written); decide a policy (close the connection) |
| **Starvation by one busy connection** | A client that always has data monopolises the loop | Limit work per wake-up; `yield` between iterations |

---

## 12. Exercises

1. **Trace a request.** Run Experiment 1's server with `strace -f -tt` and annotate each syscall with the C++ function and coroutine state responsible (use §4's diagram).
2. **Parser.** Make `parse_request` O(n) over the connection lifetime: remember the scan offset between calls; limit header count; reject duplicate `Content-Length`. Add the property tests of Experiment 2 for each.
3. **Fuzz it.** Write a libFuzzer harness for `parse_request` (`-fsanitize=fuzzer,address,undefined`) and run it for 10 minutes; fix what it finds.
4. **Edge-triggered rewrite.** Implement persistent `EPOLLET` registration (ADD once; wakes deliver readiness for both directions into per-fd flags) and show the `epoll_ctl` count falling to ~one per connection. Preserve correctness for "data arrived before the coroutine parked".
5. **Timers with `timerfd`.** Replace the timer heap with a single `timerfd` registered in the epoll set (a common pattern), and compare timer accuracy.
6. **Backpressure.** Add a per-connection output queue limit; when a client reads slowly, stop reading its requests (pause the connection) until the buffer drains.
7. **`io_uring` loop.** Reimplement `read_some`, `write_all`, `accept_one` on top of `liburing` completion operations (the awaiter submits an SQE with the handle in `user_data`; the loop reaps CQEs and resumes). Compare syscalls per request with Experiment 3.
8. **Compare with Asio.** Write the same `/ping` server with Boost.Asio coroutines (`awaitable`), measure requests/s on your machine, and read how its `io_context` differs from `EventLoop`.

---

## 13. Challenge: the capstone runtime

This chapter is the seed of the course capstone (`part-19-projects/capstone.md`). Extend it into a **small asynchronous runtime library**:

- **Event loop** with timers (`timerfd`), cancellation (`stop_token` through awaiters) and an `eventfd` to wake it from other threads
- **Thread pool** scheduler plus `resume_on(executor)` awaiter, so handlers can offload blocking work and return to their home loop
- **TCP + HTTP**: this chapter's server with idle timeouts, request limits, graceful shutdown, keep-alive and pipelining
- **Memory**: frame allocator for `Task` (Chapter 26/34), per-connection arena for request parsing
- **Errors**: `std::expected` at module boundaries, exceptions inside coroutines, never leaked across the C ABI
- **Verification**: unit tests for the parser (fuzzed), a deterministic fake-time test harness for the loop, ASan/TSan/UBSan CI, and a benchmark reporting requests/s, p50/p99 latency and syscalls per request against a plain `epoll` C++ baseline and (optionally) Asio

Write a one-page design note answering: **what does this runtime guarantee, and what does it deliberately not do?**

---

## 14. Knowledge check

1. What does `epoll` report, readiness or completion? Why does every I/O `Task` loop on `EAGAIN`?
2. Compare level-triggered, edge-triggered and one-shot. Which does our loop use and why? What's the cost?
3. What is the only coupling point between the event loop and the coroutines? Which two functions implement each side of it?
4. Why does `run()` collect the handles in `wake` and resume them after the event loop, rather than resuming inside the loop?
5. Why does `Socket`'s destructor call `forget` before the descriptor is closed? What goes wrong otherwise?
6. How does a non-blocking `connect` complete, and how do you learn whether it succeeded?
7. Why is it safe that `write_all(L, fd, res.to_string(ka))` takes a `string_view` of a temporary, when Chapter 33 says reference parameters are dangerous?
8. Why must `L.detach(serve_connection(L, std::move(c), router))` pass the `Socket` by value?
9. Explain exactly why one `std::this_thread::sleep_for` in a handler serialises three concurrent requests.
10. A request arrives split across three `recv` calls, or two requests arrive in one. Where in the code is each case handled?
11. Per request, where does the time go in Experiment 3, and what does that imply about optimising such a server?
12. State two things `io_uring` changes compared with `epoll`, and how an awaiter would use it.

<details>
<summary><strong>Answers</strong> (try first)</summary>

1. Readiness: "an operation would not block now". The data can be consumed by another party, the readiness can be spurious, and a `recv`/`send` may transfer fewer bytes than requested, so you retry the syscall, parking on `EAGAIN`.
2. Level: fires every wait while the condition holds (busy loop if unconsumed); edge: fires once per transition (must drain to `EAGAIN`); one-shot: level-triggered but disarmed after one event (must re-arm with `EPOLL_CTL_MOD`). We use one-shot: it matches "wait once, then act" and leaves no stale interest. Cost: an `epoll_ctl` per wait.
3. `EventLoop::arm(fd, event, handle)` called from `Readiness::await_suspend`, and `handle.resume()` called by `run()` when `epoll_wait` reports the fd.
4. Resumed coroutines may call `forget`, `arm`, `detach`, or close sockets, mutating `entries_` and invalidating the iteration state; collecting first makes the dispatch loop independent of those changes.
5. A parked entry for a closed fd lingers; if the kernel reuses the descriptor number for a new socket, the stale `registered` flag makes the loop use `MOD` on an fd not in the set (we handle `ENOENT`), or wake the wrong coroutine; `forget` also removes it from the epoll set, so no events for a dead fd are delivered.
6. `connect` returns `-1` with `EINPROGRESS`; the socket becomes writable when the handshake completes (or fails); then read `SO_ERROR` with `getsockopt` (0 = success).
7. The write task is awaited in the same full-expression that creates the temporary string; temporaries live until the end of the full-expression, which includes the suspension and completion of the `co_await`. The danger in Chapter 33 was a coroutine started *later*, after the temporary had died.
8. A detached task outlives the caller's scope (the accept loop's iteration); a reference would dangle. The by-value parameter is moved into the new coroutine frame, which owns it until completion.
9. The coroutines share one thread; a blocking call stops the thread, so the loop cannot run any other coroutine (accept, read, write, timers) during those 100 ms; the three handlers' sleeps execute back to back.
10. Both in `serve_connection`: the `while (parse_request(...) == NeedMore)` loop appends more data to `buf` until the request is complete (fragmentation); `buf.erase(0, pr.consumed)` keeps the bytes of the next request for the next iteration (pipelining).
11. In the kernel boundary: `recv`/`send`/`epoll_ctl`/`epoll_wait` syscalls and the TCP stack dominate; coroutine overhead is tens of ns. Optimise by reducing syscalls (edge-triggered persistent registration, batching, `io_uring`), by multi-core sharding, and by avoiding copies, not by tuning the coroutine layer.
12. It is completion-based (you submit the operation and are told when it is *done*, data included), and it batches submissions/completions through shared rings (fewer syscalls, optionally none with SQ polling). An awaiter's `await_suspend` writes an SQE with the coroutine handle in `user_data`; the loop reaps CQEs and resumes the handle, and `await_resume` returns `cqe.res` as the byte count.

</details>

---

[← Previous: Chapter 34](34-building-task.md) &nbsp;|&nbsp; [Course map](../README.md) &nbsp;|&nbsp; [Next: Chapter 36 — The C++ compilation model →](../part-14-compilation-and-linking/36-compilation-model.md)
