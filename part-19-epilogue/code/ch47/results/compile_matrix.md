| Feature | Std | GCC 13.3 | GCC 14.2 | Clang 18.1 |
|---|:--:|:--:|:--:|:--:|
| concepts + requires | C++20 | ✅ | ✅ | ✅ |
| ranges pipeline | C++20 | ✅ | ✅ | ✅ |
| std::format | C++20 | ✅ | ✅ | ✅ |
| std::span | C++20 | ✅ | ✅ | ✅ |
| <=> and defaulted == | C++20 | ✅ | ✅ | ✅ |
| designated initializers | C++20 | ✅ | ✅ | ✅ |
| consteval / constinit | C++20 | ✅ | ✅ | ✅ |
| coroutines (language + <coroutine>) | C++20 | ✅ | ✅ | ✅ |
| jthread + stop_token | C++20 | ✅ | ✅ | ✅ |
| latch / barrier / semaphore | C++20 | ✅ | ✅ | ✅ |
| atomic wait/notify | C++20 | ✅ | ✅ | ✅ |
| std::source_location | C++20 | ✅ | ✅ | ✅ |
| std::bit_cast | C++20 | ✅ | ✅ | ✅ |
| constexpr std::vector / std::string | C++20 | ✅ | ✅ | ✅ |
| calendar (year_month_day) | C++20 | ✅ | ✅ | ✅ |
| chrono time zones (tzdb) | C++20 | ✅ | ✅ | ✅ |
| std::osyncstream | C++20 | ✅ | ✅ | ✅ |
| deducing this | C++23 | ❌ | ✅ | ✅ |
| if consteval | C++23 | ✅ | ✅ | ✅ |
| std::expected (+ monadic) | C++23 | ✅ | ✅ | ❌ |
| std::print / println | C++23 | ❌ | ✅ | ✅ |
| std::generator | C++23 | ❌ | ✅ | ❌ |
| std::mdspan | C++23 | ❌ | ❌ | ❌ |
| std::flat_map | C++23 | ❌ | ❌ | ❌ |
| ranges::to | C++23 | ❌ | ✅ | ✅ |
| views::zip / enumerate / chunk | C++23 | ✅ | ✅ | ✅ |
| std::move_only_function | C++23 | ✅ | ✅ | ✅ |
| std::stacktrace (compiles) | C++23 | ✅ | ✅ | ✅ |
| std::unreachable / byteswap / to_underlying | C++23 | ✅ | ✅ | ✅ |
| std::start_lifetime_as | C++23 | ❌ | ❌ | ❌ |
| formatting ranges (format of vector) | C++23 | ❌ | ❌ | ❌ |
| static operator() / auto(x) | C++23 | ✅ | ✅ | ✅ |
| import std;  (needs prebuilt std module) | C++23 | ❌ | ❌ | ❌ |
| pack indexing  Ts...[0] | C++26 | ❌ | ❌ | ❌ |
| placeholder  _  variable | C++26 | ❌ | ✅ | ✅ |
| static_assert(cond, user message) | C++26 | ❌ | ✅ | ✅ |
| #embed | C++26 | ❌ | ❌ | ❌ |
| std::inplace_vector | C++26 | ❌ | ❌ | ❌ |
| std::function_ref | C++26 | ❌ | ❌ | ❌ |
| std::copyable_function | C++26 | ❌ | ❌ | ❌ |
| saturating arithmetic (add_sat) | C++26 | ❌ | ✅ | ✅ |
| contracts  pre / post / contract_assert | C++26 | ❌ | ❌ | ❌ |
| static reflection  ^^T  [: :] | C++26 | ❌ | ❌ | ❌ |
| template for (expansion statements) | C++26 | ❌ | ❌ | ❌ |
| std::execution  (senders/receivers) | C++26 | ❌ | ❌ | ❌ |
| std::hive | C++26 | ❌ | ❌ | ❌ |
| std::text_encoding | C++26 | ❌ | ✅ | ✅ |
| trivially relocatable  (std::is_trivially_relocatable) | C++26 | ❌ | ❌ | ❌ |
