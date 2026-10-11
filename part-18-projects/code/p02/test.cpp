#include "vector.hpp"
#include <cstdlib>
#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>
#include <cassert>

#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; std::exit(1); } } while (0)

// Instrumented type: counts live objects, can throw on the Nth copy, optional noexcept move.
struct Probe {
    static inline int live = 0, copies = 0, moves = 0, throw_at = -1;
    int v;
    Probe(int x = 0) : v(x) { ++live; }
    Probe(const Probe& o) : v(o.v) {
        if (throw_at >= 0 && copies++ == throw_at) throw std::runtime_error("copy");
        ++live;
    }
    Probe(Probe&& o) noexcept : v(o.v) { ++moves; ++live; }
    Probe& operator=(const Probe&) = default;
    ~Probe() { --live; }
};
struct ThrowingMove {                      // NOT noexcept move: vector must copy on growth
    static inline int live = 0, copies = 0, moves = 0;
    int v;
    ThrowingMove(int x = 0) : v(x) { ++live; }
    ThrowingMove(const ThrowingMove& o) : v(o.v) { ++copies; ++live; }
    ThrowingMove(ThrowingMove&& o) : v(o.v) { ++moves; ++live; }
    ~ThrowingMove() { --live; }
};

struct ThrowingMove2 {                     // copy can fail, move is not noexcept -> growth must copy
    static inline int live = 0, copies = 0, throw_at = -1;
    int v;
    ThrowingMove2(int x = 0) : v(x) { ++live; }
    ThrowingMove2(const ThrowingMove2& o) : v(o.v) { if (throw_at >= 0 && copies++ == throw_at) throw std::runtime_error("copy"); ++live; }
    ThrowingMove2(ThrowingMove2&& o) : v(o.v) { ++live; }
    ~ThrowingMove2() { --live; }
};

int main() {
    {   // basic API, growth is geometric
        edu::Vector<int> v;
        std::size_t last = 0; int reallocs = 0;
        for (int i = 0; i < 1000; ++i) { v.push_back(i); if (v.capacity() != last) { ++reallocs; last = v.capacity(); } }
        CHECK(v.size() == 1000 && v[999] == 999);
        CHECK(reallocs == 11);                       // 1,2,4,...,1024
    }
    {   // no leaks, moves in growth
        { edu::Vector<Probe> v; for (int i = 0; i < 100; ++i) v.emplace_back(i);
          CHECK(Probe::live == 100); CHECK(Probe::copies == 0); CHECK(Probe::moves > 0); }
        CHECK(Probe::live == 0);
    }
    {   // move_if_noexcept: a throwing move constructor forces COPY on growth
        { edu::Vector<ThrowingMove> v; for (int i = 0; i < 8; ++i) v.emplace_back(i);
          CHECK(ThrowingMove::copies > 0); CHECK(ThrowingMove::moves == 0); }
        CHECK(ThrowingMove::live == 0);
    }
    {   // aliasing: push_back of an element of the same vector, during reallocation
        edu::Vector<std::string> v;
        v.push_back("a-sufficiently-long-string-to-defeat-SSO-0123456789");
        for (int i = 0; i < 20; ++i) v.push_back(v[0]);
        CHECK(v.size() == 21 && v[20] == v[0]);
    }
    {   // strong guarantee: copy throws during copy-construction of the Vector; nothing leaks
        edu::Vector<Probe> a; for (int i = 0; i < 10; ++i) a.emplace_back(i);
        Probe::copies = 0; Probe::throw_at = 5;
        try { edu::Vector<Probe> b(a); CHECK(false); } catch (const std::runtime_error&) {}
        Probe::throw_at = -1;
        CHECK(Probe::live == 10);
    }
    {   // strong guarantee on growth: a copy throws mid-reallocation; vector is unchanged, nothing leaks
        { edu::Vector<ThrowingMove2> v;
          for (int i = 0; i < 4; ++i) v.emplace_back(i);       // capacity 4, full
          ThrowingMove2::copies = 0; ThrowingMove2::throw_at = 2;
          try { v.emplace_back(99); CHECK(false); } catch (const std::runtime_error&) {}
          ThrowingMove2::throw_at = -1;
          CHECK(v.size() == 4 && v.capacity() == 4);
          for (int i = 0; i < 4; ++i) CHECK(v[i].v == i);
          CHECK(ThrowingMove2::live == 4); }
        CHECK(ThrowingMove2::live == 0);
    }
    {   // copy / move assignment, self-assignment
        edu::Vector<std::string> a{"x", "y", "z"}, b;
        b = a; CHECK(b.size() == 3 && b[2] == "z");
        b = b; CHECK(b.size() == 3);
        edu::Vector<std::string> c = std::move(b);
        CHECK(b.size() == 0 && c.size() == 3);
        static_assert(std::is_nothrow_move_constructible_v<edu::Vector<int>>);
    }
    {   // allocator awareness: pmr with a monotonic buffer, element allocation propagates
        char buf[4096];
        std::pmr::monotonic_buffer_resource mr(buf, sizeof buf);
        edu::Vector<int, std::pmr::polymorphic_allocator<int>> v{std::pmr::polymorphic_allocator<int>(&mr)};
        for (int i = 0; i < 100; ++i) v.push_back(i);
        CHECK(v.get_allocator().resource() == &mr);
        CHECK(reinterpret_cast<char*>(v.data()) >= buf && reinterpret_cast<char*>(v.data()) < buf + sizeof buf);
    }
    std::cout << "all tests passed\n";
}
