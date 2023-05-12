#include "application.h"

#include <mimalloc-new-delete.h>

struct MiMallocAllocator
{
	static inline void* malloc(std::size_t size) { return mi_malloc(size); }
	static inline void free(void* ptr) { mi_free(ptr); }
};
