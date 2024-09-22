#pragma once

#include <algorithm>
#include <expected>
#include <string>
#include <system_error>

#include <mio/mmap.hpp>

//NOLINTBEGIN(readability-identifier-naming)

namespace mio_extra
{

class ResizeableMemoryMapSink : public mio::mmap_sink
{
public:
	constexpr ResizeableMemoryMapSink() noexcept = default;
#ifdef __cpp_exceptions
	explicit ResizeableMemoryMapSink(
		const std::string& path,
		const size_type offset = 0,
		const size_type length = mio::map_entire_file)
		: mio::mmap_sink(path, offset, length)
	{}
#endif
	ResizeableMemoryMapSink(const ResizeableMemoryMapSink&) = delete;
	ResizeableMemoryMapSink(ResizeableMemoryMapSink&&) noexcept = default;

	ResizeableMemoryMapSink& operator=(const ResizeableMemoryMapSink&) = delete;
	[[maybe_unused]] ResizeableMemoryMapSink& operator=(ResizeableMemoryMapSink&&) noexcept = default;

	[[maybe_unused]] std::expected<void, std::error_code> resize(size_t size)//NOLINT(readability-identifier-naming)
	{
		mySize = size;
		
		std::error_code error;
		mio::mmap_sink::remap(0, size, error);
		
		if (error)
			return std::unexpected(error);

		return {};
	}

	[[nodiscard]] size_t Size() const noexcept { return mySize; }

private:
	size_t mySize = 0;
};

} // namespace mio_extra

//NOLINTEND(readability-identifier-naming)
