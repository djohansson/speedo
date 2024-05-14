#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

// NOLINTBEGIN(readability-identifier-naming.*)
namespace std_extra
{

template <size_t N>
struct string_literal
{
	consteval string_literal(const char (&str)[N]) { std::copy_n(str, N, value); }

	char value[N];
};

template <string_literal S>
consteval std::string_view make_string_literal()
{
	return S.value;
}

template<typename... Tuples>
using tuple_cat_t = decltype(std::tuple_cat(std::declval<Tuples>()...));

template<typename, typename>
struct is_applicable : std::false_type {};

template<typename Func, template<typename...> typename Tuple, typename... Args>
struct is_applicable<Func, Tuple<Args...>> : std::is_invocable<Func, Args...> {};

template<typename F, typename Tuple>
concept applicable = is_applicable<F, Tuple>::value;

template <class F, class T, std ::size_t... I>
constexpr auto apply_impl(F&& f, T&& t, std::index_sequence<I...>) noexcept(
	std::is_nothrow_invocable<F&&, decltype(std::get<I>(std ::declval<T>()))...>{})
	-> std::invoke_result_t<F&&, decltype(std::get<I>(std ::declval<T>()))...>
{
	return invoke(std::forward<F>(f), std::get<I>(std::forward<T>(t))...);
}
template <typename F, typename Tuple>
using apply_result_t = decltype(apply_impl(
	std::declval<F>(),
	std::declval<Tuple>(),
	std::make_index_sequence<std ::tuple_size_v<std ::decay_t<Tuple>>>{}));

template <typename F, typename Tuple, typename = std::void_t<>>
class apply_result
{};
template <typename F, typename Tuple>
class apply_result<F, Tuple, std::void_t<apply_result_t<F, Tuple>>>
{
	using type = apply_result_t<F, Tuple>;
};

template <class F, class Tuple>
constexpr apply_result_t<F, Tuple> apply(F&& f, Tuple&& t) noexcept
{
	return apply_impl(std::forward<F>(f), std::forward<Tuple>(t), std::make_index_sequence<std::tuple_size_v<std::decay_t<Tuple>>>{});
}

} // namespace std_extra

// NOLINTEND(readability-identifier-naming.*)
