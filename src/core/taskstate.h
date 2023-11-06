#pragma once

#include "copyableatomic.h"

#include <optional>
#include <vector>

class Task;

struct TaskState
{
	std::optional<std::atomic_uint32_t> latch;
	std::vector<CopyableAtomic<Task*>> adjacencies;
};
