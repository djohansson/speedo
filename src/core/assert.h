#pragma once

#include <cassert>
#include <print>

#define clean_errno() (errno == 0 ? "None" : strerror(errno))
#define log_error(M, ...)                                                                          \
	std::println(                                                                                       \
		stderr,                                                                                    \
		"[ERROR] ({}:{}: errno: {}) " M,                                                      \
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
