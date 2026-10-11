#include "raii.hpp"
#include <dirent.h>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

static int open_fd_count() {
    int n = 0;
    if (DIR* d = opendir("/proc/self/fd")) {
        while (readdir(d)) ++n;
        closedir(d);
    }
    return n - 3;  // ".", "..", and the DIR's own fd
}

#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; std::exit(1); } } while (0)

int main() {
    const int base = open_fd_count();

    {   // move-only semantics, no leak, no double close
        auto fd = raii::open_fd("/dev/null", O_RDONLY);
        CHECK(fd.has_value());
        raii::Fd a = std::move(*fd);
        raii::Fd b = std::move(a);
        CHECK(!a && b);
        CHECK(open_fd_count() == base + 1);
        b.reset();
        CHECK(open_fd_count() == base);
    }
    {   // error path returns std::error_code, nothing leaks
        auto bad = raii::open_fd("/definitely/not/here", O_RDONLY);
        CHECK(!bad && bad.error() == std::errc::no_such_file_or_directory);
        CHECK(open_fd_count() == base);
    }
    {   // File round-trip
        auto f = raii::open_file("/tmp/p01_test.txt", "w");
        CHECK(f.has_value());
        std::fputs("hello\n", f->get());
        f->reset();                                   // explicit close
        auto g = raii::open_file("/tmp/p01_test.txt", "r");
        char buf[16]{};
        std::fgets(buf, sizeof buf, g->get());
        CHECK(std::string(buf) == "hello\n");
    }
    {   // sockets: pair, send, recv, shutdown gives EOF
        auto p = raii::Socket::pair();
        CHECK(p.has_value());
        auto& [x, y] = *p;
        CHECK(open_fd_count() == base + 2);
        CHECK(*x.send("ping") == 4);
        CHECK(*y.recv(16) == "ping");
        x.shutdown_write();
        CHECK(y.recv(16)->empty());                    // EOF
    }
    CHECK(open_fd_count() == base);

    {   // scope guard: runs on exception; dismiss() cancels
        int ran = 0;
        try { raii::ScopeExit g([&] { ++ran; }); throw 1; } catch (int) {}
        CHECK(ran == 1);
        { raii::ScopeExit g([&] { ++ran; }); g.dismiss(); }
        CHECK(ran == 1);
    }
    {   // Guarded: 4 threads x 100000 increments
        raii::Guarded<long> counter(0L);
        std::vector<std::thread> ts;
        for (int i = 0; i < 4; ++i) ts.emplace_back([&] { for (int k = 0; k < 100000; ++k) *counter.lock() += 1; });
        for (auto& t : ts) t.join();
        CHECK(*counter.lock() == 400000);
    }
    std::cout << "all tests passed\n";
}
