#include "small_vector.hpp"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; std::exit(1); } } while (0)

// count global allocations to prove "no allocation while inline"
static int g_allocs = 0;
void* operator new(std::size_t n) { ++g_allocs; if (void* p = std::malloc(n)) return p; throw std::bad_alloc(); }
void* operator new(std::size_t n, std::align_val_t a) { ++g_allocs; if (void* p = std::aligned_alloc(static_cast<std::size_t>(a), (n + static_cast<std::size_t>(a) - 1) / static_cast<std::size_t>(a) * static_cast<std::size_t>(a))) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

struct Probe {
    static inline int live = 0;
    int v;
    Probe(int x = 0) : v(x) { ++live; }
    Probe(const Probe& o) : v(o.v) { ++live; }
    Probe(Probe&& o) noexcept : v(o.v) { ++live; }
    ~Probe() { --live; }
};
struct alignas(64) Wide { int x = 7; };

int main() {
    {   // inline until N, then one heap allocation
        const int before = g_allocs;
        edu::SmallVector<int, 4> v;
        for (int i = 0; i < 4; ++i) v.push_back(i);
        CHECK(v.is_inline() && g_allocs == before);
        v.push_back(4);
        CHECK(!v.is_inline() && g_allocs == before + 1 && v.size() == 5 && v[0] == 0 && v[4] == 4);
    }
    {   // lifetime: every constructed element destroyed once, inline or heap
        { edu::SmallVector<Probe, 3> v; for (int i = 0; i < 2; ++i) v.emplace_back(i); CHECK(Probe::live == 2); }
        CHECK(Probe::live == 0);
        { edu::SmallVector<Probe, 3> v; for (int i = 0; i < 20; ++i) v.emplace_back(i); CHECK(Probe::live == 20); }
        CHECK(Probe::live == 0);
    }
    {   // moves: heap buffer is stolen (no element moves), inline elements are moved one by one
        edu::SmallVector<std::string, 2> a;
        for (int i = 0; i < 5; ++i) a.push_back(std::string(40, char('a' + i)));
        const std::string* p = a.data();
        edu::SmallVector<std::string, 2> b = std::move(a);
        CHECK(b.data() == p && a.size() == 0 && a.is_inline());     // pointer stolen: references stay valid
        edu::SmallVector<std::string, 2> c; c.push_back("x");
        const std::string* cp = c.data();
        edu::SmallVector<std::string, 2> d = std::move(c);
        CHECK(d.data() != cp && d[0] == "x" && c.size() == 0);      // inline: addresses change (!)
    }
    {   // assignment between all four inline/heap combinations
        for (int na : {1, 6}) for (int nb : {1, 6}) {
            edu::SmallVector<std::string, 2> a, b;
            for (int i = 0; i < na; ++i) a.push_back("a" + std::to_string(i));
            for (int i = 0; i < nb; ++i) b.push_back("b" + std::to_string(i));
            a = std::move(b);
            CHECK(a.size() == std::size_t(nb) && a[0] == "b0");
            a = a.size() ? edu::SmallVector<std::string, 2>(a) : edu::SmallVector<std::string, 2>();
        }
    }
    {   // aliasing during growth
        edu::SmallVector<std::string, 2> v;
        v.push_back(std::string(50, 'q')); v.push_back(std::string(50, 'r'));
        v.push_back(v[0]);                                           // full -> grows while copying v[0]
        CHECK(v.size() == 3 && v[2] == v[0]);
    }
    {   // over-aligned element type: both buffers honour alignof(T)
        edu::SmallVector<Wide, 2> v;
        for (int i = 0; i < 6; ++i) { v.emplace_back(); CHECK(reinterpret_cast<std::uintptr_t>(v.data()) % 64 == 0); }
    }
    std::cout << "sizeof(SmallVector<int,4>)  = " << sizeof(edu::SmallVector<int, 4>)
              << "\nsizeof(SmallVector<int,16>) = " << sizeof(edu::SmallVector<int, 16>)
              << "\nsizeof(std::vector<int>)    = " << sizeof(std::vector<int>) << "\n";
    std::cout << "all tests passed\n";
}
