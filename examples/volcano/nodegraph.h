#pragma once

#include <any>
#include <memory>
#include <string>
#include <vector>


struct INode
{
    virtual const int& getId() const = 0;
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

struct InputOutputNode : INode
{
    InputOutputNode() = default;
    InputOutputNode(int id, std::string&& name);
    virtual ~InputOutputNode() = default;

    const int& getId() const final;
    std::string& name() final;
    std::any& userData() final;
    std::vector<Attribute>& inputAttributes();
    std::vector<Attribute>& outputAttributes();

    int id_ = 0;
    std::string name_;
    std::vector<Attribute> inputAttributes_;
    std::vector<Attribute> outputAttributes_;
    std::any userData_;
};

struct SlangShaderNode : InputOutputNode
{
    SlangShaderNode() = default;
    SlangShaderNode(int id, std::string&& name);

    std::string& path();

    std::string path_;
};

struct NodeGraph
{
    std::vector<std::shared_ptr<INode>> nodes;
    std::vector<Link> links;
    std::string layout;
    int uniqueId = 0;
};

#include "nodegraph.inl"
