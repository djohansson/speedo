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

#ifdef __cplusplus
#	define LOG_ERROR(M, ...) \
	std::println(stderr, "{}:{} (errno: {})\n{}\n" M "\n", __FILE__, __LINE__, CLEAN_ERRNO(), cpptrace::generate_trace().to_string() __VA_OPT__(,) __VA_ARGS__)
#else
#	define LOG_ERROR(M, ...) \
	fprintf(stderr, "%s:%d (errno: %s)\n" M "\n", __FILE__, __LINE__, CLEAN_ERRNO() __VA_OPT__(,) __VA_ARGS__); \
	ctrace_stacktrace trace = ctrace_generate_trace(0, 64); \
	ctrace_print_stacktrace(&trace, stderr, 1)
#endif

#ifdef __cplusplus
#	define ENSURE(A) \
		if (!(A)) \
		{ \
			LOG_ERROR("Check failed: {}", #A); \
			TRAP(); \
		}
#else
#	define ENSURE(A) \
		if (!(A)) \
		{ \
			LOG_ERROR("Check failed: %s", #A); \
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
