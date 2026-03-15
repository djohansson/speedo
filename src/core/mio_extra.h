#pragma once

#include <algorithm>
#include <expected>
#include <string>
#include <system_error>

#include <mio/mmap.hpp>

//NOLINTBEGIN(readability-identifier-naming)

namespace mio_extra
{

template <typename ByteT = std::byte>
class resizeable_mmap_sink : public mio::basic_mmap_sink<ByteT>
{
public:
	constexpr resizeable_mmap_sink() noexcept = default;
#ifdef __cpp_exceptions
	explicit resizeable_mmap_sink(
		const std::string& path,
		const typename mio::basic_mmap_sink<ByteT>::size_type offset = 0,
		const typename mio::basic_mmap_sink<ByteT>::size_type length = mio::map_entire_file)
		: mio::basic_mmap_sink<ByteT>(path, offset, length)
	{}
#endif
	resizeable_mmap_sink(const resizeable_mmap_sink&) = delete;
	resizeable_mmap_sink(resizeable_mmap_sink&&) noexcept = default;

	resizeable_mmap_sink& operator=(const resizeable_mmap_sink&) = delete;
	[[maybe_unused]] resizeable_mmap_sink& operator=(resizeable_mmap_sink&&) noexcept = default;

	[[maybe_unused]] std::expected<void, std::error_code> resize(size_t size, size_t offset = 0)//NOLINT(readability-identifier-naming)
	{
		mySize = size;
		
		std::error_code error;
		mio::basic_mmap_sink<ByteT>::remap(offset, size, error);
		
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
