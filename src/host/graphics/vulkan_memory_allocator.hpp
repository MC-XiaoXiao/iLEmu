#pragma once

// The renderer requests Vulkan 1.0, so keep VMA on the same API contract.
// iLegacySim links the Vulkan loader directly and does not need VMA's dynamic
// function loading path.
#define VMA_VULKAN_VERSION 1000000
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0

#include <vk_mem_alloc.h>
