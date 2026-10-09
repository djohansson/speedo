#include <gfx/texture.h>
#include <gfx/environment.h>
#include <gfx/importversions.h>
#include <gfx/gltfimport.h>

#include <core/application.h>
#include <core/file.h>
#include <core/profiling.h>
#include <core/uuids_extra.h>

#include <glm/gtc/packing.hpp>

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
	case image::Format::kBC7: return srgb ? rhi::Format::kBC7Srgb : rhi::Format::kBC7Unorm;
	}
	return rhi::Format::kUndefined;
}

// uploads a staged image (its mip levels in staging, level 0 first) on the primary device's transfer queue, released to
// the graphics queue family (see Texture::upload)
[[nodiscard]] Texture Upload(
	Device& device,
	const std::string& name,
	rhi::Format format,
	std::span<const image::MipLevel> mipLevels,
	Buffer&& staging,
	const std::array<uint8_t, 4>& channels = {0, 1, 2, 3})
{
	using namespace rhi;

	ImageCreateDesc desc{
		device.CreateDeviceObjectCreateDesc(name),
		{},
		format,
		ImageTiling::kOptimal,
		ImageUsage::kSampled | ImageUsage::kTransferDestination,
		MemoryProperty::kDeviceLocal,
		ImageAspect::kColor,
		ImageLayout::kUndefined};
	desc.mipLevels.reserve(mipLevels.size());
	for (const auto& level : mipLevels)
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
		// sampled with the channels where the shader expects them (see image::Image::channels)
		std::array<ComponentSwizzle, 4> components{};
		for (size_t channel = 0; channel < 4; channel++)
			components[channel] = channels[channel] == image::Image::kZero	 ? ComponentSwizzle::kZero
								  : channels[channel] == image::Image::kOne ? ComponentSwizzle::kOne
								  : channels[channel] == channel			  ? ComponentSwizzle::kIdentity
																			  : static_cast<ComponentSwizzle>(std::to_underlying(ComponentSwizzle::kR) + channels[channel]);
		texture.view = std::make_shared<ImageView>(ImageViewCreateDesc{
			device.CreateDeviceObjectCreateDesc(name), *texture.image, texture.image->GetDesc().format, ImageAspect::kColor, 0, components});
		texture.upload = gfx::Upload{
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

} // namespace detail

Texture LoadTexture(
	std::string_view filePath, std::atomic_uint8_t& progress, const image::Options& options, std::optional<uint32_t> embeddedImage)
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
	// an embedded image is named after its file (a gltf model) and index
	auto name = embeddedImage ? std::format("{}#image{}", filePath, *embeddedImage) : std::string(filePath);
	auto stagingDesc = [&device, &name] { return device.CreateDeviceObjectCreateDesc(std::format("{} (staging)", name)); };

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

	// in: the file, mapped by LoadAsset (unless the image is embedded in a model, whose file it is)
	auto loadImage = [&](auto& in) -> std::error_code
	{
		progress = 32;

		bool mapped = false;
		auto allocate = [&](size_t size) -> std::byte*
		{
			staging = Buffer::CreateStaging(stagingDesc(), size);
			mapped = true;
			return staging.Map().data();
		};

		std::expected<image::Image, std::string> result;
		if (embeddedImage)
		{
			auto bytes = gltf::EmbeddedImage(std::filesystem::path(filePath), *embeddedImage);
			result = bytes ? image::Import(*bytes, name, options, allocate, &progress, cancelled) : std::unexpected(bytes.error());
		}
		else
		{
			auto bytes = in.remaining_data();
			result = image::Import(std::span<const std::byte>(bytes.data(), bytes.size()), filePath, options, allocate, &progress, cancelled);
		}

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
	params.append("|imageimport-v4"); // bump when image::Import changes what it produces
	// the decoders of the formats stb_image doesn't read
	auto extension = std::filesystem::path(filePath).extension().string();
	std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (extension == ".webp")
		params.append(std::format("|libwebp-{}", kLibWebpVersion));
	else if (extension == ".ktx2")
		params.append(std::format("|ktx-{}|ktx2-direct-v2", kKtxVersion));
	// an image a model embeds is cached against the model's file: any of the decoders may read it
	if (embeddedImage)
		params.append(std::format("|embedded-{}|libwebp-{}|ktx-{}|ktx2-direct-v2", *embeddedImage, kLibWebpVersion, kKtxVersion));
	params.append(std::format("|usage-{}", std::to_underlying(options.usage)));
	if (options.usage == image::Usage::kBump)
		params.append(std::format("|bump-scale-{}", options.bumpScale));
	params.append("|cache-v5"); // bump when the serialized layout (image::Image) changes, to invalidate stale caches
	static constexpr size_t kSha2Size = 32;
	std::array<uint8_t, kSha2Size> sha2;
	picosha2::hash256(params.cbegin(), params.cend(), sha2.begin(), sha2.end());
	picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), paramsHash);
	auto loadResult = core::file::LoadAsset(filePath, loadImage, loadBin, saveBin, paramsHash, {}, &progress, cancelled);

	if (!loadResult || !staging.IsValid())
	{
		// cancelled or failed (the staging buffer is released with it)
		if (!loadResult && loadResult.error() != std::errc::operation_canceled)
			std::println(stderr, "Failed to load image {}: {}", name, loadResult.error().message());

		return {};
	}

	auto texture = detail::Upload(device, name, detail::FormatOf(layout), layout.mipLevels, std::move(staging), layout.channels);
	texture.normalYUp = layout.normalYUp;
	return texture;
}

namespace detail
{

// a R16G16B16A16_SFLOAT image for the environment filter to write (and the frames to sample), with levels mips from
// width wide (each half the one before, at least minimum wide), and a view of all of them
Texture StorageTexture(Device& device, const std::string& name, uint32_t width, uint32_t levels, uint32_t minimum)
{
	using namespace rhi;

	std::vector<ImageMipLevelDesc> mipLevels;
	for (uint32_t level = 0; level < levels; level++)
	{
		uint32_t levelWidth = std::max(width >> level, minimum);
		mipLevels.push_back({.extent = {.width = levelWidth, .height = std::max(levelWidth / 2, 1U)}});
	}
	Texture texture;
	texture.image = std::make_shared<Image>(ImageCreateDesc{
		device.CreateDeviceObjectCreateDesc(name),
		std::move(mipLevels),
		Format::kR16G16B16A16Sfloat,
		ImageTiling::kOptimal,
		ImageUsage::kStorage | ImageUsage::kSampled,
		MemoryProperty::kDeviceLocal,
		ImageAspect::kColor,
		ImageLayout::kUndefined});
	texture.view = std::make_shared<ImageView>(ImageViewCreateDesc{
		device.CreateDeviceObjectCreateDesc(std::format("{} View", name)), *texture.image, Format::kR16G16B16A16Sfloat, ImageAspect::kColor});
	return texture;
}

// a panorama as R16G16B16A16_SFLOAT texels, into memory
void PackHalf(const environment::Panorama& panorama, std::span<std::byte> memory)
{
	auto* texels = reinterpret_cast<uint16_t*>(memory.data());
	size_t count = static_cast<size_t>(panorama.width) * panorama.height;
	for (size_t texel = 0; texel < count; texel++)
	{
		for (size_t c = 0; c < 3; c++)
			texels[(texel * 4) + c] = glm::packHalf1x16(panorama.rgb[(texel * 3) + c]);
		texels[(texel * 4) + 3] = glm::packHalf1x16(1.0F);
	}
}

} // namespace detail

EnvironmentTexture LoadEnvironment(std::optional<std::string_view> filePath, std::atomic_uint8_t& progress)
{
	using namespace rhi;

	ZoneScopedN("gfx::LoadEnvironment");

	auto* rhi = GetRHI<kGraphicsApi>();
	ENSURE(rhi);
	auto& device = rhi->GetPrimaryDevice();

	auto app = core::Application::Get();
	ENSURE(app);
	auto cancelled = [&app] { return app->IsExitRequested(); };

	std::string name = filePath ? std::string(*filePath) : std::string("procedural sky");
	EnvironmentTexture result;
	uint32_t width = environment::kSkyWidth; // the source's

	if (!filePath)
	{
		// nothing to load: the filter draws it from its parameters, and its sun is known
		const auto& sky = environment::ProceduralSkyParameters();
		result.sky = sky;
		environment::DominantLight sun{.direction = sky.sunDirection};
		sun.irradiance.fill(environment::kSkySunIrradiance);
		result.dominantLights = {sun};
	}
	else
	{
		// the panorama decoded and its dominant lights taken out (cached): the original and the lighting one, as half
		// floats in a staging buffer each
		struct Prepared
		{
			uint32_t width = 0;
			uint32_t height = 0;
			std::vector<environment::DominantLight> dominantLights;
		} prepared;
		Buffer originalStaging;
		Buffer lightingStaging;
		auto stagingDesc = [&device, &name](std::string_view what)
		{ return device.CreateDeviceObjectCreateDesc(std::format("{} {} (staging)", name, what)); };
		auto size = [&prepared] { return static_cast<size_t>(prepared.width) * prepared.height * 4 * sizeof(uint16_t); };
		auto createStaging = [&]
		{
			originalStaging = Buffer::CreateStaging(stagingDesc("original"), size());
			lightingStaging = Buffer::CreateStaging(stagingDesc("lighting"), size());
		};

		auto loadBin = [&](auto& inStream) -> std::error_code
		{
			progress = 32;
			if (auto result = inStream(prepared.width, prepared.height, prepared.dominantLights); failure(result))
				return std::make_error_code(result);
			createStaging();
			for (auto* staging : {&originalStaging, &lightingStaging})
			{
				auto memory = staging->Map();
				auto result = inStream(std::span(reinterpret_cast<uint8_t*>(memory.data()), size()));
				staging->Unmap();
				if (failure(result))
					return std::make_error_code(result);
			}
			progress = 255;
			return {};
		};
		auto saveBin = [&](auto& outStream) -> std::error_code
		{
			if (auto result = outStream(prepared.width, prepared.height, prepared.dominantLights); failure(result))
				return std::make_error_code(result);
			for (auto* staging : {&originalStaging, &lightingStaging})
			{
				auto memory = staging->Map();
				auto result = outStream(std::span(reinterpret_cast<const uint8_t*>(memory.data()), size()));
				staging->Unmap();
				if (failure(result))
					return std::make_error_code(result);
			}
			return {};
		};
		// in: the file, mapped by LoadAsset
		auto loadEnvironment = [&](auto& in) -> std::error_code
		{
			progress = 32;
			auto bytes = in.remaining_data();
			auto panorama = environment::Import(std::span<const std::byte>(bytes.data(), bytes.size()), *filePath);
			if (!panorama)
			{
				std::println(stderr, "{}", panorama.error());
				return std::make_error_code(std::errc::invalid_argument);
			}
			if (cancelled())
				return std::make_error_code(std::errc::operation_canceled);
			progress = 128;
			auto lighting = *panorama;
			prepared = {.width = panorama->width, .height = panorama->height, .dominantLights = environment::ExtractDominantLights(lighting)};
			createStaging();
			for (auto [staging, source] : {std::pair{&originalStaging, &*panorama}, std::pair{&lightingStaging, &lighting}})
			{
				detail::PackHalf(*source, staging->Map());
				staging->Unmap();
			}
			return {};
		};

		// bump environment-vN when environment::Import or ExtractDominantLights change what they produce, cache-vN when
		// what is cached does
		std::string params = std::format("stb-{}|environment-v6|cache-v5", kStbVersion);
		std::string paramsHash;
		static constexpr size_t kSha2Size = 32;
		std::array<uint8_t, kSha2Size> sha2;
		picosha2::hash256(params.cbegin(), params.cend(), sha2.begin(), sha2.end());
		picosha2::bytes_to_hex_string(sha2.cbegin(), sha2.cend(), paramsHash);
		auto loadResult = core::file::LoadAsset(*filePath, loadEnvironment, loadBin, saveBin, paramsHash, {}, &progress, cancelled);
		if (!loadResult || !originalStaging.IsValid() || !lightingStaging.IsValid())
		{
			if (!loadResult && loadResult.error() != std::errc::operation_canceled)
				std::println(stderr, "Failed to load environment {}: {}", name, loadResult.error().message());
			return {};
		}

		width = prepared.width;
		std::array level{image::MipLevel{.width = prepared.width, .height = prepared.height, .offset = 0, .size = static_cast<uint32_t>(size())}};
		result.original = detail::Upload(device, std::format("{} original", name), Format::kR16G16B16A16Sfloat, level, std::move(originalStaging));
		result.lighting = detail::Upload(device, std::format("{} lighting", name), Format::kR16G16B16A16Sfloat, level, std::move(lightingStaging));
		result.dominantLights = std::move(prepared.dominantLights);
	}

	// what the filter writes: the source's pyramid (down to 8 wide, as the cpu reference's), the levels and the sheen
	// levels (see environment::Environment)
	uint32_t sourceLevels = 1;
	while ((width >> sourceLevels) >= 8)
		sourceLevels++;
	auto source = detail::StorageTexture(device, std::format("{} source", name), width, sourceLevels, 8);
	result.source = std::move(source.image);
	result.sourceView = std::move(source.view);
	result.texture = detail::StorageTexture(device, name, width, environment::kLevelCount, 2);
	result.sheenTexture = detail::StorageTexture(device, std::format("{} sheen", name), width / 8, environment::kLevelCount, 2);
	result.levelCount = environment::kLevelCount;
	result.sheenLevelCount = environment::kLevelCount;
	return result;
}

} // namespace gfx
