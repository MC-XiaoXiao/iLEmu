/* SPDX-License-Identifier: MPL-2.0 */
#include "vulkan_fd_program_target.hpp"
#include "vulkan_program_transfer.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>
namespace ilemu {
namespace {
    constexpr VkImageUsageFlags usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    void check(VkResult result, const char* operation)
    {
        if (result != VK_SUCCESS)
            throw std::runtime_error { operation };
    }
    class SharedTarget final : public FdProgramTarget {
    public:
        SharedTarget(std::shared_ptr<GlesRenderer> owner,
            std::shared_ptr<HostSurface> surface, DeviceIdentity device,
            std::shared_ptr<VulkanFdProgramAllocation> allocation,
            std::shared_ptr<VulkanFdSemaphore> ready,
            std::function<std::shared_ptr<VulkanFdSemaphore>(
                std::shared_ptr<VulkanFdSemaphore>)>
                publish)
            : FdProgramTarget { std::move(owner), std::move(surface), device }
            , allocation_ { std::move(allocation) }
            , ready_ { std::move(ready) }
            , publish_ { std::move(publish) }
        {
        }
        std::uint64_t allocation_size() const override
        {
            return allocation_->size();
        }
        int export_memory_fd() const override
        {
            return allocation_->export_fd();
        }
        int export_ready_fd() const override { return ready_->export_fd(); }
        int begin_publication() override
        {
            publication_ =
                std::make_shared<VulkanFdSemaphore>(allocation_->device());
            return publication_->export_fd();
        }
        int publish() override
        {
            return publish_(std::move(publication_))->export_fd();
        }

    private:
        std::shared_ptr<VulkanFdProgramAllocation> allocation_;
        std::shared_ptr<VulkanFdSemaphore> ready_, publication_;
        std::function<std::shared_ptr<VulkanFdSemaphore>(
            std::shared_ptr<VulkanFdSemaphore>)>
            publish_;
    };
}
std::optional<GlesProgramTarget::DeviceIdentity> configure_fd_program_target(
    VkPhysicalDevice physical, std::uint32_t instance_version,
    std::vector<const char*>& extensions)
{
    VkPhysicalDeviceProperties legacy;
    vkGetPhysicalDeviceProperties(physical, &legacy);
    if (std::min(instance_version, legacy.apiVersion) < VK_API_VERSION_1_1)
        return std::nullopt;
    VkPhysicalDeviceIDProperties id {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES
    };
    VkPhysicalDeviceProperties2 properties {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
    };
    properties.pNext = &id;
    vkGetPhysicalDeviceProperties2(physical, &properties);
    std::uint32_t count { };
    check(vkEnumerateDeviceExtensionProperties(
              physical, nullptr, &count, nullptr),
        "enumerate external target extensions");
    std::vector<VkExtensionProperties> available(count);
    check(vkEnumerateDeviceExtensionProperties(
              physical, nullptr, &count, available.data()),
        "enumerate external target extensions");
    const char* required[] { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME };
    for (const auto* name : required)
        if (std::ranges::none_of(available, [name](const auto& p) {
                return std::strcmp(name, p.extensionName) == 0;
            }))
            return std::nullopt;
    VkPhysicalDeviceExternalImageFormatInfo external {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO
    };
    external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkPhysicalDeviceImageFormatInfo2 image {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2
    };
    image.pNext = &external;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.type = VK_IMAGE_TYPE_2D;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = usage;
    image.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VkExternalImageFormatProperties exported {
        VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES
    };
    VkImageFormatProperties2 format {
        VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2
    };
    format.pNext = &exported;
    const auto result =
        vkGetPhysicalDeviceImageFormatProperties2(physical, &image, &format);
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED)
        return std::nullopt;
    check(result, "query external target format");
    if (!(exported.externalMemoryProperties.externalMemoryFeatures &
            VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
        return std::nullopt;
    VkPhysicalDeviceExternalSemaphoreInfo semaphore {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO
    };
    semaphore.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalSemaphoreProperties sync {
        VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES
    };
    vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &semaphore, &sync);
    if (!(sync.externalSemaphoreFeatures &
            VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT))
        return std::nullopt;
    VkFormatProperties source, destination;
    vkGetPhysicalDeviceFormatProperties(
        physical, VK_FORMAT_B8G8R8A8_UNORM, &source);
    vkGetPhysicalDeviceFormatProperties(physical, image.format, &destination);
    constexpr auto blit =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((source.optimalTilingFeatures & blit) != blit ||
        (destination.optimalTilingFeatures & blit) != blit)
        return std::nullopt;
    extensions.insert(
        extensions.end(), std::begin(required), std::end(required));
    GlesProgramTarget::DeviceIdentity device;
    std::copy(std::begin(id.deviceUUID), std::end(id.deviceUUID),
        device.uuid.begin());
    std::copy(std::begin(id.driverUUID), std::end(id.driverUUID),
        device.driver.begin());
    return device;
}
VulkanFdSemaphore::VulkanFdSemaphore(VkDevice device)
    : device_ { device }
{
    VkExportSemaphoreCreateInfo external {
        VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO
    };
    external.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkSemaphoreCreateInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    info.pNext = &external;
    check(vkCreateSemaphore(device_, &info, nullptr, &semaphore_),
        "create external program semaphore");
}
VulkanFdSemaphore::~VulkanFdSemaphore()
{
    vkDestroySemaphore(device_, semaphore_, nullptr);
}
int VulkanFdSemaphore::export_fd() const
{
    VkSemaphoreGetFdInfoKHR info {
        VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR
    };
    info.semaphore = semaphore_;
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd;
    const auto export_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        vkGetDeviceProcAddr(device_, "vkGetSemaphoreFdKHR"));
    check(export_fd(device_, &info, &fd), "export program semaphore");
    return fd;
}
VulkanFdProgramAllocation::VulkanFdProgramAllocation(VkPhysicalDevice physical,
    VkDevice device, std::uint32_t width, std::uint32_t height)
    : device_ { device }
    , extent_ { width, height }
{
    try {
        VkExternalMemoryImageCreateInfo external {
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO
        };
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo image { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        image.pNext = &external;
        image.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_R8G8B8A8_UNORM;
        image.extent = { width, height, 1 };
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = usage;
        check(vkCreateImage(device_, &image, nullptr, &image_),
            "create external program image");
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device_, image_, &requirements);
        VkPhysicalDeviceMemoryProperties properties;
        vkGetPhysicalDeviceMemoryProperties(physical, &properties);
        std::uint32_t type = 0;
        while (type < properties.memoryTypeCount &&
               (!(requirements.memoryTypeBits & (1U << type)) ||
                   !(properties.memoryTypes[type].propertyFlags &
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
            ++type;
        if (type == properties.memoryTypeCount)
            throw std::runtime_error {
                "external program image has no device-local memory"
            };
        VkExportMemoryAllocateInfo exported {
            VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO
        };
        exported.handleTypes = external.handleTypes;
        VkMemoryDedicatedAllocateInfo dedicated {
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO
        };
        dedicated.pNext = &exported;
        dedicated.image = image_;
        VkMemoryAllocateInfo memory { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        memory.pNext = &dedicated;
        memory.allocationSize = size_ = requirements.size;
        memory.memoryTypeIndex = type;
        check(vkAllocateMemory(device_, &memory, nullptr, &memory_),
            "allocate external program memory");
        check(vkBindImageMemory(device_, image_, memory_, 0),
            "bind external program memory");
    } catch (...) {
        destroy();
        throw;
    }
}
void VulkanFdProgramAllocation::destroy()
{
    if (image_)
        vkDestroyImage(device_, image_, nullptr);
    if (memory_)
        vkFreeMemory(device_, memory_, nullptr);
}
VulkanFdProgramAllocation::~VulkanFdProgramAllocation() { destroy(); }
int VulkanFdProgramAllocation::export_fd() const
{
    VkMemoryGetFdInfoKHR info { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    info.memory = memory_;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd;
    const auto export_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(device_, "vkGetMemoryFdKHR"));
    check(export_fd(device_, &info, &fd), "export program image memory");
    return fd;
}
void VulkanFdProgramAllocation::encode(VkCommandBuffer command,
    VkImage canonical, std::uint32_t queue_family, bool inverted_vertical,
    bool publishing) const
{
    encode_program_transfer(command, canonical, image_, extent_, queue_family,
        VK_QUEUE_FAMILY_EXTERNAL, false, inverted_vertical, publishing);
}
std::shared_ptr<GlesProgramTarget> make_fd_program_target(
    std::shared_ptr<GlesRenderer> owner, std::shared_ptr<HostSurface> surface,
    GlesProgramTarget::DeviceIdentity device,
    std::shared_ptr<VulkanFdProgramAllocation> allocation,
    std::shared_ptr<VulkanFdSemaphore> ready,
    std::function<std::shared_ptr<VulkanFdSemaphore>(
        std::shared_ptr<VulkanFdSemaphore>)>
        publish)
{
    return std::make_shared<SharedTarget>(std::move(owner), std::move(surface),
        device, std::move(allocation), std::move(ready), std::move(publish));
}
}
