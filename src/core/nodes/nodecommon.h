#pragma once

#include <core/file.h>

#include <optional>
#include <string>

struct INode
{
    virtual ~INode() {};
    virtual int& Id() = 0;
    virtual std::optional<int>& Selected() = 0;
    virtual std::string& Name() = 0;
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
