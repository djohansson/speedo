#pragma once

#include <utility>

#include <glm/mat4x4.hpp>

namespace glm
{

// zpp::bits serializes glm's vectors and quaternions by their members, but a matrix's columns are a private member, which
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

} // namespace glm
