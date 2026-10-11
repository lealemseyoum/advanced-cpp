#include "task.hpp"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; std::exit(1); } } while (0)
using edu::Task;

// Symmetric transfer only bounds the stack if the compiler turns `return handle;` into a tail call.
// Measured here: Clang 18 does so at every -O level; GCC 13.3/14.2 only at -O2 and above, and not under ASan/TSan.
#if defined(__clang__)
constexpr bool kTailCalls = true;
#elif defined(__OPTIMIZE__) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
constexpr bool kTailCalls = true;
#else
constexpr bool kTailCalls = false;
#endif
constexpr int kLoop  = kTailCalls ? 1'000'000 : 1'000;
constexpr int kDepth = kTailCalls ? 100'000   : 1'000;

static int g_frames = 0;                       // counts coroutine frame allocations/frees
void* operator new(std::size_t n) { ++g_frames; if (void* p = std::malloc(n)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { if (p) { --g_frames; std::free(p); } }
void operator delete(void* p, std::size_t) noexcept { if (p) { --g_frames; std::free(p); } }

struct Probe { static inline int live = 0; Probe() { ++live; } Probe(Probe&&) noexcept { ++live; } ~Probe() { --live; } };

Task<int> answer() { co_return 42; }
Task<int> add(int a, int b) { co_return co_await answer() + a + b - 42; }
Task<std::string> greet(std::string n) { co_return "hello " + n; }
Task<int> thrower() { throw std::runtime_error("boom"); co_return 0; }
Task<void> nothing(bool& ran) { ran = true; co_return; }
Task<int> depth(int n) { if (n == 0) co_return 0; co_return 1 + co_await depth(n - 1); }
Task<long> loop(int n) { long s = 0; for (int i = 0; i < n; ++i) s += co_await answer(); co_return s; }
Task<Probe> make_probe() { co_return Probe{}; }
Task<std::unique_ptr<int>> boxed() { co_return std::make_unique<int>(7); }

int main() {
    CHECK(edu::sync_wait(answer()) == 42);
    CHECK(edu::sync_wait(add(1, 2)) == 3);
    CHECK(edu::sync_wait(greet("coroutines")) == "hello coroutines");
    CHECK(*edu::sync_wait(boxed()) == 7);                                    // move-only result

    {   // lazy: creating a task runs nothing
        bool ran = false;
        { Task<void> t = nothing(ran); CHECK(!ran); }                       // destroyed without ever starting: frame freed, body never ran
        CHECK(!ran);
        edu::sync_wait(nothing(ran)); CHECK(ran);
    }
    {   // exception propagation through co_await
        bool caught = false;
        try { (void)edu::sync_wait(thrower()); } catch (const std::runtime_error& e) { caught = std::string(e.what()) == "boom"; }
        CHECK(caught);
    }
    {   // frame lifetime: every frame allocated is freed, nothing leaks (also checks with ASan)
        const int before = g_frames;
        for (int i = 0; i < 1000; ++i) (void)edu::sync_wait(add(i, 1));
        CHECK(g_frames == before);
        { Task<Probe> t = make_probe(); }                                    // never started
        CHECK(Probe::live == 0);
        (void)edu::sync_wait(make_probe());
        CHECK(Probe::live == 0);
    }
    {   // symmetric transfer: sequential co_awaits in a loop use constant stack (when tail calls happen)
        CHECK(edu::sync_wait(loop(kLoop)) == 42L * kLoop);
    }
    {   // deep recursion of nested tasks completes without stack overflow (each frame is on the heap)
        CHECK(edu::sync_wait(depth(kDepth)) == kDepth);
    }
    {   // a Task can be moved, and completes on whichever thread resumes it
        Task<int> t = add(2, 3);
        Task<int> u = std::move(t);
        CHECK(!t.handle());
        int r = 0;
        std::thread th([&] { r = edu::sync_wait(std::move(u)); });
        th.join();
        CHECK(r == 5);
    }
    std::cout << "frames allocated and freed: balanced\ndeep tests: " << (kTailCalls ? "full (1M awaits, depth 100k)" : "reduced (no guaranteed tail calls in this build)") << "\nall tests passed\n";
}
