#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>
struct P { double x, y, z, vx, vy, vz, mass, charge; };
[[gnu::noinline]] double sum_aos(const std::vector<P>& v) { double s = 0; for (auto& p : v) s += p.x; return s; }
[[gnu::noinline]] double sum_soa(const std::vector<double>& x) { double s = 0; for (double d : x) s += d; return s; }
template <class F> double best_ms(F f) { double best = 1e9; for (int i = 0; i < 15; ++i) { auto t = std::chrono::steady_clock::now(); volatile double r = f(); (void)r; best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count()); } return best; }
int main() {
    for (int n : {1 << 10, 1 << 14, 1 << 18, 1 << 22}) {
        std::vector<P> a(n, P{1, 2, 3, 4, 5, 6, 7, 8});
        std::vector<double> x(n, 1.0);
        int reps = (1 << 24) / n;
        auto A = best_ms([&] { double s = 0; for (int r = 0; r < reps; ++r) { asm volatile("" : : "r"(a.data()) : "memory"); s += sum_aos(a); } return s; });
        auto S = best_ms([&] { double s = 0; for (int r = 0; r < reps; ++r) { asm volatile("" : : "r"(x.data()) : "memory"); s += sum_soa(x); } return s; });
        std::printf("n=%8d  AoS %.2f ms  SoA %.2f ms  ratio %.1fx   (same %d M element reads)\n", n, A, S, A / S, (reps * n) >> 20);
    }
}
