// Project 6 reference solution: ThreadPool with futures, cancellation and graceful shutdown.
#pragma once
#include "../p04/function.hpp"     // edu::Function: move-only callable (std::function cannot hold packaged_task)
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <vector>

namespace edu {

class ThreadPool {
public:
    explicit ThreadPool(unsigned n = std::thread::hardware_concurrency()) {
        if (n == 0) n = 1;
        workers_.reserve(n);
        for (unsigned i = 0; i < n; ++i)
            workers_.emplace_back([this](std::stop_token st) { run(st); });
    }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ~ThreadPool() { shutdown(Drain::yes); }

    // Submit any callable; returns a future for its result. Exceptions travel through the future.
    // Throws std::runtime_error if the pool has been shut down.
    template <class F, class... Args>
    [[nodiscard]] auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;
        std::packaged_task<R()> task([fn = std::forward<F>(f), tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            return std::apply(std::move(fn), std::move(tup));
        });
        auto fut = task.get_future();
        {
            std::lock_guard lk(m_);
            if (closed_) throw std::runtime_error("ThreadPool: submit after shutdown");
            q_.emplace_back([t = std::move(task)]() mutable { t(); });
        }
        cv_.notify_one();
        return fut;
    }

    // Cooperative cancellation: the callable receives a std::stop_token as its first argument.
    // cancel_all() requests stop for every running/queued cancellable task AND drops queued work.
    template <class F, class... Args>
    [[nodiscard]] auto submit_cancellable(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, std::stop_token, Args...>> {
        std::stop_token tok;
        { std::lock_guard lk(m_); tok = cancel_.get_token(); }
        return submit(std::forward<F>(f), std::move(tok), std::forward<Args>(args)...);
    }
    void cancel_all() {
        std::lock_guard lk(m_);
        cancel_.request_stop();
        q_.clear();                          // queued tasks' futures get broken_promise
        cancel_ = std::stop_source{};        // later submissions get a fresh, un-cancelled token
    }

    enum class Drain { yes, no };
    // Drain::yes  - run every queued task, then stop.   Drain::no - discard queued tasks, finish running ones.
    // Discarded tasks' futures complete with std::future_error(broken_promise): packaged_task's destructor does that.
    void shutdown(Drain d = Drain::yes) {
        {
            std::lock_guard lk(m_);
            if (closed_ && workers_.empty()) return;
            closed_ = true;
            if (d == Drain::no) q_.clear();
        }
        cv_.notify_all();
        for (auto& w : workers_) w.request_stop();
        workers_.clear();                    // jthread destructors join
    }

    std::size_t pending() const { std::lock_guard lk(m_); return q_.size(); }

private:
    void run(std::stop_token) {
        for (;;) {
            Function<void()> job;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return closed_ || !q_.empty(); });
                if (q_.empty()) return;      // closed_ and nothing left to do
                job = std::move(q_.front());
                q_.pop_front();
            }
            job();                           // run outside the lock; a task's exception is captured by packaged_task
        }
    }

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Function<void()>> q_;
    bool closed_ = false;
    std::stop_source cancel_;                // guarded by m_ (it is replaced in cancel_all)
    std::vector<std::jthread> workers_;      // declared last: destroyed (joined) before the queue and mutex
};

}  // namespace edu
