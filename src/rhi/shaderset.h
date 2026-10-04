#pragma once

#include <rhi/enums.h>

#include <core/utils.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// compiled shaders and the resources they use, in rhi's neutral terms: what a shader compiler (see gfx::ShaderLoader)
// produces, and Pipeline::CreateLayout takes
namespace rhi
{

// the shader binary format the build's graphics api takes
enum class ShaderFormat : uint8_t
{
	kSpirv16, // spir-v 1.6
};

using ShaderBinary = std::vector<char>;

struct ComputeLaunchParameters
{
	std::array<uint64_t, 3> threadGroupSize;
	uint64_t waveSize{};
};

using EntryPoint = std::tuple<std::string, ShaderStage, std::optional<ComputeLaunchParameters>>;

using Shader = std::tuple<ShaderBinary, EntryPoint>;

// a resource binding of a descriptor set
struct ShaderBinding
{
	uint32_t binding = 0;
	DescriptorType type{};
	uint32_t count = 1; // array size, or the size in bytes for an inline uniform block
	ShaderStage stages{};
	std::string name; // the shader variable, as Pipeline::SetDescriptorData takes it
	uint64_t nameHash = 0; // XXH3_64bits of name
};

struct ShaderPushConstants
{
	ShaderStage stages{};
	uint32_t offset = 0;
	uint32_t size = 0;
};

// a descriptor set's bindings, and the push constants declared with it
struct ShaderSetLayout
{
	std::vector<ShaderBinding> bindings;
	std::optional<ShaderPushConstants> pushConstants;
};

struct ShaderSet
{
	std::vector<Shader> shaders;
	core::UnorderedMap<uint32_t, ShaderSetLayout> layouts; // by set index
};

} // namespace rhi
