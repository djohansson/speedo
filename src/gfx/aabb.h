#pragma once

#include "glm_extra.h"

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>

template <typename T, size_t N>
struct AABB
{
	static constexpr size_t kDimension = N;
	static constexpr size_t kCornerCount = 1 << kDimension;
	static constexpr size_t kMaxDimension = 3;
	static constexpr double kHalf = 0.5;

	using ScalarType = T;
	using VectorType = glm::vec<kDimension, ScalarType, glm::defaultp>;
	using Type = AABB<ScalarType, kDimension>;

	template <typename U>
	using OtherVectorType = glm::vec<kDimension, U, glm::defaultp>;

	[[nodiscard]] constexpr operator bool() const noexcept { return (myMax < myMin) == 0; }//NOLINT(google-explicit-constructor)

	[[nodiscard]] constexpr const auto& GetMin() const noexcept { return myMin; }
	[[nodiscard]] constexpr const auto& GetMax() const noexcept { return myMax; }

	template <typename U = ScalarType>
	constexpr void SetMin(const OtherVectorType<U>& point) noexcept
	{
		myMin = point;
	}

	template <typename U = ScalarType>
	constexpr void SetMax(const OtherVectorType<U>& point) noexcept
	{
		myMax = point;
	}

	template <typename U = ScalarType>
	[[nodiscard]] constexpr bool Contains(const OtherVectorType<U>& point, const ScalarType epsilon = ScalarType(0)) const noexcept
	{
		return ((point > (myMax + VectorType(epsilon))) == 0) &&
			   ((point < (myMin - VectorType(epsilon))) == 0);
	}

	template <typename U = ScalarType>
	[[nodiscard]] constexpr bool Contains(const AABB<U, N>& other, const ScalarType epsilon = ScalarType(0)) const noexcept
	{
		return ((other.myMax > (myMax + VectorType(epsilon))) == 0) &&
			   ((other.myMin < (myMin - VectorType(epsilon))) == 0);
	}

	template <typename U = ScalarType>
	[[nodiscard]] constexpr bool ContainsExclusive(
		const OtherVectorType<U>& point, const ScalarType epsilon = ScalarType(0)) const noexcept
	{
		return ((point >= (myMax + VectorType(epsilon))) == 0) &&
			   ((point <= (myMin - VectorType(epsilon))) == 0);
	}

	template <typename U = ScalarType>
	[[nodiscard]] constexpr bool ContainsExclusive(const AABB<U, N>& other, const ScalarType epsilon = ScalarType(0)) const noexcept
	{
		return ((other.myMax >= (myMax + VectorType(epsilon))) == 0) &&
			   ((other.myMin <= (myMin - VectorType(epsilon))) == 0);
	}

	template <typename U = ScalarType>
	[[nodiscard]] constexpr bool Intersects(const AABB<U, N>& other, const ScalarType epsilon = ScalarType(0)) const noexcept
	{
		return ((other.myMin >= (myMax + VectorType(epsilon))) == 0) &&
			   ((other.myMax <= (myMin - VectorType(epsilon))) == 0);
	}

	template <typename U = ScalarType>
	[[nodiscard]] constexpr bool Overlaps(const AABB<U, N>& other, const ScalarType epsilon = ScalarType(0)) const noexcept
	{
		return ((other.myMin > (myMax + VectorType(epsilon))) == 0) &&
			   ((other.myMax < (myMin - VectorType(epsilon))) == 0);
	}

	template <typename U = ScalarType>
	constexpr void Merge(const AABB<U, N>& other) noexcept
	{
		myMin = glm::min(myMin, static_cast<VectorType>(other.myMin));
		myMax = glm::max(myMax, static_cast<VectorType>(other.myMax));
	}

	template <typename U = ScalarType>
	constexpr void Merge(const OtherVectorType<U>& point) noexcept
	{
		myMin = glm::min(myMin, static_cast<VectorType>(point));
		myMax = glm::max(myMax, static_cast<VectorType>(point));
	}

	constexpr void Merge(const std::array<ScalarType, N> point) noexcept
	{
		auto pointVec = std::make_from_tuple<VectorType>(point);
		
		myMin = glm::min(myMin, pointVec);
		myMax = glm::max(myMax, pointVec);
	}

	template <typename U = ScalarType>
	constexpr void MergeSphere(const OtherVectorType<U>& center, U radius) noexcept
	{
		myMin = glm::min(myMin, static_cast<VectorType>(center - OtherVectorType<U>(radius)));
		myMax = glm::max(myMax, static_cast<VectorType>(center + OtherVectorType<U>(radius)));
	}

	[[nodiscard]] constexpr VectorType Center() const noexcept{ return (myMax + myMin) * ScalarType(kHalf); }
	[[nodiscard]] constexpr VectorType Size() const noexcept{ return myMax - myMin; }
	[[nodiscard]] constexpr ScalarType Radius() const noexcept{ return glm::length(myMax - myMin) * ScalarType(kHalf); }

	template <typename U = VectorType>
	[[nodiscard]] constexpr auto Corners() const noexcept
	{
		std::array<U, kCornerCount> somePointsOut;

		for (size_t i = 0; i < kCornerCount; i++)
			for (size_t j = 0; j < N; j++)
				somePointsOut[i][j] = (i & (1 << j)) ? myMax[j] : myMin[j];

		return somePointsOut;
	}
	
	VectorType myMin{};
	VectorType myMax{};
};

using AABB2d = AABB<double, 2>;
using AABB2f = AABB<float, 2>;
using AABB3d = AABB<double, 3>;
using AABB3f = AABB<float, 3>;
