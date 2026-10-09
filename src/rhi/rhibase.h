#pragma once

#include <core/task.h>

namespace rhi
{
struct RHIBase
{
	virtual ~RHIBase() = default;

	mutable core::ConcurrentQueue<core::TaskHandle> drawCalls; // queue with tasks that will be called once on draw thread/task
};

} // namespace rhi
