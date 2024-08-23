#pragma once

#include <core/std_extra.h>
#include <gfx/glm_extra.h>

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
	ViewType type = ViewType::Perspective;
};

struct CameraCreateDesc
{
	glm::vec3 position = glm::vec3(0.0f, -2.0f, 0.0f);
	glm::vec3 cameraRotation = glm::vec3(0.0f, 0.0f, 0.0);
	ViewportCreateDesc viewport{};
};

enum FrustumPlane
{
	Left, // -x
	Right, // +x
	Bottom, // -y
	Top, // +y
	Back, // -z
	Front // +z
};

class Camera
{
public:
	constexpr Camera() noexcept = default;
	Camera(CameraCreateDesc&& data)
		: myDesc(std::forward<CameraCreateDesc>(data))
	{
		UpdateAll();
	}

	[[nodiscard]] const auto& GetViewMatrix() const noexcept { return myViewMatrix; }
	[[nodiscard]] const auto& GetProjectionMatrix() const noexcept { return myProjectionMatrix; }

	[[nodiscard]] auto& GetDesc() noexcept { return myDesc; }
	[[nodiscard]] const auto& GetDesc() const noexcept { return myDesc; }

	template <FrustumPlane Plane>
	[[nodiscard]] glm::vec4 GetFrustumPlane() const noexcept;

	void UpdateViewMatrix();
	void UpdateProjectionMatrix();

	void UpdateAll()
	{
		UpdateViewMatrix();
		UpdateProjectionMatrix();
	}

private:
	glm::mat4 myViewMatrix = glm::mat4(1.0f); // could be 4x3 but glm does not optimize that well
	glm::mat4 myProjectionMatrix = glm::mat4(1.0f);
	CameraCreateDesc myDesc{};
};

#include "camera.inl"