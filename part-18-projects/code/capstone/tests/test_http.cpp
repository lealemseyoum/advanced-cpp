#include "aio/http.hpp"
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>

#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; std::exit(1); } } while (0)
using namespace aio; using namespace aio::http; using namespace std::chrono_literals;

// ---- blocking test client (plain POSIX; the server under test is the multi-threaded aio runtime) --------------
static int dial(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    timeval tv{5, 0}; ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}
static void send_all(int fd, std::string_view s) { while (!s.empty()) { ssize_t n = ::send(fd, s.data(), s.size(), MSG_NOSIGNAL); CHECK(n > 0); s.remove_prefix(std::size_t(n)); } }
static std::string recv_until_eof(int fd) { std::string all; char b[4096]; for (;;) { ssize_t n = ::recv(fd, b, sizeof b, 0); if (n <= 0) break; all.append(b, std::size_t(n)); } return all; }
static std::string roundtrip(std::uint16_t port, std::string_view raw) { int fd = dial(port); send_all(fd, raw); std::string r = recv_until_eof(fd); ::close(fd); return r; }
struct Resp { int status = 0; std::string body; std::map<std::string, std::string> headers; };
static Resp read_response(int fd, std::string& buf) {
    for (;;) {
        auto end = buf.find("\r\n\r\n");
        if (end != std::string::npos) {
            Resp r; std::istringstream is(buf.substr(0, end)); std::string line; std::getline(is, line);
            r.status = std::stoi(line.substr(9, 3));
            while (std::getline(is, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); auto c = line.find(':'); if (c != std::string::npos) r.headers[line.substr(0, c)] = std::string(trim(line.substr(c + 1))); }
            std::size_t len = std::stoul(r.headers["content-length"]);
            if (buf.size() >= end + 4 + len) { r.body = buf.substr(end + 4, len); buf.erase(0, end + 4 + len); return r; }
        }
        char b[4096]; ssize_t n = ::recv(fd, b, sizeof b, 0);
        CHECK(n > 0);
        buf.append(b, std::size_t(n));
    }
}
static int status_of(const std::string& r) { return r.size() > 12 ? std::stoi(r.substr(9, 3)) : -1; }
static std::string body_of(const std::string& r) { auto p = r.find("\r\n\r\n"); return p == std::string::npos ? "" : r.substr(p + 4); }
static std::string get(const std::string& t) { return "GET " + t + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"; }

static void parser_tests() {
    std::pmr::monotonic_buffer_resource mr;
    std::string req = "POST /a/b?x=1&y=2 HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\nX-Thing:   spaced value  \r\n\r\nhello";
    for (std::size_t cut = 0; cut < req.size(); ++cut) CHECK(parse_request(std::string_view(req).substr(0, cut), &mr).status == ParseStatus::need_more);
    auto r = parse_request(req, &mr);
    CHECK(r.status == ParseStatus::done && r.consumed == req.size());
    CHECK(r.req.method == "POST" && r.req.path == "/a/b" && r.req.query == "x=1&y=2" && r.req.body == "hello");
    CHECK(r.req.header("x-thing") == "spaced value" && r.req.header("missing").empty());
    CHECK(*r.req.query_param("y") == "2" && *r.req.query_param("x") == "1" && !r.req.query_param("z"));
    CHECK(r.req.body.get_allocator().resource() == &mr);                                 // the request really lives in our arena
    std::string two = "GET /1 HTTP/1.1\r\n\r\nGET /2 HTTP/1.1\r\n\r\n";
    auto p1 = parse_request(two, &mr); CHECK(p1.status == ParseStatus::done && p1.consumed == 19);
    CHECK(parse_request(std::string_view(two).substr(p1.consumed), &mr).req.path == "/2");
    for (std::string_view bad : {"GET /x HTTP/2.0\r\n\r\n", "get /x HTTP/1.1\r\n\r\n", "GET x HTTP/1.1\r\n\r\n", "GET /x HTTP/1.1\r\nBad Header : v\r\n\r\n",
                                 "POST /x HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", "POST /x HTTP/1.1\r\nContent-Length: 12x\r\n\r\n"})
        CHECK(parse_request(bad, &mr).status == ParseStatus::bad_request);
    CHECK(parse_request("POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", &mr).status == ParseStatus::not_implemented);
    CHECK(parse_request("POST /x HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n", &mr).status == ParseStatus::body_too_large);
    CHECK(parse_request(std::string(20000, 'a'), &mr).status == ParseStatus::header_too_large);
}

static Router make_router(Runtime& rt) {
    Router r;
    r.get("/hello", [](Request&) -> Task<Response> { co_return Response::text(200, "Hello\n"); });
    r.get("/users/:id", [](Request& q) -> Task<Response> { co_return Response::text(200, "user " + std::string(q.params.find(std::string_view("id"))->second)); });
    r.get("/sum", [](Request& q) -> Task<Response> { co_return Response::text(200, std::to_string(std::stoi(std::string(*q.query_param("a"))) + std::stoi(std::string(*q.query_param("b"))))); });
    r.post("/echo", [](Request& q) -> Task<Response> { co_return Response::text(200, std::string(q.body)); });
    r.get("/boom", [](Request&) -> Task<Response> { throw std::runtime_error("handler failed"); co_return Response{}; });
    r.get("/slow", [](Request&) -> Task<Response> { co_await Loop::current()->sleep(100ms); co_return Response::text(200, "slow\n"); });
    r.get("/thread", [](Request&) -> Task<Response> { std::ostringstream o; o << std::this_thread::get_id(); co_return Response::text(200, o.str()); });
    r.get("/busy", [&rt](Request&) -> Task<Response> { co_await rt.blocking([] { std::this_thread::sleep_for(400ms); }); co_return Response::text(200, "done\n"); });
    r.get("/fib/:n", [&rt](Request& q) -> Task<Response> {
        int n = std::stoi(std::string(q.params.find(std::string_view("n"))->second));
        long v = co_await rt.blocking([n] { auto f = [](auto&& self, int x) -> long { return x < 2 ? x : self(self, x - 1) + self(self, x - 2); }; return f(f, n); });
        co_return Response::text(200, std::to_string(v));
    });
    return r;
}

static void functional_tests(bool use_arena) {
    Runtime rt(2, 2);
    ServerConfig cfg; cfg.use_arena = use_arena; cfg.header_timeout = 300ms; cfg.idle_timeout = 400ms;
    Server server(rt, make_router(rt), cfg);
    auto started = server.start(); CHECK(started.has_value());
    const std::uint16_t port = *started;

    CHECK(body_of(roundtrip(port, get("/hello"))) == "Hello\n");
    CHECK(body_of(roundtrip(port, get("/users/42"))) == "user 42");
    CHECK(body_of(roundtrip(port, get("/sum?a=2&b=40"))) == "42");
    CHECK(body_of(roundtrip(port, "POST /echo HTTP/1.1\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhello")) == "hello");
    CHECK(status_of(roundtrip(port, get("/nope"))) == 404);
    { auto m = roundtrip(port, "DELETE /hello HTTP/1.1\r\nConnection: close\r\n\r\n"); CHECK(status_of(m) == 405 && m.find("allow: GET") != std::string::npos); }
    CHECK(status_of(roundtrip(port, "BROKEN\r\n\r\n")) == 400);
    CHECK(status_of(roundtrip(port, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n")) == 501);
    CHECK(status_of(roundtrip(port, "POST /echo HTTP/1.1\r\nContent-Length: 2000000\r\n\r\n")) == 413);
    CHECK(status_of(roundtrip(port, "GET /x HTTP/1.1\r\nX: " + std::string(20000, 'a') + "\r\n\r\n")) == 431);
    { auto b = roundtrip(port, get("/boom")); CHECK(status_of(b) == 500 && b.find("handler failed") != std::string::npos); }
    CHECK(status_of(roundtrip(port, get("/hello"))) == 200);                                  // still alive after the 500

    {   // request split across packets
        int fd = dial(port);
        send_all(fd, "POST /echo HTTP/1.1\r\nConnection: close\r\nContent-Length: 11\r\n\r\nhello");
        std::this_thread::sleep_for(50ms);
        send_all(fd, " world");
        CHECK(body_of(recv_until_eof(fd)) == "hello world"); ::close(fd);
    }
    {   // keep-alive and pipelining over ONE connection
        const auto before = server.stats().connections.load();
        int fd = dial(port); std::string buf;
        for (int i = 0; i < 3; ++i) {
            send_all(fd, "GET /users/" + std::to_string(i) + " HTTP/1.1\r\nHost: x\r\n\r\n");
            Resp r = read_response(fd, buf);
            CHECK(r.status == 200 && r.body == "user " + std::to_string(i) && r.headers["connection"] == "keep-alive");
        }
        send_all(fd, "GET /users/a HTTP/1.1\r\n\r\nGET /users/b HTTP/1.1\r\n\r\nGET /users/c HTTP/1.1\r\nConnection: close\r\n\r\n");
        for (char c : {'a', 'b', 'c'}) CHECK(read_response(fd, buf).body == std::string("user ") + c);
        ::close(fd);
        CHECK(server.stats().connections.load() == before + 1);
    }
    {   // timeouts: a half-sent request gets 408; a silent connection is closed by the idle timeout
        int fd = dial(port); send_all(fd, "GET /hello HTTP/1.1\r\nHost: x\r\n");              // never finishes the header block
        auto t0 = std::chrono::steady_clock::now();
        std::string r = recv_until_eof(fd); ::close(fd);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(status_of(r) == 408 && ms >= 250 && ms < 1500);
        fd = dial(port); t0 = std::chrono::steady_clock::now();
        CHECK(recv_until_eof(fd).empty());                                                    // closed without a response
        ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms >= 350 && ms < 1500); ::close(fd);
        CHECK(server.stats().timeouts.load() >= 2);
    }
    {   // concurrency: 64 clients x a 100 ms handler overlap
        auto t0 = std::chrono::steady_clock::now(); std::atomic<int> ok{0}; std::vector<std::thread> ts;
        for (int i = 0; i < 64; ++i) ts.emplace_back([&] { if (status_of(roundtrip(port, get("/slow"))) == 200) ++ok; });
        for (auto& t : ts) t.join();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "  64 concurrent /slow (100 ms each): " << ms << " ms wall\n";
        CHECK(ok == 64 && ms < 1000);
    }
    {   // both loop threads serve connections (SO_REUSEPORT spreads them)
        std::set<std::string> ids;
        for (int i = 0; i < 60; ++i) ids.insert(body_of(roundtrip(port, get("/thread"))));
        CHECK(ids.size() == 2);
    }
    {   // CPU work offloaded: fib(27) is correct; and while a 400 ms blocking handler runs, other requests are still served fast
        CHECK(body_of(roundtrip(port, get("/fib/27"))) == "196418");
        std::thread busy([&] { CHECK(status_of(roundtrip(port, get("/busy"))) == 200); });
        std::this_thread::sleep_for(50ms);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i) CHECK(status_of(roundtrip(port, get("/hello"))) == 200);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "  20 requests while a 400 ms blocking handler runs: " << ms << " ms\n";
        CHECK(ms < 300);
        busy.join();
    }
    {   // graceful stop with an idle keep-alive connection open
        int fd = dial(port); std::string buf;
        send_all(fd, "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n"); CHECK(read_response(fd, buf).status == 200);
        auto t0 = std::chrono::steady_clock::now();
        bool clean = server.stop(3s);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(clean && ms < 1000);
        CHECK(recv_until_eof(fd).empty()); ::close(fd);                                       // server closed the idle connection
        CHECK(server.stats().open == 0);
    }
    for (unsigned i = 0; i < rt.io_threads(); ++i) { while (rt.loop(i).live_tasks() != 0) std::this_thread::sleep_for(1ms); CHECK(rt.loop(i).task_errors() == 0); }
    rt.stop();
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    parser_tests();
    std::cout << "with per-connection arena:\n";    functional_tests(true);
    std::cout << "with the default allocator:\n";   functional_tests(false);
    std::cout << "all tests passed\n";
}
