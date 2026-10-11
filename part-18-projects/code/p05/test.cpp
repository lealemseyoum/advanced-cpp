#include "arena.hpp"
#include <cstdlib>
#include <iostream>
#include <list>
#include <map>
#include <string>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; std::exit(1); } } while (0)

struct Counting final : std::pmr::memory_resource {            // upstream spy
    std::size_t allocs = 0, deallocs = 0, bytes = 0;
    void* do_allocate(std::size_t n, std::size_t a) override { ++allocs; bytes += n; return std::pmr::new_delete_resource()->allocate(n, a); }
    void do_deallocate(void* p, std::size_t n, std::size_t a) noexcept override { ++deallocs; std::pmr::new_delete_resource()->deallocate(p, n, a); }
    bool do_is_equal(const memory_resource& o) const noexcept override { return this == &o; }
};
struct Probe { static inline int live = 0; int v; Probe(int x) : v(x) { ++live; } ~Probe() { --live; } };

int main() {
    {   // alignment of every allocation, mixed sizes
        edu::Arena a(256);
        for (std::size_t al : {1u, 2u, 4u, 8u, 16u, 32u, 64u}) for (std::size_t n : {1u, 3u, 17u, 100u}) {
            void* p = a.allocate(n, al);
            CHECK(reinterpret_cast<std::uintptr_t>(p) % al == 0);
        }
    }
    {   // chunking: big request gets its own chunk; release returns everything upstream
        Counting up;
        { edu::Arena a(1024, &up);
          (void)a.allocate(100, 8); (void)a.allocate(100, 8);
          CHECK(up.allocs == 1);
          (void)a.allocate(5000, 8);                                    // bigger than a chunk
          CHECK(up.allocs == 2 && a.chunks() == 2); }
        CHECK(up.allocs == up.deallocs);                           // destructor released all chunks
    }
    {   // reset reuses first chunk
        Counting up; edu::Arena a(1024, &up);
        for (int r = 0; r < 100; ++r) { for (int i = 0; i < 10; ++i) (void)a.allocate(64, 8); a.reset(); }
        CHECK(up.allocs == 1);
    }
    {   // PMR containers on an arena; destructors run by the container, memory freed in bulk
        Counting up;
        { edu::Arena a(4096, &up);
          std::pmr::vector<std::pmr::string> v(&a);
          for (int i = 0; i < 1000; ++i) v.emplace_back("a string that is definitely longer than the SSO buffer " + std::to_string(i));
          std::pmr::map<int, std::pmr::vector<int>> m(&a);
          for (int i = 0; i < 100; ++i) m[i].assign(10, i);
          CHECK(v.size() == 1000 && m.size() == 100 && v[999].back() == '9'); }
        CHECK(up.allocs == up.deallocs && up.allocs > 1);
    }
    {   // Pool<T>: O(1) create/destroy, reuse of freed blocks, proper lifetime
        edu::Pool<Probe> p(8);
        Probe* a = p.create(1); Probe* b = p.create(2);
        CHECK(Probe::live == 2 && p.live() == 2);
        p.destroy(a);
        Probe* c = p.create(3);
        CHECK(c == a);                                             // freed block reused first (LIFO free list)
        p.destroy(b); p.destroy(c);
        CHECK(Probe::live == 0 && p.live() == 0);
        std::vector<Probe*> many;
        for (int i = 0; i < 100; ++i) many.push_back(p.create(i));  // grows by chunks
        for (auto* x : many) p.destroy(x);
        CHECK(Probe::live == 0);
    }
    {   // PoolResource: node containers recycle nodes without touching upstream
        Counting up;
        { edu::PoolResource pr(&up);
          std::pmr::list<int> l(&pr);
          for (int r = 0; r < 100; ++r) { for (int i = 0; i < 1000; ++i) l.push_back(i); l.clear(); }
          CHECK(up.allocs <= 6);                                   // arena chunks only, not 100'000 node allocations
          void* big = pr.allocate(10000, 8); pr.deallocate(big, 10000, 8);
          CHECK(pr.upstream_allocations() == 1); }
        CHECK(up.allocs == up.deallocs);
    }
    std::cout << "all tests passed\n";
}
