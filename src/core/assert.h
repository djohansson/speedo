#pragma once

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>

#define clean_errno() (errno == 0 ? "None" : strerror(errno))
#define log_error(M, ...)                                                                          \
	fprintf(                                                                                       \
		stderr,                                                                                    \
		"[ERROR] (%s:%d: errno: %s) " M "\n",                                                      \
		__FILE__,                                                                                  \
		__LINE__,                                                                                  \
		clean_errno(),                                                                             \
		##__VA_ARGS__)

#ifdef NDEBUG
#	define assertf(A, M, ...)
#else
#	define assertf(A, M, ...)                                                                      \
		if (!(A))                                                                                  \
		{                                                                                          \
			log_error(M, ##__VA_ARGS__);                                                           \
			assert(A);                                                                             \
		}
#endif
