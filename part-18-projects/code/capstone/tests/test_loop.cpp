#include "aio/loop.hpp"
#include <sys/socket.h>
#include <fcntl.h>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <future>

#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; std::exit(1); } } while (0)
using namespace aio; using namespace std::chrono_literals;

int main() {
    Loop loop;
    std::thread th([&] { loop.run(); });

    {   // post() from another thread runs on the loop thread
        std::promise<bool> p; auto f = p.get_future();
        loop.post([&] { p.set_value(Loop::current() == &loop && loop.in_loop_thread()); });
        CHECK(f.get());
    }
    {   // spawn from another thread; sleep ordering
        std::promise<void> done; std::vector<int> order;
        auto body = [&](int id, std::chrono::milliseconds d) -> Task<void> { co_await loop.sleep(d); order.push_back(id); };
        auto all = [&]() -> Task<void> {
            co_await body(0, 0ms);                                                 // already due: no suspension
            done.set_value();
        };
        loop.post_spawn(all());
        done.get_future().get();
        CHECK(order == std::vector<int>{0});
    }
    {   // timeout: nothing writes to the socket, readable(fd, 50ms) must return false after ~50 ms, and the fd must be reusable
        int sv[2]; CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        ::fcntl(sv[0], F_SETFL, O_NONBLOCK);
        std::promise<std::tuple<bool, long, bool>> p; auto fut = p.get_future();
        auto t = [&]() -> Task<void> {
            auto t0 = Loop::Clock::now();
            bool r1 = co_await loop.readable(sv[0], 50ms);
            long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Loop::Clock::now() - t0).count();
            (void)!::write(sv[1], "x", 1);                                          // now it IS readable: the same fd must register again
            bool r2 = co_await loop.readable(sv[0], 500ms);
            p.set_value({r1, ms, r2});
        };
        loop.post_spawn(t());
        auto [r1, ms, r2] = fut.get();
        CHECK(!r1 && ms >= 50 && ms < 200 && r2);
        ::close(sv[0]); ::close(sv[1]);
    }
    {   // an event arriving before the deadline cancels the timer (it must not fire later and resume a dead coroutine)
        int sv[2]; CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        ::fcntl(sv[0], F_SETFL, O_NONBLOCK);
        std::promise<bool> p; auto fut = p.get_future();
        auto t = [&]() -> Task<void> { bool r = co_await loop.readable(sv[0], 100ms); p.set_value(r); };
        loop.post_spawn(t());
        std::this_thread::sleep_for(10ms);
        (void)!::write(sv[1], "x", 1);
        CHECK(fut.get());
        std::this_thread::sleep_for(200ms);                                          // the old deadline passes; nothing must happen
        CHECK(loop.task_errors() == 0);
        ::close(sv[0]); ::close(sv[1]);
    }
    {   // many cross-thread posts from several producers: none lost
        std::atomic<int> n{0}; std::vector<std::thread> ps;
        for (int p = 0; p < 4; ++p) ps.emplace_back([&] { for (int i = 0; i < 20000; ++i) loop.post([&] { ++n; }); });
        for (auto& t : ps) t.join();
        std::promise<void> flush; loop.post([&] { flush.set_value(); }); flush.get_future().get();
        CHECK(n == 80000);
    }
    while (loop.live_tasks() != 0) std::this_thread::sleep_for(1ms);
    loop.stop(); th.join();
    CHECK(loop.task_errors() == 0);
    std::cout << "all tests passed\n";
}
