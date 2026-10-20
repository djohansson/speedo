#include "windowedapplicationdetail.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace client
{

namespace browser
{

// what the browser lists: folders, and the files the client loads
enum class Kind : uint8_t
{
	kFolder,
	kModel,
	kArchive,
	kImage,
	kEnvironment,
};

struct Entry
{
	std::filesystem::path path;
	std::string name; // the file's
	std::string relative; // its path from the folder listed (its name, unless subfolders are included)
	Kind kind = Kind::kFolder;
	uintmax_t size = 0;
};

[[nodiscard]] std::string Lower(std::string text)
{
	std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

[[nodiscard]] std::optional<Kind> KindOf(const std::filesystem::directory_entry& entry)
{
	std::error_code error;
	if (entry.is_directory(error))
		return Kind::kFolder;
	if (!entry.is_regular_file(error))
		return std::nullopt;
	const auto& path = entry.path();
	auto extension = Lower(path.extension().string());
	if (mesh::IsModelFile(path))
		return Kind::kModel;
	if (extension == ".zip")
		return Kind::kArchive;
	if (extension == ".hdr")
		return Kind::kEnvironment;
	if (windowedapplication::IsImageFile(path))
		return Kind::kImage;
	return std::nullopt;
}

// what a listing gathers: a folder's subfolders and files, or with recursive the files below it (no folders), of the
// kinds shown. by default every model and zip archive below the folder: the quickest way to step through what can be
// previewed
struct ListOptions
{
	bool recursive = true;
	bool models = true;
	bool archives = true;
	bool images = false;
	bool environments = false;

	bool operator==(const ListOptions&) const = default;

	[[nodiscard]] bool Shows(Kind kind) const
	{
		switch (kind)
		{
		case Kind::kFolder: return !recursive;
		case Kind::kModel: return models;
		case Kind::kArchive: return archives;
		case Kind::kImage: return images;
		case Kind::kEnvironment: return environments;
		}
		return false;
	}
};

// a directory's entries (see ListOptions): folders first, then files, each by their path from it (case insensitive).
// hidden ones (and macOS resource forks) are left out
[[nodiscard]] std::vector<Entry> List(const std::filesystem::path& directory, const ListOptions& options)
{
	std::vector<Entry> entries;
	auto add = [&entries, &directory, &options](const std::filesystem::directory_entry& entry)
	{
		auto kind = KindOf(entry);
		if (!kind || !options.Shows(*kind))
			return;
		std::error_code error;
		auto size = *kind == Kind::kFolder ? 0 : entry.file_size(error);
		entries.push_back(
			{.path = entry.path(),
			 .name = entry.path().filename().string(),
			 .relative = entry.path().lexically_relative(directory).string(),
			 .kind = *kind,
			 .size = error ? 0 : size});
	};
	auto hidden = [](const std::filesystem::path& path)
	{
		auto name = path.filename().string();
		return name.starts_with('.') || name == "__MACOSX";
	};
	std::error_code error;
	constexpr auto kOptions = std::filesystem::directory_options::skip_permission_denied;
	if (options.recursive)
	{
		for (auto it = std::filesystem::recursive_directory_iterator(directory, kOptions, error);
			 !error && it != std::filesystem::recursive_directory_iterator();
			 it.increment(error))
		{
			if (hidden(it->path()))
			{
				it.disable_recursion_pending();
				continue;
			}
			add(*it);
		}
	}
	else
	{
		for (const auto& entry : std::filesystem::directory_iterator(directory, kOptions, error))
			if (!hidden(entry.path()))
				add(entry);
	}
	std::ranges::sort(
		entries,
		[](const Entry& a, const Entry& b)
		{
			if ((a.kind == Kind::kFolder) != (b.kind == Kind::kFolder))
				return a.kind == Kind::kFolder;
			return Lower(a.relative) < Lower(b.relative);
		});
	return entries;
}

// a listing made on the task executor (a big tree takes a while), handed to the ui thread once done
struct Listing
{
	std::atomic_bool done = false;
	std::vector<Entry> entries;
};

// the preview's load in progress, shared with its task
struct PreviewLoad
{
	std::atomic_bool loading = false;
	std::mutex mutex;
	// what it installs in the preview scene, once it is known (see Scene::LoadModels: a model's path, or a set's name),
	// and whether there was nothing to show (an archive without models, or a load that failed). guarded by mutex
	std::optional<std::string> model;
	bool empty = false;
};

// the browser's state, ui thread only
struct State
{
	std::filesystem::path directory;
	std::vector<Entry> entries;
	std::vector<bool> selected;
	std::optional<size_t> anchor; // the entry last clicked: what is previewed, and where shift-click ranges start
	std::optional<size_t> scrollTo; // the entry the arrow keys moved to, scrolled into view when drawn
	bool sideBySide = true;
	bool listed = false;
	ListOptions options; // the current purpose's (see BrowserPurpose)
	windowedapplication::BrowserPurpose purpose = windowedapplication::BrowserPurpose::kFiles;
	// each purpose's, while another's are current: an environment's lists every panorama below the folder
	std::array<ListOptions, 2> purposeOptions{
		ListOptions{},
		ListOptions{.recursive = true, .models = false, .archives = false, .images = false, .environments = true}};
	std::shared_ptr<Listing> listing; // the listing in progress, if any (a newer one replaces it, which drops its result)

	// the preview (see Preview): its image's texture, what it shows (the file last loaded into it), and what it should (the
	// focused entry, if it has a preview). one load at a time, the latest selection's next
	ImTextureID texture = ImTextureID_Invalid;
	std::optional<std::filesystem::path> previewWanted;
	std::filesystem::path previewShown;
	bool previewEnvironment = false; // a file's panorama, rather than the procedural sky
	std::shared_ptr<PreviewLoad> previewLoad = std::make_shared<PreviewLoad>();
	uint32_t previewReadyFrames = 0; // the frames the focused model's preview has been drawn in, loaded
};

// whether a kind has a preview in the preview scene: a model, a zip archive's models, or a panorama (an image is drawn as
// it is, see the image preview)
[[nodiscard]] bool HasScenePreview(Kind kind)
{
	return kind == Kind::kModel || kind == Kind::kArchive || kind == Kind::kEnvironment;
}

// what a kind's thumbnail is made from (folders have none: their icon)
[[nodiscard]] std::optional<Thumbnails::Source> ThumbnailSource(Kind kind)
{
	switch (kind)
	{
	case Kind::kModel: return Thumbnails::Source::kModel;
	case Kind::kArchive: return Thumbnails::Source::kArchive;
	case Kind::kImage: return Thumbnails::Source::kImage;
	case Kind::kEnvironment: return Thumbnails::Source::kEnvironment;
	default: return std::nullopt;
	}
}

// the largest rectangle of aspect (width over height) inside min to max, centered
[[nodiscard]] std::pair<ImVec2, ImVec2> Fit(ImVec2 min, ImVec2 max, float aspect)
{
	float width = max.x - min.x;
	float height = max.y - min.y;
	float fitWidth = std::min(width, height * aspect);
	float fitHeight = fitWidth / std::max(aspect, 1e-3F);
	ImVec2 center((min.x + max.x) * 0.5F, (min.y + max.y) * 0.5F);
	return {ImVec2(center.x - (fitWidth * 0.5F), center.y - (fitHeight * 0.5F)), ImVec2(center.x + (fitWidth * 0.5F), center.y + (fitHeight * 0.5F))};
}

[[nodiscard]] const char* Label(const Entry& entry)
{
	switch (entry.kind)
	{
	case Kind::kFolder: return "folder";
	case Kind::kArchive: return "zip";
	case Kind::kEnvironment: return "hdr";
	case Kind::kImage: return "image";
	case Kind::kModel:
	{
		auto extension = Lower(entry.path.extension().string());
		return extension == ".obj" ? "obj" : extension == ".glb" ? "glb" : "gltf";
	}
	}
	return "";
}

[[nodiscard]] ImU32 Color(Kind kind)
{
	switch (kind)
	{
	case Kind::kFolder: return IM_COL32(200, 160, 70, 255);
	case Kind::kModel: return IM_COL32(70, 130, 200, 255);
	case Kind::kArchive: return IM_COL32(130, 100, 170, 255);
	case Kind::kImage: return IM_COL32(80, 160, 110, 255);
	case Kind::kEnvironment: return IM_COL32(190, 110, 80, 255);
	}
	return IM_COL32(128, 128, 128, 255);
}

[[nodiscard]] std::string SizeText(uintmax_t size)
{
	if (size >= 1024ULL * 1024 * 1024)
		return std::format("{:.1f} GiB", static_cast<double>(size) / (1024.0 * 1024 * 1024));
	if (size >= 1024ULL * 1024)
		return std::format("{:.1f} MiB", static_cast<double>(size) / (1024.0 * 1024));
	return std::format("{:.1f} KiB", static_cast<double>(size) / 1024.0);
}

} // namespace browser

void WindowedApplication::InternalDrawBrowser()
{
	using namespace windowedapplication;
	using namespace browser;
	using namespace ImGui;

	static State gState;
	static bool gOpenOnStart = false;
	auto& state = gState;
	auto& rhi = GetRHI();

	// where it starts: SPEEDO_BROWSE=<dir> (absolute, or relative to the resource dir) opens it there at startup, with
	// SPEEDO_BROWSE_SELECT=<path>[;<path>...] selected (paths from the folder, see Entry::relative), and SPEEDO_BROWSE_OPEN=side-by-side or as-authored opens them
	// (automated tests and screenshots); else where the file dialogs open
	if (static bool gStarted = false; !gStarted)
	{
		gStarted = true;
		auto resourcePath = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["ResourcePath"]);
		std::error_code error;
		auto testAssets = resourcePath / "test-assets";
		state.directory = std::filesystem::is_directory(testAssets, error) ? testAssets : resourcePath;
		if (const char* browse = std::getenv("SPEEDO_BROWSE"); browse != nullptr && *browse != '\0')
		{
			state.directory = resourcePath / browse;
			gShowBrowser = true;
		}
		// SPEEDO_BROWSE_ENVIRONMENT=1: for an environment (as View > Environment > Open Environment...)
		if (const char* environment = std::getenv("SPEEDO_BROWSE_ENVIRONMENT"); environment != nullptr && std::string_view(environment) == "1")
			gBrowserPurpose = BrowserPurpose::kEnvironment;
		// SPEEDO_BROWSE_RECURSIVE=0: without subfolders, and SPEEDO_BROWSE_KINDS=<models,archives,images,panoramas> the
		// kinds listed (by default every model and zip archive below the folder)
		if (const char* recursive = std::getenv("SPEEDO_BROWSE_RECURSIVE"); recursive != nullptr && *recursive != '\0')
			state.options.recursive = std::string_view(recursive) != "0";
		if (const char* kinds = std::getenv("SPEEDO_BROWSE_KINDS"); kinds != nullptr && *kinds != '\0')
		{
			std::string_view list(kinds);
			state.options.models = list.contains("models");
			state.options.archives = list.contains("archives");
			state.options.images = list.contains("images");
			state.options.environments = list.contains("panoramas");
		}
	}

	if (!gShowBrowser)
	{
		SetPreviewVisible(false);
		return;
	}

	// a new purpose: its own options, and a new listing
	if (gBrowserPurpose != state.purpose)
	{
		state.purposeOptions[static_cast<size_t>(state.purpose)] = state.options;
		state.purpose = gBrowserPurpose;
		state.options = state.purposeOptions[static_cast<size_t>(state.purpose)];
		state.listed = false;
	}
	const bool environment = state.purpose == BrowserPurpose::kEnvironment;

	auto navigate = [&state](std::filesystem::path directory)
	{
		state.directory = std::move(directory);
		state.listed = false;
	};
	if (!state.listed)
	{
		state.listed = true;
		state.listing = std::make_shared<Listing>();
		auto [task, future] = core::CreateTask(
			[listing = state.listing, directory = state.directory, options = state.options]
			{
				listing->entries = List(directory, options);
				listing->done.store(true, std::memory_order_release);
			});
		GetExecutor().Submit(std::span(&task, 1));
	}
	if (state.listing && state.listing->done.load(std::memory_order_acquire))
	{
		state.entries = std::move(state.listing->entries);
		state.listing.reset();
		state.selected.assign(state.entries.size(), false);
		state.anchor.reset();
		if (static bool gSelected = false; !gSelected)
		{
			gSelected = true;
			if (const char* select = std::getenv("SPEEDO_BROWSE_SELECT"); select != nullptr && *select != '\0')
				for (auto name : std::string_view(select) | std::views::split(';'))
					if (auto it = std::ranges::find(state.entries, std::string_view(name.begin(), name.end()), &Entry::relative);
						it != state.entries.end())
					{
						auto index = static_cast<size_t>(it - state.entries.begin());
						state.selected[index] = true;
						state.anchor = index;
						state.scrollTo = index;
					}
			if (const char* openNow = std::getenv("SPEEDO_BROWSE_OPEN"); openNow != nullptr && *openNow != '\0')
			{
				state.sideBySide = std::string_view(openNow) != "as-authored";
				gOpenOnStart = true;
			}
		}
	}

	// the thumbnails: the tiles' (160 pixels, cached in the user profile), and the image preview's (one, not cached)
	if (!myThumbnails)
	{
		auto userProfilePath = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["UserProfilePath"]);
		constexpr uint32_t kThumbnailSize = 160;
		// 576 slots (a 3840 pixel texture): more than the tiles in view of a browser filling a 4K window (about 300)
		constexpr uint32_t kAtlasSlots = 24;
		myThumbnails = std::make_unique<Thumbnails>(kThumbnailSize, kAtlasSlots, kAtlasSlots, userProfilePath / "thumbnails");
		constexpr uint32_t kImagePreviewSize = 768;
		myImagePreview = std::make_unique<Thumbnails>(kImagePreviewSize, 1, 1, std::filesystem::path());
	}
	myThumbnails->Update();
	myImagePreview->Update();

	// the preview: made by the draw thread once asked for, then lit by the procedural sky
	RequestPreview();
	auto* preview = GetPreview();
	if (preview && state.texture == ImTextureID_Invalid)
	{
		state.texture = myImGui->AddTexture(preview->GetOutput().GetView());
		(void)gLoads.Enqueue(
			"Preview: procedural sky",
			[preview](std::atomic_uint8_t& progress) { preview->GetScene().LoadEnvironment(std::nullopt, progress); });
	}

	// opens the selection: one model as it is, several as one (side by side, or as authored), and the rest each by its
	// type (archives offer their models, see ArchiveModels::kChoose)
	auto open = [this, &state, &rhi]
	{
		std::vector<std::string> models;
		for (size_t entryIt = 0; entryIt < state.entries.size(); entryIt++)
		{
			if (!state.selected[entryIt])
				continue;
			const auto& entry = state.entries[entryIt];
			if (entry.kind == Kind::kModel)
				models.push_back(entry.path.string());
			else if (entry.kind != Kind::kFolder)
				(void)gLoads.Enqueue(
					entry.path.string(),
					[&rhi, path = entry.path.string()](std::atomic_uint8_t& progress)
					{ LoadAndInstallFile(rhi, path, progress, ArchiveModels::kChoose); });
		}
		if (models.size() == 1)
			(void)gLoads.Enqueue(
				models.front(), [&rhi, path = models.front()](std::atomic_uint8_t& progress) { LoadAndInstallModel(rhi, path, progress); });
		else if (models.size() > 1)
			(void)gLoads.Enqueue(
				std::format("{} models", models.size()),
				[models, arrangement = state.sideBySide ? ModelArrangement::kSideBySide : ModelArrangement::kAsAuthored](
					std::atomic_uint8_t& progress) { App().GetScene().LoadModels(models, progress, {}, arrangement); });
		gShowBrowser = false;
	};

	if (std::exchange(gOpenOnStart, false))
		open();

	// centered in the window below the menu bar, at three quarters of it, each time it opens (moving or resizing it holds
	// until it is closed)
	{
		constexpr float kWindowFraction = 0.75F;
		const auto* viewport = GetMainViewport();
		SetNextWindowPos(
			ImVec2(viewport->WorkPos.x + (0.5F * viewport->WorkSize.x), viewport->WorkPos.y + (0.5F * viewport->WorkSize.y)),
			ImGuiCond_Appearing,
			ImVec2(0.5F, 0.5F));
		SetNextWindowSize(ImVec2(viewport->WorkSize.x * kWindowFraction, viewport->WorkSize.y * kWindowFraction), ImGuiCond_Appearing);
	}
	if (Begin(environment ? "Open Environment###Browser" : "Open###Browser", &gShowBrowser))
	{
		// escape closes it while it (or one of its children) has the focus (see CloseOnEscape)
		CloseOnEscape(gShowBrowser);

		// where: up, and the path's parts, each a way back
		if (Button("Up") && state.directory.has_parent_path() && state.directory.parent_path() != state.directory)
			navigate(state.directory.parent_path());
		SameLine();
		if (Button("Refresh"))
			state.listed = false;
		// the parts that fit, from the last: the ones before them behind "...", which goes to the last of them (and shows
		// them as its tooltip; a deep path ran off the window's edge)
		std::vector<std::pair<std::string, std::filesystem::path>> parts;
		{
			std::filesystem::path partial;
			for (const auto& part : state.directory)
			{
				partial /= part;
				if (auto label = part.string(); !label.empty())
					parts.emplace_back(std::move(label), partial);
			}
		}
		const auto& style = GetStyle();
		auto buttonWidth = [&style](const char* label) { return CalcTextSize(label).x + (2.0F * style.FramePadding.x) + style.ItemSpacing.x; };
		SameLine();
		float available = GetContentRegionAvail().x + style.ItemSpacing.x; // what is left of the row
		size_t first = parts.size();
		float used = 0.0F;
		while (first > 0 && used + buttonWidth(parts[first - 1].first.c_str()) + (first > 1 ? buttonWidth("...") : 0.0F) <= available)
			used += buttonWidth(parts[--first].first.c_str());
		if (first == parts.size() && first > 0)
			first--; // the folder itself, even if it doesn't fit
		if (first > 0)
		{
			if (SmallButton("..."))
				navigate(parts[first - 1].second);
			SetItemTooltip("%s", parts[first - 1].second.string().c_str());
		}
		for (size_t partIt = first; partIt < parts.size(); partIt++)
		{
			if (partIt > first || first > 0)
				SameLine();
			PushID(parts[partIt].second.string().c_str());
			if (SmallButton(parts[partIt].first.c_str()))
				navigate(parts[partIt].second);
			PopID();
		}

		// what is listed: with subfolders, every file below the folder (to step through without entering them), of the kinds
		// checked
		auto options = state.options;
		Checkbox("Include subfolders", &options.recursive);
		SetItemTooltip("Lists the files in every folder below this one, rather than its folders.");
		SameLine();
		BeginDisabled(environment);
		Checkbox("Models", &options.models);
		SameLine();
		Checkbox("Archives", &options.archives);
		SameLine();
		Checkbox("Images", &options.images);
		SameLine();
		Checkbox("Panoramas", &options.environments);
		EndDisabled();
		if (options != state.options)
		{
			state.options = options;
			state.listed = false;
		}
		SameLine();
		if (state.listing)
			TextDisabled("Listing...");
		else
			TextDisabled("%zu entries", state.entries.size());

		const float footer = GetFrameHeightWithSpacing() + GetStyle().ItemSpacing.y;
		const float previewWidth = std::min(380.0F, GetContentRegionAvail().x * 0.4F);

		// the folders and files, as tiles: click selects, ctrl (or cmd) click adds or removes, shift click selects a range,
		// double click opens (a folder: goes into it)
		BeginChild("Files", ImVec2(-previewWidth - GetStyle().ItemSpacing.x, -footer), ImGuiChildFlags_Borders);
		{
			const float tile = 96.0F;
			const float labelHeight = GetTextLineHeightWithSpacing();
			const float spacing = GetStyle().ItemSpacing.x;
			auto columns = std::max(1, static_cast<int>((GetContentRegionAvail().x + spacing) / (tile + spacing)));
			auto* drawList = GetWindowDrawList();
			std::optional<size_t> enter;
			std::optional<size_t> openNow;

			// the arrow keys step through the entries (left and right by one, up and down by a row), enter opens: while the
			// browser has the focus, any part of it (it gets it when it opens, this list only when clicked)
			if (IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !state.entries.empty())
			{
				auto count = static_cast<std::ptrdiff_t>(state.entries.size());
				std::ptrdiff_t step = IsKeyPressed(ImGuiKey_LeftArrow)	 ? -1
									  : IsKeyPressed(ImGuiKey_RightArrow) ? 1
									  : IsKeyPressed(ImGuiKey_UpArrow)	 ? -columns
									  : IsKeyPressed(ImGuiKey_DownArrow)	 ? columns
																		 : 0;
				if (step != 0)
				{
					auto index = state.anchor ? std::clamp(static_cast<std::ptrdiff_t>(*state.anchor) + step, std::ptrdiff_t{0}, count - 1) : 0;
					state.selected.assign(state.entries.size(), false);
					state.selected[static_cast<size_t>(index)] = true;
					state.anchor = static_cast<size_t>(index);
					state.scrollTo = static_cast<size_t>(index);
				}
				if (IsKeyPressed(ImGuiKey_Enter) && state.anchor)
				{
					if (state.entries[*state.anchor].kind == Kind::kFolder)
						enter = *state.anchor;
					else
						openNow = *state.anchor;
				}
			}

			// only the rows in view: tiles out of view must not ask for their thumbnails (they would all count as just drawn,
			// and with more than the atlas holds, each new one would evict one in view, which flickered), nor be built
			auto columnCount = static_cast<size_t>(columns);
			ImGuiListClipper clipper;
			clipper.Begin(
				static_cast<int>((state.entries.size() + columnCount - 1) / columnCount), tile + labelHeight + GetStyle().ItemSpacing.y);
			if (state.scrollTo)
				clipper.IncludeItemByIndex(static_cast<int>(*state.scrollTo / columnCount));
			while (clipper.Step())
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
					for (size_t entryIt = static_cast<size_t>(row) * columnCount;
						 entryIt < std::min(state.entries.size(), (static_cast<size_t>(row) + 1) * columnCount);
						 entryIt++)
					{
						const auto& entry = state.entries[entryIt];
						PushID(static_cast<int>(entryIt));
						if (entryIt % static_cast<size_t>(columns) != 0)
							SameLine();
						ImVec2 corner = GetCursorScreenPos();
						bool selected = state.selected[entryIt];
						if (Selectable("##tile", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(tile, tile + labelHeight)))
						{
							const auto& io = GetIO();
							if (io.KeyShift && state.anchor)
							{
								auto [first, last] = std::minmax(*state.anchor, entryIt);
								if (!io.KeyCtrl && !io.KeySuper)
									state.selected.assign(state.entries.size(), false);
								for (size_t rangeIt = first; rangeIt <= last; rangeIt++)
									state.selected[rangeIt] = true;
							}
							else if (io.KeyCtrl || io.KeySuper)
							{
								state.selected[entryIt] = !selected;
								state.anchor = entryIt;
							}
							else
							{
								state.selected.assign(state.entries.size(), false);
								state.selected[entryIt] = true;
								state.anchor = entryIt;
							}
							if (IsMouseDoubleClicked(ImGuiMouseButton_Left))
							{
								if (entry.kind == Kind::kFolder)
									enter = entryIt;
								else
									openNow = entryIt;
							}
						}
						if (state.scrollTo == entryIt)
						{
							SetScrollHereY(0.5F);
							state.scrollTo.reset();
						}
						if (IsItemHovered(ImGuiHoveredFlags_DelayNormal))
							SetTooltip("%s", entry.relative.c_str());

						// the thumbnail (fitted to the tile), else an icon of its kind; and the name under it, clipped to the tile
						constexpr float kPad = 8.0F;
						ImVec2 iconMin(corner.x + kPad, corner.y + kPad);
						ImVec2 iconMax(corner.x + tile - kPad, corner.y + tile - kPad);
						auto source = ThumbnailSource(entry.kind);
						auto thumbnail = source ? myThumbnails->Get(entry.path, *source) : std::nullopt;
						if (thumbnail)
						{
							auto [min, max] = Fit(iconMin, iconMax, thumbnail->aspect);
							drawList->AddImage(thumbnail->texture, min, max, thumbnail->uv0, thumbnail->uv1);
						}
						else
						{
							drawList->AddRectFilled(iconMin, iconMax, Color(entry.kind), 6.0F);
							const char* label = Label(entry);
							ImVec2 labelSize = CalcTextSize(label);
							drawList->AddText(
								ImVec2((iconMin.x + iconMax.x - labelSize.x) * 0.5F, (iconMin.y + iconMax.y - labelSize.y) * 0.5F),
								IM_COL32(255, 255, 255, 230),
								label);
						}
						drawList->PushClipRect(ImVec2(corner.x, corner.y + tile), ImVec2(corner.x + tile, corner.y + tile + labelHeight), true);
						ImVec2 nameSize = CalcTextSize(entry.name.c_str());
						drawList->AddText(
							ImVec2(corner.x + std::max(0.0F, (tile - nameSize.x) * 0.5F), corner.y + tile),
							GetColorU32(ImGuiCol_Text),
							entry.name.c_str());
						drawList->PopClipRect();
						PopID();
					}
			if (enter)
				navigate(state.entries[*enter].path);
			else if (openNow)
			{
				state.selected.assign(state.entries.size(), false);
				state.selected[*openNow] = true;
				open();
			}
		}
		EndChild();
		SameLine();

		// the preview: the entry last clicked, if a model, a zip archive's models or a panorama, drawn off-screen (one load at
		// a time: the latest selection's goes next)
		BeginChild("Preview", ImVec2(0, -footer), ImGuiChildFlags_Borders);
		{
			const Entry* focus = state.listed && state.anchor && *state.anchor < state.entries.size() ? &state.entries[*state.anchor] : nullptr;
			// nothing wanted when the focused entry has no preview (a folder, an image), so the last one isn't shown for it
			if (!focus || !HasScenePreview(focus->kind))
				state.previewWanted.reset();
			else
				state.previewWanted = focus->path;
			auto& load = *state.previewLoad;
			if (preview && !load.loading.load() && state.previewWanted && *state.previewWanted != state.previewShown)
			{
				auto path = *state.previewWanted;
				auto extension = Lower(path.extension().string());
				bool environment = extension == ".hdr";
				bool archive = extension == ".zip";
				bool restoreSky = !environment && state.previewEnvironment;
				state.previewShown = path;
				state.previewEnvironment = environment;
				{
					std::scoped_lock lock(load.mutex);
					load.model.reset();
					load.empty = false;
				}
				load.loading.store(true);
				(void)gLoads.Enqueue(
					std::format("Preview: {}", path.filename().string()),
					[preview, path, environment, archive, restoreSky, load = state.previewLoad](std::atomic_uint8_t& progress)
					{
						std::optional<std::string> model;
						if (environment)
						{
							preview->GetScene().LoadEnvironment(path.string(), progress);
						}
						else
						{
							if (restoreSky)
								preview->GetScene().LoadEnvironment(std::nullopt, progress);
							// an archive's models side by side, extracted (once, as opening it does)
							std::vector<std::string> models;
							if (!archive)
								models.push_back(path.string());
							else if (auto directory = ExtractArchive(path, progress))
								for (const auto& found : FindModels(*directory))
									models.push_back(found.string());
							if (!models.empty())
								model = preview->GetScene().LoadModels(models, progress);
						}
						{
							std::scoped_lock lock(load->mutex);
							load->empty = !environment && !model;
							load->model = std::move(model);
						}
						load->loading.store(false);
					});
			}

			// whether the preview shows the focused entry: its load done, and a model's (or an archive's) installed, which
			// the scene's file path says (the load returns before the draw thread installs it)
			std::optional<std::string> loadedModel;
			bool loadedEmpty = false;
			{
				std::scoped_lock lock(load.mutex);
				loadedModel = load.model;
				loadedEmpty = load.empty;
			}
			bool focusLoaded = preview && focus && HasScenePreview(focus->kind) && state.previewShown == focus->path && !load.loading.load();
			bool previewReady =
				focusLoaded && (focus->kind == Kind::kEnvironment ||
								(loadedModel && preview->GetScene().GetScenes().Read().Get().filePath == *loadedModel));

			// a model without a bundled screenshot, or an archive, gets its thumbnail from the preview, once it has been
			// drawn a while (counted from its install: a failed load installs nothing, and the previous model would be
			// captured as this one's)
			bool captureReady = previewReady && focus->kind != Kind::kEnvironment;
			state.previewReadyFrames = captureReady ? state.previewReadyFrames + 1 : 0;
			constexpr uint32_t kCaptureAfterFrames = 20;
			if (captureReady && state.previewReadyFrames == kCaptureAfterFrames && myThumbnails->Missing(focus->path))
				preview->RequestCapture(
					[thumbnails = myThumbnails.get(), path = focus->path](std::vector<uint8_t> rgba, uint32_t width, uint32_t height)
					{ thumbnails->Put(path, std::move(rgba), width, height); });

			float side = std::max(GetContentRegionAvail().x, 1.0F);
			auto imagePreview = focus && focus->kind == Kind::kImage ? myImagePreview->Get(focus->path, Thumbnails::Source::kImage)
																	 : std::nullopt;
			if (focus && focus->kind == Kind::kImage)
			{
				// an image: itself, fitted to the panel
				ImVec2 corner = GetCursorScreenPos();
				Dummy(ImVec2(side, side));
				if (imagePreview)
				{
					auto [min, max] = Fit(corner, ImVec2(corner.x + side, corner.y + side), imagePreview->aspect);
					GetWindowDrawList()->AddImage(imagePreview->texture, min, max, imagePreview->uv0, imagePreview->uv1);
				}
				else
				{
					GetWindowDrawList()->AddText(ImVec2(corner.x + 8.0F, corner.y + 8.0F), IM_COL32(255, 255, 255, 230), "Loading...");
				}
			}
			else if (focus && HasScenePreview(focus->kind) && state.texture != ImTextureID_Invalid)
			{
				// the preview once it shows the focused entry; until then an empty square, rather than what it showed before
				ImVec2 corner = GetCursorScreenPos();
				if (previewReady)
					ImGui::Image(ImTextureRef(state.texture), ImVec2(side, side));
				else
					Dummy(ImVec2(side, side));
				if (!previewReady)
					GetWindowDrawList()->AddText(
						ImVec2(corner.x + 8.0F, corner.y + 8.0F),
						IM_COL32(255, 255, 255, 230),
						focusLoaded && loadedEmpty ? "No models to preview." : "Loading...");
			}
			else
			{
				Dummy(ImVec2(side, side * 0.25F));
				PushStyleColor(ImGuiCol_Text, GetStyleColorVec4(ImGuiCol_TextDisabled));
				TextWrapped("Select a model, a zip archive, an image or a panorama to preview it.");
				PopStyleColor();
			}

			// what is selected
			size_t selectedCount = 0;
			size_t modelCount = 0;
			for (size_t entryIt = 0; entryIt < state.entries.size(); entryIt++)
				if (state.selected[entryIt])
				{
					selectedCount++;
					modelCount += state.entries[entryIt].kind == Kind::kModel ? 1 : 0;
				}
			if (focus)
			{
				TextWrapped("%s", focus->relative.c_str());
				if (focus->kind != Kind::kFolder)
					TextDisabled("%s, %s", Label(*focus), SizeText(focus->size).c_str());
			}
			if (selectedCount > 1)
				TextDisabled("%zu selected, %zu models", selectedCount, modelCount);
		}
		EndChild();

		// open: several models side by side, or as authored
		size_t models = 0;
		size_t files = 0;
		for (size_t entryIt = 0; entryIt < state.entries.size(); entryIt++)
			if (state.selected[entryIt] && state.entries[entryIt].kind != Kind::kFolder)
			{
				files++;
				models += state.entries[entryIt].kind == Kind::kModel ? 1 : 0;
			}
		if (!environment)
		{
			if (Button("Select All Models"))
				for (size_t entryIt = 0; entryIt < state.entries.size(); entryIt++)
					state.selected[entryIt] = state.entries[entryIt].kind == Kind::kModel;
			SameLine();
			BeginDisabled(models < 2);
			Checkbox("Side by side", &state.sideBySide);
			EndDisabled();
			SetItemTooltip("Several models: side by side, each scaled to the same size (a set of variants), or as authored, each where it is (the parts of one scene).");
			SameLine();
		}
		BeginDisabled(files == 0);
		if (Button("Open"))
			open();
		EndDisabled();
	}
	End();

	SetPreviewVisible(gShowBrowser && state.texture != ImTextureID_Invalid);
}

} // namespace client
