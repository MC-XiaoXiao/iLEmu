// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <chrono>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

namespace ilemu {

// Optional FIFO scheduling, owned and serialized by the Vulkan renderer.
// No guest clocks, window-system APIs, sleeping, or frame replacement.
class VulkanPresentTiming {
public:
    void add_instance_extensions(std::vector<const char*>& extensions);
    void configure_device(VkInstance instance, VkPhysicalDevice physical,
        VkSurfaceKHR surface, std::vector<const char*>& extensions,
        VkDeviceCreateInfo& info);
    void set_device(VkDevice device);
    [[nodiscard]] VkSwapchainCreateFlagsKHR swapchain_flags(
        VkSurfaceKHR surface) const;
    void attach(VkSwapchainKHR swapchain, VkSwapchainCreateFlagsKHR flags);
    void detach();
    [[nodiscard]] VkResult present(VkQueue queue, const VkPresentInfoKHR& info,
        std::chrono::nanoseconds period);

private:
#if defined(VK_EXT_present_timing)
    [[nodiscard]] bool surface_supported(VkSurfaceKHR surface) const;
    [[nodiscard]] bool calibrate();
    VkPhysicalDevice physical_ { };
    VkDevice device_ { };
    VkSwapchainKHR swapchain_ { };
    bool enabled_ { };
    bool instance_extensions_enabled_ { };
    VkPhysicalDevicePresentId2FeaturesKHR id_features_ { };
    VkPhysicalDevicePresentTimingFeaturesEXT timing_features_ { };
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR surface_capabilities_ { };
    PFN_vkGetSwapchainTimeDomainPropertiesEXT time_domains_ { };
    PFN_vkGetCalibratedTimestampsKHR timestamps_ { };
    std::uint64_t domain_ { };
    std::uint64_t domain_counter_ { };
    std::uint64_t present_id_ { };
    std::uint64_t calibrated_timestamp_ { };
    std::chrono::steady_clock::time_point calibrated_at_ { };
    std::chrono::steady_clock::time_point last_submission_ { };
    std::chrono::steady_clock::time_point target_ { };
    std::chrono::nanoseconds period_ { };
#endif
};

} // namespace ilemu
