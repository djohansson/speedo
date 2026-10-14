#pragma once

#include <core/utils.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

namespace client
{

// small rgba8 pictures (srgb encoded, as the ui draws them) of files, kept in one imgui texture: an atlas of square slots,
// reused least recently drawn first. made on the task executor (an image or a panorama decoded and scaled down, a
// model's bundled screenshot), or handed in (a model's preview, see Preview::RequestCapture), and cached on disk by path,
// size and time. one atlas serves the browser's tiles (many small slots), another its image preview (one big slot)
class Thumbnails final
{
public:
	// what a file's thumbnail is made from
	enum class Source : uint8_t
	{
		kImage, // the image
		kEnvironment, // the panorama, tonemapped
		kModel, // the screenshot next to it (glTF-Sample-Assets: <model>/screenshot/screenshot.*), else a capture (Put)
		kArchive, // a zip archive of models: a capture of its preview (Put)
	};

	// where a thumbnail is in the atlas, and its aspect ratio (width over height)
	struct Region
	{
		ImTextureRef texture;
		ImVec2 uv0;
		ImVec2 uv1;
		float aspect = 1.0F;
	};

	// slotSize: a thumbnail's largest side, in pixels. cacheDirectory: where they are cached (none: not cached). makes
	// and registers the atlas texture: with imgui's context current (ui thread)
	Thumbnails(uint32_t slotSize, uint32_t columns, uint32_t rows, std::filesystem::path cacheDirectory);
	// the atlas texture is left to imgui's renderer, which destroys it with its context
	~Thumbnails();

	Thumbnails(const Thumbnails&) = delete;
	Thumbnails& operator=(const Thumbnails&) = delete;

	// a file's thumbnail, if it is in the atlas; else starts making it (once: from the cache, else its source). ui thread
	[[nodiscard]] std::optional<Region> Get(const std::filesystem::path& path, Source source);
	// whether a file's thumbnail couldn't be made from its source (a model without a screenshot): Put can still give it
	[[nodiscard]] bool Missing(const std::filesystem::path& path) const;
	// a thumbnail made elsewhere (a model's preview capture): rgba8, any size, scaled down and cached. any thread
	void Put(const std::filesystem::path& path, std::vector<uint8_t> rgba, uint32_t width, uint32_t height);

	// takes the thumbnails made since into the atlas, and starts more of the waiting ones. ui thread, once a frame, before
	// the frame is ended (imgui's renderer uploads the atlas then)
	void Update();

private:
	struct Made
	{
		std::string key; // the path
		std::optional<std::vector<uint8_t>> rgba; // none if it couldn't be made
		uint32_t width = 0;
		uint32_t height = 0;
	};
	struct Entry
	{
		enum class State : uint8_t
		{
			kWaiting,
			kMaking,
			kReady,
			kMissing,
		} state = State::kWaiting;
		Source source = Source::kImage;
		std::filesystem::path path;
		uint32_t slot = 0;
		float aspect = 1.0F;
		ImVec2 uvMax; // of the slot's used part
		uint64_t lastSeen = 0; // the frame Get last asked for it in
		bool queued = false; // in myWaiting
	};

	// made on the task executor: from the cache, else the source (cached then). the cache is keyed by what the thumbnail
	// is made from, so a change to any of it makes a new one: the file (an image's, a panorama's, a model's screenshot),
	// or for a capture its file (a model, or a zip archive), the files its render reads, its importers, and renderVersion
	static std::optional<std::vector<uint8_t>> Make(
		const std::filesystem::path& path,
		Source source,
		uint32_t slotSize,
		const std::filesystem::path& cacheDirectory,
		const std::string& renderVersion,
		uint32_t& width,
		uint32_t& height);

	uint32_t mySlotSize;
	uint32_t myColumns;
	uint32_t myRows;
	std::filesystem::path myCacheDirectory;
	// how what the client draws looks (the shader sources, the texture importer, a renderer tag): part of a captured
	// thumbnail's key
	std::string myRenderVersion;
	std::unique_ptr<ImTextureData> myAtlas;

	// ui thread
	core::UnorderedMap<std::string, Entry> myEntries;
	std::deque<std::string> myWaiting; // newest first: what was asked for last is likely what is seen
	std::vector<std::string> mySlotOwners; // per slot, the key holding it (empty: free)
	std::vector<uint64_t> mySlotUsed; // per slot, the frame it was last drawn in
	std::vector<Made> myUnplaced; // made, but every slot it could take was in view (see Update)
	uint64_t myFrame = 0;

	// shared with the tasks making thumbnails
	struct Shared
	{
		std::mutex mutex;
		std::vector<Made> made; // guarded by mutex
		std::atomic_uint32_t making = 0;
	};
	std::shared_ptr<Shared> myShared = std::make_shared<Shared>();
};

} // namespace client
