// How much does a per-connection arena change the cost of parsing a typical request?
#include "aio/http.hpp"
#include <chrono>
#include <cstdio>
using namespace aio;
static const std::string kReq =
    "GET /api/v1/users/12345/orders?status=open&limit=50 HTTP/1.1\r\nHost: example.com\r\nUser-Agent: bench/1.0\r\n"
    "Accept: application/json\r\nAccept-Encoding: gzip, deflate\r\nAccept-Language: en-US,en;q=0.9\r\n"
    "Cookie: session=abcdef0123456789abcdef0123456789; theme=dark\r\nX-Request-Id: 7f1c2d3e-4b5a-6789-abcd-ef0123456789\r\nConnection: keep-alive\r\n\r\n";
template <class F> double best_ns(F f, int n) { double b = 1e18; for (int r = 0; r < 7; ++r) { auto t0 = std::chrono::steady_clock::now(); f(); b = std::min(b, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n); } return b; }
int main() {
    const int N = 300000; volatile std::size_t sink = 0;
    auto heap = [&] { for (int i = 0; i < N; ++i) { auto r = http::parse_request(kReq, std::pmr::new_delete_resource()); sink = r.consumed; } };
    Arena arena(16 * 1024);
    auto arn = [&] { for (int i = 0; i < N; ++i) { { auto r = http::parse_request(kReq, &arena); sink = r.consumed; } arena.reset(); } };
    std::printf("parse one 9-header request (%zu bytes)\n  new_delete resource   %7.0f ns\n  Arena + reset         %7.0f ns\n", kReq.size(), best_ns(heap, N), best_ns(arn, N));
}
