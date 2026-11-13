#pragma once

// tinybvh as gfx configures it: for its declarations (gfx/bvh.cpp) and its implementation (gfx/libraries.cpp) alike

// the builds run on the application's executor (see gfx/bvh.cpp's detail::Executor), not on tinybvh's own threads
#define TINYBVH_NO_BUILTIN_POOL
// the depth its builds and conversions walk (no traversal runs on the cpu here): a polygon of hundreds of vertices is a fan
// of slivers around one corner, which the builders peel off a few at a time (geodesic.zip's duals, faces of up to 384
// vertices, went past tinybvh's 128: "CombineLeafs, tree too deep")
#define TINYBVH_STACK_SIZE 1024
// its scalar code on x86-64 without SSE 4.2: tinybvh takes its SSE path for any _MSC_VER, whose SSE 4.1 intrinsics clang
// compiles only for a cpu that has them (Windows' own code is built for the baseline x86-64 cpu: the toolchain names
// x86-64-v3 for an x86_64 processor, which Windows calls AMD64; "_mm_blendv_ps requires target feature 'sse4.1'", CI's
// first build with the implementation compiled rather than taken from the cache). Linux's x86-64 is v3, Apple's NEON
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__SSE4_2__)
#define TINYBVH_NO_SIMD
#endif
#include <tinybvh/tiny_bvh.h>
