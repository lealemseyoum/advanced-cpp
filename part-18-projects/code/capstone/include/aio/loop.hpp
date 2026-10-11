// A multi-thread-aware epoll reactor. ONE thread runs a Loop; any thread may post() work to it.
//   co_await loop.readable(fd, timeout?)  -> bool  (true: ready, false: timed out)
//   co_await loop.writable(fd, timeout?)  -> bool
//   co_await loop.sleep(duration)
//   loop.post(fn)   thread-safe: run fn on the loop thread (wakes it through an eventfd)
//   loop.spawn(t)   loop-thread only;  loop.post_spawn(t) thread-safe
#pragma once
#include "function.hpp"
#include "task.hpp"
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

namespace aio {

class Loop {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;

    Loop() {
        ep_ = ::epoll_create1(EPOLL_CLOEXEC);
        wake_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (ep_ < 0 || wake_ < 0) throw std::system_error(errno, std::generic_category(), "Loop");
        epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = wake_;               // permanent, level-triggered
        if (::epoll_ctl(ep_, EPOLL_CTL_ADD, wake_, &ev) < 0) throw std::system_error(errno, std::generic_category(), "epoll_ctl");
    }
    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;
    ~Loop() { if (wake_ >= 0) ::close(wake_); if (ep_ >= 0) ::close(ep_); }

    // The loop that is running on the calling thread, or nullptr.
    static Loop* current() noexcept { return current_; }
    bool in_loop_thread() const noexcept { return owner_.load(std::memory_order_acquire) == std::this_thread::get_id(); }

    // ---- thread-safe -------------------------------------------------------------------------
    void post(Function<void()> fn) {
        { std::lock_guard lk(m_); posted_.push_back(std::move(fn)); }
        wake();
    }
    void post_spawn(Task<void> t) { post([this, t = std::move(t)]() mutable { spawn(std::move(t)); }); }
    void stop() noexcept { stop_.store(true, std::memory_order_release); wake(); }
    std::size_t task_errors() const noexcept { return errors_.load(); }
    std::size_t live_tasks() const noexcept { return active_.load(); }

    // ---- loop-thread only --------------------------------------------------------------------
    void spawn(Task<void> t) { active_.fetch_add(1, std::memory_order_relaxed); start(std::move(t)); }

    struct FdAwaiter {
        Loop& loop; int fd; std::uint32_t events; std::optional<Duration> timeout; bool ready = false;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) { loop.add_wait(fd, events, h, timeout, &ready); }
        bool await_resume() const noexcept { return ready; }
    };
    struct SleepAwaiter {
        Loop& loop; Duration d;
        bool await_ready() const noexcept { return d <= Duration::zero(); }
        void await_suspend(std::coroutine_handle<> h) { loop.timers_.emplace(Clock::now() + d, Timer{h, -1, nullptr}); }
        void await_resume() const noexcept {}
    };
    FdAwaiter readable(int fd, std::optional<Duration> t = std::nullopt) { return {*this, fd, EPOLLIN, t}; }
    FdAwaiter writable(int fd, std::optional<Duration> t = std::nullopt) { return {*this, fd, EPOLLOUT, t}; }
    template <class Rep, class Per> SleepAwaiter sleep(std::chrono::duration<Rep, Per> d) { return {*this, std::chrono::duration_cast<Duration>(d)}; }

    // Runs until stop(). Must be called by exactly one thread.
    void run() {
        owner_.store(std::this_thread::get_id(), std::memory_order_release);
        current_ = this;
        epoll_event evs[128];
        while (!stop_.load(std::memory_order_acquire)) {
            int timeout_ms = -1;
            if (!timers_.empty()) {
                auto d = timers_.begin()->first - Clock::now();
                timeout_ms = d <= Duration::zero() ? 0 : static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count()) + 1;
            }
            int n = ::epoll_wait(ep_, evs, 128, timeout_ms);
            if (n < 0) { if (errno == EINTR) continue; throw std::system_error(errno, std::generic_category(), "epoll_wait"); }
            for (int i = 0; i < n; ++i) {
                const int fd = evs[i].data.fd;
                if (fd == wake_) { drain_wake(); run_posted(); continue; }
                auto it = waits_.find(fd);
                if (it == waits_.end()) continue;
                Wait w = it->second; waits_.erase(it);
                ::epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);
                if (w.timer) timers_.erase(*w.timer);                              // cancel the timeout racing this event
                *w.ready = true;
                w.h.resume();
            }
            run_due_timers();
        }
        run_posted();
        current_ = nullptr;
        owner_.store({}, std::memory_order_release);
    }

private:
    struct Timer { std::coroutine_handle<> h; int fd; bool* ready; };
    using TimerMap = std::multimap<Clock::time_point, Timer>;
    struct Wait { std::coroutine_handle<> h; bool* ready; std::optional<TimerMap::iterator> timer; };

    void wake() noexcept { std::uint64_t one = 1; (void)!::write(wake_, &one, sizeof one); }
    void drain_wake() noexcept { std::uint64_t v; (void)!::read(wake_, &v, sizeof v); }
    void run_posted() {
        std::deque<Function<void()>> batch;
        { std::lock_guard lk(m_); batch.swap(posted_); }
        for (auto& f : batch) f();
    }
    void add_wait(int fd, std::uint32_t events, std::coroutine_handle<> h, std::optional<Duration> timeout, bool* ready) {
        if (waits_.count(fd)) throw std::logic_error("Loop: two coroutines waiting on the same fd");
        epoll_event ev{}; ev.events = events | EPOLLONESHOT | EPOLLERR | EPOLLHUP; ev.data.fd = fd;
        if (::epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) < 0) throw std::system_error(errno, std::generic_category(), "epoll_ctl ADD");
        Wait w{h, ready, std::nullopt};
        if (timeout) w.timer = timers_.emplace(Clock::now() + *timeout, Timer{h, fd, ready});
        waits_.emplace(fd, w);
    }
    void run_due_timers() {
        const auto now = Clock::now();
        while (!timers_.empty() && timers_.begin()->first <= now) {
            Timer t = timers_.begin()->second;
            timers_.erase(timers_.begin());
            if (t.fd >= 0) { waits_.erase(t.fd); ::epoll_ctl(ep_, EPOLL_CTL_DEL, t.fd, nullptr); *t.ready = false; }   // timed out: unregister the fd
            t.h.resume();
        }
    }

    struct Detached {
        struct promise_type {
            Detached get_return_object() { return {}; }
            std::suspend_never initial_suspend() noexcept { return {}; }
            std::suspend_never final_suspend() noexcept { return {}; }
            void return_void() {}
            void unhandled_exception() { std::terminate(); }
        };
    };
    Detached start(Task<void> t) {
        try { co_await std::move(t); } catch (...) { errors_.fetch_add(1); }
        active_.fetch_sub(1);
    }

    inline static thread_local Loop* current_ = nullptr;
    int ep_ = -1, wake_ = -1;
    std::atomic<bool> stop_{false};
    std::atomic<std::thread::id> owner_{};
    std::atomic<std::size_t> active_{0}, errors_{0};
    std::mutex m_;
    std::deque<Function<void()>> posted_;
    std::unordered_map<int, Wait> waits_;       // loop-thread only
    TimerMap timers_;                           // loop-thread only
};

}  // namespace aio
