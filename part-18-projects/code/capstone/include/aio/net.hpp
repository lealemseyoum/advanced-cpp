// Non-blocking TCP on top of Loop. No exceptions on expected failures: everything returns Result<T>.
#pragma once
#include "error.hpp"
#include "loop.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <cstdint>
#include <optional>
#include <string_view>

namespace aio::net {

class Fd {                                   // owner: closes in the destructor (Project 1)
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Fd& operator=(Fd&& o) noexcept { if (this != &o) { reset(); fd_ = std::exchange(o.fd_, -1); } return *this; }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }
    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    void reset() noexcept { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
private:
    int fd_ = -1;
};

inline Result<Fd> listen_tcp(std::uint16_t port, bool reuse_port, int backlog = 1024) {
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd) return fail_errno(errno);
    int one = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (reuse_port) ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) return fail_errno(errno);
    if (::listen(fd.get(), backlog) < 0) return fail_errno(errno);
    return fd;
}
inline std::uint16_t local_port(int fd) noexcept {
    sockaddr_in a{}; socklen_t l = sizeof a;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l);
    return ntohs(a.sin_port);
}

// Waits up to `poll` for a connection; Errc::timeout lets the caller re-check a stop flag.
inline Task<Result<Fd>> accept(Loop& loop, int lfd, std::optional<Loop::Duration> poll = std::nullopt) {
    for (;;) {
        int c = ::accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c >= 0) { int one = 1; ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); co_return Fd(c); }
        if (errno == EINTR || errno == ECONNABORTED) continue;
        if (errno != EAGAIN) co_return fail_errno(errno);
        if (!co_await loop.readable(lfd, poll)) co_return fail(Errc::timeout);
    }
}
inline Task<Result<Fd>> connect(Loop& loop, std::uint16_t port, std::optional<Loop::Duration> timeout = std::nullopt) {
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd) co_return fail_errno(errno);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
        if (errno != EINPROGRESS) co_return fail_errno(errno);
        if (!co_await loop.writable(fd.get(), timeout)) co_return fail(Errc::timeout);
        int err = 0; socklen_t l = sizeof err; ::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &err, &l);
        if (err) co_return fail_errno(err);
    }
    co_return fd;
}
// Reads at least 1 byte. EOF is reported as Errc::closed, a missed deadline as Errc::timeout.
inline Task<Result<std::size_t>> read_some(Loop& loop, int fd, char* buf, std::size_t cap, std::optional<Loop::Duration> timeout = std::nullopt) {
    for (;;) {
        ssize_t n = ::recv(fd, buf, cap, 0);
        if (n > 0) co_return static_cast<std::size_t>(n);
        if (n == 0) co_return fail(Errc::closed);
        if (errno == EINTR) continue;
        if (errno == ECONNRESET) co_return fail(Errc::closed);
        if (errno != EAGAIN) co_return fail_errno(errno);
        if (!co_await loop.readable(fd, timeout)) co_return fail(Errc::timeout);
    }
}
inline Task<Result<void>> write_all(Loop& loop, int fd, std::string_view s, std::optional<Loop::Duration> timeout = std::nullopt) {
    while (!s.empty()) {
        ssize_t n = ::send(fd, s.data(), s.size(), MSG_NOSIGNAL);
        if (n >= 0) { s.remove_prefix(static_cast<std::size_t>(n)); continue; }
        if (errno == EINTR) continue;
        if (errno == EPIPE || errno == ECONNRESET) co_return fail(Errc::closed);
        if (errno != EAGAIN) co_return fail_errno(errno);
        if (!co_await loop.writable(fd, timeout)) co_return fail(Errc::timeout);
    }
    co_return Result<void>{};
}

}  // namespace aio::net
