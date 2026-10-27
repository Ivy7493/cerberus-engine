#pragma once

#include "eng/render/VulkanUtils.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ImFont;
struct ImGuiStyle;

namespace eng {

class Window;
class VulkanContext;
class Swapchain;

using TextureId = uint32_t;

// Modulate2x: dst × src × 2, so mid-grey (0.5) leaves what's under it as it
// was, darker greys darken and lighter ones brighten (up to double).
enum class BlendMode : uint8_t { Alpha, Additive, Modulate2x };
enum class Space : uint8_t { World, Screen };

// One textured, tinted, optionally rotated quad. `pos` is the quad's center.
struct Sprite {
    TextureId texture = 0;  // 0 = Renderer2D::kWhite
    glm::vec2 pos{0.0f};
    glm::vec2 size{1.0f};
    float rotation = 0.0f;  // radians, clockwise in y-down space
    glm::vec4 uv{0.0f, 0.0f, 1.0f, 1.0f};  // u0, v0, u1, v1 — swap to flip
    glm::vec4 color{1.0f};
    BlendMode blend = BlendMode::Alpha;
    Space space = Space::World;
};

// Immediate-mode 2D sprite renderer + Dear ImGui overlay on one render pass.
//
// Coordinates are y-down in both spaces. Screen space is framebuffer
// pixels; world space is whatever unit the game picks, mapped to the screen
// by setCamera(). Sprites draw in submission order (painter's algorithm) —
// the game sorts by depth itself — and consecutive sprites sharing a
// texture/blend/space collapse into one draw call.
//
// Per frame: beginFrame() → draw*() / ImGui::* calls → endFrame().
class Renderer2D {
public:
    struct Options {
        std::string appName = "eng";
        // TTF used as ImGui's default font; falls back to ImGui's built-in
        // bitmap font if empty or missing.
        std::string fontPath;
        float fontSize = 22.0f;
        // Optional second, larger font (titles/big HUD numbers).
        std::string titleFontPath;
        float titleFontSize = 56.0f;
    };

    // Built-in textures, always present.
    static constexpr TextureId kWhite = 0;   // 1x1 white
    static constexpr TextureId kCircle = 1;  // soft-edged white disc
    static constexpr TextureId kGlow = 2;    // radial falloff, for big additive glows (smooth when magnified)

    Renderer2D(Window& window, const Options& options);
    ~Renderer2D();

    Renderer2D(const Renderer2D&) = delete;
    Renderer2D& operator=(const Renderer2D&) = delete;

    // PNG (or anything stb_image reads). Returns kWhite and logs on failure,
    // so a missing art file shows as a white box rather than crashing.
    TextureId loadTexture(const std::string& path);
    TextureId createTexture(const uint8_t* rgba, uint32_t width, uint32_t height);
    glm::ivec2 textureSize(TextureId id) const;
    // For ImGui::Image(...) — same descriptor set the sprite pipeline binds.
    void* imguiTexture(TextureId id) const;

    // Returns false if this frame should be skipped (minimized window /
    // swapchain being recreated) — don't call draw*/endFrame in that case.
    bool beginFrame();
    void endFrame();

    void setClearColor(const glm::vec4& c) { m_clearColor = c; }

    // World view: `center` lands mid-screen, `pixelsPerUnit` scales world
    // units to framebuffer pixels.
    void setCamera(const glm::vec2& center, float pixelsPerUnit);
    glm::vec2 cameraCenter() const { return m_camCenter; }
    float pixelsPerUnit() const { return m_pixelsPerUnit; }
    glm::vec2 worldToScreen(const glm::vec2& world) const;
    glm::vec2 screenToWorld(const glm::vec2& screen) const;
    glm::vec2 screenSize() const;

    void draw(const Sprite& sprite);
    void drawRect(const glm::vec2& center, const glm::vec2& size, const glm::vec4& color,
                  Space space = Space::World);
    void drawCircle(const glm::vec2& center, float radius, const glm::vec4& color,
                    Space space = Space::World, BlendMode blend = BlendMode::Alpha);
    // Soft radial glow (kGlow), additive.
    void drawGlow(const glm::vec2& center, float radius, const glm::vec4& color, Space space = Space::World);
    void drawLine(const glm::vec2& a, const glm::vec2& b, float thickness, const glm::vec4& color,
                  Space space = Space::World, BlendMode blend = BlendMode::Alpha);
    // One textured triangle (`uv` per corner), for fans and arbitrary polygons.
    void drawTriangle(const glm::vec2 (&p)[3], const glm::vec2 (&uv)[3], const glm::vec4& color,
                      TextureId texture = kWhite, Space space = Space::World, BlendMode blend = BlendMode::Alpha);

    // ── Lighting ──
    // A screen-covering lightmap (half resolution, linearly filtered),
    // cleared to `ambient` each frame. Between beginLights() and endLights()
    // every draw*() goes into the lightmap instead of the scene — Additive is
    // what lights want. compositeLights() then Modulate2x's the lightmap over
    // everything drawn so far (ambient 0.5 = unchanged); whatever is drawn
    // after it stays unlit. A frame that never composites skips the pass.
    void beginLights(const glm::vec4& ambient);
    void endLights();
    void compositeLights();

    ImFont* titleFont() const { return m_titleFont; }
    // Pixel size the title font renders at right now (font size x UI scale).
    float titleFontSize() const;

    // UI scale (1 = authored at 1080p). Scales ImGui fonts and style sizes;
    // fonts are rasterized at kFontOversample x so scaling up stays crisp.
    // Call captureBaseStyle() once after customizing ImGui's style, then
    // setUiScale() whenever the window size changes.
    static constexpr float kFontOversample = 2.0f;
    void captureBaseStyle();
    void setUiScale(float scale);
    float uiScale() const { return m_uiScale; }

    // Writes the next presented frame (sprites + ImGui) to `path` as PNG.
    void requestScreenshot(const std::string& path) { m_screenshotPath = path; }

    VulkanContext& context() { return *m_context; }

private:
    struct Vertex {
        glm::vec2 pos;
        glm::vec2 uv;
        glm::vec4 color;
    };
    struct Batch {
        TextureId texture;
        BlendMode blend;
        Space space;
        uint32_t firstIndex;
        uint32_t indexCount;
    };
    struct Texture {
        vkutil::TextureResource res;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
    };
    // One per frame in flight, so a frame never draws into the lightmap the
    // previous one may still be sampling.
    struct Lightmap {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        TextureId texture = 0;  // its m_textures slot (descriptor only; resources live here)
    };
    struct FrameResources {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
        void* vertexMapped = nullptr;
    };

    static constexpr int kFramesInFlight = 2;
    static constexpr uint32_t kMaxSprites = 65536;  // lights' shadow fans take a share

    void createRenderPass();
    void createFramebuffers();
    void destroyFramebuffers();
    void createPipelines();
    VkPipeline createPipeline(VkRenderPass pass, BlendMode blend);
    void createLightmaps();
    void destroyLightmaps();
    void pushVertices(const Vertex* v, TextureId texture, BlendMode blend, Space space);
    void recordBatches(VkCommandBuffer cmd, const std::vector<Batch>& batches, bool lightPass);
    void createFrameResources();
    void createIndexBuffer();
    void initImGui(const Options& options);
    void recreateSwapchain();
    void createBuiltinTextures();
    void captureScreenshot();

    Window& m_window;
    std::unique_ptr<VulkanContext> m_context;
    std::unique_ptr<Swapchain> m_swapchain;

    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;
    // One per swapchain image (not per frame in flight): a present can still
    // be reading the semaphore after the frame's fence signals.
    std::vector<VkSemaphore> m_renderFinished;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipelineAlpha = VK_NULL_HANDLE;
    VkPipeline m_pipelineAdditive = VK_NULL_HANDLE;
    VkPipeline m_pipelineModulate = VK_NULL_HANDLE;
    VkShaderModule m_vertModule = VK_NULL_HANDLE;
    VkShaderModule m_fragModule = VK_NULL_HANDLE;

    VkRenderPass m_lightPass = VK_NULL_HANDLE;
    VkPipeline m_lightPipelineAlpha = VK_NULL_HANDLE;
    VkPipeline m_lightPipelineAdditive = VK_NULL_HANDLE;
    VkSampler m_lightSampler = VK_NULL_HANDLE;
    std::array<Lightmap, kFramesInFlight> m_lightmaps{};
    VkExtent2D m_lightExtent{1, 1};
    std::vector<Batch> m_lightBatches;
    glm::vec4 m_lightAmbient{0.5f};
    bool m_inLights = false;
    bool m_lightsUsed = false;
    VkDescriptorPool m_imguiPool = VK_NULL_HANDLE;
    VkBuffer m_indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_indexMemory = VK_NULL_HANDLE;

    std::array<FrameResources, kFramesInFlight> m_frames{};
    uint32_t m_frameIndex = 0;
    uint32_t m_imageIndex = 0;
    bool m_frameActive = false;

    std::vector<Texture> m_textures;
    std::vector<Vertex> m_vertices;
    std::vector<Batch> m_batches;

    glm::vec4 m_clearColor{0.05f, 0.05f, 0.07f, 1.0f};
    glm::vec2 m_camCenter{0.0f};
    float m_pixelsPerUnit = 1.0f;
    ImFont* m_titleFont = nullptr;
    std::string m_screenshotPath;
    float m_uiScale = 1.0f;
    std::unique_ptr<ImGuiStyle> m_baseStyle;
};

}  // namespace eng
