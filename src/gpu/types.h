#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <type_traits>

// todo: make into a concept
enum /*class*/ GraphicsBackend : uint8_t
{
	Vk
};

//using enum GraphicsBackend;

#include "types.inl"
