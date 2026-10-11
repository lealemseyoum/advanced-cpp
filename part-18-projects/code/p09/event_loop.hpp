// Project 9 reference solution: a single-threaded epoll event loop with coroutine integration.
//   loop.run()                 run until there is no work left
//   loop.spawn(Task<void>)     start a detached coroutine
//   co_await loop.readable(fd) / writable(fd)      suspend until the fd is ready
//   co_await loop.sleep(dur)                       suspend until a timer fires
#pragma once
#include "../p08/task.hpp"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

namespace edu {

class EventLoop {
public:
    using Clock = std::chrono::steady_clock;

    EventLoop() {
        ep_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (ep_ < 0) throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    ~EventLoop() { if (ep_ >= 0) ::close(ep_); }

    // ---- awaitables -----------------------------------------------------------------------
    struct FdAwaiter {
        EventLoop& loop; int fd; std::uint32_t events;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) { loop.add_waiter(fd, events, h); }
        void await_resume() const noexcept {}
    };
    struct SleepAwaiter {
        EventLoop& loop; Clock::time_point when;
        bool await_ready() const noexcept { return when <= Clock::now(); }
        void await_suspend(std::coroutine_handle<> h) { loop.timers_.emplace(when, h); }
        void await_resume() const noexcept {}
    };
    FdAwaiter readable(int fd) { return {*this, fd, EPOLLIN}; }
    FdAwaiter writable(int fd) { return {*this, fd, EPOLLOUT}; }
    template <class Rep, class Per>
    SleepAwaiter sleep(std::chrono::duration<Rep, Per> d) { return {*this, Clock::now() + std::chrono::duration_cast<Clock::duration>(d)}; }

    // ---- detached tasks ---------------------------------------------------------------------
    void spawn(Task<void> t) { ++active_; start(std::move(t)); }
    void stop() noexcept { stop_ = true; }

    // Runs until all spawned tasks have finished (or stop() is called). Single-threaded.
    void run() {
        stop_ = false;
        epoll_event evs[64];
        while (!stop_ && active_ > 0) {
            int timeout_ms = -1;
            if (!timers_.empty()) {
                auto d = timers_.begin()->first - Clock::now();
                timeout_ms = d <= Clock::duration::zero() ? 0
                           : static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count()) + 1;   // round UP: never wake early
            }
            if (waiters_.empty() && timers_.empty()) break;                    // tasks exist but nothing can ever wake them
            int n = ::epoll_wait(ep_, evs, 64, timeout_ms);
            if (n < 0) { if (errno == EINTR) continue; throw std::system_error(errno, std::generic_category(), "epoll_wait"); }
            for (int i = 0; i < n; ++i) {
                const int fd = evs[i].data.fd;
                auto it = waiters_.find(fd);
                if (it == waiters_.end()) continue;
                auto h = it->second.h;
                waiters_.erase(it);                                            // one-shot: remove before resuming (the handler may re-add)
                ::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);
                h.resume();
            }
            run_due_timers();
        }
    }
    std::size_t active_tasks() const noexcept { return active_; }

private:
    struct Waiter { std::coroutine_handle<> h; };

    void add_waiter(int fd, std::uint32_t events, std::coroutine_handle<> h) {
        if (waiters_.count(fd)) throw std::logic_error("EventLoop: two coroutines waiting on the same fd");
        epoll_event ev{}; ev.events = events | EPOLLONESHOT | EPOLLERR | EPOLLHUP; ev.data.fd = fd;
        if (::epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) < 0) throw std::system_error(errno, std::generic_category(), "epoll_ctl ADD");
        waiters_.emplace(fd, Waiter{h});
    }
    void run_due_timers() {
        const auto now = Clock::now();
        while (!timers_.empty() && timers_.begin()->first <= now) {
            auto h = timers_.begin()->second;
            timers_.erase(timers_.begin());
            h.resume();
        }
    }

    // Detached coroutine: awaits the task, then decrements the active count. Frame frees itself at the end.
    struct Detached {
        struct promise_type {
            Detached get_return_object() { return {}; }
            std::suspend_never initial_suspend() noexcept { return {}; }
            std::suspend_never final_suspend() noexcept { return {}; }
            void return_void() {}
            void unhandled_exception() { std::terminate(); }           // errors are caught in start(): a detached task has no one to report to
        };
    };
    Detached start(Task<void> t) {
        try { co_await std::move(t); } catch (...) { /* report via your logging; swallowing silently is a policy decision */ ++errors_; }
        --active_;
    }

public:
    std::size_t task_errors() const noexcept { return errors_; }

private:
    int ep_ = -1;
    bool stop_ = false;
    std::size_t active_ = 0, errors_ = 0;
    std::unordered_map<int, Waiter> waiters_;
    std::multimap<Clock::time_point, std::coroutine_handle<>> timers_;
};

}  // namespace edu
