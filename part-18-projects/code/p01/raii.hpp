// Project 1 reference solution: a small RAII resource library.
#pragma once
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace raii {

inline std::error_code last_errno() { return {errno, std::generic_category()}; }

// Generic unique handle: Traits supplies the handle type, the invalid value, and close().
template <class Traits>
class Unique {
public:
    using handle_type = typename Traits::handle_type;

    Unique() noexcept = default;
    explicit Unique(handle_type h) noexcept : h_(h) {}
    Unique(const Unique&) = delete;
    Unique& operator=(const Unique&) = delete;
    Unique(Unique&& o) noexcept : h_(std::exchange(o.h_, Traits::invalid())) {}
    Unique& operator=(Unique&& o) noexcept {
        if (this != &o) reset(o.release());
        return *this;
    }
    ~Unique() { reset(); }

    handle_type get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ != Traits::invalid(); }
    [[nodiscard]] handle_type release() noexcept { return std::exchange(h_, Traits::invalid()); }
    void reset(handle_type h = Traits::invalid()) noexcept {
        handle_type old = std::exchange(h_, h);
        if (old != Traits::invalid()) Traits::close(old);
    }
    friend void swap(Unique& a, Unique& b) noexcept { std::swap(a.h_, b.h_); }

private:
    handle_type h_ = Traits::invalid();
};

struct FdTraits {
    using handle_type = int;
    static constexpr int invalid() noexcept { return -1; }
    static void close(int fd) noexcept { ::close(fd); }  // EINTR: do NOT retry on Linux (fd is gone either way)
};
using Fd = Unique<FdTraits>;

struct FileTraits {
    using handle_type = std::FILE*;
    static std::FILE* invalid() noexcept { return nullptr; }
    static void close(std::FILE* f) noexcept { std::fclose(f); }
};
using File = Unique<FileTraits>;

inline std::expected<File, std::error_code> open_file(const char* path, const char* mode) {
    File f(std::fopen(path, mode));
    if (!f) return std::unexpected(last_errno());
    return f;
}

inline std::expected<Fd, std::error_code> open_fd(const char* path, int flags) {
    Fd fd(::open(path, flags | O_CLOEXEC));
    if (!fd) return std::unexpected(last_errno());
    return fd;
}

// Sockets are fds with socket-specific operations.
class Socket {
public:
    Socket() = default;
    explicit Socket(Fd fd) noexcept : fd_(std::move(fd)) {}
    static std::expected<std::pair<Socket, Socket>, std::error_code> pair() {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) return std::unexpected(last_errno());
        return std::pair{Socket(Fd(sv[0])), Socket(Fd(sv[1]))};
    }
    int fd() const noexcept { return fd_.get(); }
    explicit operator bool() const noexcept { return static_cast<bool>(fd_); }
    std::expected<std::size_t, std::error_code> send(std::string_view s) {
        for (;;) {
            ssize_t n = ::send(fd_.get(), s.data(), s.size(), MSG_NOSIGNAL);
            if (n >= 0) return static_cast<std::size_t>(n);
            if (errno != EINTR) return std::unexpected(last_errno());
        }
    }
    std::expected<std::string, std::error_code> recv(std::size_t max) {
        std::string buf(max, '\0');
        for (;;) {
            ssize_t n = ::recv(fd_.get(), buf.data(), max, 0);
            if (n >= 0) { buf.resize(static_cast<std::size_t>(n)); return buf; }
            if (errno != EINTR) return std::unexpected(last_errno());
        }
    }
    void shutdown_write() noexcept { if (fd_) ::shutdown(fd_.get(), SHUT_WR); }

private:
    Fd fd_;
};

// Scope guard with dismiss(); runs on scope exit (always) -- see Chapter 4.
template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F f) : f_(std::move(f)) {}
    ScopeExit(ScopeExit&& o) noexcept(std::is_nothrow_move_constructible_v<F>)
        : f_(std::move(o.f_)), active_(std::exchange(o.active_, false)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() { if (active_) f_(); }
    void dismiss() noexcept { active_ = false; }
private:
    F f_;
    bool active_ = true;
};
template <class F> ScopeExit(F) -> ScopeExit<F>;

// A mutex wrapper that owns the data it protects: you cannot touch T without a lock.
template <class T, class Mutex = std::mutex>
class Guarded {
public:
    class Access {
    public:
        Access(T& v, Mutex& m) : lock_(m), v_(&v) {}
        T* operator->() noexcept { return v_; }
        T& operator*() noexcept { return *v_; }
    private:
        std::unique_lock<Mutex> lock_;
        T* v_;
    };
    template <class... A> explicit Guarded(A&&... a) : v_(std::forward<A>(a)...) {}
    Access lock() { return Access(v_, m_); }
    template <class F> decltype(auto) with(F&& f) { std::lock_guard g(m_); return std::forward<F>(f)(v_); }
private:
    T v_;
    Mutex m_;
};

}  // namespace raii
