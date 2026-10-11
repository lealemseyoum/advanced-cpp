#include <version>
#include <cstdio>
#define S2(x) #x
#define S(x) S2(x)
int main() {
#ifdef __cpp_concepts
  std::printf("__cpp_concepts=%s\n", S(__cpp_concepts));
#else
  std::printf("__cpp_concepts=-\n");
#endif
#ifdef __cpp_impl_three_way_comparison
  std::printf("__cpp_impl_three_way_comparison=%s\n", S(__cpp_impl_three_way_comparison));
#else
  std::printf("__cpp_impl_three_way_comparison=-\n");
#endif
#ifdef __cpp_designated_initializers
  std::printf("__cpp_designated_initializers=%s\n", S(__cpp_designated_initializers));
#else
  std::printf("__cpp_designated_initializers=-\n");
#endif
#ifdef __cpp_consteval
  std::printf("__cpp_consteval=%s\n", S(__cpp_consteval));
#else
  std::printf("__cpp_consteval=-\n");
#endif
#ifdef __cpp_constinit
  std::printf("__cpp_constinit=%s\n", S(__cpp_constinit));
#else
  std::printf("__cpp_constinit=-\n");
#endif
#ifdef __cpp_impl_coroutine
  std::printf("__cpp_impl_coroutine=%s\n", S(__cpp_impl_coroutine));
#else
  std::printf("__cpp_impl_coroutine=-\n");
#endif
#ifdef __cpp_modules
  std::printf("__cpp_modules=%s\n", S(__cpp_modules));
#else
  std::printf("__cpp_modules=-\n");
#endif
#ifdef __cpp_lib_modules
  std::printf("__cpp_lib_modules=%s\n", S(__cpp_lib_modules));
#else
  std::printf("__cpp_lib_modules=-\n");
#endif
#ifdef __cpp_nontype_template_args
  std::printf("__cpp_nontype_template_args=%s\n", S(__cpp_nontype_template_args));
#else
  std::printf("__cpp_nontype_template_args=-\n");
#endif
#ifdef __cpp_constexpr_dynamic_alloc
  std::printf("__cpp_constexpr_dynamic_alloc=%s\n", S(__cpp_constexpr_dynamic_alloc));
#else
  std::printf("__cpp_constexpr_dynamic_alloc=-\n");
#endif
#ifdef __cpp_char8_t
  std::printf("__cpp_char8_t=%s\n", S(__cpp_char8_t));
#else
  std::printf("__cpp_char8_t=-\n");
#endif
#ifdef __cpp_explicit_this_parameter
  std::printf("__cpp_explicit_this_parameter=%s\n", S(__cpp_explicit_this_parameter));
#else
  std::printf("__cpp_explicit_this_parameter=-\n");
#endif
#ifdef __cpp_multidimensional_subscript
  std::printf("__cpp_multidimensional_subscript=%s\n", S(__cpp_multidimensional_subscript));
#else
  std::printf("__cpp_multidimensional_subscript=-\n");
#endif
#ifdef __cpp_if_consteval
  std::printf("__cpp_if_consteval=%s\n", S(__cpp_if_consteval));
#else
  std::printf("__cpp_if_consteval=-\n");
#endif
#ifdef __cpp_static_call_operator
  std::printf("__cpp_static_call_operator=%s\n", S(__cpp_static_call_operator));
#else
  std::printf("__cpp_static_call_operator=-\n");
#endif
#ifdef __cpp_size_t_suffix
  std::printf("__cpp_size_t_suffix=%s\n", S(__cpp_size_t_suffix));
#else
  std::printf("__cpp_size_t_suffix=-\n");
#endif
#ifdef __cpp_pack_indexing
  std::printf("__cpp_pack_indexing=%s\n", S(__cpp_pack_indexing));
#else
  std::printf("__cpp_pack_indexing=-\n");
#endif
#ifdef __cpp_placeholder_variables
  std::printf("__cpp_placeholder_variables=%s\n", S(__cpp_placeholder_variables));
#else
  std::printf("__cpp_placeholder_variables=-\n");
#endif
#ifdef __cpp_static_assert
  std::printf("__cpp_static_assert=%s\n", S(__cpp_static_assert));
#else
  std::printf("__cpp_static_assert=-\n");
#endif
#ifdef __cpp_deleted_function
  std::printf("__cpp_deleted_function=%s\n", S(__cpp_deleted_function));
#else
  std::printf("__cpp_deleted_function=-\n");
#endif
#ifdef __cpp_variadic_friend
  std::printf("__cpp_variadic_friend=%s\n", S(__cpp_variadic_friend));
#else
  std::printf("__cpp_variadic_friend=-\n");
#endif
#ifdef __cpp_structured_bindings
  std::printf("__cpp_structured_bindings=%s\n", S(__cpp_structured_bindings));
#else
  std::printf("__cpp_structured_bindings=-\n");
#endif
#ifdef __cpp_trivial_relocatability
  std::printf("__cpp_trivial_relocatability=%s\n", S(__cpp_trivial_relocatability));
#else
  std::printf("__cpp_trivial_relocatability=-\n");
#endif
#ifdef __cpp_contracts
  std::printf("__cpp_contracts=%s\n", S(__cpp_contracts));
#else
  std::printf("__cpp_contracts=-\n");
#endif
#ifdef __cpp_reflection
  std::printf("__cpp_reflection=%s\n", S(__cpp_reflection));
#else
  std::printf("__cpp_reflection=-\n");
#endif
#ifdef __cpp_expansion_statements
  std::printf("__cpp_expansion_statements=%s\n", S(__cpp_expansion_statements));
#else
  std::printf("__cpp_expansion_statements=-\n");
#endif
#ifdef __cpp_lib_ranges
  std::printf("__cpp_lib_ranges=%s\n", S(__cpp_lib_ranges));
#else
  std::printf("__cpp_lib_ranges=-\n");
#endif
#ifdef __cpp_lib_format
  std::printf("__cpp_lib_format=%s\n", S(__cpp_lib_format));
#else
  std::printf("__cpp_lib_format=-\n");
#endif
#ifdef __cpp_lib_span
  std::printf("__cpp_lib_span=%s\n", S(__cpp_lib_span));
#else
  std::printf("__cpp_lib_span=-\n");
#endif
#ifdef __cpp_lib_jthread
  std::printf("__cpp_lib_jthread=%s\n", S(__cpp_lib_jthread));
#else
  std::printf("__cpp_lib_jthread=-\n");
#endif
#ifdef __cpp_lib_semaphore
  std::printf("__cpp_lib_semaphore=%s\n", S(__cpp_lib_semaphore));
#else
  std::printf("__cpp_lib_semaphore=-\n");
#endif
#ifdef __cpp_lib_latch
  std::printf("__cpp_lib_latch=%s\n", S(__cpp_lib_latch));
#else
  std::printf("__cpp_lib_latch=-\n");
#endif
#ifdef __cpp_lib_barrier
  std::printf("__cpp_lib_barrier=%s\n", S(__cpp_lib_barrier));
#else
  std::printf("__cpp_lib_barrier=-\n");
#endif
#ifdef __cpp_lib_atomic_wait
  std::printf("__cpp_lib_atomic_wait=%s\n", S(__cpp_lib_atomic_wait));
#else
  std::printf("__cpp_lib_atomic_wait=-\n");
#endif
#ifdef __cpp_lib_coroutine
  std::printf("__cpp_lib_coroutine=%s\n", S(__cpp_lib_coroutine));
#else
  std::printf("__cpp_lib_coroutine=-\n");
#endif
#ifdef __cpp_lib_bit_cast
  std::printf("__cpp_lib_bit_cast=%s\n", S(__cpp_lib_bit_cast));
#else
  std::printf("__cpp_lib_bit_cast=-\n");
#endif
#ifdef __cpp_lib_source_location
  std::printf("__cpp_lib_source_location=%s\n", S(__cpp_lib_source_location));
#else
  std::printf("__cpp_lib_source_location=-\n");
#endif
#ifdef __cpp_lib_three_way_comparison
  std::printf("__cpp_lib_three_way_comparison=%s\n", S(__cpp_lib_three_way_comparison));
#else
  std::printf("__cpp_lib_three_way_comparison=-\n");
#endif
#ifdef __cpp_lib_chrono
  std::printf("__cpp_lib_chrono=%s\n", S(__cpp_lib_chrono));
#else
  std::printf("__cpp_lib_chrono=-\n");
#endif
#ifdef __cpp_lib_syncbuf
  std::printf("__cpp_lib_syncbuf=%s\n", S(__cpp_lib_syncbuf));
#else
  std::printf("__cpp_lib_syncbuf=-\n");
#endif
#ifdef __cpp_lib_to_array
  std::printf("__cpp_lib_to_array=%s\n", S(__cpp_lib_to_array));
#else
  std::printf("__cpp_lib_to_array=-\n");
#endif
#ifdef __cpp_lib_expected
  std::printf("__cpp_lib_expected=%s\n", S(__cpp_lib_expected));
#else
  std::printf("__cpp_lib_expected=-\n");
#endif
#ifdef __cpp_lib_print
  std::printf("__cpp_lib_print=%s\n", S(__cpp_lib_print));
#else
  std::printf("__cpp_lib_print=-\n");
#endif
#ifdef __cpp_lib_generator
  std::printf("__cpp_lib_generator=%s\n", S(__cpp_lib_generator));
#else
  std::printf("__cpp_lib_generator=-\n");
#endif
#ifdef __cpp_lib_mdspan
  std::printf("__cpp_lib_mdspan=%s\n", S(__cpp_lib_mdspan));
#else
  std::printf("__cpp_lib_mdspan=-\n");
#endif
#ifdef __cpp_lib_flat_map
  std::printf("__cpp_lib_flat_map=%s\n", S(__cpp_lib_flat_map));
#else
  std::printf("__cpp_lib_flat_map=-\n");
#endif
#ifdef __cpp_lib_ranges_to_container
  std::printf("__cpp_lib_ranges_to_container=%s\n", S(__cpp_lib_ranges_to_container));
#else
  std::printf("__cpp_lib_ranges_to_container=-\n");
#endif
#ifdef __cpp_lib_ranges_zip
  std::printf("__cpp_lib_ranges_zip=%s\n", S(__cpp_lib_ranges_zip));
#else
  std::printf("__cpp_lib_ranges_zip=-\n");
#endif
#ifdef __cpp_lib_ranges_chunk
  std::printf("__cpp_lib_ranges_chunk=%s\n", S(__cpp_lib_ranges_chunk));
#else
  std::printf("__cpp_lib_ranges_chunk=-\n");
#endif
#ifdef __cpp_lib_ranges_enumerate
  std::printf("__cpp_lib_ranges_enumerate=%s\n", S(__cpp_lib_ranges_enumerate));
#else
  std::printf("__cpp_lib_ranges_enumerate=-\n");
#endif
#ifdef __cpp_lib_move_only_function
  std::printf("__cpp_lib_move_only_function=%s\n", S(__cpp_lib_move_only_function));
#else
  std::printf("__cpp_lib_move_only_function=-\n");
#endif
#ifdef __cpp_lib_stacktrace
  std::printf("__cpp_lib_stacktrace=%s\n", S(__cpp_lib_stacktrace));
#else
  std::printf("__cpp_lib_stacktrace=-\n");
#endif
#ifdef __cpp_lib_unreachable
  std::printf("__cpp_lib_unreachable=%s\n", S(__cpp_lib_unreachable));
#else
  std::printf("__cpp_lib_unreachable=-\n");
#endif
#ifdef __cpp_lib_start_lifetime_as
  std::printf("__cpp_lib_start_lifetime_as=%s\n", S(__cpp_lib_start_lifetime_as));
#else
  std::printf("__cpp_lib_start_lifetime_as=-\n");
#endif
#ifdef __cpp_lib_byteswap
  std::printf("__cpp_lib_byteswap=%s\n", S(__cpp_lib_byteswap));
#else
  std::printf("__cpp_lib_byteswap=-\n");
#endif
#ifdef __cpp_lib_is_scoped_enum
  std::printf("__cpp_lib_is_scoped_enum=%s\n", S(__cpp_lib_is_scoped_enum));
#else
  std::printf("__cpp_lib_is_scoped_enum=-\n");
#endif
#ifdef __cpp_lib_optional
  std::printf("__cpp_lib_optional=%s\n", S(__cpp_lib_optional));
#else
  std::printf("__cpp_lib_optional=-\n");
#endif
#ifdef __cpp_lib_inplace_vector
  std::printf("__cpp_lib_inplace_vector=%s\n", S(__cpp_lib_inplace_vector));
#else
  std::printf("__cpp_lib_inplace_vector=-\n");
#endif
#ifdef __cpp_lib_hive
  std::printf("__cpp_lib_hive=%s\n", S(__cpp_lib_hive));
#else
  std::printf("__cpp_lib_hive=-\n");
#endif
#ifdef __cpp_lib_linalg
  std::printf("__cpp_lib_linalg=%s\n", S(__cpp_lib_linalg));
#else
  std::printf("__cpp_lib_linalg=-\n");
#endif
#ifdef __cpp_lib_execution
  std::printf("__cpp_lib_execution=%s\n", S(__cpp_lib_execution));
#else
  std::printf("__cpp_lib_execution=-\n");
#endif
#ifdef __cpp_lib_submdspan
  std::printf("__cpp_lib_submdspan=%s\n", S(__cpp_lib_submdspan));
#else
  std::printf("__cpp_lib_submdspan=-\n");
#endif
#ifdef __cpp_lib_function_ref
  std::printf("__cpp_lib_function_ref=%s\n", S(__cpp_lib_function_ref));
#else
  std::printf("__cpp_lib_function_ref=-\n");
#endif
#ifdef __cpp_lib_copyable_function
  std::printf("__cpp_lib_copyable_function=%s\n", S(__cpp_lib_copyable_function));
#else
  std::printf("__cpp_lib_copyable_function=-\n");
#endif
#ifdef __cpp_lib_rcu
  std::printf("__cpp_lib_rcu=%s\n", S(__cpp_lib_rcu));
#else
  std::printf("__cpp_lib_rcu=-\n");
#endif
#ifdef __cpp_lib_hazard_pointer
  std::printf("__cpp_lib_hazard_pointer=%s\n", S(__cpp_lib_hazard_pointer));
#else
  std::printf("__cpp_lib_hazard_pointer=-\n");
#endif
#ifdef __cpp_lib_saturation_arithmetic
  std::printf("__cpp_lib_saturation_arithmetic=%s\n", S(__cpp_lib_saturation_arithmetic));
#else
  std::printf("__cpp_lib_saturation_arithmetic=-\n");
#endif
#ifdef __cpp_lib_text_encoding
  std::printf("__cpp_lib_text_encoding=%s\n", S(__cpp_lib_text_encoding));
#else
  std::printf("__cpp_lib_text_encoding=-\n");
#endif
#ifdef __cpp_lib_philox_engine
  std::printf("__cpp_lib_philox_engine=%s\n", S(__cpp_lib_philox_engine));
#else
  std::printf("__cpp_lib_philox_engine=-\n");
#endif
#ifdef __cpp_lib_ranges_concat
  std::printf("__cpp_lib_ranges_concat=%s\n", S(__cpp_lib_ranges_concat));
#else
  std::printf("__cpp_lib_ranges_concat=-\n");
#endif
#ifdef __cpp_lib_ranges_to_input
  std::printf("__cpp_lib_ranges_to_input=%s\n", S(__cpp_lib_ranges_to_input));
#else
  std::printf("__cpp_lib_ranges_to_input=-\n");
#endif
#ifdef __cpp_lib_fixed_string
  std::printf("__cpp_lib_fixed_string=%s\n", S(__cpp_lib_fixed_string));
#else
  std::printf("__cpp_lib_fixed_string=-\n");
#endif
#ifdef __cpp_lib_contracts
  std::printf("__cpp_lib_contracts=%s\n", S(__cpp_lib_contracts));
#else
  std::printf("__cpp_lib_contracts=-\n");
#endif
#ifdef __cpp_lib_constexpr_exceptions
  std::printf("__cpp_lib_constexpr_exceptions=%s\n", S(__cpp_lib_constexpr_exceptions));
#else
  std::printf("__cpp_lib_constexpr_exceptions=-\n");
#endif
#ifdef __cpp_lib_ranges_cache_latest
  std::printf("__cpp_lib_ranges_cache_latest=%s\n", S(__cpp_lib_ranges_cache_latest));
#else
  std::printf("__cpp_lib_ranges_cache_latest=-\n");
#endif
}
