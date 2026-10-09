#pragma once

#include <gfx/gpu.h>
#include <gfx/upload.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

// work the draw thread submits to the graphics queue between frames, ordered with the frames by the graphics timeline
// (each submission waits for all graphics work submitted before it): updates, uploads made usable, and resources freed
// once the gpu is done with them
namespace gfx
{

// buffers and images uploaded by the loaders (see Upload), to install
struct Uploads
{
	std::vector<std::pair<const Buffer*, Upload>> buffers;
	std::vector<std::pair<std::shared_ptr<Image>, Upload>> images;
};

// hands `resource` to a graphics queue submission that waits for all graphics work submitted so far, and releases it
// from that submission's timeline callback, i.e. once the gpu can no longer be using it. call on the draw thread.
void RetireAfterGraphicsWork(QueueTimelineContextData& graphics, std::shared_ptr<void> resource);

// writes data to buffer at offset, in a submission on the graphics queue that is ordered after all graphics work
// submitted so far, and before all that follows. call on the draw thread.
void UpdateBufferOnGraphics(
	QueueTimelineContextData& graphics, const Buffer& buffer, uint64_t offset, std::span<const std::byte> data);

// in a graphics queue submission that waits for the uploads, acquires the uploaded resources for the graphics queue
// family and transitions the images to a shader readable layout. once that has executed, has the draw thread call
// bind (with the graphics queue context). call on the draw thread.
void TransitionThenBind(
	RHI& rhi,
	QueueTimelineContextData& graphics,
	const Uploads& uploads,
	std::function<void(QueueTimelineContextData&)> bind);

} // namespace gfx
