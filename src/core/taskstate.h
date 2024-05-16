#pragma once

#include "memorypool.h"
#include "upgradablesharedmutex.h"

#include <vector>

using TaskHandle = MemoryPoolHandle;

struct TaskState
{
	std::atomic_uint32_t latch{1U};

	UpgradableSharedMutex mutex; // Protects the variables below
	std::vector<TaskHandle> adjacencies;
	bool isContinuation = false;
	//
};
