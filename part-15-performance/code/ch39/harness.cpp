// A 40-line benchmark harness: interleaved A/B, min and median of N, no framework
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <vector>

template <class T> inline void escape(T& v) { asm volatile("" : "+r,m"(v) : : "memory"); }
inline void clobber() { asm volatile("" : : : "memory"); }

using clk = std::chrono::steady_clock;
template <class F> double time_ns(F&& f, int reps) {
    auto t0 = clk::now();
    for (int i = 0; i < reps; ++i) f();
    return std::chrono::duration<double, std::nano>(clk::now() - t0).count() / reps;
}

__attribute__((noinline)) long sum_loop(const std::vector<int>& v) { long s = 0; for (int x : v) s += x; return s; }
__attribute__((noinline)) long sum_acc (const std::vector<int>& v) { return std::accumulate(v.begin(), v.end(), 0L); }

int main() {
    std::vector<int> v(10'000, 3);
    constexpr int N = 21, REPS = 2000;
    std::vector<double> a, b;
    for (int round = 0; round < N; ++round) {                 // interleave: drift hits both equally
        a.push_back(time_ns([&] { long r = sum_loop(v); escape(r); }, REPS));
        b.push_back(time_ns([&] { long r = sum_acc (v); escape(r); }, REPS));
    }
    auto report = [](const char* name, std::vector<double>& x) {
        std::sort(x.begin(), x.end());
        std::printf("%-10s min %.0f ns   median %.0f ns   max %.0f ns   (spread %.1f%%)\n",
                    name, x.front(), x[x.size() / 2], x.back(), 100.0 * (x.back() - x.front()) / x.front());
    };
    report("loop", a); report("accumulate", b);
}
