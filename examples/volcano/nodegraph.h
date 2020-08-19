#pragma once

#include "utils.h"

#include <any>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct INode
{
    virtual ~INode() noexcept {};
    virtual int& id() = 0;
    virtual std::string& name() = 0;
    virtual std::any& userData() = 0;
};

struct Attribute
{
    int id = 0;
    std::string name;
};

struct Link
{
    int fromId = 0;
    int toId = 0;
};

class InputOutputNode : public INode
{
public:

    InputOutputNode() = default;
    InputOutputNode(int id, std::string&& name);
    InputOutputNode(const InputOutputNode&) = default;
    InputOutputNode(InputOutputNode&&) = default;
    ~InputOutputNode() noexcept = default;

    InputOutputNode& operator=(const InputOutputNode&) = default;
    InputOutputNode& operator=(InputOutputNode&&) = default;

    int& id() final;
    std::string& name() final;
    std::any& userData() final;

    std::vector<Attribute>& inputAttributes();
    std::vector<Attribute>& outputAttributes();

private:

    int myId = 0;
    std::string myName;
    std::vector<Attribute> myInputAttributes;
    std::vector<Attribute> myOutputAttributes;
    std::any myUserData;
};

class SlangShaderNode : public InputOutputNode
{
public:

    SlangShaderNode() = default;
    SlangShaderNode(int id, std::string&& name, std::filesystem::path&& path);
    SlangShaderNode(const SlangShaderNode&) = default;
    SlangShaderNode(SlangShaderNode&&) = default;
    ~SlangShaderNode() noexcept = default;

    SlangShaderNode& operator=(const SlangShaderNode&) = default;
    SlangShaderNode& operator=(SlangShaderNode&&) = default;

    std::filesystem::path& path();

private:

    std::filesystem::path myPath;
};

struct NodeGraph
{
    std::vector<std::shared_ptr<INode>> nodes;
    std::vector<Link> links;
    std::string layout;
    int uniqueId = 0;
};

#include "nodegraph.inl"
