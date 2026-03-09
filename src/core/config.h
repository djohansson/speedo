#pragma once

#ifdef __cplusplus
#	include <cerrno>
#	include <csignal>
#	include <cstdlib>
#else
#	include <errno.h>
#	include <signal.h>
#	include <stdlib.h>
#endif

#ifndef __has_builtin
#	define __has_builtin(x) 0
#endif
#if __has_builtin(__builtin_trap)
#	define TRAP() __builtin_trap()
#else
#	define TRAP() abort()
#endif

#if __has_builtin(__builtin_debugtrap)
#	define DEBUGTRAP() __builtin_debugtrap()
#else
#	define DEBUGTRAP() raise(SIGTRAP)
#endif

#define CLEAN_ERRNO() (errno == 0 ? "NULL" : strerror(errno))
#define CONCAT_IMPL(x, y) x##y
#define CONCAT(x, y) CONCAT_IMPL(x, y)
#define COUNTED_VAR_DECLARE(x) CONCAT(x, __COUNTER__)
#define SIZEOF__VA_ARGS__(...)  (sizeof((int[]){__VA_ARGS__})/sizeof(int)) //NOLINT(modernize-avoid-c-arrays)
