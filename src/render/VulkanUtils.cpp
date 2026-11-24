#include "eng/render/VulkanUtils.h"

#include "eng/render/VulkanContext.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace eng {

namespace vkutil {

void createBuffer(const VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
                   VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(context.device(), &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        throw std::runtime_error("failed to create buffer");
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(context.device(), buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = context.findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(context.device(), &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate buffer memory");
    }

    vkBindBufferMemory(context.device(), buffer, memory, 0);
}

void createImage(const VulkanContext& context, uint32_t width, uint32_t height, VkFormat format,
                  VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                  VkImage& image, VkDeviceMemory& memory, uint32_t mipLevels, VkSampleCountFlagBits samples) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = samples;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(context.device(), &imageInfo, nullptr, &image) != VK_SUCCESS) {
        throw std::runtime_error("failed to create image");
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(context.device(), image, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = context.findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(context.device(), &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate image memory");
    }

    vkBindImageMemory(context.device(), image, memory, 0);
}

VkImageView createImageView(const VulkanContext& context, VkImage image, VkFormat format,
                             VkImageAspectFlags aspectFlags, uint32_t mipLevels) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView imageView;
    if (vkCreateImageView(context.device(), &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
        throw std::runtime_error("failed to create image view");
    }

    return imageView;
}

VkCommandBuffer beginSingleTimeCommands(const VulkanContext& context, VkCommandPool commandPool) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(context.device(), &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    return commandBuffer;
}

void endSingleTimeCommands(const VulkanContext& context, VkCommandPool commandPool, VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(context.graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(context.graphicsQueue());

    vkFreeCommandBuffers(context.device(), commandPool, 1, &commandBuffer);
}

void copyBuffer(const VulkanContext& context, VkCommandPool commandPool, VkBuffer src, VkBuffer dst, VkDeviceSize size) {
    VkCommandBuffer commandBuffer = beginSingleTimeCommands(context, commandPool);

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, src, dst, 1, &copyRegion);

    endSingleTimeCommands(context, commandPool, commandBuffer);
}

// ── Texture helpers ──────────────────────────────────────────────────────────

static void transitionImageLayout(const VulkanContext& context, VkCommandPool commandPool,
                                   VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkCommandBuffer cmd = beginSingleTimeCommands(context, commandPool);

    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout           = oldLayout;
    barrier.newLayout           = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = image;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkPipelineStageFlags srcStage, dstStage;
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
        newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    endSingleTimeCommands(context, commandPool, cmd);
}

// Blits mip N-1 down into mip N for every level after the base, leaving
// every mip in SHADER_READ_ONLY_OPTIMAL — called only when the format
// actually supports a linearly-filtered blit (see createTextureFromPixels'
// fallback when it doesn't).
static void generateMipmaps(const VulkanContext& context, VkCommandPool commandPool, VkImage image,
                             int32_t width, int32_t height, uint32_t mipLevels) {
    VkCommandBuffer cmd = beginSingleTimeCommands(context, commandPool);

    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image               = image;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;
    barrier.subresourceRange.levelCount     = 1;

    int32_t mipW = width, mipH = height;
    for (uint32_t i = 1; i < mipLevels; ++i) {
        // Mip i-1 was written (as TRANSFER_DST, by the base copy or the
        // previous loop iteration) — flip it to TRANSFER_SRC so this
        // iteration can blit from it.
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                              0, nullptr, 0, nullptr, 1, &barrier);

        int32_t nextW = std::max(mipW / 2, 1);
        int32_t nextH = std::max(mipH / 2, 1);
        VkImageBlit blit{};
        blit.srcOffsets[1] = {mipW, mipH, 1};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
        blit.dstOffsets[1] = {nextW, nextH, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        // Done reading mip i-1 — leave it shader-readable for good.
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                              0, nullptr, 0, nullptr, 1, &barrier);

        mipW = nextW;
        mipH = nextH;
    }

    // The last mip was only ever a blit destination, never a source —
    // the loop above never flips it out of TRANSFER_DST.
    barrier.subresourceRange.baseMipLevel = mipLevels - 1;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                          0, nullptr, 0, nullptr, 1, &barrier);

    endSingleTimeCommands(context, commandPool, cmd);
}

TextureResource createTextureFromPixels(const VulkanContext& context, VkCommandPool commandPool,
                                        const uint8_t* pixels, uint32_t width, uint32_t height,
                                        bool generateMips, bool linearFilter) {
    VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4;

    VkBuffer stagingBuf;
    VkDeviceMemory stagingMem;
    createBuffer(context, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 stagingBuf, stagingMem);
    void* data;
    vkMapMemory(context.device(), stagingMem, 0, imageSize, 0, &data);
    std::memcpy(data, pixels, static_cast<size_t>(imageSize));
    vkUnmapMemory(context.device(), stagingMem);

    // Only bother with a mip chain if the format/device can actually
    // produce one with a linearly-filtered blit — true on essentially
    // every desktop GPU for VK_FORMAT_R8G8B8A8_UNORM, but checked rather
    // than assumed so a missing feature degrades to today's single-mip
    // behavior instead of hitting a validation error.
    VkFormatProperties formatProps{};
    vkGetPhysicalDeviceFormatProperties(context.physicalDevice(), VK_FORMAT_R8G8B8A8_UNORM, &formatProps);
    bool canBlitLinear = (formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    uint32_t mipLevels = (generateMips && canBlitLinear)
        ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1
        : 1;

    TextureResource res;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (mipLevels > 1) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // each mip blits from the one above it
    createImage(context, width, height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL, usage,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, res.image, res.memory, mipLevels);

    transitionImageLayout(context, commandPool, res.image,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkCommandBuffer cmd = beginSingleTimeCommands(context, commandPool);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuf, res.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    endSingleTimeCommands(context, commandPool, cmd);

    if (mipLevels > 1)
        generateMipmaps(context, commandPool, res.image, static_cast<int32_t>(width), static_cast<int32_t>(height), mipLevels);
    else
        transitionImageLayout(context, commandPool, res.image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(context.device(), stagingBuf, nullptr);
    vkFreeMemory(context.device(), stagingMem, nullptr);

    res.view = createImageView(context, res.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);

    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = linearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;  // nearest: crisp up close, matches the low-poly art style
    // LINEAR (not NEAREST) minification + a real mip chain: distant floor
    // tiles/textures sample a properly downsampled mip instead of
    // skipping pixels in the full-res one, which is what caused the
    // shimmer/moire this was added to fix. Falls back to NEAREST when
    // mipLevels is 1 (generateMips=false, or the format/device couldn't
    // blit) so nothing changes for UI art.
    si.minFilter    = mipLevels > 1 || linearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.mipmapMode   = mipLevels > 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.minLod       = 0.0f;
    si.maxLod       = static_cast<float>(mipLevels);
    if (vkCreateSampler(context.device(), &si, nullptr, &res.sampler) != VK_SUCCESS)
        throw std::runtime_error("failed to create texture sampler");

    return res;
}

void destroyTexture(const VulkanContext& context, TextureResource& res) {
    VkDevice dev = context.device();
    if (res.sampler) { vkDestroySampler(dev, res.sampler, nullptr);    res.sampler = VK_NULL_HANDLE; }
    if (res.view)    { vkDestroyImageView(dev, res.view, nullptr);     res.view    = VK_NULL_HANDLE; }
    if (res.image)   { vkDestroyImage(dev, res.image, nullptr);        res.image   = VK_NULL_HANDLE; }
    if (res.memory)  { vkFreeMemory(dev, res.memory, nullptr);         res.memory  = VK_NULL_HANDLE; }
}

}  // namespace vkutil

}  // namespace eng
