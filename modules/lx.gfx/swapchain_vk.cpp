module;

// The Vulkan tier of `lx.gfx:swapchain` (ADR-010 §4.10 item 1): a `gfx::device` on the
// physical device whose DRM node is the render node (VK_EXT_physical_device_drm), images with
// VK_EXT_image_drm_format_modifier from the feedback's list filtered by what the device can
// render to and export (vkGetPhysicalDeviceImageFormatProperties2), memory exported as a
// dma-buf (VK_EXT_external_memory_dma_buf + VK_KHR_external_memory_fd), drawn by :ui_vk.
//
// Ownership: the image belongs to the foreign queue family (VK_EXT_queue_family_foreign) in
// GENERAL while the compositor has it. Each image has three command buffers recorded once at
// creation — first use (UNDEFINED → COLOR_ATTACHMENT_OPTIMAL), acquire (foreign/GENERAL →
// ours/COLOR_ATTACHMENT_OPTIMAL) and release (back) — submitted around :ui_vk's draw on the
// same queue, so a frame records nothing and allocates nothing.
//
// Explicit sync: a timeline semaphore per image, exported as an opaque fd — a DRM syncobj on
// Mesa's DRM drivers, what wp_linux_drm_syncobj_manager_v1.import_timeline takes and what the
// compositor's `drm_timeline_table::import` (drmSyncobjFDToHandle) expects. The release submit
// signals the acquire point; the release point is the compositor's, waited on our render node
// (the same syncobj imported there, an eventfd). A device whose opaque fd is not a syncobj
// (lavapipe) falls back to implicit sync.
//
// Implicit sync: the release submit signals a binary semaphore exported as a sync_file, which
// DMA_BUF_IOCTL_IMPORT_SYNC_FILE attaches to the dma-buf as its write fence (what Mesa's WSI
// does for implicit-sync compositors); before the image is drawn again,
// DMA_BUF_IOCTL_EXPORT_SYNC_FILE of the compositor's read fences is imported into a binary
// semaphore the acquire submit waits on. Without those ioctls (kernel < 6.0) the release
// waits for its fence on the CPU (logged once).

#if defined(LUMEN_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif
#include <errno.h>
#include <linux/dma-buf.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

module lx.gfx;

import std;

import lx.foundation;
import lx.runtime;
import lx.trace;
import :vk_renderer;
import :ui_vk;
import :drm_timeline;
import :swapchain;

namespace lx::gfx {

namespace {

constexpr lx::trace::category log_swapchain_vk{"gfx.swapchain"};

[[maybe_unused]] std::uint64_t vk_now_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

#if defined(LUMEN_HAS_VULKAN)
enum vk_fn : unsigned {
    fn_get_memory_fd,
    fn_get_semaphore_fd,
    fn_import_semaphore_fd,
    fn_modifier_properties,
    fn_count,
};
static_assert(fn_count <= 12);

[[nodiscard]] VkFormat vk_format_of(std::uint32_t fourcc) {
    switch (fourcc) {
    case k_fourcc_argb8888:
    case k_fourcc_xrgb8888: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

[[nodiscard]] lx::error vk_error(lx::vulkan_err code, const char* message) {
    return lx::make_error(code, message);
}

constexpr VkImageUsageFlags k_usage =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
#endif

} // namespace

// ── client_gpu (Vulkan half and the parts that own the type-erased device) ──────

void client_gpu::destroy() {
    delete static_cast<lx::gfx::device*>(vk_);
    vk_ = nullptr;
    egl_ = egl_device{};
    node_ = render_node{};
    tier_ = client_tier::none;
    software_ = false;
    forced_ = false;
    timeline_ = false;
    name_[0] = '\0';
    for (auto& r : refused_)
        r[0] = '\0';
}

vk_context client_gpu::vulkan_context() const {
    if (tier_ != client_tier::vulkan || !vk_)
        return {};
    return static_cast<const lx::gfx::device*>(vk_)->context();
}

lx::result<void> client_gpu::try_vulkan(bool forced, bool allow_software) {
#if defined(LUMEN_HAS_VULKAN)
    // A software device is refused before it is created where possible: lavapipe's device
    // costs ~28 MB of heap and its JIT. `probe_devices` makes an instance only.
    if (!(forced && allow_software)) {
        // As the compositor probes (compositor_impl's start-up): the loader skips software
        // ICDs, so asking costs no lavapipe load (~15 of the probe's 20 ms on this VM).
        // UI thread at window creation, before any thread of ours writes the environment.
        physical_device_list list{};
        {
            const hardware_probe_scope hardware_only;
            list = device_selector::probe_devices();
        }
        bool hardware = false;
        for (unsigned i = 0; i < list.count; ++i)
            hardware = hardware || list.devices[i].kind != physical_device_kind::cpu;
        if (!hardware) {
            refuse(client_tier::vulkan, list.count ? "software Vulkan device (lavapipe)"
                                                   : "no hardware Vulkan device");
            return vk_error(lx::vulkan_err::unsupported, "no hardware Vulkan device");
        }
    }
    device_info info{};
    info.device_name = "vulkan";
    info.supports_dmabuf_import = true;
    info.card = {true, ::major(static_cast<dev_t>(node_.dev)), ::minor(static_cast<dev_t>(node_.dev))};
    auto made = lx::gfx::device::create(info);
    if (!made) {
        refuse(client_tier::vulkan, "no Vulkan device with dma-buf export");
        return made.get_error();
    }
    auto* dev = new lx::gfx::device(std::move(made).value());
    const device_info di = dev->info();
    const auto drop = [&](const char* why, lx::vulkan_err code) {
        refuse(client_tier::vulkan, why);
        delete dev;
        return vk_error(code, why);
    };
    if (!dev->vk_device())
        return drop("no Vulkan device with dma-buf export", lx::vulkan_err::unsupported);
    if (di.is_software_rasterizer && !(forced && allow_software))
        return drop("software Vulkan device (lavapipe)", lx::vulkan_err::unsupported);
    // The device must be the compositor's: a buffer from another GPU would cross PCIe (or
    // fail to import) every frame. Without node information the match is unknown: only a
    // hardware device the caller forced, or a node that was not matched to begin with.
    const bool node_match =
        di.match == device_match::primary_node || di.match == device_match::render_node;
    if (!node_match && !di.is_software_rasterizer && node_.matched && !forced)
        return drop("no Vulkan device owns the render node", lx::vulkan_err::unsupported);
    if (!di.supports_drm_format_modifier || !di.supports_dmabuf_export)
        return drop("no VK_EXT_image_drm_format_modifier / dma-buf export",
                    lx::vulkan_err::unsupported);
    vk_ = dev;
    software_ = di.is_software_rasterizer;
    std::snprintf(name_, sizeof(name_), "%s", di.device_name ? di.device_name : "vulkan");
    return {};
#else
    (void)forced;
    (void)allow_software;
    refuse(client_tier::vulkan, "built without Vulkan");
    return lx::not_implemented("lx::gfx::client_gpu::try_vulkan");
#endif
}

// ── client_swapchain, Vulkan ─────────────────────────────────────────────────────

lx::result<void> client_swapchain::make_vk_pool() {
#if defined(LUMEN_HAS_VULKAN)
    const vk_context ctx = gpu_->vulkan_context();
    if (!ctx.valid())
        return lx::not_implemented("lx::gfx::client_swapchain::make_vk_pool");
    auto dev = static_cast<VkDevice>(ctx.device);
    auto phys = static_cast<VkPhysicalDevice>(ctx.physical_device);
    vk_fns_[fn_get_memory_fd] = reinterpret_cast<void*>(vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR"));
    vk_fns_[fn_get_semaphore_fd] =
        reinterpret_cast<void*>(vkGetDeviceProcAddr(dev, "vkGetSemaphoreFdKHR"));
    vk_fns_[fn_import_semaphore_fd] =
        reinterpret_cast<void*>(vkGetDeviceProcAddr(dev, "vkImportSemaphoreFdKHR"));
    vk_fns_[fn_modifier_properties] = reinterpret_cast<void*>(
        vkGetDeviceProcAddr(dev, "vkGetImageDrmFormatModifierPropertiesEXT"));
    if (!vk_fns_[fn_get_memory_fd] || !vk_fns_[fn_modifier_properties])
        return vk_error(lx::vulkan_err::unsupported, "dma-buf export entry points missing");
    vk_foreign_ = ctx.supports_queue_family_foreign;

    const VkFormat format = vk_format_of(fourcc_);
    if (format == VK_FORMAT_UNDEFINED)
        return vk_error(lx::vulkan_err::unsupported, "no Vulkan format for the fourcc");

    // The feedback's modifiers, kept only where the device renders to and exports them.
    VkDrmFormatModifierPropertiesEXT props[64]{};
    VkDrmFormatModifierPropertiesListEXT list{};
    list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 fp{};
    fp.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    fp.pNext = &list;
    vkGetPhysicalDeviceFormatProperties2(phys, format, &fp);
    if (list.drmFormatModifierCount > 64)
        list.drmFormatModifierCount = 64;
    list.pDrmFormatModifierProperties = props;
    vkGetPhysicalDeviceFormatProperties2(phys, format, &fp);

    format_choice kept = options_.format;
    kept.modifier_count = 0;
    for (std::uint64_t m : options_.format.list()) {
        const VkDrmFormatModifierPropertiesEXT* p = nullptr;
        for (unsigned i = 0; i < list.drmFormatModifierCount; ++i)
            if (props[i].drmFormatModifier == m)
                p = &props[i];
        constexpr VkFormatFeatureFlags need =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        if (!p || (p->drmFormatModifierTilingFeatures & need) != need ||
            p->drmFormatModifierPlaneCount > 4)
            continue;
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info{};
        mod_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
        mod_info.drmFormatModifier = m;
        mod_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkPhysicalDeviceExternalImageFormatInfo ext_info{};
        ext_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        ext_info.pNext = &mod_info;
        ext_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkPhysicalDeviceImageFormatInfo2 info{};
        info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        info.pNext = &ext_info;
        info.format = format;
        info.type = VK_IMAGE_TYPE_2D;
        info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        info.usage = k_usage;
        VkExternalImageFormatProperties ext_props{};
        ext_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        VkImageFormatProperties2 out{};
        out.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        out.pNext = &ext_props;
        if (vkGetPhysicalDeviceImageFormatProperties2(phys, &info, &out) != VK_SUCCESS)
            continue;
        if (!(ext_props.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
            continue;
        if (out.imageFormatProperties.maxExtent.width < 16384 &&
            (out.imageFormatProperties.maxExtent.width < width_ ||
             out.imageFormatProperties.maxExtent.height < height_))
            continue;
        kept.modifiers[kept.modifier_count++] = m;
    }
    if (kept.modifier_count == 0)
        return vk_error(lx::vulkan_err::unsupported,
                        "no offered modifier is renderable and exportable on the device");
    options_.format = kept;

    // Binary semaphores as sync_files: the implicit path's fences.
    VkPhysicalDeviceExternalSemaphoreInfo sem_info{};
    sem_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
    sem_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkExternalSemaphoreProperties sem_props{};
    sem_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
    vkGetPhysicalDeviceExternalSemaphoreProperties(phys, &sem_info, &sem_props);
    constexpr VkExternalSemaphoreFeatureFlags both =
        VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    vk_sync_file_ = ctx.supports_external_semaphore_fd && vk_fns_[fn_get_semaphore_fd] &&
                    vk_fns_[fn_import_semaphore_fd] &&
                    (sem_props.externalSemaphoreFeatures & both) == both;
    if (explicit_ && !(ctx.supports_external_semaphore_fd && vk_fns_[fn_get_semaphore_fd])) {
        log_swapchain_vk.info("explicit sync unavailable on Vulkan (no external semaphore fd): "
                              "implicit sync");
        explicit_ = false;
    }

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = ctx.queue_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(dev, &pci, nullptr, &pool) != VK_SUCCESS)
        return vk_error(lx::vulkan_err::out_of_memory, "swapchain command pool");
    vk_pool_ = pool;
    return {};
#else
    return lx::not_implemented("lx::gfx::client_swapchain::make_vk_pool");
#endif
}

void client_swapchain::destroy_vk_pool() {
#if defined(LUMEN_HAS_VULKAN)
    if (vk_pool_ && gpu_) {
        const vk_context ctx = gpu_->vulkan_context();
        if (ctx.device)
            vkDestroyCommandPool(static_cast<VkDevice>(ctx.device),
                                 static_cast<VkCommandPool>(vk_pool_), nullptr);
    }
#endif
    vk_pool_ = nullptr;
}

void client_swapchain::vk_wait_device() {
#if defined(LUMEN_HAS_VULKAN)
    if (!gpu_)
        return;
    const vk_context ctx = gpu_->vulkan_context();
    if (ctx.queue)
        (void)vkQueueWaitIdle(static_cast<VkQueue>(ctx.queue));
#endif
}

bool client_swapchain::vk_idle(const slot& s) const {
#if defined(LUMEN_HAS_VULKAN)
    if (!s.vk_submitted || !s.vk_fence)
        return true;
    const vk_context ctx = gpu_->vulkan_context();
    return vkGetFenceStatus(static_cast<VkDevice>(ctx.device), static_cast<VkFence>(s.vk_fence)) ==
           VK_SUCCESS;
#else
    (void)s;
    return true;
#endif
}

lx::result<void> client_swapchain::make_vk(slot& s) {
#if defined(LUMEN_HAS_VULKAN)
    const vk_context ctx = gpu_->vulkan_context();
    auto dev = static_cast<VkDevice>(ctx.device);
    auto phys = static_cast<VkPhysicalDevice>(ctx.physical_device);
    const VkFormat format = vk_format_of(fourcc_);

    VkImageDrmFormatModifierListCreateInfoEXT mods{};
    mods.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    mods.drmFormatModifierCount = options_.format.modifier_count;
    mods.pDrmFormatModifiers = options_.format.modifiers;
    VkExternalMemoryImageCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.pNext = &mods;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &ext;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width_, height_, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage = k_usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(dev, &ici, nullptr, &image) != VK_SUCCESS)
        return vk_error(lx::vulkan_err::out_of_memory, "vkCreateImage (drm format modifier)");
    s.vk_image = image;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, image, &req);
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    std::uint32_t type = UINT32_MAX;
    for (std::uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if (!(req.memoryTypeBits & (1u << i)))
            continue;
        if (type == UINT32_MAX || (mem.memoryTypes[i].propertyFlags &
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
        if (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            break;
    }
    if (type == UINT32_MAX)
        return vk_error(lx::vulkan_err::out_of_memory, "no memory type for the image");
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image;
    VkExportMemoryAllocateInfo export_info{};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.pNext = &dedicated;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &export_info;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &mai, nullptr, &memory) != VK_SUCCESS)
        return vk_error(lx::vulkan_err::out_of_memory, "vkAllocateMemory (exportable)");
    s.vk_memory = memory;
    if (vkBindImageMemory(dev, image, memory, 0) != VK_SUCCESS)
        return vk_error(lx::vulkan_err::out_of_memory, "vkBindImageMemory");

    VkImageDrmFormatModifierPropertiesEXT chosen{};
    chosen.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
    auto get_modifier =
        reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(vk_fns_[fn_modifier_properties]);
    if (get_modifier(dev, image, &chosen) != VK_SUCCESS)
        return vk_error(lx::vulkan_err::export_failed, "vkGetImageDrmFormatModifierPropertiesEXT");
    s.image.modifier = chosen.drmFormatModifier;

    unsigned planes = 1;
    {
        VkDrmFormatModifierPropertiesEXT props[64]{};
        VkDrmFormatModifierPropertiesListEXT list{};
        list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
        VkFormatProperties2 fp{};
        fp.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
        fp.pNext = &list;
        vkGetPhysicalDeviceFormatProperties2(phys, format, &fp);
        if (list.drmFormatModifierCount > 64)
            list.drmFormatModifierCount = 64;
        list.pDrmFormatModifierProperties = props;
        vkGetPhysicalDeviceFormatProperties2(phys, format, &fp);
        for (unsigned i = 0; i < list.drmFormatModifierCount; ++i)
            if (props[i].drmFormatModifier == chosen.drmFormatModifier)
                planes = props[i].drmFormatModifierPlaneCount;
    }
    if (planes < 1 || planes > 4)
        return vk_error(lx::vulkan_err::export_failed, "modifier plane count");

    VkMemoryGetFdInfoKHR fd_info{};
    fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory = memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    auto get_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vk_fns_[fn_get_memory_fd]);
    if (get_fd(dev, &fd_info, &fd) != VK_SUCCESS || fd < 0)
        return vk_error(lx::vulkan_err::export_failed, "vkGetMemoryFdKHR (dma-buf)");
    s.plane_fds[0].reset(fd);
    s.image.plane_count = planes;
    static constexpr VkImageAspectFlagBits k_aspect[4] = {
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT};
    for (unsigned p = 0; p < planes; ++p) {
        VkImageSubresource sub{};
        sub.aspectMask = k_aspect[p];
        VkSubresourceLayout layout{};
        vkGetImageSubresourceLayout(dev, image, &sub, &layout);
        // One memory object: every plane is the same dma-buf at its own offset.
        s.image.planes[p] = {fd, static_cast<std::uint32_t>(layout.offset),
                             static_cast<std::uint32_t>(layout.rowPitch)};
    }

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS)
        return vk_error(lx::vulkan_err::out_of_memory, "vkCreateImageView");
    s.vk_view = view;

    auto get_semaphore_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vk_fns_[fn_get_semaphore_fd]);
    if (explicit_) {
        VkSemaphoreTypeCreateInfo type_info{};
        type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkExportSemaphoreCreateInfo exp{};
        exp.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        exp.pNext = &type_info;
        exp.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sci.pNext = &exp;
        VkSemaphore timeline = VK_NULL_HANDLE;
        int sem_fd = -1;
        unsigned handle = 0;
        bool ok = vkCreateSemaphore(dev, &sci, nullptr, &timeline) == VK_SUCCESS;
        if (ok) {
            s.vk_timeline = timeline;
            VkSemaphoreGetFdInfoKHR gi{};
            gi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
            gi.semaphore = timeline;
            gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
            ok = get_semaphore_fd(dev, &gi, &sem_fd) == VK_SUCCESS && sem_fd >= 0;
        }
        // The opaque fd must be a DRM syncobj: that is what the compositor imports.
        if (ok)
            ok = ops_.fd_to_handle(ops_.user, sem_fd, &handle) == 0;
        if (ok) {
            s.timeline.reset(sem_fd);
            s.timeline_handle = handle;
        } else {
            if (sem_fd >= 0)
                ::close(sem_fd);
            if (explicit_settled_)
                // Images on the wire already carry timelines (a syncobj surface exists):
                // every commit needs its points, so this image fails rather than downgrade.
                return vk_error(lx::vulkan_err::export_failed,
                                "timeline semaphore for a new image not exportable as a DRM "
                                "syncobj (explicit sync settled)");
            log_swapchain_vk.info("explicit sync unavailable on Vulkan (the timeline semaphore "
                                  "is not exportable as a DRM syncobj): implicit sync");
            explicit_ = false;
        }
    }
    if (!explicit_ && vk_sync_file_) {
        VkExportSemaphoreCreateInfo exp{};
        exp.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        exp.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sci.pNext = &exp;
        VkSemaphore binary = VK_NULL_HANDLE;
        VkSemaphore wait = VK_NULL_HANDLE;
        if (vkCreateSemaphore(dev, &sci, nullptr, &binary) != VK_SUCCESS)
            return vk_error(lx::vulkan_err::out_of_memory, "vkCreateSemaphore (sync_fd)");
        s.vk_binary = binary;
        VkSemaphoreCreateInfo plain{};
        plain.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(dev, &plain, nullptr, &wait) != VK_SUCCESS)
            return vk_error(lx::vulkan_err::out_of_memory, "vkCreateSemaphore");
        s.vk_wait = wait;
    }

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS)
        return vk_error(lx::vulkan_err::out_of_memory, "vkCreateFence");
    s.vk_fence = fence;

    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = static_cast<VkCommandPool>(vk_pool_);
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 3;
    VkCommandBuffer cbs[3]{};
    if (vkAllocateCommandBuffers(dev, &cai, cbs) != VK_SUCCESS)
        return vk_error(lx::vulkan_err::out_of_memory, "vkAllocateCommandBuffers");
    s.cb_first = cbs[0];
    s.cb_acquire = cbs[1];
    s.cb_release = cbs[2];
    const std::uint32_t ours = ctx.queue_family;
    const std::uint32_t foreign = vk_foreign_ ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED;
    const std::uint32_t local = vk_foreign_ ? ours : VK_QUEUE_FAMILY_IGNORED;
    const auto record = [&](VkCommandBuffer cb, VkImageLayout from, VkImageLayout to,
                            std::uint32_t src_family, std::uint32_t dst_family,
                            VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                            VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS)
            return false;
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = src_stage;
        b.dstStageMask = dst_stage;
        b.srcAccessMask = src_access;
        b.dstAccessMask = dst_access;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = src_family;
        b.dstQueueFamilyIndex = dst_family;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &dep);
        return vkEndCommandBuffer(cb) == VK_SUCCESS;
    };
    constexpr VkAccessFlags2 color =
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    constexpr VkPipelineStageFlags2 attachment = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const bool recorded =
        record(cbs[0], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, VK_PIPELINE_STAGE_2_NONE,
               VK_ACCESS_2_NONE, attachment, color) &&
        record(cbs[1], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, foreign,
               local, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, attachment, color) &&
        record(cbs[2], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, local,
               foreign, attachment, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
    if (!recorded)
        return vk_error(lx::vulkan_err::out_of_memory, "recording the ownership barriers");

    s.image.vk.image = image;
    s.image.vk.view = view;
    s.image.vk.width = width_;
    s.image.vk.height = height_;
    s.image.vk.format = k_ui_vk_bgra8_unorm;
    return {};
#else
    (void)s;
    return lx::not_implemented("lx::gfx::client_swapchain::make_vk");
#endif
}

void client_swapchain::destroy_vk(slot& s) {
#if defined(LUMEN_HAS_VULKAN)
    const vk_context ctx = gpu_ ? gpu_->vulkan_context() : vk_context{};
    auto dev = static_cast<VkDevice>(ctx.device);
    if (dev) {
        if (s.vk_fence && s.vk_submitted) {
            auto f = static_cast<VkFence>(s.vk_fence);
            (void)vkWaitForFences(dev, 1, &f, VK_TRUE, 1'000'000'000ull);
        }
        // Acquired and never presented (cancelled, or a resize between acquire and present):
        // the acquire submit has no fence of its own. Structural path only.
        if (s.vk_owned && ctx.queue)
            (void)vkQueueWaitIdle(static_cast<VkQueue>(ctx.queue));
        VkCommandBuffer cbs[3] = {static_cast<VkCommandBuffer>(s.cb_first),
                                  static_cast<VkCommandBuffer>(s.cb_acquire),
                                  static_cast<VkCommandBuffer>(s.cb_release)};
        if (cbs[0] && vk_pool_)
            vkFreeCommandBuffers(dev, static_cast<VkCommandPool>(vk_pool_), 3, cbs);
        if (s.vk_fence)
            vkDestroyFence(dev, static_cast<VkFence>(s.vk_fence), nullptr);
        for (void* sem : {s.vk_timeline, s.vk_binary, s.vk_wait})
            if (sem)
                vkDestroySemaphore(dev, static_cast<VkSemaphore>(sem), nullptr);
        if (s.vk_view)
            vkDestroyImageView(dev, static_cast<VkImageView>(s.vk_view), nullptr);
        if (s.vk_image)
            vkDestroyImage(dev, static_cast<VkImage>(s.vk_image), nullptr);
        if (s.vk_memory)
            vkFreeMemory(dev, static_cast<VkDeviceMemory>(s.vk_memory), nullptr);
    }
#endif
    s.cb_first = s.cb_acquire = s.cb_release = nullptr;
    s.vk_fence = s.vk_timeline = s.vk_binary = s.vk_wait = nullptr;
    s.vk_view = s.vk_image = s.vk_memory = nullptr;
}

lx::result<void> client_swapchain::acquire_vk(slot& s) {
#if defined(LUMEN_HAS_VULKAN)
    if (s.vk_owned)
        return {}; // a cancelled frame left it ours
    const vk_context ctx = gpu_->vulkan_context();
    auto dev = static_cast<VkDevice>(ctx.device);
    const bool first = s.last_frame == 0 && !s.vk_submitted;
    VkSemaphore wait = VK_NULL_HANDLE;
    if (!first && !explicit_ && s.vk_wait) {
        // The compositor's reads of the dma-buf (its implicit fences), as a semaphore the
        // acquire waits on: the GPU waits, not this thread.
        dma_buf_export_sync_file req{};
        req.flags = DMA_BUF_SYNC_WRITE;
        req.fd = -1;
        if (::ioctl(s.plane_fds[0].get(), DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &req) == 0 &&
            req.fd >= 0) {
            VkImportSemaphoreFdInfoKHR ii{};
            ii.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
            ii.semaphore = static_cast<VkSemaphore>(s.vk_wait);
            ii.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
            ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            ii.fd = req.fd;
            auto import_fd =
                reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vk_fns_[fn_import_semaphore_fd]);
            if (import_fd(dev, &ii) == VK_SUCCESS)
                wait = static_cast<VkSemaphore>(s.vk_wait);
            else
                ::close(req.fd);
        }
    }
    VkCommandBuffer cb = static_cast<VkCommandBuffer>(first ? s.cb_first : s.cb_acquire);
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = wait ? 1u : 0u;
    si.pWaitSemaphores = &wait;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (vkQueueSubmit(static_cast<VkQueue>(ctx.queue), 1, &si, VK_NULL_HANDLE) != VK_SUCCESS)
        return vk_error(lx::vulkan_err::device_lost, "acquire barrier submit");
    s.vk_owned = true;
    return {};
#else
    (void)s;
    return lx::not_implemented("lx::gfx::client_swapchain::acquire_vk");
#endif
}

lx::result<void> client_swapchain::present_vk(slot& s) {
#if defined(LUMEN_HAS_VULKAN)
    const vk_context ctx = gpu_->vulkan_context();
    auto dev = static_cast<VkDevice>(ctx.device);
    auto fence = static_cast<VkFence>(s.vk_fence);
    if (s.vk_submitted && vkResetFences(dev, 1, &fence) != VK_SUCCESS)
        return vk_error(lx::vulkan_err::device_lost, "vkResetFences");
    VkCommandBuffer cb = static_cast<VkCommandBuffer>(s.cb_release);
    VkSemaphore signal = VK_NULL_HANDLE;
    std::uint64_t value = 0;
    VkTimelineSemaphoreSubmitInfo timeline{};
    timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (explicit_) {
        signal = static_cast<VkSemaphore>(s.vk_timeline);
        value = s.image.acquire_point;
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &value;
        si.pNext = &timeline;
    } else if (s.vk_binary) {
        signal = static_cast<VkSemaphore>(s.vk_binary);
    }
    si.signalSemaphoreCount = signal ? 1u : 0u;
    si.pSignalSemaphores = &signal;
    if (vkQueueSubmit(static_cast<VkQueue>(ctx.queue), 1, &si, fence) != VK_SUCCESS)
        return vk_error(lx::vulkan_err::device_lost, "release barrier submit");
    s.vk_owned = false;
    s.vk_submitted = true;
    if (explicit_)
        return {};

    bool attached = false;
    if (s.vk_binary) {
        // The frame's completion as the dma-buf's write fence: the compositor's import waits
        // for it like any implicitly synchronised buffer.
        VkSemaphoreGetFdInfoKHR gi{};
        gi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
        gi.semaphore = signal;
        gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        int sync_file = -1;
        auto get_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vk_fns_[fn_get_semaphore_fd]);
        if (get_fd(dev, &gi, &sync_file) == VK_SUCCESS) {
            if (sync_file < 0) {
                attached = true; // already signalled: nothing to wait for
            } else {
                dma_buf_import_sync_file req{};
                req.flags = DMA_BUF_SYNC_WRITE;
                req.fd = sync_file;
                attached = ::ioctl(s.plane_fds[0].get(), DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &req) == 0;
                ::close(sync_file);
            }
        }
    }
    if (!attached) {
        if (!warned_fence_) {
            warned_fence_ = true;
            log_swapchain_vk.warn("no sync_file for the dma-buf (DMA_BUF_IOCTL_IMPORT_SYNC_FILE "
                                  "or SYNC_FD export unavailable): the frame's fence is waited "
                                  "on the CPU before commit (reported once)");
        }
        if (vkWaitForFences(dev, 1, &fence, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS)
            return vk_error(lx::vulkan_err::device_lost, "frame fence wait");
    }
    return {};
#else
    (void)s;
    return lx::not_implemented("lx::gfx::client_swapchain::present_vk");
#endif
}

lx::result<std::uint64_t> client_swapchain::wait_vk() {
#if defined(LUMEN_HAS_VULKAN)
    if (!last_presented_)
        return std::uint64_t{0};
    const slot& s = slots_[last_presented_->slot];
    if (!s.vk_fence || !s.vk_submitted)
        return std::uint64_t{0};
    const vk_context ctx = gpu_->vulkan_context();
    auto fence = static_cast<VkFence>(s.vk_fence);
    const std::uint64_t t0 = vk_now_ns();
    if (vkWaitForFences(static_cast<VkDevice>(ctx.device), 1, &fence, VK_TRUE,
                        5'000'000'000ull) != VK_SUCCESS)
        return vk_error(lx::vulkan_err::device_lost, "frame fence wait");
    return vk_now_ns() - t0;
#else
    return lx::not_implemented("lx::gfx::client_swapchain::wait_vk");
#endif
}

} // namespace lx::gfx
