#pragma once

#include <core/std_extra.h>

//#define GLM_FORCE_MESSAGES
#define GLM_LANG_STL11_FORCED
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/hash.hpp>
//#include <glm/gtx/matrix_interpolation.hpp>

#include <glaze/glaze.hpp>

#include <zpp_bits.h>

//NOLINTBEGIN(readability-identifier-naming, readability-magic-numbers)

template <>
struct glz::meta<glm::vec2>
{
	using T = glm::vec2;
	static constexpr auto value = object(&T::x, &T::y);
};

[[nodiscard]] zpp::bits::members<2/*std_extra::member_count<glm::vec2>()*/> serialize(const glm::vec2&);
[[nodiscard]] zpp::bits::members<3/*std_extra::member_count<glm::vec3>()*/> serialize(const glm::vec3&);

namespace glm
{

[[nodiscard]] constexpr auto serialize(auto& archive, vec3& value)
{
	return archive(value.x, value.y, value.z);
}

[[nodiscard]] constexpr auto serialize(auto& archive, const vec3& value)
{
	return archive(value.x, value.y, value.z);
}

} // namespace glm

//NOLINTEND(readability-identifier-naming, readability-magic-numbers)
