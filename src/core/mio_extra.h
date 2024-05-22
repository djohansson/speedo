#pragma once

#include <mio/mmap.hpp>

//NOLINTBEGIN(readability-identifier-naming)

namespace mio_extra
{

class ResizeableMemoryMapSink : public mio::mmap_sink
{
public:
	constexpr ResizeableMemoryMapSink() noexcept = default;
	explicit ResizeableMemoryMapSink(
		const std::string& path,
		const size_type offset = 0,
		const size_type length = mio::map_entire_file)
		: mio::mmap_sink(path, offset, length)
	{}
	ResizeableMemoryMapSink(const ResizeableMemoryMapSink&) = delete;
	ResizeableMemoryMapSink(ResizeableMemoryMapSink&&) noexcept = default;

	ResizeableMemoryMapSink& operator=(const ResizeableMemoryMapSink&) = delete;
	ResizeableMemoryMapSink& operator=(ResizeableMemoryMapSink&&) noexcept = default;

	void resize(size_t size)
	{
		myHighWaterMark = std::max(myHighWaterMark, size);

		std::error_code error;
		mio::mmap_sink::remap(0, size, error);
		if (error)
			throw std::system_error(error);
	}

	[[nodiscard]] size_t HighWaterMark() const noexcept { return myHighWaterMark; }

private:
	size_t myHighWaterMark = 0;
};

} // namespace mio_extra

//NOLINTEND(readability-identifier-naming)
