#pragma once

#include "copyableatomic.h"

#include <vector>

template <typename T, typename ContainerT = std::vector<T>>
class CircularContainer : public ContainerT
{
public:
	[[nodiscard]] T& Get() { return ContainerT::at(myHead.load(std::memory_order_relaxed)); }
	[[nodiscard]] const T& Get() const { return ContainerT::at(myHead.load(std::memory_order_relaxed)); }

	[[nodiscard]] T& FetchAdd(size_t offset = 1u)
	{
		size_t index = myHead.fetch_add(offset, std::memory_order_relaxed);
		size_t newHeadWithOverflow = index + offset;
		size_t newHeadModulo = newHeadWithOverflow % ContainerT::size();
		while (!myHead.compare_exchange_weak(newHeadWithOverflow, newHeadModulo, std::memory_order_acq_rel))
			newHeadModulo = newHeadWithOverflow % ContainerT::size();
		
		return ContainerT::at(index);
	}

private:
	CopyableAtomic<size_t> myHead{};
};
