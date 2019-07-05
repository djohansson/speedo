#include "application.h"

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

template <>
void Application<GraphicsBackend::Vulkan>::updateInput(WindowData<GraphicsBackend::Vulkan>& window) const
{
    ZoneScoped;

    auto& frame = window.frames[window.lastFrameIndex];
    float dt = frame.graphicsDeltaTime.count();

    // update input dependent state
    {
        auto& io = ImGui::GetIO();

        static bool escBufferState = false;
        bool escState = io.KeysDown[io.KeyMap[ImGuiKey_Escape]];
        if (escState && !escBufferState)
            window.imguiEnable = !window.imguiEnable;
        escBufferState = escState;
    }

    if (myCommandBufferThreadCount != myRequestedCommandBufferThreadCount)
        window.createFrameResourcesFlag = true;

    if (window.activeView)
    {
        // std::cout << "window.activeView read/consume" << std::endl;

        float dx = 0;
        float dz = 0;

        for (const auto& [key, pressed] : myKeysPressed)
        {
            if (pressed)
            {
                switch (key)
                {
                case GLFW_KEY_W:
                    dz = 1;
                    break;
                case GLFW_KEY_S:
                    dz = -1;
                    break;
                case GLFW_KEY_A:
                    dx = 1;
                    break;
                case GLFW_KEY_D:
                    dx = -1;
                    break;
                default:
                    break;
                }
            }
        }

        auto& view = window.views[*window.activeView];

        bool doUpdateViewMatrix = false;

        if (dx != 0 || dz != 0)
        {
            auto forward = glm::vec3(view.view[0][2], view.view[1][2], view.view[2][2]);
            auto strafe = glm::vec3(view.view[0][0], view.view[1][0], view.view[2][0]);

            constexpr auto moveSpeed = 2.0f;

            view.camPos += dt * (dz * forward + dx * strafe) * moveSpeed;

            // std::cout << *window.activeView << ":pos:[" << view.camPos.x << ", " <<
            // view.camPos.y << ", " << view.camPos.z << "]" << std::endl;

            doUpdateViewMatrix = true;
        }

        if (myMouseButtonsPressed[0])
        {
            constexpr auto rotSpeed = 10.0f;

            auto dM = myMousePosition[0] - myMousePosition[1];

            view.camRot +=
                dt * glm::vec3(dM.y / view.viewport.height, dM.x / view.viewport.width, 0.0f) *
                rotSpeed;

            // std::cout << *window.activeView << ":rot:[" << view.camRot.x << ", " <<
            // view.camRot.y << ", " << view.camRot.z << "]" << std::endl;

            doUpdateViewMatrix = true;
        }

        if (doUpdateViewMatrix)
        {
            updateViewMatrix(window.views[*window.activeView]);
        }
    }
}

template <>
void Application<GraphicsBackend::Vulkan>::onMouse(const mouse_state& state)
{
    ZoneScoped;

    auto& window = *myDefaultResources->window;

    bool leftPressed = state.button == GLFW_MOUSE_BUTTON_LEFT && state.action == GLFW_PRESS;
    bool rightPressed = state.button == GLFW_MOUSE_BUTTON_RIGHT && state.action == GLFW_PRESS;
    
    auto screenPos = glm::vec2(state.xpos, state.ypos);
    // auto screenPos = ImGui::GetCursorScreenPos();

    if (state.inside_window && !myMouseButtonsPressed[0])
    {
        // todo: generic view index calculation
        size_t viewIdx = screenPos.x / (window.width / NX);
        size_t viewIdy = screenPos.y / (window.height / NY);
        window.activeView = std::min((viewIdy * NX) + viewIdx, window.views.size() - 1);

        // std::cout << *window.activeView << ":[" << screenPos.x << ", " << screenPos.y << "]"
        // 		  << std::endl;
    }
    else if (!leftPressed)
    {
        window.activeView.reset();

        // std::cout << "window.activeView.reset()" << std::endl;
    }

    myMousePosition[0] =
        glm::vec2{static_cast<float>(screenPos.x), static_cast<float>(screenPos.y)};
    myMousePosition[1] =
        leftPressed && !myMouseButtonsPressed[0] ? myMousePosition[0] : myMousePosition[1];

    myMouseButtonsPressed[0] = leftPressed;
    myMouseButtonsPressed[1] = rightPressed;
}

template <>
void Application<GraphicsBackend::Vulkan>::onKeyboard(const keyboard_state& state)
{
    ZoneScoped;

    if (state.action == GLFW_PRESS)
        myKeysPressed[state.key] = true;
    else if (state.action == GLFW_RELEASE)
        myKeysPressed[state.key] = false;
}

template <>
SurfaceHandle<GraphicsBackend::Vulkan> Application<GraphicsBackend::Vulkan>::createSurface(InstanceHandle<GraphicsBackend::Vulkan> instance, void* view) const
{
    ZoneScoped;

    SurfaceHandle<GraphicsBackend::Vulkan> surface;
    CHECK_VK(glfwCreateWindowSurface(
        instance, reinterpret_cast<GLFWwindow*>(view), nullptr, &surface));

    return surface;
}
