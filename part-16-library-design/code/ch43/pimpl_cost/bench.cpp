#include <benchmark/benchmark.h>
#include "counter.hpp"
static void BM_plain_add(benchmark::State& s)  { CounterPlain c;  for (auto _ : s) { c.add(1); benchmark::ClobberMemory(); } benchmark::DoNotOptimize(c.value()); }
static void BM_pimpl_add(benchmark::State& s)  { CounterPimpl c;  for (auto _ : s) { c.add(1); benchmark::ClobberMemory(); } benchmark::DoNotOptimize(c.value()); }
static void BM_plain_make(benchmark::State& s) { for (auto _ : s) { CounterPlain c; c.add(1); benchmark::DoNotOptimize(c); } }
static void BM_pimpl_make(benchmark::State& s) { for (auto _ : s) { CounterPimpl c; c.add(1); benchmark::DoNotOptimize(c); } }
BENCHMARK(BM_plain_add); BENCHMARK(BM_pimpl_add); BENCHMARK(BM_plain_make); BENCHMARK(BM_pimpl_make);
BENCHMARK_MAIN();
