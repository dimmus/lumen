module;

#if defined(LUMEN_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif
#include <cstdint>
#include <cstring>

import lx.foundation;

export module lx.gfx:vk_renderer;

import :dmabuf;
import :renderer;

export namespace lx::gfx {

/// Opaque Vulkan handles handed from `lx::gfx::device` to the renderer. Kept as `void*`
/// so this partition's interface stays free of Vulkan types, and so the partition does
/// not have to import the primary module interface that owns `device`.
struct vk_context {
    void* device = nullptr;
    void* physical_device = nullptr;
    void* queue = nullptr;
    void* command_pool = nullptr;
    unsigned queue_family = 0;
    bool supports_queue_family_foreign = false;

    [[nodiscard]] bool valid() const {
        return device && physical_device && queue && command_pool;
    }
};

/// GPU composite target whose memory is exportable as a dmabuf, so the same image the
/// compositor renders into can be handed to KMS as a scanout framebuffer.
class render_target {
public:
    render_target() = default;
    ~render_target();

    render_target(const render_target&) = delete;
    render_target& operator=(const render_target&) = delete;
    render_target(render_target&& other) noexcept;
    render_target& operator=(render_target&& other) noexcept;

    [[nodiscard]] static lx::result<render_target> create(const vk_context& ctx, unsigned width,
                                                          unsigned height);

    [[nodiscard]] unsigned width() const { return width_; }
    [[nodiscard]] unsigned height() const { return height_; }
    [[nodiscard]] bool valid() const { return image_ != nullptr; }

    /// Fresh dmabuf FD for `drmModeAddFB2WithModifiers`. The caller owns the FD.
    [[nodiscard]] lx::result<exported_dmabuf> export_dmabuf() const;

    [[nodiscard]] void* vk_image() const { return image_; }
    [[nodiscard]] void* vk_image_view() const { return view_; }
    [[nodiscard]] void* vk_memory() const { return memory_; }

private:
    void destroy();

    vk_context ctx_{};
    void* image_ = nullptr;
    void* memory_ = nullptr;
    void* view_ = nullptr;
    unsigned width_ = 0;
    unsigned height_ = 0;
};

struct composite_stats {
    unsigned draws_submitted = 0;
    /// Draws whose texture id was never registered. Non-zero means the commit path and
    /// the render path disagree — it must not be papered over with a placeholder.
    unsigned draws_skipped = 0;
    /// Clipped away entirely, so never submitted. Expected, not an error.
    unsigned draws_culled = 0;
};

/// Records and submits the compositor's textured-quad pass.
class vulkan_compositor {
public:
    vulkan_compositor() = default;
    ~vulkan_compositor();

    vulkan_compositor(const vulkan_compositor&) = delete;
    vulkan_compositor& operator=(const vulkan_compositor&) = delete;

    [[nodiscard]] lx::result<void> initialize(const vk_context& ctx);
    void shutdown();
    [[nodiscard]] bool ready() const { return pipeline_ != nullptr; }

    /// Make a dmabuf-imported client image samplable under `image.image_id`.
    [[nodiscard]] lx::result<void> bind_imported(const imported_image& image);

    /// Upload shared-memory pixels (tightly packed RGBA8) under `texture_id`.
    [[nodiscard]] lx::result<void> upload_rgba(unsigned texture_id, unsigned width,
                                               unsigned height, const unsigned char* rgba);

    void forget_texture(unsigned texture_id);

    [[nodiscard]] lx::result<composite_stats> composite(render_target& target, lx::color clear,
                                                         const blit_command* cmds,
                                                         unsigned count);

    /// Copy the target back into tightly packed RGBA8 for golden-image verification.
    [[nodiscard]] lx::result<void> read_back(const render_target& target, unsigned char* rgba,
                                             unsigned capacity);

private:
    static constexpr unsigned k_max_textures = 256;

    struct texture_slot {
        unsigned id = 0;
        void* image = nullptr;
        void* view = nullptr;
        void* memory = nullptr;
        void* descriptor_set = nullptr;
        unsigned width = 0;
        unsigned height = 0;
        bool owns_image = false;
        bool layout_ready = false;
        bool used = false;
    };

    [[nodiscard]] int find_texture(unsigned id) const;
    [[nodiscard]] int alloc_texture(unsigned id);
    void destroy_texture(texture_slot& slot);
    [[nodiscard]] lx::result<void> ensure_framebuffer(const render_target& target);
    [[nodiscard]] lx::result<void> create_descriptor(texture_slot& slot);
    [[nodiscard]] lx::result<void> ensure_staging_ring(unsigned need_bytes);
    [[nodiscard]] lx::result<void> submit_current_slot(bool wait_for_completion);
    /// Blocks until every submitted frame slot has completed. Required before destroying
    /// any resource a queued command buffer might reference.
    void wait_all_slots_idle();

    static constexpr unsigned k_frame_slots = 2;
    static constexpr unsigned k_staging_ring_bytes = 16u * 1024u * 1024u;

    vk_context ctx_{};
    void* render_pass_ = nullptr;
    void* pipeline_ = nullptr;
    void* pipeline_layout_ = nullptr;
    void* descriptor_layout_ = nullptr;
    void* descriptor_pool_ = nullptr;
    void* sampler_ = nullptr;
    void* command_buffers_[k_frame_slots]{};
    void* fences_[k_frame_slots]{};
    unsigned submit_slot_ = 0;
    void* staging_buffer_ = nullptr;
    void* staging_memory_ = nullptr;
    unsigned char* staging_mapped_ = nullptr;
    unsigned staging_capacity_ = 0;
    unsigned staging_offset_ = 0;

    /// One framebuffer per render target. A double-buffered scanout alternates targets
    /// every frame, so a single-entry cache would destroy and recreate it per frame.
    static constexpr unsigned k_max_framebuffers = 4;
    struct framebuffer_slot {
        void* framebuffer = nullptr;
        void* image = nullptr;
    };
    framebuffer_slot framebuffers_[k_max_framebuffers]{};
    void* framebuffer_ = nullptr;
    texture_slot textures_[k_max_textures]{};
};

} // namespace lx::gfx

#if defined(LUMEN_HAS_VULKAN)

namespace {

/// Mirrors the `quad_push` block in composite.vert/.frag. Field order matters: each vec4
/// sits at a 16-byte-aligned offset, which the layout below satisfies naturally (dst at 0,
/// src_uv at 32, tint at 48) for 64 bytes total — well inside the 128 every driver
/// guarantees for push constants.
struct quad_push {
    float dst[4]{};
    float target[2]{};
    float opacity = 1.f;
    float pad = 0.f;
    /// Source rectangle in normalized texture coordinates: u0, v0, du, dv.
    float src_uv[4]{0.f, 0.f, 1.f, 1.f};
    float tint[4]{1.f, 1.f, 1.f, 1.f};
    /// Half-texel-inset sampling bounds for `src_uv`: u_lo, v_lo, u_hi, v_hi. The fragment
    /// shader clamps to these so a magnified crop cannot bleed texels from outside the
    /// source rectangle — which is precisely what viewporter must not do.
    float src_bounds[4]{0.f, 0.f, 1.f, 1.f};
    /// Transfer function to decode the sampled texel with. Matches `transfer_slot`.
    int transfer = 1;
    float pad1 = 0.f;
    float pad2 = 0.f;
    float pad3 = 0.f;
};

/// Shader-side selector for a transfer function. Must match the `decode` branch order in
/// composite.frag; `lx::transfer_function`'s own values are an independent enum.
[[nodiscard]] inline int transfer_slot(lx::transfer_function tf) {
    switch (tf) {
    case lx::transfer_function::linear: return 0;
    case lx::transfer_function::gamma22: return 2;
    case lx::transfer_function::pq: return 3;
    case lx::transfer_function::hlg: return 4;
    case lx::transfer_function::srgb:
    default: return 1;
    }
}

// sRGB, not UNORM. The shader writes linear light, and an sRGB attachment makes the
// fixed-function blender decode the destination to linear, blend, and re-encode on write —
// which is what makes alpha compositing correct. The DRM fourcc for scanout is unchanged:
// DRM formats carry no transfer function, so KMS still sees ARGB8888.
//
// This is the SDR half of the color pipeline. Wider storage (A2B10G10R10 or RGBA16F, with
// PQ output for HDR) needs a linear intermediate plus an explicit encode pass, because no
// 10-bit format has an sRGB variant for the hardware to encode into.
constexpr VkFormat k_target_format = VK_FORMAT_B8G8R8A8_SRGB;

lx::error vk_error(const char* message) {
    return lx::make_error(lx::error_domain::vulkan,
                          static_cast<int>(lx::vulkan_err::device_lost), message);
}

int find_memory_type(VkPhysicalDevice phys, uint32_t type_bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
            return static_cast<int>(i);
    }
    return -1;
}

} // namespace

#endif

#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)

namespace {

// glslc -mfmt=c emits a bare C initializer list, so the SPIR-V lives in the binary and
// the compositor never depends on shader files at runtime.
constexpr uint32_t k_composite_vert_spv[] =
#include "shaders/composite.vert.spv.h"
    ;
constexpr uint32_t k_composite_frag_spv[] =
#include "shaders/composite.frag.spv.h"
    ;

VkShaderModule create_shader(VkDevice dev, const uint32_t* code, std::size_t bytes) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule module_handle = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &info, nullptr, &module_handle) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module_handle;
}

} // namespace

#endif

lx::gfx::render_target::~render_target() { destroy(); }

lx::gfx::render_target::render_target(render_target&& other) noexcept
    : ctx_{other.ctx_}, image_{other.image_}, memory_{other.memory_}, view_{other.view_},
      width_{other.width_}, height_{other.height_} {
    other.image_ = nullptr;
    other.memory_ = nullptr;
    other.view_ = nullptr;
    other.width_ = 0;
    other.height_ = 0;
}

lx::gfx::render_target& lx::gfx::render_target::operator=(render_target&& other) noexcept {
    if (this == &other)
        return *this;
    destroy();
    ctx_ = other.ctx_;
    image_ = other.image_;
    memory_ = other.memory_;
    view_ = other.view_;
    width_ = other.width_;
    height_ = other.height_;
    other.image_ = nullptr;
    other.memory_ = nullptr;
    other.view_ = nullptr;
    other.width_ = 0;
    other.height_ = 0;
    return *this;
}

void lx::gfx::render_target::destroy() {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    if (dev) {
        if (view_)
            vkDestroyImageView(dev, static_cast<VkImageView>(view_), nullptr);
        if (image_)
            vkDestroyImage(dev, static_cast<VkImage>(image_), nullptr);
        if (memory_)
            vkFreeMemory(dev, static_cast<VkDeviceMemory>(memory_), nullptr);
    }
#endif
    image_ = nullptr;
    memory_ = nullptr;
    view_ = nullptr;
    width_ = 0;
    height_ = 0;
}

lx::result<lx::gfx::render_target> lx::gfx::render_target::create(const vk_context& ctx,
                                                                  unsigned width,
                                                                  unsigned height) {
#if defined(LUMEN_HAS_VULKAN)
    if (!ctx.valid() || width == 0 || height == 0)
        return lx::not_implemented("lx::gfx::render_target::create");

    auto dev = static_cast<VkDevice>(ctx.device);
    auto phys = static_cast<VkPhysicalDevice>(ctx.physical_device);

    // Linear tiling with an exportable allocation: KMS consumes the same memory as a
    // DRM_FORMAT_MOD_LINEAR framebuffer, so no blit or copy sits between the two.
    VkExternalMemoryImageCreateInfo external{};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &external;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = k_target_format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    render_target target;
    target.ctx_ = ctx;
    target.width_ = width;
    target.height_ = height;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(dev, &ici, nullptr, &image) != VK_SUCCESS)
        return vk_error("render target vkCreateImage failed");
    target.image_ = image;

    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(dev, image, &reqs);

    const int type_index =
        find_memory_type(phys, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type_index < 0)
        return vk_error("no device-local memory type for render target");

    VkExportMemoryAllocateInfo export_info{};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image;
    dedicated.pNext = &export_info;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &dedicated;
    mai.allocationSize = reqs.size;
    mai.memoryTypeIndex = static_cast<uint32_t>(type_index);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &mai, nullptr, &memory) != VK_SUCCESS)
        return vk_error("render target vkAllocateMemory failed");
    target.memory_ = memory;

    if (vkBindImageMemory(dev, image, memory, 0) != VK_SUCCESS)
        return vk_error("render target vkBindImageMemory failed");

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = k_target_format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS)
        return vk_error("render target vkCreateImageView failed");
    target.view_ = view;

    return target;
#else
    (void)ctx;
    (void)width;
    (void)height;
    return lx::not_implemented("lx::gfx::render_target::create");
#endif
}

lx::result<lx::gfx::exported_dmabuf> lx::gfx::render_target::export_dmabuf() const {
#if defined(LUMEN_HAS_VULKAN)
    if (!image_ || !memory_)
        return lx::not_implemented("lx::gfx::render_target::export_dmabuf");

    imported_image shim{};
    shim.width = width_;
    shim.height = height_;
    shim.format = static_cast<lx::fourcc>(lx::pixel_format::argb8888);
    shim.vk_image = image_;
    shim.vk_memory = memory_;

    dmabuf_exporter exporter{ctx_.device, ctx_.physical_device};
    return exporter.export_image(shim);
#else
    return lx::not_implemented("lx::gfx::render_target::export_dmabuf");
#endif
}

lx::gfx::vulkan_compositor::~vulkan_compositor() { shutdown(); }

int lx::gfx::vulkan_compositor::find_texture(unsigned id) const {
    if (id == 0)
        return -1;
    for (unsigned i = 0; i < k_max_textures; ++i) {
        if (textures_[i].used && textures_[i].id == id)
            return static_cast<int>(i);
    }
    return -1;
}

int lx::gfx::vulkan_compositor::alloc_texture(unsigned id) {
    if (const int existing = find_texture(id); existing >= 0)
        return existing;
    for (unsigned i = 0; i < k_max_textures; ++i) {
        if (textures_[i].used)
            continue;
        textures_[i] = {};
        textures_[i].id = id;
        textures_[i].used = true;
        return static_cast<int>(i);
    }
    return -1;
}

void lx::gfx::vulkan_compositor::destroy_texture(texture_slot& slot) {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    if (dev) {
        // Uploads are submitted without waiting, so a queued command buffer may still
        // reference this image. Destroying it first is a use-after-free inside the driver.
        wait_all_slots_idle();
        if (slot.view)
            vkDestroyImageView(dev, static_cast<VkImageView>(slot.view), nullptr);
        if (slot.owns_image && slot.image)
            vkDestroyImage(dev, static_cast<VkImage>(slot.image), nullptr);
        if (slot.owns_image && slot.memory)
            vkFreeMemory(dev, static_cast<VkDeviceMemory>(slot.memory), nullptr);
        if (slot.descriptor_set && descriptor_pool_) {
            auto set = static_cast<VkDescriptorSet>(slot.descriptor_set);
            vkFreeDescriptorSets(dev, static_cast<VkDescriptorPool>(descriptor_pool_), 1, &set);
        }
    }
#endif
    slot = {};
}

void lx::gfx::vulkan_compositor::wait_all_slots_idle() {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    if (!dev)
        return;
    VkFence fences[k_frame_slots]{};
    unsigned count = 0;
    for (unsigned i = 0; i < k_frame_slots; ++i) {
        if (fences_[i])
            fences[count++] = static_cast<VkFence>(fences_[i]);
    }
    if (count > 0)
        (void)vkWaitForFences(dev, count, fences, VK_TRUE, 1'000'000'000ull);
#endif
}

void lx::gfx::vulkan_compositor::forget_texture(unsigned id) {
    if (const int slot = find_texture(id); slot >= 0)
        destroy_texture(textures_[static_cast<unsigned>(slot)]);
}

lx::result<void> lx::gfx::vulkan_compositor::initialize(const vk_context& ctx) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    if (!ctx.valid())
        return lx::not_implemented("lx::gfx::vulkan_compositor::initialize");
    shutdown();
    ctx_ = ctx;

    auto dev = static_cast<VkDevice>(ctx_.device);

    VkAttachmentDescription color{};
    color.format = k_target_format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // GENERAL, not PRESENT_SRC: the consumer is KMS via dmabuf, not a Vulkan swapchain.
    color.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;

    VkRenderPass render_pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(dev, &rpci, nullptr, &render_pass) != VK_SUCCESS)
        return vk_error("vkCreateRenderPass failed");
    render_pass_ = render_pass;

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(dev, &sci, nullptr, &sampler) != VK_SUCCESS)
        return vk_error("vkCreateSampler failed");
    sampler_ = sampler;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 1;
    dlci.pBindings = &binding;

    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(dev, &dlci, nullptr, &descriptor_layout) != VK_SUCCESS)
        return vk_error("vkCreateDescriptorSetLayout failed");
    descriptor_layout_ = descriptor_layout;

    // One set per texture, allocated on registration and reused: the frame path must not
    // create or destroy descriptor objects.
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = k_max_textures;

    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets = k_max_textures;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &pool_size;

    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &descriptor_pool) != VK_SUCCESS)
        return vk_error("vkCreateDescriptorPool failed");
    descriptor_pool_ = descriptor_pool;

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(quad_push);

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &descriptor_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push_range;

    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &pipeline_layout) != VK_SUCCESS)
        return vk_error("vkCreatePipelineLayout failed");
    pipeline_layout_ = pipeline_layout;

    VkShaderModule vert = create_shader(dev, k_composite_vert_spv, sizeof(k_composite_vert_spv));
    VkShaderModule frag = create_shader(dev, k_composite_frag_spv, sizeof(k_composite_frag_spv));
    if (!vert || !frag) {
        if (vert)
            vkDestroyShaderModule(dev, vert, nullptr);
        if (frag)
            vkDestroyShaderModule(dev, frag, nullptr);
        return vk_error("composite shader module creation failed");
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    // Quad corners come from gl_VertexIndex, so there is no vertex input state.
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend_state{};
    blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &blend;

    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                              VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vertex_input;
    gpci.pInputAssemblyState = &input_assembly;
    gpci.pViewportState = &viewport_state;
    gpci.pRasterizationState = &raster;
    gpci.pMultisampleState = &multisample;
    gpci.pColorBlendState = &blend_state;
    gpci.pDynamicState = &dynamic;
    gpci.layout = pipeline_layout;
    gpci.renderPass = render_pass;
    gpci.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult pipeline_result =
        vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline);
    vkDestroyShaderModule(dev, vert, nullptr);
    vkDestroyShaderModule(dev, frag, nullptr);
    if (pipeline_result != VK_SUCCESS)
        return vk_error("vkCreateGraphicsPipelines failed");
    pipeline_ = pipeline;

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = static_cast<VkCommandPool>(ctx_.command_pool);
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = k_frame_slots;

    VkCommandBuffer command_buffers[k_frame_slots]{};
    if (vkAllocateCommandBuffers(dev, &cbai, command_buffers) != VK_SUCCESS)
        return vk_error("vkAllocateCommandBuffers failed");
    for (unsigned i = 0; i < k_frame_slots; ++i)
        command_buffers_[i] = command_buffers[i];

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (unsigned i = 0; i < k_frame_slots; ++i) {
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS)
            return vk_error("vkCreateFence failed");
        fences_[i] = fence;
    }

    if (auto staging = ensure_staging_ring(k_staging_ring_bytes); !staging)
        return staging.get_error();

    return {};
#else
    (void)ctx;
    return lx::not_implemented("lx::gfx::vulkan_compositor::initialize");
#endif
}

void lx::gfx::vulkan_compositor::shutdown() {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    if (dev) {
        vkDeviceWaitIdle(dev);
        for (unsigned i = 0; i < k_max_textures; ++i) {
            if (textures_[i].used)
                destroy_texture(textures_[i]);
        }
        for (unsigned i = 0; i < k_max_framebuffers; ++i) {
            if (framebuffers_[i].framebuffer)
                vkDestroyFramebuffer(dev, static_cast<VkFramebuffer>(framebuffers_[i].framebuffer),
                                     nullptr);
        }
        for (unsigned i = 0; i < k_frame_slots; ++i) {
            if (fences_[i])
                vkDestroyFence(dev, static_cast<VkFence>(fences_[i]), nullptr);
            if (command_buffers_[i]) {
                auto cb = static_cast<VkCommandBuffer>(command_buffers_[i]);
                vkFreeCommandBuffers(dev, static_cast<VkCommandPool>(ctx_.command_pool), 1, &cb);
            }
        }
        if (staging_memory_) {
            if (staging_mapped_)
                vkUnmapMemory(dev, static_cast<VkDeviceMemory>(staging_memory_));
            vkFreeMemory(dev, static_cast<VkDeviceMemory>(staging_memory_), nullptr);
        }
        if (staging_buffer_)
            vkDestroyBuffer(dev, static_cast<VkBuffer>(staging_buffer_), nullptr);
        if (pipeline_)
            vkDestroyPipeline(dev, static_cast<VkPipeline>(pipeline_), nullptr);
        if (pipeline_layout_)
            vkDestroyPipelineLayout(dev, static_cast<VkPipelineLayout>(pipeline_layout_),
                                    nullptr);
        if (descriptor_pool_)
            vkDestroyDescriptorPool(dev, static_cast<VkDescriptorPool>(descriptor_pool_),
                                    nullptr);
        if (descriptor_layout_)
            vkDestroyDescriptorSetLayout(
                dev, static_cast<VkDescriptorSetLayout>(descriptor_layout_), nullptr);
        if (sampler_)
            vkDestroySampler(dev, static_cast<VkSampler>(sampler_), nullptr);
        if (render_pass_)
            vkDestroyRenderPass(dev, static_cast<VkRenderPass>(render_pass_), nullptr);
    }
#endif
    framebuffer_ = nullptr;
    for (unsigned i = 0; i < k_max_framebuffers; ++i)
        framebuffers_[i] = {};
    for (unsigned i = 0; i < k_frame_slots; ++i) {
        fences_[i] = nullptr;
        command_buffers_[i] = nullptr;
    }
    staging_buffer_ = nullptr;
    staging_memory_ = nullptr;
    staging_mapped_ = nullptr;
    staging_capacity_ = 0;
    staging_offset_ = 0;
    submit_slot_ = 0;
    pipeline_ = nullptr;
    pipeline_layout_ = nullptr;
    descriptor_pool_ = nullptr;
    descriptor_layout_ = nullptr;
    sampler_ = nullptr;
    render_pass_ = nullptr;
    ctx_ = {};
}

lx::result<void> lx::gfx::vulkan_compositor::create_descriptor(texture_slot& slot) {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    auto layout = static_cast<VkDescriptorSetLayout>(descriptor_layout_);
    if (!dev || !layout || !descriptor_pool_ || !slot.view)
        return lx::not_implemented("lx::gfx::vulkan_compositor::create_descriptor");

    if (!slot.descriptor_set) {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = static_cast<VkDescriptorPool>(descriptor_pool_);
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(dev, &dsai, &set) != VK_SUCCESS)
            return vk_error("vkAllocateDescriptorSets failed");
        slot.descriptor_set = set;
    }

    VkDescriptorImageInfo image_info{};
    image_info.sampler = static_cast<VkSampler>(sampler_);
    image_info.imageView = static_cast<VkImageView>(slot.view);
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = static_cast<VkDescriptorSet>(slot.descriptor_set);
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
    return {};
#else
    (void)slot;
    return lx::not_implemented("lx::gfx::vulkan_compositor::create_descriptor");
#endif
}

lx::result<void> lx::gfx::vulkan_compositor::bind_imported(const imported_image& image) {
#if defined(LUMEN_HAS_VULKAN)
    if (!ready())
        return lx::not_implemented("lx::gfx::vulkan_compositor::bind_imported");
    if (!image.vk_image || image.image_id == 0) {
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "bind_imported without a Vulkan image");
    }

    const int index = alloc_texture(image.image_id);
    if (index < 0) {
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "texture table full");
    }
    auto& slot = textures_[static_cast<unsigned>(index)];
    if (slot.image == image.vk_image && slot.view)
        return {};

    auto dev = static_cast<VkDevice>(ctx_.device);
    if (slot.view) {
        vkDestroyImageView(dev, static_cast<VkImageView>(slot.view), nullptr);
        slot.view = nullptr;
    }

    slot.image = image.vk_image;
    slot.memory = image.vk_memory;
    slot.width = image.width;
    slot.height = image.height;
    slot.owns_image = false;
    slot.layout_ready = false;

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = static_cast<VkImage>(slot.image);
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = image.format == static_cast<lx::fourcc>(lx::pixel_format::rgba8888)
                     ? VK_FORMAT_R8G8B8A8_UNORM
                     : VK_FORMAT_B8G8R8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS)
        return vk_error("imported image vkCreateImageView failed");
    slot.view = view;

    return create_descriptor(slot);
#else
    (void)image;
    return lx::not_implemented("lx::gfx::vulkan_compositor::bind_imported");
#endif
}

lx::result<void> lx::gfx::vulkan_compositor::upload_rgba(unsigned texture_id, unsigned width,
                                                          unsigned height,
                                                          const unsigned char* rgba) {
#if defined(LUMEN_HAS_VULKAN)
    if (!ready())
        return lx::not_implemented("lx::gfx::vulkan_compositor::upload_rgba");
    if (texture_id == 0 || width == 0 || height == 0 || !rgba) {
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "upload_rgba with empty source");
    }

    auto dev = static_cast<VkDevice>(ctx_.device);
    auto phys = static_cast<VkPhysicalDevice>(ctx_.physical_device);

    const int index = alloc_texture(texture_id);
    if (index < 0) {
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "texture table full");
    }
    auto& slot = textures_[static_cast<unsigned>(index)];

    // Reallocate only on a size change; a resizing client is rare, a redrawing one is not.
    if (slot.image && (slot.width != width || slot.height != height)) {
        const unsigned keep_id = slot.id;
        destroy_texture(slot);
        slot.id = keep_id;
        slot.used = true;
    }

    if (!slot.image) {
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(dev, &ici, nullptr, &image) != VK_SUCCESS)
            return vk_error("shm texture vkCreateImage failed");

        VkMemoryRequirements reqs{};
        vkGetImageMemoryRequirements(dev, image, &reqs);
        const int type_index =
            find_memory_type(phys, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type_index < 0) {
            vkDestroyImage(dev, image, nullptr);
            return vk_error("no device-local memory type for shm texture");
        }

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = reqs.size;
        mai.memoryTypeIndex = static_cast<uint32_t>(type_index);

        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (vkAllocateMemory(dev, &mai, nullptr, &memory) != VK_SUCCESS) {
            vkDestroyImage(dev, image, nullptr);
            return vk_error("shm texture vkAllocateMemory failed");
        }
        if (vkBindImageMemory(dev, image, memory, 0) != VK_SUCCESS) {
            vkFreeMemory(dev, memory, nullptr);
            vkDestroyImage(dev, image, nullptr);
            return vk_error("shm texture vkBindImageMemory failed");
        }

        slot.image = image;
        slot.memory = memory;
        slot.width = width;
        slot.height = height;
        slot.owns_image = true;
        slot.layout_ready = false;

        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;

        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS)
            return vk_error("shm texture vkCreateImageView failed");
        slot.view = view;

        if (auto described = create_descriptor(slot); !described)
            return described.get_error();
    }

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;
    if (auto staged = ensure_staging_ring(k_staging_ring_bytes); !staged)
        return staged.get_error();

    if (bytes > staging_capacity_) {
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "upload exceeds staging ring capacity");
    }
    if (staging_offset_ + bytes > staging_capacity_) {
        // Wrapping reuses memory an earlier submission may still be reading, and only the
        // slot fences tell us when that is done. Drain them before rewinding.
        VkFence all[k_frame_slots]{};
        unsigned pending = 0;
        for (unsigned i = 0; i < k_frame_slots; ++i) {
            if (fences_[i])
                all[pending++] = static_cast<VkFence>(fences_[i]);
        }
        if (pending > 0 &&
            vkWaitForFences(dev, pending, all, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS)
            return vk_error("vkWaitForFences timed out rewinding staging ring");
        staging_offset_ = 0;
    }

    const VkDeviceSize ring_offset = static_cast<VkDeviceSize>(staging_offset_);
    std::memcpy(staging_mapped_ + staging_offset_, rgba, static_cast<std::size_t>(bytes));
    staging_offset_ = static_cast<unsigned>((staging_offset_ + bytes + 255u) & ~255u);

    const unsigned frame_slot = submit_slot_;
    auto cb = static_cast<VkCommandBuffer>(command_buffers_[frame_slot]);
    auto fence = static_cast<VkFence>(fences_[frame_slot]);
    // Fences start signaled, so the first use of each slot does not block.
    if (vkWaitForFences(dev, 1, &fence, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS)
        return vk_error("vkWaitForFences timed out reclaiming upload slot");
    vkResetFences(dev, 1, &fence);
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    VkImageMemoryBarrier to_transfer{};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = static_cast<VkImage>(slot.image);
    to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_transfer.subresourceRange.levelCount = 1;
    to_transfer.subresourceRange.layerCount = 1;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_transfer);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {width, height, 1};
    copy.bufferOffset = ring_offset;
    vkCmdCopyBufferToImage(cb, static_cast<VkBuffer>(staging_buffer_),
                           static_cast<VkImage>(slot.image),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier to_sample = to_transfer;
    to_sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_sample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_sample);

    vkEndCommandBuffer(cb);

    if (auto submitted = submit_current_slot(false); !submitted)
        return submitted.get_error();

    slot.layout_ready = true;
    return {};
#else
    (void)texture_id;
    (void)width;
    (void)height;
    (void)rgba;
    return lx::not_implemented("lx::gfx::vulkan_compositor::upload_rgba");
#endif
}

lx::result<void> lx::gfx::vulkan_compositor::ensure_staging_ring(unsigned need_bytes) {
#if defined(LUMEN_HAS_VULKAN)
    if (staging_buffer_ && staging_capacity_ >= need_bytes)
        return {};
    auto dev = static_cast<VkDevice>(ctx_.device);
    auto phys = static_cast<VkPhysicalDevice>(ctx_.physical_device);
    if (!dev || !phys)
        return lx::not_implemented("lx::gfx::vulkan_compositor::ensure_staging_ring");

    const VkDeviceSize capacity =
        need_bytes > k_staging_ring_bytes ? static_cast<VkDeviceSize>(need_bytes)
                                          : static_cast<VkDeviceSize>(k_staging_ring_bytes);

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = capacity;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev, &bci, nullptr, &buffer) != VK_SUCCESS)
        return vk_error("staging ring vkCreateBuffer failed");

    VkMemoryRequirements buffer_reqs{};
    vkGetBufferMemoryRequirements(dev, buffer, &buffer_reqs);
    const int staging_type =
        find_memory_type(phys, buffer_reqs.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (staging_type < 0) {
        vkDestroyBuffer(dev, buffer, nullptr);
        return vk_error("no host-visible memory type for staging ring");
    }

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = buffer_reqs.size;
    mai.memoryTypeIndex = static_cast<uint32_t>(staging_type);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &mai, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(dev, buffer, nullptr);
        return vk_error("staging ring vkAllocateMemory failed");
    }
    if (vkBindBufferMemory(dev, buffer, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(dev, memory, nullptr);
        vkDestroyBuffer(dev, buffer, nullptr);
        return vk_error("staging ring vkBindBufferMemory failed");
    }

    void* mapped = nullptr;
    if (vkMapMemory(dev, memory, 0, capacity, 0, &mapped) != VK_SUCCESS) {
        vkFreeMemory(dev, memory, nullptr);
        vkDestroyBuffer(dev, buffer, nullptr);
        return vk_error("staging ring vkMapMemory failed");
    }

    staging_buffer_ = buffer;
    staging_memory_ = memory;
    staging_mapped_ = static_cast<unsigned char*>(mapped);
    staging_capacity_ = static_cast<unsigned>(capacity);
    staging_offset_ = 0;
    return {};
#else
    (void)need_bytes;
    return lx::not_implemented("lx::gfx::vulkan_compositor::ensure_staging_ring");
#endif
}

lx::result<void> lx::gfx::vulkan_compositor::submit_current_slot(bool wait_for_completion) {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    auto queue = static_cast<VkQueue>(ctx_.queue);
    const unsigned slot = submit_slot_;
    auto cb = static_cast<VkCommandBuffer>(command_buffers_[slot]);
    auto fence = static_cast<VkFence>(fences_[slot]);
    if (!dev || !queue || !cb || !fence)
        return lx::not_implemented("lx::gfx::vulkan_compositor::submit_current_slot");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS)
        return vk_error("vkQueueSubmit failed");

    if (wait_for_completion) {
        const VkResult waited = vkWaitForFences(dev, 1, &fence, VK_TRUE, 1'000'000'000ull);
        if (waited != VK_SUCCESS)
            return vk_error("vkWaitForFences timed out");
    }

    submit_slot_ = (submit_slot_ + 1u) % k_frame_slots;
    return {};
#else
    (void)wait_for_completion;
    return lx::not_implemented("lx::gfx::vulkan_compositor::submit_current_slot");
#endif
}

lx::result<void> lx::gfx::vulkan_compositor::ensure_framebuffer(const render_target& target) {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(ctx_.device);
    if (!dev || !render_pass_ || !target.valid())
        return lx::not_implemented("lx::gfx::vulkan_compositor::ensure_framebuffer");

    int free_slot = -1;
    for (unsigned i = 0; i < k_max_framebuffers; ++i) {
        if (framebuffers_[i].framebuffer && framebuffers_[i].image == target.vk_image()) {
            framebuffer_ = framebuffers_[i].framebuffer;
            return {};
        }
        if (!framebuffers_[i].framebuffer && free_slot < 0)
            free_slot = static_cast<int>(i);
    }

    if (free_slot < 0) {
        // More distinct targets than slots: evict the first rather than grow unbounded.
        vkDestroyFramebuffer(dev, static_cast<VkFramebuffer>(framebuffers_[0].framebuffer),
                             nullptr);
        framebuffers_[0] = {};
        free_slot = 0;
    }

    VkImageView attachment = static_cast<VkImageView>(target.vk_image_view());
    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = static_cast<VkRenderPass>(render_pass_);
    fbci.attachmentCount = 1;
    fbci.pAttachments = &attachment;
    fbci.width = target.width();
    fbci.height = target.height();
    fbci.layers = 1;

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(dev, &fbci, nullptr, &framebuffer) != VK_SUCCESS)
        return vk_error("vkCreateFramebuffer failed");
    framebuffers_[static_cast<unsigned>(free_slot)] = {framebuffer, target.vk_image()};
    framebuffer_ = framebuffer;
    return {};
#else
    (void)target;
    return lx::not_implemented("lx::gfx::vulkan_compositor::ensure_framebuffer");
#endif
}

lx::result<lx::gfx::composite_stats> lx::gfx::vulkan_compositor::composite(
    render_target& target, lx::color clear, const blit_command* cmds, unsigned count) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_SHADERS)
    if (!ready())
        return lx::not_implemented("lx::gfx::vulkan_compositor::composite");
    if (!target.valid()) {
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "composite into an invalid render target");
    }
    if (auto framebuffer = ensure_framebuffer(target); !framebuffer)
        return framebuffer.get_error();

    const unsigned slot = submit_slot_;
    auto fence = static_cast<VkFence>(fences_[slot]);
    if (vkWaitForFences(static_cast<VkDevice>(ctx_.device), 1, &fence, VK_TRUE,
                        1'000'000'000ull) != VK_SUCCESS)
        return vk_error("vkWaitForFences timed out reclaiming composite slot");
    vkResetFences(static_cast<VkDevice>(ctx_.device), 1, &fence);

    auto cb = static_cast<VkCommandBuffer>(command_buffers_[slot]);
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    // Acquire freshly imported client images. A dma-buf written by another device has no
    // Vulkan release to pair with, so the acquire comes from the foreign queue family and
    // the modifier — not a Vulkan layout — is what guarantees the contents survive.
    for (unsigned i = 0; i < k_max_textures; ++i) {
        auto& slot = textures_[i];
        if (!slot.used || slot.layout_ready || !slot.image || slot.owns_image)
            continue;

        VkImageMemoryBarrier acquire{};
        acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        acquire.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        acquire.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        acquire.srcQueueFamilyIndex = ctx_.supports_queue_family_foreign
                                          ? VK_QUEUE_FAMILY_FOREIGN_EXT
                                          : VK_QUEUE_FAMILY_IGNORED;
        acquire.dstQueueFamilyIndex = ctx_.supports_queue_family_foreign
                                          ? ctx_.queue_family
                                          : VK_QUEUE_FAMILY_IGNORED;
        acquire.image = static_cast<VkImage>(slot.image);
        acquire.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        acquire.subresourceRange.levelCount = 1;
        acquire.subresourceRange.layerCount = 1;
        acquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &acquire);
        slot.layout_ready = true;
    }

    VkClearValue clear_value{};
    clear_value.color = {{clear.r, clear.g, clear.b, clear.a}};

    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = static_cast<VkRenderPass>(render_pass_);
    rpbi.framebuffer = static_cast<VkFramebuffer>(framebuffer_);
    rpbi.renderArea.extent = {target.width(), target.height()};
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear_value;
    vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(target.width());
    viewport.height = static_cast<float>(target.height());
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(cb, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = {target.width(), target.height()};
    vkCmdSetScissor(cb, 0, 1, &scissor);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, static_cast<VkPipeline>(pipeline_));

    composite_stats stats{};
    for (unsigned i = 0; i < count && cmds; ++i) {
        const auto& cmd = cmds[i];
        const int index = find_texture(cmd.texture_id);
        if (index < 0) {
            ++stats.draws_skipped;
            continue;
        }
        const auto& slot = textures_[static_cast<unsigned>(index)];
        if (!slot.descriptor_set) {
            ++stats.draws_skipped;
            continue;
        }

        auto set = static_cast<VkDescriptorSet>(slot.descriptor_set);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                static_cast<VkPipelineLayout>(pipeline_layout_), 0, 1, &set, 0,
                                nullptr);

        // Clip is a scissor, which costs nothing extra here — the pipeline already
        // declares scissor as dynamic state. An empty clip means unclipped.
        VkRect2D draw_scissor = scissor;
        if (cmd.clip.width > 0 && cmd.clip.height > 0) {
            const int x0 = cmd.clip.x > 0 ? cmd.clip.x : 0;
            const int y0 = cmd.clip.y > 0 ? cmd.clip.y : 0;
            const int x1 = cmd.clip.x + cmd.clip.width;
            const int y1 = cmd.clip.y + cmd.clip.height;
            const int max_x = static_cast<int>(target.width());
            const int max_y = static_cast<int>(target.height());
            const int cx1 = x1 < max_x ? x1 : max_x;
            const int cy1 = y1 < max_y ? y1 : max_y;
            if (cx1 <= x0 || cy1 <= y0) {
                ++stats.draws_culled;
                continue; // clipped away entirely
            }
            draw_scissor.offset = {x0, y0};
            draw_scissor.extent = {static_cast<unsigned>(cx1 - x0),
                                   static_cast<unsigned>(cy1 - y0)};
        }
        vkCmdSetScissor(cb, 0, 1, &draw_scissor);

        quad_push push{};
        push.dst[0] = static_cast<float>(cmd.dst.x);
        push.dst[1] = static_cast<float>(cmd.dst.y);
        push.dst[2] = static_cast<float>(cmd.dst.width);
        push.dst[3] = static_cast<float>(cmd.dst.height);
        push.target[0] = static_cast<float>(target.width());
        push.target[1] = static_cast<float>(target.height());
        push.opacity = cmd.opacity;

        // Source rect in normalized coordinates. An empty src means the whole texture,
        // which keeps the default {0,0,1,1} and reproduces the old behavior exactly.
        if (slot.width > 0 && slot.height > 0) {
            const float tw = static_cast<float>(slot.width);
            const float th = static_cast<float>(slot.height);
            if (cmd.src.width > 0 && cmd.src.height > 0) {
                push.src_uv[0] = static_cast<float>(cmd.src.x) / tw;
                push.src_uv[1] = static_cast<float>(cmd.src.y) / th;
                push.src_uv[2] = static_cast<float>(cmd.src.width) / tw;
                push.src_uv[3] = static_cast<float>(cmd.src.height) / th;
            }
            // Inset by half a texel on each side: linear filtering samples between texel
            // centers, so without this a magnified crop reads its neighbours across the
            // crop edge. Degenerate rects collapse to the center rather than inverting.
            const float half_u = 0.5f / tw;
            const float half_v = 0.5f / th;
            float u_lo = push.src_uv[0] + half_u;
            float v_lo = push.src_uv[1] + half_v;
            float u_hi = push.src_uv[0] + push.src_uv[2] - half_u;
            float v_hi = push.src_uv[1] + push.src_uv[3] - half_v;
            if (u_lo > u_hi) u_lo = u_hi = 0.5f * (u_lo + u_hi);
            if (v_lo > v_hi) v_lo = v_hi = 0.5f * (v_lo + v_hi);
            push.src_bounds[0] = u_lo;
            push.src_bounds[1] = v_lo;
            push.src_bounds[2] = u_hi;
            push.src_bounds[3] = v_hi;
        }

        push.tint[0] = cmd.tint.r;
        push.tint[1] = cmd.tint.g;
        push.tint[2] = cmd.tint.b;
        push.tint[3] = cmd.tint.a;
        push.transfer = transfer_slot(cmd.src_transfer);

        vkCmdPushConstants(cb, static_cast<VkPipelineLayout>(pipeline_layout_),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);

        vkCmdDraw(cb, 6, 1, 0, 0);
        ++stats.draws_submitted;
    }

    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    if (auto submitted = submit_current_slot(true); !submitted)
        return submitted.get_error();
    return stats;
#else
    (void)target;
    (void)clear;
    (void)cmds;
    (void)count;
    return lx::not_implemented("lx::gfx::vulkan_compositor::composite");
#endif
}

lx::result<void> lx::gfx::vulkan_compositor::read_back(const render_target& target,
                                                        unsigned char* rgba,
                                                        unsigned capacity) {
#if defined(LUMEN_HAS_VULKAN)
    if (!ready() || !target.valid())
        return lx::not_implemented("lx::gfx::vulkan_compositor::read_back");
    const unsigned needed = target.width() * target.height() * 4u;
    if (!rgba || capacity < needed) {
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "read_back destination too small");
    }

    auto dev = static_cast<VkDevice>(ctx_.device);
    auto phys = static_cast<VkPhysicalDevice>(ctx_.physical_device);

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = needed;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer staging = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev, &bci, nullptr, &staging) != VK_SUCCESS)
        return vk_error("read_back vkCreateBuffer failed");

    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(dev, staging, &reqs);
    const int type_index =
        find_memory_type(phys, reqs.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type_index < 0) {
        vkDestroyBuffer(dev, staging, nullptr);
        return vk_error("no host-visible memory type for read_back");
    }

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = reqs.size;
    mai.memoryTypeIndex = static_cast<uint32_t>(type_index);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &mai, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(dev, staging, nullptr);
        return vk_error("read_back vkAllocateMemory failed");
    }
    if (vkBindBufferMemory(dev, staging, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(dev, memory, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        return vk_error("read_back vkBindBufferMemory failed");
    }

    const unsigned frame_slot = submit_slot_;
    auto fence = static_cast<VkFence>(fences_[frame_slot]);
    if (vkWaitForFences(dev, 1, &fence, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS)
        return vk_error("vkWaitForFences timed out reclaiming read_back slot");
    vkResetFences(dev, 1, &fence);

    auto cb = static_cast<VkCommandBuffer>(command_buffers_[frame_slot]);
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = static_cast<VkImage>(target.vk_image());
    to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_src.subresourceRange.levelCount = 1;
    to_src.subresourceRange.layerCount = 1;
    to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {target.width(), target.height(), 1};
    vkCmdCopyImageToBuffer(cb, static_cast<VkImage>(target.vk_image()),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);

    VkImageMemoryBarrier back = to_src;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = 0;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &back);

    vkEndCommandBuffer(cb);

    if (auto submitted = submit_current_slot(true); !submitted)
        return submitted.get_error();

    void* mapped = nullptr;
    if (vkMapMemory(dev, memory, 0, needed, 0, &mapped) == VK_SUCCESS) {
        // The target is B8G8R8A8; callers expect tightly packed RGBA.
        const auto* src = static_cast<const unsigned char*>(mapped);
        for (unsigned i = 0; i < needed; i += 4u) {
            rgba[i + 0] = src[i + 2];
            rgba[i + 1] = src[i + 1];
            rgba[i + 2] = src[i + 0];
            rgba[i + 3] = src[i + 3];
        }
        vkUnmapMemory(dev, memory);
    } else {
        vkFreeMemory(dev, memory, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        return vk_error("read_back vkMapMemory failed");
    }

    vkFreeMemory(dev, memory, nullptr);
    vkDestroyBuffer(dev, staging, nullptr);
    return {};
#else
    (void)target;
    (void)rgba;
    (void)capacity;
    return lx::not_implemented("lx::gfx::vulkan_compositor::read_back");
#endif
}
