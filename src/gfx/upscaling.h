#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace gfx
{

// temporal upscaling (FSR 2's, see upscale.slang): the frame rendered at a share of the output's size each way and
// reconstructed at it over frames, from jittered projections (see Views::Upload). off draws at the output's size,
// without a history; native reconstructs at the output's size (antialiasing alone). View > Upscaling, SPEEDO_UPSCALING
enum class Upscaling : uint8_t
{
	kOff,
	kNative,
	kQuality,
	kBalanced,
	kPerformance,
};
inline constexpr std::array kUpscalings{Upscaling::kOff, Upscaling::kNative, Upscaling::kQuality, Upscaling::kBalanced, Upscaling::kPerformance};
// the default (since 2026-10-29, the user's call once FSR 3.1 was ported)
inline constexpr Upscaling kDefaultUpscaling = Upscaling::kBalanced;

// the share of the output's size each way a frame is rendered at
[[nodiscard]] constexpr float RenderScale(Upscaling upscaling) noexcept
{
	switch (upscaling)
	{
	case Upscaling::kQuality: return 0.77F;
	case Upscaling::kBalanced: return 0.67F;
	case Upscaling::kPerformance: return 0.5F;
	case Upscaling::kOff:
	case Upscaling::kNative: break;
	}
	return 1.0F;
}

// as SPEEDO_UPSCALING and Copy Pose name it
[[nodiscard]] constexpr std::string_view UpscalingName(Upscaling upscaling) noexcept
{
	switch (upscaling)
	{
	case Upscaling::kOff: return "off";
	case Upscaling::kNative: return "native";
	case Upscaling::kQuality: return "quality";
	case Upscaling::kBalanced: return "balanced";
	case Upscaling::kPerformance: return "performance";
	}
	return "off";
}

[[nodiscard]] constexpr std::optional<Upscaling> ParseUpscaling(std::string_view name) noexcept
{
	for (auto upscaling : kUpscalings)
		if (UpscalingName(upscaling) == name)
			return upscaling;
	return std::nullopt;
}

} // namespace gfx
