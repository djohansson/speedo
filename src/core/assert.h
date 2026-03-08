#pragma once

#include <core/config.h>

#ifdef __cplusplus
#	include <cstdio>
#	include <cstring>
#	include <print>
#	include <cpptrace/cpptrace.hpp>
#else
#	include <stdio.h>
#	include <string.h>
#	include <ctrace/ctrace.h>
#endif

#ifdef __cplusplus
#	define LOG_ERROR_IMPL(M, ...) \
	std::println(stderr, "{}", cpptrace::generate_trace().to_string()); \
	std::println(stderr, "{}:{} (errno: {})\n" M "\n", __FILE__, __LINE__, CLEAN_ERRNO() __VA_OPT__(,) __VA_ARGS__)
#	define LOG_ERROR(M, ...) LOG_ERROR_IMPL(M __VA_OPT__(,) __VA_ARGS__)
#else
#	define LOG_ERROR_IMPL(M, C, ...) do \
{ \
	ctrace_stacktrace C = ctrace_generate_trace(0, 64); \
	ctrace_print_stacktrace(&C, stderr, 1); \
	fprintf(stderr, "%s:%d (errno: %s)\n" M "\n", __FILE__, __LINE__, CLEAN_ERRNO() __VA_OPT__(,) __VA_ARGS__); \
} while (false)
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
#ifdef __cplusplus
#	define CHECK(A) \
		if (!(A)) \
		{ \
			LOG_ERROR("Check failed: {}", #A); \
			DEBUGTRAP(); \
		}
#else
#	define CHECK(A) \
		if (!(A)) \
		{ \
			LOG_ERROR("Check failed: %s", #A); \
			DEBUGTRAP(); \
		}
#endif
#define CHECKF(A, M, ...) \
	if (!(A)) \
	{ \
		LOG_ERROR(M __VA_OPT__(,) __VA_ARGS__); \
		DEBUGTRAP(); \
	}
#ifdef NDEBUG
#	define ASSERT(A) ((void)(A))
#	define ASSERTF(A, M, ...) ((void)(A))
#else
#	define ASSERT(A) CHECK(A)
#	define ASSERTF(A, M, ...) CHECKF(A, M __VA_OPT__(,) __VA_ARGS__)
#endif
