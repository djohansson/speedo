#pragma once

#include <algorithm>
#include <cstdlib> // mio's mmap.ipp calls alloca without including it
#include <expected>
#include <string>
#include <system_error>

// mio includes <windows.h>, which core/file.h passes on to everything reading files: only its kernel part, which mio
// uses, since winuser.h's macros rename our functions (LoadImage to LoadImageW: gfx::Scene's then link only where it
// wasn't included) and mmsystem.h's typedefs are rewritten by gfx/shaders/capi.h's macros
#if defined(_WIN32) && !defined(_WINDOWS_)
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#		define SPEEDO_MIO_LEAN_AND_MEAN
#	endif
#	ifndef NOUSER
#		define NOUSER
#		define SPEEDO_MIO_NOUSER
#	endif
#	ifndef NOGDI
#		define NOGDI
#		define SPEEDO_MIO_NOGDI
#	endif
#	include <mio/mmap.hpp>
#	ifdef SPEEDO_MIO_LEAN_AND_MEAN
#		undef WIN32_LEAN_AND_MEAN
#		undef SPEEDO_MIO_LEAN_AND_MEAN
#	endif
#	ifdef SPEEDO_MIO_NOUSER
#		undef NOUSER
#		undef SPEEDO_MIO_NOUSER
#	endif
#	ifdef SPEEDO_MIO_NOGDI
#		undef NOGDI
#		undef SPEEDO_MIO_NOGDI
#	endif
#else
#	include <mio/mmap.hpp>
#endif

//NOLINTBEGIN(readability-identifier-naming)

namespace core
{

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

} // namespace core

//NOLINTEND(readability-identifier-naming)
