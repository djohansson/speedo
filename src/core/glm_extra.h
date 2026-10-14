#pragma once

#include <utility>

#include <glm/ext/quaternion_float.hpp>
#include <glm/mat4x4.hpp>

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
