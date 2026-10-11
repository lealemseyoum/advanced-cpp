#include "sqlitepp.hpp"
#include <cstdio>
using namespace sqlitepp;

int main() {
    auto db = Db::open(":memory:");
    if (!db) { std::printf("open failed: %s\n", db.error().message.c_str()); return 1; }

    if (auto r = db->exec("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, score INTEGER);"); !r) return 2;
    for (auto [n, s] : {std::pair{"ada", 90}, {"bob", 72}, {"cy", 85}}) {
        auto st = Stmt::prepare(*db, "INSERT INTO t(name, score) VALUES(?1, ?2)");   // (a real wrapper would reset() and reuse)
        st->bind(1, n).bind(2, s);
        st->step();
    }
    std::puts("rows with score >= 80:");
    auto r = db->query("SELECT name, score FROM t WHERE score >= 80 ORDER BY score DESC",
                       [](const std::vector<std::string>& row) { std::printf("  %s = %s\n", row[0].c_str(), row[1].c_str()); return true; });
    if (!r) return 4;

    auto bad = db->exec("SELEKT nonsense");                 // an error from the C library becomes a value
    std::printf("bad sql -> code %d: %s\n", bad.error().code, bad.error().message.c_str());

    auto missing = Db::open("/nonexistent-dir/x.db");        // open failure
    std::printf("open missing -> code %d: %s\n", missing.error().code, missing.error().message.c_str());

    // an exception thrown by the user callback must not unwind through sqlite's C frames
    auto thrown = db->query("SELECT name FROM t", [](const std::vector<std::string>&) -> bool { throw std::runtime_error("boom"); });
    std::printf("callback threw -> %s (code %d)\n", thrown ? "ok" : "query aborted", thrown ? 0 : thrown.error().code);
}
