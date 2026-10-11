# (name, standard, code). Each is a complete translation unit; "ok" means it compiles (-fsyntax-only) in the newest mode.
S = []
def add(name, std, code): S.append((name, std, code))

# ---------------- C++20 ----------------
add("concepts + requires", "C++20", """
#include <concepts>
template <std::integral T> T twice(T x) { return x * 2; }
template <class T> concept HasSize = requires(T t) { { t.size() } -> std::convertible_to<std::size_t>; };
static_assert(HasSize<int[3]> == false); int f() { return twice(2); }""")
add("ranges pipeline", "C++20", """
#include <ranges>
#include <vector>
int f() { std::vector<int> v{1,2,3,4}; int s = 0; for (int x : v | std::views::filter([](int i){return i%2==0;}) | std::views::transform([](int i){return i*i;})) s += x; return s; }""")
add("std::format", "C++20", """
#include <format>
#include <string>
std::string f() { return std::format("{:>8.3f}|{:#x}|{}", 3.14159, 255, "x"); }""")
add("std::span", "C++20", """
#include <span>
int f(std::span<const int> s) { return s.empty() ? 0 : s.front(); }""")
add("<=> and defaulted ==", "C++20", """
#include <compare>
struct P { int a, b; auto operator<=>(const P&) const = default; };
static_assert(P{1,2} < P{1,3});""")
add("designated initializers", "C++20", """
struct O { int a = 0; int b = 0; }; constexpr O o{.a = 1, .b = 2}; static_assert(o.b == 2);""")
add("consteval / constinit", "C++20", """
consteval int sq(int x) { return x * x; } constinit int g = sq(7); int f() { return g; }""")
add("coroutines (language + <coroutine>)", "C++20", """
#include <coroutine>
struct T { struct promise_type { T get_return_object() { return {}; } std::suspend_never initial_suspend() noexcept { return {}; }
  std::suspend_never final_suspend() noexcept { return {}; } void return_void() {} void unhandled_exception() {} }; };
T f() { co_return; }""")
add("jthread + stop_token", "C++20", """
#include <thread>
#include <stop_token>
void f() { std::jthread t([](std::stop_token st) { while (!st.stop_requested()) {} }); t.request_stop(); }""")
add("latch / barrier / semaphore", "C++20", """
#include <latch>
#include <barrier>
#include <semaphore>
void f() { std::latch l(1); std::barrier b(1); std::counting_semaphore<4> s(1); l.count_down(); b.arrive_and_wait(); s.acquire(); }""")
add("atomic wait/notify", "C++20", """
#include <atomic>
void f() { std::atomic<int> a{0}; a.wait(1); a.notify_one(); }""")
add("std::source_location", "C++20", """
#include <source_location>
int f(std::source_location l = std::source_location::current()) { return int(l.line()); }""")
add("std::bit_cast", "C++20", """
#include <bit>
constexpr auto f() { return std::bit_cast<unsigned>(1.0f); } static_assert(f() == 0x3f800000u);""")
add("constexpr std::vector / std::string", "C++20", """
#include <vector>
#include <string>
constexpr int f() { std::vector<int> v{1,2,3}; std::string s = "hello"; return int(v.size() + s.size()); } static_assert(f() == 8);""")
add("calendar (year_month_day)", "C++20", """
#include <chrono>
using namespace std::chrono; constexpr auto d = year{2026}/October/11; static_assert(d.ok());""")
add("chrono time zones (tzdb)", "C++20", """
#include <chrono>
auto f() { return std::chrono::current_zone(); }""")
add("std::osyncstream", "C++20", """
#include <syncstream>
#include <iostream>
void f() { std::osyncstream(std::cout) << "x"; }""")

# ---------------- C++23 ----------------
add("deducing this", "C++23", """
struct S { int v; template <class Self> auto&& get(this Self&& self) { return self.v; } }; int f() { S s{1}; return s.get(); }""")
add("if consteval", "C++23", """
constexpr int f(int x) { if consteval { return x; } else { return x + 1; } } static_assert(f(1) == 1);""")
add("std::expected (+ monadic)", "C++23", """
#include <expected>
std::expected<int, int> f() { return std::expected<int,int>(1).and_then([](int x) -> std::expected<int,int> { return x + 1; }); }""")
add("std::print / println", "C++23", """
#include <print>
void f() { std::println("{} {}", 1, "two"); }""")
add("std::generator", "C++23", """
#include <generator>
std::generator<int> f() { for (int i = 0; i < 3; ++i) co_yield i; }""")
add("std::mdspan", "C++23", """
#include <mdspan>
int f(int* p) { std::mdspan m(p, 2, 3); return m[1, 2]; }""")
add("std::flat_map", "C++23", """
#include <flat_map>
int f() { std::flat_map<int,int> m; m[1] = 2; return m[1]; }""")
add("ranges::to", "C++23", """
#include <ranges>
#include <vector>
auto f() { return std::views::iota(0, 5) | std::ranges::to<std::vector>(); }""")
add("views::zip / enumerate / chunk", "C++23", """
#include <ranges>
#include <vector>
void f() { std::vector<int> a{1,2}, b{3,4}; for (auto [x, y] : std::views::zip(a, b)) (void)(x+y);
  for (auto [i, x] : std::views::enumerate(a)) (void)(i+x); for (auto c : a | std::views::chunk(2)) (void)c; }""")
add("std::move_only_function", "C++23", """
#include <functional>
#include <memory>
void f() { std::move_only_function<int()> g = [p = std::make_unique<int>(1)] { return *p; }; (void)g(); }""")
add("std::stacktrace (compiles)", "C++23", """
#include <stacktrace>
auto f() { return std::stacktrace::current(); }""")
add("std::unreachable / byteswap / to_underlying", "C++23", """
#include <utility>
#include <bit>
int f(int x) { switch (x) { case 0: return int(std::byteswap(1u)); default: std::unreachable(); } }""")
add("std::start_lifetime_as", "C++23", """
#include <memory>
struct P { int a; }; const P* f(const void* p) { return std::start_lifetime_as<P>(p); }""")
add("formatting ranges (format of vector)", "C++23", """
#include <format>
#include <vector>
auto f() { return std::format("{}", std::vector<int>{1,2}); }""")
add("static operator() / auto(x)", "C++23", """
struct F { static int operator()(int x) { return x; } }; int g(int x) { return F{}(auto(x)); }""")
add("import std;  (needs prebuilt std module)", "C++23", """
import std;
int main() { std::println("hi"); }""")

# ---------------- C++26 ----------------
add("pack indexing  Ts...[0]", "C++26", """
template <class... Ts> using First = Ts...[0]; static_assert(sizeof(First<char, int>) == 1);""")
add("placeholder  _  variable", "C++26", """
int f() { int _ = 1; int _ = 2; return 0; }""")
add("static_assert(cond, user message)", "C++26", """
#include <string_view>
struct M { constexpr std::size_t size() const { return 2; } constexpr const char* data() const { return "ok"; } };
static_assert(true, M{});""")
add("#embed", "C++26", """
const unsigned char d[] = {
#embed "/dev/null" if_empty(0)
};""")
add("std::inplace_vector", "C++26", """
#include <inplace_vector>
int f() { std::inplace_vector<int, 4> v; v.push_back(1); return v[0]; }""")
add("std::function_ref", "C++26", """
#include <functional>
int g(std::function_ref<int(int)> f) { return f(1); }""")
add("std::copyable_function", "C++26", """
#include <functional>
void f() { std::copyable_function<int()> g = [] { return 1; }; (void)g(); }""")
add("saturating arithmetic (add_sat)", "C++26", """
#include <numeric>
int f(int a, int b) { return std::add_sat(a, b); }""")
add("contracts  pre / post / contract_assert", "C++26", """
int f(int x) pre(x > 0) post(r : r > 0) { contract_assert(x != 5); return x; }""")
add("static reflection  ^^T  [: :]", "C++26", """
#include <meta>
constexpr auto r = ^^int; using I = [:r:];""")
add("template for (expansion statements)", "C++26", """
#include <tuple>
void f() { template for (auto x : std::tuple{1, 2.0}) (void)x; }""")
add("std::execution  (senders/receivers)", "C++26", """
#include <execution>
namespace ex = std::execution; auto f() { return ex::just(1) | ex::then([](int x) { return x + 1; }); }""")
add("std::hive", "C++26", """
#include <hive>
int f() { std::hive<int> h; h.insert(1); return *h.begin(); }""")
add("std::text_encoding", "C++26", """
#include <text_encoding>
auto f() { return std::text_encoding::environment(); }""")
add("trivially relocatable  (std::is_trivially_relocatable)", "C++26", """
#include <type_traits>
static_assert(std::is_trivially_relocatable_v<int>);""")
