// A small RAII wrapper over the SQLite C API: handles, errors, callbacks, ownership
#pragma once
#include <sqlite3.h>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sqlitepp {

struct Error { int code; std::string message; };
template <class T> using Result = std::expected<T, Error>;

struct DbDeleter   { void operator()(sqlite3* db)      const noexcept { sqlite3_close_v2(db); } };
struct StmtDeleter { void operator()(sqlite3_stmt* s)  const noexcept { sqlite3_finalize(s); } };

class Db {
    std::unique_ptr<sqlite3, DbDeleter> db_;
    explicit Db(sqlite3* p) : db_(p) {}
public:
    [[nodiscard]] static Result<Db> open(const char* path) {
        sqlite3* raw = nullptr;
        int rc = sqlite3_open(path, &raw);              // raw is non-null even on error: it must still be closed
        Db db(raw);                                       // ownership taken BEFORE we look at rc
        if (rc != SQLITE_OK) return std::unexpected(Error{rc, raw ? sqlite3_errmsg(raw) : "out of memory"});
        return db;
    }

    [[nodiscard]] Result<void> exec(const char* sql) {
        char* msg = nullptr;
        int rc = sqlite3_exec(db_.get(), sql, nullptr, nullptr, &msg);
        if (rc == SQLITE_OK) return {};
        std::string m = msg ? msg : "";
        sqlite3_free(msg);                                // the library allocated it, the library frees it
        return std::unexpected(Error{rc, std::move(m)});
    }

    // Callback bridge: C wants `int (*)(void*, int, char**, char**)` and a void* context.
    // A captureless trampoline recovers the C++ callable from the context pointer.
    using RowFn = std::function<bool(const std::vector<std::string>&)>;     // return false to abort
    [[nodiscard]] Result<void> query(const char* sql, RowFn fn) {
        struct Ctx { RowFn* fn; };
        Ctx ctx{&fn};
        auto trampoline = +[](void* user, int ncol, char** vals, char**) noexcept -> int {
            auto* c = static_cast<Ctx*>(user);
            try {                                           // NO exception may cross the C frames of sqlite3_exec
                std::vector<std::string> row;
                for (int i = 0; i < ncol; ++i) row.emplace_back(vals[i] ? vals[i] : "NULL");
                return (*c->fn)(row) ? 0 : 1;               // non-zero aborts the query
            } catch (...) { return 1; }
        };
        char* msg = nullptr;
        int rc = sqlite3_exec(db_.get(), sql, trampoline, &ctx, &msg);
        if (rc == SQLITE_OK) return {};
        std::string m = msg ? msg : "";
        sqlite3_free(msg);
        return std::unexpected(Error{rc, std::move(m)});
    }

    sqlite3* raw() const noexcept { return db_.get(); }
};

class Stmt {
    std::unique_ptr<sqlite3_stmt, StmtDeleter> s_;
public:
    [[nodiscard]] static Result<Stmt> prepare(Db& db, std::string_view sql) {
        sqlite3_stmt* raw = nullptr;
        int rc = sqlite3_prepare_v2(db.raw(), sql.data(), static_cast<int>(sql.size()), &raw, nullptr);   // length-aware: no NUL needed
        Stmt st; st.s_.reset(raw);
        if (rc != SQLITE_OK) return std::unexpected(Error{rc, sqlite3_errmsg(db.raw())});
        return st;
    }
    Stmt& bind(int idx, int v)                { sqlite3_bind_int(s_.get(), idx, v); return *this; }
    Stmt& bind(int idx, std::string_view v)   { sqlite3_bind_text(s_.get(), idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT); return *this; }  // TRANSIENT: sqlite copies
    bool step() { return sqlite3_step(s_.get()) == SQLITE_ROW; }
    int          col_int(int i) const  { return sqlite3_column_int(s_.get(), i); }
    std::string  col_text(int i) const { auto* p = sqlite3_column_text(s_.get(), i); return p ? reinterpret_cast<const char*>(p) : ""; }
    Stmt() = default;
};

}  // namespace sqlitepp
