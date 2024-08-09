#pragma once

#ifdef __cplusplus
#	include <cerrno>
#	include <cstdio>
#	include <cstring>
#	include <print>
#	include <cpptrace/cpptrace.hpp>
#else
#	include <errno.h>
#	include <stdio.h>
#	include <string.h>
#endif

#ifndef __has_builtin
#	define __has_builtin(x) 0
#endif
#if __has_builtin(__builtin_trap)
#	define TRAP() __builtin_trap()
#else
#	define TRAP() abort()
#endif

#define CLEAN_ERRNO() (errno == 0 ? "NULL" : strerror(errno))

#ifdef __cplusplus
#	define LOG_ERROR(M, ...) std::println(stderr, "{}:{}\nerrno: {}\n{}\n" M, __FILE__, __LINE__, CLEAN_ERRNO(), cpptrace::generate_trace().to_string(), ##__VA_ARGS__);
#else
#	define LOG_ERROR(M, ...) fprintf(stderr, "%s:%d\nerrno: %s\n" M, __FILE__, __LINE__, CLEAN_ERRNO(), ##__VA_ARGS__);
#endif

#ifdef NDEBUG
#	define ASSERT(A) ((void)(A))
#	define ASSERTF(A, M, ...) ((void)(A))
#else
#ifdef __cplusplus
#	define ASSERT(A)                                                                               \
		if (!(A))                                                                                  \
		{                                                                                          \
			LOG_ERROR("Assertion failed: {}", #A);                                                 \
			TRAP();                                                                                \
		}
#else
#	define ASSERT(A)                                                                               \
		if (!(A))                                                                                  \
		{                                                                                          \
			LOG_ERROR("Assertion failed: %s", #A);                                                 \
			TRAP();                                                                                \
		}
#endif
#	define ASSERTF(A, M, ...)                                                                      \
		if (!(A))                                                                                  \
		{                                                                                          \
			LOG_ERROR(M, ##__VA_ARGS__);                                                           \
			TRAP();                                                                                \
		}
#endif
#ifdef __cplusplus
#	define CHECK(A)                                                                                \
		if (!(A))                                                                                  \
		{                                                                                          \
			LOG_ERROR("Check failed: {}", #A);                                                     \
			TRAP();                                                                                \
		}
#else
#	define CHECK(A)                                                                                \
		if (!(A))                                                                                  \
		{                                                                                          \
			LOG_ERROR("Check failed: %s", #A);                                                     \
			TRAP();                                                                                \
		}
#endif
#define CHECKF(A, M, ...)                                                                          \
	if (!(A))                                                                                      \
	{                                                                                              \
		LOG_ERROR(M, ##__VA_ARGS__);                                                               \
		TRAP();                                                                                    \
	}
