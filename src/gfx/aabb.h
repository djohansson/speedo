#pragma once

#include "glm_extra.h"

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>

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
	explicit AABB(const AABB<U, N>& other) noexcept
		: myMin(static_cast<VectorType>(other.myMin))
		, myMax(static_cast<VectorType>(other.myMax))
	{}

	template <typename U = ScalarType>
	AABB(const OtherVectorType<U>& pmin, const OtherVectorType<U>& pmax) noexcept
		: myMin(static_cast<VectorType>(pmin))
		, myMax(static_cast<VectorType>(pmax))
	{}

	AABB(const std::array<ScalarType, N>& pmin, const std::array<ScalarType, N>& pmax) noexcept
		: myMin(glm::vec<N, ScalarType, glm::defaultp>(std::apply(pmin)))
		, myMax(glm::vec<N, ScalarType, glm::defaultp>(std::apply(pmax)))
	{}

	[[nodiscard]] explicit operator bool() const noexcept { return (myMax < myMin) == 0; }

	[[nodiscard]] const auto& GetMin() const noexcept { return myMin; }
	[[nodiscard]] const auto& GetMax() const noexcept { return myMax; }

	template <typename U = ScalarType>
	void SetMin(const OtherVectorType<U>& point) noexcept
	{
		myMin = point;
	}

	template <typename U = ScalarType>
	void SetMax(const OtherVectorType<U>& point) noexcept
	{
		myMax = point;
	}

	template <typename U = ScalarType>
	[[nodiscard]] bool Contains(const OtherVectorType<U>& point, const ScalarType epsilon = ScalarType(0)) const noexcept
	{
		return ((point > (myMax + VectorType(epsilon))) == 0) &&
			   ((point < (myMin - VectorType(epsilon))) == 0);
	}

	template <typename U = ScalarType>
	[[nodiscard]] bool Contains(const AABB<U, N>& other, const ScalarType epsilon = ScalarType(0)) const noexcept
	{
		return ((other.myMax > (myMax + VectorType(epsilon))) == 0) &&
			   ((other.myMin < (myMin - VectorType(epsilon))) == 0);
	}

	template <typename U = ScalarType>
	[[nodiscard]] bool ContainsExclusive(
		const OtherVectorType<U>& point, const ScalarType epsilon = ScalarType(0)) const noexcept
	{
		return ((point >= (myMax + VectorType(epsilon))) == 0) &&
			   ((point <= (myMin - VectorType(epsilon))) == 0);
	}

	template <typename U = ScalarType>
	[[nodiscard]] bool ContainsExclusive(const AABB<U, N>& other, const ScalarType epsilon = ScalarType(0)) const noexcept
	{
		return ((other.myMax >= (myMax + VectorType(epsilon))) == 0) &&
			   ((other.myMin <= (myMin - VectorType(epsilon))) == 0);
	}

	template <typename U = ScalarType>
	[[nodiscard]] bool Intersects(const AABB<U, N>& other, const ScalarType epsilon = ScalarType(0)) const noexcept
	{
		return ((other.myMin >= (myMax + VectorType(epsilon))) == 0) &&
			   ((other.myMax <= (myMin - VectorType(epsilon))) == 0);
	}

	template <typename U = ScalarType>
	[[nodiscard]] bool Overlaps(const AABB<U, N>& other, const ScalarType epsilon = ScalarType(0)) const noexcept
	{
		return ((other.myMin > (myMax + VectorType(epsilon))) == 0) &&
			   ((other.myMax < (myMin - VectorType(epsilon))) == 0);
	}

	template <typename U = ScalarType>
	void Merge(const AABB<U, N>& other) noexcept
	{
		myMin = glm::min(myMin, static_cast<VectorType>(other.myMin));
		myMax = glm::max(myMax, static_cast<VectorType>(other.myMax));
	}

	template <typename U = ScalarType>
	void Merge(const OtherVectorType<U>& point) noexcept
	{
		myMin = glm::min(myMin, static_cast<VectorType>(point));
		myMax = glm::max(myMax, static_cast<VectorType>(point));
	}

	void Merge(const std::array<ScalarType, N> point) noexcept
	{
		auto pointVec = glm::vec(std::apply(point));
		
		myMin = glm::min(myMin, pointVec);
		myMax = glm::max(myMax, pointVec);
	}

	template <typename U = ScalarType>
	void MergeSphere(const OtherVectorType<U>& center, U radius) noexcept
	{
		myMin = glm::min(myMin, static_cast<VectorType>(center - OtherVectorType<U>(radius)));
		myMax = glm::max(myMax, static_cast<VectorType>(center + OtherVectorType<U>(radius)));
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
