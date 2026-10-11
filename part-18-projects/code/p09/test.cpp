#include "event_loop.hpp"
#include <sys/socket.h>
#include <fcntl.h>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; std::exit(1); } } while (0)
using edu::Task; using namespace std::chrono_literals;

static void nonblock(int fd) { ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK); }

// async read: read what is available, suspending while EAGAIN
Task<std::string> read_some(edu::EventLoop& loop, int fd) {
    char buf[256];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n >= 0) co_return std::string(buf, std::size_t(n));
        if (errno != EAGAIN) throw std::system_error(errno, std::generic_category(), "read");
        co_await loop.readable(fd);
    }
}
Task<void> write_all(edu::EventLoop& loop, int fd, std::string s) {
    std::size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::write(fd, s.data() + off, s.size() - off);
        if (n >= 0) { off += std::size_t(n); continue; }
        if (errno != EAGAIN) throw std::system_error(errno, std::generic_category(), "write");
        co_await loop.writable(fd);
    }
}

int main() {
    {   // timers fire in deadline order, not creation order, and never early
        edu::EventLoop loop; std::vector<int> order;
        auto start = edu::EventLoop::Clock::now();
        auto sleeper = [&](int id, std::chrono::milliseconds d) -> Task<void> { co_await loop.sleep(d); order.push_back(id); };
        loop.spawn(sleeper(3, 90ms)); loop.spawn(sleeper(1, 30ms)); loop.spawn(sleeper(2, 60ms));
        loop.run();
        auto el = std::chrono::duration_cast<std::chrono::milliseconds>(edu::EventLoop::Clock::now() - start);
        CHECK((order == std::vector<int>{1, 2, 3}));
        CHECK(el >= 90ms && el < 200ms);
        CHECK(loop.active_tasks() == 0);
    }
    {   // concurrency without threads: 1000 sleepers of 50 ms finish in ~50 ms total
        edu::EventLoop loop; int done = 0;
        auto one = [&]() -> Task<void> { co_await loop.sleep(50ms); ++done; };
        auto start = edu::EventLoop::Clock::now();
        for (int i = 0; i < 1000; ++i) loop.spawn(one());
        loop.run();
        auto el = std::chrono::duration_cast<std::chrono::milliseconds>(edu::EventLoop::Clock::now() - start);
        CHECK(done == 1000 && el < 150ms);
    }
    {   // I/O: a coroutine reads from a socket that is written later by another coroutine
        edu::EventLoop loop; int sv[2]; CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        nonblock(sv[0]); nonblock(sv[1]);
        std::string got;
        auto reader = [&]() -> Task<void> { got = co_await read_some(loop, sv[0]); };
        auto writer = [&]() -> Task<void> { co_await loop.sleep(40ms); co_await write_all(loop, sv[1], "hello from the other side"); };
        loop.spawn(reader()); loop.spawn(writer());
        loop.run();
        CHECK(got == "hello from the other side");
        ::close(sv[0]); ::close(sv[1]);
    }
    {   // echo ping-pong between two coroutines over a socketpair, 2000 round trips on one thread
        edu::EventLoop loop; int sv[2]; CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        nonblock(sv[0]); nonblock(sv[1]);
        int rounds = 0;
        auto server = [&]() -> Task<void> { for (int i = 0; i < 2000; ++i) { std::string m = co_await read_some(loop, sv[1]); co_await write_all(loop, sv[1], m); } };
        auto client = [&]() -> Task<void> { for (int i = 0; i < 2000; ++i) { co_await write_all(loop, sv[0], "ping"); std::string r = co_await read_some(loop, sv[0]); CHECK(r == "ping"); ++rounds; } };
        loop.spawn(server()); loop.spawn(client());
        loop.run();
        CHECK(rounds == 2000);
        ::close(sv[0]); ::close(sv[1]);
    }
    {   // an exception in one detached task does not kill the loop
        edu::EventLoop loop; bool other_ran = false;
        auto bad = []() -> Task<void> { throw std::runtime_error("x"); co_return; };          // no captures: safe
        auto good = [&]() -> Task<void> { co_await loop.sleep(10ms); other_ran = true; };    // captures: the closure must outlive the coroutine
        loop.spawn(bad()); loop.spawn(good());
        loop.run();
        CHECK(other_ran && loop.task_errors() == 1);
    }
    {   // a task that waits for nothing: run() returns instead of hanging forever
        edu::EventLoop loop; int sv[2]; CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0); nonblock(sv[0]);
        auto brief = [&]() -> Task<void> { co_await loop.sleep(5ms); };
        loop.spawn(brief());
        loop.run();
        CHECK(loop.active_tasks() == 0);
        ::close(sv[0]); ::close(sv[1]);
    }
    std::cout << "all tests passed\n";
}
