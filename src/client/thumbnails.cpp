#include "thumbnails.h"

#include <core/application.h>
#include <core/file.h>
#include <core/profiling.h>
#include <core/task.h>

#include <gfx/environment.h>
#include <gfx/imageimport.h>
#include <gfx/meshimport.h>
#include <gfx/model.h>
#include <gfx/texture.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>
#include <print>
#include <span>
#include <utility>

#include <imgui_internal.h>
#include <xxhash.h>

namespace client
{

namespace thumbnails
{

// how many thumbnails are made at once (each decodes a file)
constexpr uint32_t kMaxMaking = 4;
// a thumbnail asked for but not drawn for this many frames is no longer made (it scrolled away): asked for again, it is
constexpr uint64_t kStaleFrames = 3;
// the cache files: a header, then the pixels. bumped when what is made changes
constexpr std::array<char, 4> kMagic{'T', 'H', 'M', '1'};

struct Header
{
	std::array<char, 4> magic = kMagic;
	uint32_t width = 0;
	uint32_t height = 0;
};

// a file's path, size and time, which a change to the file changes (or that it is missing)
[[nodiscard]] std::string Stamp(const std::filesystem::path& path)
{
	std::error_code error;
	auto size = std::filesystem::file_size(path, error);
	if (error)
		return std::format("{}|missing", path.string());
	auto time = std::filesystem::last_write_time(path, error);
	return std::format("{}|{}|{}", path.string(), size, error ? 0 : time.time_since_epoch().count());
}

// the cache file of a thumbnail: by what it is made from (its key, see Thumbnails::Make) and the slot size
[[nodiscard]] std::optional<std::filesystem::path> CachePath(const std::string& key, uint32_t slotSize, const std::filesystem::path& cacheDirectory)
{
	if (cacheDirectory.empty())
		return std::nullopt;
	auto text = std::format("{}|{}|{}", key, slotSize, std::string_view(kMagic.data(), kMagic.size()));
	return cacheDirectory / std::format("{:016x}.thumb", XXH3_64bits(text.data(), text.size()));
}

// what a model's preview capture shows depends on: the model file, every file its render reads (its buffers and
// material files, Dependencies, and its images, ImageFiles), what imports them (Model::ImportVersion, and the textures'
// in renderVersion), and what draws them (the shaders, in renderVersion). a change to any of them is a new key
[[nodiscard]] std::string CaptureKey(const std::filesystem::path& path, std::string_view renderVersion)
{
	// a zip archive holds everything its models' renders read, and they may be either kind
	auto extension = path.extension().string();
	std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (extension == ".zip")
		return std::format(
			"capture|{}|{}|{}|{}",
			Stamp(path),
			gfx::Model::ImportVersion("model.obj"),
			gfx::Model::ImportVersion("model.gltf"),
			renderVersion);

	auto key = std::format("capture|{}|{}|{}", Stamp(path), gfx::Model::ImportVersion(path.string()), renderVersion);
	for (const auto& files : {gfx::mesh::Dependencies(path), gfx::mesh::ImageFiles(path)})
		for (const auto& file : files)
			key.append(std::format("|{}", Stamp(file)));
	return key;
}

// how what the client draws looks, for captured thumbnails: the shader sources (every file of src/gfx/shaders, as the
// client compiles them at run time), what textures are imported with, and a tag to bump when the renderer's c++ changes
// how things look
[[nodiscard]] std::string RenderVersion()
{
	constexpr std::string_view kRendererTag = "renderer-v1";
	auto root = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["RootPath"]);
	std::vector<std::filesystem::path> files;
	std::error_code error;
	for (auto it = std::filesystem::recursive_directory_iterator(root / "src/gfx/shaders", error);
		 !error && it != std::filesystem::recursive_directory_iterator();
		 it.increment(error))
		if (it->is_regular_file(error))
			files.push_back(it->path());
	std::ranges::sort(files);
	XXH3_state_t* state = XXH3_createState();
	XXH3_64bits_reset(state);
	for (const auto& file : files)
	{
		auto name = file.filename().string();
		XXH3_64bits_update(state, name.data(), name.size());
		if (auto mapped = core::file::Map(file); mapped && mapped->size() > 0)
			XXH3_64bits_update(state, mapped->data(), mapped->size());
	}
	auto shaders = XXH3_64bits_digest(state);
	XXH3_freeState(state);
	return std::format("shaders-{:016x}|{}|{}", shaders, gfx::TextureImportVersion(), kRendererTag);
}

// rgba8 scaled down (by the mean of each pixel's box) so that its larger side is at most size; as it is if already
[[nodiscard]] std::vector<uint8_t> Shrink(std::span<const uint8_t> rgba, uint32_t& width, uint32_t& height, uint32_t size)
{
	if (width <= size && height <= size)
		return {rgba.begin(), rgba.end()};
	double scale = static_cast<double>(size) / std::max(width, height);
	auto outWidth = std::max(1U, static_cast<uint32_t>(std::lround(width * scale)));
	auto outHeight = std::max(1U, static_cast<uint32_t>(std::lround(height * scale)));
	std::vector<uint8_t> out(static_cast<size_t>(outWidth) * outHeight * 4);
	for (uint32_t y = 0; y < outHeight; y++)
	{
		uint32_t y0 = y * height / outHeight;
		uint32_t y1 = std::max(y0 + 1, (y + 1) * height / outHeight);
		for (uint32_t x = 0; x < outWidth; x++)
		{
			uint32_t x0 = x * width / outWidth;
			uint32_t x1 = std::max(x0 + 1, (x + 1) * width / outWidth);
			std::array<uint32_t, 4> sum{};
			for (uint32_t sy = y0; sy < y1; sy++)
				for (uint32_t sx = x0; sx < x1; sx++)
					for (uint32_t c = 0; c < 4; c++)
						sum[c] += rgba[((static_cast<size_t>(sy) * width + sx) * 4) + c];
			auto count = (y1 - y0) * (x1 - x0);
			for (uint32_t c = 0; c < 4; c++)
				out[((static_cast<size_t>(y) * outWidth + x) * 4) + c] = static_cast<uint8_t>(sum[c] / count);
		}
	}
	width = outWidth;
	height = outHeight;
	return out;
}

[[nodiscard]] uint8_t ToSrgb8(float linear)
{
	linear = std::clamp(linear, 0.0F, 1.0F);
	float encoded = linear <= 0.0031308F ? 12.92F * linear : (1.055F * std::pow(linear, 1.0F / 2.4F)) - 0.055F;
	return static_cast<uint8_t>(std::lround(encoded * 255.0F));
}

// a model's bundled screenshot: <model>/screenshot/screenshot.* next to its folder, or the folder above (the
// glTF-Sample-Assets keep a model's encodings in folders of their own)
[[nodiscard]] std::optional<std::filesystem::path> FindScreenshot(const std::filesystem::path& model)
{
	for (const auto& directory : {model.parent_path(), model.parent_path().parent_path()})
		for (const char* name : {"screenshot.jpg", "screenshot.png", "screenshot.jpeg", "screenshot.webp", "screenshot.gif"})
		{
			auto candidate = directory / "screenshot" / name;
			if (std::error_code error; std::filesystem::is_regular_file(candidate, error))
				return candidate;
		}
	return std::nullopt;
}

void Save(const std::filesystem::path& cachePath, std::span<const uint8_t> rgba, uint32_t width, uint32_t height)
{
	std::error_code error;
	std::filesystem::create_directories(cachePath.parent_path(), error);
	Header header{.width = width, .height = height};
	std::vector<std::byte> bytes(sizeof(Header) + rgba.size());
	std::memcpy(bytes.data(), &header, sizeof(Header));
	std::memcpy(bytes.data() + sizeof(Header), rgba.data(), rgba.size());
	if (auto written = core::file::Write(cachePath, bytes); !written)
		std::println(stderr, "Failed to cache thumbnail {}: {}", cachePath.string(), written.error().message());
}

} // namespace thumbnails

Thumbnails::Thumbnails(uint32_t slotSize, uint32_t columns, uint32_t rows, std::filesystem::path cacheDirectory)
	: mySlotSize(slotSize)
	, myColumns(columns)
	, myRows(rows)
	, myCacheDirectory(std::move(cacheDirectory))
	, myRenderVersion(thumbnails::RenderVersion())
	, myAtlas(std::make_unique<ImTextureData>())
	, mySlotOwners(static_cast<size_t>(columns) * rows)
	, mySlotUsed(static_cast<size_t>(columns) * rows, 0)
{
	myAtlas->Create(ImTextureFormat_RGBA32, static_cast<int>(slotSize * columns), static_cast<int>(slotSize * rows));
	ImGui::RegisterUserTexture(myAtlas.get());
	if (!myCacheDirectory.empty())
	{
		auto [task, future] = core::CreateTask([cacheDirectory = myCacheDirectory] { Prune(cacheDirectory); });
		core::Application::Get()->GetExecutor().Submit(std::span(&task, 1));
	}
}

void Thumbnails::Prune(const std::filesystem::path& cacheDirectory)
{
	ZoneScopedN("Thumbnails::Prune");

	constexpr int kUnusedDays = 30;
	auto oldest = std::filesystem::file_time_type::clock::now() - std::chrono::days(kUnusedDays);
	std::error_code error;
	uint32_t prunedCount = 0;
	for (std::filesystem::directory_iterator it(cacheDirectory, error), end; !error && it != end; it.increment(error))
	{
		if (it->path().extension() != ".thumb")
			continue;
		std::error_code fileError;
		auto time = it->last_write_time(fileError);
		if (!fileError && time < oldest && std::filesystem::remove(it->path(), fileError))
			prunedCount++;
	}
	if (prunedCount > 0)
		std::println("Pruned {} thumbnails unused for {} days from {}", prunedCount, kUnusedDays, cacheDirectory.string());
}

Thumbnails::~Thumbnails()
{
	// imgui's context (and its renderer, which destroyed the atlas's gpu texture) may be gone by now
	if (ImGui::GetCurrentContext() != nullptr)
		ImGui::UnregisterUserTexture(myAtlas.get());
}

std::optional<std::vector<uint8_t>> Thumbnails::Make(
	const std::filesystem::path& path,
	Source source,
	uint32_t slotSize,
	const std::filesystem::path& cacheDirectory,
	const std::string& renderVersion,
	uint32_t& width,
	uint32_t& height)
{
	using namespace thumbnails;

	ZoneScopedN("Thumbnails::Make");

	// what it is made from: an image or a panorama, the file; a model, its screenshot, else its preview's capture (see
	// CaptureKey: only ever cached, made by Put)
	auto screenshot = source == Source::kModel ? FindScreenshot(path) : std::nullopt;
	auto key = source == Source::kArchive ? CaptureKey(path, renderVersion)
			   : source != Source::kModel ? std::format("{}|{}", std::to_underlying(source), Stamp(path))
			   : screenshot				? std::format("screenshot|{}", Stamp(*screenshot))
										: CaptureKey(path, renderVersion);
	auto cachePath = CachePath(key, slotSize, cacheDirectory);
	if (cachePath)
		if (auto mapped = core::file::Map(*cachePath); mapped && mapped->size() >= sizeof(Header))
		{
			Header header;
			std::memcpy(&header, mapped->data(), sizeof(Header));
			if (header.magic == kMagic && mapped->size() == sizeof(Header) + (static_cast<size_t>(header.width) * header.height * 4))
			{
				// used: kept by Prune
				std::error_code error;
				std::filesystem::last_write_time(*cachePath, std::filesystem::file_time_type::clock::now(), error);
				width = header.width;
				height = header.height;
				const auto* pixels = reinterpret_cast<const uint8_t*>(mapped->data() + sizeof(Header));
				return std::vector<uint8_t>(pixels, pixels + (static_cast<size_t>(width) * height * 4));
			}
		}

	std::vector<uint8_t> rgba;
	switch (source)
	{
	case Source::kArchive: return std::nullopt; // a capture, only ever cached
	case Source::kModel:
	{
		if (!screenshot)
			return std::nullopt;
		auto pixels = gfx::image::Decode(*screenshot, {.usage = gfx::image::Usage::kColor});
		if (!pixels)
			return std::nullopt;
		width = pixels->width;
		height = pixels->height;
		rgba = std::move(pixels->rgba);
		break;
	}
	case Source::kImage:
	{
		auto pixels = gfx::image::Decode(path, {.usage = gfx::image::Usage::kColor});
		if (!pixels)
			return std::nullopt;
		width = pixels->width;
		height = pixels->height;
		rgba = std::move(pixels->rgba);
		break;
	}
	case Source::kEnvironment:
	{
		// small, and tonemapped (Reinhard) into srgb
		auto panorama = gfx::environment::Import(path, slotSize * 2);
		if (!panorama)
			return std::nullopt;
		width = panorama->width;
		height = panorama->height;
		rgba.resize(static_cast<size_t>(width) * height * 4);
		for (size_t texel = 0; texel < static_cast<size_t>(width) * height; texel++)
		{
			for (size_t c = 0; c < 3; c++)
			{
				float value = panorama->rgb[(texel * 3) + c];
				rgba[(texel * 4) + c] = ToSrgb8(value / (1.0F + value));
			}
			rgba[(texel * 4) + 3] = 255;
		}
		break;
	}
	}

	auto thumbnail = Shrink(rgba, width, height, slotSize);
	if (cachePath)
		Save(*cachePath, thumbnail, width, height);
	return thumbnail;
}

std::optional<Thumbnails::Region> Thumbnails::Get(const std::filesystem::path& path, Source source)
{
	auto key = path.string();
	auto [it, inserted] = myEntries.try_emplace(key);
	auto& entry = it->second;
	if (inserted)
	{
		entry.source = source;
		entry.path = path;
	}
	entry.lastSeen = myFrame;
	if (entry.state == Entry::State::kWaiting && !entry.queued)
	{
		myWaiting.push_front(key);
		entry.queued = true;
	}
	if (entry.state != Entry::State::kReady)
		return std::nullopt;

	mySlotUsed[entry.slot] = myFrame;
	auto atlasWidth = static_cast<float>(myAtlas->Width);
	auto atlasHeight = static_cast<float>(myAtlas->Height);
	ImVec2 origin(
		static_cast<float>((entry.slot % myColumns) * mySlotSize) / atlasWidth,
		static_cast<float>((entry.slot / myColumns) * mySlotSize) / atlasHeight);
	return Region{
		.texture = myAtlas->GetTexRef(),
		.uv0 = origin,
		.uv1 = ImVec2(origin.x + entry.uvMax.x, origin.y + entry.uvMax.y),
		.aspect = entry.aspect};
}

bool Thumbnails::Missing(const std::filesystem::path& path) const
{
	auto it = myEntries.find(path.string());
	return it != myEntries.end() && it->second.state == Entry::State::kMissing;
}

void Thumbnails::Put(const std::filesystem::path& path, std::vector<uint8_t> rgba, uint32_t width, uint32_t height)
{
	using namespace thumbnails;

	auto thumbnail = Shrink(rgba, width, height, mySlotSize);
	if (auto cachePath = CachePath(CaptureKey(path, myRenderVersion), mySlotSize, myCacheDirectory))
		Save(*cachePath, thumbnail, width, height);
	std::scoped_lock lock(myShared->mutex);
	myShared->made.push_back({.key = path.string(), .rgba = std::move(thumbnail), .width = width, .height = height});
}

void Thumbnails::Update()
{
	using namespace thumbnails;

	ZoneScopedN("Thumbnails::Update");

	myFrame++;

	// what the renderer uploaded since: as imgui does for its own textures
	if (myAtlas->Status == ImTextureStatus_OK)
	{
		myAtlas->Updates.resize(0);
		myAtlas->UpdateRect.x = myAtlas->UpdateRect.y = static_cast<unsigned short>(~0);
		myAtlas->UpdateRect.w = myAtlas->UpdateRect.h = 0;
	}

	// the thumbnails made since (after the ones still waiting for a slot), each into a free slot, else the one drawn
	// longest ago (whose thumbnail is made again, from the cache, when it is seen again), unless that one is in view: it
	// would be made again at once and take another in view, and with more in view than the atlas holds they flickered
	std::vector<Made> made = std::exchange(myUnplaced, {});
	{
		std::scoped_lock lock(myShared->mutex);
		std::ranges::move(myShared->made, std::back_inserter(made));
		myShared->made.clear();
	}
	for (auto& result : made)
	{
		auto it = myEntries.find(result.key);
		if (it == myEntries.end())
			continue;
		auto& entry = it->second;
		if (!result.rgba)
		{
			if (entry.state != Entry::State::kReady)
				entry.state = Entry::State::kMissing;
			continue;
		}

		uint32_t slot = 0;
		if (entry.state == Entry::State::kReady)
		{
			slot = entry.slot;
		}
		else
		{
			auto free = std::ranges::find_if(mySlotOwners, [](const std::string& owner) { return owner.empty(); });
			slot = free != mySlotOwners.end() ? static_cast<uint32_t>(free - mySlotOwners.begin())
											  : static_cast<uint32_t>(std::ranges::min_element(mySlotUsed) - mySlotUsed.begin());
			// drawn last frame (or just placed): in view. this one waits while it is seen too, else it is made again when it is
			if (!mySlotOwners[slot].empty() && mySlotUsed[slot] + 1 >= myFrame)
			{
				if (myFrame - entry.lastSeen <= 1)
					myUnplaced.push_back(std::move(result));
				else
					entry.state = Entry::State::kWaiting;
				continue;
			}
			if (!mySlotOwners[slot].empty())
				if (auto owner = myEntries.find(mySlotOwners[slot]); owner != myEntries.end())
				{
					owner->second.state = Entry::State::kWaiting;
					owner->second.queued = false;
				}
			mySlotOwners[slot] = result.key;
		}

		auto x = (slot % myColumns) * mySlotSize;
		auto y = (slot / myColumns) * mySlotSize;
		auto stride = static_cast<size_t>(myAtlas->Width) * 4;
		for (uint32_t row = 0; row < result.height; row++)
			std::memcpy(
				myAtlas->Pixels + ((y + row) * stride) + (static_cast<size_t>(x) * 4),
				result.rgba->data() + (static_cast<size_t>(row) * result.width * 4),
				static_cast<size_t>(result.width) * 4);
		if (myAtlas->Status != ImTextureStatus_WantCreate)
			ImTextureDataQueueUpload(myAtlas.get(), static_cast<int>(x), static_cast<int>(y), static_cast<int>(result.width), static_cast<int>(result.height));

		entry.state = Entry::State::kReady;
		entry.slot = slot;
		entry.aspect = static_cast<float>(result.width) / static_cast<float>(std::max(result.height, 1U));
		entry.uvMax = ImVec2(
			static_cast<float>(result.width) / static_cast<float>(myAtlas->Width),
			static_cast<float>(result.height) / static_cast<float>(myAtlas->Height));
		mySlotUsed[slot] = myFrame;
	}

	// start making the waiting ones still seen, newest first
	while (myShared->making.load() < kMaxMaking && !myWaiting.empty())
	{
		auto key = std::move(myWaiting.front());
		myWaiting.pop_front();
		auto it = myEntries.find(key);
		if (it == myEntries.end())
			continue;
		auto& entry = it->second;
		entry.queued = false;
		if (entry.state != Entry::State::kWaiting || myFrame - entry.lastSeen > kStaleFrames)
			continue;
		entry.state = Entry::State::kMaking;
		myShared->making++;
		auto [task, future] = core::CreateTask(
			[shared = myShared, key = std::move(key), path = entry.path, source = entry.source, slotSize = mySlotSize,
			 cacheDirectory = myCacheDirectory, renderVersion = myRenderVersion]
			{
				Made result{.key = key};
				result.rgba = Make(path, source, slotSize, cacheDirectory, renderVersion, result.width, result.height);
				{
					std::scoped_lock lock(shared->mutex);
					shared->made.push_back(std::move(result));
				}
				shared->making--;
			});
		core::Application::Get()->GetExecutor().Submit(std::span(&task, 1));
	}
}

} // namespace client
