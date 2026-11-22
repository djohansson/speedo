#include "framecapture.h"

#include "windowedapplicationdetail.h"

#include <core/runtimings.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <utility>

namespace client::inline SPEEDO_GRAPHICS_API_NAMESPACE
{

namespace framecapture
{

// a read back of the frame into a bitmap file at path, then done (if given) once written: out of the present layout and
// back (headless, out of the image output's and back)
void RecordCapture(
	CommandBufferHandle cmd, const FrameCaptureTarget& target, std::string path, std::atomic_uint8_t* done, std::vector<core::TaskHandle>& callbacks)
{
	auto extent = target.extent;
	auto size = static_cast<size_t>(extent.width) * extent.height * 4;
	auto buffer = std::make_shared<gfx::Buffer>(gfx::BufferCreateDesc{
		target.device.CreateDeviceObjectCreateDesc("Frame Capture"), size, BufferUsage::kTransferDestination, MemoryProperty::kHostVisible});
	gfx::CommandEncoder encoder(cmd);
	Format format = Format::kUndefined;
	if (target.swapchain != nullptr)
	{
		auto& frame = target.swapchain->GetFrames()[target.outputIndex];
		auto layout = frame.GetLayout(0);
		frame.Transition(cmd, ImageLayout::kTransferSource, ImageAspect::kColor, 0);
		encoder.CopyImageToBuffer(frame.GetImages()[0], extent, *buffer);
		frame.Transition(cmd, layout, ImageAspect::kColor, 0);
		format = frame.GetDesc().imageFormats[0];
	}
	else
	{
		auto& image = target.imageOutput->GetImage();
		auto layout = image.GetDesc().layout;
		image.Transition(cmd, ImageLayout::kTransferSource, ImageAspect::kColor);
		encoder.CopyImageToBuffer(image, *buffer);
		image.Transition(cmd, layout, ImageAspect::kColor);
		format = image.GetDesc().format;
	}
	encoder.Barrier(PipelineStage::kTransfer, Access::kTransferWrite, PipelineStage::kHost, Access::kHostRead);
	bool bgra = format == Format::kB8G8R8A8Unorm || format == Format::kB8G8R8A8Srgb;
	if (done != nullptr)
		core::RunTiming("the capture recorded");
	callbacks.emplace_back(
		core::CreateTask(
			[buffer, extent, size, bgra, path = std::move(path), done]
			{
				if (done != nullptr)
					core::RunTiming("the capture's callback started");
				auto memory = buffer->Map();
				buffer->Invalidate(0, size);
				auto file = EncodeBmp(memory.first(size), extent, bgra);
				buffer->Unmap();
				if (done != nullptr)
					core::RunTiming("the capture encoded");
				if (!core::file::Write(path, file))
					std::println(stderr, "SPEEDO_CAPTURE: couldn't write {}", path);
				if (done != nullptr)
				{
					core::RunTiming("the capture saved");
					*done = 3;
				}
			}).handle);
}

} // namespace framecapture

void RecordFrameCaptures(CommandBufferHandle cmd, const FrameCaptureTarget& target, std::vector<core::TaskHandle>& callbacks)
{
	using namespace windowedapplication;

	// the frame an autoload exit ends on (SPEEDO_CAPTURE, see gCaptureState): the ui's request, or with SPEEDO_REPEATABLE
	// the frame counted from the sequences' restart, here (see gRepeatableCaptureFrame)
	if (gRepeatableCaptureFrame && gDrawnFrameCount.load(std::memory_order_relaxed) == *gRepeatableCaptureFrame)
	{
		if (uint8_t idle = 0; gCaptureState.compare_exchange_strong(idle, 2))
			framecapture::RecordCapture(cmd, target, gCapturePath, &gCaptureState, callbacks);
	}
	else if (uint8_t requested = 1; gCaptureState.compare_exchange_strong(requested, 2))
		framecapture::RecordCapture(cmd, target, gCapturePath, &gCaptureState, callbacks);
	// SPEEDO_CAPTURE_COUNT=<n>: the n frames the count ends on (the last among them), as <capture>-<frame of the count>.bmp
	// (see gRepeatableCaptureCountFrames); SPEEDO_CAPTURE_STRIDE=<s>: every s-th frame back from the last instead (how
	// something converges from the count's start, in one run)
	auto captureSetting = [](const char* name)
	{
		const char* value = std::getenv(name);
		return value != nullptr && *value != '\0' ? std::max(static_cast<uint32_t>(std::strtoul(value, nullptr, 10)), 1U) : 1U;
	};
	static const uint32_t kCaptureCount = captureSetting("SPEEDO_CAPTURE_COUNT");
	static const uint32_t kCaptureStride = captureSetting("SPEEDO_CAPTURE_STRIDE");
	if (kCaptureCount > 1 && gRepeatableCaptureFrame && !gCapturePath.empty())
	{
		uint64_t drawn = gDrawnFrameCount.load(std::memory_order_relaxed);
		uint64_t last = *gRepeatableCaptureFrame;
		if (drawn <= last && (last - drawn) % kCaptureStride == 0 && (last - drawn) / kCaptureStride < kCaptureCount &&
			last - drawn < gRepeatableCaptureCountFrames)
		{
			std::filesystem::path path(gCapturePath);
			uint64_t frame = gRepeatableCaptureCountFrames - (last - drawn);
			auto numbered = path.parent_path() / std::format("{}-{:04}{}", path.stem().string(), frame, path.extension().string());
			framecapture::RecordCapture(cmd, target, numbered.string(), nullptr, callbacks);
		}
	}
	// each frame drawing a pose of SPEEDO_CAMERA_PATH for the first time (SPEEDO_CAPTURE_PATH=<directory>:
	// pose-<NNNN>.bmp), to compare a move's frames
	static const char* gCapturePathDirectory = std::getenv("SPEEDO_CAPTURE_PATH");
	// SPEEDO_CAPTURE_PATH_FIRST=<pose>: from that pose on (a still pose repeated, once what settles over frames has: the
	// indirect lighting, auto exposure); SPEEDO_CAPTURE_PATH_STRIDE=<n>: every n-th pose of those (a long move)
	auto setting = [](const char* name, uint32_t fallback)
	{
		const char* value = std::getenv(name);
		return value != nullptr && *value != '\0' ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : fallback;
	};
	static const uint32_t kCapturePathFirst = setting("SPEEDO_CAPTURE_PATH_FIRST", 0U);
	static const uint32_t kCapturePathStride = std::max(setting("SPEEDO_CAPTURE_PATH_STRIDE", 1U), 1U);
	if (gCapturePathDirectory != nullptr && *gCapturePathDirectory != '\0')
		if (auto pose = target.placedPose; pose && *pose >= kCapturePathFirst && (*pose - kCapturePathFirst) % kCapturePathStride == 0)
			framecapture::RecordCapture(
				cmd, target, (std::filesystem::path(gCapturePathDirectory) / std::format("pose-{:04}.bmp", *pose)).string(), nullptr, callbacks);
}

std::vector<std::byte> EncodeBmp(std::span<const std::byte> pixels, rhi::Extent2d extent, bool bgra)
{
	// a bitmap file header and info header, then the pixels
	constexpr size_t kHeaderSize = 54;
	std::vector<std::byte> file(kHeaderSize + pixels.size());
	auto put = [&file](size_t offset, uint32_t value, size_t bytes)
	{
		for (size_t byteIt = 0; byteIt < bytes; byteIt++)
			file[offset + byteIt] = static_cast<std::byte>((value >> (8 * byteIt)) & 0xFFU);
	};
	file[0] = std::byte{'B'};
	file[1] = std::byte{'M'};
	put(2, static_cast<uint32_t>(file.size()), 4);
	put(10, kHeaderSize, 4);
	put(14, 40, 4);
	put(18, extent.width, 4);
	put(22, static_cast<uint32_t>(-static_cast<int32_t>(extent.height)), 4);
	put(26, 1, 2);
	put(28, 32, 2);
	std::ranges::copy(pixels, file.begin() + kHeaderSize);
	if (!bgra)
		for (size_t pixelIt = kHeaderSize; pixelIt + 3 < file.size(); pixelIt += 4)
			std::swap(file[pixelIt], file[pixelIt + 2]);
	return file;
}

} // namespace client::inline SPEEDO_GRAPHICS_API_NAMESPACE
