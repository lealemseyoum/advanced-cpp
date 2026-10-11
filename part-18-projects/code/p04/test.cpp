#include "function.hpp"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; std::exit(1); } } while (0)
static int g_allocs = 0;
void* operator new(std::size_t n) { ++g_allocs; if (void* p = std::malloc(n)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

struct Probe {
    static inline int live = 0, copies = 0;
    int v;
    Probe(int x) : v(x) { ++live; }
    Probe(Probe&& o) noexcept : v(o.v) { ++live; }
    ~Probe() { --live; }
    int operator()(int x) const { return v + x; }
};
struct Big { char pad[200]; int v; int operator()(int x) const { return v * x; } };
struct ThrowsOnMove { ThrowsOnMove() = default; ThrowsOnMove(ThrowsOnMove&&) {} int operator()() const { return 1; } };
int free_fn(int x) { return x * 2; }
struct M { int k = 5; int get(int x) const { return k + x; } };

int main() {
    {   // free function, lambda, member pointer (through std::invoke), functor
        edu::Function<int(int)> f = free_fn;       CHECK(f(4) == 8);
        int k = 10; edu::Function<int(int)> g = [k](int x) { return x + k; };  CHECK(g(1) == 11);
        edu::Function<int(const M&, int)> h = &M::get; CHECK(h(M{}, 1) == 6);
    }
    {   // small buffer: small callables do not allocate; big ones do exactly once
        int before = g_allocs;
        int a = 1, b = 2;
        edu::Function<int(int)> f = [a, b](int x) { return a + b + x; };
        CHECK(f.is_inline() && g_allocs == before);
        edu::Function<int(int)> big = Big{{}, 3};
        CHECK(!big.is_inline() && g_allocs == before + 1 && big(7) == 21);
    }
    {   // lifetime: destroyed exactly once, through moves, reassignment and reset
        { edu::Function<int(int)> f = Probe(1); CHECK(Probe::live == 1);
          edu::Function<int(int)> g = std::move(f); CHECK(!f && Probe::live == 1 && g(1) == 2);
          edu::Function<int(int)> h; h = std::move(g); CHECK(h(2) == 3);
          h = nullptr; CHECK(Probe::live == 0); }
        CHECK(Probe::live == 0);
    }
    {   // move-only callables (std::function cannot hold these)
        auto p = std::make_unique<int>(41);
        edu::Function<int()> f = [p = std::move(p)] { return *p + 1; };
        CHECK(f() == 42);
        edu::Function<int()> g = std::move(f);
        CHECK(g() == 42);
    }
    {   // a throwing move constructor forces heap storage (so Function's own move can be noexcept)
        edu::Function<int()> f = ThrowsOnMove{};
        CHECK(!f.is_inline());
        static_assert(std::is_nothrow_move_constructible_v<edu::Function<int()>>);
    }
    {   // empty call throws
        edu::Function<void()> f;
        bool threw = false; try { f(); } catch (const std::bad_function_call&) { threw = true; }
        CHECK(threw && !f);
    }
    {   // storing in containers (this needs the noexcept move)
        std::vector<edu::Function<int()>> v;
        for (int i = 0; i < 100; ++i) v.emplace_back([i] { return i; });
        int s = 0; for (auto& f : v) s += f();
        CHECK(s == 4950);
    }
    std::cout << "sizeof(edu::Function<int(int)>) = " << sizeof(edu::Function<int(int)>)
              << ",  sizeof(std::function<int(int)>) = " << sizeof(std::function<int(int)>) << "\n";
    std::cout << "all tests passed\n";
}
