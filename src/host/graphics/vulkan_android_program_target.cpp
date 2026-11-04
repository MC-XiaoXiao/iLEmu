// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "vulkan_android_program_target.hpp"
#include "graphics/gles_renderer.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace ilemu {
namespace {
    void check(VkResult result, const char* operation)
    {
        if (result != VK_SUCCESS)
            throw std::runtime_error { operation };
    }
    class SharedTarget final : public AndroidProgramTarget {
    public:
        SharedTarget(std::shared_ptr<GlesRenderer> owner,
            std::shared_ptr<HostSurface> surface,
            std::shared_ptr<VulkanProgramTargetAllocation> allocation, int fd,
            std::function<bool(int)> publish)
            : AndroidProgramTarget { std::move(owner), std::move(surface) }
            , allocation_ { std::move(allocation) }
            , fd_ { fd }
            , publish_ { std::move(publish) }
        {
        }
        ~SharedTarget() override
        {
            if (fd_ >= 0)
                close(fd_);
        }
        AHardwareBuffer* buffer() const override
        {
            return allocation_->buffer();
        }
        int take_ready_fd() override { return std::exchange(fd_, -1); }
        bool publish(int fd) override { return publish_(fd); }

    private:
        std::shared_ptr<VulkanProgramTargetAllocation> allocation_;
        int fd_;
        std::function<bool(int)> publish_;
    };
}

bool configure_android_program_target(VkPhysicalDevice physical,
    std::uint32_t instance_version, std::vector<const char*>& extensions)
{
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (std::min(instance_version, properties.apiVersion) < VK_API_VERSION_1_1)
        return false;
    std::uint32_t count { };
    check(vkEnumerateDeviceExtensionProperties(
              physical, nullptr, &count, nullptr),
        "enumerate program-input extensions");
    std::vector<VkExtensionProperties> available(count);
    check(vkEnumerateDeviceExtensionProperties(
              physical, nullptr, &count, available.data()),
        "enumerate program-input extensions");
    const char* required[] {
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME
    };
    for (const auto* name : required)
        if (std::ranges::none_of(available, [name](const auto& extension) {
                return std::strcmp(name, extension.extensionName) == 0;
            }))
            return false;
    VkPhysicalDeviceExternalSemaphoreInfo info {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO
    };
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkExternalSemaphoreProperties semaphore {
        VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES
    };
    vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &info, &semaphore);
    constexpr auto required_features =
        VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
        VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    if ((semaphore.externalSemaphoreFeatures & required_features) !=
        required_features)
        return false;
    VkFormatProperties source, destination;
    vkGetPhysicalDeviceFormatProperties(
        physical, VK_FORMAT_B8G8R8A8_UNORM, &source);
    vkGetPhysicalDeviceFormatProperties(
        physical, VK_FORMAT_R8G8B8A8_UNORM, &destination);
    constexpr auto blit_features =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((source.optimalTilingFeatures & blit_features) != blit_features ||
        (destination.optimalTilingFeatures & blit_features) != blit_features)
        return false;
    extensions.insert(
        extensions.end(), std::begin(required), std::end(required));
    return true;
}

VulkanProgramTargetAllocation::VulkanProgramTargetAllocation(
    VkPhysicalDevice physical, VkDevice device, std::uint32_t width,
    std::uint32_t height)
    : device_ { device }
    , width_ { width }
    , height_ { height }
{
    try {
        AHardwareBuffer_Desc description { };
        description.width = width;
        description.height = height;
        description.layers = 1;
        description.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        description.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
        if (AHardwareBuffer_allocate(&description, &buffer_) != 0)
            throw std::runtime_error { "allocate shader input buffer" };
        VkExternalMemoryImageCreateInfo external {
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO
        };
        external.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkImageCreateInfo image { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        image.pNext = &external;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_R8G8B8A8_UNORM;
        image.extent = { width, height, 1 };
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateImage(device_, &image, nullptr, &image_),
            "create shader input image");
        const auto get_properties =
            reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
                vkGetDeviceProcAddr(
                    device_, "vkGetAndroidHardwareBufferPropertiesANDROID"));
        VkAndroidHardwareBufferFormatPropertiesANDROID format {
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID
        };
        VkAndroidHardwareBufferPropertiesANDROID properties {
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID
        };
        properties.pNext = &format;
        check(get_properties(device_, buffer_, &properties),
            "query shader input buffer");
        constexpr auto blit_features =
            VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if (format.format != image.format ||
            (format.formatFeatures & blit_features) != blit_features)
            throw std::runtime_error {
                "shader input buffer cannot receive image blits"
            };
        VkPhysicalDeviceMemoryProperties types;
        vkGetPhysicalDeviceMemoryProperties(physical, &types);
        std::uint32_t type = 0;
        while (type < types.memoryTypeCount &&
               !(properties.memoryTypeBits & (1U << type)))
            ++type;
        VkImportAndroidHardwareBufferInfoANDROID imported {
            VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID
        };
        imported.buffer = buffer_;
        VkMemoryDedicatedAllocateInfo dedicated {
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO
        };
        dedicated.pNext = &imported;
        dedicated.image = image_;
        VkMemoryAllocateInfo allocation {
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
        };
        allocation.pNext = &dedicated;
        allocation.allocationSize = properties.allocationSize;
        allocation.memoryTypeIndex = type;
        check(vkAllocateMemory(device_, &allocation, nullptr, &memory_),
            "import shader input memory");
        check(vkBindImageMemory(device_, image_, memory_, 0),
            "bind shader input memory");
    } catch (...) {
        destroy();
        throw;
    }
}

void VulkanProgramTargetAllocation::destroy()
{
    if (image_)
        vkDestroyImage(device_, image_, nullptr);
    if (memory_)
        vkFreeMemory(device_, memory_, nullptr);
    if (buffer_)
        AHardwareBuffer_release(buffer_);
}
VulkanProgramTargetAllocation::~VulkanProgramTargetAllocation() { destroy(); }

void VulkanProgramTargetAllocation::encode(VkCommandBuffer command,
    VkImage source, std::uint32_t queue_family, bool inverted_vertical) const
{
    transfer(command, source, queue_family, inverted_vertical, false);
}

void VulkanProgramTargetAllocation::encode_publish(VkCommandBuffer command,
    VkImage destination, std::uint32_t queue_family,
    bool inverted_vertical) const
{
    transfer(command, destination, queue_family, inverted_vertical, true);
}

void VulkanProgramTargetAllocation::transfer(VkCommandBuffer command,
    VkImage canonical, std::uint32_t queue_family, bool inverted_vertical,
    bool publishing) const
{
    VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout =
        publishing ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = publishing ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                   : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barrier.dstQueueFamilyIndex = queue_family;
    barrier.dstAccessMask =
        publishing ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image = image_;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkImageBlit blit { };
    blit.srcSubresource =
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1] = { static_cast<std::int32_t>(width_),
        static_cast<std::int32_t>(height_), 1 };
    blit.dstOffsets[0] = { 0,
        inverted_vertical ? 0 : static_cast<std::int32_t>(height_), 0 };
    blit.dstOffsets[1] = { static_cast<std::int32_t>(width_),
        inverted_vertical ? static_cast<std::int32_t>(height_) : 0, 1 };
    vkCmdBlitImage(command, publishing ? image_ : canonical,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, publishing ? canonical : image_,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    barrier.oldLayout = barrier.newLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = queue_family;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barrier.srcAccessMask = barrier.dstAccessMask;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &barrier);
}

std::shared_ptr<GlesProgramTarget> make_android_program_target(
    std::shared_ptr<GlesRenderer> owner, std::shared_ptr<HostSurface> surface,
    std::shared_ptr<VulkanProgramTargetAllocation> allocation, int ready_fd,
    std::function<bool(int)> publish)
{
    try {
        return std::make_shared<SharedTarget>(std::move(owner),
            std::move(surface), std::move(allocation), ready_fd,
            std::move(publish));
    } catch (...) {
        if (ready_fd >= 0)
            close(ready_fd);
        throw;
    }
}
}
