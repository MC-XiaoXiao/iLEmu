// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>
#include <memory>
#include <vulkan/vulkan.h>

namespace ilemu {

// A retained observer pins the exact queue submission. The renderer serializes
// prepare_for_submit and publication; a retained fence is never reset/reused.
// All references must be released before the owning VkDevice is destroyed.
class VulkanSubmissionFence final {
public:
    explicit VulkanSubmissionFence(VkDevice device);
    ~VulkanSubmissionFence();
    VulkanSubmissionFence(const VulkanSubmissionFence&) = delete;
    VulkanSubmissionFence& operator=(const VulkanSubmissionFence&) = delete;

    static void prepare_for_submit(
        std::shared_ptr<VulkanSubmissionFence>& fence, VkDevice device);
    [[nodiscard]] VkFence handle() const { return fence_; }
    [[nodiscard]] VkResult status() const;
    [[nodiscard]] VkResult wait(std::uint64_t timeout) const;

private:
    VkDevice device_ { };
    VkFence fence_ { };
};

} // namespace ilemu
