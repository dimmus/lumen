module;

#if defined(LUMEN_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif
#include <unistd.h>

import lx.foundation;

export module lx.gfx:dmabuf;

export namespace lx::gfx {

struct dmabuf_plane {
    lx::unique_fd fd{};
    unsigned offset = 0;
    unsigned stride = 0;
    unsigned long long modifier = 0;
};

struct dmabuf_desc {
    unsigned width = 0;
    unsigned height = 0;
    lx::fourcc format = static_cast<lx::fourcc>(lx::pixel_format::argb8888);
    unsigned plane_count = 0;
    dmabuf_plane planes[4]{};
};

/// DRM_FORMAT_MOD_LINEAR / DRM_FORMAT_MOD_INVALID, mirrored to avoid a drm_fourcc.h
/// dependency in the graphics layer.
inline constexpr unsigned long long modifier_linear = 0ull;
inline constexpr unsigned long long modifier_invalid = 0x00ffffffffffffffull;

struct imported_image {
    unsigned image_id = 0;
    unsigned memory_id = 0;
    unsigned semaphore_id = 0;
    int layout = 0;
    unsigned width = 0;
    unsigned height = 0;
    lx::fourcc format = static_cast<lx::fourcc>(lx::pixel_format::argb8888);
    /// Opaque VkImage / VkDeviceMemory. Declared unconditionally so callers can pass
    /// them across the UI-to-render handoff without Vulkan-conditional code.
    void* vk_image = nullptr;
    void* vk_memory = nullptr;
};

class dmabuf_importer {
public:
    dmabuf_importer() = default;
    dmabuf_importer(void* vk_device, void* vk_physical_device,
                    bool supports_drm_format_modifier = false);

    [[nodiscard]] bool can_import(const dmabuf_desc& desc) const;

    /// Imports `desc` as a sampled Vulkan image. Plane FDs are borrowed: the importer
    /// duplicates them, because a successful Vulkan import consumes the FD it is given.
    [[nodiscard]] lx::result<imported_image> import(const dmabuf_desc& desc);
    void release(imported_image& image);

    void set_allowed_modifiers(const unsigned long long* mods, unsigned count);

    /// True when this importer talks to a real Vulkan device (as opposed to the
    /// bookkeeping-only soft importer used by headless CI).
    [[nodiscard]] bool is_hardware() const { return device_ != nullptr; }

private:
#if defined(LUMEN_HAS_VULKAN)
    [[nodiscard]] lx::result<imported_image> import_vulkan(const dmabuf_desc& desc);
#endif

    static constexpr unsigned k_max_allowed_modifiers = 32;

    void* device_ = nullptr;
    void* physical_device_ = nullptr;
    bool drm_format_modifier_ = false;
    unsigned next_image_id_ = 1;
    unsigned long long allowed_modifiers_[k_max_allowed_modifiers]{};
    unsigned allowed_modifier_count_ = 0;
};

/// Export a Vulkan image as a dmabuf FD for DRM scanout (`drmModeAddFB2WithModifiers`).
struct exported_dmabuf {
    lx::unique_fd fd{};
    unsigned width = 0;
    unsigned height = 0;
    unsigned stride = 0;
    unsigned offset = 0;
    lx::fourcc format = static_cast<lx::fourcc>(lx::pixel_format::argb8888);
    unsigned long long modifier = 0;
};

class dmabuf_exporter {
public:
    dmabuf_exporter() = default;
    dmabuf_exporter(void* vk_device, void* vk_physical_device);

    /// Export a Vulkan image whose memory was allocated with an
    /// `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT` export info. The row pitch and
    /// offset are read back from the driver rather than assumed, because a linear image
    /// is free to pad its rows.
    [[nodiscard]] lx::result<exported_dmabuf> export_image(const imported_image& image);

    [[nodiscard]] bool is_hardware() const { return device_ != nullptr; }

private:
    void* device_ = nullptr;
    void* physical_device_ = nullptr;
};

} // namespace lx::gfx


lx::gfx::dmabuf_importer::dmabuf_importer(void* dev, void* phys, bool drm_format_modifier)
    : device_{dev}, physical_device_{phys}, drm_format_modifier_{drm_format_modifier} {}

bool lx::gfx::dmabuf_importer::can_import(const dmabuf_desc& desc) const {
    if (desc.width == 0 || desc.height == 0 || desc.plane_count == 0 || desc.plane_count > 4)
        return false;
    if (allowed_modifier_count_ == 0)
        return true;
    for (unsigned i = 0; i < allowed_modifier_count_; ++i) {
        if (allowed_modifiers_[i] == desc.planes[0].modifier)
            return true;
    }
    return false;
}

#if defined(LUMEN_HAS_VULKAN)
namespace {

VkFormat fourcc_to_vk_format(lx::fourcc fmt) {
    switch (static_cast<lx::pixel_format>(fmt)) {
    case lx::pixel_format::argb8888:
    case lx::pixel_format::xrgb8888:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case lx::pixel_format::rgba8888:
        return VK_FORMAT_R8G8B8A8_UNORM;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

} // namespace

lx::result<lx::gfx::imported_image> lx::gfx::dmabuf_importer::import_vulkan(
    const dmabuf_desc& desc) {
    auto* vkdev = static_cast<VkDevice>(device_);
    auto* phys = static_cast<VkPhysicalDevice>(physical_device_);
    if (!vkdev || !phys)
        return lx::not_implemented("lx::gfx::dmabuf_importer::import_vulkan");

    const int borrowed_fd = desc.planes[0].fd.get();
    if (borrowed_fd < 0) {
        // A hardware importer with no FD means the protocol layer lost the plane FD on
        // the way here. Reporting success would fabricate an image that samples nothing.
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "dmabuf import without a plane fd");
    }

    const VkFormat vk_fmt = fourcc_to_vk_format(desc.format);
    if (vk_fmt == VK_FORMAT_UNDEFINED) {
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "unsupported dmabuf format");
    }

    const unsigned long long modifier = desc.planes[0].modifier;
    const bool implicit_layout = modifier == modifier_linear || modifier == modifier_invalid;
    if (!drm_format_modifier_ && !implicit_layout) {
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "tiled dmabuf needs VK_EXT_image_drm_format_modifier");
    }
    if (!drm_format_modifier_ && desc.plane_count > 1) {
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "multi-plane dmabuf needs VK_EXT_image_drm_format_modifier");
    }

    VkExternalMemoryImageCreateInfo ext_img{};
    ext_img.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    // Explicit layout is mandatory when importing: the exporter already fixed the
    // tiling and per-plane offsets, so the driver must not choose its own.
    VkSubresourceLayout plane_layouts[4]{};
    for (unsigned i = 0; i < desc.plane_count && i < 4; ++i) {
        plane_layouts[i].offset = desc.planes[i].offset;
        plane_layouts[i].rowPitch = desc.planes[i].stride;
    }

    VkImageDrmFormatModifierExplicitCreateInfoEXT mod_info{};
    mod_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    mod_info.drmFormatModifier = modifier == modifier_invalid ? modifier_linear : modifier;
    mod_info.drmFormatModifierPlaneCount = desc.plane_count;
    mod_info.pPlaneLayouts = plane_layouts;

    const bool use_modifier = drm_format_modifier_;
    if (use_modifier)
        ext_img.pNext = &mod_info;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &ext_img;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = vk_fmt;
    ici.extent = {desc.width, desc.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = use_modifier ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
                              : VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(vkdev, &ici, nullptr, &image) != VK_SUCCESS) {
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "vkCreateImage failed");
    }

    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(vkdev, image, &reqs);

    // The set of memory types valid for an imported dma-buf comes from the driver, not
    // from a DEVICE_LOCAL guess — intersect it with the image's requirements.
    uint32_t type_bits = reqs.memoryTypeBits;
    if (auto get_fd_props = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
            vkGetDeviceProcAddr(vkdev, "vkGetMemoryFdPropertiesKHR"))) {
        VkMemoryFdPropertiesKHR fd_props{};
        fd_props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
        if (get_fd_props(vkdev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, borrowed_fd,
                         &fd_props) == VK_SUCCESS) {
            type_bits &= fd_props.memoryTypeBits;
        }
    }
    if (type_bits == 0) {
        vkDestroyImage(vkdev, image, nullptr);
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "no memory type accepts this dmabuf");
    }

    uint32_t type_index = 0;
    for (uint32_t i = 0; i < 32; ++i) {
        if (type_bits & (1u << i)) {
            type_index = i;
            break;
        }
    }

    // A successful import consumes the FD, but the buffer entry keeps ownership so the
    // same client buffer can be re-imported after a cache eviction.
    const int owned_fd = ::dup(borrowed_fd);
    if (owned_fd < 0) {
        vkDestroyImage(vkdev, image, nullptr);
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "dup of dmabuf fd failed");
    }

    VkImportMemoryFdInfoKHR import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_info.fd = owned_fd;

    // dma-buf image imports must be dedicated allocations.
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image;
    dedicated.pNext = &import_info;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &dedicated;
    mai.allocationSize = reqs.size;
    mai.memoryTypeIndex = type_index;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(vkdev, &mai, nullptr, &memory) != VK_SUCCESS) {
        ::close(owned_fd);
        vkDestroyImage(vkdev, image, nullptr);
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "vkAllocateMemory failed");
    }

    if (vkBindImageMemory(vkdev, image, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(vkdev, memory, nullptr);
        vkDestroyImage(vkdev, image, nullptr);
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "vkBindImageMemory failed");
    }

    imported_image out{};
    out.image_id = next_image_id_++;
    out.memory_id = out.image_id;
    out.width = desc.width;
    out.height = desc.height;
    out.format = desc.format;
    out.vk_image = image;
    out.vk_memory = memory;
    return out;
}
#endif

lx::result<lx::gfx::imported_image> lx::gfx::dmabuf_importer::import(const dmabuf_desc& desc) {
    if (!can_import(desc)) {
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "invalid dmabuf desc");
    }

#if defined(LUMEN_HAS_VULKAN)
    if (device_)
        return import_vulkan(desc);
#endif

    // Soft import: bookkeeping identity only, for headless CI where no GPU exists.
    imported_image image{};
    image.image_id = next_image_id_++;
    if (image.image_id == 0)
        image.image_id = next_image_id_++;
    image.memory_id = image.image_id;
    image.width = desc.width;
    image.height = desc.height;
    image.format = desc.format;
    return image;
}

void lx::gfx::dmabuf_importer::release(imported_image& img) {
#if defined(LUMEN_HAS_VULKAN)
    if (device_ && img.vk_image) {
        auto* vkdev = static_cast<VkDevice>(device_);
        vkDestroyImage(vkdev, static_cast<VkImage>(img.vk_image), nullptr);
        if (img.vk_memory)
            vkFreeMemory(vkdev, static_cast<VkDeviceMemory>(img.vk_memory), nullptr);
    }
#endif
    img = {};
}

void lx::gfx::dmabuf_importer::set_allowed_modifiers(const unsigned long long* mods,
                                                     unsigned count) {
    allowed_modifier_count_ = 0;
    if (!mods)
        return;
    for (unsigned i = 0; i < count && i < k_max_allowed_modifiers; ++i)
        allowed_modifiers_[allowed_modifier_count_++] = mods[i];
}

lx::gfx::dmabuf_exporter::dmabuf_exporter(void* dev, void* phys)
    : device_{dev}, physical_device_{phys} {}

lx::result<lx::gfx::exported_dmabuf> lx::gfx::dmabuf_exporter::export_image(
    const imported_image& image) {
#if defined(LUMEN_HAS_VULKAN)
    auto* vkdev = static_cast<VkDevice>(device_);
    if (!vkdev || !image.vk_memory || !image.vk_image) {
        return lx::not_implemented("lx::gfx::dmabuf_exporter::export_image");
    }
    auto get_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(vkdev, "vkGetMemoryFdKHR"));
    if (!get_fd) {
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "vkGetMemoryFdKHR unavailable");
    }

    VkImageSubresource subresource{};
    subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubresourceLayout layout{};
    vkGetImageSubresourceLayout(vkdev, static_cast<VkImage>(image.vk_image), &subresource,
                                &layout);

    VkMemoryGetFdInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    info.memory = static_cast<VkDeviceMemory>(image.vk_memory);
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    if (get_fd(vkdev, &info, &fd) != VK_SUCCESS || fd < 0) {
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "vkGetMemoryFdKHR failed");
    }

    exported_dmabuf out{};
    out.fd.reset(fd);
    out.width = image.width;
    out.height = image.height;
    out.stride = static_cast<unsigned>(layout.rowPitch);
    out.offset = static_cast<unsigned>(layout.offset);
    out.format = image.format;
    out.modifier = modifier_linear;
    return out;
#else
    (void)image;
    return lx::not_implemented("lx::gfx::dmabuf_exporter::export_image");
#endif
}

