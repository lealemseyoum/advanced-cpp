#include "http.hpp"
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sstream>

#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; std::exit(1); } } while (0)
using namespace edu; using namespace edu::http; using namespace std::chrono_literals;

// ---- tiny client helpers -------------------------------------------------------------------
Task<std::string> exchange(EventLoop& loop, std::uint16_t port, std::string raw) {      // send, read until EOF
    Fd fd = co_await connect_to(loop, port);
    co_await write_all(loop, fd.get(), raw);
    std::string all;
    for (;;) { std::string s = co_await read_some(loop, fd.get()); if (s.empty()) break; all += s; }
    co_return all;
}
struct Resp { int status = 0; std::string body; Headers headers; };
// Read exactly one response (by Content-Length) from an open connection; leftover bytes stay in `buf`.
Task<Resp> read_response(EventLoop& loop, int fd, std::string& buf) {
    for (;;) {
        auto end = buf.find("\r\n\r\n");
        if (end != std::string::npos) {
            Resp r; std::istringstream is(buf.substr(0, end)); std::string line; std::getline(is, line);
            r.status = std::stoi(line.substr(9, 3));
            while (std::getline(is, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); auto c = line.find(':'); if (c != std::string::npos) r.headers[line.substr(0, c)] = std::string(trim(line.substr(c + 1))); }
            std::size_t len = std::stoul(r.headers["content-length"]);
            if (buf.size() >= end + 4 + len) { r.body = buf.substr(end + 4, len); buf.erase(0, end + 4 + len); co_return r; }
        }
        std::string more = co_await read_some(loop, fd);
        if (more.empty()) throw std::runtime_error("connection closed early");
        buf += more;
    }
}
static int status_of(const std::string& resp) { return std::stoi(resp.substr(9, 3)); }
static std::string body_of(const std::string& resp) { return resp.substr(resp.find("\r\n\r\n") + 4); }
static std::string get(const std::string& target, const char* extra = "") { return "GET " + target + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n" + extra + "\r\n"; }

// ---- parser unit tests (no sockets) -----------------------------------------------------------
void parser_tests() {
    std::string req = "POST /a/b?x=1&y=2 HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\nX-Thing:   spaced value  \r\n\r\nhello";
    for (std::size_t cut = 0; cut < req.size(); ++cut)                               // every prefix is "need more", never an error
        CHECK(parse_request(std::string_view(req).substr(0, cut)).status == ParseStatus::need_more);
    auto r = parse_request(req);
    CHECK(r.status == ParseStatus::done && r.consumed == req.size());
    CHECK(r.req.method == "POST" && r.req.path == "/a/b" && r.req.query == "x=1&y=2" && r.req.body == "hello");
    CHECK(r.req.headers["x-thing"] == "spaced value");
    CHECK(*r.req.query_param("y") == "2" && !r.req.query_param("z"));
    // pipelining: consumed stops at the end of the first request
    std::string two = "GET /1 HTTP/1.1\r\n\r\nGET /2 HTTP/1.1\r\n\r\n";
    auto p1 = parse_request(two); CHECK(p1.status == ParseStatus::done && p1.req.path == "/1" && p1.consumed == 19);
    CHECK(parse_request(std::string_view(two).substr(p1.consumed)).req.path == "/2");
    // rejections
    CHECK(parse_request("GET /x HTTP/2.0\r\n\r\n").status == ParseStatus::bad_request);
    CHECK(parse_request("get /x HTTP/1.1\r\n\r\n").status == ParseStatus::bad_request);
    CHECK(parse_request("GET x HTTP/1.1\r\n\r\n").status == ParseStatus::bad_request);
    CHECK(parse_request("GET /x HTTP/1.1\r\nBad Header : v\r\n\r\n").status == ParseStatus::bad_request);       // space before colon
    CHECK(parse_request("POST /x HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n").status == ParseStatus::bad_request);
    CHECK(parse_request("POST /x HTTP/1.1\r\nContent-Length: 12x\r\n\r\n").status == ParseStatus::bad_request);
    CHECK(parse_request("POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n").status == ParseStatus::not_implemented);
    CHECK(parse_request("POST /x HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n").status == ParseStatus::body_too_large);
    CHECK(parse_request(std::string(20000, 'a')).status == ParseStatus::header_too_large);
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    parser_tests();

    EventLoop loop;
    Router r;
    r.add("GET", "/hello", [](Request&) -> Task<Response> { co_return Response::text(200, "Hello\n"); });
    r.add("GET", "/users/:id", [](Request& q) -> Task<Response> { co_return Response::text(200, "user " + q.params["id"]); });
    r.add("GET", "/sum", [](Request& q) -> Task<Response> { co_return Response::text(200, std::to_string(std::stoi(*q.query_param("a")) + std::stoi(*q.query_param("b")))); });
    r.add("POST", "/echo", [](Request& q) -> Task<Response> { co_return Response::text(200, q.body); });
    r.add("GET", "/boom", [](Request&) -> Task<Response> { throw std::runtime_error("handler failed"); co_return Response{}; });
    EventLoop* lp = &loop;
    r.add("GET", "/slow", [lp](Request&) -> Task<Response> { co_await lp->sleep(100ms); co_return Response::text(200, "slow\n"); });
    Server server(loop, std::move(r));
    const std::uint16_t port = server.listen(0);

    auto tests = [&]() -> Task<void> {
        {   // basic routes
            auto a = co_await exchange(loop, port, get("/hello"));
            CHECK(status_of(a) == 200 && body_of(a) == "Hello\n");
            CHECK(body_of(co_await exchange(loop, port, get("/users/42"))) == "user 42");
            CHECK(body_of(co_await exchange(loop, port, get("/sum?a=2&b=40"))) == "42");
        }
        {   // error responses
            CHECK(status_of(co_await exchange(loop, port, get("/nope"))) == 404);
            auto m = co_await exchange(loop, port, "DELETE /hello HTTP/1.1\r\nConnection: close\r\n\r\n");
            CHECK(status_of(m) == 405 && m.find("allow: GET") != std::string::npos);
            CHECK(status_of(co_await exchange(loop, port, "BROKEN\r\n\r\n")) == 400);
            CHECK(status_of(co_await exchange(loop, port, "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n")) == 501);
            CHECK(status_of(co_await exchange(loop, port, "POST /echo HTTP/1.1\r\nContent-Length: 2000000\r\n\r\n")) == 413);
            CHECK(status_of(co_await exchange(loop, port, "GET /x HTTP/1.1\r\nX: " + std::string(20000, 'a') + "\r\n\r\n")) == 431);
            auto b = co_await exchange(loop, port, get("/boom"));
            CHECK(status_of(b) == 500 && b.find("handler failed") != std::string::npos);
            CHECK(status_of(co_await exchange(loop, port, get("/hello"))) == 200);       // the server survived
        }
        {   // request split across packets: header, pause, body
            Fd fd = co_await connect_to(loop, port);
            co_await write_all(loop, fd.get(), "POST /echo HTTP/1.1\r\nConnection: close\r\nContent-Length: 11\r\n\r\nhello");
            co_await loop.sleep(30ms);
            co_await write_all(loop, fd.get(), " world");
            std::string buf; Resp rr = co_await read_response(loop, fd.get(), buf);
            CHECK(rr.status == 200 && rr.body == "hello world");
        }
        {   // keep-alive: several requests on one connection; then pipelining: three requests in one write
            const auto before = server.connections_total();
            Fd fd = co_await connect_to(loop, port); std::string buf;
            for (int i = 0; i < 3; ++i) {
                co_await write_all(loop, fd.get(), "GET /users/" + std::to_string(i) + " HTTP/1.1\r\nHost: x\r\n\r\n");
                Resp rr = co_await read_response(loop, fd.get(), buf);
                CHECK(rr.status == 200 && rr.body == "user " + std::to_string(i) && rr.headers["connection"] == "keep-alive");
            }
            co_await write_all(loop, fd.get(), "GET /users/a HTTP/1.1\r\n\r\nGET /users/b HTTP/1.1\r\n\r\nGET /users/c HTTP/1.1\r\nConnection: close\r\n\r\n");
            for (char c : {'a', 'b', 'c'}) { Resp rr = co_await read_response(loop, fd.get(), buf); CHECK(rr.body == std::string("user ") + c); }
            CHECK(server.connections_total() == before + 1);                               // one TCP connection served all six
        }
        {   // concurrency: 200 clients x a 100 ms handler complete in far less than 200 x 100 ms
            auto start = EventLoop::Clock::now();
            int ok = 0;
            auto one = [&]() -> Task<void> { auto s = co_await exchange(loop, port, get("/slow")); if (status_of(s) == 200) ++ok; };
            for (int i = 0; i < 200; ++i) loop.spawn(one());
            while (ok < 200) co_await loop.sleep(10ms);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(EventLoop::Clock::now() - start).count();
            std::cout << "200 concurrent /slow requests (100 ms each): " << ms << " ms wall\n";
            CHECK(ms < 1000);
        }
        // graceful shutdown: connections drain, the accept loop exits, run() returns
        while (server.connections_open() != 0) co_await loop.sleep(5ms);
        server.shutdown();
    };
    loop.spawn(tests());
    loop.run();
    CHECK(loop.active_tasks() == 0 && loop.task_errors() == 0 && server.connections_open() == 0);
    std::cout << "requests served: " << server.requests() << ", connections: " << server.connections_total() << "\nall tests passed\n";
}
