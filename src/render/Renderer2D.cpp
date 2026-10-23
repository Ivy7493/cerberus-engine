#include "eng/render/Renderer2D.h"

#include "eng/core/Log.h"
#include "eng/platform/Window.h"
#include "eng/render/Swapchain.h"
#include "eng/render/VulkanContext.h"
#include "eng/render/VulkanUtils.h"

#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace eng {

namespace {

// SPIR-V compiled at build time (glslc -mfmt=c, see engine CMakeLists.txt).
const uint32_t kSpriteVertSpv[] =
#include "sprite.vert.inc"
    ;
const uint32_t kSpriteFragSpv[] =
#include "sprite.frag.inc"
    ;

struct PushConstants {
    glm::vec2 scale;
    glm::vec2 offset;
};

VkShaderModule createShaderModule(VkDevice device, const uint32_t* code, size_t sizeBytes) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = sizeBytes;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
        throw std::runtime_error("failed to create shader module");
    return module;
}

}  // namespace

Renderer2D::Renderer2D(Window& window, const Options& options) : m_window(window) {
    m_context = std::make_unique<VulkanContext>(window.handle(), options.appName.c_str());
    m_swapchain = std::make_unique<Swapchain>(*m_context, window.handle(), VK_SAMPLE_COUNT_1_BIT,
                                              /*srgb=*/false, /*depth=*/false);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_context->queueFamilies().graphicsFamily.value();
    if (vkCreateCommandPool(m_context->device(), &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create command pool");

    createRenderPass();
    createFramebuffers();
    createPipelines();
    createFrameResources();
    createIndexBuffer();
    initImGui(options);
    createBuiltinTextures();

    m_vertices.reserve(kMaxSprites * 4);
}

Renderer2D::~Renderer2D() {
    VkDevice dev = m_context->device();
    vkDeviceWaitIdle(dev);

    for (auto& tex : m_textures) {
        ImGui_ImplVulkan_RemoveTexture(tex.descriptor);
        vkutil::destroyTexture(*m_context, tex.res);
    }
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(dev, m_imguiPool, nullptr);

    for (auto& f : m_frames) {
        vkDestroySemaphore(dev, f.imageAvailable, nullptr);
        vkDestroyFence(dev, f.inFlight, nullptr);
        vkDestroyBuffer(dev, f.vertexBuffer, nullptr);
        vkFreeMemory(dev, f.vertexMemory, nullptr);
    }
    vkDestroyBuffer(dev, m_indexBuffer, nullptr);
    vkFreeMemory(dev, m_indexMemory, nullptr);
    vkDestroyPipeline(dev, m_pipelineAlpha, nullptr);
    vkDestroyPipeline(dev, m_pipelineAdditive, nullptr);
    vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(dev, m_textureSetLayout, nullptr);
    destroyFramebuffers();
    vkDestroyRenderPass(dev, m_renderPass, nullptr);
    vkDestroyCommandPool(dev, m_commandPool, nullptr);
    m_swapchain.reset();
    m_context.reset();
}

// ── Setup ────────────────────────────────────────────────────────────────────

void Renderer2D::createRenderPass() {
    VkAttachmentDescription color{};
    color.format = m_swapchain->imageFormat();
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dep;
    if (vkCreateRenderPass(m_context->device(), &info, nullptr, &m_renderPass) != VK_SUCCESS)
        throw std::runtime_error("failed to create render pass");
}

void Renderer2D::createFramebuffers() {
    VkDevice dev = m_context->device();
    const auto& views = m_swapchain->imageViews();
    m_framebuffers.resize(views.size());
    for (size_t i = 0; i < views.size(); ++i) {
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = m_renderPass;
        info.attachmentCount = 1;
        info.pAttachments = &views[i];
        info.width = m_swapchain->extent().width;
        info.height = m_swapchain->extent().height;
        info.layers = 1;
        if (vkCreateFramebuffer(dev, &info, nullptr, &m_framebuffers[i]) != VK_SUCCESS)
            throw std::runtime_error("failed to create framebuffer");
    }

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    m_renderFinished.resize(views.size());
    for (auto& sem : m_renderFinished)
        if (vkCreateSemaphore(dev, &semInfo, nullptr, &sem) != VK_SUCCESS)
            throw std::runtime_error("failed to create semaphore");
}

void Renderer2D::destroyFramebuffers() {
    VkDevice dev = m_context->device();
    for (auto fb : m_framebuffers) vkDestroyFramebuffer(dev, fb, nullptr);
    for (auto sem : m_renderFinished) vkDestroySemaphore(dev, sem, nullptr);
    m_framebuffers.clear();
    m_renderFinished.clear();
}

void Renderer2D::createPipelines() {
    VkDevice dev = m_context->device();

    // Must stay identical to Dear ImGui's own texture set layout (one
    // combined image sampler, binding 0, fragment stage): sprite textures
    // get their descriptor sets from ImGui_ImplVulkan_AddTexture, and
    // Vulkan only allows binding a set against an identically-defined layout.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 1;
    setInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(dev, &setInfo, nullptr, &m_textureSetLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create descriptor set layout");

    VkPushConstantRange pushRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_textureSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create pipeline layout");

    VkShaderModule vert = createShaderModule(dev, kSpriteVertSpv, sizeof(kSpriteVertSpv));
    VkShaderModule frag = createShaderModule(dev, kSpriteFragSpv, sizeof(kSpriteFragSpv));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vbind{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, pos)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &vbind;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;  // flipped sprites (swapped uvs/negative size) must still draw
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blendState{};
    blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blendState.attachmentCount = 1;
    blendState.pAttachments = &blend;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blendState;
    info.pDynamicState = &dynamic;
    info.layout = m_pipelineLayout;
    info.renderPass = m_renderPass;
    info.subpass = 0;

    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &m_pipelineAlpha) != VK_SUCCESS)
        throw std::runtime_error("failed to create sprite pipeline");

    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &m_pipelineAdditive) != VK_SUCCESS)
        throw std::runtime_error("failed to create additive sprite pipeline");

    vkDestroyShaderModule(dev, vert, nullptr);
    vkDestroyShaderModule(dev, frag, nullptr);
}

void Renderer2D::createFrameResources() {
    VkDevice dev = m_context->device();

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = m_commandPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkDeviceSize vbSize = sizeof(Vertex) * kMaxSprites * 4;
    for (auto& f : m_frames) {
        if (vkAllocateCommandBuffers(dev, &alloc, &f.cmd) != VK_SUCCESS ||
            vkCreateSemaphore(dev, &semInfo, nullptr, &f.imageAvailable) != VK_SUCCESS ||
            vkCreateFence(dev, &fenceInfo, nullptr, &f.inFlight) != VK_SUCCESS)
            throw std::runtime_error("failed to create frame resources");
        vkutil::createBuffer(*m_context, vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             f.vertexBuffer, f.vertexMemory);
        vkMapMemory(dev, f.vertexMemory, 0, vbSize, 0, &f.vertexMapped);
    }
}

void Renderer2D::createIndexBuffer() {
    std::vector<uint32_t> indices(kMaxSprites * 6);
    for (uint32_t i = 0; i < kMaxSprites; ++i) {
        uint32_t v = i * 4;
        uint32_t* q = &indices[i * 6];
        q[0] = v; q[1] = v + 1; q[2] = v + 2;
        q[3] = v + 2; q[4] = v + 3; q[5] = v;
    }
    VkDeviceSize size = indices.size() * sizeof(uint32_t);
    vkutil::createBuffer(*m_context, size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         m_indexBuffer, m_indexMemory);
    void* data = nullptr;
    vkMapMemory(m_context->device(), m_indexMemory, 0, size, 0, &data);
    std::memcpy(data, indices.data(), size);
    vkUnmapMemory(m_context->device(), m_indexMemory);
}

void Renderer2D::initImGui(const Options& options) {
    // Sized for every sprite texture too (see createPipelines' layout note) —
    // one combined-image-sampler set per loaded texture plus ImGui's font.
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 2048;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(m_context->device(), &poolInfo, nullptr, &m_imguiPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create ImGui descriptor pool");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini littering the working directory
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    // Rasterized at kFontOversample x and scaled back down via
    // FontGlobalScale (see setUiScale), so large UI scales stay sharp.
    float body = options.fontSize * kFontOversample, title = options.titleFontSize * kFontOversample;
    if (!options.fontPath.empty() && std::filesystem::exists(options.fontPath)) {
        io.Fonts->AddFontFromFileTTF(options.fontPath.c_str(), body);
    } else {
        ImFontConfig cfg;
        cfg.SizePixels = body;
        io.Fonts->AddFontDefault(&cfg);
    }
    if (!options.titleFontPath.empty() && std::filesystem::exists(options.titleFontPath))
        m_titleFont = io.Fonts->AddFontFromFileTTF(options.titleFontPath.c_str(), title);
    if (!m_titleFont) {
        ImFontConfig cfg;
        cfg.SizePixels = title;
        m_titleFont = io.Fonts->AddFontDefault(&cfg);
    }
    io.FontGlobalScale = 1.0f / kFontOversample;

    ImGui_ImplGlfw_InitForVulkan(m_window.handle(), /*install_callbacks=*/true);

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance = m_context->instance();
    initInfo.PhysicalDevice = m_context->physicalDevice();
    initInfo.Device = m_context->device();
    initInfo.QueueFamily = m_context->queueFamilies().graphicsFamily.value();
    initInfo.Queue = m_context->graphicsQueue();
    initInfo.DescriptorPool = m_imguiPool;
    initInfo.RenderPass = m_renderPass;
    initInfo.MinImageCount = kFramesInFlight;
    initInfo.ImageCount = static_cast<uint32_t>(m_swapchain->imageCount());
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&initInfo);
    // Font texture uploads automatically on the first ImGui_ImplVulkan_NewFrame().
}

void Renderer2D::createBuiltinTextures() {
    const uint8_t white[4] = {255, 255, 255, 255};
    TextureId w = createTexture(white, 1, 1);

    constexpr int kSize = 64;
    std::vector<uint8_t> disc(kSize * kSize * 4);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            float dx = (x + 0.5f) / kSize * 2.0f - 1.0f;
            float dy = (y + 0.5f) / kSize * 2.0f - 1.0f;
            float d = std::sqrt(dx * dx + dy * dy);
            // ~1.5px anti-aliased rim rather than a hard pixel staircase.
            float a = std::clamp((1.0f - d) * kSize * 0.33f, 0.0f, 1.0f);
            uint8_t* p = &disc[(y * kSize + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<uint8_t>(a * 255.0f);
        }
    }
    TextureId c = createTexture(disc.data(), kSize, kSize);
    if (w != kWhite || c != kCircle) throw std::logic_error("builtin texture ids out of order");
}

// ── UI scale ─────────────────────────────────────────────────────────────────

float Renderer2D::titleFontSize() const { return m_titleFont ? m_titleFont->FontSize * ImGui::GetIO().FontGlobalScale : 0.0f; }

void Renderer2D::captureBaseStyle() {
    m_baseStyle = std::make_unique<ImGuiStyle>(ImGui::GetStyle());
    setUiScale(m_uiScale);
}

void Renderer2D::setUiScale(float scale) {
    m_uiScale = scale;
    ImGui::GetIO().FontGlobalScale = scale / kFontOversample;
    if (m_baseStyle) {
        ImGuiStyle style = *m_baseStyle;
        style.ScaleAllSizes(scale);
        ImGui::GetStyle() = style;
    }
}

// ── Textures ─────────────────────────────────────────────────────────────────

TextureId Renderer2D::createTexture(const uint8_t* rgba, uint32_t width, uint32_t height) {
    Texture tex;
    // No mips: pixel art is drawn at integer-ish scales and should stay
    // nearest-filtered at every size.
    tex.res = vkutil::createTextureFromPixels(*m_context, m_commandPool, rgba, width, height,
                                              /*generateMips=*/false);
    tex.descriptor = ImGui_ImplVulkan_AddTexture(tex.res.sampler, tex.res.view,
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    tex.width = width;
    tex.height = height;
    m_textures.push_back(tex);
    return static_cast<TextureId>(m_textures.size() - 1);
}

TextureId Renderer2D::loadTexture(const std::string& path) {
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels) {
        ENG_LOG("[render] failed to load texture '" << path << "': " << stbi_failure_reason());
        return kWhite;
    }
    TextureId id = createTexture(pixels, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    stbi_image_free(pixels);
    return id;
}

glm::ivec2 Renderer2D::textureSize(TextureId id) const {
    if (id >= m_textures.size()) return {0, 0};
    return {m_textures[id].width, m_textures[id].height};
}

void* Renderer2D::imguiTexture(TextureId id) const {
    if (id >= m_textures.size()) id = kWhite;
    return reinterpret_cast<void*>(m_textures[id].descriptor);
}

// ── Camera ───────────────────────────────────────────────────────────────────

void Renderer2D::setCamera(const glm::vec2& center, float pixelsPerUnit) {
    m_camCenter = center;
    m_pixelsPerUnit = pixelsPerUnit;
}

glm::vec2 Renderer2D::screenSize() const {
    VkExtent2D e = m_swapchain->extent();
    return {static_cast<float>(e.width), static_cast<float>(e.height)};
}

glm::vec2 Renderer2D::worldToScreen(const glm::vec2& world) const {
    return (world - m_camCenter) * m_pixelsPerUnit + screenSize() * 0.5f;
}

glm::vec2 Renderer2D::screenToWorld(const glm::vec2& screen) const {
    return (screen - screenSize() * 0.5f) / m_pixelsPerUnit + m_camCenter;
}

// ── Drawing ──────────────────────────────────────────────────────────────────

void Renderer2D::draw(const Sprite& s) {
    if (m_vertices.size() + 4 > kMaxSprites * 4) return;  // silently cap; never overrun the mapped buffer

    glm::vec2 h = s.size * 0.5f;
    glm::vec2 corners[4] = {{-h.x, -h.y}, {h.x, -h.y}, {h.x, h.y}, {-h.x, h.y}};
    if (s.rotation != 0.0f) {
        float c = std::cos(s.rotation), sn = std::sin(s.rotation);
        for (auto& p : corners) p = {p.x * c - p.y * sn, p.x * sn + p.y * c};
    }
    glm::vec2 uvs[4] = {{s.uv.x, s.uv.y}, {s.uv.z, s.uv.y}, {s.uv.z, s.uv.w}, {s.uv.x, s.uv.w}};

    auto firstIndex = static_cast<uint32_t>(m_vertices.size() / 4 * 6);
    for (int i = 0; i < 4; ++i) m_vertices.push_back({s.pos + corners[i], uvs[i], s.color});

    TextureId tex = s.texture < m_textures.size() ? s.texture : kWhite;
    if (!m_batches.empty()) {
        Batch& last = m_batches.back();
        if (last.texture == tex && last.blend == s.blend && last.space == s.space) {
            last.indexCount += 6;
            return;
        }
    }
    m_batches.push_back({tex, s.blend, s.space, firstIndex, 6});
}

void Renderer2D::drawRect(const glm::vec2& center, const glm::vec2& size, const glm::vec4& color, Space space) {
    Sprite s;
    s.pos = center;
    s.size = size;
    s.color = color;
    s.space = space;
    draw(s);
}

void Renderer2D::drawCircle(const glm::vec2& center, float radius, const glm::vec4& color, Space space,
                            BlendMode blend) {
    Sprite s;
    s.texture = kCircle;
    s.pos = center;
    s.size = glm::vec2(radius * 2.0f);
    s.color = color;
    s.space = space;
    s.blend = blend;
    draw(s);
}

void Renderer2D::drawLine(const glm::vec2& a, const glm::vec2& b, float thickness, const glm::vec4& color,
                          Space space, BlendMode blend) {
    glm::vec2 d = b - a;
    float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len <= 0.0f) return;
    Sprite s;
    s.pos = (a + b) * 0.5f;
    s.size = {len, thickness};
    s.rotation = std::atan2(d.y, d.x);
    s.color = color;
    s.space = space;
    s.blend = blend;
    draw(s);
}

// ── Frame ────────────────────────────────────────────────────────────────────

void Renderer2D::recreateSwapchain() {
    vkDeviceWaitIdle(m_context->device());
    destroyFramebuffers();
    m_swapchain->recreate();
    createFramebuffers();
    ImGui_ImplVulkan_SetMinImageCount(kFramesInFlight);
}

bool Renderer2D::beginFrame() {
    // Minimized: skip rendering entirely instead of letting
    // Swapchain::recreate() block in glfwWaitEvents — the caller may be a
    // host whose simulation must keep ticking for everyone else.
    if (m_window.framebufferWidth() == 0 || m_window.framebufferHeight() == 0) return false;
    if (m_window.consumeResized()) recreateSwapchain();

    FrameResources& f = m_frames[m_frameIndex];
    VkDevice dev = m_context->device();
    vkWaitForFences(dev, 1, &f.inFlight, VK_TRUE, UINT64_MAX);

    VkResult r = vkAcquireNextImageKHR(dev, m_swapchain->handle(), UINT64_MAX, f.imageAvailable,
                                       VK_NULL_HANDLE, &m_imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) throw std::runtime_error("failed to acquire swapchain image");
    vkResetFences(dev, 1, &f.inFlight);

    m_vertices.clear();
    m_batches.clear();

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    m_frameActive = true;
    return true;
}

void Renderer2D::endFrame() {
    if (!m_frameActive) return;
    m_frameActive = false;
    ImGui::Render();

    FrameResources& f = m_frames[m_frameIndex];
    std::memcpy(f.vertexMapped, m_vertices.data(), m_vertices.size() * sizeof(Vertex));

    VkCommandBuffer cmd = f.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkExtent2D extent = m_swapchain->extent();
    VkClearValue clear{};
    clear.color = {{m_clearColor.r, m_clearColor.g, m_clearColor.b, m_clearColor.a}};
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_renderPass;
    rp.framebuffer = m_framebuffers[m_imageIndex];
    rp.renderArea.extent = extent;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    if (!m_batches.empty()) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &f.vertexBuffer, &offset);
        vkCmdBindIndexBuffer(cmd, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        glm::vec2 size = screenSize();
        PushConstants screenPc{2.0f / size, glm::vec2(-1.0f)};
        PushConstants worldPc{2.0f * m_pixelsPerUnit / size, -2.0f * m_camCenter * m_pixelsPerUnit / size};

        VkPipeline bound = VK_NULL_HANDLE;
        for (const Batch& b : m_batches) {
            VkPipeline p = b.blend == BlendMode::Additive ? m_pipelineAdditive : m_pipelineAlpha;
            if (p != bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                bound = p;
            }
            const PushConstants& pc = b.space == Space::World ? worldPc : screenPc;
            vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1,
                                    &m_textures[b.texture].descriptor, 0, nullptr);
            vkCmdDrawIndexed(cmd, b.indexCount, 1, b.firstIndex, 0, 0);
        }
    }

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphore renderDone = m_renderFinished[m_imageIndex];
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &f.imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderDone;
    if (vkQueueSubmit(m_context->graphicsQueue(), 1, &submit, f.inFlight) != VK_SUCCESS)
        throw std::runtime_error("failed to submit frame");

    if (!m_screenshotPath.empty()) captureScreenshot();

    VkSwapchainKHR swapchain = m_swapchain->handle();
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderDone;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &m_imageIndex;
    VkResult r = vkQueuePresentKHR(m_context->presentQueue(), &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreateSwapchain();
    else if (r != VK_SUCCESS) throw std::runtime_error("failed to present");

    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
}

void Renderer2D::captureScreenshot() {
    std::string path = std::move(m_screenshotPath);
    m_screenshotPath.clear();
    if (!m_swapchain->supportsReadback()) {
        ENG_LOG("[render] screenshot unsupported: swapchain images can't be transfer sources here");
        return;
    }
    // Debug-path simplicity over speed: stall the queue, copy, convert.
    VkDevice dev = m_context->device();
    vkQueueWaitIdle(m_context->graphicsQueue());
    VkExtent2D e = m_swapchain->extent();
    VkDeviceSize size = VkDeviceSize(e.width) * e.height * 4;
    VkBuffer buf;
    VkDeviceMemory mem;
    vkutil::createBuffer(*m_context, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buf, mem);

    VkImage image = m_swapchain->images()[m_imageIndex];
    VkCommandBuffer cmd = vkutil::beginSingleTimeCommands(*m_context, m_commandPool);
    auto barrier = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
    };
    barrier(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {e.width, e.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0);
    vkutil::endSingleTimeCommands(*m_context, m_commandPool, cmd);

    void* data = nullptr;
    vkMapMemory(dev, mem, 0, size, 0, &data);
    std::vector<uint8_t> pixels(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    vkUnmapMemory(dev, mem);
    vkDestroyBuffer(dev, buf, nullptr);
    vkFreeMemory(dev, mem, nullptr);

    // Swapchain is BGRA; PNG wants RGBA, and the window itself is opaque.
    bool bgr = m_swapchain->imageFormat() == VK_FORMAT_B8G8R8A8_UNORM || m_swapchain->imageFormat() == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (bgr) std::swap(pixels[i], pixels[i + 2]);
        pixels[i + 3] = 255;
    }
    if (stbi_write_png(path.c_str(), int(e.width), int(e.height), 4, pixels.data(), int(e.width) * 4))
        ENG_LOG("[render] screenshot saved: " << path);
    else
        ENG_LOG("[render] failed to write screenshot " << path);
}

}  // namespace eng
