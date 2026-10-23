#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>
#include <vector>

struct GLFWwindow;

namespace eng {


#ifdef NDEBUG
constexpr bool kEnableValidationLayers = false;
#else
constexpr bool kEnableValidationLayers = true;
#endif

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete() const {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapchainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

// Owns the core Vulkan objects: instance, debug messenger, surface,
// physical/logical device and the graphics/present queues.
class VulkanContext {
public:
    // `appName` is reported to the driver (VkApplicationInfo) — purely
    // informational, shows up in tools like RenderDoc.
    VulkanContext(GLFWwindow* window, const char* appName);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    VkInstance instance() const { return m_instance; }
    VkSurfaceKHR surface() const { return m_surface; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice device() const { return m_device; }
    VkQueue graphicsQueue() const { return m_graphicsQueue; }
    VkQueue presentQueue() const { return m_presentQueue; }
    const QueueFamilyIndices& queueFamilies() const { return m_queueFamilies; }

    SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device) const;
    SwapchainSupportDetails querySwapchainSupport() const { return querySwapchainSupport(m_physicalDevice); }

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    VkFormat findDepthFormat() const;

    // Highest sample count both color and depth attachments support on
    // this GPU, capped at 4x — diminishing returns past that for this
    // low-poly art style don't justify the extra bandwidth/memory cost.
    // Callers combine this with GraphicsSettings::msaaEnabled to decide
    // the sample count Swapchain/Pipeline are actually built with.
    VkSampleCountFlagBits maxUsableSampleCount() const;

private:
    void createInstance(const char* appName);
    void setupDebugMessenger();
    void createSurface(GLFWwindow* window);
    void pickPhysicalDevice();
    void createLogicalDevice();

    VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling,
                                  VkFormatFeatureFlags features) const;

    bool isDeviceSuitable(VkPhysicalDevice device) const;
    bool checkDeviceExtensionSupport(VkPhysicalDevice device) const;
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;

    bool checkValidationLayerSupport() const;
    std::vector<const char*> getRequiredExtensions() const;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue m_presentQueue = VK_NULL_HANDLE;
    QueueFamilyIndices m_queueFamilies;
    bool m_validationLayersEnabled = false;
};

}  // namespace eng
