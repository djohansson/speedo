#pragma once

#include <rhi/vocabulary.h>

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

// the shader binary formats the backends take (a copy of gfx takes its backend's, gfx::kShaderFormat)
enum class ShaderFormat : uint8_t
{
	kSpirv16, // spir-v 1.6 (Vulkan)
	kMsl, // Metal shading language source, an entry point each (Metal, see docs/backends-plan.md)
	kDxil, // DXIL for shader model 6.8, an entry point each, each descriptor set a register space (D3D12)
};

using ShaderBinary = std::vector<char>;

struct ComputeLaunchParameters
{
	std::array<uint64_t, 3> threadGroupSize;
	uint64_t waveSize{};
};

using EntryPoint = std::tuple<std::string, ShaderStage, std::optional<ComputeLaunchParameters>>;

using Shader = std::tuple<ShaderBinary, EntryPoint>;

// a set's, a binding's or the push constants' slot in the code, for a format whose code binds each at one of its own:
// MSL's buffer index of a set or the push constants (an argument buffer's, or the push constants' bytes), which slang
// numbers by the parameters' declaration order across the shader set's program, not by their sets (environment.slang's
// parameter block is buffer 0, its push constants buffer 1); DXIL's register space of a set (a parameter block's own),
// and register of a binding in its set's space, or of the push constants (in their space, ShaderPushConstants::space),
// which slang numbers per register class in the fields' order, not by their bindings (an array takes its count of
// registers, and a set's read only and writable resources are numbered apart: gfx's model instances' set has t0, t5,
// t9, t14, t38). none for spir-v, whose sets are its descriptor sets and bindings
inline constexpr uint32_t kNoShaderSlot = ~0U;

// a resource binding of a descriptor set
struct ShaderBinding
{
	uint32_t binding = 0;
	DescriptorType type{};
	uint32_t count = 1; // array size, or the size in bytes for an inline uniform block
	ShaderStage stages{};
	std::string name; // the shader variable, as Pipeline::SetDescriptorData takes it
	uint64_t nameHash = 0; // XXH3_64bits of name
	// DXIL's: its register (see kNoShaderSlot); a storage buffer's element's size in the code's layout (a structured
	// buffer's view takes it), 0 for any other; and whether a storage buffer is written (an unordered access view, else a
	// shader resource view: rhi's storage buffer type doesn't tell them apart, D3D12's views do)
	uint32_t slot = kNoShaderSlot;
	uint32_t stride = 0;
	bool writable = false;
};

struct ShaderPushConstants
{
	ShaderStage stages{};
	uint32_t offset = 0;
	uint32_t size = 0;
	uint32_t slot = kNoShaderSlot;
	uint32_t space = 0; // DXIL's: their register's space
};

// a descriptor set's bindings, and the push constants declared with it
struct ShaderSetLayout
{
	std::vector<ShaderBinding> bindings;
	std::optional<ShaderPushConstants> pushConstants;
	uint32_t slot = kNoShaderSlot;
};

// a shader compiled with its specialization constant 0 a value, for a format without specialization constants (DXIL):
// the compiler folds the value as Vulkan's driver does a specialization, dropping the code it disables
struct ShaderSpecialization
{
	uint32_t shader = 0; // its index in ShaderSet::shaders (whose binary is its specialization 0)
	uint32_t value = 0;
	ShaderBinary code;
};

struct ShaderSet
{
	std::vector<Shader> shaders;
	core::UnorderedMap<uint32_t, ShaderSetLayout> layouts; // by set index
	// DXIL's: the shaders that read the specialization constant, compiled again for each of its other values the layout's
	// configuration names (slang's [vk::constant_id] constant is a uniform otherwise, which D3D12 can't specialize)
	std::vector<ShaderSpecialization> specializations;
};

} // namespace rhi
