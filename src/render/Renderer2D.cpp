#include "eng/render/Renderer2D.h"

#include <chrono>

#include "eng/core/Log.h"
#include "eng/platform/Window.h"
#include "eng/render/Swapchain.h"
#include "eng/render/VulkanContext.h"
#include "eng/render/VulkanUtils.h"
#include "Font.h"

#include <GLFW/glfw3.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace eng {

namespace {

// SPIR-V compiled at build time (glslc -mfmt=c, see build.zig).
const uint32_t kSpriteVertSpv[] =
#include "sprite.vert.inc"
    ;
const uint32_t kSpriteFragSpv[] =
#include "sprite.frag.inc"
    ;

constexpr VkFormat kLightmapFormat = VK_FORMAT_R8G8B8A8_UNORM;

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
    if (options.vsync) {  // before anything is built on the swapchain
        m_swapchain->setVsync(true);
        m_swapchain->recreate();
    }

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
    createDescriptorPool();
    createBuiltinTextures();
    createLightmaps();  // after the builtins (their fixed ids)

    m_vertices.reserve(kMaxSprites * 4);
    m_clips.assign(1, VkRect2D{});
}

Renderer2D::~Renderer2D() {
    VkDevice dev = m_context->device();
    vkDeviceWaitIdle(dev);

    destroyLightmaps();
    for (auto& tex : m_textures) {
        vkutil::destroyTexture(*m_context, tex.res);
    }
    vkDestroyDescriptorPool(dev, m_descriptorPool, nullptr);  // frees every texture's set with it

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
    vkDestroyPipeline(dev, m_pipelineModulate, nullptr);
    vkDestroyPipeline(dev, m_lightPipelineAlpha, nullptr);
    vkDestroyPipeline(dev, m_lightPipelineAdditive, nullptr);
    vkDestroySampler(dev, m_lightSampler, nullptr);
    vkDestroyRenderPass(dev, m_lightPass, nullptr);
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

    // The lightmap pass: cleared to the ambient, left ready for the main
    // pass's composite to sample.
    color.format = kLightmapFormat;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    info.dependencyCount = 2;
    info.pDependencies = deps;
    if (vkCreateRenderPass(m_context->device(), &info, nullptr, &m_lightPass) != VK_SUCCESS)
        throw std::runtime_error("failed to create lightmap render pass");
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

    // One combined image sampler (binding 0, fragment stage) per texture.
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

    m_vertModule = createShaderModule(dev, kSpriteVertSpv, sizeof(kSpriteVertSpv));
    m_fragModule = createShaderModule(dev, kSpriteFragSpv, sizeof(kSpriteFragSpv));
    m_pipelineAlpha = createPipeline(m_renderPass, BlendMode::Alpha);
    m_pipelineAdditive = createPipeline(m_renderPass, BlendMode::Additive);
    m_pipelineModulate = createPipeline(m_renderPass, BlendMode::Modulate2x);
    m_lightPipelineAlpha = createPipeline(m_lightPass, BlendMode::Alpha);
    m_lightPipelineAdditive = createPipeline(m_lightPass, BlendMode::Additive);
    vkDestroyShaderModule(dev, m_vertModule, nullptr);
    vkDestroyShaderModule(dev, m_fragModule, nullptr);
    m_vertModule = m_fragModule = VK_NULL_HANDLE;
}

VkPipeline Renderer2D::createPipeline(VkRenderPass pass, BlendMode blendMode) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_fragModule;
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
    if (blendMode == BlendMode::Additive) {
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    } else if (blendMode == BlendMode::Modulate2x) {  // src·dst + dst·src
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    }
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
    info.renderPass = pass;
    info.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(m_context->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        throw std::runtime_error("failed to create sprite pipeline");
    return pipeline;
}

void Renderer2D::createLightmaps() {
    VkDevice dev = m_context->device();
    if (!m_lightSampler) {
        // Linear, unlike sprite textures: the half-resolution lightmap is
        // stretched over the screen and should read as smooth light.
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.0f;
        if (vkCreateSampler(dev, &si, nullptr, &m_lightSampler) != VK_SUCCESS)
            throw std::runtime_error("failed to create lightmap sampler");
    }
    VkExtent2D e = m_swapchain->extent();
    m_lightExtent = {std::max(1u, e.width / 2), std::max(1u, e.height / 2)};
    for (Lightmap& lm : m_lightmaps) {
        vkutil::createImage(*m_context, m_lightExtent.width, m_lightExtent.height, kLightmapFormat, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, lm.image, lm.memory);
        lm.view = vkutil::createImageView(*m_context, lm.image, kLightmapFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = m_lightPass;
        fb.attachmentCount = 1;
        fb.pAttachments = &lm.view;
        fb.width = m_lightExtent.width;
        fb.height = m_lightExtent.height;
        fb.layers = 1;
        if (vkCreateFramebuffer(dev, &fb, nullptr, &lm.framebuffer) != VK_SUCCESS)
            throw std::runtime_error("failed to create lightmap framebuffer");
        // A texture slot like any other, so the composite is an ordinary
        // sprite batch. Resources are owned here; the slot only holds the set.
        Texture tex;
        tex.descriptor = allocateTextureSet(m_lightSampler, lm.view);
        tex.width = m_lightExtent.width;
        tex.height = m_lightExtent.height;
        if (lm.texture) {
            m_textures[lm.texture] = tex;
        } else {
            m_textures.push_back(tex);
            lm.texture = static_cast<TextureId>(m_textures.size() - 1);
        }
    }
}

void Renderer2D::destroyLightmaps() {
    VkDevice dev = m_context->device();
    for (Lightmap& lm : m_lightmaps) {
        if (lm.texture && m_textures[lm.texture].descriptor) {
            freeTextureSet(m_textures[lm.texture].descriptor);
            m_textures[lm.texture].descriptor = VK_NULL_HANDLE;
        }
        if (lm.framebuffer) vkDestroyFramebuffer(dev, lm.framebuffer, nullptr);
        if (lm.view) vkDestroyImageView(dev, lm.view, nullptr);
        if (lm.image) vkDestroyImage(dev, lm.image, nullptr);
        if (lm.memory) vkFreeMemory(dev, lm.memory, nullptr);
        lm.framebuffer = VK_NULL_HANDLE;
        lm.view = VK_NULL_HANDLE;
        lm.image = VK_NULL_HANDLE;
        lm.memory = VK_NULL_HANDLE;
    }
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

void Renderer2D::createDescriptorPool() {
    // One combined-image-sampler set per texture (sprites, font atlases, the
    // lightmaps); sets are freed individually as lightmaps are recreated.
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 4096;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(m_context->device(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create descriptor pool");
}

VkDescriptorSet Renderer2D::allocateTextureSet(VkSampler sampler, VkImageView view) {
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = m_descriptorPool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &m_textureSetLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_context->device(), &alloc, &set) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate a texture descriptor set (pool exhausted?)");
    VkDescriptorImageInfo image{};
    image.sampler = sampler;
    image.imageView = view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(m_context->device(), 1, &write, 0, nullptr);
    return set;
}

void Renderer2D::freeTextureSet(VkDescriptorSet set) {
    if (set) vkFreeDescriptorSets(m_context->device(), m_descriptorPool, 1, &set);
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

    // Glow: smooth falloff to zero at the rim, so even hugely magnified with
    // nearest filtering there's no visible edge.
    constexpr int kGlowSize = 128;
    std::vector<uint8_t> glow(kGlowSize * kGlowSize * 4);
    for (int y = 0; y < kGlowSize; ++y) {
        for (int x = 0; x < kGlowSize; ++x) {
            float dx = (x + 0.5f) / kGlowSize * 2.0f - 1.0f;
            float dy = (y + 0.5f) / kGlowSize * 2.0f - 1.0f;
            float d = std::min(1.0f, std::sqrt(dx * dx + dy * dy));
            float a = (1.0f - d) * (1.0f - d);
            uint8_t* p = &glow[(y * kGlowSize + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<uint8_t>(a * 255.0f);
        }
    }
    TextureId g = createTexture(glow.data(), kGlowSize, kGlowSize);
    if (w != kWhite || c != kCircle || g != kGlow) throw std::logic_error("builtin texture ids out of order");

    // The three of them in one texture too, which is what actually gets
    // drawn (pushVertices remaps their UVs), so rects, circles, glows,
    // triangles and UI shapes batch together instead of each switch of
    // shape starting a draw call. Nearest filtering, so the 2 px transparent
    // gaps keep neighbours from bleeding; the glow's and disc's own rims are
    // transparent anyway. Layout: glow (0,0) 128², disc (130,0) 64², white
    // (130,66) 4².
    constexpr int kAtlasW = 196, kAtlasH = 128;
    std::vector<uint8_t> atlas(size_t(kAtlasW) * kAtlasH * 4, 0);
    auto blit = [&](const std::vector<uint8_t>& src, int size, int ox, int oy) {
        for (int y = 0; y < size; ++y)
            std::memcpy(&atlas[(size_t(oy + y) * kAtlasW + ox) * 4], &src[size_t(y) * size * 4], size_t(size) * 4);
    };
    blit(glow, kGlowSize, 0, 0);
    blit(disc, kSize, 130, 0);
    blit(std::vector<uint8_t>(4 * 4 * 4, 255), 4, 130, 66);
    m_builtinAtlas = createTexture(atlas.data(), kAtlasW, kAtlasH);
    auto rect = [&](int x, int y, int w, int h) {
        return glm::vec4(float(x) / kAtlasW, float(y) / kAtlasH, float(w) / kAtlasW, float(h) / kAtlasH);
    };
    m_builtinUv[kWhite] = rect(132, 68, 0, 0);  // the block's centre: every UV maps there
    m_builtinUv[kCircle] = rect(130, 0, kSize, kSize);
    m_builtinUv[kGlow] = rect(0, 0, kGlowSize, kGlowSize);
}

// ── Textures ─────────────────────────────────────────────────────────────────

TextureId Renderer2D::createTexture(const uint8_t* rgba, uint32_t width, uint32_t height, bool linear) {
    Texture tex;
    // No mips: pixel art is drawn at integer-ish scales and should stay
    // nearest-filtered at every size.
    tex.res = vkutil::createTextureFromPixels(*m_context, m_commandPool, rgba, width, height,
                                              /*generateMips=*/false, linear);
    tex.descriptor = allocateTextureSet(tex.res.sampler, tex.res.view);
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
    if (!quadFits()) return;  // cap (counted in FrameStats); never overrun the mapped buffer

    glm::vec2 h = s.size * 0.5f;
    glm::vec2 corners[4] = {{-h.x, -h.y}, {h.x, -h.y}, {h.x, h.y}, {-h.x, h.y}};
    if (s.rotation != 0.0f) {
        float c = std::cos(s.rotation), sn = std::sin(s.rotation);
        for (auto& p : corners) p = {p.x * c - p.y * sn, p.x * sn + p.y * c};
    }
    // A builtin's UV rect maps into the shared atlas once here, rather than
    // per vertex in pushVertices (most quads are sprites).
    TextureId tex = s.texture < m_textures.size() ? s.texture : kWhite;
    glm::vec4 uv = s.uv;
    if (tex <= kGlow && m_builtinAtlas != kWhite) {
        const glm::vec4& r = m_builtinUv[tex];
        uv = {r.x + uv.x * r.z, r.y + uv.y * r.w, r.x + uv.z * r.z, r.y + uv.w * r.w};
        tex = m_builtinAtlas;
    }
    glm::vec2 uvs[4] = {{uv.x, uv.y}, {uv.z, uv.y}, {uv.z, uv.w}, {uv.x, uv.w}};
    Vertex v[4];
    for (int i = 0; i < 4; ++i) v[i] = {s.pos + corners[i], uvs[i], s.color};
    appendQuad(v, tex, s.blend, s.space);
}

// One quad's worth of vertices onto the scene's or (between beginLights and
// endLights) the lightmap's batch list; both index the same vertex buffer.
void Renderer2D::pushVertices(const Vertex* v, TextureId texture, BlendMode blend, Space space) {
    TextureId tex = texture < m_textures.size() ? texture : kWhite;
    if (tex <= kGlow && m_builtinAtlas != kWhite) {  // a builtin: its place in the shared atlas
        Vertex q[4];
        std::memcpy(q, v, sizeof q);
        const float ox = m_builtinUv[tex].x, oy = m_builtinUv[tex].y, sx = m_builtinUv[tex].z, sy = m_builtinUv[tex].w;
        for (Vertex& x : q) {
            x.uv.x = ox + x.uv.x * sx;
            x.uv.y = oy + x.uv.y * sy;
        }
        appendQuad(q, m_builtinAtlas, blend, space);
        return;
    }
    appendQuad(v, tex, blend, space);
}

// The quad as given (UVs already in `tex`), into the current batch or a new one.
void Renderer2D::appendQuad(const Vertex* v, TextureId tex, BlendMode blend, Space space) {
    auto firstIndex = static_cast<uint32_t>(m_vertices.size() / 4 * 6);
    m_vertices.insert(m_vertices.end(), v, v + 4);
    // Clips are a screen/UI thing; the lightmap pass ignores them.
    uint32_t clip = m_inLights || m_clipStack.empty() ? 0 : m_clipStack.back();
    std::vector<Batch>& batches = m_inLights ? m_lightBatches : m_batches;
    if (!batches.empty()) {
        Batch& last = batches.back();
        if (last.texture == tex && last.blend == blend && last.space == space && last.clip == clip &&
            last.firstIndex + last.indexCount == firstIndex) {
            last.indexCount += 6;
            return;
        }
    }
    batches.push_back({tex, blend, space, clip, firstIndex, 6});
}

void Renderer2D::pushQuad(const glm::vec2 (&p)[4], const glm::vec4& color, Space space) {
    if (!quadFits()) return;
    glm::vec2 uv(0.5f);
    Vertex v[4] = {{p[0], uv, color}, {p[1], uv, color}, {p[2], uv, color}, {p[3], uv, color}};
    pushVertices(v, kWhite, BlendMode::Alpha, space);
}

void Renderer2D::drawTriangleColors(const glm::vec2 (&p)[3], const glm::vec4 (&c)[3], Space space) {
    if (!quadFits()) return;
    glm::vec2 uv(0.5f);
    Vertex v[4] = {{p[0], uv, c[0]}, {p[1], uv, c[1]}, {p[2], uv, c[2]}, {p[2], uv, c[2]}};  // degenerate 2nd triangle
    pushVertices(v, kWhite, BlendMode::Alpha, space);
}

void Renderer2D::drawQuadColors(const glm::vec2 (&p)[4], const glm::vec4 (&c)[4], Space space) {
    if (!quadFits()) return;
    glm::vec2 uv(0.5f);
    Vertex v[4] = {{p[0], uv, c[0]}, {p[1], uv, c[1]}, {p[2], uv, c[2]}, {p[3], uv, c[3]}};
    pushVertices(v, kWhite, BlendMode::Alpha, space);
}

// ── Clipping ─────────────────────────────────────────────────────────────────

void Renderer2D::pushClip(const Rect& r) {
    float x0 = std::floor(r.pos.x), y0 = std::floor(r.pos.y);
    float x1 = std::ceil(r.pos.x + r.size.x), y1 = std::ceil(r.pos.y + r.size.y);
    glm::vec2 screen = screenSize();
    x0 = std::max(x0, 0.0f), y0 = std::max(y0, 0.0f);
    x1 = std::min(x1, screen.x), y1 = std::min(y1, screen.y);
    if (!m_clipStack.empty()) {  // nested: intersect with the enclosing clip
        const VkRect2D& p = m_clips[m_clipStack.back()];
        x0 = std::max(x0, float(p.offset.x)), y0 = std::max(y0, float(p.offset.y));
        x1 = std::min(x1, float(p.offset.x + int32_t(p.extent.width)));
        y1 = std::min(y1, float(p.offset.y + int32_t(p.extent.height)));
    }
    VkRect2D rect{};
    rect.offset = {int32_t(x0), int32_t(y0)};
    rect.extent = {uint32_t(std::max(0.0f, x1 - x0)), uint32_t(std::max(0.0f, y1 - y0))};
    m_clips.push_back(rect);
    m_clipStack.push_back(uint32_t(m_clips.size() - 1));
}

void Renderer2D::popClip() {
    if (!m_clipStack.empty()) m_clipStack.pop_back();
}

void Renderer2D::drawTriangle(const glm::vec2 (&p)[3], const glm::vec2 (&uv)[3], const glm::vec4& color,
                              TextureId texture, Space space, BlendMode blend) {
    if (!quadFits()) return;
    // A quad whose second triangle (c, c, a) is degenerate.
    Vertex v[4] = {{p[0], uv[0], color}, {p[1], uv[1], color}, {p[2], uv[2], color}, {p[2], uv[2], color}};
    pushVertices(v, texture, blend, space);
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

void Renderer2D::drawGlow(const glm::vec2& center, float radius, const glm::vec4& color, Space space) {
    Sprite s;
    s.texture = kGlow;
    s.pos = center;
    s.size = glm::vec2(radius * 2.0f);
    s.color = color;
    s.space = space;
    s.blend = BlendMode::Additive;
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

// ── Lighting ─────────────────────────────────────────────────────────────────

void Renderer2D::beginLights(const glm::vec4& ambient) {
    m_lightAmbient = ambient;
    m_inLights = true;
}

void Renderer2D::endLights() { m_inLights = false; }

void Renderer2D::compositeLights() {
    m_inLights = false;
    m_lightsUsed = true;
    Sprite s;
    s.texture = m_lightmaps[m_frameIndex].texture;
    s.pos = screenSize() * 0.5f;
    s.size = screenSize();
    s.blend = BlendMode::Modulate2x;
    s.space = Space::Screen;
    draw(s);
}

// ── Frame ────────────────────────────────────────────────────────────────────

void Renderer2D::setVsync(bool on) {
    if (on == m_swapchain->vsync()) return;
    m_swapchain->setVsync(on);
    m_swapchainDirty = true;
}

bool Renderer2D::vsync() const { return m_swapchain->vsync(); }

const char* Renderer2D::presentModeName() const {
    switch (m_swapchain->presentMode()) {
        case VK_PRESENT_MODE_FIFO_KHR: return "FIFO (vsync)";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO relaxed";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX (uncapped, no tearing)";
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE (uncapped, may tear)";
        default: return "other";
    }
}

void Renderer2D::recreateSwapchain() {
    vkDeviceWaitIdle(m_context->device());
    destroyFramebuffers();
    destroyLightmaps();
    m_swapchain->recreate();
    createFramebuffers();
    createLightmaps();
}

bool Renderer2D::beginFrame() {
    // Minimized: skip rendering entirely instead of letting
    // Swapchain::recreate() block in glfwWaitEvents — the caller may be a
    // host whose simulation must keep ticking for everyone else.
    if (m_window.framebufferWidth() == 0 || m_window.framebufferHeight() == 0) return false;
    if (m_window.consumeResized() || m_swapchainDirty) {
        m_swapchainDirty = false;
        recreateSwapchain();
    }

    FrameResources& f = m_frames[m_frameIndex];
    VkDevice dev = m_context->device();
    auto waitStart = std::chrono::steady_clock::now();
    vkWaitForFences(dev, 1, &f.inFlight, VK_TRUE, UINT64_MAX);

    VkResult r = vkAcquireNextImageKHR(dev, m_swapchain->handle(), UINT64_MAX, f.imageAvailable,
                                       VK_NULL_HANDLE, &m_imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) throw std::runtime_error("failed to acquire swapchain image");
    vkResetFences(dev, 1, &f.inFlight);
    m_waitMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
    m_droppedQuads = 0;

    m_vertices.clear();
    m_batches.clear();
    m_lightBatches.clear();
    m_clips.resize(1);
    m_clipStack.clear();
    m_inLights = false;
    m_lightsUsed = false;
    m_frameActive = true;
    return true;
}

void Renderer2D::endFrame() {
    if (!m_frameActive) return;
    m_frameActive = false;
    auto submitStart = std::chrono::steady_clock::now();
    m_lastStats.quads = static_cast<uint32_t>(m_vertices.size() / 4);
    m_lastStats.batches = static_cast<uint32_t>(m_batches.size());
    m_lastStats.lightBatches = m_lightsUsed ? static_cast<uint32_t>(m_lightBatches.size()) : 0;
    m_lastStats.droppedQuads = m_droppedQuads;
    m_lastStats.quadCapacity = kMaxSprites;
    m_lastStats.waitMs = m_waitMs;
    ++m_lastStats.frame;

    FrameResources& f = m_frames[m_frameIndex];
    std::memcpy(f.vertexMapped, m_vertices.data(), m_vertices.size() * sizeof(Vertex));

    VkCommandBuffer cmd = f.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // The lightmap first, so the main pass can composite it.
    if (m_lightsUsed) {
        const Lightmap& lm = m_lightmaps[m_frameIndex];
        VkClearValue ambient{};
        ambient.color = {{m_lightAmbient.r, m_lightAmbient.g, m_lightAmbient.b, m_lightAmbient.a}};
        VkRenderPassBeginInfo lp{};
        lp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        lp.renderPass = m_lightPass;
        lp.framebuffer = lm.framebuffer;
        lp.renderArea.extent = m_lightExtent;
        lp.clearValueCount = 1;
        lp.pClearValues = &ambient;
        vkCmdBeginRenderPass(cmd, &lp, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport lvp{0.0f, 0.0f, float(m_lightExtent.width), float(m_lightExtent.height), 0.0f, 1.0f};
        VkRect2D lscissor{{0, 0}, m_lightExtent};
        vkCmdSetViewport(cmd, 0, 1, &lvp);
        vkCmdSetScissor(cmd, 0, 1, &lscissor);
        recordBatches(cmd, m_lightBatches, /*lightPass=*/true);
        vkCmdEndRenderPass(cmd);
    }

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

    recordBatches(cmd, m_batches, /*lightPass=*/false);

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
    m_lastStats.submitMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - submitStart).count();
}

void Renderer2D::recordBatches(VkCommandBuffer cmd, const std::vector<Batch>& batches, bool lightPass) {
    if (batches.empty()) return;
    FrameResources& f = m_frames[m_frameIndex];
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &f.vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    // Same mapping in both passes: positions go to NDC, whatever the target's size.
    glm::vec2 size = screenSize();
    PushConstants screenPc{2.0f / size, glm::vec2(-1.0f)};
    PushConstants worldPc{2.0f * m_pixelsPerUnit / size, -2.0f * m_camCenter * m_pixelsPerUnit / size};

    VkPipeline bound = VK_NULL_HANDLE;
    uint32_t boundClip = 0;  // the caller set the full-target scissor
    VkExtent2D extent = m_swapchain->extent();
    for (const Batch& b : batches) {
        if (!lightPass && b.clip != boundClip) {
            VkRect2D sc = b.clip ? m_clips[b.clip] : VkRect2D{{0, 0}, extent};
            if (sc.extent.width == 0 || sc.extent.height == 0) continue;  // fully clipped away
            vkCmdSetScissor(cmd, 0, 1, &sc);
            boundClip = b.clip;
        }
        VkPipeline p;
        if (lightPass) p = b.blend == BlendMode::Additive ? m_lightPipelineAdditive : m_lightPipelineAlpha;
        else if (b.blend == BlendMode::Additive) p = m_pipelineAdditive;
        else if (b.blend == BlendMode::Modulate2x) p = m_pipelineModulate;
        else p = m_pipelineAlpha;
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
