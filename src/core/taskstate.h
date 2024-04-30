#pragma once

#include "memorypool.h"
#include "upgradablesharedmutex.h"
#include "utils.h"

#include <vector>

using TaskHandle = MemoryPoolHandle;
static constexpr TaskHandle NullTaskHandle{};

struct TaskState
{
	std::atomic_uint32_t latch{1u};

	UpgradableSharedMutex mutex; // Protects the variables below
	std::vector<TaskHandle> adjacencies;
	bool isContinuation = false;
	//
};
