#pragma once

#include "glm.h"

#include <cassert>
#include <cstdint>

enum class ViewType : uint8_t { Perspective };

struct ViewportData
{
	uint16_t x = 0;
	uint16_t y = 0;
	uint16_t width = 0;
	uint16_t height = 0;
	float minDepth = 0.0f;
	float maxDepth = 1.0f;
};

struct ViewData
{
	ViewType type = ViewType::Perspective;
	ViewportData viewport = {};
	glm::vec3 cameraPosition = glm::vec3(0.0f, -2.0f, 0.0f);
	glm::vec3 cameraRotation = glm::vec3(0.0f, 0.0f, 0.0);
};

struct ViewBufferData // todo: needs to be aligned to VkPhysicalDeviceLimits.minUniformBufferOffsetAlignment. right now uses manual padding.
{
	glm::mat4 viewProj = glm::mat4(1.0f);
	glm::mat4 pad0;
	glm::mat4 pad1;
	glm::mat4 pad2;
};

class View
{
public:

	View() = default;
	View(ViewData&& data)
		: myData(std::move(data))
	{
		updateAll();
	}

	auto& data() { return myData; }
	const auto& getViewMatrix() const { return myViewMatrix; }
	const auto& getProjectionMatrix() const { return myProjectionMatrix; }

	void updateViewMatrix();
	void updateProjectionMatrix();

	void updateAll()
	{
		updateViewMatrix();
		updateProjectionMatrix();
	}

private:

    ViewData myData = {};
	glm::mat4x3 myViewMatrix = glm::mat4x3(1.0f);
	glm::mat4 myProjectionMatrix = glm::mat4(1.0f);
};
