#pragma once

// tinybvh as gfx configures it: for its declarations (gfx/bvh.cpp) and its implementation (gfx/libraries.cpp) alike

// the builds run on the application's executor (see gfx/bvh.cpp's detail::Executor), not on tinybvh's own threads
#define TINYBVH_NO_BUILTIN_POOL
// the depth its builds and conversions walk (no traversal runs on the cpu here): a polygon of hundreds of vertices is a fan
// of slivers around one corner, which the builders peel off a few at a time (geodesic.zip's duals, faces of up to 384
// vertices, went past tinybvh's 128: "CombineLeafs, tree too deep")
#define TINYBVH_STACK_SIZE 1024
#include <tinybvh/tiny_bvh.h>
