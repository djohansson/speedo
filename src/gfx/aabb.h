#pragma once

#include "glm.h"

#include <algorithm>
#include <array>
#include <limits>

template <typename T, size_t N = 3>
class AABB
{
	using ScalarType = T;
	using VectorType = glm::vec<N, T, glm::defaultp>;
	using Type = AABB<T, N>;

	template <typename U>
	using OtherVectorType = glm::vec<N, U, glm::defaultp>;

	static constexpr size_t CornerCount = 1 << N;

public:
	friend zpp::bits::access;

	constexpr AABB() noexcept
		: myMin(std::numeric_limits<ScalarType>::max())
		, myMax(std::numeric_limits<ScalarType>::lowest())
	{}

	template <typename U = ScalarType>
	AABB(const AABB<U, N>& other)
		: myMin(static_cast<VectorType>(other.myMin))
		, myMax(static_cast<VectorType>(other.myMax))
	{}

	template <typename U = ScalarType>
	AABB(const OtherVectorType<U>& aMin, const OtherVectorType<U>& aMax)
		: myMin(static_cast<VectorType>(aMin))
		, myMax(static_cast<VectorType>(aMax))
	{}

	operator bool() const { return (myMax < myMin) == 0; }

	bool operator!() const { return !static_cast<bool>(*this); }

	const auto& getMin() const { return myMin; }
	const auto& getMax() const { return myMax; }

	template <typename U = ScalarType>
	void setMin(const OtherVectorType<U>& aPoint)
	{
		myMin = aPoint;
	}

	template <typename U = ScalarType>
	void setMax(const OtherVectorType<U>& aPoint)
	{
		myMax = aPoint;
	}

	template <typename U = ScalarType>
	bool contains(const OtherVectorType<U>& aPoint, const ScalarType aEpsilon = ScalarType(0)) const
	{
		return ((aPoint > (myMax + VectorType(aEpsilon))) == 0) &&
			   ((aPoint < (myMin - VectorType(aEpsilon))) == 0);
	}

	template <typename U = ScalarType>
	bool contains(const AABB<U, N>& anOther, const ScalarType aEpsilon = ScalarType(0)) const
	{
		return ((anOther.myMax > (myMax + VectorType(aEpsilon))) == 0) &&
			   ((anOther.myMin < (myMin - VectorType(aEpsilon))) == 0);
	}

	template <typename U = ScalarType>
	bool containsExclusive(
		const OtherVectorType<U>& aPoint, const ScalarType aEpsilon = ScalarType(0)) const
	{
		return ((aPoint >= (myMax + VectorType(aEpsilon))) == 0) &&
			   ((aPoint <= (myMin - VectorType(aEpsilon))) == 0);
	}

	template <typename U = ScalarType>
	bool
	containsExclusive(const AABB<U, N>& anOther, const ScalarType aEpsilon = ScalarType(0)) const
	{
		return ((anOther.myMax >= (myMax + VectorType(aEpsilon))) == 0) &&
			   ((anOther.myMin <= (myMin - VectorType(aEpsilon))) == 0);
	}

	template <typename U = ScalarType>
	bool intersects(const AABB<U, N>& anOther, const ScalarType aEpsilon = ScalarType(0)) const
	{
		return ((anOther.myMin >= (myMax + VectorType(aEpsilon))) == 0) &&
			   ((anOther.myMax <= (myMin - VectorType(aEpsilon))) == 0);
	}

	template <typename U = ScalarType>
	bool overlaps(const AABB<U, N>& anOther, const ScalarType aEpsilon = ScalarType(0)) const
	{
		return ((anOther.myMin > (myMax + VectorType(aEpsilon))) == 0) &&
			   ((anOther.myMax < (myMin - VectorType(aEpsilon))) == 0);
	}

	template <typename U = ScalarType>
	void merge(const AABB<U, N>& anOther)
	{
		myMin = glm::min(myMin, static_cast<VectorType>(anOther.myMin));
		myMax = glm::max(myMax, static_cast<VectorType>(anOther.myMax));
	}

	template <typename U = ScalarType>
	void merge(const OtherVectorType<U>& aPoint)
	{
		myMin = glm::min(myMin, static_cast<VectorType>(aPoint));
		myMax = glm::max(myMax, static_cast<VectorType>(aPoint));
	}

	template <typename U = ScalarType>
	void mergeSphere(const OtherVectorType<U>& aCenter, U aRadius)
	{
		myMin = glm::min(myMin, static_cast<VectorType>(aCenter - OtherVectorType<U>(aRadius)));
		myMax = glm::max(myMax, static_cast<VectorType>(aCenter + OtherVectorType<U>(aRadius)));
	}

	VectorType center() const { return (myMax + myMin) * ScalarType(0.5); }

	VectorType size() const { return (myMax - myMin); }

	ScalarType radius() const { return glm::length(myMax - myMin) * ScalarType(0.5); }

	template <typename U = VectorType>
	std::array<U, CornerCount> corners() const
	{
		std::array<U, CornerCount> somePointsOut;

		for (size_t i = 0; i < CornerCount; i++)
			for (size_t j = 0; j < N; j++)
				somePointsOut[i][j] = (i & (1 << j)) ? myMax[j] : myMin[j];

		return somePointsOut;
	}

private:
	VectorType myMin{};
	VectorType myMax{};
};

using AABB2d = AABB<double, 2>;
using AABB2f = AABB<float, 2>;
using AABB3d = AABB<double, 3>;
using AABB3f = AABB<float, 3>;
