#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace gfx
{

// the scene's unit: what a file's coordinates are in (View > Units, SPEEDO_UNITS; Scene::Settings::metersPerUnit holds
// it as meters per unit, which a custom value sets too, SPEEDO_METERS_PER_UNIT). the lights' inverse square falloff, the
// ground plane's checker (squares of a meter) and the air go by it; what is relative to the model (the framing, the move
// speed, the clipmap, the cascades, the contacts) doesn't. a file's is taken as GuessUnit says until told, and told, it
// is remembered for the file (the user profile's units.txt, see client's RememberUnit)
enum class SceneUnit : uint8_t
{
	kMeters,
	kCentimeters,
	kMillimeters,
	kInches,
	kFeet,
};

inline constexpr std::array kSceneUnits{SceneUnit::kMeters, SceneUnit::kCentimeters, SceneUnit::kMillimeters, SceneUnit::kInches, SceneUnit::kFeet};

[[nodiscard]] constexpr float MetersPerUnit(SceneUnit unit) noexcept
{
	switch (unit)
	{
	case SceneUnit::kCentimeters: return 0.01F;
	case SceneUnit::kMillimeters: return 0.001F;
	case SceneUnit::kInches: return 0.0254F;
	case SceneUnit::kFeet: return 0.3048F;
	case SceneUnit::kMeters:
	default: return 1.0F;
	}
}

[[nodiscard]] constexpr std::string_view UnitName(SceneUnit unit) noexcept
{
	switch (unit)
	{
	case SceneUnit::kCentimeters: return "Centimeters";
	case SceneUnit::kMillimeters: return "Millimeters";
	case SceneUnit::kInches: return "Inches";
	case SceneUnit::kFeet: return "Feet";
	case SceneUnit::kMeters:
	default: return "Meters";
	}
}

// the unit's symbol: what SPEEDO_UNITS takes and Copy Pose writes
[[nodiscard]] constexpr std::string_view UnitSymbol(SceneUnit unit) noexcept
{
	switch (unit)
	{
	case SceneUnit::kCentimeters: return "cm";
	case SceneUnit::kMillimeters: return "mm";
	case SceneUnit::kInches: return "in";
	case SceneUnit::kFeet: return "ft";
	case SceneUnit::kMeters:
	default: return "m";
	}
}

// the unit a symbol or a name says (either case), none for anything else
[[nodiscard]] constexpr std::optional<SceneUnit> ParseUnit(std::string_view text) noexcept
{
	constexpr auto same = [](std::string_view a, std::string_view b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t charIt = 0; charIt < a.size(); charIt++)
		{
			auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
			if (lower(a[charIt]) != lower(b[charIt]))
				return false;
		}
		return true;
	};
	for (auto unit : kSceneUnits)
		if (same(text, UnitSymbol(unit)) || same(text, UnitName(unit)))
			return unit;
	if (same(text, "meter") || same(text, "metres") || same(text, "metre"))
		return SceneUnit::kMeters;
	if (same(text, "centimeter") || same(text, "centimetres") || same(text, "centimetre"))
		return SceneUnit::kCentimeters;
	if (same(text, "millimeter") || same(text, "millimetres") || same(text, "millimetre"))
		return SceneUnit::kMillimeters;
	if (same(text, "inch"))
		return SceneUnit::kInches;
	if (same(text, "foot"))
		return SceneUnit::kFeet;
	return std::nullopt;
}

// the unit of a meters per unit value, none where it is no unit's (a custom value)
[[nodiscard]] inline std::optional<SceneUnit> UnitOf(float metersPerUnit) noexcept
{
	for (auto unit : kSceneUnits)
		if (std::abs(metersPerUnit - MetersPerUnit(unit)) <= 1e-5F * MetersPerUnit(unit))
			return unit;
	return std::nullopt;
}

// the unit a file is taken as until told: a gltf's meters, its spec's; an obj's, which has none, by its size, centimeters
// over kCentimeterExtent units across (the centimeter scenes of the McGuire archive are 594 to 10860 across: bmw, white
// oak, holodeck, conference, Sponza, Bistro; the meter ones under 170: San Miguel, Sibenik, salle de bain, erato, Lost
// Empire, Rungholt's house, the rooms, the small models), else meters. a set of files scaled to one size has no unit:
// meters
inline constexpr float kCentimeterExtent = 500.0F;

[[nodiscard]] constexpr SceneUnit GuessUnit(bool gltf, bool set, float longestExtent) noexcept
{
	return !gltf && !set && longestExtent > kCentimeterExtent ? SceneUnit::kCentimeters : SceneUnit::kMeters;
}

} // namespace gfx
