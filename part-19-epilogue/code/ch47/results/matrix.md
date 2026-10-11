| Feature | Std | GCC 13.3 | GCC 14.2 | Clang 18.1.3 |
|---|:--:|:--:|:--:|:--:|
| concepts (`__cpp_concepts`) | C++20 | `202002L` | `202002L` | `201907L` |
| operator<=> (three-way comparison) (`__cpp_impl_three_way_comparison`) | C++20 | `201907L` | `201907L` | `201907L` |
| designated initializers (`__cpp_designated_initializers`) | C++20 | `201707L` | `201707L` | `201707L` |
| consteval (`__cpp_consteval`) | C++20 | `201811L` | `202211L` | `202211L` |
| constinit (`__cpp_constinit`) | C++20 | `201907L` | `201907L` | `201907L` |
| coroutines (language) (`__cpp_impl_coroutine`) | C++20 | `201902L` | `201902L` | `201902L` |
| modules (language) (`__cpp_modules`) | C++20 | — | — | — |
| import std; (library modules) (`__cpp_lib_modules`) | C++23 | — | — | — |
| class-type / floating NTTP (`__cpp_nontype_template_args`) | C++20 | `201911L` | `201911L` | `201411L` |
| constexpr new/delete (`__cpp_constexpr_dynamic_alloc`) | C++20 | `201907L` | `201907L` | `201907L` |
| char8_t (`__cpp_char8_t`) | C++20 | `202207L` | `202207L` | `202207L` |
| deducing this (`__cpp_explicit_this_parameter`) | C++23 | — | `202110L` | — |
| multidimensional operator[] (`__cpp_multidimensional_subscript`) | C++23 | `202211L` | `202211L` | `202211L` |
| if consteval (`__cpp_if_consteval`) | C++23 | `202106L` | `202106L` | `202106L` |
| static operator() (`__cpp_static_call_operator`) | C++23 | `202207L` | `202207L` | `202207L` |
| uz literal suffix (`__cpp_size_t_suffix`) | C++23 | `202011L` | `202011L` | `202011L` |
| pack indexing  Ts...[N] (`__cpp_pack_indexing`) | C++26 | — | — | — |
| placeholder _ variables (`__cpp_placeholder_variables`) | C++26 | — | `202306L` | `202306L` |
| static_assert with user message (`__cpp_static_assert`) | C++26 | `201411L` | `202306L` | `202306L` |
| deleted function with reason (`__cpp_deleted_function`) | C++26 | — | — | — |
| variadic friend (`__cpp_variadic_friend`) | C++26 | — | — | — |
| structured binding packs / attributes (`__cpp_structured_bindings`) | C++26 | `201606L` | `201606L` | `201606L` |
| trivial relocatability (`__cpp_trivial_relocatability`) | C++26 | — | — | — |
| contracts (`__cpp_contracts`) | C++26 | — | — | — |
| static reflection (`__cpp_reflection`) | C++26 | — | — | — |
| template for (expansion statements) (`__cpp_expansion_statements`) | C++26 | — | — | — |
| <ranges> core (`__cpp_lib_ranges`) | C++20 | `202202L` | `202211L` | `202211L` |
| std::format (`__cpp_lib_format`) | C++20 | `202110L` | `202110L` | `202110L` |
| std::span (`__cpp_lib_span`) | C++20 | `202002L` | `202311L` | `202311L` |
| std::jthread / stop_token (`__cpp_lib_jthread`) | C++20 | `201911L` | `201911L` | `201911L` |
| std::counting_semaphore (`__cpp_lib_semaphore`) | C++20 | `201907L` | `201907L` | `201907L` |
| std::latch (`__cpp_lib_latch`) | C++20 | `201907L` | `201907L` | `201907L` |
| std::barrier (`__cpp_lib_barrier`) | C++20 | `201907L` | `201907L` | `201907L` |
| std::atomic::wait/notify (`__cpp_lib_atomic_wait`) | C++20 | `201907L` | `201907L` | `201907L` |
| <coroutine> (`__cpp_lib_coroutine`) | C++20 | `201902L` | `201902L` | `201902L` |
| std::bit_cast (`__cpp_lib_bit_cast`) | C++20 | `201806L` | `201806L` | `201806L` |
| std::source_location (`__cpp_lib_source_location`) | C++20 | `201907L` | `201907L` | `201907L` |
| <compare> (`__cpp_lib_three_way_comparison`) | C++20 | `201907L` | `201907L` | `201907L` |
| calendar & time zones (<chrono> C++20 parts) (`__cpp_lib_chrono`) | C++20 | `201611L` | `201907L` | `201907L` |
| std::osyncstream (`__cpp_lib_syncbuf`) | C++20 | `201803L` | `201803L` | `201803L` |
| std::to_array (`__cpp_lib_to_array`) | C++20 | `201907L` | `201907L` | `201907L` |
| std::expected (`__cpp_lib_expected`) | C++23 | `202211L` | `202211L` | — |
| std::print / println (`__cpp_lib_print`) | C++23 | — | `202211L` | `202211L` |
| std::generator (`__cpp_lib_generator`) | C++23 | — | `202207L` | — |
| std::mdspan (`__cpp_lib_mdspan`) | C++23 | — | — | — |
| std::flat_map / flat_set (`__cpp_lib_flat_map`) | C++23 | — | — | — |
| ranges::to (`__cpp_lib_ranges_to_container`) | C++23 | — | `202202L` | `202202L` |
| views::zip (`__cpp_lib_ranges_zip`) | C++23 | `202110L` | `202110L` | `202110L` |
| views::chunk (`__cpp_lib_ranges_chunk`) | C++23 | `202202L` | `202202L` | `202202L` |
| views::enumerate (`__cpp_lib_ranges_enumerate`) | C++23 | `202302L` | `202302L` | `202302L` |
| std::move_only_function (`__cpp_lib_move_only_function`) | C++23 | `202110L` | `202110L` | `202110L` |
| std::stacktrace (`__cpp_lib_stacktrace`) | C++23 | `202011L` | `202011L` | `202011L` |
| std::unreachable (`__cpp_lib_unreachable`) | C++23 | `202202L` | `202202L` | `202202L` |
| std::start_lifetime_as (`__cpp_lib_start_lifetime_as`) | C++23 | — | — | — |
| std::byteswap (`__cpp_lib_byteswap`) | C++23 | `202110L` | `202110L` | `202110L` |
| is_scoped_enum (`__cpp_lib_is_scoped_enum`) | C++23 | `202011L` | `202011L` | `202011L` |
| optional monadic ops (C++23 level) (`__cpp_lib_optional`) | C++23 | `202110L` | `202110L` | `202110L` |
| std::inplace_vector (`__cpp_lib_inplace_vector`) | C++26 | — | — | — |
| std::hive (`__cpp_lib_hive`) | C++26 | — | — | — |
| std::linalg (`__cpp_lib_linalg`) | C++26 | — | — | — |
| std::execution (senders/receivers) (`__cpp_lib_execution`) | C++26 | `201902L` | `201902L` | `201902L` |
| std::submdspan (`__cpp_lib_submdspan`) | C++26 | — | — | — |
| std::function_ref (`__cpp_lib_function_ref`) | C++26 | — | — | — |
| std::copyable_function (`__cpp_lib_copyable_function`) | C++26 | — | — | — |
| std::rcu (`__cpp_lib_rcu`) | C++26 | — | — | — |
| std::hazard_pointer (`__cpp_lib_hazard_pointer`) | C++26 | — | — | — |
| saturating arithmetic (`__cpp_lib_saturation_arithmetic`) | C++26 | — | `202311L` | `202311L` |
| std::text_encoding (`__cpp_lib_text_encoding`) | C++26 | — | `202306L` | `202306L` |
| philox_engine (`__cpp_lib_philox_engine`) | C++26 | — | — | — |
| views::concat (`__cpp_lib_ranges_concat`) | C++26 | — | — | — |
| views::to_input (`__cpp_lib_ranges_to_input`) | C++26 | — | — | — |
| fixed_string (`__cpp_lib_fixed_string`) | C++26 | — | — | — |
| contracts library support (`__cpp_lib_contracts`) | C++26 | — | — | — |
| constexpr exceptions (`__cpp_lib_constexpr_exceptions`) | C++26 | — | — | — |
| views::cache_latest (`__cpp_lib_ranges_cache_latest`) | C++26 | — | — | — |
