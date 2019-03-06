#pragma once

#include <cmath>
#include <iostream>

template <typename T>
inline constexpr auto sizeof_array(const T &array)
{
	return (sizeof(array) / sizeof(array[0]));
}

inline constexpr double pi() { return 3.14159265358979323846; }

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
