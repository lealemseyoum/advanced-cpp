// Keep-alive load generator built on the same event loop:  ./loadgen PORT CONNECTIONS REQUESTS_PER_CONNECTION
#include "http.hpp"
#include <csignal>
#include <cstdio>
using namespace edu; using namespace edu::http;

Task<void> client(EventLoop& loop, std::uint16_t port, int requests, std::size_t& done, std::size_t& errors) {
    Fd fd = co_await connect_to(loop, port);
    std::string buf;
    const std::string req = "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n";
    for (int i = 0; i < requests; ++i) {
        co_await write_all(loop, fd.get(), req);
        for (;;) {                                           // response is "...content-length: 13\r\n...\r\n\r\nHello, world\n"
            auto end = buf.find("\r\n\r\n");
            if (end != std::string::npos && buf.size() >= end + 4 + 13) { buf.erase(0, end + 4 + 13); ++done; break; }
            std::string more = co_await read_some(loop, fd.get());
            if (more.empty()) { ++errors; co_return; }
            buf += more;
        }
    }
}
int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    if (argc < 4) { std::fprintf(stderr, "usage: loadgen PORT CONNS REQS\n"); return 2; }
    const auto port = static_cast<std::uint16_t>(std::stoi(argv[1])); const int conns = std::stoi(argv[2]), reqs = std::stoi(argv[3]);
    EventLoop loop; std::size_t done = 0, errors = 0;
    auto t0 = EventLoop::Clock::now();
    for (int i = 0; i < conns; ++i) loop.spawn(client(loop, port, reqs, done, errors));
    loop.run();
    double s = std::chrono::duration<double>(EventLoop::Clock::now() - t0).count();
    std::printf("%d connections x %d requests: %zu ok, %zu errors, %.2f s  =  %.0f req/s\n", conns, reqs, done, errors, s, done / s);
}
