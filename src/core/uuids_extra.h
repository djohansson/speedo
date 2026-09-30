#pragma once

#include <array>
#include <cstddef>
#include <ranges>
#include <type_traits>

#include <uuid.h>
#include <zpp_bits.h>

namespace uuids
{

// uuids::uuid keeps its bytes private, so zpp::bits can't reflect it via structured bindings;
// this ADL-found overload serializes it as its raw 16 bytes instead.
constexpr auto serialize(auto& archive, auto& self)//NOLINT(readability-identifier-naming)
	requires std::same_as<std::remove_cvref_t<decltype(self)>, uuid>
{
	std::array<uint8_t, sizeof(uuid)> bytes{};
	if constexpr (std::remove_cvref_t<decltype(archive)>::kind() == zpp::bits::kind::out)
	{
		std::ranges::transform(self.as_bytes(), bytes.begin(), [](std::byte byte) { return std::to_integer<uint8_t>(byte); });
		return archive(bytes);
	}
	else
	{
		auto result = archive(bytes);
		if (!zpp::bits::failure(result))
			self = uuid(bytes);
		return result;
	}
}

} // namespace uuids