#pragma once

#include <atomic>
#include <utility>
#include <vector>

template <typename T, typename ContainerT = std::vector<T>>
class CircularContainer : private ContainerT
{
public:
	explicit CircularContainer() noexcept = default;
	explicit CircularContainer(ContainerT&& other) noexcept : ContainerT(std::forward<ContainerT>(other)) {}
	CircularContainer(const CircularContainer&) = delete;
	CircularContainer(CircularContainer&& other) noexcept = default;
	~CircularContainer() = default;

	[[maybe_unused]] CircularContainer& operator=(const CircularContainer&) = delete;
	[[maybe_unused]] CircularContainer& operator=(CircularContainer&&) noexcept = default;
	[[maybe_unused]] CircularContainer& operator=(const ContainerT& other) = delete;
	[[maybe_unused]] CircularContainer& operator=(ContainerT&& other) noexcept { ContainerT::operator=(std::forward<ContainerT>(other)); return *this; }

	[[nodiscard]] size_t Capacity() const noexcept { return ContainerT::size(); }
	[[nodiscard]] size_t Head() const noexcept { return std::atomic_ref(myHead).load(std::memory_order_relaxed); }
	[[nodiscard]] bool Empty() const noexcept { return ContainerT::empty(); }

	[[nodiscard]] T& Get() { return ContainerT::at(std::atomic_ref(myHead).load(std::memory_order_relaxed)); }
	[[nodiscard]] const T& Get() const { return ContainerT::at(std::atomic_ref(myHead).load(std::memory_order_relaxed)); }

	[[nodiscard]] T& FetchAdd(size_t offset = 1u)
	{
		auto head = std::atomic_ref(myHead);
		size_t index = head.fetch_add(offset, std::memory_order_relaxed);
		size_t newHeadWithOverflow = index + offset;
		size_t newHeadModulo = newHeadWithOverflow % ContainerT::size();
		while (!head.compare_exchange_weak(newHeadWithOverflow, newHeadModulo, std::memory_order_acq_rel))
			newHeadModulo = newHeadWithOverflow % ContainerT::size();
		
		return ContainerT::at(index);
	}

	using ContainerT::begin;
	using ContainerT::end;
	using ContainerT::cbegin;
	using ContainerT::cend;

private:
	size_t myHead{};
};
