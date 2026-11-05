// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "android_program_target.hpp"
#include <functional>
#include <vector>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

namespace ilemu {
// The command ring retains only this allocation, without a renderer reference.
// The consumer separately retains the renderer through GlesProgramTarget.
class VulkanProgramTargetAllocation final {
public:
    VulkanProgramTargetAllocation(VkPhysicalDevice physical, VkDevice device,
        std::uint32_t width, std::uint32_t height);
    ~VulkanProgramTargetAllocation();
    VulkanProgramTargetAllocation(
        const VulkanProgramTargetAllocation&) = delete;
    VulkanProgramTargetAllocation& operator=(
        const VulkanProgramTargetAllocation&) = delete;
    void encode(VkCommandBuffer command, VkImage source,
        std::uint32_t queue_family, bool inverted_vertical) const;
    void encode_publish(VkCommandBuffer command, VkImage destination,
        std::uint32_t queue_family, bool inverted_vertical) const;
    [[nodiscard]] AHardwareBuffer* buffer() const { return buffer_; }
    void retain_import(std::shared_ptr<void> image)
    {
        import_ = std::move(image);
    }

private:
    void transfer(VkCommandBuffer command, VkImage canonical,
        std::uint32_t queue_family, bool inverted_vertical,
        bool publishing) const;
    void destroy();
    VkDevice device_;
    AHardwareBuffer* buffer_ { };
    VkImage image_ { };
    VkDeviceMemory memory_ { };
    std::uint32_t width_, height_;
    std::shared_ptr<void> import_;
};

[[nodiscard]] bool configure_android_program_target(VkPhysicalDevice physical,
    std::uint32_t instance_version, std::vector<const char*>& extensions);
[[nodiscard]] std::shared_ptr<GlesProgramTarget> make_android_program_target(
    std::shared_ptr<GlesRenderer> owner, std::shared_ptr<HostSurface> surface,
    std::shared_ptr<VulkanProgramTargetAllocation> allocation, int ready_fd,
    std::function<bool(int)> publish);
}
