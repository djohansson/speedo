#include "slangshadernode.h"

SlangShaderNode::SlangShaderNode(int id, std::string&& name, std::filesystem::path&& path)
: InputOutputNode(id, std::forward<std::string>(name))
, myPath(std::forward<std::filesystem::path>(path))
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
