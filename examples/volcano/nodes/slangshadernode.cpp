#include "slangshadernode.h"

#include <cereal/archives/json.hpp>

CEREAL_REGISTER_TYPE(SlangShaderNode);

SlangShaderNode::SlangShaderNode(int id, std::string&& name, std::filesystem::path&& path)
: InputOutputNode(id, std::move(name))
, myPath(std::move(path))
{
}

void SlangShaderNode::swap(SlangShaderNode& rhs) noexcept
{
    InputOutputNode::swap(rhs);
    std::swap(myPath, rhs.myPath);
}

std::filesystem::path& SlangShaderNode::path()
{
    return myPath;
}
