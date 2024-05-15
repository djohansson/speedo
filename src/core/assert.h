#pragma once

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef __has_builtin
#	define __has_builtin(x) 0
#endif
#if __has_builtin(__builtin_trap)
#	define TRAP() __builtin_trap()
#else
#	define TRAP() abort()
#endif

#define CLEAN_ERRNO() (errno == 0 ? "NULL" : strerror(errno))
#define LOG_ERROR(M, ...) fprintf(stderr, "[ERROR] (%s:%d: errno: %s) " M "\n", __FILE__, __LINE__, CLEAN_ERRNO(), ##__VA_ARGS__) // NOLINT(modernize-use-std-print)

#ifdef NDEBUG
#	define ASSERT(A)
#	define ASSERTF(A, M, ...)
#else
#	define ASSERT(A)                                                                               \
		if (!(A))                                                                                  \
		{                                                                                          \
			LOG_ERROR("Assertion failed: %s", #A);                                                 \
			TRAP();                                                                                \
		}
#	define ASSERTF(A, M, ...)                                                                      \
		if (!(A))                                                                                  \
		{                                                                                          \
			LOG_ERROR(M, ##__VA_ARGS__);                                                           \
			TRAP();                                                                                \
		}
#endif
