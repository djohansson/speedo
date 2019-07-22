#pragma once

class View
{
public:

	struct BufferData // todo: needs to be aligned to VkPhysicalDeviceLimits.minUniformBufferOffsetAlignment. right now uses manual padding.
	{
		glm::mat4 viewProj = glm::mat4(1.0f);
        glm::mat4 pad0;
        glm::mat4 pad1;
        glm::mat4 pad2;
	};

	struct ViewportData
	{
		uint32_t width = 0;
		uint32_t height = 0;
	};

    ViewportData viewport = {};

	glm::vec3 camPos = glm::vec3(0.0f, -2.0f, 0.0f);
	glm::vec3 camRot = glm::vec3(0.0f, 0.0f, 0.0);

	glm::mat4x3 view = glm::mat4x3(1.0f);
	glm::mat4 projection = glm::mat4(1.0f);
};
