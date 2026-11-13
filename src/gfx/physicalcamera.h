#pragma once

#include <gfx/shaders/capi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

// the physical camera (View > Camera): a sensor and a focal length, which give the field of view, and an aperture, a
// shutter and an ISO, which give the exposure (see Scene::Settings). the renderer's units: a unit of radiance is
// kUnitLuminance cd/m^2 (the sky's sun gives SHADER_TYPES_ATMOSPHERE_SUN_ILLUMINANCE units for 128000 lux, and a white
// Lambertian surface under a unit of illuminance radiates a unit over pi), and a sensor saturates at a luminance of
// 1.2 * 2^EV100 cd/m^2 (Lagarde and de Rousiers' physically based camera: the standard output sensitivity, its lens's
// transmittance and vignetting folded into the 1.2), so the exposure that shows a unit as white, the renderer's 0 stops,
// is EV100 kSaturationEv100, sunny sixteen's: f/16 at 1/160 s and ISO 100
namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE::camera
{

inline constexpr float kUnitLuminance = 128000.0F / static_cast<float>(SHADER_TYPES_ATMOSPHERE_SUN_ILLUMINANCE); // cd/m^2
inline const float kSaturationEv100 = std::log2(kUnitLuminance / 1.2F); // 15.32

// the exposure value at ISO 100 of an aperture (an f-number), a shutter (seconds) and an ISO
[[nodiscard]] inline float Ev100(float aperture, float shutter, float iso) noexcept
{
	return std::log2(aperture * aperture / shutter) - std::log2(iso / 100.0F);
}

// the exposure in stops (what the image is scaled by, 2^stops, before tonemapping: Scene::Settings::exposureStops is
// added) the settings give
[[nodiscard]] inline float ExposureStops(float aperture, float shutter, float iso) noexcept
{
	return kSaturationEv100 - Ev100(aperture, shutter, iso);
}

// the setting that gives stops with the other two
[[nodiscard]] inline float IsoFor(float stops, float aperture, float shutter) noexcept
{
	return 100.0F * std::exp2(std::log2(aperture * aperture / shutter) - (kSaturationEv100 - stops));
}

[[nodiscard]] inline float ShutterFor(float stops, float aperture, float iso) noexcept
{
	return aperture * aperture / (std::exp2(kSaturationEv100 - stops) * iso / 100.0F);
}

[[nodiscard]] inline float ApertureFor(float stops, float shutter, float iso) noexcept
{
	return std::sqrt(shutter * std::exp2(kSaturationEv100 - stops) * iso / 100.0F);
}

// how the camera exposes (View > Camera > Exposure, SPEEDO_EXPOSURE_MODE): what the user sets, and what the camera
// chooses to reach the exposure auto exposure asks for (see Choose). program: all three, along a line from sunny
// sixteen; aperture or shutter priority: the one set, the other along the line, then the ISO; manual: all three set
// (SPEEDO_AUTO_EXPOSURE=0, as before the camera)
enum class ExposureMode : uint8_t
{
	kProgram,
	kAperturePriority,
	kShutterPriority,
	kManual,
};

[[nodiscard]] constexpr std::string_view ExposureModeName(ExposureMode mode) noexcept
{
	switch (mode)
	{
	case ExposureMode::kAperturePriority: return "aperture";
	case ExposureMode::kShutterPriority: return "shutter";
	case ExposureMode::kManual: return "manual";
	case ExposureMode::kProgram:
	default: return "program";
	}
}

[[nodiscard]] constexpr std::optional<ExposureMode> ParseExposureMode(std::string_view text) noexcept
{
	for (auto mode : {ExposureMode::kProgram, ExposureMode::kAperturePriority, ExposureMode::kShutterPriority, ExposureMode::kManual})
		if (text == ExposureModeName(mode))
			return mode;
	if (text == "auto")
		return ExposureMode::kProgram;
	return std::nullopt;
}

// what the camera exposes with
struct Exposure
{
	float aperture = 16.0F; // an f-number
	float shutter = 1.0F / 160.0F; // seconds
	float iso = 100.0F;

	[[nodiscard]] float Ev100() const noexcept { return camera::Ev100(aperture, shutter, iso); }
	[[nodiscard]] float Stops() const noexcept { return ExposureStops(aperture, shutter, iso); }
};

// what the camera can expose with: the aperture's range is the lens's (a pinhole's any, see Lens), the shutter's and
// the ISO's a camera's, and the slowest shutter the program chooses one held by hand (slower, it raises the ISO)
struct ExposureLimits
{
	float widestAperture = 0.7F;
	float narrowestAperture = 64.0F;
	float fastestShutter = 1.0F / 8000.0F;
	float slowestShutter = 30.0F;
	float handheldShutter = 1.0F / 30.0F;
	float lowestIso = 50.0F;
	float highestIso = 409600.0F;
};

// the exposure the camera chooses in a mode for the stops wanted (auto exposure's, see Scene's UpdateAutoExposure) from
// what is set and within the limits: the aperture and the shutter along the program line through sunny sixteen, a stop
// of the difference to the shutter for every stop to the aperture (narrower and faster where the scene is brighter,
// wider and slower where it is darker), each as far as its limit, and the ISO for the rest, as far as its own; beyond
// that the camera can't: the image is under or over exposed, as a real one's. manual exposes with what is set
[[nodiscard]] inline Exposure Choose(ExposureMode mode, float stops, const Exposure& set, const ExposureLimits& limits) noexcept
{
	if (mode == ExposureMode::kManual)
	{
		// what is set, within the lens's aperture range (a lens chosen after the aperture was set may not open that wide)
		Exposure manual = set;
		manual.aperture = std::clamp(manual.aperture, limits.widestAperture, limits.narrowestAperture);
		return manual;
	}
	Exposure chosen = set;
	const Exposure pivot{}; // sunny sixteen
	float below = pivot.Stops() - stops; // stops the scene is brighter than the pivot's exposure (negative: darker)
	if (mode != ExposureMode::kAperturePriority)
		chosen.aperture = pivot.aperture * std::exp2(0.25F * below);
	if (mode != ExposureMode::kShutterPriority)
		chosen.shutter = pivot.shutter * std::exp2(-0.5F * below);
	chosen.aperture = std::clamp(chosen.aperture, limits.widestAperture, limits.narrowestAperture);
	chosen.shutter = std::clamp(chosen.shutter, limits.fastestShutter, mode == ExposureMode::kShutterPriority ? limits.slowestShutter : limits.handheldShutter);
	// where the aperture or the shutter couldn't go (priority: half of the way was never theirs), the other takes it
	float left = stops - ExposureStops(chosen.aperture, chosen.shutter, 100.0F);
	if (mode != ExposureMode::kAperturePriority && left != 0.0F)
		chosen.aperture = std::clamp(ApertureFor(stops, chosen.shutter, 100.0F), limits.widestAperture, limits.narrowestAperture);
	left = stops - ExposureStops(chosen.aperture, chosen.shutter, 100.0F);
	if (mode != ExposureMode::kShutterPriority && left != 0.0F)
		chosen.shutter = std::clamp(ShutterFor(stops, chosen.aperture, 100.0F), limits.fastestShutter, limits.handheldShutter);
	chosen.iso = std::clamp(IsoFor(stops, chosen.aperture, chosen.shutter), limits.lowestIso, limits.highestIso);
	return chosen;
}

// the sensor (View > Camera > Sensor, SPEEDO_SENSOR): its size, mm, which with the focal length gives the field of view
enum class Sensor : uint8_t
{
	kFullFrame,
	kApsC,
	kSuper35,
	kOneInch,
	kPhone,
};

inline constexpr std::array kSensors{Sensor::kFullFrame, Sensor::kApsC, Sensor::kSuper35, Sensor::kOneInch, Sensor::kPhone};

struct SensorSize
{
	float width;
	float height;
};

[[nodiscard]] constexpr SensorSize SensorSizeOf(Sensor sensor) noexcept
{
	switch (sensor)
	{
	case Sensor::kApsC: return {23.5F, 15.6F};
	case Sensor::kSuper35: return {24.89F, 18.66F};
	case Sensor::kOneInch: return {13.2F, 8.8F};
	case Sensor::kPhone: return {6.17F, 4.55F}; // 1/2.3 inch
	case Sensor::kFullFrame:
	default: return {36.0F, 24.0F};
	}
}

[[nodiscard]] constexpr std::string_view SensorName(Sensor sensor) noexcept
{
	switch (sensor)
	{
	case Sensor::kApsC: return "APS-C";
	case Sensor::kSuper35: return "Super 35";
	case Sensor::kOneInch: return "1 inch";
	case Sensor::kPhone: return "Phone (1/2.3 inch)";
	case Sensor::kFullFrame:
	default: return "Full frame";
	}
}

// the sensor's name as SPEEDO_SENSOR takes it and Copy Pose writes it
[[nodiscard]] constexpr std::string_view SensorKey(Sensor sensor) noexcept
{
	switch (sensor)
	{
	case Sensor::kApsC: return "aps-c";
	case Sensor::kSuper35: return "super35";
	case Sensor::kOneInch: return "1-inch";
	case Sensor::kPhone: return "phone";
	case Sensor::kFullFrame:
	default: return "full-frame";
	}
}

[[nodiscard]] constexpr std::optional<Sensor> ParseSensor(std::string_view text) noexcept
{
	for (auto sensor : kSensors)
		if (text == SensorKey(sensor))
			return sensor;
	return std::nullopt;
}

// the vertical field of view (radians) of a focal length (mm) on a sensor height (mm), and back
[[nodiscard]] inline float FieldOfView(float sensorHeight, float focalLength) noexcept
{
	return 2.0F * std::atan(sensorHeight / (2.0F * std::max(focalLength, 1e-3F)));
}

[[nodiscard]] inline float FocalLength(float sensorHeight, float fieldOfView) noexcept
{
	return sensorHeight / (2.0F * std::tan(std::clamp(0.5F * fieldOfView, 1e-3F, 1.5F)));
}

} // namespace gfx::inline SPEEDO_GRAPHICS_API_NAMESPACE::camera
