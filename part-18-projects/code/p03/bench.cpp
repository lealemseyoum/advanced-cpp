#include "small_vector.hpp"
#include <chrono>
#include <cstdio>
#include <vector>
template <class F> double best_ms(F f) { double b = 1e9; for (int r = 0; r < 9; ++r) { auto t0 = std::chrono::steady_clock::now(); f(); auto t1 = std::chrono::steady_clock::now(); b = std::min(b, std::chrono::duration<double, std::milli>(t1 - t0).count()); } return b; }
template <class V> long work(int n) { long s = 0; for (int i = 0; i < n; ++i) { V v; for (int k = 0; k < 6; ++k) v.push_back(i + k); for (int x : v) s += x; asm volatile("" : "+r"(s) : "r"(v.data()) : "memory"); } return s; }
int main() {
    volatile long sink = 0;
    std::printf("2M short lists (6 ints)  std::vector %7.2f ms   SmallVector<int,8> %7.2f ms   SmallVector<int,4> (spills) %7.2f ms\n",
        best_ms([&] { sink = work<std::vector<int>>(2'000'000); }),
        best_ms([&] { sink = work<edu::SmallVector<int, 8>>(2'000'000); }),
        best_ms([&] { sink = work<edu::SmallVector<int, 4>>(2'000'000); }));
}
