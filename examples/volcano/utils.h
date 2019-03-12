#pragma once

#include <cmath>
#include <iostream>

template <typename T>
inline constexpr auto sizeof_array(const T &array)
{
	return (sizeof(array) / sizeof(array[0]));
}

template <typename T>
struct ArrayDeleter
{
	using DeleteFcn = std::function<void(T*, size_t)>;

	ArrayDeleter() = default;
	ArrayDeleter(DeleteFcn&& deleter_, size_t size_) : deleter(deleter_), size(size_) {}
	
	inline void operator()(T* array) const { deleter(array, size); }

	DeleteFcn deleter;
	size_t size;
};

namespace stbi_istream_callbacks
{
int read(void *user, char *data, int size)
{
	std::istream *stream = static_cast<std::istream *>(user);
	return stream->rdbuf()->sgetn(data, size);
}

void skip(void *user, int size)
{
	std::istream *stream = static_cast<std::istream *>(user);
	stream->seekg(size, std::ios::cur);
}

int eof(void *user)
{
	std::istream *stream = static_cast<std::istream *>(user);
	return stream->tellg() != std::istream::pos_type(-1);
}
} // namespace stbi_istream_callbacks
