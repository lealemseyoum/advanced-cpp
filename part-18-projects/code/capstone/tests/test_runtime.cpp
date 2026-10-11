#include "aio/runtime.hpp"
#include <cstdlib>
#include <future>
#include <iostream>
#include <numeric>
#include <set>

#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; std::exit(1); } } while (0)
using namespace aio; using namespace std::chrono_literals;

int main() {
    Runtime rt(2, 2);
    CHECK(rt.io_threads() == 2);

    {   // tasks are spread over the loop threads
        std::mutex m; std::set<std::thread::id> ids; std::atomic<int> done{0};
        auto note = [&]() -> Task<void> { { std::lock_guard lk(m); ids.insert(std::this_thread::get_id()); } ++done; co_return; };
        for (int i = 0; i < 20; ++i) rt.spawn(note());
        while (done < 20) std::this_thread::sleep_for(1ms);
        CHECK(ids.size() == 2);
    }
    {   // blocking(): runs on a pool thread, resumes on the SAME loop thread that awaited it
        std::promise<std::tuple<bool, bool, int>> p; auto fut = p.get_future();
        auto t = [&]() -> Task<void> {
            auto loop_thread = std::this_thread::get_id();
            std::thread::id worker;
            int v = co_await rt.blocking([&] { worker = std::this_thread::get_id(); std::this_thread::sleep_for(20ms); return 42; });
            p.set_value({worker != loop_thread, std::this_thread::get_id() == loop_thread, v});
        };
        rt.spawn(t());
        auto [off, back, v] = fut.get();
        CHECK(off && back && v == 42);
    }
    {   // the loop stays responsive while a blocking call is in flight: 100 sleeps of 50 ms overlap with a 300 ms blocking call
        std::promise<std::pair<long, int>> p; auto fut = p.get_future();
        std::atomic<int> ticks{0};
        auto ticker = [&]() -> Task<void> { for (int i = 0; i < 100; ++i) { co_await rt.loop(0).sleep(2ms); ++ticks; } };
        auto t = [&]() -> Task<void> {
            auto t0 = Loop::Clock::now();
            co_await rt.blocking([] { std::this_thread::sleep_for(300ms); });
            p.set_value({std::chrono::duration_cast<std::chrono::milliseconds>(Loop::Clock::now() - t0).count(), ticks.load()});
        };
        rt.spawn_on(0, ticker()); rt.spawn_on(0, t());
        auto [ms, tk] = fut.get();
        CHECK(ms >= 300 && tk >= 50);                                   // the ticker kept running on the same loop while the pool blocked
    }
    {   // exceptions cross the pool boundary and are rethrown in the awaiting coroutine
        std::promise<std::string> p; auto fut = p.get_future();
        auto t = [&]() -> Task<void> {
            try { co_await rt.blocking([]() -> int { throw std::runtime_error("from the pool"); }); p.set_value("no exception"); }
            catch (const std::exception& e) { p.set_value(e.what()); }
        };
        rt.spawn(t());
        CHECK(fut.get() == "from the pool");
    }
    {   // CPU fan-out: 8 blocking sums in parallel on a 2-thread pool
        std::promise<long> p; auto fut = p.get_future();
        auto t = [&]() -> Task<void> {
            long total = 0;
            for (int i = 0; i < 8; ++i) total += co_await rt.blocking([i] { long s = 0; for (long k = 0; k < 1'000'000; ++k) s += k % (i + 2); return s; });
            p.set_value(total);
        };
        rt.spawn(t());
        long expect = 0; for (int i = 0; i < 8; ++i) for (long k = 0; k < 1'000'000; ++k) expect += k % (i + 2);
        CHECK(fut.get() == expect);
    }
    for (unsigned i = 0; i < rt.io_threads(); ++i) while (rt.loop(i).live_tasks() != 0) std::this_thread::sleep_for(1ms);
    rt.stop();
    std::cout << "all tests passed\n";
}
