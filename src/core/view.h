#pragma once

#include "glm.h"

#include <cassert>
#include <cstdint>

enum class ViewType : uint8_t
{
	Perspective
};

struct ViewportCreateDesc
{
	uint16_t x = 0;
	uint16_t y = 0;
	uint16_t width = 0;
	uint16_t height = 0;
	float minDepth = 0.0f;
	float maxDepth = 1.0f;
};

struct ViewCreateDesc
{
	ViewType type = ViewType::Perspective;
	ViewportCreateDesc viewport{};
	glm::vec3 cameraPosition = glm::vec3(0.0f, -2.0f, 0.0f);
	glm::vec3 cameraRotation = glm::vec3(0.0f, 0.0f, 0.0);
};

class View
{
public:
	constexpr View() noexcept = default;
	View(ViewCreateDesc&& data)
		: myDesc(std::forward<ViewCreateDesc>(data))
	{
		updateAll();
	}

	const auto& getViewMatrix() const noexcept { return myViewMatrix; }
	const auto& getProjectionMatrix() const noexcept { return myProjectionMatrix; }

	auto& desc() { return myDesc; }

	void updateViewMatrix();
	void updateProjectionMatrix();

	void updateAll()
	{
		updateViewMatrix();
		updateProjectionMatrix();
	}

private:
	ViewCreateDesc myDesc{};
	glm::mat4x3 myViewMatrix = glm::mat4x3(1.0f);
	glm::mat4 myProjectionMatrix = glm::mat4(1.0f);
};
