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

#include <cereal/cereal.hpp>

namespace glm
{
	template<typename Archive> void serialize(Archive& archive, glm::vec2& v) { archive(v.x, v.y); }
	template<typename Archive> void serialize(Archive& archive, glm::vec3& v) { archive(v.x, v.y, v.z); }
	template<typename Archive> void serialize(Archive& archive, glm::vec4& v) { archive(v.x, v.y, v.z, v.w); }
	template<typename Archive> void serialize(Archive& archive, glm::ivec2& v) { archive(v.x, v.y); }
	template<typename Archive> void serialize(Archive& archive, glm::ivec3& v) { archive(v.x, v.y, v.z); }
	template<typename Archive> void serialize(Archive& archive, glm::ivec4& v) { archive(v.x, v.y, v.z, v.w); }
	template<typename Archive> void serialize(Archive& archive, glm::uvec2& v) { archive(v.x, v.y); }
	template<typename Archive> void serialize(Archive& archive, glm::uvec3& v) { archive(v.x, v.y, v.z); }
	template<typename Archive> void serialize(Archive& archive, glm::uvec4& v) { archive(v.x, v.y, v.z, v.w); }
	template<typename Archive> void serialize(Archive& archive, glm::dvec2& v) { archive(v.x, v.y); }
	template<typename Archive> void serialize(Archive& archive, glm::dvec3& v) { archive(v.x, v.y, v.z); }
	template<typename Archive> void serialize(Archive& archive, glm::dvec4& v) { archive(v.x, v.y, v.z, v.w); }

	template<typename Archive> void serialize(Archive& archive, glm::mat2& m) { archive(m[0], m[1]); }
	template<typename Archive> void serialize(Archive& archive, glm::dmat2& m) { archive(m[0], m[1]); }
	template<typename Archive> void serialize(Archive& archive, glm::mat3& m) { archive(m[0], m[1], m[2]); }
	template<typename Archive> void serialize(Archive& archive, glm::mat4& m) { archive(m[0], m[1], m[2], m[3]); }
	template<typename Archive> void serialize(Archive& archive, glm::dmat4& m) { archive(m[0], m[1], m[2], m[3]); }

	template<typename Archive> void serialize(Archive& archive, glm::quat& q) { archive(q.x, q.y, q.z, q.w); }
	template<typename Archive> void serialize(Archive& archive, glm::dquat& q) { archive(q.x, q.y, q.z, q.w); }
}
