#pragma once

#include <mio/mmap.hpp>

namespace mio_extra
{

struct resizeable_mmap_sink : public mio::mmap_sink
{
	resizeable_mmap_sink(const std::string& path, const size_type offset = 0, const size_type length = mio::map_entire_file)
		: mio::mmap_sink(path, offset, length)
	{}

	void resize(size_t size)
	{
		std::error_code error;
		mio::mmap_sink::remap(0, size, error);
		if (error)
			throw std::system_error(std::move(error));
	}
};

} // namespace mio_extra
