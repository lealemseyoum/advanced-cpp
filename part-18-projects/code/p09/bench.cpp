#include "event_loop.hpp"
#include <sys/socket.h>
#include <fcntl.h>
#include <cstdio>
#include <thread>
using edu::Task; using clk = std::chrono::steady_clock;
Task<std::size_t> rd(edu::EventLoop& l, int fd, char* b) { for (;;) { ssize_t n = ::read(fd, b, 16); if (n >= 0) co_return std::size_t(n); co_await l.readable(fd); } }
int main() {
    const int R = 100000;
    {   // coroutines on one thread
        edu::EventLoop loop; int sv[2]; ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        for (int fd : sv) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        auto server = [&]() -> Task<void> { char b[16]; for (int i = 0; i < R; ++i) { auto n = co_await rd(loop, sv[1], b); (void)!::write(sv[1], b, n); } };
        auto client = [&]() -> Task<void> { char b[16]; for (int i = 0; i < R; ++i) { (void)!::write(sv[0], "ping", 4); co_await rd(loop, sv[0], b); } };
        auto t0 = clk::now(); loop.spawn(server()); loop.spawn(client()); loop.run();
        double us = std::chrono::duration<double, std::micro>(clk::now() - t0).count() / R;
        std::printf("coroutines, 1 thread     : %6.2f us per round trip\n", us);
    }
    {   // two threads, blocking I/O
        int sv[2]; ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        auto t0 = clk::now();
        std::thread s([&] { char b[16]; for (int i = 0; i < R; ++i) { ssize_t n = ::read(sv[1], b, 16); (void)!::write(sv[1], b, n); } });
        std::thread c([&] { char b[16]; for (int i = 0; i < R; ++i) { (void)!::write(sv[0], "ping", 4); (void)!::read(sv[0], b, 16); } });
        s.join(); c.join();
        double us = std::chrono::duration<double, std::micro>(clk::now() - t0).count() / R;
        std::printf("2 threads, blocking I/O  : %6.2f us per round trip\n", us);
    }
    {   // timer scalability: 100k concurrent sleepers
        edu::EventLoop loop; int done = 0;
        auto one = [&](int i) -> Task<void> { co_await loop.sleep(std::chrono::milliseconds(50 + i % 50)); ++done; };
        auto t0 = clk::now();
        for (int i = 0; i < 100000; ++i) loop.spawn(one(i));
        loop.run();
        std::printf("100k concurrent sleepers : %6.1f ms wall (sleep spread 50-99 ms), done=%d\n", std::chrono::duration<double, std::milli>(clk::now() - t0).count(), done);
    }
}
