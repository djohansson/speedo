#include "view.h"

void View::updateViewMatrix()
{
    auto Rx = glm::rotate(glm::mat4(1.0), myData.cameraRotation.x, glm::vec3(-1, 0, 0));
    auto Ry = glm::rotate(glm::mat4(1.0), myData.cameraRotation.y, glm::vec3(0, -1, 0));
    auto T = glm::translate(glm::mat4(1.0), -myData.cameraPosition);

    myViewMatrix = glm::inverse(T * Ry * Rx);
}

void View::updateProjectionMatrix()
{
    switch (myData.type)
    {
    case ViewType::Perspective:
        {
            static const glm::mat4 clip = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f,
                                            0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f,  0.5f, 1.0f};
            constexpr auto fov = 75.0f;
            auto aspect =
                static_cast<float>(myData.viewport.width) / static_cast<float>(myData.viewport.height);
            constexpr auto nearplane = 0.01f;
            constexpr auto farplane = 100.0f;
            myProjectionMatrix = clip * glm::perspective(glm::radians(fov), aspect, nearplane, farplane);
        }
        break;
    default:
        assert(false);
        break;
    }
}
