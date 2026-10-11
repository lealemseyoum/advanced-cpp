#include "queues.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; std::exit(1); } } while (0)

struct Probe { static inline std::atomic<int> live{0}; int v; Probe(int x = 0) : v(x) { ++live; } Probe(const Probe& o) : v(o.v) { ++live; } Probe(Probe&& o) noexcept : v(o.v) { ++live; } ~Probe() { --live; } };

// P producers push disjoint ranges; C consumers pop; verify every value exactly once (sum and count).
template <class Push, class Pop>
void mpmc_check(int P, int C, int per, Push push, Pop pop) {
    std::atomic<long> sum{0}, count{0}; std::atomic<int> done_producers{0};
    std::vector<std::thread> ts;
    for (int p = 0; p < P; ++p) ts.emplace_back([&, p] { for (int i = 0; i < per; ++i) push(p * per + i + 1); ++done_producers; });
    for (int c = 0; c < C; ++c) ts.emplace_back([&] {
        for (;;) {
            if (auto v = pop()) { sum += *v; ++count; }
            else if (done_producers == P) { if (auto v2 = pop()) { sum += *v2; ++count; } else return; }
            else std::this_thread::yield();
        }
    });
    for (auto& t : ts) t.join();
    const long n = long(P) * per;
    CHECK(count == n && sum == n * (n + 1) / 2);
}

int main() {
    {   // MutexQueue
        edu::MutexQueue<int> q;
        mpmc_check(3, 3, 20000, [&](int v) { q.push(v); }, [&] { return q.try_pop(); });
    }
    {   // BoundedQueue: capacity respected (producer blocks), close() drains then ends consumers
        edu::BoundedQueue<int> q(8);
        std::atomic<long> sum{0}; std::atomic<int> max_seen{0};
        std::thread prod([&] { for (int i = 1; i <= 50000; ++i) CHECK(q.push(i)); q.close(); });
        std::vector<std::thread> cons;
        for (int c = 0; c < 3; ++c) cons.emplace_back([&] { while (auto v = q.pop()) sum += *v; });
        prod.join(); for (auto& t : cons) t.join();
        CHECK(sum == 50000L * 50001 / 2);
        CHECK(!q.push(1));                                                    // closed
    }
    {   // SPSC: FIFO order preserved, no loss, no duplication
        edu::SpscRing<int> q(1024);
        const int N = 2'000'000;
        std::thread prod([&] { for (int i = 0; i < N;) if (q.try_push(i)) ++i; });
        int expect = 0;
        while (expect < N) if (auto v = q.try_pop()) { CHECK(*v == expect); ++expect; }
        prod.join();
        CHECK(!q.try_pop());
    }
    {   // SPSC with non-trivial T: lifetime accounting, leftovers destroyed in destructor
        { edu::SpscRing<std::string> q(8);
          for (int i = 0; i < 5; ++i) CHECK(q.try_push(std::string(40, 'a' + i)));
          CHECK(*q.try_pop() == std::string(40, 'a')); }
        { edu::SpscRing<Probe> q(8); for (int i = 0; i < 6; ++i) CHECK(q.try_push(Probe(i))); }
        CHECK(Probe::live == 0);
    }
    {   // MPMC ring: 4 producers x 4 consumers, tiny capacity forces wrap-around and contention
        edu::MpmcRing<int> q(16);
        mpmc_check(4, 4, 50000, [&](int v) { while (!q.try_push(v)) std::this_thread::yield(); }, [&] { return q.try_pop(); });
    }
    {   // MPMC: full/empty boundaries
        edu::MpmcRing<int> q(4);
        for (int i = 0; i < 4; ++i) CHECK(q.try_push(i));
        CHECK(!q.try_push(99));
        for (int i = 0; i < 4; ++i) CHECK(*q.try_pop() == i);
        CHECK(!q.try_pop());
        { edu::MpmcRing<Probe> p(8); for (int i = 0; i < 5; ++i) CHECK(p.try_push(Probe(i))); }
        CHECK(Probe::live == 0);
    }
    std::cout << "all tests passed\n";
}
