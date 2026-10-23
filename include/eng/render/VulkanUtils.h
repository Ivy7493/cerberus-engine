#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

namespace eng {

class VulkanContext;

// Small free-function helpers for the recurring buffer/image/command
// boilerplate that Vulkan requires.
namespace vkutil {

void createBuffer(const VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
                   VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory);

// `mipLevels`/`samples` default to the plain single-mip, single-sample case
// every existing caller (shadow maps, point-shadow targets, depth buffers)
// wants — only texture loading and the MSAA color/depth targets pass
// anything else.
void createImage(const VulkanContext& context, uint32_t width, uint32_t height, VkFormat format,
                  VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                  VkImage& image, VkDeviceMemory& memory,
                  uint32_t mipLevels = 1, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);

VkImageView createImageView(const VulkanContext& context, VkImage image, VkFormat format,
                             VkImageAspectFlags aspectFlags, uint32_t mipLevels = 1);

VkCommandBuffer beginSingleTimeCommands(const VulkanContext& context, VkCommandPool commandPool);
void endSingleTimeCommands(const VulkanContext& context, VkCommandPool commandPool, VkCommandBuffer commandBuffer);

void copyBuffer(const VulkanContext& context, VkCommandPool commandPool, VkBuffer src, VkBuffer dst, VkDeviceSize size);

// ── Texture helpers ──────────────────────────────────────────────────────────

struct TextureResource {
    VkImage        image   = VK_NULL_HANDLE;
    VkDeviceMemory memory  = VK_NULL_HANDLE;
    VkImageView    view    = VK_NULL_HANDLE;
    VkSampler      sampler = VK_NULL_HANDLE;
};

// Uploads raw RGBA pixel data to a device-local VkImage and creates a
// sampler. `generateMips` builds a full mip chain and uses linear
// minification (still nearest at magnification, to keep close-up textures
// crisp) — on by default for world/character textures, where it cuts down
// shimmer on distant floor tiles; HudCanvas passes false for UI art, which
// is always drawn at a fixed/known scale and has no "far away" case.
TextureResource createTextureFromPixels(const VulkanContext& context, VkCommandPool commandPool,
                                        const uint8_t* pixels, uint32_t width, uint32_t height,
                                        bool generateMips = true);

void destroyTexture(const VulkanContext& context, TextureResource& res);

}  // namespace vkutil

}  // namespace eng
