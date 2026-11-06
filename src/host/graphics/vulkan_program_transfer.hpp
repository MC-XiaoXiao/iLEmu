/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <vulkan/vulkan.h>
namespace ilemu {
// Shared by platform allocation adapters; pixels stay on the GPU in both
// directions. Imported Android allocations begin in foreign ownership.
inline void encode_program_transfer(VkCommandBuffer command, VkImage canonical,
    VkImage shared, VkExtent2D extent, std::uint32_t queue_family,
    std::uint32_t external_family, bool imported_allocation,
    bool inverted_vertical, bool publishing)
{
    VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout =
        publishing ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = publishing ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                   : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const bool acquiring = publishing || imported_allocation;
    barrier.srcQueueFamilyIndex =
        acquiring ? external_family : VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex =
        acquiring ? queue_family : VK_QUEUE_FAMILY_IGNORED;
    barrier.dstAccessMask =
        publishing ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image = shared;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkImageBlit blit { };
    blit.srcSubresource =
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1] = { static_cast<std::int32_t>(extent.width),
        static_cast<std::int32_t>(extent.height), 1 };
    blit.dstOffsets[0] = { 0,
        inverted_vertical ? 0 : static_cast<std::int32_t>(extent.height), 0 };
    blit.dstOffsets[1] = { static_cast<std::int32_t>(extent.width),
        inverted_vertical ? static_cast<std::int32_t>(extent.height) : 0, 1 };
    vkCmdBlitImage(command, publishing ? shared : canonical,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, publishing ? canonical : shared,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    barrier.oldLayout = barrier.newLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = queue_family;
    barrier.dstQueueFamilyIndex = external_family;
    barrier.srcAccessMask = barrier.dstAccessMask;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &barrier);
}
}
