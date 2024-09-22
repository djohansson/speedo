#pragma once

#include "inputoutputnode.h"

#include <filesystem>

class SlangShaderNode final : public InputOutputNode
{
public:
	SlangShaderNode() noexcept = default;
	SlangShaderNode(int id, std::string&& name, std::filesystem::path&& path);
	SlangShaderNode(const SlangShaderNode&) = default;
	SlangShaderNode(SlangShaderNode&&) noexcept = default;
	~SlangShaderNode() = default;

	[[maybe_unused]] SlangShaderNode& operator=(const SlangShaderNode&) = default;
	[[maybe_unused]] SlangShaderNode& operator=(SlangShaderNode&&) noexcept = default;

	void Swap(SlangShaderNode& rhs) noexcept;
	friend void Swap(SlangShaderNode& lhs, SlangShaderNode& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] std::filesystem::path& Path();

private:
	std::filesystem::path myPath;
};
