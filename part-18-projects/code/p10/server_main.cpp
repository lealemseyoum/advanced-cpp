// A runnable demo server:  ./server 8080   then   curl -i localhost:8080/hello
#include "http.hpp"
#include <csignal>
#include <iostream>
using namespace edu; using namespace edu::http;

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    const std::uint16_t port = argc > 1 ? static_cast<std::uint16_t>(std::stoi(argv[1])) : 8080;
    EventLoop loop;
    Router r;
    r.add("GET", "/hello", [](Request&) -> Task<Response> { co_return Response::text(200, "Hello, world\n"); });
    r.add("GET", "/users/:id", [](Request& q) -> Task<Response> { co_return Response::text(200, "user " + q.params["id"] + "\n"); });
    r.add("POST", "/echo", [](Request& q) -> Task<Response> { co_return Response::text(200, q.body); });
    r.add("GET", "/sum", [](Request& q) -> Task<Response> {
        int a = std::stoi(q.query_param("a").value_or("0")), b = std::stoi(q.query_param("b").value_or("0"));
        co_return Response::text(200, std::to_string(a + b) + "\n"); });
    Server s(loop, std::move(r));
    std::cout << "listening on http://127.0.0.1:" << s.listen(port) << "\n" << std::flush;
    loop.run();
}
