// Experiment 3/4: the same code on different data; working-set sweep
#include <benchmark/benchmark.h>
#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

static std::vector<int> make(size_t n, bool sorted) {
    std::mt19937 rng(42);
    std::vector<int> v(n);
    for (auto& x : v) x = static_cast<int>(rng() % 256);
    if (sorted) std::sort(v.begin(), v.end());
    return v;
}

// Branchy sum: add only elements >= 128. Same instructions, different predictability.
__attribute__((noinline)) long branchy(const int* p, size_t n) {
    long s = 0;
    for (size_t i = 0; i < n; ++i) if (p[i] >= 128) s += p[i];
    return s;
}

static void BM_Branchy(benchmark::State& st) {
    auto v = make(1 << 16, st.range(0));
    for (auto _ : st) benchmark::DoNotOptimize(branchy(v.data(), v.size()));
    st.SetItemsProcessed(st.iterations() * v.size());
}
BENCHMARK(BM_Branchy)->Arg(0)->Arg(1)->ArgName("sorted");

// Working-set sweep: random pointer-chase through n ints
static void BM_Chase(benchmark::State& st) {
    size_t n = st.range(0) / sizeof(size_t);
    std::vector<size_t> next(n), perm(n);
    std::iota(perm.begin(), perm.end(), 0);
    std::mt19937_64 rng(1);
    std::shuffle(perm.begin(), perm.end(), rng);
    for (size_t i = 0; i < n; ++i) next[perm[i]] = perm[(i + 1) % n];     // one big cycle
    size_t cur = 0;
    for (auto _ : st) {
        for (int k = 0; k < 1000; ++k) cur = next[cur];
        benchmark::DoNotOptimize(cur);
    }
    st.SetItemsProcessed(st.iterations() * 1000);
}
BENCHMARK(BM_Chase)->RangeMultiplier(8)->Range(4 << 10, 256 << 20)->ArgName("bytes");
BENCHMARK_MAIN();
