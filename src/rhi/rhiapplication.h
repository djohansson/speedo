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
#include <memory>
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
struct Rhi
{
	std::shared_ptr<Instance<G>> instance;
	std::shared_ptr<Device<G>> device;

	std::shared_ptr<Window<G>> mainWindow;
	std::shared_ptr<Pipeline<G>> pipeline;

	CircularContainer<Queue<G>> graphicsQueues;
	CircularContainer<Queue<G>> computeQueues;
	CircularContainer<Queue<G>> transferQueues;

	std::array<CircularContainer<CommandPoolContext<G>>, CommandType_Count> commands;

	//std::shared_ptr<ResourceContext<G>> resources;

	std::shared_ptr<RenderImageSet<G>> renderImageSet;

	std::shared_ptr<Buffer<G>> materials;
	std::shared_ptr<Buffer<G>> objects;

	template <
		typename Key,
		typename Handle,
		typename KeyHash = HandleHash<Key, Handle>,
		typename KeyEqualTo = SharedPtrEqualTo<>>
	using HandleSet = UnorderedSet<Key, KeyHash, KeyEqualTo>;
	HandleSet<std::shared_ptr<PipelineLayout<G>>, PipelineLayoutHandle<G>> layouts;
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

	//void setGraphicsApi(GraphicsApi api);
	void createDevice(const WindowState& window);
	const WindowState& createWindow();

protected:
	RhiApplication(std::string_view name, Environment&& env);

	template <GraphicsApi G>
	Rhi<G>& rhi();

	template <GraphicsApi G>
	const Rhi<G>& rhi() const;

private:
	std::shared_ptr<void> myRhi;

	Future<std::tuple<nfdresult_t, nfdchar_t*, std::function<uint32_t(nfdchar_t*)>>> myOpenFileFuture;

	std::function<void()> myIMGUIPrepareDrawFunction;
	std::function<void(CommandBufferHandle<Vk> cmd)> myIMGUIDrawFunction; // todo: remove GraphicsApi specialization

	Future<void> myPresentFuture;

	bool myRequestExit = false;
};
