#include <gfx/sceneanimation.h>

#include <gfx/shaders/capi.h>

#include <core/profiling.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gfx
{

namespace sceneanimation
{

struct Trs
{
	glm::vec3 translation{0.0F};
	glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F}; // wxyz
	glm::vec3 scale{1.0F};
};

[[nodiscard]] static glm::quat ToQuat(const float* xyzw)
{
	return glm::quat(xyzw[3], xyzw[0], xyzw[1], xyzw[2]);
}

// a channel's value at time, with components floats per key (3 or 4)
static void Sample(const SceneAnimationChannel& channel, float time, size_t components, float* out)
{
	const auto& times = channel.times;
	const auto& values = channel.values;
	bool cubic = channel.interpolation == SceneAnimationChannel::Interpolation::kCubicSpline;
	// a key's value: cubic splines store (in tangent, value, out tangent) per key
	auto value = [&](size_t key, size_t part) { return &values[((cubic ? (key * 3) + part : key) * components)]; };
	auto keyCount = times.size();
	if (keyCount == 0 || values.size() < keyCount * components * (cubic ? 3 : 1))
		return;

	if (keyCount == 1 || time <= times.front())
	{
		std::copy_n(value(0, 1), components, out);
		return;
	}
	if (time >= times.back())
	{
		std::copy_n(value(keyCount - 1, 1), components, out);
		return;
	}

	auto next = static_cast<size_t>(std::ranges::upper_bound(times, time) - times.begin());
	auto key = next - 1;
	auto delta = times[next] - times[key];
	auto t = delta > 0.0F ? (time - times[key]) / delta : 0.0F;

	switch (channel.interpolation)
	{
	case SceneAnimationChannel::Interpolation::kStep:
		std::copy_n(value(key, 1), components, out);
		break;
	case SceneAnimationChannel::Interpolation::kLinear:
		if (channel.path == SceneAnimationChannel::Path::kRotation)
		{
			auto q = glm::slerp(glm::normalize(ToQuat(value(key, 1))), glm::normalize(ToQuat(value(next, 1))), t);
			out[0] = q.x;
			out[1] = q.y;
			out[2] = q.z;
			out[3] = q.w;
		}
		else
		{
			for (size_t i = 0; i < components; i++)
				out[i] = std::lerp(value(key, 1)[i], value(next, 1)[i], t);
		}
		break;
	case SceneAnimationChannel::Interpolation::kCubicSpline:
	{
		// hermite, with the tangents scaled by the key interval (the gltf spec's appendix C)
		auto t2 = t * t;
		auto t3 = t2 * t;
		for (size_t i = 0; i < components; i++)
		{
			auto p0 = value(key, 1)[i];
			auto m0 = delta * value(key, 2)[i];
			auto p1 = value(next, 1)[i];
			auto m1 = delta * value(next, 0)[i];
			out[i] = ((2 * t3) - (3 * t2) + 1) * p0 + (t3 - (2 * t2) + t) * m0 + ((-2 * t3) + (3 * t2)) * p1 + (t3 - t2) * m1;
		}
		if (channel.path == SceneAnimationChannel::Path::kRotation)
		{
			auto q = glm::normalize(glm::quat(out[3], out[0], out[1], out[2]));
			out[0] = q.x;
			out[1] = q.y;
			out[2] = q.z;
			out[3] = q.w;
		}
		break;
	}
	}
}

} // namespace sceneanimation

// the nodes' local transforms with an animation at time (wrapped to its duration), or at rest if animation is out of
// range
[[nodiscard]] static std::vector<sceneanimation::Trs> EvaluateLocals(const SceneAnimationData& data, size_t animation, float time)
{
	using namespace sceneanimation;

	std::vector<Trs> locals(data.nodes.size());
	for (size_t nodeIt = 0; nodeIt < data.nodes.size(); nodeIt++)
	{
		const auto& node = data.nodes[nodeIt];
		locals[nodeIt] = {
			.translation = glm::make_vec3(node.translation.data()),
			.rotation = ToQuat(node.rotation.data()),
			.scale = glm::make_vec3(node.scale.data())};
	}

	if (animation < data.animations.size())
	{
		const auto& clip = data.animations[animation];
		auto clipTime = clip.duration > 0.0F ? std::fmod(std::max(time, 0.0F), clip.duration) : 0.0F;
		for (const auto& channel : clip.channels)
		{
			if (channel.node >= locals.size() || data.nodes[channel.node].hasMatrix)
				continue;

			auto& local = locals[channel.node];
			std::array<float, 4> value{};
			switch (channel.path)
			{
			case SceneAnimationChannel::Path::kTranslation:
				value = {local.translation.x, local.translation.y, local.translation.z, 0.0F};
				Sample(channel, clipTime, 3, value.data());
				local.translation = glm::make_vec3(value.data());
				break;
			case SceneAnimationChannel::Path::kRotation:
				value = {local.rotation.x, local.rotation.y, local.rotation.z, local.rotation.w};
				Sample(channel, clipTime, 4, value.data());
				local.rotation = glm::normalize(ToQuat(value.data()));
				break;
			case SceneAnimationChannel::Path::kScale:
				value = {local.scale.x, local.scale.y, local.scale.z, 0.0F};
				Sample(channel, clipTime, 3, value.data());
				local.scale = glm::make_vec3(value.data());
				break;
			}
		}
	}

	return locals;
}

// the nodes' world transforms from their local ones
[[nodiscard]] static std::vector<SceneMatrix> ComposeWorlds(const SceneAnimationData& data, const std::vector<sceneanimation::Trs>& locals)
{
	// world = parent's world * local, parents first (the nodes needn't be ordered)
	std::vector<glm::mat4> worlds(data.nodes.size());
	std::vector<uint8_t> done(data.nodes.size(), 0);
	std::vector<size_t> chain;
	for (size_t nodeIt = 0; nodeIt < data.nodes.size(); nodeIt++)
	{
		for (auto it = static_cast<int64_t>(nodeIt); it >= 0 && done[it] == 0; it = data.nodes[it].parent)
		{
			if (std::ranges::find(chain, static_cast<size_t>(it)) != chain.end())
				break; // a cycle, which a valid file doesn't have
			chain.push_back(static_cast<size_t>(it));
		}
		for (auto it = chain.rbegin(); it != chain.rend(); ++it)
		{
			const auto& node = data.nodes[*it];
			const auto& local = locals[*it];
			glm::mat4 matrix = node.hasMatrix ? glm::make_mat4(node.matrix.data())
											  : glm::translate(glm::mat4(1.0F), local.translation) * glm::mat4_cast(local.rotation) *
													glm::scale(glm::mat4(1.0F), local.scale);
			worlds[*it] = node.parent >= 0 && done[node.parent] != 0 ? worlds[node.parent] * matrix : matrix;
			done[*it] = 1;
		}
		chain.clear();
	}

	std::vector<SceneMatrix> result(worlds.size());
	for (size_t nodeIt = 0; nodeIt < worlds.size(); nodeIt++)
		std::memcpy(result[nodeIt].data(), glm::value_ptr(worlds[nodeIt]), sizeof(SceneMatrix));
	return result;
}

std::vector<SceneMatrix> EvaluateNodes(const SceneAnimationData& data, size_t animation, float time)
{
	ZoneScopedN("gfx::EvaluateNodes");

	return ComposeWorlds(data, EvaluateLocals(data, animation, time));
}

std::vector<SceneMatrix> EvaluateNodes(const SceneAnimationData& data, const ScenePose& pose, const ScenePose& from, float weight)
{
	ZoneScopedN("gfx::EvaluateNodes");

	auto rest = data.animations.size();
	auto locals = EvaluateLocals(data, pose.animation.value_or(rest), pose.time);
	if (weight < 1.0F)
	{
		// each node's translation and scale lerped, and its rotation slerped, from the pose faded out
		auto fromLocals = EvaluateLocals(data, from.animation.value_or(rest), from.time);
		auto t = std::clamp(weight, 0.0F, 1.0F);
		for (size_t nodeIt = 0; nodeIt < locals.size(); nodeIt++)
		{
			auto& local = locals[nodeIt];
			const auto& fromLocal = fromLocals[nodeIt];
			local.translation = glm::mix(fromLocal.translation, local.translation, t);
			local.rotation = glm::slerp(fromLocal.rotation, local.rotation, t);
			local.scale = glm::mix(fromLocal.scale, local.scale, t);
		}
	}
	return ComposeWorlds(data, locals);
}

void WriteInstances(const SceneAnimationData& data, std::span<const SceneMatrix> worlds, std::span<std::byte> modelInstances)
{
	ZoneScopedN("gfx::WriteInstances");

	for (const auto& link : data.instanceLinks)
	{
		if (link.node >= worlds.size() || (link.instance + 1) * sizeof(ModelInstance) > modelInstances.size())
			continue;
		auto transform = glm::make_mat4(worlds[link.node].data()) * glm::make_mat4(link.local.data());
		auto inverseTranspose = glm::transpose(glm::inverse(transform));
		ModelInstance instance;
		std::memcpy(&instance.modelTransform[0][0], glm::value_ptr(transform), sizeof(SceneMatrix));
		std::memcpy(&instance.inverseTransposeModelTransform[0][0], glm::value_ptr(inverseTranspose), sizeof(SceneMatrix));
		std::memcpy(modelInstances.data() + (link.instance * sizeof(ModelInstance)), &instance, sizeof(ModelInstance));
	}
}

std::vector<SceneMatrix> RestJoints(const SceneAnimationData& data)
{
	std::vector<SceneMatrix> joints(data.jointCount, SceneMatrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
	WriteJoints(data, EvaluateNodes(data, data.animations.size(), 0.0F), joints);
	return joints;
}

std::array<float, 3> SkinPosition(
	std::span<const SceneMatrix> joints, uint32_t jointBase, const SkinVertex& skin, const std::array<float, 3>& position)
{
	glm::vec4 result(0.0F);
	auto p = glm::vec4(position[0], position[1], position[2], 1.0F);
	for (uint32_t i = 0; i < 4; i++)
	{
		auto shift = (i % 2) * 16;
		auto joint = (skin.joints[i / 2] >> shift) & 0xffffU;
		auto weight = static_cast<float>((skin.weights[i / 2] >> shift) & 0xffffU) / 65535.0F;
		if (weight > 0.0F && jointBase + joint < joints.size())
			result += weight * (glm::make_mat4(joints[jointBase + joint].data()) * p);
	}
	return {result.x, result.y, result.z};
}

void WriteJoints(const SceneAnimationData& data, std::span<const SceneMatrix> worlds, std::span<SceneMatrix> joints)
{
	ZoneScopedN("gfx::WriteJoints");

	for (const auto& skin : data.skins)
	{
		for (size_t jointIt = 0; jointIt < skin.joints.size(); jointIt++)
		{
			auto index = skin.jointBase + jointIt;
			if (index >= joints.size() || skin.joints[jointIt] >= worlds.size())
				continue;
			auto inverseBind = jointIt < skin.inverseBindMatrices.size() ? glm::make_mat4(skin.inverseBindMatrices[jointIt].data())
																		  : glm::mat4(1.0F);
			auto joint = glm::make_mat4(worlds[skin.joints[jointIt]].data()) * inverseBind;
			std::memcpy(joints[index].data(), glm::value_ptr(joint), sizeof(SceneMatrix));
		}
	}
}

} // namespace gfx
