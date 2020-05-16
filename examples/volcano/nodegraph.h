#pragma once

#include <filesystem>
#include <string>
#include <vector>


struct INode
{
    virtual int getIdx() const = 0;
    virtual const std::string& getName() const = 0;
};

struct Attribute
{
    int idx = 0;
    std::string name;
};

struct Link
{
    int fromIdx = 0;
    int toIdx = 0;
};

struct InputOutputNode : INode
{
    InputOutputNode() = default;
    InputOutputNode(int idx, std::string&& name);
    virtual ~InputOutputNode() = default;

    int getIdx() const final;
    const std::string& getName() const final;

    int idx = 0;
    std::string name;
    std::vector<Attribute> inputAttributes;
    std::vector<Attribute> outputAttributes;
};

struct SlangShaderNode : InputOutputNode
{
    SlangShaderNode() = default;
    SlangShaderNode(int idx, std::string&& name);

    std::filesystem::path path;
};

struct NodeGraph
{
    std::vector<std::shared_ptr<INode>> nodes;
    std::vector<Link> links;
};

#include "nodegraph.inl"
