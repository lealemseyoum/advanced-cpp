// ./server [port] [io_threads] [noarena]    then:  curl -i localhost:8080/hello
#include "aio/http.hpp"
#include <csignal>
#include <cstdio>
#include <iostream>
#include <semaphore>
using namespace aio; using namespace aio::http;

static std::binary_semaphore g_quit{0};
int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, [](int) { g_quit.release(); });
    const std::uint16_t port = argc > 1 ? static_cast<std::uint16_t>(std::stoi(argv[1])) : 8080;
    const unsigned threads = argc > 2 ? static_cast<unsigned>(std::stoi(argv[2])) : 2;
    Runtime rt(threads, 2);
    Router r;
    r.get("/hello", [](Request&) -> Task<Response> { co_return Response::text(200, "Hello, world\n"); });
    r.get("/users/:id", [](Request& q) -> Task<Response> { co_return Response::text(200, "user " + std::string(q.params.find(std::string_view("id"))->second) + "\n"); });
    r.post("/echo", [](Request& q) -> Task<Response> { co_return Response::text(200, std::string(q.body)); });
    // CPU-heavy work goes to the pool so the event loop keeps serving other connections
    r.get("/fib/:n", [&rt](Request& q) -> Task<Response> {
        int n = std::stoi(std::string(q.params.find(std::string_view("n"))->second));
        auto fib = [](int k) { auto f = [](auto&& self, int x) -> long { return x < 2 ? x : self(self, x - 1) + self(self, x - 2); }; return f(f, k); };
        long v = co_await rt.blocking([=] { return fib(n); });
        co_return Response::text(200, std::to_string(v) + "\n");
    });
    ServerConfig cfg; cfg.port = port;
    if (argc > 3 && std::string(argv[3]) == "noarena") cfg.use_arena = false;
    Server s(rt, std::move(r), cfg);
    auto p = s.start();
    if (!p) { std::cerr << "start failed: " << p.error().message() << "\n"; return 1; }
    std::cout << "listening on http://127.0.0.1:" << *p << " with " << threads << " io thread(s)\n" << std::flush;
    g_quit.acquire();
    std::cout << "\nshutting down...\n";
    bool clean = s.stop();
    std::cout << (clean ? "clean shutdown" : "timed out waiting for connections") << ", " << s.stats().requests << " requests\n";
}
