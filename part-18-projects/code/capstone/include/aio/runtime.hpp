// Runtime = N event-loop threads (shared-nothing) + a thread pool for blocking/CPU work.
//   rt.spawn(task)          start a detached task on a loop (round-robin), from any thread
//   rt.spawn_on(i, task)    start on loop i
//   co_await rt.blocking(f) run f on the pool; resume on the loop that awaited it
#pragma once
#include "loop.hpp"
#include "thread_pool.hpp"
#include <atomic>
#include <memory>
#include <thread>
#include <variant>
#include <vector>

namespace aio {

class Runtime {
public:
    explicit Runtime(unsigned io_threads = 2, unsigned pool_threads = 2) : pool_(pool_threads) {
        if (io_threads == 0) io_threads = 1;
        for (unsigned i = 0; i < io_threads; ++i) loops_.push_back(std::make_unique<Loop>());
        for (auto& l : loops_) threads_.emplace_back([p = l.get()] { p->run(); });
    }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime() { stop(); }

    std::size_t io_threads() const noexcept { return loops_.size(); }
    Loop& loop(std::size_t i) { return *loops_[i % loops_.size()]; }
    ThreadPool& pool() noexcept { return pool_; }

    void spawn(Task<void> t) { spawn_on(next_.fetch_add(1, std::memory_order_relaxed), std::move(t)); }
    void spawn_on(std::size_t i, Task<void> t) { loop(i).post_spawn(std::move(t)); }

    // Offload a blocking or CPU-heavy callable. The awaiting coroutine resumes on the loop thread it started on,
    // so code after the co_await can keep using loop-confined state without locks.
    template <class F>
    auto blocking(F f) -> Task<std::invoke_result_t<F&>> {
        using R = std::invoke_result_t<F&>;
        using Stored = std::conditional_t<std::is_void_v<R>, std::monostate, R>;
        struct Awaiter {
            Runtime& rt; Loop* home; F f;
            std::variant<std::monostate, Stored, std::exception_ptr> out;
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) {
                // The Awaiter lives in the suspended coroutine's frame, so `this` stays valid until we resume it.
                (void)rt.pool_.submit([this, h] {
                    try { if constexpr (std::is_void_v<R>) { f(); out.template emplace<1>(); } else out.template emplace<1>(f()); }
                    catch (...) { out.template emplace<2>(std::current_exception()); }
                    home->post([h] { h.resume(); });
                });
            }
            Stored take() { if (out.index() == 2) std::rethrow_exception(std::get<2>(out)); return std::move(std::get<1>(out)); }
            R await_resume() { if constexpr (std::is_void_v<R>) take(); else return take(); }
        };
        Loop* home = Loop::current();
        if (!home) throw std::logic_error("Runtime::blocking must be awaited on a loop thread");
        co_return co_await Awaiter{*this, home, std::move(f), {}};
    }

    void stop() {
        for (auto& l : loops_) l->stop();
        threads_.clear();                 // jthreads join
        pool_.shutdown();
    }

private:
    std::vector<std::unique_ptr<Loop>> loops_;
    ThreadPool pool_;
    std::atomic<std::size_t> next_{0};
    std::vector<std::jthread> threads_;
};

}  // namespace aio
