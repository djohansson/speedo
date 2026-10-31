#pragma once

#include <gfx/bounds.h>
#include <gfx/camera.h>
#include <gfx/gpu.h>
#include <gfx/scenecamera.h>

#include <core/concurrentaccess.h>
#include <core/inputstate.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <span>
#include <vector>

#include <glm/glm.hpp>

namespace gfx
{

// a viewport of the output's in the render target, which is scale of its size each way (see FrameInputs::upscaling): its
// edges at the scaled ones, rounded down, so that a grid's cells stay side by side
[[nodiscard]] ViewportCreateDesc ScaleViewport(const ViewportCreateDesc& viewport, glm::vec2 scale);

// the views of a window, split screen in a grid: a camera each, controlled with the mouse and keyboard over its view,
// and the buffers (one per frame) the shaders read the views' view projections from
class Views final
{
public:
	Views(Device& device, glm::uvec2 framebufferExtent);
	Views(const Views&) = delete;
	Views(Views&&) noexcept = delete;
	~Views();

	Views& operator=(const Views&) = delete;
	Views& operator=(Views&&) noexcept = delete;

	// the grid's width and height in views. read on the draw thread, which is the only one that changes it.
	[[nodiscard]] glm::uvec2 GetGrid() const noexcept { return myGrid; }
	[[nodiscard]] const Buffer& GetBuffer(size_t frameIndex) const noexcept { return myBuffers[frameIndex]; }

	// lays the views out over the framebuffer again, e.g. after a resize. call on the draw thread.
	void OnResizeFramebuffer(glm::uvec2 framebufferExtent);
	// changes the grid. call on the draw thread.
	void OnResizeGrid(glm::uvec2 grid);
	// moves the active view's camera (the one under the mouse, or being dragged) by the input, and scales the movement
	// speed with the scroll wheel (the next frame uploads it, see Upload).
	void OnInputStateChanged(const core::InputState& input);

	// moves every camera back from bounds (looking down -z) until all of it is in view, fits the near and far planes to
	// its size, and sets the movement speed after it (see GetMoveSpeed). call on the draw thread.
	void FrameBounds(const Bounds3f& bounds);
	// places every view's camera at eye (world space), turned by rotation (pitch, yaw and roll, in radians: see
	// CameraCreateDesc::cameraRotation), keeping its projection (e.g. SPEEDO_CAMERA, for measurements and screenshots).
	// any thread
	void Place(const glm::vec3& eye, const glm::vec3& rotation);
	// and one view's (SPEEDO_CAMERA_VIEWS: a camera of its own each, as split screen has them), if there is that view
	void Place(size_t view, const glm::vec3& eye, const glm::vec3& rotation);
	// turns every view about the vertical axis (radians of yaw), and moves it forward (along where it looks), where it is
	void Turn(float yaw, float forward = 0.0F);
	// moves every view around the scene's bounds' center (radians of yaw about the vertical, and of pitch, positive
	// looking down from above, at most a little short of straight down or up), at its distance, looking at the center
	// (e.g. dragging the file browser's preview). any thread
	void Orbit(float yaw, float pitch);

	// the installed model's bounds and cameras (see ModelDesc::cameras): sets the views to its first camera, or frames
	// the bounds if it has none. call on the draw thread.
	void SetScene(const Bounds3f& bounds, std::vector<SceneCamera> cameras);
	// sets every view to one of the scene's cameras (its projection too: field of view, orthographic, near and far
	// planes; not its roll, which the views don't have), or frames the scene's bounds (nullopt). call on the draw thread.
	void UseSceneCamera(std::optional<size_t> camera);
	// the scene's cameras as an animation moves them (see Model::GetCameras): the views follow the one they look through,
	// if it is animated (SceneCamera::animated). any thread.
	void UpdateSceneCameras(std::span<const SceneCamera> cameras);
	// the scene's camera names, and which one the views were last set to (nullopt: framed). any thread.
	[[nodiscard]] std::vector<std::string> GetSceneCameraNames() const;
	[[nodiscard]] std::optional<size_t> GetSceneCamera() const;

	// how fast the cameras move (with w, a, s, d), in units per second: a quarter of the framed bounds' radius, so
	// crossing them takes the same time whatever the scale of the model. any thread.
	[[nodiscard]] float GetMoveSpeed() const noexcept { return myMoveSpeed.load(std::memory_order_relaxed); }
	void SetMoveSpeed(float speed) noexcept;

	// writes the views (ViewData: view projections, eyes, viewports) to frameIndex's buffer from the cameras as they are
	// now, and returns those cameras: what the frame draws with, so what it culls by and fits its shadows and clusters
	// to. the other frame indices' buffers may still be in use. call on the draw thread, recording that frame (after its
	// fence). with temporal upscaling (see FrameInputs::upscaling) the frame is rendered at viewportScale of the output's
	// size each way, and with jitter its projections are offset by a point of a sequence a frame, within a pixel (the
	// cameras returned aren't: what is culled and fitted by doesn't need it), and each view's image scaled by its exposure
	// before tonemapping (ViewData::exposure; past those given, the first's, 1 without any)
	[[nodiscard]] std::vector<Camera> Upload(uint32_t frameIndex, glm::vec2 viewportScale = glm::vec2(1.0F), bool jitter = false, std::span<const float> exposures = {});

	// a path of poses (eye and rotation, as Place takes them): Upload places the views at the next before each frame it
	// writes, so one pose a frame, and keeps them at the last (SPEEDO_CAMERA_PATH: an exact, repeatable move, whatever the
	// frame times and in whatever order the draw calls run). any thread.
	void FollowPath(std::vector<std::pair<glm::vec3, glm::vec3>> poses);
	// whether poses of the path are left to place. any thread.
	[[nodiscard]] bool OnPath() const;
	// the pose of the path the last Upload placed, if it placed one of the path's for the first time (not the last kept):
	// which pose the frame being recorded draws (SPEEDO_CAPTURE_PATH). call on the draw thread, after Upload.
	[[nodiscard]] std::optional<size_t> GetPlacedPose() const;

	// where each view draws in the framebuffer: its grid cell, letterboxed to its camera's aspect ratio. any thread.
	[[nodiscard]] std::vector<ViewportCreateDesc> GetViewports() const;

	// a view's camera (a copy), if there is that view. any thread.
	[[nodiscard]] std::optional<Camera> GetCamera(size_t view) const;

private:
	void InternalLayout();

	std::vector<Buffer> myBuffers;
	core::ConcurrentAccess<std::vector<gfx::Camera>> myCameras;
	// what the last Upload wrote per view, which the next writes as its previous frame (ViewData::previousViewProjection).
	// the draw thread's
	struct Uploaded
	{
		glm::mat4 viewProjection;
		glm::mat4 unjittered; // the view projection without the jitter, by which the next tells whether the view moved
		glm::vec4 eye;
		glm::vec4 viewport;
		glm::vec2 jitter;
	};
	std::vector<Uploaded> myUploaded;
	uint32_t myJitterFrame = 0; // the frames jittered (Upload's), the sequence's place
	glm::uvec2 myFramebufferExtent{};
	glm::uvec2 myGrid{1, 1};
	std::optional<size_t> myActiveCamera;
	struct Scene
	{
		Bounds3f bounds;
		std::vector<SceneCamera> cameras;
		std::optional<size_t> current;
	};
	core::ConcurrentAccess<Scene> myScene;
	// a scene camera's pose and projection, for all views (see UseSceneCamera)
	void InternalApplySceneCamera(const SceneCamera& camera, const Bounds3f& bounds);
	static constexpr float kDefaultMoveSpeed = 5.0F; // until a model is framed

	std::atomic<float> myMoveSpeed{kDefaultMoveSpeed};
	// see FollowPath: the poses, and the next to place
	struct Path
	{
		std::vector<std::pair<glm::vec3, glm::vec3>> poses;
		size_t next = 0;
		std::optional<size_t> placed; // see GetPlacedPose
	};
	core::ConcurrentAccess<Path> myPath;
};

} // namespace gfx
