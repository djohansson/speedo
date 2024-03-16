#pragma once

#include "copyableatomic.h"

#include <vector>

template <typename T, typename ContainerT = std::vector<T>>
class CircularContainer : public ContainerT
{
public:
	T& get() { return ContainerT::at(myHead); }
	const T& get() const { return ContainerT::at(myHead); }

	T& fetchAdd(size_t offset = 1u)
	{
		size_t index = myHead.fetch_add(offset, std::memory_order_relaxed);
		size_t newHeadWithOverflow = myHead;
		size_t newHeadModulo = newHeadWithOverflow % ContainerT::size();
		while (!myHead.compare_exchange_weak(newHeadWithOverflow, newHeadModulo, std::memory_order_relaxed))
			newHeadModulo = newHeadWithOverflow % ContainerT::size();
		
		return ContainerT::at(index);
	}

private:
	CopyableAtomic<size_t> myHead{};
};
