// What happens when a C++ exception unwinds through C library frames?
#include <sqlite3.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
int main(int argc, char** argv) {
    int which = argc > 1 ? std::atoi(argv[1]) : 0;
    if (which == 0) {                                   // glibc qsort calling a throwing comparator
        int v[] = {3, 1, 2};
        try {
            std::qsort(v, 3, sizeof(int), [](const void*, const void*) -> int { throw std::runtime_error("cmp"); });
        } catch (const std::exception& e) { std::printf("qsort: caught '%s'\n", e.what()); }
        return 0;
    }
    sqlite3* db; sqlite3_open(":memory:", &db);
    sqlite3_exec(db, "CREATE TABLE t(a); INSERT INTO t VALUES(1);", nullptr, nullptr, nullptr);
    auto cb = [](void* user, int, char**, char**) -> int {
        if (user) throw std::runtime_error("cb");        // which == 1: throw through sqlite3_exec
        return 1;                                         // which == 2: the control: abort the query politely
    };
    try {
        sqlite3_exec(db, "SELECT a FROM t", cb, which == 1 ? (void*)1 : nullptr, nullptr);
    } catch (const std::exception& e) { std::printf("sqlite: caught '%s'\n", e.what()); }
    std::printf("sqlite3_close -> %d (0 = OK, 5 = SQLITE_BUSY)\n", sqlite3_close(db));
}
