/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "fd_program_target.hpp"
#include <functional>
#include <optional>
#include <vector>
#include <vulkan/vulkan.h>
namespace ilemu {
[[nodiscard]] std::optional<GlesProgramTarget::DeviceIdentity>
configure_fd_program_target(VkPhysicalDevice physical,
    std::uint32_t instance_version, std::vector<const char*>& extensions);
class VulkanFdSemaphore final {
public:
    explicit VulkanFdSemaphore(VkDevice device);
    ~VulkanFdSemaphore();
    VulkanFdSemaphore(const VulkanFdSemaphore&) = delete;
    VulkanFdSemaphore& operator=(const VulkanFdSemaphore&) = delete;
    [[nodiscard]] VkSemaphore handle() const { return semaphore_; }
    [[nodiscard]] int export_fd() const;

private:
    VkDevice device_;
    VkSemaphore semaphore_ { };
};
class VulkanFdProgramAllocation final {
public:
    VulkanFdProgramAllocation(VkPhysicalDevice physical, VkDevice device,
        std::uint32_t width, std::uint32_t height);
    ~VulkanFdProgramAllocation();
    VulkanFdProgramAllocation(const VulkanFdProgramAllocation&) = delete;
    VulkanFdProgramAllocation& operator=(
        const VulkanFdProgramAllocation&) = delete;
    [[nodiscard]] VkDevice device() const { return device_; }
    [[nodiscard]] std::uint64_t size() const { return size_; }
    [[nodiscard]] int export_fd() const;
    void encode(VkCommandBuffer command, VkImage canonical,
        std::uint32_t queue_family, bool inverted_vertical,
        bool publishing) const;

private:
    void destroy();
    VkDevice device_;
    VkImage image_ { };
    VkDeviceMemory memory_ { };
    VkDeviceSize size_ { };
    VkExtent2D extent_;
};
[[nodiscard]] std::shared_ptr<GlesProgramTarget> make_fd_program_target(
    std::shared_ptr<GlesRenderer> owner, std::shared_ptr<HostSurface> surface,
    GlesProgramTarget::DeviceIdentity device,
    std::shared_ptr<VulkanFdProgramAllocation> allocation,
    std::shared_ptr<VulkanFdSemaphore> ready,
    std::function<std::shared_ptr<VulkanFdSemaphore>(
        std::shared_ptr<VulkanFdSemaphore>)> publish);
}
