// Project 10 reference solution: an HTTP/1.1 server on the Project 9 event loop. No framework.
//   net:     nonblocking accept / connect / read / write as awaitables
//   parser:  incremental, with size limits; pipelining-safe
//   router:  method + path pattern with :params
//   server:  one coroutine per connection, keep-alive, error responses, graceful shutdown
#pragma once
#include "../p09/event_loop.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edu::http {

// ------------------------------------------------------------------------------------------------ fd + net
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Fd& operator=(Fd&& o) noexcept { if (this != &o) { reset(); fd_ = std::exchange(o.fd_, -1); } return *this; }
    ~Fd() { reset(); }
    int get() const noexcept { return fd_; }
    void reset() noexcept { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
private:
    int fd_ = -1;
};

[[noreturn]] inline void throw_errno(const char* what) { throw std::system_error(errno, std::generic_category(), what); }

inline Fd listen_on(std::uint16_t port, int backlog = 512) {     // port 0 = pick a free port
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (fd.get() < 0) throw_errno("socket");
    int one = 1; ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) throw_errno("bind");
    if (::listen(fd.get(), backlog) < 0) throw_errno("listen");
    return fd;
}
inline std::uint16_t local_port(int fd) {
    sockaddr_in a{}; socklen_t l = sizeof a;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l);
    return ntohs(a.sin_port);
}

inline Task<Fd> accept_one(EventLoop& loop, int listen_fd) {
    for (;;) {
        int c = ::accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c >= 0) { int one = 1; ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); co_return Fd(c); }
        if (errno == EINTR || errno == ECONNABORTED) continue;
        if (errno != EAGAIN) throw_errno("accept4");
        co_await loop.readable(listen_fd);
    }
}
inline Task<Fd> connect_to(EventLoop& loop, std::uint16_t port) {
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (fd.get() < 0) throw_errno("socket");
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
        if (errno != EINPROGRESS) throw_errno("connect");
        co_await loop.writable(fd.get());
        int err = 0; socklen_t l = sizeof err; ::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &err, &l);
        if (err) { errno = err; throw_errno("connect"); }
    }
    co_return fd;
}
// Returns bytes read; empty string means the peer closed (EOF).
inline Task<std::string> read_some(EventLoop& loop, int fd) {
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n >= 0) co_return std::string(buf, static_cast<std::size_t>(n));
        if (errno == EINTR) continue;
        if (errno == ECONNRESET) co_return std::string();
        if (errno != EAGAIN) throw_errno("recv");
        co_await loop.readable(fd);
    }
}
inline Task<void> write_all(EventLoop& loop, int fd, std::string_view s) {
    while (!s.empty()) {
        ssize_t n = ::send(fd, s.data(), s.size(), MSG_NOSIGNAL);
        if (n >= 0) { s.remove_prefix(static_cast<std::size_t>(n)); continue; }
        if (errno == EINTR) continue;
        if (errno == EPIPE || errno == ECONNRESET) co_return;          // peer is gone: nothing more to do
        if (errno != EAGAIN) throw_errno("send");
        co_await loop.writable(fd);
    }
}

// ------------------------------------------------------------------------------------------------ messages
using Headers = std::map<std::string, std::string>;       // lower-cased names

struct Request {
    std::string method, path, query, version, body;
    Headers headers;
    std::map<std::string, std::string> params;             // from the route pattern
    bool keep_alive() const {
        auto it = headers.find("connection");
        std::string v = it == headers.end() ? "" : it->second;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
        if (v == "close") return false;
        if (v == "keep-alive") return true;
        return version == "HTTP/1.1";
    }
    std::optional<std::string> query_param(std::string_view key) const {          // no percent-decoding: left as an exercise
        std::string_view q = query;
        while (!q.empty()) {
            auto amp = q.find('&'); auto kv = q.substr(0, amp);
            auto eq = kv.find('=');
            if (kv.substr(0, eq) == key) return std::string(eq == std::string_view::npos ? "" : kv.substr(eq + 1));
            if (amp == std::string_view::npos) break;
            q.remove_prefix(amp + 1);
        }
        return std::nullopt;
    }
};

struct Response {
    int status = 200;
    Headers headers;
    std::string body;
    static Response text(int status, std::string body) { Response r; r.status = status; r.headers["content-type"] = "text/plain; charset=utf-8"; r.body = std::move(body); return r; }
};

inline const char* reason(int s) {
    switch (s) { case 200: return "OK"; case 201: return "Created"; case 204: return "No Content"; case 400: return "Bad Request";
        case 404: return "Not Found"; case 405: return "Method Not Allowed"; case 413: return "Content Too Large"; case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error"; case 501: return "Not Implemented"; default: return "Unknown"; }
}
inline std::string serialize(const Response& r, bool keep_alive) {
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
    for (auto& [k, v] : r.headers) out += k + ": " + v + "\r\n";
    out += "content-length: " + std::to_string(r.body.size()) + "\r\n";
    out += keep_alive ? "connection: keep-alive\r\n" : "connection: close\r\n";
    out += "\r\n";
    out += r.body;
    return out;
}

// ------------------------------------------------------------------------------------------------ parser
struct Limits { std::size_t max_header = 16 * 1024, max_body = 1024 * 1024; };
enum class ParseStatus { need_more, done, bad_request, header_too_large, body_too_large, not_implemented };
struct ParseResult { ParseStatus status = ParseStatus::need_more; std::size_t consumed = 0; Request req; };

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}
inline ParseResult parse_request(std::string_view buf, const Limits& lim = {}) {
    ParseResult res;
    const auto end = buf.find("\r\n\r\n");
    if (end == std::string_view::npos) {
        res.status = buf.size() > lim.max_header ? ParseStatus::header_too_large : ParseStatus::need_more;
        return res;
    }
    if (end > lim.max_header) { res.status = ParseStatus::header_too_large; return res; }
    std::string_view head = buf.substr(0, end);
    // request line
    const auto eol = head.find("\r\n");
    std::string_view line = head.substr(0, eol);
    const auto sp1 = line.find(' '); const auto sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
    auto bad = [&] { res.status = ParseStatus::bad_request; return res; };
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) return bad();
    Request& r = res.req;
    r.method = std::string(line.substr(0, sp1));
    std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    r.version = std::string(line.substr(sp2 + 1));
    if (r.method.empty() || target.empty() || target[0] != '/') return bad();
    if (r.version != "HTTP/1.1" && r.version != "HTTP/1.0") return bad();
    if (!std::all_of(r.method.begin(), r.method.end(), [](unsigned char c) { return std::isupper(c); })) return bad();
    if (auto q = target.find('?'); q != std::string_view::npos) { r.query = std::string(target.substr(q + 1)); target = target.substr(0, q); }
    r.path = std::string(target);
    // headers
    std::string_view rest = eol == std::string_view::npos ? std::string_view{} : head.substr(eol + 2);
    while (!rest.empty()) {
        const auto e = rest.find("\r\n"); std::string_view h = rest.substr(0, e);
        rest = e == std::string_view::npos ? std::string_view{} : rest.substr(e + 2);
        const auto colon = h.find(':');
        if (colon == std::string_view::npos || colon == 0) return bad();
        std::string name(h.substr(0, colon));
        if (name.find_first_of(" \t") != std::string::npos) return bad();                 // no whitespace before the colon (request smuggling vector)
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        std::string value(trim(h.substr(colon + 1)));
        if (name == "content-length" && r.headers.count(name) && r.headers[name] != value) return bad();   // conflicting lengths
        r.headers[name] = value;
    }
    if (r.headers.count("transfer-encoding")) { res.status = ParseStatus::not_implemented; return res; }   // chunked: out of scope
    std::size_t len = 0;
    if (auto it = r.headers.find("content-length"); it != r.headers.end()) {
        const std::string& v = it->second;
        auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), len);
        if (ec != std::errc{} || p != v.data() + v.size()) return bad();
        if (len > lim.max_body) { res.status = ParseStatus::body_too_large; return res; }
    }
    const std::size_t body_start = end + 4;
    if (buf.size() < body_start + len) { res.status = ParseStatus::need_more; res.req = {}; return res; }   // wait for the body
    r.body = std::string(buf.substr(body_start, len));
    res.consumed = body_start + len;
    res.status = ParseStatus::done;
    return res;
}

// ------------------------------------------------------------------------------------------------ router
using Handler = std::function<Task<Response>(Request&)>;

class Router {
public:
    void add(std::string method, std::string pattern, Handler h) { routes_.push_back({std::move(method), split(pattern), std::move(h)}); }
    struct Match { const Handler* handler = nullptr; std::map<std::string, std::string> params; std::vector<std::string> allowed; };
    Match find(const std::string& method, const std::string& path) const {
        Match m; auto segs = split(path);
        for (auto& r : routes_) {
            std::map<std::string, std::string> p;
            if (!match(r.segs, segs, p)) continue;
            if (r.method == method) { m.handler = &r.handler; m.params = std::move(p); return m; }
            m.allowed.push_back(r.method);                                // path matched, method did not
        }
        return m;
    }
private:
    struct Route { std::string method; std::vector<std::string> segs; Handler handler; };
    static std::vector<std::string> split(std::string_view p) {
        std::vector<std::string> out;
        while (!p.empty()) { if (p.front() == '/') { p.remove_prefix(1); continue; } auto s = p.find('/'); out.emplace_back(p.substr(0, s)); p = s == std::string_view::npos ? std::string_view{} : p.substr(s); }
        return out;
    }
    static bool match(const std::vector<std::string>& pat, const std::vector<std::string>& got, std::map<std::string, std::string>& out) {
        if (pat.size() != got.size()) return false;
        for (std::size_t i = 0; i < pat.size(); ++i) {
            if (!pat[i].empty() && pat[i][0] == ':') out[pat[i].substr(1)] = got[i];
            else if (pat[i] != got[i]) return false;
        }
        return true;
    }
    std::vector<Route> routes_;
};

// ------------------------------------------------------------------------------------------------ server
class Server {
public:
    Server(EventLoop& loop, Router router, Limits lim = {}) : loop_(loop), router_(std::move(router)), lim_(lim) {}

    // Start accepting. The accept loop and every connection are detached tasks on the loop.
    std::uint16_t listen(std::uint16_t port = 0) {
        listener_ = listen_on(port);
        std::uint16_t p = local_port(listener_.get());
        loop_.spawn(accept_loop());
        return p;
    }
    // Graceful: stop accepting; existing connections finish their current request and close when idle/peer closes.
    void shutdown() {
        stopping_ = true;
        // wake the accept loop (it is parked in epoll) with a throwaway connection
        loop_.spawn(poke(local_port(listener_.get())));
    }
    std::size_t requests() const noexcept { return requests_; }
    std::size_t connections_open() const noexcept { return open_; }
    std::size_t connections_total() const noexcept { return total_; }

private:
    Task<void> poke(std::uint16_t port) { Fd f = co_await connect_to(loop_, port); }

    Task<void> accept_loop() {
        while (!stopping_) {
            Fd c = co_await accept_one(loop_, listener_.get());
            if (stopping_) break;
            loop_.spawn(connection(std::move(c)));
        }
        listener_.reset();
    }

    Task<void> connection(Fd fd) {
        ++open_; ++total_;
        struct Count { std::size_t& n; ~Count() { --n; } } count{open_};
        std::string buf;
        for (;;) {
            ParseResult pr = parse_request(buf, lim_);
            if (pr.status == ParseStatus::need_more) {
                std::string more = co_await read_some(loop_, fd.get());
                if (more.empty()) co_return;                                          // peer closed (a partial request is dropped)
                buf += more;
                continue;
            }
            if (pr.status != ParseStatus::done) {
                int code = pr.status == ParseStatus::header_too_large ? 431 : pr.status == ParseStatus::body_too_large ? 413
                         : pr.status == ParseStatus::not_implemented ? 501 : 400;
                co_await write_all(loop_, fd.get(), serialize(Response::text(code, std::string(reason(code)) + "\n"), false));
                co_return;                                                            // protocol error: close
            }
            buf.erase(0, pr.consumed);                                                // keep pipelined bytes for the next iteration
            ++requests_;
            const bool ka = pr.req.keep_alive() && !stopping_;
            Response resp = co_await dispatch(pr.req);
            co_await write_all(loop_, fd.get(), serialize(resp, ka));
            if (!ka) co_return;
        }
    }

    Task<Response> dispatch(Request& req) {
        auto m = router_.find(req.method, req.path);
        if (!m.handler) {
            if (!m.allowed.empty()) {
                Response r = Response::text(405, "Method Not Allowed\n");
                std::string allow; for (auto& a : m.allowed) allow += (allow.empty() ? "" : ", ") + a;
                r.headers["allow"] = allow; co_return r;
            }
            co_return Response::text(404, "Not Found\n");
        }
        req.params = std::move(m.params);
        try { co_return co_await (*m.handler)(req); }
        catch (const std::exception& e) { co_return Response::text(500, std::string("Internal Server Error: ") + e.what() + "\n"); }
        catch (...) { co_return Response::text(500, "Internal Server Error\n"); }
    }

    EventLoop& loop_;
    Router router_;
    Limits lim_;
    Fd listener_;
    bool stopping_ = false;
    std::size_t requests_ = 0, open_ = 0, total_ = 0;
};

}  // namespace edu::http
