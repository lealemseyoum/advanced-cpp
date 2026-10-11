#include <benchmark/benchmark.h>
#include <memory>
struct Widget { int x = 1; };
[[gnu::noinline]] int by_value(std::shared_ptr<Widget> p)       { return p->x; }
[[gnu::noinline]] int by_cref (const std::shared_ptr<Widget>& p) { return p->x; }
[[gnu::noinline]] int by_ref  (const Widget& w)                  { return w.x; }
[[gnu::noinline]] int by_ptr  (const Widget* w)                  { return w->x; }
static auto sp = std::make_shared<Widget>();
static void BM_value(benchmark::State& s) { for (auto _ : s) { benchmark::DoNotOptimize(sp); benchmark::DoNotOptimize(by_value(sp)); } }
static void BM_cref (benchmark::State& s) { for (auto _ : s) { benchmark::DoNotOptimize(sp); benchmark::DoNotOptimize(by_cref(sp)); } }
static void BM_ref  (benchmark::State& s) { for (auto _ : s) { benchmark::DoNotOptimize(sp); benchmark::DoNotOptimize(by_ref(*sp)); } }
static void BM_ptr  (benchmark::State& s) { for (auto _ : s) { benchmark::DoNotOptimize(sp); benchmark::DoNotOptimize(by_ptr(sp.get())); } }
BENCHMARK(BM_value); BENCHMARK(BM_cref); BENCHMARK(BM_ref); BENCHMARK(BM_ptr);
BENCHMARK(BM_value)->Threads(2); BENCHMARK(BM_cref)->Threads(2);
BENCHMARK_MAIN();
