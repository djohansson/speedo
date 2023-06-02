#pragma once

#include "../utils.h"

#include <optional>
#include <string>

struct INode
{
    virtual ~INode() {};
    virtual int& id() = 0;
    virtual std::optional<int>& selected() = 0;
    virtual std::string& name() = 0;
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

#include "nodecommon.inl"
