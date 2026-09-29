#pragma once

#include <core/nodes/nodecommon.h>

#include <memory>
#include <string>
#include <vector>

namespace core
{

struct NodeGraph
{
	std::vector<std::shared_ptr<INode>> nodes;
	std::vector<Link> links;
	std::string layout;
	int uniqueId = 0;
};

} // namespace core
