#include "queues.hpp"
#include <chrono>
#include <cstdio>
#include <thread>
using clk = std::chrono::steady_clock;
template <class Push, class Pop>
double run(int P, int C, long total, Push push, Pop pop) {          // returns M items/s
    std::atomic<long> consumed{0}; std::atomic<bool> go{false};
    std::vector<std::thread> ts; const long per = total / P;
    for (int p = 0; p < P; ++p) ts.emplace_back([&, p] { while (!go) {} for (long i = 0; i < per; ++i) push(long(i + 1)); });
    for (int c = 0; c < C; ++c) ts.emplace_back([&] { while (!go) {} while (consumed.load(std::memory_order_relaxed) < per * P) { if (pop()) consumed.fetch_add(1, std::memory_order_relaxed); } });
    auto t0 = clk::now(); go = true; for (auto& t : ts) t.join();
    return total / std::chrono::duration<double>(clk::now() - t0).count() / 1e6;
}
int main() {
    const long N = 4'000'000;
    auto best = [](auto f) { double b = 0; for (int r = 0; r < 5; ++r) b = std::max(b, f()); return b; };
    std::printf("%-34s %10s %10s\n", "queue (M items/s, best of 5)", "1P / 1C", "2P / 2C");
    {   // Mutex queue: consumers poll with try_pop (same pop style for all, apples to apples)
        auto f = [&](int P, int C) { edu::MutexQueue<long> q; return run(P, C, N, [&](long v) { q.push(v); }, [&] { return q.try_pop(); }); };
        std::printf("%-34s %10.1f %10.1f\n", "MutexQueue (mutex+deque)", best([&] { return f(1, 1); }), best([&] { return f(2, 2); }));
    }
    {   // Bounded blocking queue: use a non-blocking-consumer trick is not possible, so measure the blocking API with close()
        auto f = [&](int P, int C) {
            edu::BoundedQueue<long> q(1024); std::atomic<long> n{0};
            std::vector<std::thread> ts; const long per = N / P;
            auto t0 = clk::now();
            std::vector<std::thread> prods, cons;
            for (int p = 0; p < P; ++p) prods.emplace_back([&] { for (long i = 0; i < per; ++i) q.push(i + 1); });
            for (int c = 0; c < C; ++c) cons.emplace_back([&] { while (q.pop()) n.fetch_add(1, std::memory_order_relaxed); });
            for (auto& t : prods) t.join(); q.close(); for (auto& t : cons) t.join();
            return N / std::chrono::duration<double>(clk::now() - t0).count() / 1e6;
        };
        std::printf("%-34s %10.1f %10.1f\n", "BoundedQueue (blocking, cap 1024)", best([&] { return f(1, 1); }), best([&] { return f(2, 2); }));
    }
    {   edu::SpscRing<long> q(1024);
        std::printf("%-34s %10.1f %10s\n", "SpscRing (lock-free, cap 1024)",
            best([&] { return run(1, 1, N, [&](long v) { while (!q.try_push(v)) {} }, [&] { return q.try_pop(); }); }), "n/a");
    }
    {   auto f = [&](int P, int C) { edu::MpmcRing<long> q(1024); return run(P, C, N, [&](long v) { while (!q.try_push(v)) {} }, [&] { return q.try_pop(); }); };
        std::printf("%-34s %10.1f %10.1f\n", "MpmcRing (lock-free, cap 1024)", best([&] { return f(1, 1); }), best([&] { return f(2, 2); }));
    }
}
