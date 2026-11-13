#include <gfx/camera.h>

#include <core/assert.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE
{

void Camera::UpdateViewMatrix()
{
	auto rotx = glm::rotate(glm::mat4(1.0), myDesc.cameraRotation.x, glm::vec3(-1, 0, 0));
	auto roty = glm::rotate(glm::mat4(1.0), myDesc.cameraRotation.y, glm::vec3(0, -1, 0));
	auto rotz = glm::rotate(glm::mat4(1.0), myDesc.cameraRotation.z, glm::vec3(0, 0, 1));
	auto trans = glm::translate(glm::mat4(1.0), -myDesc.position);

	myViewMatrix = glm::inverse(trans * roty * rotx * rotz);
}

void Camera::UpdateProjectionMatrix()
{
	switch (myDesc.viewport.type)
	{
	case ViewType::Perspective:
	case ViewType::Orthographic: {
		// gl's clip space to vulkan's: y down, and depth reversed, 1 at the near plane to 0 at the far (float depth keeps
		// its precision with the distance then: standard, 0 near, two surfaces a centimetre apart fought 50 m away)
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
			-0.5F,
			0.0F,
			0.0F,
			0.0F,
			0.5F,
			1.0F};
		auto aspect =
			static_cast<float>(myDesc.viewport.width) / static_cast<float>(myDesc.viewport.height);
		if (myDesc.viewport.type == ViewType::Perspective && myDesc.farPlane <= 0.0F)
		{
			// no far plane: glm::perspective's as the far plane goes to infinity (depth 0 there, which nothing reaches)
			float tanHalf = std::tan(0.5F * myDesc.fovY);
			glm::mat4 projection(0.0F);
			projection[0][0] = 1.0F / (aspect * tanHalf);
			projection[1][1] = 1.0F / tanHalf;
			projection[2][2] = -1.0F;
			projection[2][3] = -1.0F;
			projection[3][2] = -2.0F * myDesc.nearPlane;
			myProjectionMatrix = kClip * projection;
		}
		else if (myDesc.viewport.type == ViewType::Perspective)
		{
			myProjectionMatrix = kClip * glm::perspective(myDesc.fovY, aspect, myDesc.nearPlane, myDesc.farPlane);
		}
		else
		{
			auto halfHeight = myDesc.orthoHalfHeight;
			auto halfWidth = halfHeight * aspect;
			myProjectionMatrix =
				kClip * glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, myDesc.nearPlane, myDesc.farPlane);
		}
	}
	break;
	default:
		ASSERT(false);
		break;
	}
}

} // namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE
