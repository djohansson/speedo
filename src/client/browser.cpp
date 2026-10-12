#include "windowedapplicationdetail.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
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
	std::string name;
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

// a directory's folders, then its files the client loads, each by name (case insensitive). hidden ones are left out
[[nodiscard]] std::vector<Entry> List(const std::filesystem::path& directory)
{
	std::vector<Entry> entries;
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(directory, std::filesystem::directory_options::skip_permission_denied, error))
	{
		auto name = entry.path().filename().string();
		if (name.starts_with('.') || name == "__MACOSX")
			continue;
		auto kind = KindOf(entry);
		if (!kind)
			continue;
		std::error_code sizeError;
		auto size = *kind == Kind::kFolder ? 0 : entry.file_size(sizeError);
		entries.push_back({.path = entry.path(), .name = std::move(name), .kind = *kind, .size = sizeError ? 0 : size});
	}
	std::ranges::sort(
		entries,
		[](const Entry& a, const Entry& b)
		{
			if ((a.kind == Kind::kFolder) != (b.kind == Kind::kFolder))
				return a.kind == Kind::kFolder;
			return Lower(a.name) < Lower(b.name);
		});
	return entries;
}

// the browser's state, ui thread only
struct State
{
	std::filesystem::path directory;
	std::vector<Entry> entries;
	std::vector<bool> selected;
	std::optional<size_t> anchor; // the entry last clicked: what is previewed, and where shift-click ranges start
	bool sideBySide = true;
	bool listed = false;

	// the preview (see Preview): its image's texture, what it shows, and what it should. one load at a time, the
	// latest selection's next
	ImTextureID texture = ImTextureID_Invalid;
	std::optional<std::filesystem::path> previewWanted;
	std::filesystem::path previewShown;
	bool previewEnvironment = false; // a file's panorama, rather than the procedural sky
	std::shared_ptr<std::atomic_bool> previewLoading = std::make_shared<std::atomic_bool>(false);
};

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
	// SPEEDO_BROWSE_SELECT=<name>[;<name>...] selected, and SPEEDO_BROWSE_OPEN=side-by-side or as-authored opens them
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
	}

	if (!gShowBrowser)
	{
		SetPreviewVisible(false);
		return;
	}

	auto navigate = [&state](std::filesystem::path directory)
	{
		state.directory = std::move(directory);
		state.listed = false;
	};
	if (!state.listed)
	{
		state.entries = List(state.directory);
		state.selected.assign(state.entries.size(), false);
		state.anchor.reset();
		state.listed = true;
		if (static bool gSelected = false; !gSelected)
		{
			gSelected = true;
			if (const char* select = std::getenv("SPEEDO_BROWSE_SELECT"); select != nullptr && *select != '\0')
				for (auto name : std::string_view(select) | std::views::split(';'))
					if (auto it = std::ranges::find(state.entries, std::string_view(name.begin(), name.end()), &Entry::name);
						it != state.entries.end())
					{
						auto index = static_cast<size_t>(it - state.entries.begin());
						state.selected[index] = true;
						state.anchor = index;
					}
			if (const char* openNow = std::getenv("SPEEDO_BROWSE_OPEN"); openNow != nullptr && *openNow != '\0')
			{
				state.sideBySide = std::string_view(openNow) != "as-authored";
				gOpenOnStart = true;
			}
		}
	}

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

	SetNextWindowSize(ImVec2(1000, 640), ImGuiCond_FirstUseEver);
	if (Begin("Browse", &gShowBrowser))
	{
		// where: up, and the path's parts, each a way back
		if (Button("Up") && state.directory.has_parent_path() && state.directory.parent_path() != state.directory)
			navigate(state.directory.parent_path());
		SameLine();
		if (Button("Refresh"))
			state.listed = false;
		std::filesystem::path partial;
		for (const auto& part : state.directory)
		{
			partial /= part;
			auto label = part.string();
			if (label.empty())
				continue;
			SameLine();
			PushID(partial.string().c_str());
			if (SmallButton(label.c_str()))
				navigate(partial);
			PopID();
		}

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
			for (size_t entryIt = 0; entryIt < state.entries.size(); entryIt++)
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
				if (IsItemHovered(ImGuiHoveredFlags_DelayNormal))
					SetTooltip("%s", entry.name.c_str());

				// the icon (a thumbnail later), and the name under it, clipped to the tile
				constexpr float kPad = 8.0F;
				ImVec2 iconMin(corner.x + kPad, corner.y + kPad);
				ImVec2 iconMax(corner.x + tile - kPad, corner.y + tile - kPad);
				drawList->AddRectFilled(iconMin, iconMax, Color(entry.kind), 6.0F);
				const char* label = Label(entry);
				ImVec2 labelSize = CalcTextSize(label);
				drawList->AddText(
					ImVec2((iconMin.x + iconMax.x - labelSize.x) * 0.5F, (iconMin.y + iconMax.y - labelSize.y) * 0.5F),
					IM_COL32(255, 255, 255, 230),
					label);
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

		// the preview: the entry last clicked, if a model or a panorama, drawn off-screen (one load at a time: the latest
		// selection's goes next)
		BeginChild("Preview", ImVec2(0, -footer), ImGuiChildFlags_Borders);
		{
			const Entry* focus = state.listed && state.anchor && *state.anchor < state.entries.size() ? &state.entries[*state.anchor] : nullptr;
			if (focus && (focus->kind == Kind::kModel || focus->kind == Kind::kEnvironment))
				state.previewWanted = focus->path;
			if (preview && !state.previewLoading->load() && state.previewWanted && *state.previewWanted != state.previewShown)
			{
				auto path = *state.previewWanted;
				bool environment = Lower(path.extension().string()) == ".hdr";
				bool restoreSky = !environment && state.previewEnvironment;
				state.previewShown = path;
				state.previewEnvironment = environment;
				state.previewLoading->store(true);
				(void)gLoads.Enqueue(
					std::format("Preview: {}", path.filename().string()),
					[preview, path = path.string(), environment, restoreSky, loading = state.previewLoading](std::atomic_uint8_t& progress)
					{
						if (environment)
						{
							preview->GetScene().LoadEnvironment(path, progress);
						}
						else
						{
							if (restoreSky)
								preview->GetScene().LoadEnvironment(std::nullopt, progress);
							preview->GetScene().LoadModels({path}, progress);
						}
						loading->store(false);
					});
			}

			float side = std::max(GetContentRegionAvail().x, 1.0F);
			if (state.texture != ImTextureID_Invalid && !state.previewShown.empty())
			{
				ImVec2 corner = GetCursorScreenPos();
				ImGui::Image(ImTextureRef(state.texture), ImVec2(side, side));
				if (state.previewLoading->load() || (state.previewWanted && *state.previewWanted != state.previewShown))
					GetWindowDrawList()->AddText(ImVec2(corner.x + 8.0F, corner.y + 8.0F), IM_COL32(255, 255, 255, 230), "Loading...");
			}
			else
			{
				Dummy(ImVec2(side, side * 0.25F));
				TextDisabled("Select a model or a panorama to preview it.");
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
				TextWrapped("%s", focus->name.c_str());
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
		if (Button("Select All Models"))
			for (size_t entryIt = 0; entryIt < state.entries.size(); entryIt++)
				state.selected[entryIt] = state.entries[entryIt].kind == Kind::kModel;
		SameLine();
		BeginDisabled(models < 2);
		Checkbox("Side by side", &state.sideBySide);
		EndDisabled();
		SetItemTooltip("Several models: side by side, each scaled to the same size (a set of variants), or as authored, each where it is (the parts of one scene).");
		SameLine();
		BeginDisabled(files == 0);
		if (Button("Open"))
			open();
		EndDisabled();
	}
	End();

	SetPreviewVisible(gShowBrowser && state.texture != ImTextureID_Invalid);
}

} // namespace client
