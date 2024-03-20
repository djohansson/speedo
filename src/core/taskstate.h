#pragma once

#include "copyableatomic.h"
#include "utils.h"

#include <optional>
#include <vector>

struct TaskHandle
{
	static constexpr uint32_t InvalidIndex = ~0u;
	uint32_t value = InvalidIndex;

	bool operator!() const { return value == InvalidIndex; }
	auto operator<=>(const TaskHandle&) const = default;
};

struct TaskState
{
	std::atomic_uint32_t latch{1u};

	// todo: verify thread safety in these
	std::vector<CopyableAtomic<TaskHandle>> adjacencies;
	bool isContinuation = false;
	//
};
