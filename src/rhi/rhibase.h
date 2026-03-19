#pragma once

#include "types.h"
#include <core/task.h>

struct RHIBase
{
	virtual ~RHIBase() = default;
	[[nodiscard]] virtual GraphicsApi GetApi() const = 0;

	// todo: move these elsewhere?
	mutable ConcurrentQueue<TaskHandle> mainCalls; // queue with tasks that will be called once on main thread
	mutable ConcurrentQueue<TaskHandle> drawCalls; // queue with tasks that will be called once on draw thread/task
};
