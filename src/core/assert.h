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
#	include <ctrace/ctrace.h>
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
#define CONCAT_IMPL(x, y) x##y
#define CONCAT(x, y) CONCAT_IMPL(x, y)
#define COUNTED_VAR_DECLARE(x) CONCAT(x, __COUNTER__)

#ifdef __cplusplus
#	define LOG_ERROR_IMPL(M, ...) \
	std::println(stderr, "{}:{} (errno: {})\n" M "\n", __FILE__, __LINE__, CLEAN_ERRNO() __VA_OPT__(,) __VA_ARGS__); \
	std::println(stderr, "{}", cpptrace::generate_trace().to_string())
#	define LOG_ERROR(M, ...) LOG_ERROR_IMPL(M __VA_OPT__(,) __VA_ARGS__)
#else
#	define LOG_ERROR_IMPL(M, C, ...) \
	fprintf(stderr, "%s:%d (errno: %s)\n" M "\n", __FILE__, __LINE__, CLEAN_ERRNO() __VA_OPT__(,) __VA_ARGS__); \
	ctrace_stacktrace C = ctrace_generate_trace(0, 64); \
	ctrace_print_stacktrace(&C, stderr, 1)
#	define LOG_ERROR(M, ...) LOG_ERROR_IMPL(M, COUNTED_VAR_DECLARE(__trace__) __VA_OPT__(,) __VA_ARGS__)
#endif

#ifdef __cplusplus
#	define ENSURE(A) \
		if (!(A)) \
		{ \
			LOG_ERROR("Ensure failed: {}", #A); \
			TRAP(); \
		}
#else
#	define ENSURE(A) \
		if (!(A)) \
		{ \
			LOG_ERROR("Ensure failed: %s", #A); \
			TRAP(); \
		}
#endif
#define ENSUREF(A, M, ...) \
	if (!(A)) \
	{ \
		LOG_ERROR(M __VA_OPT__(,) __VA_ARGS__); \
		TRAP(); \
	}
#ifdef NDEBUG
#	define ASSERT(A) ((void)(A))
#	define ASSERTF(A, M, ...) ((void)(A))
#else
#	define ASSERT(A) ENSURE(A)
#	define ASSERTF(A, M, ...) ENSUREF(A, M __VA_OPT__(,) __VA_ARGS__)
#endif
