// Experiment 1: the same four kernels at every optimisation level
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <vector>

[[gnu::noinline]] float  sum_f (const float* p, int n)  { float s = 0;  for (int i = 0; i < n; ++i) s += p[i]; return s; }
[[gnu::noinline]] int    sum_i (const int* p, int n)    { int s = 0;    for (int i = 0; i < n; ++i) s += p[i]; return s; }
[[gnu::noinline]] void   saxpy (float* y, const float* x, float a, int n) { for (int i = 0; i < n; ++i) y[i] += a * x[i]; }
[[gnu::noinline]] long   count_if_even(const int* p, int n) { long c = 0; for (int i = 0; i < n; ++i) if ((p[i] & 1) == 0) ++c; return c; }

template <class F> double best_ns(F f, int reps) {
    double best = 1e30;
    for (int r = 0; r < 9; ++r) {
        auto t = std::chrono::steady_clock::now();
        for (int i = 0; i < reps; ++i) f();
        best = std::min(best, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t).count() / reps);
    }
    return best;
}
int main() {
    const int n = 1 << 12;                                   // 4096 elements: 16 KiB, stays in L1
    std::vector<float> xf(n, 1.5f), yf(n, 0.f);
    std::vector<int>   xi(n, 3);
    int reps = 5000;
    float sf = 0; long ci = 0; int si = 0;
    double a = best_ns([&] { asm volatile("" ::"r"(xf.data()) : "memory"); sf += sum_f(xf.data(), n); }, reps);
    double b = best_ns([&] { asm volatile("" ::"r"(xi.data()) : "memory"); si += sum_i(xi.data(), n); }, reps);
    double c = best_ns([&] { asm volatile("" ::"r"(xf.data()) : "memory"); saxpy(yf.data(), xf.data(), 0.5f, n); }, reps);
    double d = best_ns([&] { asm volatile("" ::"r"(xi.data()) : "memory"); ci += count_if_even(xi.data(), n); }, reps);
    std::printf("sum_f %7.0f ns  sum_i %7.0f ns  saxpy %7.0f ns  count_even %7.0f ns   (n=%d)   [%g %d %ld %g]\n", a, b, c, d, n, sf, si, ci, yf[0]);
}
