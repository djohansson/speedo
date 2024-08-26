#pragma once

#include <algorithm>
#include <bit>
#include <concepts>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

//NOLINTBEGIN(readability-identifier-naming)
namespace std_extra
{

template <std::size_t N>
using min_unsigned_t = std::tuple_element_t<
	std::bit_width(N) / 8,
	std::tuple<std::uint8_t, std::uint16_t, std::uint32_t, std::uint64_t>>;

#if __cpp_lib_hardware_interference_size >= 201603
using std::hardware_constructive_interference_size;
using std::hardware_destructive_interference_size;
#else
static constexpr std::size_t hardware_constructive_interference_size = 64;
static constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

template <typename T, std::align_val_t Alignment>
class aligned_allocator
{
public:
	using value_type = T;
	static constexpr auto kAlignment{Alignment};

	constexpr aligned_allocator() noexcept = default;

	template <typename U>
	constexpr explicit aligned_allocator(const aligned_allocator<U, kAlignment>& other) noexcept {};

	template <typename U>
	constexpr bool operator==(const aligned_allocator<U, kAlignment>& other) const noexcept
	{
		return true;
	}

	template <typename U>
	constexpr bool operator!=(const aligned_allocator<U, kAlignment>& other) const noexcept
	{
		return false;
	}

	template <typename U>
	struct rebind
	{
		using other = aligned_allocator<U, kAlignment>;
	};

	[[nodiscard]] T* allocate(std::size_t count)
	{
		return reinterpret_cast<T*>(::operator new[](count * sizeof(T), kAlignment));
	}
	
	void deallocate(T* allocPtr, [[maybe_unused]] std::size_t count)
	{
		::operator delete[](allocPtr, kAlignment);
	}
};

template <size_t N>
struct string_literal
{
	consteval string_literal(const char (&str)[N]) { std::copy_n(str, N, value); }//NOLINT(google-explicit-constructor,modernize-avoid-c-arrays)

	char value[N];//NOLINT(modernize-avoid-c-arrays)
};

template <string_literal S>
[[nodiscard]] consteval std::string_view make_string_literal()
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

template <typename F, typename Tuple>
using apply_result_t = decltype(std::apply(std::declval<F>(), std::declval<Tuple>()));

template <typename F, typename Tuple, typename = std::void_t<>>
class apply_result
{};
template <typename F, typename Tuple>
class apply_result<F, Tuple, std::void_t<apply_result_t<F, Tuple>>>
{
	using type = apply_result_t<F, Tuple>;
};

namespace detail
{
struct universal_type
{
	template <typename T>
	operator T();//NOLINT(google-explicit-constructor)
};

} // namespace detail

template <typename T, typename... Args>
consteval auto member_count()
{
	static_assert(std::is_aggregate_v<std::remove_cvref_t<T>>);

	if constexpr (requires { T{{Args{}}..., {detail::universal_type{}}}; })
	{
		return member_count<T, Args..., detail::universal_type>();
	}

	return sizeof...(Args);
}

} // namespace std_extra

//NOLINTEND(readability-identifier-naming)
