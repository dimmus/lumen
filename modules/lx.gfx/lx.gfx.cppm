module;

#if defined(LUMEN_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif
#include <cstring>
#include <unistd.h>
#include <utility>

import lx.foundation;
import lx.runtime;
import lx.drm;

export module lx.gfx;

export import :dmabuf;
export import :syncobj;
export import :semaphore_pool;
export import :import_cache;
export import :headless;
export import :renderer;
export import :vk_renderer;
export import :cpu_renderer;
export import :gl_renderer;
export import :glyph_atlas;

export namespace lx::gfx {

enum class gpu_tier { discrete, integrated, embedded };

struct device_info {
    gpu_tier tier = gpu_tier::integrated;
    const char* device_name = "";
    bool supports_dmabuf_import = false;
    bool supports_timeline_semaphore = false;
    /// VK_KHR_external_semaphore_fd — required to export a frame's completion as a
    /// sync_file, which is what lets present stop blocking the render thread.
    bool supports_external_semaphore_fd = false;
    /// VK_EXT_image_drm_format_modifier — required to import tiled client buffers.
    bool supports_drm_format_modifier = false;
    /// VK_EXT_external_memory_dma_buf — required to export a scanout target.
    bool supports_dmabuf_export = false;
    /// VK_EXT_queue_family_foreign — lets imported client buffers be acquired from the
    /// foreign queue family instead of an unsynchronised layout transition.
    bool supports_queue_family_foreign = false;
    /// `VK_PHYSICAL_DEVICE_TYPE_CPU` — the "GPU" is a software rasterizer (llvmpipe,
    /// SwiftShader). Vulkan still works, but every composite is CPU work reached through
    /// an upload and a tiling pass, so the present path should prefer `cpu_compositor`.
    bool is_software_rasterizer = false;
};

class device {
public:
    device() = default;
    ~device();

    device(const device&) = delete;
    device& operator=(const device&) = delete;
    device(device&& other) noexcept;
    device& operator=(device&& other) noexcept;

    [[nodiscard]] static lx::result<device> create(device_info info);

    [[nodiscard]] device_info info() const;
    [[nodiscard]] dmabuf_importer& dmabuf();
    [[nodiscard]] dmabuf_exporter& exporter();
    [[nodiscard]] syncobj_bridge& syncobj();

    /// Opaque Vulkan handles for renderer partitions. Null on the headless device.
    [[nodiscard]] void* vk_device() const;
    [[nodiscard]] void* vk_physical_device() const;
    [[nodiscard]] void* vk_queue() const;
    [[nodiscard]] void* vk_command_pool() const;
    [[nodiscard]] unsigned queue_family() const;

    /// Handles bundled for the renderer partition.
    [[nodiscard]] vk_context context() const;

private:
#if defined(LUMEN_HAS_VULKAN)
    void destroy_vulkan();
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice vk_device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
#endif
    /// Owned copy of `VkPhysicalDeviceProperties::deviceName`. `info_.device_name` points
    /// here, so both move operations must re-point it at the destination's own buffer.
    char device_name_[256]{};
    unsigned queue_family_ = 0;
    device_info info_{};
    dmabuf_importer dmabuf_{nullptr, nullptr};
    dmabuf_exporter exporter_{nullptr, nullptr};
    syncobj_bridge syncobj_{nullptr, -1};
};

class device_selector {
public:
    [[nodiscard]] static lx::result<device> select_best();
    [[nodiscard]] static gpu_tier probe_tier();
};

/// Recording handle threaded through widget paint. The real composite pass lives in
/// `vulkan_compositor`; widget-level painting is not wired yet, so this deliberately
/// exposes no operations rather than no-op ones that would look implemented.
class render_pass {};

class device_recovery {
public:
    explicit device_recovery(device& dev);

    [[nodiscard]] lx::result<void> handle_device_lost();
    [[nodiscard]] bool needs_recovery() const;

private:
    device* device_ = nullptr;
    bool needs_recovery_ = false;
};

} // namespace lx::gfx

module :private;

#if defined(LUMEN_HAS_VULKAN)
void lx::gfx::device::destroy_vulkan() {
    if (vk_device_ != VK_NULL_HANDLE) {
        if (command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(vk_device_, command_pool_, nullptr);
            command_pool_ = VK_NULL_HANDLE;
        }
        queue_ = VK_NULL_HANDLE;
        vkDestroyDevice(vk_device_, nullptr);
        vk_device_ = VK_NULL_HANDLE;
    }
    physical_ = VK_NULL_HANDLE;
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}
#endif

lx::gfx::device::~device() {
#if defined(LUMEN_HAS_VULKAN)
    destroy_vulkan();
#endif
}

lx::gfx::device::device(device&& other) noexcept
    : queue_family_{other.queue_family_}, info_{other.info_}, dmabuf_{other.dmabuf_},
      exporter_{other.exporter_}, syncobj_{other.syncobj_} {
#if defined(LUMEN_HAS_VULKAN)
    instance_ = other.instance_;
    physical_ = other.physical_;
    vk_device_ = other.vk_device_;
    queue_ = other.queue_;
    command_pool_ = other.command_pool_;
    other.instance_ = VK_NULL_HANDLE;
    other.physical_ = VK_NULL_HANDLE;
    other.vk_device_ = VK_NULL_HANDLE;
    other.queue_ = VK_NULL_HANDLE;
    other.command_pool_ = VK_NULL_HANDLE;
#endif
    std::memcpy(device_name_, other.device_name_, sizeof(device_name_));
    if (info_.device_name == other.device_name_)
        info_.device_name = device_name_;
    other.dmabuf_ = dmabuf_importer{nullptr, nullptr};
    other.exporter_ = dmabuf_exporter{nullptr, nullptr};
    other.syncobj_ = syncobj_bridge{nullptr, -1};
    other.queue_family_ = 0;
    other.info_ = {};
    other.device_name_[0] = '\0';
}

lx::gfx::device& lx::gfx::device::operator=(device&& other) noexcept {
    if (this == &other)
        return *this;
#if defined(LUMEN_HAS_VULKAN)
    destroy_vulkan();
    instance_ = other.instance_;
    physical_ = other.physical_;
    vk_device_ = other.vk_device_;
    queue_ = other.queue_;
    command_pool_ = other.command_pool_;
    other.instance_ = VK_NULL_HANDLE;
    other.physical_ = VK_NULL_HANDLE;
    other.vk_device_ = VK_NULL_HANDLE;
    other.queue_ = VK_NULL_HANDLE;
    other.command_pool_ = VK_NULL_HANDLE;
#endif
    queue_family_ = other.queue_family_;
    info_ = other.info_;
    dmabuf_ = other.dmabuf_;
    exporter_ = other.exporter_;
    syncobj_ = other.syncobj_;
    std::memcpy(device_name_, other.device_name_, sizeof(device_name_));
    if (info_.device_name == other.device_name_)
        info_.device_name = device_name_;
    other.dmabuf_ = dmabuf_importer{nullptr, nullptr};
    other.exporter_ = dmabuf_exporter{nullptr, nullptr};
    other.syncobj_ = syncobj_bridge{nullptr, -1};
    other.queue_family_ = 0;
    other.info_ = {};
    other.device_name_[0] = '\0';
    return *this;
}

#if defined(LUMEN_HAS_VULKAN)
namespace {

bool lx_gfx_has_extension(const VkExtensionProperties* props, unsigned count, const char* name) {
    for (unsigned i = 0; i < count; ++i) {
        if (std::strcmp(props[i].extensionName, name) == 0)
            return true;
    }
    return false;
}

/// Prefer a graphics+transfer family; queue family 0 is not guaranteed to be one.
int lx_gfx_pick_graphics_family(VkPhysicalDevice phys) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
    if (count == 0)
        return -1;
    if (count > 16)
        count = 16;
    VkQueueFamilyProperties families[16]{};
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, families);
    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueCount > 0 && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            return static_cast<int>(i);
    }
    return -1;
}

} // namespace
#endif

lx::result<lx::gfx::device> lx::gfx::device::create(device_info info) {
    device d;
    d.info_ = info;

#if defined(LUMEN_HAS_VULKAN)
    if (info.supports_dmabuf_import) {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "lumen";
        app.applicationVersion = VK_MAKE_VERSION(0, 3, 0);
        app.pEngineName = "lumen";
        app.engineVersion = VK_MAKE_VERSION(0, 3, 0);
        app.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;

        VkInstance instance = VK_NULL_HANDLE;
        if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
            return lx::make_error(lx::error_domain::vulkan,
                                  static_cast<int>(lx::vulkan_err::device_lost),
                                  "vkCreateInstance failed");
        }
        d.instance_ = instance;

        uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
            d.destroy_vulkan();
            return lx::make_error(lx::error_domain::vulkan,
                                  static_cast<int>(lx::vulkan_err::device_lost),
                                  "no Vulkan physical devices");
        }

        VkPhysicalDevice phys = VK_NULL_HANDLE;
        {
            VkPhysicalDevice devices[8]{};
            uint32_t n = count < 8 ? count : 8;
            vkEnumeratePhysicalDevices(instance, &n, devices);
            phys = devices[0];
        }
        d.physical_ = phys;

        const int family = lx_gfx_pick_graphics_family(phys);
        if (family < 0) {
            d.destroy_vulkan();
            return lx::make_error(lx::error_domain::vulkan,
                                  static_cast<int>(lx::vulkan_err::device_lost),
                                  "no graphics queue family");
        }
        d.queue_family_ = static_cast<unsigned>(family);

        // dmabuf import/export is only real when the external-memory extensions are
        // present; without them the compositor must fall back to the headless path
        // rather than silently importing nothing.
        uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, nullptr);
        VkExtensionProperties ext_props[512]{};
        if (ext_count > 512)
            ext_count = 512;
        if (ext_count > 0)
            vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, ext_props);

        const bool has_ext_mem = lx_gfx_has_extension(ext_props, ext_count,
                                                      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        const bool has_dmabuf = lx_gfx_has_extension(ext_props, ext_count,
                                                     VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        const bool has_modifier =
            lx_gfx_has_extension(ext_props, ext_count,
                                 VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) &&
            lx_gfx_has_extension(ext_props, ext_count,
                                 VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
        const bool has_foreign = lx_gfx_has_extension(
            ext_props, ext_count, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
        const bool has_timeline = lx_gfx_has_extension(
            ext_props, ext_count, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        const bool has_ext_sem_fd = lx_gfx_has_extension(
            ext_props, ext_count, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);

        if (!has_ext_mem || !has_dmabuf) {
            d.destroy_vulkan();
            return lx::make_error(lx::error_domain::vulkan,
                                  static_cast<int>(lx::vulkan_err::import_failed),
                                  "device lacks dma-buf external memory extensions");
        }

        const char* extensions[8]{};
        uint32_t enabled = 0;
        extensions[enabled++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        extensions[enabled++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
        if (has_modifier) {
            extensions[enabled++] = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME;
            extensions[enabled++] = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
        }
        if (has_foreign)
            extensions[enabled++] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
        if (has_timeline)
            extensions[enabled++] = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
        if (has_ext_sem_fd)
            extensions[enabled++] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;

        float priority = 1.f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = d.queue_family_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;

        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = enabled;
        dci.ppEnabledExtensionNames = extensions;

        VkDevice vkdev = VK_NULL_HANDLE;
        if (vkCreateDevice(phys, &dci, nullptr, &vkdev) != VK_SUCCESS) {
            d.destroy_vulkan();
            return lx::make_error(lx::error_domain::vulkan,
                                  static_cast<int>(lx::vulkan_err::device_lost),
                                  "vkCreateDevice failed");
        }
        d.vk_device_ = vkdev;
        vkGetDeviceQueue(vkdev, d.queue_family_, 0, &d.queue_);

        VkCommandPoolCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = d.queue_family_;
        if (vkCreateCommandPool(vkdev, &cpci, nullptr, &d.command_pool_) != VK_SUCCESS) {
            d.destroy_vulkan();
            return lx::make_error(lx::error_domain::vulkan,
                                  static_cast<int>(lx::vulkan_err::device_lost),
                                  "vkCreateCommandPool failed");
        }

        d.info_.supports_dmabuf_import = true;
        d.info_.supports_dmabuf_export = true;
        d.info_.supports_drm_format_modifier = has_modifier;
        d.info_.supports_queue_family_foreign = has_foreign;
        d.info_.supports_timeline_semaphore = has_timeline && has_ext_sem_fd;
        d.info_.supports_external_semaphore_fd = has_ext_sem_fd;

        {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(phys, &props);
            d.info_.is_software_rasterizer = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
            // Reporting "vulkan" for every device hides the difference that matters most
            // here — a real GPU versus llvmpipe. `props` is a stack copy, so the name has
            // to be owned rather than pointed at.
            static_assert(sizeof(d.device_name_) >= sizeof(props.deviceName));
            std::memcpy(d.device_name_, props.deviceName, sizeof(props.deviceName));
            d.device_name_[sizeof(d.device_name_) - 1] = '\0';
            if (d.device_name_[0] != '\0')
                d.info_.device_name = d.device_name_;
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                d.info_.tier = gpu_tier::discrete;
        }

        if (!d.info_.device_name || d.info_.device_name[0] == '\0')
            d.info_.device_name = "vulkan";
        d.dmabuf_ = dmabuf_importer{static_cast<void*>(vkdev), static_cast<void*>(phys),
                                    has_modifier};
        d.exporter_ = dmabuf_exporter{static_cast<void*>(vkdev), static_cast<void*>(phys)};
        d.syncobj_ = syncobj_bridge{static_cast<void*>(vkdev), -1};
        return d;
    }
#endif

    // Headless-capable device for CI / no-GPU builds (or Vulkan fallback).
    d.info_.supports_dmabuf_import = false;
    d.info_.supports_timeline_semaphore = false;
    d.info_.supports_external_semaphore_fd = false;
    d.info_.supports_dmabuf_export = false;
    d.info_.supports_drm_format_modifier = false;
    d.info_.supports_queue_family_foreign = false;
    if (!d.info_.device_name || d.info_.device_name[0] == '\0')
        d.info_.device_name = "headless";
    d.dmabuf_ = dmabuf_importer{nullptr, nullptr};
    d.exporter_ = dmabuf_exporter{nullptr, nullptr};
    d.syncobj_ = syncobj_bridge{nullptr, -1};
    return d;
}

lx::gfx::device_info lx::gfx::device::info() const { return info_; }
lx::gfx::dmabuf_importer& lx::gfx::device::dmabuf() { return dmabuf_; }
lx::gfx::dmabuf_exporter& lx::gfx::device::exporter() { return exporter_; }
lx::gfx::syncobj_bridge& lx::gfx::device::syncobj() { return syncobj_; }

unsigned lx::gfx::device::queue_family() const { return queue_family_; }

#if defined(LUMEN_HAS_VULKAN)
void* lx::gfx::device::vk_device() const { return static_cast<void*>(vk_device_); }
void* lx::gfx::device::vk_physical_device() const { return static_cast<void*>(physical_); }
void* lx::gfx::device::vk_queue() const { return static_cast<void*>(queue_); }
void* lx::gfx::device::vk_command_pool() const { return static_cast<void*>(command_pool_); }
#else
void* lx::gfx::device::vk_device() const { return nullptr; }
void* lx::gfx::device::vk_physical_device() const { return nullptr; }
void* lx::gfx::device::vk_queue() const { return nullptr; }
void* lx::gfx::device::vk_command_pool() const { return nullptr; }
#endif

lx::gfx::vk_context lx::gfx::device::context() const {
    vk_context ctx{};
    ctx.device = vk_device();
    ctx.physical_device = vk_physical_device();
    ctx.queue = vk_queue();
    ctx.command_pool = vk_command_pool();
    ctx.queue_family = queue_family_;
    ctx.supports_queue_family_foreign = info_.supports_queue_family_foreign;
    ctx.supports_external_semaphore_fd = info_.supports_external_semaphore_fd;
    return ctx;
}

lx::result<lx::gfx::device> lx::gfx::device_selector::select_best() {
    device_info info{};
    info.tier = probe_tier();
#if defined(LUMEN_HAS_VULKAN)
    info.device_name = "vulkan";
    info.supports_dmabuf_import = true;
    if (auto vulkan = device::create(info); vulkan)
        return vulkan;
#endif
    // Headless bookkeeping device — compositor start must succeed without a GPU.
    info.device_name = "headless";
    info.supports_dmabuf_import = false;
    return device::create(info);
}

lx::gfx::gpu_tier lx::gfx::device_selector::probe_tier() {
    // Dual-GPU heuristic: secondary render node implies discrete + integrated PRIME setup.
    if (access("/dev/dri/renderD128", F_OK) == 0 && access("/dev/dri/renderD129", F_OK) == 0)
        return lx::gfx::gpu_tier::discrete;
    if (access("/dev/dri/card1", F_OK) == 0)
        return lx::gfx::gpu_tier::discrete;
    return lx::gfx::gpu_tier::integrated;
}

lx::gfx::device_recovery::device_recovery(device& dev) : device_{&dev} {}

lx::result<void> lx::gfx::device_recovery::handle_device_lost() {
#if defined(LUMEN_HAS_VULKAN)
    needs_recovery_ = true;
    if (auto next = device_selector::select_best(); next) {
        *device_ = std::move(next).value();
        needs_recovery_ = false;
        return {};
    }
    return lx::make_error(lx::error_domain::vulkan,
                          static_cast<int>(lx::vulkan_err::device_lost),
                          "device recovery failed");
#else
    return lx::not_implemented("lx::gfx::device_recovery::handle_device_lost");
#endif
}

bool lx::gfx::device_recovery::needs_recovery() const { return needs_recovery_; }
