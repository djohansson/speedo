#pragma once

#include "command.h"
#include "device.h"
#include "instance.h"
#include "pipeline.h"
#include "renderimageset.h"
#include "types.h"
#include "window.h"

#include <core/application.h>
#include <core/concurrency-utils.h>
#include <core/file.h>
#include <core/nodegraph.h>

// todo: move to Config.h
#if defined(__WINDOWS__)
#	include <sdkddkver.h>
#endif

#include <algorithm>
#include <filesystem>
#include <functional>
#include <future>
#include <thread>
#include <tuple>

#include <nfd.h>

template <GraphicsBackend B>
class GraphicsApplication : public ApplicationBase
{
public:
	GraphicsApplication(const WindowState& window);
	~GraphicsApplication();

	std::string_view getName() const final;

	bool tick();

	void resizeWindow(const WindowState& window);
	void resizeFramebuffer(int width, int height);

	void onMouse(const MouseState& mouse);
	void onKeyboard(const KeyboardState& keyboard);

	void setGraphicsBackend(GraphicsBackend backend);

private:
	GraphicsApplication();

	void initIMGUI(
		const WindowState& window,
		const std::shared_ptr<Device<B>>& device,
		CommandBufferHandle<B> commands,
		RenderPassHandle<B> renderPass,
		SurfaceHandle<B> surface,
		const std::filesystem::path& userProfilePath) const;
	void shutdownIMGUI();

	void createWindowDependentObjects(Extent2d<B> frameBufferExtent);

	auto& gfx() noexcept { return myGraphicsContext; }
	const auto& gfx() const noexcept { return myGraphicsContext; }

	TaskExecutor myExecutor{std::max(1u, std::thread::hardware_concurrency() - 1)};

	InputState myInput{};

	struct GraphicsContext
	{
		std::shared_ptr<Instance<B>> myInstance;
		std::shared_ptr<Device<B>> myDevice;

		std::shared_ptr<Window<B>> myMainWindow;
		std::shared_ptr<Pipeline<B>> myPipeline;

		CircularContainer<Queue<B>> myGraphicsQueues;
		CircularContainer<Queue<B>> myComputeQueues;
		CircularContainer<Queue<B>> myTransferQueues;

		enum CommandType : uint8_t
		{
			CommandType_GeneralPrimary,
			CommandType_GeneralSecondary,
			CommandType_GeneralTransfer,
			CommandType_DedicatedCompute,
			CommandType_DedicatedTransfer,
			CommandType_Count
		};

		std::array<CircularContainer<CommandPoolContext<B>>, CommandType_Count> myCommands;

		//std::shared_ptr<ResourceContext<B>> myResources;

		std::shared_ptr<RenderImageSet<B>> myRenderImageSet;

		std::unique_ptr<Buffer<B>> myMaterials;
		std::unique_ptr<Buffer<B>> myObjects;

		template <
			typename Key,
			typename Handle,
			typename KeyHash = HandleHash<Key, Handle>,
			typename KeyEqualTo = SharedPtrEqualTo<>>
		using HandleSet = UnorderedSet<Key, KeyHash, KeyEqualTo>;
		HandleSet<std::shared_ptr<PipelineLayout<B>>, PipelineLayoutHandle<B>> myLayouts;
	};

	GraphicsContext myGraphicsContext{std::make_shared<Instance<B>>()};

	Future<std::tuple<nfdresult_t, nfdchar_t*, std::function<uint32_t(nfdchar_t*)>>> myOpenFileFuture;

	std::function<void()> myIMGUIPrepareDrawFunction;
	std::function<void(CommandBufferHandle<B> cmd)> myIMGUIDrawFunction;

	Future<void> myPresentFuture;

	bool myRequestExit = false;

	//AutoSaveJSONFileObject<NodeGraph> myNodeGraph; // temp - should be stored elsewhere
};

#include "graphicsapplication.inl"
