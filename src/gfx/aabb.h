#pragma once

#include "glm_extra.h"

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

	static constexpr size_t kCornerCount = 1 << N;

public:
	friend zpp::bits::access;

	constexpr explicit AABB() noexcept
		: myMin(std::numeric_limits<ScalarType>::max())
		, myMax(std::numeric_limits<ScalarType>::lowest())
	{}

	template <typename U = ScalarType>
	AABB(const AABB<U, N>& other) noexcept
		: myMin(static_cast<VectorType>(other.myMin))
		, myMax(static_cast<VectorType>(other.myMax))
	{}

	template <typename U = ScalarType>
	AABB(const OtherVectorType<U>& aMin, const OtherVectorType<U>& aMax) noexcept
		: myMin(static_cast<VectorType>(aMin))
		, myMax(static_cast<VectorType>(aMax))
	{}

	AABB(const ScalarType aMin[N], const ScalarType aMax[N]) noexcept
		: myMin(glm::make_vec3(aMin))
		, myMax(glm::make_vec3(aMax))
	{
		static_assert(N == 3, "Only 3 dimensions is supported.");
	}

	[[nodiscard]] operator bool() const noexcept { return (myMax < myMin) == 0; }
	
	[[nodiscard]] const auto& GetMin() const noexcept { return myMin; }
	[[nodiscard]] const auto& GetMax() const noexcept { return myMax; }

	template <typename U = ScalarType>
	void SetMin(const OtherVectorType<U>& aPoint) noexcept
	{
		myMin = aPoint;
	}

	template <typename U = ScalarType>
	void SetMax(const OtherVectorType<U>& aPoint) noexcept
	{
		myMax = aPoint;
	}

	template <typename U = ScalarType>
	[[nodiscard]] bool Contains(const OtherVectorType<U>& aPoint, const ScalarType aEpsilon = ScalarType(0)) const noexcept
	{
		return ((aPoint > (myMax + VectorType(aEpsilon))) == 0) &&
			   ((aPoint < (myMin - VectorType(aEpsilon))) == 0);
	}

	template <typename U = ScalarType>
	[[nodiscard]] bool Contains(const AABB<U, N>& anOther, const ScalarType aEpsilon = ScalarType(0)) const noexcept
	{
		return ((anOther.myMax > (myMax + VectorType(aEpsilon))) == 0) &&
			   ((anOther.myMin < (myMin - VectorType(aEpsilon))) == 0);
	}

	template <typename U = ScalarType>
	[[nodiscard]] bool ContainsExclusive(
		const OtherVectorType<U>& aPoint, const ScalarType aEpsilon = ScalarType(0)) const noexcept
	{
		return ((aPoint >= (myMax + VectorType(aEpsilon))) == 0) &&
			   ((aPoint <= (myMin - VectorType(aEpsilon))) == 0);
	}

	template <typename U = ScalarType>
	[[nodiscard]] bool ContainsExclusive(const AABB<U, N>& anOther, const ScalarType aEpsilon = ScalarType(0)) const noexcept
	{
		return ((anOther.myMax >= (myMax + VectorType(aEpsilon))) == 0) &&
			   ((anOther.myMin <= (myMin - VectorType(aEpsilon))) == 0);
	}

	template <typename U = ScalarType>
	[[nodiscard]] bool Intersects(const AABB<U, N>& anOther, const ScalarType aEpsilon = ScalarType(0)) const noexcept
	{
		return ((anOther.myMin >= (myMax + VectorType(aEpsilon))) == 0) &&
			   ((anOther.myMax <= (myMin - VectorType(aEpsilon))) == 0);
	}

	template <typename U = ScalarType>
	[[nodiscard]] bool Overlaps(const AABB<U, N>& anOther, const ScalarType aEpsilon = ScalarType(0)) const noexcept
	{
		return ((anOther.myMin > (myMax + VectorType(aEpsilon))) == 0) &&
			   ((anOther.myMax < (myMin - VectorType(aEpsilon))) == 0);
	}

	template <typename U = ScalarType>
	void Merge(const AABB<U, N>& anOther) noexcept
	{
		myMin = glm::min(myMin, static_cast<VectorType>(anOther.myMin));
		myMax = glm::max(myMax, static_cast<VectorType>(anOther.myMax));
	}

	template <typename U = ScalarType>
	void Merge(const OtherVectorType<U>& aPoint) noexcept
	{
		myMin = glm::min(myMin, static_cast<VectorType>(aPoint));
		myMax = glm::max(myMax, static_cast<VectorType>(aPoint));
	}

	void Merge(const ScalarType aPoint[N]) noexcept
	{
		static_assert(N == 3, "Only 3 dimensions is supported.");

		auto aPointVec = glm::make_vec3(aPoint);
		
		myMin = glm::min(myMin, aPointVec);
		myMax = glm::max(myMax, aPointVec);
	}

	template <typename U = ScalarType>
	void MergeSphere(const OtherVectorType<U>& aCenter, U aRadius) noexcept
	{
		myMin = glm::min(myMin, static_cast<VectorType>(aCenter - OtherVectorType<U>(aRadius)));
		myMax = glm::max(myMax, static_cast<VectorType>(aCenter + OtherVectorType<U>(aRadius)));
	}

	[[nodiscard]] VectorType Center() const noexcept{ return (myMax + myMin) * ScalarType(0.5); }
	[[nodiscard]] VectorType Size() const noexcept{ return (myMax - myMin); }
	[[nodiscard]] ScalarType Radius() const noexcept{ return glm::length(myMax - myMin) * ScalarType(0.5); }

	template <typename U = VectorType>
	[[nodiscard]] auto Corners() const noexcept
	{
		std::array<U, kCornerCount> somePointsOut;

		for (size_t i = 0; i < kCornerCount; i++)
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
