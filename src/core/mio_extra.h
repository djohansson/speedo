#pragma once

#include <mio/mmap.hpp>

namespace mio_extra
{

struct ResizeableMemoryMapSink : public mio::mmap_sink
{
	constexpr ResizeableMemoryMapSink() noexcept = default;
	ResizeableMemoryMapSink(const std::string& path, const size_type offset = 0, const size_type length = mio::map_entire_file)
		: mio::mmap_sink(path, offset, length)
	{}

	void resize(size_t size) // NOLINT(readability-identifier-naming.*)
	{
		myHighWaterMark = std::max(myHighWaterMark, size);

		std::error_code error;
		mio::mmap_sink::remap(0, size, error);
		if (error)
			throw std::system_error(std::move(error));
	}

	size_t HighWaterMark() const noexcept { return myHighWaterMark; }

	size_t myHighWaterMark = 0;
};

} // namespace mio_extra
