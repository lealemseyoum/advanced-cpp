#include "function.hpp"
#include <chrono>
#include <cstdio>
#include <functional>
#include <vector>
template <class F> double best_ms(F f) { double b = 1e9; for (int r = 0; r < 9; ++r) { auto t0 = std::chrono::steady_clock::now(); f(); auto t1 = std::chrono::steady_clock::now(); b = std::min(b, std::chrono::duration<double, std::milli>(t1 - t0).count()); } return b; }
template <class Fn> long call_loop(Fn& f, int n) { long s = 0; for (int i = 0; i < n; ++i) { s += f(i); asm volatile("" : "+r"(s)); } return s; }
template <class Fn> long create_loop(int n) { long s = 0; int a = 1, b = 2; for (int i = 0; i < n; ++i) { Fn f = [a, b, i](int x) { return a + b + i + x; }; s += f(1); asm volatile("" : "+r"(s) : "r"(&f) : "memory"); } return s; }
template <class Fn> long create_big(int n) { long s = 0; long pad[8] = {1,2,3,4,5,6,7,8}; for (int i = 0; i < n; ++i) { Fn f = [pad, i](int x) { return pad[x & 7] + i; }; s += f(1); asm volatile("" : "+r"(s) : "r"(&f) : "memory"); } return s; }
int main() {
    volatile long sink = 0;
    const int N = 100'000'000;
    int k = 3;
    edu::Function<int(int)> ef = [k](int x) { return x + k; };
    std::function<int(int)> sf = [k](int x) { return x + k; };
    std::printf("call  %dM:  edu::Function %7.1f ms   std::function %7.1f ms\n", N / 1'000'000,
        best_ms([&] { sink = call_loop(ef, N); }), best_ms([&] { sink = call_loop(sf, N); }));
    const int M = 20'000'000;
    std::printf("create+call+destroy, 12-byte capture %dM:  edu %7.1f ms   std %7.1f ms\n", M / 1'000'000,
        best_ms([&] { sink = create_loop<edu::Function<int(int)>>(M); }), best_ms([&] { sink = create_loop<std::function<int(int)>>(M); }));
    std::printf("create+call+destroy, 72-byte capture %dM:  edu %7.1f ms   std %7.1f ms\n", M / 1'000'000,
        best_ms([&] { sink = create_big<edu::Function<int(int)>>(M); }), best_ms([&] { sink = create_big<std::function<int(int)>>(M); }));
}
