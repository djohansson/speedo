#pragma once

#include "copyableatomic.h"
#include "memorypool.h"
#include "utils.h"

#include <optional>
#include <vector>

using TaskHandle = MemoryPoolHandle;

struct TaskState
{
	std::atomic_uint32_t latch{1u};

	// todo: verify thread safety in these
	std::vector<CopyableAtomic<TaskHandle>> adjacencies;
	bool isContinuation = false;
	//
};
