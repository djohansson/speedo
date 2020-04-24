#pragma once

#include "glm.h"

#include <cassert>
#include <cstdint>

enum class ViewType : uint8_t { Perspective };

struct ViewportDesc
{
	uint16_t x = 0;
	uint16_t y = 0;
	uint16_t width = 0;
	uint16_t height = 0;
	float minDepth = 0.0f;
	float maxDepth = 1.0f;
};

struct ViewDesc
{
	ViewType type = ViewType::Perspective;
	ViewportDesc viewport = {};
	glm::vec3 cameraPosition = glm::vec3(0.0f, -2.0f, 0.0f);
	glm::vec3 cameraRotation = glm::vec3(0.0f, 0.0f, 0.0);
};

class View
{
public:

	View() = default;
	View(ViewDesc&& data)
		: myViewDesc(std::move(data))
	{
		updateAll();
	}

	const auto& getViewMatrix() const { return myViewMatrix; }
	const auto& getProjectionMatrix() const { return myProjectionMatrix; }

	auto& viewDesc() { return myViewDesc; }

	void updateViewMatrix();
	void updateProjectionMatrix();

	void updateAll()
	{
		updateViewMatrix();
		updateProjectionMatrix();
	}

private:

    ViewDesc myViewDesc = {};
	glm::mat4x3 myViewMatrix = glm::mat4x3(1.0f);
	glm::mat4 myProjectionMatrix = glm::mat4(1.0f);
};
