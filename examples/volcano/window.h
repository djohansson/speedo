#pragma once

#include "buffer.h"
#include "device.h"
#include "frame.h"
#include "gfx.h"
#include "glm.h"
#include "state.h"
#include "swapchain.h"
#include "texture.h"
#include "view.h"

#include <optional>
#include <memory>
#include <vector>

#include <imgui.h>

#include <nfd.h>

template <GraphicsBackend B>
struct WindowDesc
{
	std::shared_ptr<DeviceContext<B>> deviceContext;
	Extent2d<B> windowExtent;
	Extent2d<B> framebufferExtent;
	bool clearEnable = true;
	ClearValue<B> clearValue = {};
	bool imguiEnable = true;
};

template <GraphicsBackend B>
class Window
{
public:

	struct ViewBufferData // todo: needs to be aligned to VkPhysicalDeviceLimits.minUniformBufferOffsetAlignment. right now uses manual padding.
	{
		glm::mat4 viewProj = glm::mat4(1.0f);
		glm::mat4 pad0;
		glm::mat4 pad1;
		glm::mat4 pad2;
	};

	Window(WindowDesc<B>&& desc, const CommandContext<B>& commands);
	~Window();

	const auto& getWindowDesc() const { return myDesc; }
	const auto& getSwapchainContext() const { return *mySwapchainContext; }
	const auto& getDepthTexture() const { return *myDepthTexture; }
	const auto& getViews() const { return myViews; }
	const auto& getActiveView() const { return myActiveView; }
	const auto& getViewBuffer() const { return *myViewBuffer; }
	const auto& getFrameIndex() const { return myFrameIndex; }
	const auto& getLastFrameIndex() const { return myLastFrameIndex; }
	const auto& getFrames() const { return myFrames; }
	const auto& getRenderPass() const { return myRenderPass; }

	void createFrameObjects(const CommandContext<B>& commands);
	void destroyFrameObjects();
	
	void updateInput(const InputState& input);

	void submitFrame(const PipelineConfiguration<B>& config);
	void presentFrame() const;

private:

	void drawIMGUI();
	void updateViewBuffer() const;

	WindowDesc<B> myDesc = {};
	std::unique_ptr<SwapchainContext<B>> mySwapchainContext;
	std::unique_ptr<Texture<B>> myDepthTexture;
	std::vector<View> myViews;
	std::optional<size_t> myActiveView;
	std::unique_ptr<Buffer<B>> myViewBuffer; // cbuffer data for all views
	uint32_t myFrameIndex = 0; // Current frame being rendered to (0 <= myFrameIndex < myFrames.size())
	uint32_t myLastFrameIndex = 0; // Last frame being rendered to (0 <= myLastFrameIndex < myFrames.size())
	std::vector<Frame<B>> myFrames;
	RenderPassHandle<B> myRenderPass = 0;

	static constexpr uint32_t NX = 2;
	static constexpr uint32_t NY = 2;
};


template <GraphicsBackend B>
void Window<B>::drawIMGUI()
{
    using namespace ImGui;

    NewFrame();
    ShowDemoWindow();

    {
        Begin("Render Options");
        // DragInt(
        //     "Command Buffer Threads", &myRequestedCommandBufferThreadCount, 0.1f, 2, 32);
        ColorEdit3(
            "Clear Color", &myDesc.clearValue.color.float32[0]);
        End();
    }

    {
        Begin("GUI Options");
        // static int styleIndex = 0;
        ShowStyleSelector("Styles" /*, &styleIndex*/);
        ShowFontSelector("Fonts");
        if (Button("Show User Guide"))
        {
            SetNextWindowPos(ImVec2(0.5f, 0.5f));
            OpenPopup("UserGuide");
        }
        if (BeginPopup(
                "UserGuide", ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        {
            ShowUserGuide();
            EndPopup();
        }
        End();
    }

    // {
    //     Begin("File");

    //     if (Button("Open OBJ file"))
    //     {
    //         nfdchar_t* pathStr;
    //         auto res = NFD_OpenDialog("obj", std::filesystem::absolute(myResourcePath).u8string().c_str(), &pathStr);
    //         if (res == NFD_OKAY)
    //         {
    //             myDefaultResources->model = std::make_shared<Model<B>>(
    //                 myDevice, myTransferCommandPool, myQueue, myAllocator,
    //                 std::filesystem::absolute(pathStr));

    //             updateDescriptorSets(window, *myGraphicsPipelineConfig);
    //         }
    //     }

    //     if (Button("Open JPG file"))
    //     {
    //         nfdchar_t* pathStr;
    //         auto res = NFD_OpenDialog("jpg", std::filesystem::absolute(myResourcePath).u8string().c_str(), &pathStr);
    //         if (res == NFD_OKAY)
    //         {
    //             myDefaultResources->texture = std::make_shared<Texture<B>>(
    //                 myDevice, myTransferCommandPool, myQueue, myAllocator,
    //                 std::filesystem::absolute(pathStr));

    //             updateDescriptorSets(window, *myGraphicsPipelineConfig);
    //         }
    //     }

    //     End();
    // }

    {
        ShowMetricsWindow();
    }

    Render();
}
