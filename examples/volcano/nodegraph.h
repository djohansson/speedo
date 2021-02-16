#pragma once

#include "nodes/nodecommon.h"

#include <memory>
#include <string>
#include <vector>

struct NodeGraph
{
    std::vector<std::shared_ptr<INode>> nodes;
    std::vector<Link> links;
    std::string layout;
    int uniqueId = 0;
};

#include "nodegraph.inl"
