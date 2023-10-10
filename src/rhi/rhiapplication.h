#pragma once

#include "command.h"
#include "device.h"
#include "instance.h"
#include "pipeline.h"
#include "renderimageset.h"
#include "types.h"
#include "window.h"

#include <core/application.h>
#include <core/file.h>
#include <core/nodegraph.h>

// todo: move to Config.h
#if defined(__WINDOWS__)
#	include <sdkddkver.h>
#endif

#include <filesystem>
#include <functional>
#include <tuple>

#include <nfd.h>

enum CommandType : uint8_t
{
	CommandType_GeneralPrimary,
	CommandType_GeneralSecondary,
	CommandType_GeneralTransfer,
	CommandType_DedicatedCompute,
	CommandType_DedicatedTransfer,
	CommandType_Count
};

template <GraphicsApi G>
struct RhiBackend
{
	std::shared_ptr<Instance<G>> myInstance;
	std::shared_ptr<Device<G>> myDevice;

	std::shared_ptr<Window<G>> myMainWindow;
	std::shared_ptr<Pipeline<G>> myPipeline;

	CircularContainer<Queue<G>> myGraphicsQueues;
	CircularContainer<Queue<G>> myComputeQueues;
	CircularContainer<Queue<G>> myTransferQueues;

	std::array<CircularContainer<CommandPoolContext<G>>, CommandType_Count> myCommands;

	//std::shared_ptr<ResourceContext<G>> myResources;

	std::shared_ptr<RenderImageSet<G>> myRenderImageSet;

	std::unique_ptr<Buffer<G>> myMaterials;
	std::unique_ptr<Buffer<G>> myObjects;

	template <
		typename Key,
		typename Handle,
		typename KeyHash = HandleHash<Key, Handle>,
		typename KeyEqualTo = SharedPtrEqualTo<>>
	using HandleSet = UnorderedSet<Key, KeyHash, KeyEqualTo>;
	HandleSet<std::shared_ptr<PipelineLayout<G>>, PipelineLayoutHandle<G>> myLayouts;
};

class RhiApplication : public Application
{	
public:
	~RhiApplication() override;

	bool tick() override;

	void onResizeWindow(const WindowState& window);
	void onResizeFramebuffer(uint32_t width, uint32_t height);

	void onMouse(const MouseState& mouse);
	void onKeyboard(const KeyboardState& keyboard);

	//void setGraphicsBackend(GraphicsApi Gackend);
	void createDevice(const WindowState& window);
	const WindowState& createWindow();

protected:
	RhiApplication(Application::State&& state);

private:

	auto& gfx() noexcept { return std::get<RhiBackend<Vk>>(myGraphicsContext); }
	const auto& gfx() const noexcept { return std::get<RhiBackend<Vk>>(myGraphicsContext); }

	std::variant<RhiBackend<Vk>> myGraphicsContext; // todo: remove GraphicsApi specialization

	Future<std::tuple<nfdresult_t, nfdchar_t*, std::function<uint32_t(nfdchar_t*)>>> myOpenFileFuture;

	std::function<void()> myIMGUIPrepareDrawFunction;
	std::function<void(CommandBufferHandle<Vk> cmd)> myIMGUIDrawFunction; // todo: remove GraphicsApi specialization

	Future<void> myPresentFuture;

	bool myRequestExit = false;

	//AutoSaveJSONFileObject<NodeGraph> myNodeGraph; // temp - should be stored elsewhere
};
