#pragma once

#include "descriptorset.h"
#include "gfx-types.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <slang.h>


using EntryPoint = std::pair<std::string, uint32_t>;
using ShaderBinary = std::vector<std::byte>;
using ShaderEntry = std::pair<ShaderBinary, EntryPoint>;

// this is a temporary object only used during loading.
template <GraphicsBackend B>
struct SerializableShaderReflectionModule
{
	std::vector<ShaderEntry> shaders;
	BindingsMap<B> bindings;
	std::vector<SamplerCreateInfo<B>> immutableSamplers;
};

template <GraphicsBackend B>
std::shared_ptr<SerializableShaderReflectionModule<B>> loadSlangShaders(const std::filesystem::path& slangFile);

template <GraphicsBackend B>
void createLayoutBindings(slang::VariableLayoutReflection* parameter, BindingsMap<B>& bindings);

#include "shader.inl"
#include "shader-vulkan.inl"
