#pragma once

#include <core/math.h>

#include <utility>

#include <glm/common.hpp>
#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/quaternion_geometric.hpp>
#include <glm/ext/scalar_constants.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>

namespace core
{

// glm's functions that compute with the platform's math functions, as glm computes them, with core's (core/math.h):
// glm::perspective (perspectiveRH_NO, glm/ext/matrix_clip_space.inl), rotate (from the identity,
// glm/ext/matrix_transform.inl), angleAxis (glm/ext/quaternion_trigonometric.inl) and slerp
// (glm/ext/quaternion_common.inl) aren't called

// gl's perspective projection (right handed, depth -1 to 1, as glm's defaults): fovY radians
template <typename T>
[[nodiscard]] glm::mat<4, 4, T> Perspective(T fovY, T aspect, T zNear, T zFar) noexcept
{
	const T tanHalfFovY = Tan(fovY / static_cast<T>(2));
	glm::mat<4, 4, T> result(static_cast<T>(0));
	result[0][0] = static_cast<T>(1) / (aspect * tanHalfFovY);
	result[1][1] = static_cast<T>(1) / tanHalfFovY;
	result[2][2] = -(zFar + zNear) / (zFar - zNear);
	result[2][3] = -static_cast<T>(1);
	result[3][2] = -(static_cast<T>(2) * zFar * zNear) / (zFar - zNear);
	return result;
}

// the rotation by angle radians about axis (glm::rotate of the identity)
template <typename T, glm::qualifier Q>
[[nodiscard]] glm::mat<4, 4, T, Q> Rotation(T angle, const glm::vec<3, T, Q>& axis) noexcept
{
	const T cosine = Cos(angle);
	const T sine = Sin(angle);
	const glm::vec<3, T, Q> unit = glm::normalize(axis);
	const glm::vec<3, T, Q> temp = (static_cast<T>(1) - cosine) * unit;
	glm::mat<4, 4, T, Q> result(static_cast<T>(1));
	result[0][0] = cosine + temp[0] * unit[0];
	result[0][1] = temp[0] * unit[1] + sine * unit[2];
	result[0][2] = temp[0] * unit[2] - sine * unit[1];
	result[1][0] = temp[1] * unit[0] - sine * unit[2];
	result[1][1] = cosine + temp[1] * unit[1];
	result[1][2] = temp[1] * unit[2] + sine * unit[0];
	result[2][0] = temp[2] * unit[0] + sine * unit[1];
	result[2][1] = temp[2] * unit[1] - sine * unit[0];
	result[2][2] = cosine + temp[2] * unit[2];
	return result;
}

// the quaternion of the rotation by angle radians about a unit axis
template <typename T, glm::qualifier Q>
[[nodiscard]] glm::qua<T, Q> AngleAxis(T angle, const glm::vec<3, T, Q>& axis) noexcept
{
	const T half = angle * static_cast<T>(0.5);
	return glm::qua<T, Q>(Cos(half), axis * Sin(half));
}

// spherical interpolation from one unit quaternion to another (the short way), linear where they are nearly the same
template <typename T, glm::qualifier Q>
[[nodiscard]] glm::qua<T, Q> Slerp(const glm::qua<T, Q>& start, const glm::qua<T, Q>& end, T amount) noexcept
{
	glm::qua<T, Q> target = end;
	T cosTheta = glm::dot(start, end);
	if (cosTheta < static_cast<T>(0))
	{
		target = -end;
		cosTheta = -cosTheta;
	}
	if (cosTheta > static_cast<T>(1) - glm::epsilon<T>())
		return glm::qua<T, Q>(
			glm::mix(start.w, target.w, amount), glm::mix(start.x, target.x, amount), glm::mix(start.y, target.y, amount),
			glm::mix(start.z, target.z, amount));
	const T angle = Acos(cosTheta);
	return (Sin((static_cast<T>(1) - amount) * angle) * start + Sin(amount * angle) * target) / Sin(angle);
}

} // namespace core

namespace glm
{

// zpp::bits serializes glm's vectors by their members, but a matrix's columns are a private member, which
// it can't bind: matrices are serialized column by column (found by argument dependent lookup)
template <typename Archive, length_t C, length_t R, typename T, qualifier Q>
constexpr auto serialize(Archive& archive, mat<C, R, T, Q>& matrix)
{
	return [&]<length_t... I>(std::integer_sequence<length_t, I...>) { return archive(matrix[I]...); }(
		std::make_integer_sequence<length_t, C>{});
}

template <typename Archive, length_t C, length_t R, typename T, qualifier Q>
constexpr auto serialize(Archive& archive, const mat<C, R, T, Q>& matrix)
{
	return [&]<length_t... I>(std::integer_sequence<length_t, I...>) { return archive(matrix[I]...); }(
		std::make_integer_sequence<length_t, C>{});
}

// GLM_FORCE_XYZW_ONLY keeps the vectors' members plain, but not a quaternion's: with the language extensions glm detects
// (_MSC_EXTENSIONS, which clang defines on Windows, or clang and gcc with simd) they are an anonymous union, which
// structured bindings can't bind. Serialized by its components, in the order of its members (x, y, z, w)
template <typename Archive, typename T, qualifier Q>
constexpr auto serialize(Archive& archive, qua<T, Q>& quaternion)
{
	return archive(quaternion.x, quaternion.y, quaternion.z, quaternion.w);
}

template <typename Archive, typename T, qualifier Q>
constexpr auto serialize(Archive& archive, const qua<T, Q>& quaternion)
{
	return archive(quaternion.x, quaternion.y, quaternion.z, quaternion.w);
}

} // namespace glm
