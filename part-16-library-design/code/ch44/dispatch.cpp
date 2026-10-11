// Four ways to dispatch over a closed set of shapes, and four ways to inject a strategy
#include <benchmark/benchmark.h>
#include <functional>
#include <memory>
#include <random>
#include <variant>
#include <vector>

// ---- A. classic OO ----
struct Shape { virtual ~Shape() = default; virtual double area() const = 0; };
struct Circle  : Shape { double r;    explicit Circle(double r)  : r(r) {} double area() const override { return 3.14159 * r * r; } };
struct Rect    : Shape { double w, h; Rect(double w, double h) : w(w), h(h) {} double area() const override { return w * h; } };
struct Tri     : Shape { double b, h; Tri(double b, double h)   : b(b), h(h) {} double area() const override { return 0.5 * b * h; } };

// ---- B. closed sum type ----
struct C { double r; };  struct R { double w, h; };  struct T3 { double b, h; };
using V = std::variant<C, R, T3>;
struct AreaVisitor {
    double operator()(const C& c) const { return 3.14159 * c.r * c.r; }
    double operator()(const R& r) const { return r.w * r.h; }
    double operator()(const T3& t) const { return 0.5 * t.b * t.h; }
};

// ---- C. enum tag + switch (the C way) ----
enum class Kind : unsigned char { C, R, T };
struct Tagged { Kind k; double a, b; };
inline double area(const Tagged& t) {
    switch (t.k) { case Kind::C: return 3.14159 * t.a * t.a; case Kind::R: return t.a * t.b; case Kind::T: return 0.5 * t.a * t.b; }
    return 0;
}

// ---- D. type-erased value (std::function per object) ----
using Fn = std::function<double()>;

static constexpr int N = 1 << 16;
static std::vector<int> kinds() { std::mt19937 g(1); std::vector<int> v(N); for (auto& k : v) k = g() % 3; return v; }

static void BM_virtual(benchmark::State& st) {
    auto ks = kinds(); std::vector<std::unique_ptr<Shape>> v;
    for (int k : ks) { if (k == 0) v.push_back(std::make_unique<Circle>(2)); else if (k == 1) v.push_back(std::make_unique<Rect>(2, 3)); else v.push_back(std::make_unique<Tri>(2, 3)); }
    for (auto _ : st) { double s = 0; for (auto& p : v) s += p->area(); benchmark::DoNotOptimize(s); }
    st.SetItemsProcessed(st.iterations() * N);
}
static void BM_variant(benchmark::State& st) {
    auto ks = kinds(); std::vector<V> v;
    for (int k : ks) { if (k == 0) v.emplace_back(C{2}); else if (k == 1) v.emplace_back(R{2, 3}); else v.emplace_back(T3{2, 3}); }
    for (auto _ : st) { double s = 0; for (auto& x : v) s += std::visit(AreaVisitor{}, x); benchmark::DoNotOptimize(s); }
    st.SetItemsProcessed(st.iterations() * N);
}
static void BM_tagged(benchmark::State& st) {
    auto ks = kinds(); std::vector<Tagged> v;
    for (int k : ks) v.push_back(k == 0 ? Tagged{Kind::C, 2, 0} : k == 1 ? Tagged{Kind::R, 2, 3} : Tagged{Kind::T, 2, 3});
    for (auto _ : st) { double s = 0; for (auto& x : v) s += area(x); benchmark::DoNotOptimize(s); }
    st.SetItemsProcessed(st.iterations() * N);
}
static void BM_function(benchmark::State& st) {
    auto ks = kinds(); std::vector<Fn> v;
    for (int k : ks) { if (k == 0) v.emplace_back([r = 2.0] { return 3.14159 * r * r; }); else if (k == 1) v.emplace_back([w = 2.0, h = 3.0] { return w * h; }); else v.emplace_back([b = 2.0, h = 3.0] { return 0.5 * b * h; }); }
    for (auto _ : st) { double s = 0; for (auto& f : v) s += f(); benchmark::DoNotOptimize(s); }
    st.SetItemsProcessed(st.iterations() * N);
}
BENCHMARK(BM_virtual); BENCHMARK(BM_variant); BENCHMARK(BM_tagged); BENCHMARK(BM_function);

// ---- Strategy injection: apply a unary op to 1M ints ----
struct IStrategy { virtual ~IStrategy() = default; virtual int apply(int) const = 0; };
struct AddOne : IStrategy { int apply(int x) const override { return x + 1; } };
template <class S> long run_t(const std::vector<int>& v, S s) { long t = 0; for (int x : v) t += s(x); return t; }
[[gnu::noinline]] long run_virtual(const std::vector<int>& v, const IStrategy& s) { long t = 0; for (int x : v) t += s.apply(x); return t; }
[[gnu::noinline]] long run_fn(const std::vector<int>& v, const std::function<int(int)>& f) { long t = 0; for (int x : v) t += f(x); return t; }
[[gnu::noinline]] long run_fptr(const std::vector<int>& v, int (*f)(int)) { long t = 0; for (int x : v) t += f(x); return t; }
[[gnu::noinline]] long run_tmpl(const std::vector<int>& v) { return run_t(v, [](int x) { return x + 1; }); }
[[gnu::noinline]] int plus1(int x) { return x + 1; }

static std::vector<int> data() { std::vector<int> v(1 << 16, 3); return v; }
static void BM_strat_virtual(benchmark::State& st)  { auto v = data(); AddOne a; for (auto _ : st) { benchmark::DoNotOptimize(&a); benchmark::DoNotOptimize(run_virtual(v, a)); } st.SetItemsProcessed(st.iterations() * v.size()); }
static void BM_strat_function(benchmark::State& st) { auto v = data(); std::function<int(int)> f = [](int x) { return x + 1; }; for (auto _ : st) { benchmark::DoNotOptimize(&f); benchmark::DoNotOptimize(run_fn(v, f)); } st.SetItemsProcessed(st.iterations() * v.size()); }
static void BM_strat_fptr(benchmark::State& st)     { auto v = data(); int (*f)(int) = &plus1; for (auto _ : st) { benchmark::DoNotOptimize(&f); benchmark::DoNotOptimize(run_fptr(v, f)); } st.SetItemsProcessed(st.iterations() * v.size()); }
static void BM_strat_template(benchmark::State& st) { auto v = data(); for (auto _ : st) benchmark::DoNotOptimize(run_tmpl(v)); st.SetItemsProcessed(st.iterations() * v.size()); }
BENCHMARK(BM_strat_virtual); BENCHMARK(BM_strat_function); BENCHMARK(BM_strat_fptr); BENCHMARK(BM_strat_template);
BENCHMARK_MAIN();
