template <GraphicsBackend B>
void Application<B>::resizeWindow(const window_state& state)
{
    auto& window = *myDefaultResources->window;

    if (state.fullscreen_enabled)
    {
        window.width = state.fullscreen_width;
        window.height = state.fullscreen_height;
    }
    else
    {
        window.width = state.width;
        window.height = state.height;
    }
}

template <GraphicsBackend B>
void Application<B>::onMouse(const mouse_state& state)
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

template <GraphicsBackend B>
void Application<B>::onKeyboard(const keyboard_state& state)
{
    ZoneScoped;

    if (state.action == GLFW_PRESS)
        myKeysPressed[state.key] = true;
    else if (state.action == GLFW_RELEASE)
        myKeysPressed[state.key] = false;
}
