// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "vulkan_submission_fence.hpp"

#include <chrono>
#include <stdexcept>

#include "foundation/performance.hpp"

namespace ilemu {

VulkanSubmissionFence::VulkanSubmissionFence(VkDevice device)
    : device_ { device }
{
    VkFenceCreateInfo info { };
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device_, &info, nullptr, &fence_) != VK_SUCCESS)
        throw std::runtime_error { "vkCreateFence failed" };
}

VulkanSubmissionFence::~VulkanSubmissionFence()
{
    if (fence_ != VK_NULL_HANDLE)
        vkDestroyFence(device_, fence_, nullptr);
}

void VulkanSubmissionFence::prepare_for_submit(
    std::shared_ptr<VulkanSubmissionFence>& fence, VkDevice device)
{
    // The slot's previous GPU use has already completed. Only its exclusive
    // owner may reset it: an outside waiter must keep observing its old batch.
    if (!fence || fence.use_count() != 1) {
        fence = std::make_shared<VulkanSubmissionFence>(device);
    } else if (vkResetFences(device, 1, &fence->fence_) != VK_SUCCESS) {
        throw std::runtime_error { "vkResetFences failed" };
    }
}

VkResult VulkanSubmissionFence::status() const
{
    return vkGetFenceStatus(device_, fence_);
}

VkResult VulkanSubmissionFence::wait(std::uint64_t timeout) const
{
    const auto measure = performance_counters().enabled();
    const auto start = measure ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point { };
    const auto result = vkWaitForFences(device_, 1, &fence_, VK_TRUE, timeout);
    if (measure) {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        performance_counters().record_fence_wait(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                .count()));
    }
    return result;
}

} // namespace ilemu
