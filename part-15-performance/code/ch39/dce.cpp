// Experiment 1: dead-code elimination and constant folding in microbenchmarks
#include <benchmark/benchmark.h>
#include <numeric>
#include <vector>

static void BM_Naive(benchmark::State& st) {            // BROKEN: result unused, input constant
    std::vector<int> v(1000, 1);
    for (auto _ : st) {
        int s = std::accumulate(v.begin(), v.end(), 0);
        (void)s;
    }
}
BENCHMARK(BM_Naive);

static void BM_Escaped(benchmark::State& st) {           // result escapes, memory clobbered
    std::vector<int> v(1000, 1);
    for (auto _ : st) {
        benchmark::DoNotOptimize(v.data());              // compiler must assume v changed
        int s = std::accumulate(v.begin(), v.end(), 0);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_Escaped);

static void BM_Constant(benchmark::State& st) {          // folded: answer known at compile time
    for (auto _ : st) {
        int s = 0;
        for (int i = 0; i < 1000; ++i) s += i;
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_Constant);

static void BM_Runtime(benchmark::State& st) {           // size is a runtime value: really loops
    int n = static_cast<int>(st.range(0));
    for (auto _ : st) {
        benchmark::DoNotOptimize(n);
        int s = 0;
        for (int i = 0; i < n; ++i) s += i;
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_Runtime)->Arg(1000);
BENCHMARK_MAIN();
