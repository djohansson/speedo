#pragma once

//#define GLM_FORCE_MESSAGES
#define GLM_LANG_STL11_FORCED
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/hash.hpp>
//#include <glm/gtx/matrix_interpolation.hpp>


namespace glm
{

constexpr auto serialize(auto& archive, vec3& v)
{
	return archive(v.x, v.y, v.z);
}

constexpr auto serialize(auto& archive, const vec3& v)
{
	return archive(v.x, v.y, v.z);
}

} // namespace glm