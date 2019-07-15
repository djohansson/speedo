#pragma once

#include <cmath>
#include <functional>
#include <iostream>

template <typename T>
inline constexpr auto sizeof_array(const T& array)
{
	return (sizeof(array) / sizeof(array[0]));
}

inline uint32_t roundUp(uint32_t numToRound, uint32_t multiple)
{
	if (multiple == 0)
		return numToRound;

	auto remainder = numToRound % multiple;
	if (remainder == 0)
		return numToRound;

	return numToRound + multiple - remainder;
}

template <typename T>
class ArrayDeleter
{
	using DeleteFcn = std::function<void(T*, size_t)>;

public:

	ArrayDeleter() = default;
	ArrayDeleter(DeleteFcn&& deleter)
		: myDeleter(std::move(deleter))
	{}
	ArrayDeleter(DeleteFcn&& deleter, size_t size)
		: myDeleter(std::move(deleter))
		, mySize(size)
	{}

	inline void operator()(T* array) const
	{
		myDeleter(array, mySize);
	}

	inline size_t getSize() const { return mySize; }

private:

	DeleteFcn myDeleter = [](T* data, size_t){ delete [] data; };
	size_t mySize = 0;
};

class Noncopyable
{
public:
	Noncopyable() = default;
	~Noncopyable() = default;

private:
	Noncopyable(const Noncopyable&) = delete;
	Noncopyable& operator=(const Noncopyable&) = delete;
};
