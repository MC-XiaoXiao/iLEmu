// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "vulkan_present_timing.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace ilemu {
#if defined(VK_EXT_present_timing)
namespace {
    template <typename T> T structure(VkStructureType type)
    {
        T value { };
        value.sType = type;
        return value;
    }

    bool has_extension(
        const std::vector<VkExtensionProperties>& properties, const char* name)
    {
        return std::ranges::any_of(properties, [name](const auto& property) {
            return std::strcmp(property.extensionName, name) == 0;
        });
    }

    void add_extension(std::vector<const char*>& extensions, const char* name)
    {
        if (std::ranges::none_of(extensions, [name](const char* existing) {
                return std::strcmp(existing, name) == 0;
            })) {
            extensions.push_back(name);
        }
    }
} // namespace
#endif

void VulkanPresentTiming::add_instance_extensions(
    std::vector<const char*>& extensions)
{
#if defined(VK_EXT_present_timing)
    std::uint32_t count { };
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) !=
        VK_SUCCESS)
        return;
    std::vector<VkExtensionProperties> properties(count);
    if (vkEnumerateInstanceExtensionProperties(
            nullptr, &count, properties.data()) != VK_SUCCESS)
        return;
    for (const auto name :
        { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME }) {
        if (!has_extension(properties, name))
            return;
    }
    add_extension(
        extensions, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    add_extension(extensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    instance_extensions_enabled_ = true;
#else
    static_cast<void>(extensions);
#endif
}

void VulkanPresentTiming::configure_device(VkInstance instance,
    VkPhysicalDevice physical, VkSurfaceKHR surface,
    std::vector<const char*>& extensions, VkDeviceCreateInfo& info)
{
#if defined(VK_EXT_present_timing)
    if (!instance_extensions_enabled_)
        return;
    physical_ = physical;
    const auto features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2KHR>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2KHR"));
    surface_capabilities_ =
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
            vkGetInstanceProcAddr(
                instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
    if (!features || !surface_capabilities_ || surface == VK_NULL_HANDLE)
        return;
    std::uint32_t count { };
    if (vkEnumerateDeviceExtensionProperties(
            physical, nullptr, &count, nullptr) != VK_SUCCESS)
        return;
    std::vector<VkExtensionProperties> properties(count);
    if (vkEnumerateDeviceExtensionProperties(
            physical, nullptr, &count, properties.data()) != VK_SUCCESS)
        return;
    for (const auto name : { VK_KHR_PRESENT_ID_2_EXTENSION_NAME,
             VK_EXT_PRESENT_TIMING_EXTENSION_NAME,
             VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME }) {
        if (!has_extension(properties, name))
            return;
    }
    id_features_ = structure<VkPhysicalDevicePresentId2FeaturesKHR>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR);
    timing_features_ = structure<VkPhysicalDevicePresentTimingFeaturesEXT>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT);
    id_features_.pNext = &timing_features_;
    auto supported = structure<VkPhysicalDeviceFeatures2>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    supported.pNext = &id_features_;
    features(physical, &supported);
    if (!id_features_.presentId2 || !timing_features_.presentTiming ||
        !timing_features_.presentAtAbsoluteTime ||
        !surface_supported(surface)) {
        return;
    }
    timing_features_.presentAtRelativeTime = VK_FALSE;
    timing_features_.pNext = const_cast<void*>(info.pNext);
    info.pNext = &id_features_;
    for (const auto name : { VK_KHR_PRESENT_ID_2_EXTENSION_NAME,
             VK_EXT_PRESENT_TIMING_EXTENSION_NAME,
             VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME }) {
        add_extension(extensions, name);
    }
    enabled_ = true;
#else
    static_cast<void>(instance);
    static_cast<void>(physical);
    static_cast<void>(surface);
    static_cast<void>(extensions);
    static_cast<void>(info);
#endif
}

void VulkanPresentTiming::set_device(VkDevice device)
{
#if defined(VK_EXT_present_timing)
    device_ = device;
    if (!enabled_)
        return;
    time_domains_ = reinterpret_cast<PFN_vkGetSwapchainTimeDomainPropertiesEXT>(
        vkGetDeviceProcAddr(device, "vkGetSwapchainTimeDomainPropertiesEXT"));
    timestamps_ = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(
        vkGetDeviceProcAddr(device, "vkGetCalibratedTimestampsKHR"));
    enabled_ = time_domains_ && timestamps_;
#else
    static_cast<void>(device);
#endif
}

#if defined(VK_EXT_present_timing)
bool VulkanPresentTiming::surface_supported(VkSurfaceKHR surface) const
{
    auto timing = structure<VkPresentTimingSurfaceCapabilitiesEXT>(
        VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT);
    auto ids = structure<VkSurfaceCapabilitiesPresentId2KHR>(
        VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR);
    ids.pNext = &timing;
    auto capabilities = structure<VkSurfaceCapabilities2KHR>(
        VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);
    capabilities.pNext = &ids;
    auto info = structure<VkPhysicalDeviceSurfaceInfo2KHR>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR);
    info.surface = surface;
    return surface_capabilities_(physical_, &info, &capabilities) ==
               VK_SUCCESS &&
           ids.presentId2Supported && timing.presentTimingSupported &&
           timing.presentAtAbsoluteTimeSupported &&
           (timing.presentStageQueries &
               VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
}
#endif

VkSwapchainCreateFlagsKHR VulkanPresentTiming::swapchain_flags(
    VkSurfaceKHR surface) const
{
#if defined(VK_EXT_present_timing)
    if (enabled_ && surface_supported(surface))
        return VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR |
               VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
#else
    static_cast<void>(surface);
#endif
    return 0;
}

void VulkanPresentTiming::attach(
    VkSwapchainKHR swapchain, VkSwapchainCreateFlagsKHR flags)
{
    detach();
#if defined(VK_EXT_present_timing)
    if (enabled_ && (flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT)) {
        swapchain_ = swapchain;
        if (!calibrate())
            detach();
    }
#else
    static_cast<void>(swapchain);
    static_cast<void>(flags);
#endif
}

void VulkanPresentTiming::detach()
{
#if defined(VK_EXT_present_timing)
    swapchain_ = VK_NULL_HANDLE;
    present_id_ = 0;
    last_submission_ = { };
    target_ = { };
    period_ = { };
#endif
}

#if defined(VK_EXT_present_timing)
bool VulkanPresentTiming::calibrate()
{
    auto domains = structure<VkSwapchainTimeDomainPropertiesEXT>(
        VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT);
    if (time_domains_(device_, swapchain_, &domains, &domain_counter_) !=
            VK_SUCCESS ||
        !domains.timeDomainCount)
        return false;
    std::vector<VkTimeDomainKHR> types(domains.timeDomainCount);
    std::vector<std::uint64_t> ids(domains.timeDomainCount);
    domains.pTimeDomains = types.data();
    domains.pTimeDomainIds = ids.data();
    if (time_domains_(device_, swapchain_, &domains, &domain_counter_) !=
        VK_SUCCESS)
        return false;
    auto found = std::find(
        types.begin(), types.end(), VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT);
    if (found == types.end())
        return false;
    domain_ = ids[static_cast<std::size_t>(found - types.begin())];
    auto stage = structure<VkSwapchainCalibratedTimestampInfoEXT>(
        VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT);
    stage.swapchain = swapchain_;
    stage.timeDomainId = domain_;
    stage.presentStage = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
    auto info = structure<VkCalibratedTimestampInfoKHR>(
        VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR);
    info.timeDomain = VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT;
    info.pNext = &stage;
    std::uint64_t timestamp { }, deviation { };
    const auto before = std::chrono::steady_clock::now();
    const auto result = timestamps_(device_, 1, &info, &timestamp, &deviation);
    const auto after = std::chrono::steady_clock::now();
    // A wide calibration window would defeat sub-frame scheduling. Ordinary
    // FIFO is preferable to a target in a clock domain we cannot map reliably.
    if (result != VK_SUCCESS || deviation > 500'000 ||
        after - before > std::chrono::microseconds(500)) {
        return false;
    }
    const auto midpoint = before + (after - before) / 2;
    calibrated_timestamp_ = timestamp;
    calibrated_at_ = midpoint;
    return true;
}
#endif

VkResult VulkanPresentTiming::present(VkQueue queue,
    const VkPresentInfoKHR& info, std::chrono::nanoseconds period)
{
#if defined(VK_EXT_present_timing)
    if (swapchain_ != VK_NULL_HANDLE && info.swapchainCount == 1 &&
        info.pSwapchains[0] == swapchain_ && period.count() > 0 &&
        period <= std::chrono::seconds(1)) {
        auto domains = structure<VkSwapchainTimeDomainPropertiesEXT>(
            VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT);
        std::uint64_t counter { };
        const auto now = std::chrono::steady_clock::now();
        if (time_domains_(device_, swapchain_, &domains, &counter) !=
                VK_SUCCESS ||
            ((counter != domain_counter_ ||
                 now - calibrated_at_ > std::chrono::seconds(1)) &&
                !calibrate())) {
            detach();
            return vkQueuePresentKHR(queue, &info);
        }
        const auto lead = period + period / 2;
        // A gap spanning two display periods breaks the source cadence.
        // Re-anchor that boundary so its depleted buffer does not carry into
        // the next continuous animation. Ordinary late frames retain phase.
        if (period != period_ ||
            last_submission_ == std::chrono::steady_clock::time_point { } ||
            now - last_submission_ > 2 * period) {
            target_ = now + lead;
        } else {
            // Retain phase through normal translation jitter. A missed target
            // is presented as soon as possible, without refilling the buffer.
            target_ = std::clamp(target_ + period, now, now + lead + period);
        }
        period_ = period;
        last_submission_ = now;
        const auto delta = std::chrono::duration_cast<std::chrono::nanoseconds>(
            target_ - calibrated_at_)
                               .count();
        if (delta < 0 ||
            calibrated_timestamp_ > std::numeric_limits<std::uint64_t>::max() -
                                        static_cast<std::uint64_t>(delta) ||
            present_id_ == std::numeric_limits<std::uint64_t>::max()) {
            detach();
            return vkQueuePresentKHR(queue, &info);
        }
        ++present_id_;
        auto timing = structure<VkPresentTimingInfoEXT>(
            VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT);
        timing.targetTime =
            calibrated_timestamp_ + static_cast<std::uint64_t>(delta);
        timing.timeDomainId = domain_;
        timing.targetTimeDomainPresentStage =
            VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
        auto timings = structure<VkPresentTimingsInfoEXT>(
            VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT);
        timings.swapchainCount = 1;
        timings.pTimingInfos = &timing;
        timings.pNext = info.pNext;
        auto ids =
            structure<VkPresentId2KHR>(VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR);
        ids.swapchainCount = 1;
        ids.pPresentIds = &present_id_;
        ids.pNext = &timings;
        auto timed = info;
        timed.pNext = &ids;
        return vkQueuePresentKHR(queue, &timed);
    }
    // Synthetic frames and power transitions have no cadence to preserve.
    last_submission_ = { };
#endif
    return vkQueuePresentKHR(queue, &info);
}
} // namespace ilemu
