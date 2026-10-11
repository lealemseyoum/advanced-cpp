#include "task.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
static long g_allocs = 0;
void* operator new(std::size_t n) { ++g_allocs; if (void* p = std::malloc(n)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
using edu::Task;
__attribute__((noinline)) int plain(int x) { asm volatile("" : "+r"(x)); return x + 1; }
Task<int> leaf(int x) { asm volatile("" : "+r"(x)); co_return x + 1; }
Task<long> plain_loop(int n) { long s = 0; for (int i = 0; i < n; ++i) s += plain(i); co_return s; }
Task<long> task_loop(int n)  { long s = 0; for (int i = 0; i < n; ++i) s += co_await leaf(i); co_return s; }
int main() {
    const int N = 10'000'000;
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now(); long a = edu::sync_wait(plain_loop(N)); auto t1 = clk::now();
    long allocs0 = g_allocs;
    long b = edu::sync_wait(task_loop(N)); auto t2 = clk::now();
    double p = std::chrono::duration<double, std::nano>(t1 - t0).count() / N, q = std::chrono::duration<double, std::nano>(t2 - t1).count() / N;
    std::printf("plain call: %5.2f ns/iter    co_await Task: %5.2f ns/iter    frame allocations for the awaited loop: %ld of %d  (%ld %ld)\n", p, q, g_allocs - allocs0, N, a, b);
}
