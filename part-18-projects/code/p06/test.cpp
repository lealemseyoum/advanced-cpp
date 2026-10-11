#include "thread_pool.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <set>

#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; std::exit(1); } } while (0)
using namespace std::chrono_literals;

int main() {
    {   // results through futures, arguments forwarded
        edu::ThreadPool pool(4);
        std::vector<std::future<long>> fs;
        for (int i = 0; i < 100; ++i) fs.push_back(pool.submit([](int a, int b) { return long(a) * b; }, i, 2));
        long s = 0; for (auto& f : fs) s += f.get();
        CHECK(s == 2 * 4950);
    }
    {   // exceptions travel through the future, worker survives
        edu::ThreadPool pool(2);
        auto f = pool.submit([]() -> int { throw std::runtime_error("boom"); });
        bool caught = false; try { f.get(); } catch (const std::runtime_error& e) { caught = std::string(e.what()) == "boom"; }
        CHECK(caught);
        CHECK(pool.submit([] { return 7; }).get() == 7);
    }
    {   // move-only arguments and results
        edu::ThreadPool pool(2);
        auto f = pool.submit([](std::unique_ptr<int> p) { return std::make_unique<int>(*p + 1); }, std::make_unique<int>(41));
        CHECK(*f.get() == 42);
    }
    {   // real parallelism: 4 tasks that each sleep 100 ms finish in ~100 ms on 4 workers, not 400
        edu::ThreadPool pool(4);
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::future<void>> fs;
        for (int i = 0; i < 4; ++i) fs.push_back(pool.submit([] { std::this_thread::sleep_for(100ms); }));
        for (auto& f : fs) f.get();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 250);
    }
    {   // graceful shutdown: every queued task runs
        std::atomic<int> ran{0};
        {
            edu::ThreadPool pool(2);
            for (int i = 0; i < 200; ++i) (void)pool.submit([&] { std::this_thread::sleep_for(100us); ++ran; });
        }                                                           // destructor drains
        CHECK(ran == 200);
    }
    {   // shutdown without draining: queued tasks are discarded, their futures break, running tasks finish
        std::atomic<int> ran{0};
        edu::ThreadPool pool(1);
        std::vector<std::future<void>> fs;
        for (int i = 0; i < 50; ++i) fs.push_back(pool.submit([&] { std::this_thread::sleep_for(5ms); ++ran; }));
        pool.shutdown(edu::ThreadPool::Drain::no);
        int broken = 0, done = 0;
        for (auto& f : fs) { try { f.get(); ++done; } catch (const std::future_error& e) { if (e.code() == std::future_errc::broken_promise) ++broken; } }
        CHECK(done + broken == 50 && broken > 0 && done == ran);
        bool threw = false; try { (void)pool.submit([] {}); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
    }
    {   // stress: many submitters, many tasks, no lost or duplicated tasks
        std::atomic<long> sum{0};
        edu::ThreadPool pool(4);
        std::vector<std::thread> producers;
        for (int p = 0; p < 4; ++p) producers.emplace_back([&] { for (int i = 1; i <= 5000; ++i) (void)pool.submit([&sum, i] { sum += i; }); });
        for (auto& t : producers) t.join();
        pool.shutdown();
        CHECK(sum == 4L * 5000 * 5001 / 2);
    }
    {   // tasks can submit tasks (but must not wait on them on a 1-thread pool: that would deadlock)
        edu::ThreadPool pool(2);
        auto outer = pool.submit([&pool] { return pool.submit([] { return 5; }); });   // returns a future, does not wait
        CHECK(outer.get().get() == 5);
    }
    {   // cooperative cancellation: a long cancellable task stops promptly; queued tasks break
        edu::ThreadPool pool(1);
        std::atomic<bool> started{false};
        auto f = pool.submit_cancellable([&](std::stop_token st) {
            started = true; long n = 0;
            while (!st.stop_requested()) { ++n; std::this_thread::sleep_for(1ms); }
            return n;
        });
        auto queued = pool.submit([] { return 1; });
        while (!started) std::this_thread::yield();
        auto t0 = std::chrono::steady_clock::now();
        pool.cancel_all();
        CHECK(f.get() > 0);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 100);
        bool broken = false; try { (void)queued.get(); } catch (const std::future_error&) { broken = true; }
        CHECK(broken);
        CHECK(pool.submit([] { return 3; }).get() == 3);                       // pool still usable
        CHECK(!pool.submit_cancellable([](std::stop_token st) { return st.stop_requested(); }).get());   // fresh token
    }
    std::cout << "all tests passed\n";
}
