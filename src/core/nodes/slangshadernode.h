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

    SlangShaderNode& operator=(const SlangShaderNode&) = default;
    SlangShaderNode& operator=(SlangShaderNode&&) noexcept = default;

    void swap(SlangShaderNode& rhs) noexcept;
    friend void swap(SlangShaderNode& lhs, SlangShaderNode& rhs) noexcept { lhs.swap(rhs); }

    std::filesystem::path& path();

private:
    std::filesystem::path myPath;
};
