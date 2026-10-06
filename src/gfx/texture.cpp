#include <gfx/texture.h>
#include <gfx/importversions.h>

#include <core/application.h>
#include <core/file.h>
#include <core/profiling.h>
#include <core/uuids_extra.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace gfx
{

namespace detail
{

[[nodiscard]] rhi::Format FormatOf(const image::Image& image) noexcept
{
	bool srgb = image.usage == image::Usage::kColor;
	switch (image.format)
	{
	case image::Format::kBC1: return srgb ? rhi::Format::kBC1RgbSrgb : rhi::Format::kBC1RgbUnorm;
	case image::Format::kBC3: return srgb ? rhi::Format::kBC3Srgb : rhi::Format::kBC3Unorm;
	case image::Format::kBC4: return rhi::Format::kBC4Unorm;
	case image::Format::kBC5: return rhi::Format::kBC5Unorm;
	}
	return rhi::Format::kUndefined;
}

} // namespace detail

Texture LoadTexture(std::string_view filePath, std::atomic_uint8_t& progress, const image::Options& options)
{
	using namespace rhi;

	ZoneScopedN("gfx::LoadTexture");

	auto* rhi = GetRHI<kGraphicsApi>();
	ENSURE(rhi);
	auto& device = rhi->GetPrimaryDevice();

	// loading is given up when the application exits (only where it takes long; decoding itself can't be interrupted)
	auto app = core::Application::Get();
	ENSURE(app);
	auto cancelled = [&app] { return app->IsExitRequested(); };

	// the compressed mip chain, in a staging buffer: filled here, before the upload takes the transfer queue's lock
	image::Image layout;
	Buffer staging;
	auto stagingDesc = [&device, filePath] { return device.CreateDeviceObjectCreateDesc(std::format("{} (staging)", filePath)); };

	auto loadBin = [&](auto& inStream) -> std::error_code
	{
		progress = 32;

		if (auto result = inStream(layout); failure(result))
			return std::make_error_code(result);

		staging = Buffer::CreateStaging(stagingDesc(), layout.size);
		auto memory = staging.Map();
		auto result = inStream(std::span(reinterpret_cast<uint8_t*>(memory.data()), layout.size));
		staging.Unmap();
		if (failure(result))
		{
			staging = {};
			return std::make_error_code(result);
		}

		progress = 255;

		return {};
	};

	auto saveBin = [&](auto& outStream) -> std::error_code
	{
		if (auto result = outStream(layout); failure(result))
			return std::make_error_code(result);

		auto memory = staging.Map();
		auto result = outStream(std::span(reinterpret_cast<const uint8_t*>(memory.data()), layout.size));
		staging.Unmap();
		if (failure(result))
			return std::make_error_code(result);

		// LoadAsset reports the rest, while hashing the saved cache
		return {};
	};

	auto loadImage = [&](auto& /*todo: use me: in*/) -> std::error_code
	{
		progress = 32;

		bool mapped = false;
		auto allocate = [&](size_t size) -> std::byte*
		{
			staging = Buffer::CreateStaging(stagingDesc(), size);
			mapped = true;
			return staging.Map().data();
		};

		auto result = image::Import(std::filesystem::path(filePath), options, allocate, &progress, cancelled);

		if (mapped)
			staging.Unmap();

		if (!result)
		{
			staging = {};

			if (cancelled())
				return std::make_error_code(std::errc::operation_canceled);

			std::println(stderr, "{}", result.error());
			return std::make_error_code(std::errc::invalid_argument);
		}

		layout = std::move(*result);

		return {};
	};

	std::string params;
	std::string paramsHash;
	params.append(std::format("stb-{}", kStbVersion)); // stb_image, stb_image_resize2 and stb_dxt
	params.append("|imageimport-v3"); // bump when image::Import changes what it produces
	// the decoders of the formats stb_image doesn't read
	auto extension = std::filesystem::path(filePath).extension().string();
	std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (extension == ".webp")
		params.append(std::format("|libwebp-{}", kLibWebpVersion));
	else if (extension == ".ktx2")
		params.append(std::format("|ktx-{}", kKtxVersion));
	params.append(std::format("|usage-{}", std::to_underlying(options.usage)));
	if (options.usage == image::Usage::kBump)
		params.append(std::format("|bump-scale-{}", options.bumpScale));
	params.append("|cache-v3"); // bump when the serialized layout (image::Image) changes, to invalidate stale caches
	static constexpr size_t kSha2Size = 32;
	std::array<uint8_t, kSha2Size> sha2;
	picosha2::hash256(params.cbegin(), params.cend(), sha2.begin(), sha2.end());
	picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), paramsHash);
	auto loadResult = core::file::LoadAsset(filePath, loadImage, loadBin, saveBin, paramsHash, {}, &progress, cancelled);

	if (!loadResult || !staging.IsValid())
	{
		// cancelled or failed (the staging buffer is released with it)
		if (!loadResult && loadResult.error() != std::errc::operation_canceled)
			std::println(stderr, "Failed to load image {}: {}", filePath, loadResult.error().message());

		return {};
	}

	ImageCreateDesc desc{
		device.CreateDeviceObjectCreateDesc(filePath),
		{},
		detail::FormatOf(layout),
		ImageTiling::kOptimal,
		ImageUsage::kSampled | ImageUsage::kTransferDestination,
		MemoryProperty::kDeviceLocal,
		ImageAspect::kColor,
		ImageLayout::kUndefined};
	desc.mipLevels.reserve(layout.mipLevels.size());
	for (const auto& level : layout.mipLevels)
		desc.mipLevels.push_back(
			{.extent = {.width = level.width, .height = level.height}, .size = level.size, .offset = level.offset});

	// one queue lock at a time: the queue types may alias the same context (see Device::GetQueue)
	auto graphicsQueueFamilyIndex = device.GetQueue(kQueueTypeGraphics).Read()->queueFamilyIndex;

	Texture texture;
	{
		auto transfer = device.GetQueue(kQueueTypeTransfer).Write();
		auto& [transferQueue, transferSubmits] = transfer->queues.Get();

		auto cmd = transferQueue.GetPool().Commands();

		core::TaskCreateInfo<void> transferDone;
		texture.image = std::make_shared<Image>(std::move(desc), std::move(staging), cmd, transferDone);
		texture.view = std::make_shared<ImageView>(ImageViewCreateDesc{
			device.CreateDeviceObjectCreateDesc(filePath), *texture.image, texture.image->GetDesc().format, ImageAspect::kColor});
		texture.upload = Upload{
			.semaphore = &transfer->semaphore, .value = ++transfer->timeline, .queueFamilyIndex = transfer->queueFamilyIndex};

		CommandEncoder(cmd).ReleaseOwnership(
			*texture.image, texture.upload.queueFamilyIndex, graphicsQueueFamilyIndex, PipelineStage::kTransfer, Access::kTransferWrite);
		cmd.End();

		std::vector<core::TaskHandle> transferTimelineCallbacks;
		transferTimelineCallbacks.emplace_back(transferDone.handle);
		// the caller may drop the texture before the upload has completed, e.g. if the load it is part of is cancelled
		transferTimelineCallbacks.emplace_back(core::CreateTask([image = texture.image, view = texture.view] {}).handle);

		transferQueue.EnqueueSubmit(QueueDeviceSyncInfo{
			.waitSemaphores = {},
			.waitDstStageMasks = {},
			.waitSemaphoreValues = {},
			.signalSemaphores = {transfer->semaphore},
			.signalSemaphoreValues = {texture.upload.value},
			.callbacks = std::move(transferTimelineCallbacks)});

		transferSubmits |= transferQueue.Submit();
	}

	return texture;
}

} // namespace gfx
