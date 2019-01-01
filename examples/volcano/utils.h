#pragma once

template <typename T>
constexpr auto sizeof_array(const T& array)
{
	return (sizeof(array) / sizeof(array[0]));
}
