// Structured error handling for the runtime: std::expected<T, std::error_code> with our own category.
#pragma once
#include <expected>
#include <string>
#include <system_error>

namespace aio {

enum class Errc {
    ok = 0,
    timeout = 1,        // an operation exceeded its deadline
    closed = 2,         // peer closed the connection
    parse_error = 3,    // malformed protocol input
    too_large = 4,      // a protocol limit was exceeded
    shutting_down = 5,  // runtime or server is stopping
};

class ErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "aio"; }
    std::string message(int c) const override {
        switch (static_cast<Errc>(c)) {
            case Errc::ok: return "ok";
            case Errc::timeout: return "operation timed out";
            case Errc::closed: return "connection closed";
            case Errc::parse_error: return "malformed input";
            case Errc::too_large: return "limit exceeded";
            case Errc::shutting_down: return "shutting down";
        }
        return "unknown aio error";
    }
};
inline const std::error_category& aio_category() noexcept { static const ErrorCategory c; return c; }
inline std::error_code make_error_code(Errc e) noexcept { return {static_cast<int>(e), aio_category()}; }

template <class T> using Result = std::expected<T, std::error_code>;
inline std::unexpected<std::error_code> fail(Errc e) { return std::unexpected(make_error_code(e)); }
inline std::unexpected<std::error_code> fail_errno(int e) { return std::unexpected(std::error_code(e, std::generic_category())); }

}  // namespace aio

template <> struct std::is_error_code_enum<aio::Errc> : std::true_type {};
