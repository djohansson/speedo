#pragma once

#include "device.h"
#include "frame.h"
#include "rendertarget.h"
#include "types.h"

#include <memory>

template <GraphicsBackend B>
struct SwapchainCreateDesc : RenderTargetCreateDesc<B>
{
	SwapchainHandle<B> previous = 0;
};

template <GraphicsBackend B>
class SwapchainContext : public IRenderTarget<B>, public DeviceResource<B>
{
public:

	SwapchainContext(SwapchainContext&& other) = default;
	SwapchainContext(
		const std::shared_ptr<DeviceContext<B>>& deviceContext,
		SwapchainCreateDesc<B>&& desc);
    ~SwapchainContext();

	SwapchainContext& operator=(SwapchainContext&& other) = default;

	virtual const RenderTargetCreateDesc<B>& getRenderTargetDesc() const final;

	virtual ImageLayout<B> getColorImageLayout(uint32_t index) const final;
    virtual ImageLayout<B> getDepthStencilImageLayout() const final;

	virtual void clearSingleAttachment(
        CommandBufferHandle<B> cmd,
        const ClearAttachment<B>& clearAttachment) const final;
    virtual void clearAllAttachments(
        CommandBufferHandle<B> cmd,
        const ClearColorValue<B>& color = {},
        const ClearDepthStencilValue<B>& depthStencil = {}) const final;

	virtual void clearColor(CommandBufferHandle<B> cmd, const ClearColorValue<B>& color, uint32_t index) final;
    virtual void clearDepthStencil(CommandBufferHandle<B> cmd, const ClearDepthStencilValue<B>& depthStencil) final;

    virtual void transitionColor(CommandBufferHandle<B> cmd, ImageLayout<B> layout, uint32_t index) final;
    virtual void transitionDepthStencil(CommandBufferHandle<B> cmd, ImageLayout<B> layout) final;

	const std::optional<RenderPassBeginInfo<B>>& begin(CommandBufferHandle<B> cmd, SubpassContents<B> contents) final;
    void end(CommandBufferHandle<B> cmd) final;

    const auto& getDesc() const { return myDesc; }
	const auto& getSwapchain() const { return mySwapchain; }
	const auto& getFrames() const { return myFrames; }
	auto getFrameIndex() const { return myFrameIndex; }
	auto getLastFrameIndex() const { return myLastFrameIndex; }

	std::tuple<bool, uint64_t> flip();
	uint64_t submit(
		const std::shared_ptr<CommandContext<B>>& commandContext,
		uint64_t waitTimelineValue);
	void present();

private:

	const SwapchainCreateDesc<B> myDesc = {};
	SwapchainHandle<B> mySwapchain = 0;
	std::vector<std::unique_ptr<Frame<B>>> myFrames;
	uint32_t myFrameIndex = 0;
	uint32_t myLastFrameIndex = 0;
};
