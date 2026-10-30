// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "vulkan_memory_allocator.hpp"

namespace ilemu {

// Physical image ownership, independent of target names or pooled bindings.
// Observers also retain the renderer, so its device/allocator outlive this
// lease.
class VulkanImageAllocation final {
public:
    VulkanImageAllocation(VmaAllocator allocator, VmaAllocation allocation,
        VkDevice device, VkImage image, VkImageView view, VkSampler sampler)
        : allocator_ { allocator }
        , allocation_ { allocation }
        , device_ { device }
        , image_ { image }
        , view_ { view }
        , sampler_ { sampler }
    {
    }
    ~VulkanImageAllocation();
    VulkanImageAllocation(const VulkanImageAllocation&) = delete;
    VulkanImageAllocation& operator=(const VulkanImageAllocation&) = delete;

private:
    VmaAllocator allocator_ { };
    VmaAllocation allocation_ { };
    VkDevice device_ { };
    VkImage image_ { };
    VkImageView view_ { };
    VkSampler sampler_ { };
};

} // namespace ilemu
