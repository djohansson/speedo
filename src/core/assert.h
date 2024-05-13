#pragma once

#include <cstring>
#include <print>

#ifndef __has_builtin
#	define __has_builtin(x) 0
#endif
#if __has_builtin(__builtin_trap)
#	define TRAP() __builtin_trap()
#else
#	define TRAP() abort()
#endif

#define CLEAN_ERRNO() (errno == 0 ? "NULL" : std::strerror(errno))
#define LOG_ERROR(M, ...)                                                                          \
	std::println(                                                                                  \
		stderr,                                                                                    \
		"[ERROR] ({}:{}: errno: {}) " M,                                                           \
		__FILE__,                                                                                  \
		__LINE__,                                                                                  \
		CLEAN_ERRNO(),                                                                             \
		##__VA_ARGS__)

#ifdef NDEBUG
#	define ASSERT(A, M, ...)
#else
#	define ASSERT(A, M, ...)                                                                       \
		if (!(A))                                                                                  \
		{                                                                                          \
			LOG_ERROR(M, ##__VA_ARGS__);                                                           \
			TRAP();                                                                                \
		}
#endif
