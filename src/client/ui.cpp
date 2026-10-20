#include "windowedapplicationdetail.h"

#include <gfx/lightgizmos.h>

#include <version.h>

#include <imgui.h>

#include <glm/mat4x4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>
#include <tuple>
#include <vector>
#include <numbers>

// a menu item's shortcut label, by the platform's name for ImGuiMod_Ctrl (cmd on macOS, see PrepareDraw)
#if defined(__APPLE__)
#	define SHORTCUT_LABEL(key) "Cmd+" key
#else
#	define SHORTCUT_LABEL(key) "Ctrl+" key
#endif

namespace client
{

namespace ui
{

// SPEEDO_OPEN_MENUS=<label>[/<label>...]: the menus of these labels are held open (each where its parent menu is), to
// show them in screenshots without clicking (which would bring the window to the front)
[[nodiscard]] bool BeginMenu(const char* label)
{
	static const std::string kOpenMenus = []
	{
		const char* menus = std::getenv("SPEEDO_OPEN_MENUS");
		return menus != nullptr ? std::format("/{}/", menus) : std::string();
	}();
	if (!kOpenMenus.empty() && kOpenMenus.find(std::format("/{}/", label)) != std::string::npos)
		ImGui::OpenPopup(ImGui::GetID(label));
	return ImGui::BeginMenu(label);
}

// View > Debug View's views (DEBUG_VIEW_*), by group: SPEEDO_DEBUG_VIEW takes their keys (or numbers)
struct DebugView
{
	const char* key;
	const char* name;
	uint32_t view;
	const char* description; // the tooltip, and the overlay's legend
};
enum class DebugViewGroup : uint8_t
{
	kSurface,
	kLighting,
	kIndirectLighting,
};
struct DebugViewEntry
{
	DebugViewGroup group;
	DebugView view;
};
constexpr std::array kDebugViews{
	DebugViewEntry{DebugViewGroup::kSurface, {"base-color", "Base Color", DEBUG_VIEW_BASE_COLOR, "The base color, textures and vertex colors applied."}},
	DebugViewEntry{DebugViewGroup::kSurface, {"normal", "Normal", DEBUG_VIEW_NORMAL, "The shading normal, normal mapped, in world space (x red, y green, z blue)."}},
	DebugViewEntry{DebugViewGroup::kSurface, {"geometry-normal", "Geometry Normal", DEBUG_VIEW_GEOMETRY_NORMAL, "The interpolated vertex normal, facing the viewer, in world space."}},
	DebugViewEntry{DebugViewGroup::kSurface, {"roughness", "Roughness", DEBUG_VIEW_ROUGHNESS, "Perceptual roughness: black is a mirror, white fully rough."}},
	DebugViewEntry{DebugViewGroup::kSurface, {"metallic", "Metallic", DEBUG_VIEW_METALLIC, "Metallic: white is metal."}},
	DebugViewEntry{DebugViewGroup::kSurface, {"occlusion", "Occlusion", DEBUG_VIEW_OCCLUSION, "The material's ambient occlusion (its texture, by its strength)."}},
	DebugViewEntry{DebugViewGroup::kSurface, {"material", "Material", DEBUG_VIEW_MATERIAL, "A color per material."}},
	DebugViewEntry{DebugViewGroup::kSurface, {"texcoord", "Texcoord", DEBUG_VIEW_TEXCOORD, "Texcoord set 0 (u red, v green), repeated."}},
	DebugViewEntry{DebugViewGroup::kLighting, {"emissive", "Emissive", DEBUG_VIEW_EMISSIVE, "The light the surfaces give off."}},
	DebugViewEntry{DebugViewGroup::kLighting, {"direct", "Direct Light", DEBUG_VIEW_DIRECT, "The punctual lights' light alone (with their shadows)."}},
	DebugViewEntry{DebugViewGroup::kLighting, {"indirect", "Ambient Light", DEBUG_VIEW_INDIRECT, "The ambient light alone, diffuse and specular: the indirect lighting's where it covers a point, else the environment's."}},
	DebugViewEntry{DebugViewGroup::kLighting, {"indirect-diffuse", "Ambient Diffuse (white)", DEBUG_VIEW_INDIRECT_DIFFUSE, "The ambient irradiance over pi: what a white surface would reflect of it."}},
	DebugViewEntry{DebugViewGroup::kLighting, {"indirect-specular", "Ambient Specular", DEBUG_VIEW_INDIRECT_SPECULAR, "The ambient light's specular reflection alone."}},
	DebugViewEntry{DebugViewGroup::kLighting, {"shadows", "Shadows", DEBUG_VIEW_SHADOWS, "The share of the direct light that reaches a point unshadowed: white lit, black shadowed, dark blue where no light faces it."}},
	DebugViewEntry{DebugViewGroup::kLighting, {"light-count", "Light Count", DEBUG_VIEW_LIGHT_COUNT, "The lights each pixel loops over (its cluster's): blue 1, cyan to green about 15, red 255 or more."}},
	DebugViewEntry{DebugViewGroup::kIndirectLighting, {"gi-irradiance", "Irradiance", DEBUG_VIEW_GI_IRRADIANCE, "The indirect lighting's diffuse alone, a white surface's. Magenta where no clip covers a point."}},
	DebugViewEntry{DebugViewGroup::kIndirectLighting, {"gi-visibility", "Probe Visibility", DEBUG_VIEW_GI_VISIBILITY, "The share of the probes around a point that it sees: dark where it sees few (inside walls, in corners)."}},
	DebugViewEntry{DebugViewGroup::kIndirectLighting, {"gi-voxel-behind", "Voxel Radiance (behind)", DEBUG_VIEW_GI_VOXEL_BEHIND, "The radiance of the voxel behind a point: its back face's, lit from behind the surface. Magenta where there is none, cyan outside the clip."}},
	DebugViewEntry{DebugViewGroup::kIndirectLighting, {"gi-voxel-front", "Voxel Radiance (in front)", DEBUG_VIEW_GI_VOXEL_FRONT, "The radiance of the voxel in front of a point: what a ray toward the surface meets first, as the cascades trace it. Magenta where there is none, cyan outside the clip."}},
	DebugViewEntry{DebugViewGroup::kIndirectLighting, {"gi-voxel-albedo", "Voxel Albedo", DEBUG_VIEW_GI_VOXEL_ALBEDO, "The albedo voxelized behind a point. Magenta where there is none, cyan outside the clip."}},
	DebugViewEntry{DebugViewGroup::kIndirectLighting, {"gi-clip", "Clips", DEBUG_VIEW_GI_CLIP, "Which clip lights a point, finest first: red, green, blue, yellow (blended where they meet). Dark outside them all."}},
	DebugViewEntry{DebugViewGroup::kIndirectLighting, {"gi-grid", "Voxel and Probe Grid", DEBUG_VIEW_GI_GRID, "The irradiance with the finest clip's voxels (thin lines, in its color) and cascade 0 probe cells (white) drawn over it."}},
	DebugViewEntry{DebugViewGroup::kIndirectLighting, {"gi-voxel-world", "Voxel World", DEBUG_VIEW_GI_VOXEL_WORLD, "The voxels themselves, as the cascades see them: each pixel's ray from the eye through the clips, and the radiance of the first voxel it meets (the sky where it meets none)."}},
	DebugViewEntry{DebugViewGroup::kIndirectLighting, {"gi-voxel-world-albedo", "Voxel World Albedo", DEBUG_VIEW_GI_VOXEL_WORLD_ALBEDO, "The albedo of the first voxel each pixel's ray meets (dark blue where it meets none)."}},
	DebugViewEntry{DebugViewGroup::kIndirectLighting, {"gi-voxel-world-normal", "Voxel World Normal", DEBUG_VIEW_GI_VOXEL_WORLD_NORMAL, "The normal of the first voxel each pixel's ray meets, dimmed where it faces away from the eye (lit from behind what is seen)."}},
	DebugViewEntry{DebugViewGroup::kIndirectLighting, {"gi-probes", "Probes (unmasked)", DEBUG_VIEW_GI_PROBES, "The irradiance of the 8 probes around a point as they hold it, trilinear: without their visibility masks and the resolve into the voxels, to tell what the probes see from what the lookups make of it."}},
	DebugViewEntry{DebugViewGroup::kIndirectLighting, {"gi-reference", "Reference (slow)", DEBUG_VIEW_GI_REFERENCE, "Each probe's light from 512 rays through the voxels straight to the environment: what the cascades approximate. Seconds a frame."}},
	DebugViewEntry{DebugViewGroup::kIndirectLighting, {"gi-level0", "Cascades at Level 0", DEBUG_VIEW_GI_LEVEL0, "The cascades traced through the finest voxels only: what the coarser levels lose."}},
};

[[nodiscard]] const DebugView* FindDebugView(uint32_t view)
{
	for (const auto& entry : kDebugViews)
		if (entry.view.view == view)
			return &entry.view;
	return nullptr;
}

} // namespace ui

void WindowedApplication::PrepareDraw()
{
	ZoneScopedN("WindowedApplication::PrepareDraw");

	using namespace windowedapplication;
	using namespace ImGui;

	auto& rhi = GetRHI();
	auto& device = rhi.GetPrimaryDevice();

	myImGui->BeginFrame();

	// ticks per second: PrepareDraw runs once per tick, measured like the frame rate (see Draw)
	static float gTicksPerSecond = 0.0F;
	{
		using namespace std::chrono_literals;
		static auto gTpsTime = std::chrono::steady_clock::now();
		static uint32_t gTpsTickCount = 0;
		gTpsTickCount++;
		if (auto now = std::chrono::steady_clock::now(); now - gTpsTime >= 500ms)
		{
			gTicksPerSecond = static_cast<float>(gTpsTickCount) / std::chrono::duration<float>(now - gTpsTime).count();
			gTpsTickCount = 0;
			gTpsTime = now;
		}
	}

	// frame/tick rate overlay in the top right corner, below the menu bar
	if (gShowFps || gShowTps)
	{
		constexpr float kFpsOverlayPadding = 10.0F;
		constexpr float kFpsOverlayBgAlpha = 0.35F;
		const auto* viewport = GetMainViewport();
		SetNextWindowPos(
			ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - kFpsOverlayPadding, viewport->WorkPos.y + kFpsOverlayPadding),
			ImGuiCond_Always,
			ImVec2(1.0F, 0.0F));
		SetNextWindowBgAlpha(kFpsOverlayBgAlpha);
		if (Begin(
				"Rates",
				nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
					ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
		{
			if (gShowFps)
			{
				float fps = gFramesPerSecond.load(std::memory_order_relaxed);
				Text("%.0f FPS (%.2f ms)", fps, fps > 0.0F ? 1000.0F / fps : 0.0F);
			}
			if (gShowTps)
				Text("%.0f TPS (%.2f ms)", gTicksPerSecond, gTicksPerSecond > 0.0F ? 1000.0F / gTicksPerSecond : 0.0F);
		}
		End();
	}

	// the debug view shown (View > Debug View), in the bottom left corner (the Statistics window opens in the top left): its
	// name and what its colors mean
	if (const auto* debugView = ui::FindDebugView(App().GetScene().GetSettings().debugView.load(std::memory_order_relaxed)))
	{
		constexpr float kOverlayPadding = 10.0F;
		constexpr float kOverlayBgAlpha = 0.35F;
		constexpr float kOverlayWrapWidth = 360.0F;
		const auto* viewport = GetMainViewport();
		SetNextWindowPos(
			ImVec2(viewport->WorkPos.x + kOverlayPadding, viewport->WorkPos.y + viewport->WorkSize.y - kOverlayPadding),
			ImGuiCond_Always,
			ImVec2(0.0F, 1.0F));
		SetNextWindowBgAlpha(kOverlayBgAlpha);
		if (Begin(
				"Debug view",
				nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
					ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
		{
			Text("Debug view: %s", debugView->name);
			PushTextWrapPos(kOverlayWrapWidth);
			TextDisabled("%s", debugView->description);
			PopTextWrapPos();
			if (debugView->view >= DEBUG_VIEW_GI_FIRST && !App().GetScene().GetSettings().indirectLighting.load(std::memory_order_relaxed))
				TextColored(ImVec4(1.0F, 0.6F, 0.2F, 1.0F), "Indirect lighting is off (View > Indirect Lighting)");
			TextDisabled(SHORTCUT_LABEL("0") " to go back to the light, " SHORTCUT_LABEL("[") " and " SHORTCUT_LABEL("]") " to step");
		}
		End();
	}

	// the move speed for a moment after it changes (with the mouse wheel, or a newly framed model), at the bottom
	{
		using namespace std::chrono_literals;
		static float gShownMoveSpeed = myViews->GetMoveSpeed();
		static auto gMoveSpeedChanged = std::chrono::steady_clock::time_point{};
		auto now = std::chrono::steady_clock::now();
		if (auto moveSpeed = myViews->GetMoveSpeed(); moveSpeed != gShownMoveSpeed)
		{
			gShownMoveSpeed = moveSpeed;
			gMoveSpeedChanged = now;
		}
		if (now - gMoveSpeedChanged < 1500ms)
		{
			constexpr float kOverlayPadding = 10.0F;
			constexpr float kOverlayBgAlpha = 0.35F;
			const auto* viewport = GetMainViewport();
			SetNextWindowPos(
				ImVec2(viewport->WorkPos.x + (0.5F * viewport->WorkSize.x), viewport->WorkPos.y + viewport->WorkSize.y - kOverlayPadding),
				ImGuiCond_Always,
				ImVec2(0.5F, 1.0F));
			SetNextWindowBgAlpha(kOverlayBgAlpha);
			if (Begin(
					"Move speed",
					nullptr,
					ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
						ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
				Text("Move speed: %.3g units/s", gShownMoveSpeed);
			End();
		}
	}

	// one row per load in progress
	if (bool loading = !gLoads.Empty() &&
				 Begin(
					 "Loading",
					 &loading,
					 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDecoration |
						 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
						 ImGuiWindowFlags_AlwaysAutoResize |
						 // appearing, it would take the focus (the browser's, whose previews load through here)
						 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
	{
		constexpr uint8_t kProgressMax = 255;
		constexpr float kProgressBarWidth = 160.0F;
		gLoads.ForEach([](const core::LoadQueue::Load& load)
		{
			ProgressBar((1.F / kProgressMax) * static_cast<float>(load.progress), ImVec2(kProgressBarWidth, 0));
			SameLine();
			TextUnformatted(load.name.c_str());
		});
		End();
	}

	static float gMenuBarHeight = 0.0F; // in imgui's coordinates, as the menu bar drew last frame
	// the ui fades out when it isn't in use: when the mouse isn't over the menu bar and no other window, menu or popup is
	// up, after a moment, and back in when it is (or one comes up, from a shortcut, a load's choice, ...). the overlays
	// above stay as they are. the windows are last frame's, the menu bar's height too
	{
		constexpr float kFadeInSeconds = 0.15F;
		constexpr float kFadeOutSeconds = 0.5F;
		constexpr float kHoldSeconds = 1.0F; // in use this recently counts as in use
		static float gFade = 1.0F; // 0: faded out, 1: shown
		static float gIdleSeconds = 0.0F;
		const auto& io = GetIO();
		bool overMenuBar = IsMousePosValid() && io.MousePos.y >= 0.0F && io.MousePos.y < gMenuBarHeight;
		bool inUse = overMenuBar || myImGui->ShowedFadingWindows() || IsAnyItemActive();
		gIdleSeconds = inUse ? 0.0F : gIdleSeconds + io.DeltaTime;
		gFade = gIdleSeconds < kHoldSeconds ? std::min(gFade + (io.DeltaTime / kFadeInSeconds), 1.0F)
											: std::max(gFade - (io.DeltaTime / kFadeOutSeconds), 0.0F);
		myImGui->Fade(gFade * gFade * (3.0F - (2.0F * gFade))); // eased
	}

	// View > Statistics (SPEEDO_SHOW_STATISTICS=1 at startup), in the builds that profile (debug and profile): the pass
	// timings (the frame graph's, written in those builds only), submits and transient memory, and in debug the vulkan
	// objects (tracked by validation builds only)
#if (SPEEDO_PROFILING_LEVEL > 0)
	static bool gShowStatistics = []
	{
		const char* show = std::getenv("SPEEDO_SHOW_STATISTICS");
		return show != nullptr && std::string_view(show) == "1";
	}();
	{
		if (gShowStatistics)
		{
			if (Begin("Statistics", &gShowStatistics, ImGuiWindowFlags_AlwaysAutoResize))
			{
				CloseOnEscape(gShowStatistics);
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
				if (CollapsingHeader("Vulkan Objects"))
					for (const auto& [name, count] : GetObjectCounts<kGraphicsApi>())
						Text("%.*s: %u", static_cast<int>(name.size()), name.data(), count);
#endif

				// gpu submits per frame, averaged over half a second as the frame rate is (see Draw)
				Text("Queue Submits: %.1f/frame", gSubmitsPerFrame.load(std::memory_order_relaxed));
				Text("Submit Batches: %.1f/frame", gSubmitBatchesPerFrame.load(std::memory_order_relaxed));

				// the frame graph's passes, on the gpu (see GpuProfiler::Collect): only where the backend writes their timestamps
				// (KosmicKrisp: at the beginning and end of its own command encoders). in two columns of passes, which keeps
				// the window short enough for the sections below
				if (myScene && CollapsingHeader("GPU Passes", ImGuiTreeNodeFlags_DefaultOpen) &&
					BeginTable("GPU Passes", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV))
				{
					auto timings = myScene->GetRenderer().GetPassTimings();
					size_t rowCount = (timings.size() + 1) / 2;
					for (size_t rowIt = 0; rowIt < rowCount; rowIt++)
					{
						TableNextRow();
						for (size_t index : {rowIt, rowIt + rowCount})
						{
							TableNextColumn();
							if (index < timings.size())
								TextUnformatted(timings[index].first.c_str());
							TableNextColumn();
							if (index < timings.size())
								Text("%.3f ms", timings[index].second);
						}
					}
					EndTable();
				}

				// what the frame draws and how the culling did (the cpu's plan for the last frame recorded; the gpu's decisions
				// of the last frame it finished, a frame or two behind), the lights and the indirect lighting
				if (myScene)
				{
					const auto& renderer = myScene->GetRenderer();
					auto frame = renderer.GetFrameStatistics();
					auto decisions = renderer.GetCullStatistics();
					auto percent = [](uint64_t part, uint64_t whole)
					{ return whole > 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0; };
					auto compact = [](uint64_t count)
					{
						return count >= 10'000'000 ? std::format("{:.1f} M", static_cast<double>(count) / 1e6)
							   : count >= 10'000	? std::format("{:.1f} k", static_cast<double>(count) / 1e3)
													: std::format("{}", count);
					};

					if (CollapsingHeader("Draws", ImGuiTreeNodeFlags_DefaultOpen))
					{
						// per list, one view's worth (a draw is a chunk of a submesh, see mesh::Chunk)
						if (BeginTable("Draw lists", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
						{
							TableSetupColumn("List");
							TableSetupColumn("Draws");
							TableSetupColumn("Triangles");
							TableHeadersRow();
							for (size_t listIt = 0; listIt < gfx::FrameStatistics::kListNames.size(); listIt++)
							{
								if (frame.listDraws[listIt] == 0)
									continue;
								TableNextRow();
								TableNextColumn();
								TextUnformatted(gfx::FrameStatistics::kListNames[listIt]);
								TableNextColumn();
								Text("%u", frame.listDraws[listIt]);
								TableNextColumn();
								TextUnformatted(compact(frame.listTriangles[listIt]).c_str());
							}
							EndTable();
						}
						Text("Views: %u main, %u shadow", frame.mainViewCount, frame.shadowViewCount);
					}

					if (CollapsingHeader("Culling", ImGuiTreeNodeFlags_DefaultOpen))
					{
						Text(
							"%s, occlusion %.*s%s",
							!frame.culling	   ? "Culling off"
							: frame.gpuCulling ? "On the gpu"
											   : "On the cpu (by the frustum: no gpu statistics)",
							static_cast<int>(renderer.GetOcclusionModeName().size()),
							renderer.GetOcclusionModeName().data(),
							frame.occlusion ? "" : " (not this frame)");
						// per group, the draw and view pairs tested, and what became of them: outside the frustum, hidden by
						// the depth pyramid, drawn (before it, or after it: the late phase's), and the triangles drawn
						if (frame.gpuCulling &&
							BeginTable("Culling", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
						{
							TableSetupColumn("");
							TableSetupColumn("Main views");
							TableSetupColumn(frame.shadowOcclusion ? "Shadow views" : "Shadow views (no occlusion)");
							TableHeadersRow();
							auto row = [&](const char* label, auto&& cell)
							{
								TableNextRow();
								TableNextColumn();
								TextUnformatted(label);
								for (uint32_t groupIt = 0; groupIt < CULL_STATISTIC_GROUP_COUNT; groupIt++)
								{
									TableNextColumn();
									cell(groupIt, &decisions[groupIt * CULL_STATISTIC_COUNT]);
								}
							};
							row("Tested", [&](uint32_t group, const uint32_t*) { Text("%s", compact(frame.testedDraws[group]).c_str()); });
							row("Outside frustum", [&](uint32_t group, const uint32_t* d)
								{ Text("%u (%.0f%%)", d[CULL_STATISTIC_FRUSTUM], percent(d[CULL_STATISTIC_FRUSTUM], frame.testedDraws[group])); });
							row("Occluded", [&](uint32_t group, const uint32_t* d)
								{ Text("%u (%.0f%%)", d[CULL_STATISTIC_HIDDEN], percent(d[CULL_STATISTIC_HIDDEN], frame.testedDraws[group])); });
							row("Drawn early", [&](uint32_t, const uint32_t* d) { Text("%u", d[CULL_STATISTIC_EARLY]); });
							row("Drawn late", [&](uint32_t, const uint32_t* d)
								{ Text("%u (%u blended)", d[CULL_STATISTIC_LATE] + d[CULL_STATISTIC_LATE_BLENDED], d[CULL_STATISTIC_LATE_BLENDED]); });
							row("Drawn", [&](uint32_t group, const uint32_t* d)
								{
									uint64_t drawn = uint64_t{d[CULL_STATISTIC_EARLY]} + d[CULL_STATISTIC_LATE] + d[CULL_STATISTIC_LATE_BLENDED];
									Text("%s (%.0f%%)", compact(drawn).c_str(), percent(drawn, frame.testedDraws[group]));
								});
							row("Triangles drawn", [&](uint32_t group, const uint32_t* d)
								{
									Text(
										"%s of %s (%.0f%%)",
										compact(d[CULL_STATISTIC_TRIANGLES]).c_str(),
										compact(frame.testedTriangles[group]).c_str(),
										percent(d[CULL_STATISTIC_TRIANGLES], frame.testedTriangles[group]));
								});
							EndTable();
						}
						// drawing late is what the reprojection alone would have missed (hybrid), or what moved into view
						// from behind something (two-phase): a lot of it means the early phase guesses badly
						if (frame.occlusion)
						{
							const auto* main = &decisions[CULL_STATISTIC_MAIN * CULL_STATISTIC_COUNT];
							uint64_t drawn = uint64_t{main[CULL_STATISTIC_EARLY]} + main[CULL_STATISTIC_LATE] + main[CULL_STATISTIC_LATE_BLENDED];
							TextDisabled("Late share of the main draws: %.1f%% (opaque %u)", percent(main[CULL_STATISTIC_LATE], drawn), main[CULL_STATISTIC_LATE]);
						}
					}

					if (CollapsingHeader("Lights", ImGuiTreeNodeFlags_DefaultOpen))
					{
						Text("Lights: %u, %u shadowed in %u shadow views", frame.lightCount, frame.shadowedLightCount, frame.shadowViewCount);
						if (frame.clusteredLights)
							Text(
								"Clusters: %u light indices, at most %u lights in a cluster, %.1f on average where any",
								frame.clusterLightIndexCount,
								frame.clusterMaxLightCount,
								static_cast<double>(frame.clusterMeanLightCount));
						else
							TextDisabled("Clusters: off (every pixel loops over every light)");
					}

					if (CollapsingHeader("Indirect Lighting", ImGuiTreeNodeFlags_DefaultOpen))
					{
						if (!frame.indirectLighting)
							TextDisabled("Off (View > Indirect Lighting)");
						else
						{
							Text("%u clips, %u bounces; voxelized this frame: %u", frame.giClipCount, frame.giBounceCount, frame.giClipsVoxelized);
							for (uint32_t clipIt = 0; clipIt < frame.giClipCount; clipIt++)
							{
								auto voxels = frame.giVoxelCounts[clipIt];
								Text(
									"Clip %u: %ux%ux%u voxels of %.3g units (%s)",
									clipIt,
									voxels.x,
									voxels.y,
									voxels.z,
									static_cast<double>(frame.giVoxelSize[clipIt]),
									compact(uint64_t{voxels.x} * voxels.y * voxels.z).c_str());
							}
							Text("Moving: %u draws, %s triangles voxelized every frame", frame.giMovingDraws, compact(frame.giMovingTriangles).c_str());
						}
					}
				}

				// the frame graph's transients (see FrameGraph), in shared memory
				Separator();
				constexpr double kMiB = 1024.0 * 1024.0;
				Text(
					"Transient Memory: %.1f MiB (%.1f MiB unaliased)",
					static_cast<double>(App().GetScene().GetTransientMemory()) / kMiB,
					static_cast<double>(App().GetScene().GetUnaliasedTransientMemory()) / kMiB);
			}
			End();
		}
	}
#endif

	// the scene's lights (View > Lights): listed, and drawn over the views (see gfx::BuildLightGizmos)
	static bool gShowLights = []
	{
		const char* show = std::getenv("SPEEDO_SHOW_LIGHTS");
		return show != nullptr && std::string_view(show) == "1";
	}();
	App().GetScene().GetSettings().captureLights.store(gShowLights, std::memory_order_relaxed);
	if (gShowLights)
	{
		static bool gLightMarkers = true;
		static bool gShadowFrusta = true;
		static std::optional<size_t> gSelectedLight;
		auto capture = App().GetScene().GetRenderer().GetLightCapture();
		if (gSelectedLight && *gSelectedLight >= capture.lights.size())
			gSelectedLight.reset();

		if (Begin("Lights", &gShowLights, ImGuiWindowFlags_AlwaysAutoResize))
		{
			CloseOnEscape(gShowLights);
			Checkbox("Markers", &gLightMarkers);
			SameLine();
			Checkbox("Shadow Frusta", &gShadowFrusta);
			SetItemTooltip("Each shadow view's frustum: a directional light's cascades, a point light's cube faces, a spot light's view.");
			TextDisabled("The model's lights, then the environment's dominant lights (merged ones aren't drawn).");
			if (Selectable("All", !gSelectedLight))
				gSelectedLight.reset();
			for (size_t lightIt = 0; lightIt < capture.lights.size(); lightIt++)
			{
				const auto& light = capture.lights[lightIt];
				static constexpr std::array kTypeNames{"Directional", "Point", "Spot"};
				const char* type = light.type < kTypeNames.size() ? kTypeNames[light.type] : "?";
				float luminance = (0.2126F * light.intensity[0]) + (0.7152F * light.intensity[1]) + (0.0722F * light.intensity[2]);
				uint32_t first = lightIt < capture.lightShadows.size() ? capture.lightShadows[lightIt] : SHADER_TYPES_NO_SHADOW;
				auto label = std::format(
					"#{} {} ({}): {:.3g} {}, {}",
					lightIt,
					type,
					lightIt < capture.modelLightCount ? "model" : "environment",
					luminance,
					light.type == LIGHT_TYPE_DIRECTIONAL ? "lux" : "cd",
					first == SHADER_TYPES_NO_SHADOW ? std::string("no shadows") : std::format("{} shadow views", gfx::ShadowViewCount(light.type)));
				PushID(static_cast<int>(lightIt));
				float brightest = std::max({light.intensity[0], light.intensity[1], light.intensity[2], 1e-6F});
				ColorButton(
					"##color",
					ImVec4(light.intensity[0] / brightest, light.intensity[1] / brightest, light.intensity[2] / brightest, 1.0F),
					ImGuiColorEditFlags_NoTooltip,
					ImVec2(GetTextLineHeight(), GetTextLineHeight()));
				SameLine();
				if (Selectable(label.c_str(), gSelectedLight == lightIt))
					gSelectedLight = lightIt;
				PopID();
			}
		}
		End();

		// over each view: its camera's projection into its viewport (framebuffer pixels, imgui's coordinates are points)
		auto gizmos = gfx::BuildLightGizmos(capture, {.shadowFrusta = gShadowFrusta, .light = gSelectedLight});
		auto* drawList = GetBackgroundDrawList();
		ImVec2 scale = GetIO().DisplayFramebufferScale;
		auto viewports = myViews->GetViewports();
		for (size_t viewIt = 0; viewIt < viewports.size(); viewIt++)
		{
			auto camera = myViews->GetCamera(viewIt);
			if (!camera)
				continue;
			const auto& viewport = viewports[viewIt];
			glm::mat4 viewProjection = camera->GetProjectionMatrix() * camera->GetViewMatrix();
			auto toScreen = [&viewport, scale](glm::vec4 clip)
			{
				glm::vec2 ndc = glm::vec2(clip) / clip.w;
				return ImVec2(
					(static_cast<float>(viewport.x) + ((ndc.x * 0.5F) + 0.5F) * static_cast<float>(viewport.width)) / scale.x,
					(static_cast<float>(viewport.y) + ((ndc.y * 0.5F) + 0.5F) * static_cast<float>(viewport.height)) / scale.y);
			};
			constexpr float kNearW = 1e-3F; // in front of the camera
			drawList->PushClipRect(
				ImVec2(static_cast<float>(viewport.x) / scale.x, static_cast<float>(viewport.y) / scale.y),
				ImVec2(static_cast<float>(viewport.x + viewport.width) / scale.x, static_cast<float>(viewport.y + viewport.height) / scale.y),
				true);
			for (const auto& line : gizmos.lines)
			{
				glm::vec4 from = viewProjection * glm::vec4(line.from, 1.0F);
				glm::vec4 to = viewProjection * glm::vec4(line.to, 1.0F);
				if (from.w < kNearW && to.w < kNearW)
					continue;
				// clipped to what is in front of the camera
				if (from.w < kNearW)
					from += (to - from) * ((kNearW - from.w) / (to.w - from.w));
				else if (to.w < kNearW)
					to += (from - to) * ((kNearW - to.w) / (from.w - to.w));
				drawList->AddLine(
					toScreen(from), toScreen(to), ColorConvertFloat4ToU32(ImVec4(line.color.x, line.color.y, line.color.z, line.color.w)), 1.5F);
			}
			if (gLightMarkers)
				for (const auto& marker : gizmos.markers)
				{
					// a directional light where it is in the sky (at infinity), kept at the view's edge when it is out of
					// it, dimmed if behind the camera
					bool directional = marker.type == LIGHT_TYPE_DIRECTIONAL;
					glm::vec4 clip = viewProjection * glm::vec4(marker.position, directional ? 0.0F : 1.0F);
					float alpha = 1.0F;
					if (directional)
					{
						glm::vec2 ndc = glm::vec2(clip) / std::max(std::abs(clip.w), kNearW);
						if (clip.w < kNearW)
						{
							ndc = -glm::vec2(clip);
							alpha = 0.4F;
						}
						float extent = std::max({std::abs(ndc.x), std::abs(ndc.y), 1e-6F});
						constexpr float kEdge = 0.92F;
						if (clip.w < kNearW || extent > kEdge)
							ndc *= kEdge / extent;
						clip = glm::vec4(ndc, 0.0F, 1.0F);
					}
					else if (clip.w < kNearW)
						continue;
					ImVec2 at = toScreen(clip);
					ImU32 color = ColorConvertFloat4ToU32(ImVec4(marker.color.x, marker.color.y, marker.color.z, alpha));
					constexpr ImU32 kOutline = IM_COL32(0, 0, 0, 220);
					constexpr float kRadius = 7.0F;
					// a sun with rays, a bulb, or a spot's cone
					if (marker.type == LIGHT_TYPE_DIRECTIONAL)
						for (int rayIt = 0; rayIt < 8; rayIt++)
						{
							float angle = static_cast<float>(rayIt) * std::numbers::pi_v<float> / 4.0F;
							ImVec2 direction(std::cos(angle), std::sin(angle));
							drawList->AddLine(
								ImVec2(at.x + direction.x * kRadius * 1.4F, at.y + direction.y * kRadius * 1.4F),
								ImVec2(at.x + direction.x * kRadius * 2.1F, at.y + direction.y * kRadius * 2.1F),
								color,
								2.0F);
						}
					else if (marker.type == LIGHT_TYPE_SPOT)
						drawList->AddTriangleFilled(
							ImVec2(at.x, at.y + kRadius * 0.4F), ImVec2(at.x - kRadius * 1.3F, at.y + kRadius * 2.2F),
							ImVec2(at.x + kRadius * 1.3F, at.y + kRadius * 2.2F), color);
					drawList->AddCircleFilled(at, kRadius, color);
					drawList->AddCircle(at, kRadius, kOutline, 0, 1.5F);
					auto label = std::format("#{}", marker.light);
					drawList->AddText(ImVec2(at.x + kRadius * 2.3F, at.y - kRadius), IM_COL32(255, 255, 255, 230), label.c_str());
				}
			drawList->PopClipRect();
		}
	}

	InternalDrawBrowser();

	// the models of an archive with several (see LoadAndInstallArchive)
	static std::optional<ArchiveChoice> gShownArchiveChoice;
	if (!gShownArchiveChoice)
	{
		std::scoped_lock lock(gArchiveChoiceMutex);
		if (gArchiveChoice)
		{
			gShownArchiveChoice = std::move(gArchiveChoice);
			gArchiveChoice.reset();
			OpenPopup("Load Model");
		}
	}
	if (BeginPopupModal("Load Model", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		const auto& choice = *gShownArchiveChoice;
		Text("%s holds %zu models:", choice.archive.c_str(), choice.models.size());

		constexpr float kListHeight = 300.0F;
		std::optional<std::filesystem::path> chosen;
		if (BeginChild("Models", ImVec2(0.0F, kListHeight), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_Borders))
		{
			for (const auto& model : choice.models)
				if (Selectable(model.lexically_relative(choice.directory).string().c_str()))
					chosen = model;
		}
		EndChild();

		if (chosen)
			(void)gLoads.Enqueue(
				chosen->filename().string(),
				[&rhi, path = chosen->string()](std::atomic_uint8_t& progress) { LoadAndInstallModel(rhi, path, progress); });

		bool all = Button("All, side by side");
		if (all)
		{
			std::vector<std::string> paths;
			for (const auto& model : choice.models)
				paths.push_back(model.string());
			(void)gLoads.Enqueue(
				choice.archive,
				[&rhi, paths = std::move(paths)](std::atomic_uint8_t& progress) { App().GetScene().LoadModels(paths, progress); });
		}
		SameLine();

		// escape cancels too (see CloseOnEscape)
		if (chosen || all || Button("Cancel") || Shortcut(ImGuiKey_Escape))
		{
			gShownArchiveChoice.reset();
			CloseCurrentPopup();
		}
		EndPopup();
	}

	if (gShowDemoWindow)
		ShowDemoWindow(&gShowDemoWindow);

	if (gShowAbout)
	{
		if (Begin("About client", &gShowAbout, ImGuiWindowFlags_AlwaysAutoResize))
		{
			CloseOnEscape(gShowAbout);
			Text("speedo %d.%d.%d", speedo::version::Major, speedo::version::Minor, speedo::version::Patch);
			TextDisabled("A real-time renderer and asset viewer, built on Vulkan.");
			Separator();
			auto name = device.GetName();
			Text("GPU: %.*s", static_cast<int>(name.size()), name.data());
#if (SPEEDO_GRAPHICS_VALIDATION_LEVEL > 0)
			Text("Build: debug (validation on)");
#elif (SPEEDO_PROFILING_LEVEL > 0)
			Text("Build: profile");
#else
			Text("Build: release");
#endif
		}
		End();
	}

	auto resourcePath = std::get<std::filesystem::path>(core::Application::Get()->GetEnv().variables["ResourcePath"]);


	// automation: SPEEDO_AUTOLOAD_MODEL (a model, or a zip archive or directory, whose models are loaded side by side)
	// and SPEEDO_AUTOLOAD_IMAGE (an image, on the default material) name files to load at startup, absolute or relative to
	// the resource directory, through the same load + install path as the "File" menu (SPEEDO_AUTOLOAD_SCENE=<index>: a
	// gltf model's scene, as View > Scene loads it). with
	// SPEEDO_AUTOLOAD_EXIT=<frames>, the application exits that many frames after the loads have finished (see
	// scripts/assettest.ps1).
	static std::vector<core::Future<void>> gAutoLoads;
	static std::optional<uint32_t> gAutoLoadExitFrames;
	// SPEEDO_CAMERA=<x>,<y>,<z>,<pitch>,<yaw> (world space, degrees): where the views look from once the autoloads are
	// installed (the model's install frames them first), for measurements and screenshots; SPEEDO_CAMERA=frame frames the
	// model, as View > Camera > Frame model (a model with cameras starts looking through its first)
	static std::optional<std::array<float, 5>> gAutoCamera;
	static bool gAutoFrame = false;
	static bool gAutoCameraAfterModel = false; // wait for the model's install (on the draw thread), which frames the views
	// SPEEDO_CAMERA_TURN=<degrees per second>: the views turn about the vertical axis once the autoloads have installed
	// (after SPEEDO_CAMERA's placement), for comparing renders and timings with a camera that moves
	static float gAutoTurn = 0.0F;
	static float gAutoWalk = 0.0F; // SPEEDO_CAMERA_WALK=<units per second>: forward, as the turn
	static bool gAutoTurning = false;
	// SPEEDO_CAMERA_PATH=<x>,<y>,<z>,<pitch>,<yaw>[;...]: the views placed at each pose in turn, one per drawn frame, from
	// the frame after SPEEDO_CAMERA's placement, and kept at the last: an exact, repeatable move (the walk and the turn go
	// by the frame time), to compare a view arrived at with one started at
	static std::vector<std::array<float, 5>> gAutoPath;
	static bool gAutoPathFollowed = false;
	// a pose: x, y, z, pitch, yaw, separated by commas, up to the end or a semicolon
	auto parsePose = [](const char*& it) -> std::optional<std::array<float, 5>>
	{
		std::array<float, 5> values{};
		size_t count = 0;
		for (; count < values.size() && *it != '\0' && *it != ';'; count++)
		{
			char* end = nullptr;
			values[count] = std::strtof(it, &end);
			if (end == it)
				break;
			it = *end == ',' ? end + 1 : end;
		}
		if (count != values.size())
			return std::nullopt;
		return values;
	};
	if (static bool gAutoLoadDone = false; !gAutoLoadDone)
	{
		gAutoLoadDone = true;

		// queued as separate loads, which run concurrently
		// a zip archive loads all of its models, side by side
		mesh::ImportOptions autoLoadOptions;
		if (const char* scene = std::getenv("SPEEDO_AUTOLOAD_SCENE"); scene != nullptr && *scene != '\0')
			autoLoadOptions.scene = std::strtoull(scene, nullptr, 10);
		if (const char* variant = std::getenv("SPEEDO_AUTOLOAD_VARIANT"); variant != nullptr && *variant != '\0')
			autoLoadOptions.variant = std::strtoull(variant, nullptr, 10);
		if (const char* autoLoadModel = std::getenv("SPEEDO_AUTOLOAD_MODEL"); autoLoadModel != nullptr && *autoLoadModel != '\0')
			gAutoLoads.emplace_back(gLoads.Enqueue(
				autoLoadModel,
				[&rhi, path = (resourcePath / autoLoadModel).string(), autoLoadOptions](std::atomic_uint8_t& progress)
				{
					if ((autoLoadOptions.scene || autoLoadOptions.variant) && mesh::IsModelFile(path))
						App().GetScene().LoadModels({path}, progress, autoLoadOptions);
					else
						LoadAndInstallFile(rhi, path, progress, ArchiveModels::kAll);
				}));
		// the tonemapper (SPEEDO_TONEMAPPER: pbr-neutral, aces, agx, reinhard or linear), auto exposure
		// (SPEEDO_AUTO_EXPOSURE=1) and the exposure to start with (as View sets them)
		if (const char* tonemapper = std::getenv("SPEEDO_TONEMAPPER"); tonemapper != nullptr)
		{
			std::string_view name(tonemapper);
			App().GetScene().GetSettings().tonemapper.store(
				name == "aces"		 ? TONEMAPPER_ACES
				: name == "agx"		 ? TONEMAPPER_AGX
				: name == "reinhard" ? TONEMAPPER_REINHARD
				: name == "linear"	 ? TONEMAPPER_LINEAR
									 : TONEMAPPER_PBR_NEUTRAL,
				std::memory_order_relaxed);
		}
		if (const char* autoExposure = std::getenv("SPEEDO_AUTO_EXPOSURE"); autoExposure != nullptr && std::string_view(autoExposure) == "1")
			App().GetScene().GetSettings().autoExposure.store(true, std::memory_order_relaxed);
		// SPEEDO_EXPOSURE=<stops>: the exposure (View > Exposure), e.g. to capture a dark interior as it is, comparably
		if (const char* exposure = std::getenv("SPEEDO_EXPOSURE"); exposure != nullptr && *exposure != '\0')
			App().GetScene().GetSettings().exposureStops.store(std::strtof(exposure, nullptr), std::memory_order_relaxed);
		if (const char* shadows = std::getenv("SPEEDO_SHADOWS"); shadows != nullptr && std::string_view(shadows) == "0")
			App().GetScene().GetSettings().shadows.store(false, std::memory_order_relaxed);
		if (const char* culling = std::getenv("SPEEDO_CULLING"); culling != nullptr && std::string_view(culling) == "0")
			App().GetScene().GetSettings().culling.store(false, std::memory_order_relaxed);
		if (const char* clustered = std::getenv("SPEEDO_CLUSTERED_LIGHTS"); clustered != nullptr && std::string_view(clustered) == "0")
			App().GetScene().GetSettings().clusteredLights.store(false, std::memory_order_relaxed);
		// the indirect lighting (SPEEDO_GI=1) and the debug view (SPEEDO_DEBUG_VIEW: a key of ui::kDebugViews, or its
		// number; SPEEDO_GI_DEBUG=<n> is the indirect lighting's n-th, as GiData::debug numbers them) to start with
		if (const char* gi = std::getenv("SPEEDO_GI"); gi != nullptr && std::string_view(gi) == "1")
			App().GetScene().GetSettings().indirectLighting.store(true, std::memory_order_relaxed);
		if (const char* giDebug = std::getenv("SPEEDO_GI_DEBUG"); giDebug != nullptr && *giDebug != '\0')
			if (auto index = static_cast<uint32_t>(std::strtoul(giDebug, nullptr, 10)); index > 0 && DEBUG_VIEW_GI_FIRST + index - 1 < DEBUG_VIEW_COUNT)
				App().GetScene().GetSettings().debugView.store(DEBUG_VIEW_GI_FIRST + index - 1, std::memory_order_relaxed);
		if (const char* debugView = std::getenv("SPEEDO_DEBUG_VIEW"); debugView != nullptr && *debugView != '\0')
		{
			std::string_view name(debugView);
			uint32_t view = static_cast<uint32_t>(std::strtoul(debugView, nullptr, 10));
			for (const auto& entry : ui::kDebugViews)
				if (name == entry.view.key)
					view = entry.view.view;
			if (view < DEBUG_VIEW_COUNT)
				App().GetScene().GetSettings().debugView.store(view, std::memory_order_relaxed);
		}
		if (const char* merge = std::getenv("SPEEDO_MERGE_LIGHTS"); merge != nullptr && std::string_view(merge) == "0")
			App().GetScene().GetSettings().mergeLights.store(false, std::memory_order_relaxed);
		// the environment's rotation (degrees) and intensity to start with (as View > Environment sets them)
		if (const char* rotation = std::getenv("SPEEDO_ENVIRONMENT_ROTATION"); rotation != nullptr && *rotation != '\0')
			App().GetScene().GetSettings().environmentRotationDegrees.store(std::strtof(rotation, nullptr), std::memory_order_relaxed);
		if (const char* intensity = std::getenv("SPEEDO_ENVIRONMENT_INTENSITY"); intensity != nullptr && *intensity != '\0')
			App().GetScene().GetSettings().environmentIntensity.store(std::max(std::strtof(intensity, nullptr), 0.0F), std::memory_order_relaxed);
		// the environment: SPEEDO_AUTOLOAD_ENVIRONMENT's panorama, else the procedural sky
		std::optional<std::string> environmentPath;
		if (const char* autoLoadEnvironment = std::getenv("SPEEDO_AUTOLOAD_ENVIRONMENT");
			autoLoadEnvironment != nullptr && *autoLoadEnvironment != '\0')
			environmentPath = (resourcePath / autoLoadEnvironment).string();
		gAutoLoads.emplace_back(gLoads.Enqueue(
			environmentPath.value_or("Procedural sky"),
			[&rhi, environmentPath](std::atomic_uint8_t& progress) { App().GetScene().LoadEnvironment(environmentPath, progress); }));
		if (const char* autoLoadImage = std::getenv("SPEEDO_AUTOLOAD_IMAGE"); autoLoadImage != nullptr && *autoLoadImage != '\0')
			gAutoLoads.emplace_back(gLoads.Enqueue(
				autoLoadImage,
				[&rhi, path = (resourcePath / autoLoadImage).string()](std::atomic_uint8_t& progress)
				{ App().GetScene().LoadImage(path, progress); }));
		if (const char* autoLoadExit = std::getenv("SPEEDO_AUTOLOAD_EXIT"); autoLoadExit != nullptr && *autoLoadExit != '\0')
			gAutoLoadExitFrames = static_cast<uint32_t>(std::strtoul(autoLoadExit, nullptr, 10));
		if (const char* turn = std::getenv("SPEEDO_CAMERA_TURN"); turn != nullptr && *turn != '\0')
			gAutoTurn = glm::radians(std::strtof(turn, nullptr));
		if (const char* walk = std::getenv("SPEEDO_CAMERA_WALK"); walk != nullptr && *walk != '\0')
			gAutoWalk = std::strtof(walk, nullptr);
		// either waits for the model's install, which frames the views (a big model's installs frames after its load is
		// ready: Bistro's came after 30 frames of placements), as SPEEDO_AUTOLOAD_EXIT's count does
		const char* model = std::getenv("SPEEDO_AUTOLOAD_MODEL");
		gAutoCameraAfterModel = model != nullptr && *model != '\0';
		if (const char* camera = std::getenv("SPEEDO_CAMERA"); camera != nullptr && std::string_view(camera) == "frame")
			gAutoFrame = true;
		else if (camera != nullptr && *camera != '\0')
		{
			const char* it = camera;
			if (auto values = parsePose(it))
			{
				gAutoCamera = values;
				const char* model = std::getenv("SPEEDO_AUTOLOAD_MODEL");
				gAutoCameraAfterModel = model != nullptr && *model != '\0';
			}
			else
				std::println(stderr, "SPEEDO_CAMERA: expected x,y,z,pitch,yaw, got {}", camera);
		}
		if (const char* path = std::getenv("SPEEDO_CAMERA_PATH"); path != nullptr && *path != '\0')
		{
			for (const char* it = path; *it != '\0';)
			{
				auto pose = parsePose(it);
				if (!pose)
				{
					std::println(stderr, "SPEEDO_CAMERA_PATH: expected x,y,z,pitch,yaw[;...], got {}", path);
					gAutoPath.clear();
					break;
				}
				gAutoPath.push_back(*pose);
				if (*it == ';')
					it++;
			}
		}
	}

	// the autoloaded model installed (its file named by the scene), or given up on 600 drawn frames after the loads were
	// ready: one that failed never installs, and the run must still end (and report it)
	bool autoLoadsReady = std::ranges::all_of(gAutoLoads, [](const auto& load) { return load.IsReady(); });
	static std::optional<uint64_t> gAutoLoadsReadyFrame;
	if (autoLoadsReady && !gAutoLoadsReadyFrame)
		gAutoLoadsReadyFrame = gDrawnFrameCount.load(std::memory_order_relaxed);
	constexpr uint64_t kInstallWaitFrameCount = 600;
	bool modelInstalled = !gAutoCameraAfterModel || !App().GetScene().GetScenes().Read().Get().filePath.empty() ||
						  (gAutoLoadsReadyFrame && gDrawnFrameCount.load(std::memory_order_relaxed) > *gAutoLoadsReadyFrame + kInstallWaitFrameCount);
	if ((gAutoCamera || gAutoFrame) && autoLoadsReady && modelInstalled)
	{
		if (gAutoFrame)
		{
			// on the draw thread, as the Camera menu does
			auto [task, future] = core::CreateTask([this] { myViews->UseSceneCamera(std::nullopt); });
			rhi.drawCalls.enqueue(task);
		}
		else
		{
			// on the draw thread too, after the install (which frames the views, or sets the model's camera), and for a
			// few frames: the install may be queued after the scene names its file
			auto values = *gAutoCamera;
			auto [task, future] = core::CreateTask(
				[this, values]
				{ myViews->Place(glm::vec3(values[0], values[1], values[2]), glm::vec3(glm::radians(values[3]), glm::radians(values[4]), 0.0F)); });
			rhi.drawCalls.enqueue(task);
		}
		static uint32_t gAutoCameraRepeats = 30;
		if (--gAutoCameraRepeats == 0)
		{
			gAutoCamera.reset();
			gAutoFrame = false;
		}
	}
	if (!gAutoPath.empty() && !gAutoPathFollowed && !gAutoCamera && !gAutoFrame && autoLoadsReady && modelInstalled)
	{
		// placed by the views as they upload, a pose a frame (draw calls from the ui's frames needn't run in their order:
		// the queue keeps it per thread only, and the ui's frames run on any of the executor's)
		std::vector<std::pair<glm::vec3, glm::vec3>> poses;
		for (const auto& values : gAutoPath)
			poses.emplace_back(glm::vec3(values[0], values[1], values[2]), glm::vec3(glm::radians(values[3]), glm::radians(values[4]), 0.0F));
		myViews->FollowPath(std::move(poses));
		gAutoPathFollowed = true;
	}
	if ((gAutoTurn != 0.0F || gAutoWalk != 0.0F) && std::ranges::all_of(gAutoLoads, [](const auto& load) { return load.IsReady(); }) &&
		!App().GetScene().GetScenes().Read().Get().filePath.empty())
	{
		// from the frame after the placement
		if (gAutoTurning && !gAutoCamera && !gAutoFrame)
			myViews->Turn(gAutoTurn * ImGui::GetIO().DeltaTime, gAutoWalk * ImGui::GetIO().DeltaTime);
		gAutoTurning = true;
	}

	// the installs are queued to the draw thread when the loads finish, so they have happened a frame later. the count is
	// of drawn frames (the ui thread's run ahead of them), from when the views are placed (SPEEDO_CAMERA), after the model's
	// install: a big model's came late in a count of the ui's frames from the loads, and a capture showed the frame before
	// the placement
	static std::optional<uint64_t> gAutoLoadExitStart;
	if (gAutoLoadExitFrames && std::ranges::all_of(gAutoLoads, [](const auto& load) { return load.IsReady(); }) && !gAutoCamera &&
		!gAutoFrame && modelInstalled && (gAutoPath.empty() || (gAutoPathFollowed && !myViews->OnPath())))
	{
		auto drawn = gDrawnFrameCount.load(std::memory_order_relaxed);
		if (!gAutoLoadExitStart)
			gAutoLoadExitStart = drawn;
		if (drawn < *gAutoLoadExitStart + *gAutoLoadExitFrames)
		{
			// still drawing
		}
		else if (const char* capture = std::getenv("SPEEDO_CAPTURE"); capture != nullptr && *capture != '\0' && gCaptureState != 3)
		{
			// the last frame read back first (see gCaptureState)
			uint8_t idle = 0;
			if (gCapturePath.empty())
				gCapturePath = capture;
			gCaptureState.compare_exchange_strong(idle, 1);
		}
		else
		{
			core::Application::Get()->RequestExit();
			gAutoLoadExitFrames.reset();
		}
	}

	// the views' grid is owned by the draw thread (see Views::OnResizeGrid), so track the requested one here. nothing else
	// changes the grid, so the window's is current when this is initialized
	static Extent2d gSplitScreenGrid{.width = myViews->GetGrid().x, .height = myViews->GetGrid().y};
	auto setSplitScreenGrid = [this, &rhi](uint32_t width, uint32_t height)
	{
		if (gSplitScreenGrid.width == width && gSplitScreenGrid.height == height)
			return;
		gSplitScreenGrid = {.width = width, .height = height};
		auto [resizeTask, resizeFuture] = core::CreateTask(
			[this, grid = gSplitScreenGrid] { myViews->OnResizeGrid({grid.width, grid.height}); });
		rhi.drawCalls.enqueue(resizeTask);
	};
	// SPEEDO_LAYOUT=<columns>x<rows> (1x1, 1x2, 2x1 or 2x2): the views' grid at startup, as View > Layout sets it
	if (static bool gLayoutDone = false; !gLayoutDone)
	{
		gLayoutDone = true;
		if (const char* layout = std::getenv("SPEEDO_LAYOUT"); layout != nullptr)
		{
			std::string_view text(layout);
			if (text.size() == 3 && text[1] == 'x' && (text[0] == '1' || text[0] == '2') && (text[2] == '1' || text[2] == '2'))
				setSplitScreenGrid(static_cast<uint32_t>(text[0] - '0'), static_cast<uint32_t>(text[2] - '0'));
			else
				std::println(stderr, "SPEEDO_LAYOUT: expected 1x1, 1x2, 2x1 or 2x2, got {}", layout);
		}
	}

	// the menus' shortcuts: the labels MenuItem shows are only text. global routes, so they work whichever window has
	// the focus, unless its active item takes the chord (a text field's ctrl+a). ImGuiMod_Ctrl is cmd on macOS
	// (io.ConfigMacOSXBehaviors swaps them), as the labels say
	if (Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal))
	{
		gShowBrowser = !(gShowBrowser && gBrowserPurpose == BrowserPurpose::kFiles);
		gBrowserPurpose = BrowserPurpose::kFiles;
	}
	if (Shortcut(ImGuiMod_Ctrl | ImGuiKey_E, ImGuiInputFlags_RouteGlobal))
	{
		gShowBrowser = !(gShowBrowser && gBrowserPurpose == BrowserPurpose::kEnvironment);
		gBrowserPurpose = BrowserPurpose::kEnvironment;
	}
	if (Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, ImGuiInputFlags_RouteGlobal))
		core::Application::Get()->RequestExit();
	if (Shortcut(ImGuiMod_Ctrl | ImGuiKey_1, ImGuiInputFlags_RouteGlobal))
		setSplitScreenGrid(1, 1);
	if (Shortcut(ImGuiMod_Ctrl | ImGuiKey_2, ImGuiInputFlags_RouteGlobal))
		setSplitScreenGrid(1, 2);
	if (Shortcut(ImGuiMod_Ctrl | ImGuiKey_3, ImGuiInputFlags_RouteGlobal))
		setSplitScreenGrid(2, 1);
	if (Shortcut(ImGuiMod_Ctrl | ImGuiKey_4, ImGuiInputFlags_RouteGlobal))
		setSplitScreenGrid(2, 2);
	// the debug views: back to the light, or the one before or after in View > Debug View's order (the indirect
	// lighting's only while it runs)
	{
		auto& settings = App().GetScene().GetSettings();
		if (Shortcut(ImGuiMod_Ctrl | ImGuiKey_0, ImGuiInputFlags_RouteGlobal))
			settings.debugView.store(DEBUG_VIEW_NONE, std::memory_order_relaxed);
		int step = Shortcut(ImGuiMod_Ctrl | ImGuiKey_RightBracket, ImGuiInputFlags_RouteGlobal) ? 1
				   : Shortcut(ImGuiMod_Ctrl | ImGuiKey_LeftBracket, ImGuiInputFlags_RouteGlobal) ? -1
																								  : 0;
		if (step != 0)
		{
			bool gi = settings.indirectLighting.load(std::memory_order_relaxed);
			std::vector<uint32_t> order{DEBUG_VIEW_NONE};
			for (const auto& entry : ui::kDebugViews)
				if (gi || entry.group != ui::DebugViewGroup::kIndirectLighting)
					order.push_back(entry.view.view);
			auto at = std::ranges::find(order, settings.debugView.load(std::memory_order_relaxed));
			auto index = at == order.end() ? 0 : static_cast<int>(at - order.begin());
			auto count = static_cast<int>(order.size());
			settings.debugView.store(order[static_cast<size_t>((index + step + count) % count)], std::memory_order_relaxed);
		}
	}

	// the menu bar, as macOS's: translucent over its backdrop, which the renderer blurs (a frosted panel, see
	// ImGuiLayer::TakeFrame), a little taller, without a border, with a faint edge below. its window would draw its own
	// background under the menu bar's (WindowBg, 85% opaque)
	PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
	PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(GetStyle().FramePadding.x, GetStyle().FramePadding.y * 1.75F));
	PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
	if (BeginMainMenuBar())
	{
		gMenuBarHeight = GetWindowHeight();
		auto* drawList = GetWindowDrawList();
		drawList->PushClipRectFullScreen();
		drawList->AddLine(
			ImVec2(GetWindowPos().x, GetWindowPos().y + GetWindowHeight() - 0.5F),
			ImVec2(GetWindowPos().x + GetWindowWidth(), GetWindowPos().y + GetWindowHeight() - 0.5F),
			IM_COL32(255, 255, 255, 40));
		drawList->PopClipRect();

		if (ui::BeginMenu("File"))
		{
			if (MenuItem("Open...", SHORTCUT_LABEL("O"), gShowBrowser && gBrowserPurpose == BrowserPurpose::kFiles))
			{
				gShowBrowser = !(gShowBrowser && gBrowserPurpose == BrowserPurpose::kFiles);
				gBrowserPurpose = BrowserPurpose::kFiles;
			}
			SetItemTooltip("Opens models, zip archives, images and panoramas from the file browser: preview them, and open one or several (side by side, or as authored).");
			Separator();
			if (MenuItem("Exit", SHORTCUT_LABEL("Q")))
				core::Application::Get()->RequestExit();

			ImGui::EndMenu();
		}
		if (ui::BeginMenu("View"))
		{
			// if (MenuItem("Node Editor..."))
			// 	showNodeEditor = !showNodeEditor;
			if (ui::BeginMenu("Layout"))
			{
				for (auto [label, shortcut, width, height] : std::array{
						 std::tuple{"1x1", SHORTCUT_LABEL("1"), 1U, 1U},
						 std::tuple{"1x2", SHORTCUT_LABEL("2"), 1U, 2U},
						 std::tuple{"2x1", SHORTCUT_LABEL("3"), 2U, 1U},
						 std::tuple{"2x2", SHORTCUT_LABEL("4"), 2U, 2U}})
					if (MenuItem(label, shortcut, gSplitScreenGrid.width == width && gSplitScreenGrid.height == height))
						setSplitScreenGrid(width, height);

				ImGui::EndMenu();
			}
			if (ui::BeginMenu("Presentation"))
			{
				// how the window goes fullscreen (alt+enter) and presents (see FULLSCREEN_MODE_* and Swapchain::Configure):
				// to compare them, e.g. a driver's direct scanout on macOS
				auto& windowState = GetWindow(GetCurrentWindow()).GetState();
				SeparatorText("Fullscreen (Alt+Enter)");
				for (auto [label, mode] : std::array{
						 std::pair{"Exclusive", FULLSCREEN_MODE_EXCLUSIVE},
						 std::pair{"Borderless", FULLSCREEN_MODE_BORDERLESS},
#if defined(__APPLE__)
						 std::pair{"Native", FULLSCREEN_MODE_NATIVE},
#endif
					 })
					if (MenuItem(label, nullptr, windowState.fullscreenMode == mode))
						windowState.fullscreenMode = mode;
				SetItemTooltip("Exclusive takes the monitor (on macOS, native too: scanned out directly); borderless is a window covering it, which stays composited. Takes effect the next time the window goes fullscreen.");
				// what the swapchain has, of what the surface supports (none listed until the first frame)
				auto presentation = UnpackPresentation(myPresentation.load(std::memory_order_relaxed));
				SeparatorText("Swapchain");
				for (auto [label, option, tooltip] : std::array{
						 std::tuple{"FIFO", rhi::PresentationMode::kFifo, "In order, one frame per vertical blank."},
						 std::tuple{"FIFO Relaxed", rhi::PresentationMode::kFifoRelaxed, "As FIFO, but a late frame is shown at once (may tear then)."},
						 std::tuple{"Mailbox", rhi::PresentationMode::kMailbox, "One frame per vertical blank, the newest: no tearing, lowest latency."},
						 std::tuple{"Immediate", rhi::PresentationMode::kImmediate, "At once, without waiting for the vertical blank (may tear)."}})
				{
					if ((presentation.modes & (1U << static_cast<uint32_t>(option))) == 0)
						continue;
					if (MenuItem(label, nullptr, presentation.mode == option))
					{
						auto request = presentation;
						request.mode = option;
						myPresentationRequest.store(PackPresentation(request), std::memory_order_relaxed);
					}
					SetItemTooltip("%s", tooltip);
				}
				// the image counts the surface takes (no limit: two more than the least)
				auto maxImageCount = presentation.maxImageCount != 0 ? presentation.maxImageCount : presentation.minImageCount + 2;
				for (uint32_t count = presentation.minImageCount; count != 0 && count <= maxImageCount; count++)
				{
					bool only = presentation.minImageCount == maxImageCount;
					if (MenuItem(std::format("{} images{}", count, only ? " (all the surface takes)" : "").c_str(), nullptr, presentation.imageCount == count, !only))
					{
						auto request = presentation;
						request.imageCount = count;
						myPresentationRequest.store(PackPresentation(request), std::memory_order_relaxed);
					}
				}
				SetItemTooltip("The swapchain is recreated before the next frame.");
				ImGui::EndMenu();
			}
			if (ui::BeginMenu("Environment"))
			{
				// what lights the scene (see InstallEnvironment): the procedural sky, or a panorama file
				std::string current = App().GetScene().GetEnvironmentName().Read().Get();
				TextDisabled("%s", current.empty() ? "None" : current.c_str());
				{
					float intensity = App().GetScene().GetSettings().environmentIntensity.load(std::memory_order_relaxed);
					if (DragFloat("Intensity", &intensity, 0.01F, 0.0F, 100.0F, "%.2f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
						App().GetScene().GetSettings().environmentIntensity.store(intensity, std::memory_order_relaxed);
					SetItemTooltip("Scales its light. Drag, or double-click to type.");
					float rotation = App().GetScene().GetSettings().environmentRotationDegrees.load(std::memory_order_relaxed);
					if (DragFloat("Rotation", &rotation, 0.5F, -180.0F, 180.0F, "%.1f deg", ImGuiSliderFlags_WrapAround))
						App().GetScene().GetSettings().environmentRotationDegrees.store(rotation, std::memory_order_relaxed);
					SetItemTooltip("Turns it about the vertical axis. Drag, or double-click to type.");
					bool backdrop = App().GetScene().GetSettings().environmentBackdrop.load(std::memory_order_relaxed);
					if (MenuItem("Backdrop", nullptr, &backdrop))
						App().GetScene().GetSettings().environmentBackdrop.store(backdrop, std::memory_order_relaxed);
					// its strongest light sources, directional lights (with shadows) rather than part of the environment
					{
						auto info = App().GetScene().GetEnvironmentLightInfo().Read();
						if (info.Get().empty())
							TextDisabled("Dominant lights: none");
						for (size_t lightIt = 0; lightIt < info.Get().size(); lightIt++)
						{
							const auto& light = info.Get()[lightIt];
							if (light.merged)
								TextDisabled("Dominant light %zu: %.3g lux, the model's own", lightIt + 1, light.lux);
							else
								TextDisabled("Dominant light %zu: %.3g lux (x intensity)", lightIt + 1, light.lux);
							SetItemTooltip(
								"One of the environment's strongest light sources, which light the scene as directional "
								"lights (with shadows). Where the model has a directional light from the same direction, "
								"the model's is used instead, if merging is on.");
						}
					}
					bool merge = App().GetScene().GetSettings().mergeLights.load(std::memory_order_relaxed);
					if (MenuItem("Merge with the model's lights", nullptr, &merge))
						App().GetScene().GetSettings().mergeLights.store(merge, std::memory_order_relaxed);
					SetItemTooltip(
						"Treats a dominant light and a directional light of the model's within 5 degrees of it as one, "
						"the model's. Off: both light the scene.");
				}
				Separator();
				if (MenuItem("Procedural sky"))
					(void)gLoads.Enqueue(
						"Procedural sky",
						[&rhi](std::atomic_uint8_t& progress) { App().GetScene().LoadEnvironment(std::nullopt, progress); });
				if (MenuItem("Open Environment...", SHORTCUT_LABEL("E"), gShowBrowser && gBrowserPurpose == BrowserPurpose::kEnvironment))
				{
					// the file browser, listing panoramas only
					gShowBrowser = !(gShowBrowser && gBrowserPurpose == BrowserPurpose::kEnvironment);
					gBrowserPurpose = BrowserPurpose::kEnvironment;
				}
				ImGui::EndMenu();
			}
			if (ui::BeginMenu("Scene"))
			{
				// the installed gltf file's scenes: choosing one loads the file again with it
				Scene::SceneState scenes = App().GetScene().GetScenes().Read().Get();
				if (scenes.names.size() < 2)
					TextDisabled(scenes.names.empty() ? "No scenes" : "One scene");
				for (size_t sceneIt = 0; sceneIt < scenes.names.size(); sceneIt++)
				{
					PushID(static_cast<int>(sceneIt));
					if (MenuItem(scenes.names[sceneIt].c_str(), nullptr, scenes.current == sceneIt) && scenes.current != sceneIt)
						(void)gLoads.Enqueue(
							std::format("{} ({})", std::filesystem::path(scenes.filePath).filename().string(), scenes.names[sceneIt]),
							[&rhi, path = scenes.filePath, sceneIt, variant = scenes.variant](std::atomic_uint8_t& progress)
							{
								App().GetScene().LoadModels(
									{path}, progress, {.scene = sceneIt, .variant = variant ? std::optional<size_t>(*variant) : std::nullopt});
							});
					PopID();
				}
				ImGui::EndMenu();
			}
			if (ui::BeginMenu("Variant"))
			{
				// the installed gltf file's material variants (KHR_materials_variants): choosing one loads the file again
				// with its materials, in the scene shown
				Scene::SceneState scenes = App().GetScene().GetScenes().Read().Get();
				if (scenes.variants.empty())
					TextDisabled("No variants");
				auto load = [&scenes](std::optional<size_t> variant, std::string_view name)
				{
					(void)gLoads.Enqueue(
						std::format("{} ({})", std::filesystem::path(scenes.filePath).filename().string(), name),
						[path = scenes.filePath, scene = scenes.current, variant](std::atomic_uint8_t& progress)
						{ App().GetScene().LoadModels({path}, progress, {.scene = scene, .variant = variant}); });
				};
				if (!scenes.variants.empty() && MenuItem("Default", nullptr, !scenes.variant) && scenes.variant)
					load(std::nullopt, "default materials");
				for (size_t variantIt = 0; variantIt < scenes.variants.size(); variantIt++)
				{
					PushID(static_cast<int>(variantIt));
					bool current = scenes.variant == variantIt;
					if (MenuItem(scenes.variants[variantIt].c_str(), nullptr, current) && !current)
						load(variantIt, scenes.variants[variantIt]);
					PopID();
				}
				ImGui::EndMenu();
			}
			if (ui::BeginMenu("Animation"))
			{
				// the installed model's animations (see Model::Animate)
				auto animation = App().GetScene().GetAnimation().Write();
				auto& state = animation.Get();
				MenuItem("Play", nullptr, &state.playing);
				if (MenuItem("Restart"))
					state.time = 0.0;
				Separator();
				if (MenuItem("Rest pose", nullptr, !state.selected.has_value()))
					state.Select(std::nullopt);
				for (size_t animationIt = 0; animationIt < state.names.size(); animationIt++)
				{
					PushID(static_cast<int>(animationIt));
					if (MenuItem(state.names[animationIt].c_str(), nullptr, state.selected == animationIt))
						state.Select(animationIt);
					PopID();
				}
				ImGui::EndMenu();
			}
			if (ui::BeginMenu("Camera"))
			{
				// the installed model's cameras (see Views::SetScene), applied on the draw thread
				auto useCamera = [this, &rhi](std::optional<size_t> camera)
				{
					auto [task, future] = core::CreateTask([this, camera] { myViews->UseSceneCamera(camera); });
					rhi.drawCalls.enqueue(task);
				};
				auto current = myViews->GetSceneCamera();
				if (MenuItem("Frame model", nullptr, !current.has_value()))
					useCamera(std::nullopt);
				auto names = myViews->GetSceneCameraNames();
				if (!names.empty())
					Separator();
				for (size_t cameraIt = 0; cameraIt < names.size(); cameraIt++)
				{
					PushID(static_cast<int>(cameraIt));
					if (MenuItem(names[cameraIt].c_str(), nullptr, current == cameraIt))
						useCamera(cameraIt);
					PopID();
				}
				ImGui::EndMenu();
			}
			MenuItem("FPS", nullptr, &gShowFps);
			MenuItem("TPS", nullptr, &gShowTps);
			Separator();
			{
				// logarithmic: model scales span many orders of magnitude (see Views::FrameBounds). a drag box rather than a
				// slider: a double-click (or Ctrl/Cmd+click) types a number into it, which a slider only does on
				// Ctrl/Cmd+click. its speed is in the logarithmic range's terms: about 1% faster or slower per pixel.
				constexpr float kMinMoveSpeed = 1e-4F;
				constexpr float kMaxMoveSpeed = 1e6F;
				constexpr float kDragPixelsAcrossRange = 1000.0F;
				float moveSpeed = myViews->GetMoveSpeed();
				if (DragFloat(
						"Move speed",
						&moveSpeed,
						(kMaxMoveSpeed - kMinMoveSpeed) / kDragPixelsAcrossRange,
						kMinMoveSpeed,
						kMaxMoveSpeed,
						"%.3g units/s",
						ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
					myViews->SetMoveSpeed(moveSpeed);
				SetItemTooltip(
					"Drag to change, or double-click to type a speed.\n"
					"w, a, s, d move the camera under the mouse, and the mouse wheel changes the speed.");
			}
			{
				float stops = App().GetScene().GetSettings().exposureStops.load(std::memory_order_relaxed);
				if (DragFloat("Exposure", &stops, 0.05F, -16.0F, 16.0F, "%+.2f EV", ImGuiSliderFlags_AlwaysClamp))
					App().GetScene().GetSettings().exposureStops.store(stops, std::memory_order_relaxed);
				SetItemTooltip("Scales the image by 2^EV before tonemapping (with auto exposure, on top of what it picks). Drag, or double-click to type.");
				bool autoExposure = App().GetScene().GetSettings().autoExposure.load(std::memory_order_relaxed);
				if (MenuItem("Auto Exposure", nullptr, &autoExposure))
					App().GetScene().GetSettings().autoExposure.store(autoExposure, std::memory_order_relaxed);
				SetItemTooltip("Adapts the exposure to the scene's brightness over time.");
				if (autoExposure)
					TextDisabled("Auto: %+.2f EV", App().GetScene().GetAutoExposureStops());
				bool shadows = App().GetScene().GetSettings().shadows.load(std::memory_order_relaxed);
				if (MenuItem("Shadows", nullptr, &shadows))
					App().GetScene().GetSettings().shadows.store(shadows, std::memory_order_relaxed);
				SetItemTooltip("The lights' shadows: cascades for directional lights, cubes for point lights, a view per spot light.");
				bool culling = App().GetScene().GetSettings().culling.load(std::memory_order_relaxed);
				if (MenuItem("Culling", nullptr, &culling))
					App().GetScene().GetSettings().culling.store(culling, std::memory_order_relaxed);
				SetItemTooltip("Skips the chunks of the model outside each view's frustum, and each shadow view's.");
				bool clusteredLights = App().GetScene().GetSettings().clusteredLights.load(std::memory_order_relaxed);
				if (MenuItem("Clustered Lights", nullptr, &clusteredLights))
					App().GetScene().GetSettings().clusteredLights.store(clusteredLights, std::memory_order_relaxed);
				SetItemTooltip("Shades each pixel with only the lights that may reach its cluster (a tile of the view and a slice of depth), not all of them.");
				bool indirectLighting = App().GetScene().GetSettings().indirectLighting.load(std::memory_order_relaxed);
				if (MenuItem("Indirect Lighting", nullptr, &indirectLighting))
					App().GetScene().GetSettings().indirectLighting.store(indirectLighting, std::memory_order_relaxed);
				SetItemTooltip(
					"Diffuse ambient light from radiance cascades over a voxel clipmap (experimental, SPEEDO_GI=1 to start with it): "
					"bounced light, colour bleeding, and the sky occluded indoors. Turned on, the scene is voxelized again.");
				if (ui::BeginMenu("Debug View"))
				{
					// what the views show instead of their light (see DEBUG_VIEW_*): values (base color, normals, ...) as they
					// are, light exposed and tonemapped
					auto& settings = App().GetScene().GetSettings();
					auto current = settings.debugView.load(std::memory_order_relaxed);
					if (MenuItem("Lit", SHORTCUT_LABEL("0"), current == DEBUG_VIEW_NONE))
						settings.debugView.store(DEBUG_VIEW_NONE, std::memory_order_relaxed);
					SetItemTooltip("The scene's light, as rendered. " SHORTCUT_LABEL("[") " and " SHORTCUT_LABEL("]") " step through the debug views.");
					auto group = [&](ui::DebugViewGroup kind, const char* label, bool enabled)
					{
						SeparatorText(label);
						BeginDisabled(!enabled);
						for (const auto& entry : ui::kDebugViews)
						{
							if (entry.group != kind)
								continue;
							if (MenuItem(entry.view.name, nullptr, current == entry.view.view))
								settings.debugView.store(entry.view.view, std::memory_order_relaxed);
							SetItemTooltip("%s", entry.view.description);
						}
						EndDisabled();
					};
					group(ui::DebugViewGroup::kSurface, "Surface", true);
					group(ui::DebugViewGroup::kLighting, "Lighting", true);
					group(ui::DebugViewGroup::kIndirectLighting, "Indirect Lighting", indirectLighting);
					if (!indirectLighting)
						TextDisabled("Turn on View > Indirect Lighting for these.");
					ImGui::EndMenu();
				}
				if (ui::BeginMenu("Tonemapper"))
				{
					static constexpr std::array<std::pair<const char*, uint32_t>, 5> kTonemappers{{
						{"Khronos PBR Neutral", TONEMAPPER_PBR_NEUTRAL},
						{"ACES", TONEMAPPER_ACES},
						{"AgX", TONEMAPPER_AGX},
						{"Reinhard", TONEMAPPER_REINHARD},
						{"Linear (clamped)", TONEMAPPER_LINEAR},
					}};
					auto current = App().GetScene().GetSettings().tonemapper.load(std::memory_order_relaxed);
					for (auto [name, tonemapper] : kTonemappers)
						if (MenuItem(name, nullptr, current == tonemapper))
							App().GetScene().GetSettings().tonemapper.store(tonemapper, std::memory_order_relaxed);
					ImGui::EndMenu();
				}
			}
			if (MenuItem("Lights...", nullptr, gShowLights))
				gShowLights = !gShowLights;
			SetItemTooltip("Lists the lights, and draws them over the views: where they are (a directional light outside the scene, shining in), spot cones, point light ranges and the shadow views' frusta.");
#if (SPEEDO_PROFILING_LEVEL > 0)
			if (MenuItem("Statistics...", nullptr, gShowStatistics))
				gShowStatistics = !gShowStatistics;
			SetItemTooltip("The passes' gpu timings (where the driver writes them), submits, transient memory, and in debug builds the vulkan objects.");
#endif
			ImGui::EndMenu();
		}
		if (ui::BeginMenu("About"))
		{
			if (MenuItem("Show IMGUI Demo..."))
				gShowDemoWindow = !gShowDemoWindow;
			Separator();
			if (MenuItem("About client..."))
				gShowAbout = !gShowAbout;
			ImGui::EndMenu();
		}

		EndMainMenuBar();
	}
	PopStyleVar(2);
	PopStyleColor();

	myImGui->EndFrame();
}

} // namespace client
