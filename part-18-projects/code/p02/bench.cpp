#include "vector.hpp"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
template <class F> double best_ms(F f) {
    double best = 1e9;
    for (int r = 0; r < 9; ++r) {
        auto t0 = std::chrono::steady_clock::now(); f();
        auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}
template <class V> long run_int() { V v; for (int i = 0; i < 4'000'000; ++i) v.push_back(i); long s = 0; for (int x : v) s += x; return s; }
template <class V> std::size_t run_str() { V v; for (int i = 0; i < 500'000; ++i) v.push_back(std::string("a string long enough to allocate on the heap, number ") + std::to_string(i)); return v.size(); }
int main() {
    volatile long sink = 0;
    std::printf("push_back 4M int      std::vector %7.2f ms   edu::Vector %7.2f ms\n",
                best_ms([&] { sink = run_int<std::vector<int>>(); }), best_ms([&] { sink = run_int<edu::Vector<int>>(); }));
    std::printf("push_back 500k string std::vector %7.2f ms   edu::Vector %7.2f ms\n",
                best_ms([&] { sink = run_str<std::vector<std::string>>(); }), best_ms([&] { sink = run_str<edu::Vector<std::string>>(); }));
}
