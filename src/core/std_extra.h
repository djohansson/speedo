#pragma once

#include <core/crc32.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#if !defined(__cpp_lib_function_ref) || __cpp_lib_function_ref < 202306L
#include <tl/function_ref.hpp>
#endif

namespace core
{

//NOLINTBEGIN(readability-identifier-naming)
namespace std_extra
{

template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };

template <std::size_t N>
using min_unsigned_t = std::tuple_element_t<
	std::bit_width(N) / 8,
	std::tuple<std::uint8_t, std::uint16_t, std::uint32_t, std::uint64_t>>;

#if defined(__cpp_lib_hardware_interference_size) && __cpp_lib_hardware_interference_size >= 201603
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

template <typename>
std::false_type is_pointer_like(unsigned long);

template <typename T>
auto is_pointer_like(int) -> decltype(*std::declval<T>(), std::true_type{});

template <typename T>
auto is_pointer_like(long) -> decltype(std::declval<T>().operator->(), std::true_type{});

template <typename T>
auto is_pointer_like(short) -> decltype(std::declval<T>().operator*(), std::true_type{});

template <typename T>
using is_pointer_like_dereferencable = decltype(is_pointer_like<T>(0));

template <typename T>
static auto constexpr is_pointer_like_v = is_pointer_like_dereferencable<T>::value;

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

// a callable that is called during the call it is passed to (see CLAUDE.md: "Parallel work is tasks; callables are
// templates or tasks, not std::function"): not owning, never empty. c++26's, else tl's
#if defined(__cpp_lib_function_ref) && __cpp_lib_function_ref >= 202306L
template <typename Signature>
using function_ref = std::function_ref<Signature>;
#else
template <typename Signature>
using function_ref = tl::function_ref<Signature>;
#endif

// a callable that is stored and called again: owning, move only, maybe empty. c++23's, else this one, which keeps
// callables of up to four pointers (and nothrow movable) in place, larger ones on the heap. a const signature
// (R(Args...) const) calls the callable as const, and can be called on a const move_only_function
#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
using std::move_only_function;
#else
namespace detail
{

template <bool Const, typename R, typename... Args>
class move_only_function_base
{
public:
	move_only_function_base() noexcept = default;
	move_only_function_base(std::nullptr_t) noexcept {}//NOLINT(google-explicit-constructor)
	template <typename F>
	requires(!std::is_base_of_v<move_only_function_base, std::remove_cvref_t<F>> &&
			 std::is_invocable_r_v<R, std::conditional_t<Const, const std::decay_t<F>&, std::decay_t<F>&>, Args...>)
	move_only_function_base(F&& callable)//NOLINT(google-explicit-constructor,bugprone-forwarding-reference-overload)
	{
		using Callable = std::decay_t<F>;
		if constexpr (kInPlace<Callable>)
		{
			::new (static_cast<void*>(myStorage)) Callable(std::forward<F>(callable));
		}
		else
		{
			::new (static_cast<void*>(myStorage)) Callable*(new Callable(std::forward<F>(callable)));
		}
		myOps = &kOps<Callable>;
	}
	move_only_function_base(move_only_function_base&& other) noexcept { InternalMoveFrom(other); }
	move_only_function_base& operator=(move_only_function_base&& other) noexcept
	{
		if (this != &other)
		{
			InternalReset();
			InternalMoveFrom(other);
		}
		return *this;
	}
	move_only_function_base& operator=(std::nullptr_t) noexcept
	{
		InternalReset();
		return *this;
	}
	move_only_function_base(const move_only_function_base&) = delete;
	move_only_function_base& operator=(const move_only_function_base&) = delete;
	~move_only_function_base() { InternalReset(); }

	[[nodiscard]] explicit operator bool() const noexcept { return myOps != nullptr; }

protected:
	// const or not, as the signature says (see move_only_function's operator())
	R InternalInvoke(Args&&... args) const { return myOps->invoke(myStorage, std::forward<Args>(args)...); }

private:
	static constexpr std::size_t kInPlaceSize = 4 * sizeof(void*);

	template <typename Callable>
	static constexpr bool kInPlace = sizeof(Callable) <= kInPlaceSize && alignof(Callable) <= alignof(std::max_align_t) &&
									 std::is_nothrow_move_constructible_v<Callable>;

	struct Ops
	{
		R (*invoke)(std::byte* storage, Args&&... args);
		void (*move)(std::byte* destination, std::byte* source) noexcept; // and destroys source's
		void (*destroy)(std::byte* storage) noexcept;
	};

	template <typename Callable>
	[[nodiscard]] static Callable& Get(std::byte* storage) noexcept
	{
		if constexpr (kInPlace<Callable>)
			return *std::launder(reinterpret_cast<Callable*>(storage));
		else
			return **std::launder(reinterpret_cast<Callable**>(storage));
	}

	// as the signature says: const, or not
	template <typename Callable>
	[[nodiscard]] static decltype(auto) GetForInvoke(std::byte* storage) noexcept
	{
		if constexpr (Const)
			return std::as_const(Get<Callable>(storage));
		else
			return Get<Callable>(storage);
	}

	template <typename Callable>
	static constexpr Ops kOps{
		.invoke = [](std::byte* storage, Args&&... args) -> R
		{
			if constexpr (std::is_void_v<R>)
			{
				std::invoke(GetForInvoke<Callable>(storage), std::forward<Args>(args)...);
			}
			else
			{
				return std::invoke(GetForInvoke<Callable>(storage), std::forward<Args>(args)...);
			}
		},
		.move = [](std::byte* destination, std::byte* source) noexcept
		{
			if constexpr (kInPlace<Callable>)
			{
				::new (static_cast<void*>(destination)) Callable(std::move(Get<Callable>(source)));
				Get<Callable>(source).~Callable();
			}
			else
			{
				::new (static_cast<void*>(destination)) Callable*(&Get<Callable>(source));
			}
		},
		.destroy = [](std::byte* storage) noexcept
		{
			if constexpr (kInPlace<Callable>)
			{
				Get<Callable>(storage).~Callable();
			}
			else
			{
				delete &Get<Callable>(storage);
			}
		},
	};

	void InternalMoveFrom(move_only_function_base& other) noexcept
	{
		if (other.myOps == nullptr)
			return;
		other.myOps->move(myStorage, other.myStorage);
		myOps = std::exchange(other.myOps, nullptr);
	}
	void InternalReset() noexcept
	{
		if (myOps != nullptr)
			std::exchange(myOps, nullptr)->destroy(myStorage);
	}

	// mutable: a const signature calls the callable as const (see GetForInvoke) from a const operator()
	alignas(std::max_align_t) mutable std::byte myStorage[kInPlaceSize]{};//NOLINT(modernize-avoid-c-arrays)
	const Ops* myOps = nullptr;
};

} // namespace detail

template <typename Signature>
class move_only_function;

template <typename R, typename... Args>
class move_only_function<R(Args...)> : public detail::move_only_function_base<false, R, Args...>
{
	using Base = detail::move_only_function_base<false, R, Args...>;

public:
	using Base::Base;
	using Base::operator=;

	R operator()(Args... args) { return Base::InternalInvoke(std::forward<Args>(args)...); }
};

template <typename R, typename... Args>
class move_only_function<R(Args...) const> : public detail::move_only_function_base<true, R, Args...>
{
	using Base = detail::move_only_function_base<true, R, Args...>;

public:
	using Base::Base;
	using Base::operator=;

	R operator()(Args... args) const { return Base::InternalInvoke(std::forward<Args>(args)...); }
};
#endif

} // namespace std_extra

} // namespace core

#define SOURCE_LOCATION_DATA(tag) \
	SourceLocationData{ \
		.name = core::std_extra::make_string_literal<__func__>().data(), \
		.function = core::std_extra::make_string_literal<__PRETTY_FUNCTION__>().data(), \
		.file = core::std_extra::make_string_literal<__FILE__>().data(), \
		.line = __LINE__, \
		.color = 0xFF000000 | (CORE_COMPILE_TIME_CRC32_STR(#tag) & 0x00FFFFFF) }

//NOLINTEND(readability-identifier-naming)
