#include "camera.h"

#include <core/assert.h>


void Camera::UpdateViewMatrix()
{
	auto rotx = glm::rotate(glm::mat4(1.0), myDesc.cameraRotation.x, glm::vec3(-1, 0, 0));
	auto roty = glm::rotate(glm::mat4(1.0), myDesc.cameraRotation.y, glm::vec3(0, -1, 0));
	auto trans = glm::translate(glm::mat4(1.0), -myDesc.position);

	myViewMatrix = glm::inverse(trans * roty * rotx);
}

void Camera::UpdateProjectionMatrix()
{
	switch (myDesc.viewport.type)
	{
	case ViewType::Perspective: {
		static const glm::mat4 kClip{
			1.0F,
			0.0F,
			0.0F,
			0.0F,
			0.0F,
			-1.0F,
			0.0F,
			0.0F,
			0.0F,
			0.0F,
			0.5F,
			0.0F,
			0.0F,
			0.0F,
			0.5F,
			1.0F};
		constexpr auto kFov = 75.0F;
		auto aspect =
			static_cast<float>(myDesc.viewport.width) / static_cast<float>(myDesc.viewport.height);
		constexpr auto kNearplane = 0.01F;
		constexpr auto kFarplane = 100.0F;
		myProjectionMatrix =
			kClip * glm::perspective(glm::radians(kFov), aspect, kNearplane, kFarplane);
	}
	break;
	default:
		ASSERT(false);
		break;
	}
}
