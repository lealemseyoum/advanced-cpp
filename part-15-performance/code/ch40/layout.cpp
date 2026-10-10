// Same loop, two layouts. The loop only reads `x`; each particle carries 56 bytes of other fields.
#include <cstdio>
#include <vector>
struct P { double x, y, z, vx, vy, vz, mass, charge; };            // 64 bytes: one cache line per particle
struct SoA { std::vector<double> x, y, z, vx, vy, vz, mass, charge; };

[[gnu::noinline]] double sum_aos(const std::vector<P>& v) { double s = 0; for (auto& p : v) s += p.x; return s; }
[[gnu::noinline]] double sum_soa(const SoA& v)            { double s = 0; for (double x : v.x) s += x;  return s; }

int main() {
    const int n = 1 << 20;                                            // 1M particles: 64 MiB as AoS, 8 MiB for x alone
    std::vector<P> a(n, P{1, 2, 3, 4, 5, 6, 7, 8});
    SoA b; b.x.assign(n, 1.0);
    std::printf("%.0f %.0f\n", sum_aos(a), sum_soa(b));
}
