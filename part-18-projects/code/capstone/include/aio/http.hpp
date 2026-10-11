// HTTP/1.1 server on the aio runtime.
//   - Request fields live in a per-connection Arena (std::pmr), reset after every request
//   - Router::add is constrained by a concept; handlers are coroutines
//   - Header/idle/write timeouts; graceful stop; one SO_REUSEPORT listener per loop thread
#pragma once
#include "arena.hpp"
#include "net.hpp"
#include "runtime.hpp"
#include <algorithm>
#include <charconv>
#include <concepts>
#include <map>
#include <memory_resource>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace aio::http {

using namespace std::chrono_literals;

struct Request {
    using Str = std::pmr::string;
    using Map = std::pmr::map<Str, Str, std::less<>>;
    explicit Request(std::pmr::memory_resource* mr = std::pmr::get_default_resource())
        : method(mr), path(mr), query(mr), version(mr), body(mr), headers(mr), params(mr) {}
    Str method, path, query, version, body;
    Map headers, params;

    std::string_view header(std::string_view name) const {
        auto it = headers.find(name);
        return it == headers.end() ? std::string_view{} : std::string_view(it->second);
    }
    bool keep_alive() const {
        std::string v(header("connection"));
        std::ranges::transform(v, v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (v == "close") return false;
        if (v == "keep-alive") return true;
        return version == "HTTP/1.1";
    }
    // Ranges: split "a=1&b=2" on '&', then on the first '='.
    std::optional<std::string_view> query_param(std::string_view key) const {
        std::string_view q = query;
        for (auto part : std::views::split(q, '&')) {
            std::string_view kv(part.begin(), part.end());
            auto eq = kv.find('=');
            if (kv.substr(0, eq) == key) return eq == std::string_view::npos ? std::string_view{} : kv.substr(eq + 1);
        }
        return std::nullopt;
    }
};

struct Response {
    int status = 200;
    std::map<std::string, std::string> headers;
    std::string body;
    static Response text(int status, std::string body) { Response r; r.status = status; r.headers["content-type"] = "text/plain; charset=utf-8"; r.body = std::move(body); return r; }
};

inline const char* reason(int s) {
    switch (s) { case 200: return "OK"; case 400: return "Bad Request"; case 404: return "Not Found"; case 405: return "Method Not Allowed";
        case 408: return "Request Timeout"; case 413: return "Content Too Large"; case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error"; case 501: return "Not Implemented"; default: return "Unknown"; }
}
inline std::string serialize(const Response& r, bool keep_alive) {
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
    for (auto& [k, v] : r.headers) { out += k; out += ": "; out += v; out += "\r\n"; }
    out += "content-length: " + std::to_string(r.body.size()) + "\r\n";
    out += keep_alive ? "connection: keep-alive\r\n\r\n" : "connection: close\r\n\r\n";
    out += r.body;
    return out;
}

// ---- parser --------------------------------------------------------------------------------------------
struct Limits { std::size_t max_header = 16 * 1024, max_body = 1024 * 1024; };
enum class ParseStatus { need_more, done, bad_request, header_too_large, body_too_large, not_implemented };
struct ParseResult {
    explicit ParseResult(std::pmr::memory_resource* mr) : req(mr) {}
    ParseStatus status = ParseStatus::need_more;
    std::size_t consumed = 0;
    Request req;
};
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}
inline ParseResult parse_request(std::string_view buf, std::pmr::memory_resource* mr, const Limits& lim = {}) {
    ParseResult res(mr);
    const auto end = buf.find("\r\n\r\n");
    if (end == std::string_view::npos) { res.status = buf.size() > lim.max_header ? ParseStatus::header_too_large : ParseStatus::need_more; return res; }
    if (end > lim.max_header) { res.status = ParseStatus::header_too_large; return res; }
    std::string_view head = buf.substr(0, end);
    const auto eol = head.find("\r\n");
    std::string_view line = head.substr(0, eol);
    const auto sp1 = line.find(' ');
    const auto sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
    auto bad = [&] { res.status = ParseStatus::bad_request; return std::move(res); };
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) return bad();
    Request& r = res.req;
    std::string_view method = line.substr(0, sp1), target = line.substr(sp1 + 1, sp2 - sp1 - 1), version = line.substr(sp2 + 1);
    if (method.empty() || target.empty() || target[0] != '/') return bad();
    if (version != "HTTP/1.1" && version != "HTTP/1.0") return bad();
    if (!std::ranges::all_of(method, [](unsigned char c) { return std::isupper(c) != 0; })) return bad();
    r.method.assign(method); r.version.assign(version);
    if (auto q = target.find('?'); q != std::string_view::npos) { r.query.assign(target.substr(q + 1)); target = target.substr(0, q); }
    r.path.assign(target);
    std::string_view rest = eol == std::string_view::npos ? std::string_view{} : head.substr(eol + 2);
    while (!rest.empty()) {
        const auto e = rest.find("\r\n");
        std::string_view h = rest.substr(0, e);
        rest = e == std::string_view::npos ? std::string_view{} : rest.substr(e + 2);
        const auto colon = h.find(':');
        if (colon == std::string_view::npos || colon == 0) return bad();
        std::string_view rawname = h.substr(0, colon);
        if (rawname.find_first_of(" \t") != std::string_view::npos) return bad();           // smuggling vector
        Request::Str name(mr); name.assign(rawname);
        std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string_view value = trim(h.substr(colon + 1));
        if (name == "content-length") { auto it = r.headers.find(std::string_view(name)); if (it != r.headers.end() && it->second != value) return bad(); }
        r.headers[std::move(name)].assign(value);
    }
    if (r.headers.contains(std::string_view("transfer-encoding"))) { res.status = ParseStatus::not_implemented; return res; }
    std::size_t len = 0;
    if (auto v = r.header("content-length"); !v.empty()) {
        auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), len);
        if (ec != std::errc{} || p != v.data() + v.size()) return bad();
        if (len > lim.max_body) { res.status = ParseStatus::body_too_large; return res; }
    }
    const std::size_t body_start = end + 4;
    if (buf.size() < body_start + len) { res.status = ParseStatus::need_more; return res; }
    r.body.assign(buf.substr(body_start, len));
    res.consumed = body_start + len;
    res.status = ParseStatus::done;
    return res;
}

// ---- router ----------------------------------------------------------------------------------------
template <class H>
concept RequestHandler = std::move_constructible<H> && requires(H& h, Request& r) {
    { h(r) } -> std::same_as<Task<Response>>;
};

class Router {
public:
    using Fn = Function<Task<Response>(Request&)>;
    template <RequestHandler H> Router& add(std::string_view method, std::string_view pattern, H h) {
        Route r{std::string(method), {}, Fn(std::move(h))};
        for (auto seg : segments(pattern)) r.segs.emplace_back(seg);
        routes_.push_back(std::move(r));
        return *this;
    }
    template <RequestHandler H> Router& get(std::string_view p, H h) { return add("GET", p, std::move(h)); }
    template <RequestHandler H> Router& post(std::string_view p, H h) { return add("POST", p, std::move(h)); }

    struct Match { const Fn* fn = nullptr; std::vector<std::pair<std::string, std::string>> params; std::vector<std::string> allowed; };
    Match find(std::string_view method, std::string_view path) const {
        Match m; auto got = segments(path);
        for (auto& r : routes_) {
            std::vector<std::pair<std::string, std::string>> p;
            if (r.segs.size() != got.size()) continue;
            bool ok = true;
            for (std::size_t i = 0; i < got.size() && ok; ++i) {
                if (!r.segs[i].empty() && r.segs[i][0] == ':') p.emplace_back(r.segs[i].substr(1), std::string(got[i]));
                else ok = r.segs[i] == got[i];
            }
            if (!ok) continue;
            if (r.method == method) { m.fn = &r.fn; m.params = std::move(p); return m; }
            m.allowed.push_back(r.method);
        }
        return m;
    }
private:
    struct Route { std::string method; std::vector<std::string> segs; Fn fn; };
    // Ranges: split on '/', drop empty segments.
    static std::vector<std::string_view> segments(std::string_view p) {
        std::vector<std::string_view> out;
        for (auto part : std::views::split(p, '/')) { std::string_view s(part.begin(), part.end()); if (!s.empty()) out.push_back(s); }
        return out;
    }
    std::vector<Route> routes_;
};

// ---- server --------------------------------------------------------------------------------------------
struct ServerConfig {
    std::uint16_t port = 0;
    Limits limits;
    Loop::Duration header_timeout = 5s;      // from the first byte of a request to its last header byte
    Loop::Duration idle_timeout = 30s;       // between requests on a keep-alive connection
    Loop::Duration write_timeout = 5s;
    bool use_arena = true;                   // per-connection arena for request objects
    std::size_t arena_chunk = 16 * 1024;
};
struct ServerStats { std::atomic<std::size_t> requests{0}, connections{0}, open{0}, timeouts{0}, errors{0}; };

class Server {
public:
    Server(Runtime& rt, Router router, ServerConfig cfg = {}) : rt_(rt), router_(std::move(router)), cfg_(cfg) {}
    ~Server() { stop(); }

    // Binds one SO_REUSEPORT listener per loop thread. Returns the port (useful with port 0).
    Result<std::uint16_t> start() {
        std::uint16_t port = cfg_.port;
        for (std::size_t i = 0; i < rt_.io_threads(); ++i) {
            auto l = net::listen_tcp(port, /*reuse_port=*/true);
            if (!l) return std::unexpected(l.error());
            if (i == 0) port = net::local_port(l->get());
            listeners_.push_back(std::make_unique<net::Fd>(std::move(*l)));
        }
        accepting_.store(static_cast<int>(rt_.io_threads()));
        for (std::size_t i = 0; i < rt_.io_threads(); ++i) rt_.spawn_on(i, accept_loop(i));
        return port;
    }
    // Graceful: stop accepting, let connections finish their current request, wait until all are closed (or the timeout).
    bool stop(std::chrono::milliseconds wait = 5s) {
        stopping_.store(true, std::memory_order_release);
        auto deadline = std::chrono::steady_clock::now() + wait;
        while ((accepting_.load() > 0 || stats_.open.load() > 0) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        return accepting_.load() == 0 && stats_.open.load() == 0;
    }
    const ServerStats& stats() const noexcept { return stats_; }

private:
    Task<void> accept_loop(std::size_t i) {
        Loop& loop = rt_.loop(i);
        const int lfd = listeners_[i]->get();
        while (!stopping_.load(std::memory_order_acquire)) {
            auto c = co_await net::accept(loop, lfd, 50ms);              // wake every 50 ms to observe stop
            if (!c) { if (c.error() != make_error_code(Errc::timeout)) { stats_.errors++; co_await loop.sleep(10ms); } continue; }
            loop.spawn(connection(loop, std::move(*c)));
        }
        listeners_[i]->reset();
        accepting_--;
    }

    Task<void> connection(Loop& loop, net::Fd fd) {
        stats_.connections++; stats_.open++;
        struct Open { std::atomic<std::size_t>& n; ~Open() { n--; } } open{stats_.open};
        Arena arena(cfg_.arena_chunk);
        std::pmr::memory_resource* mr = cfg_.use_arena ? static_cast<std::pmr::memory_resource*>(&arena) : std::pmr::new_delete_resource();
        std::string buf;
        char tmp[8192];
        for (;;) {
            {   // everything that lives in the arena is confined to this scope
                ParseResult pr = parse_request(buf, mr, cfg_.limits);
                if (pr.status == ParseStatus::need_more) {
                    // Wait for more bytes. An idle connection (nothing buffered) is polled in 100 ms slices so that a
                    // stopping server can close it; a half-received request gets the header timeout.
                    const bool idle = buf.empty();
                    auto remaining = idle ? cfg_.idle_timeout : cfg_.header_timeout;
                    for (;;) {
                        auto slice = std::min<Loop::Duration>(remaining, 100ms);
                        auto n = co_await net::read_some(loop, fd.get(), tmp, sizeof tmp, slice);
                        if (n) { buf.append(tmp, *n); break; }
                        if (n.error() == make_error_code(Errc::closed)) co_return;
                        if (n.error() != make_error_code(Errc::timeout)) { stats_.errors++; co_return; }
                        remaining -= slice;
                        if (idle && stopping_.load(std::memory_order_acquire)) co_return;
                        if (remaining <= Loop::Duration::zero()) {
                            stats_.timeouts++;
                            if (!idle) (void)co_await net::write_all(loop, fd.get(), serialize(Response::text(408, "Request Timeout\n"), false), cfg_.write_timeout);
                            co_return;
                        }
                    }
                    continue;
                }
                if (pr.status != ParseStatus::done) {
                    int code = pr.status == ParseStatus::header_too_large ? 431 : pr.status == ParseStatus::body_too_large ? 413
                             : pr.status == ParseStatus::not_implemented ? 501 : 400;
                    (void)co_await net::write_all(loop, fd.get(), serialize(Response::text(code, std::string(reason(code)) + "\n"), false), cfg_.write_timeout);
                    co_return;
                }
                buf.erase(0, pr.consumed);
                stats_.requests++;
                const bool ka = pr.req.keep_alive() && !stopping_.load(std::memory_order_acquire);
                Response resp = co_await dispatch(pr.req);
                auto w = co_await net::write_all(loop, fd.get(), serialize(resp, ka), cfg_.write_timeout);
                if (!w || !ka) co_return;
            }
            if (cfg_.use_arena) arena.reset();                                      // all request memory is released at once
        }
    }

    Task<Response> dispatch(Request& req) {
        auto m = router_.find(req.method, req.path);
        if (!m.fn) {
            if (!m.allowed.empty()) {
                Response r = Response::text(405, "Method Not Allowed\n");
                std::string allow; for (auto& a : m.allowed) allow += (allow.empty() ? "" : ", ") + a;
                r.headers["allow"] = allow;
                co_return r;
            }
            co_return Response::text(404, "Not Found\n");
        }
        for (auto& [k, v] : m.params) req.params[Request::Str(k, req.params.get_allocator())].assign(v);
        try { co_return co_await (*m.fn)(req); }
        catch (const std::exception& e) { co_return Response::text(500, std::string("Internal Server Error: ") + e.what() + "\n"); }
        catch (...) { co_return Response::text(500, "Internal Server Error\n"); }
    }

    Runtime& rt_;
    Router router_;
    ServerConfig cfg_;
    ServerStats stats_;
    std::vector<std::unique_ptr<net::Fd>> listeners_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> accepting_{0};
};

}  // namespace aio::http
