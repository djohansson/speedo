#pragma once

#include <gfx/gpu.h>

#include <cstdint>

namespace gfx
{

// the transfer queue submission that uploads a loaded resource (see Model::Load and LoadTexture), which return
// without waiting for it. gpu work that uses the resource must wait for the submission on the gpu (semaphore at
// value), and acquire the resource for its queue family (see CommandEncoder::AcquireOwnership): the upload released
// it from queueFamilyIndex to the graphics queue's family.
struct Upload
{
	const Semaphore* semaphore = nullptr; // the transfer queue's timeline semaphore
	uint64_t value = 0; // signaled once the upload has completed
	uint32_t queueFamilyIndex = 0; // the transfer queue's
};

} // namespace gfx
