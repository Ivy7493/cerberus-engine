#pragma once

#include <vulkan/vulkan.h>

#include <vector>

struct GLFWwindow;

namespace eng {

class VulkanContext;

// Owns the swapchain and its image views. Can be recreated in place
// when the window is resized.
class Swapchain {
public:
    // `msaaSamples` sizes the depth buffer (the only swapchain-owned
    // resource MSAA touches — the multisampled color target lives on
    // Pipeline, since that's what owns the framebuffers it's attached to)
    // — fixed for this Swapchain's lifetime, including recreate().
    //
    // `srgb` picks an sRGB surface format (correct for a lit 3D renderer
    // writing linear color) vs plain UNORM (what a 2D sprite renderer wants:
    // texture/vertex colors pass through untouched, and Dear ImGui's colors
    // aren't washed out). `depth` skips the depth buffer entirely for
    // renderers that never depth-test.
    Swapchain(VulkanContext& context, GLFWwindow* window,
              VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT,
              bool srgb = true, bool depth = true);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    void recreate();

    VkSwapchainKHR handle() const { return m_swapchain; }
    VkFormat imageFormat() const { return m_imageFormat; }
    VkExtent2D extent() const { return m_extent; }
    const std::vector<VkImageView>& imageViews() const { return m_imageViews; }
    const std::vector<VkImage>& images() const { return m_images; }
    bool supportsReadback() const { return m_transferSrc; }
    size_t imageCount() const { return m_images.size(); }

    VkImageView depthImageView() const { return m_depthImageView; }
    VkFormat depthFormat() const { return m_depthFormat; }

private:
    void create();
    void cleanup();
    void createDepthResources();

    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes) const;
    VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;

    VulkanContext& m_context;
    GLFWwindow* m_window;
    VkSampleCountFlagBits m_msaaSamples;
    bool m_srgb;
    bool m_depth;

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_imageViews;
    VkFormat m_imageFormat = VK_FORMAT_UNDEFINED;
    bool m_transferSrc = false;
    VkExtent2D m_extent{};

    VkImage m_depthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_depthImageMemory = VK_NULL_HANDLE;
    VkImageView m_depthImageView = VK_NULL_HANDLE;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
};

}  // namespace eng
