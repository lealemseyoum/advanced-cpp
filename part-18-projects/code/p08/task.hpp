// Project 8 reference solution: lazy Task<T> with symmetric transfer, plus sync_wait and a manual-reset event.
#pragma once
#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>

namespace edu {

template <class T = void> class Task;

namespace detail {

// Shared by Task<T> and Task<void>: owns the continuation and the final-suspend behaviour.
struct PromiseBase {
    std::coroutine_handle<> continuation = std::noop_coroutine();

    std::suspend_always initial_suspend() noexcept { return {}; }       // lazy: nothing runs until awaited / started

    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }
        // Symmetric transfer: returning a handle makes the compiler *tail-call* the continuation, so a chain of
        // N awaits does not consume N stack frames.
        template <class P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept { return h.promise().continuation; }
        void await_resume() noexcept {}
    };
    FinalAwaiter final_suspend() noexcept { return {}; }                // frame stays alive: the Task object destroys it
};

}  // namespace detail

template <class T>
class [[nodiscard]] Task {
public:
    struct promise_type : detail::PromiseBase {
        std::variant<std::monostate, T, std::exception_ptr> result;
        Task get_return_object() { return Task(std::coroutine_handle<promise_type>::from_promise(*this)); }
        template <class U> void return_value(U&& v) { result.template emplace<1>(std::forward<U>(v)); }
        void unhandled_exception() noexcept { result.template emplace<2>(std::current_exception()); }
    };

    Task() noexcept = default;
    Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task& operator=(Task&& o) noexcept { if (this != &o) { destroy(); h_ = std::exchange(o.h_, {}); } return *this; }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { destroy(); }

    // co_await task: start it, suspend the caller, resume the caller when the task finishes.
    auto operator co_await() && noexcept {
        struct Awaiter {
            std::coroutine_handle<promise_type> h;
            bool await_ready() noexcept { return !h || h.done(); }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
                h.promise().continuation = caller;
                return h;                                               // symmetric transfer INTO the task
            }
            T await_resume() {
                auto& r = h.promise().result;
                if (r.index() == 2) std::rethrow_exception(std::get<2>(r));
                return std::move(std::get<1>(r));
            }
        };
        return Awaiter{h_};
    }

    // Used by sync_wait: run to the first suspension point without a caller.
    std::coroutine_handle<promise_type> handle() const noexcept { return h_; }

private:
    explicit Task(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}
    void destroy() noexcept { if (h_) { h_.destroy(); h_ = {}; } }       // owned: the Task owns the frame
    std::coroutine_handle<promise_type> h_{};
};

template <>
class [[nodiscard]] Task<void> {
public:
    struct promise_type : detail::PromiseBase {
        std::exception_ptr error;
        Task get_return_object() { return Task(std::coroutine_handle<promise_type>::from_promise(*this)); }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { error = std::current_exception(); }
    };
    Task() noexcept = default;
    Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task& operator=(Task&& o) noexcept { if (this != &o) { destroy(); h_ = std::exchange(o.h_, {}); } return *this; }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { destroy(); }

    auto operator co_await() && noexcept {
        struct Awaiter {
            std::coroutine_handle<promise_type> h;
            bool await_ready() noexcept { return !h || h.done(); }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept { h.promise().continuation = caller; return h; }
            void await_resume() { if (h.promise().error) std::rethrow_exception(h.promise().error); }
        };
        return Awaiter{h_};
    }
    std::coroutine_handle<promise_type> handle() const noexcept { return h_; }
private:
    explicit Task(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}
    void destroy() noexcept { if (h_) { h_.destroy(); h_ = {}; } }
    std::coroutine_handle<promise_type> h_{};
};

// ---- sync_wait: block the calling thread until the task completes ----------------------------------
namespace detail {
struct Latch {
    std::mutex m; std::condition_variable cv; bool done = false;
    void set() { { std::lock_guard lk(m); done = true; } cv.notify_all(); }
    void wait() { std::unique_lock lk(m); cv.wait(lk, [&] { return done; }); }
};
// A tiny detached coroutine that awaits the task, then signals the latch. Its frame destroys itself.
struct Runner {
    struct promise_type {
        Runner get_return_object() { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }       // self-destroying
        void return_void() {}
        void unhandled_exception() { std::terminate(); }                 // exceptions are caught inside run_into
    };
};
template <class T>
Runner run_into(Task<T> t, Latch& l, std::optional<T>& out, std::exception_ptr& err) {
    try { out.emplace(co_await std::move(t)); } catch (...) { err = std::current_exception(); }
    l.set();
}
inline Runner run_into(Task<void> t, Latch& l, bool&, std::exception_ptr& err) {
    try { co_await std::move(t); } catch (...) { err = std::current_exception(); }
    l.set();
}
}  // namespace detail

template <class T>
T sync_wait(Task<T> t) {
    detail::Latch l; std::optional<T> out; std::exception_ptr err;
    detail::run_into(std::move(t), l, out, err);
    l.wait();
    if (err) std::rethrow_exception(err);
    return std::move(*out);
}
inline void sync_wait(Task<void> t) {
    detail::Latch l; bool dummy = false; std::exception_ptr err;
    detail::run_into(std::move(t), l, dummy, err);
    l.wait();
    if (err) std::rethrow_exception(err);
}

}  // namespace edu
