// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "vulkan_image_allocation.hpp"

namespace ilemu {

VulkanImageAllocation::~VulkanImageAllocation()
{
    if (sampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device_, sampler_, nullptr);
    if (view_ != VK_NULL_HANDLE)
        vkDestroyImageView(device_, view_, nullptr);
    if (allocator_ != nullptr)
        vmaDestroyImage(allocator_, image_, allocation_);
}

} // namespace ilemu
