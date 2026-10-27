module;

#if defined(LUMEN_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif
#include <stdlib.h>
#include <stdint.h>

import lx.foundation;
import lx.runtime;

export module lx.gfx;

import std;

export import :damage;
export import :primitives;
export import :dmabuf;
export import :syncobj;
export import :drm_timeline;
export import :semaphore_pool;
export import :import_cache;
export import :headless;
export import :renderer;
export import :vk_renderer;
export import :cpu_renderer;
export import :gl_renderer;
export import :gl_capture;
export import :atlas;
export import :ui_cpu;
export import :ui_gl;
export import :ui_vk;
export import :swapchain;

export namespace lx::gfx {

enum class gpu_tier { discrete, integrated, embedded };

/// A DRM device node as the numbers of its dev_t: the card the compositor scans out on
/// (`major`/`minor` of its `st_rdev`). It is how the Vulkan device behind the card is found
/// without a DRM or Vulkan type crossing into lx.gfx. `present` false: no card (headless,
/// nested, virtual outputs).
struct drm_node {
    bool present = false;
    unsigned major = 0;
    unsigned minor = 0;
};

/// `VkPhysicalDeviceType`, numbered the same, as a plain value.
enum class physical_device_kind : unsigned char {
    other = 0,
    integrated_gpu = 1,
    discrete_gpu = 2,
    virtual_gpu = 3,
    cpu = 4,
};

/// Discrete for a discrete GPU; integrated otherwise (an integrated GPU, a guest's virtual
/// GPU, a software rasterizer). What the present mode follows.
[[nodiscard]] constexpr gpu_tier tier_of(physical_device_kind kind) {
    return kind == physical_device_kind::discrete_gpu ? gpu_tier::discrete
                                                      : gpu_tier::integrated;
}

/// How `choose_physical_device` found its device.
enum class device_match : unsigned char {
    /// The device's primary node (VK_EXT_physical_device_drm) is the card.
    primary_node,
    /// The device's render node is the node given (a render node named as the card).
    render_node,
    /// No card to match: the first hardware device.
    no_card,
    /// No device reports its DRM nodes (VK_EXT_physical_device_drm missing everywhere):
    /// the first hardware device, as before matching existed.
    no_extension,
    /// Devices report their nodes and none is the card — a display-only card (simpledrm,
    /// evdi, vkms) or a GPU without a Vulkan driver: the first hardware device, which then
    /// composites for another GPU's scanout.
    no_match,
};

[[nodiscard]] constexpr const char* device_match_name(device_match m) {
    switch (m) {
    case device_match::primary_node: return "primary node";
    case device_match::render_node: return "render node";
    case device_match::no_card: return "no card";
    case device_match::no_extension: return "VK_EXT_physical_device_drm missing";
    case device_match::no_match: return "no device owns the card";
    }
    return "?";
}

/// One Vulkan physical device as the selector sees it, in plain values.
struct physical_device_desc {
    physical_device_kind kind = physical_device_kind::other;
    /// VK_EXT_physical_device_drm is supported: the node fields below are meaningful.
    bool has_drm = false;
    /// `VkPhysicalDeviceDrmPropertiesEXT` — `present` mirrors `hasPrimary` / `hasRender`.
    drm_node primary{};
    drm_node render{};
};

inline constexpr unsigned k_max_physical_devices = 8;

/// Physical devices in enumeration order (at most `k_max_physical_devices`).
struct physical_device_list {
    physical_device_desc devices[k_max_physical_devices]{};
    unsigned count = 0;
};

struct device_choice {
    /// Into the list; 0 for an empty list (check `count`).
    unsigned index = 0;
    device_match match = device_match::no_card;
};

/// Pure: the device behind `card` — the one whose primary node is the card, else whose
/// render node is. Without a match, the first device that is not a software rasterizer
/// (the first device when every one is). A hybrid laptop's panel is on the integrated GPU
/// while the loader often lists the discrete one first; compositing there would put every
/// frame across PCIe and make client dma-bufs foreign to the scanout device.
[[nodiscard]] device_choice choose_physical_device(const physical_device_list& list,
                                                   drm_node card);

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
    /// Input to `device::create`: the card to match the physical device to (`select_best`).
    /// Kept, so device recovery re-selects for the same card.
    drm_node card{};
    /// How the physical device was matched to `card`, and that device's own nodes
    /// (`present` false without VK_EXT_physical_device_drm).
    device_match match = device_match::no_card;
    drm_node primary_node{};
    drm_node render_node{};
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
    /// A Vulkan device on the physical device behind `card` (`choose_physical_device`),
    /// else the headless bookkeeping device.
    [[nodiscard]] static lx::result<device> select_best(drm_node card = {});
    /// The tier of the device `choose_physical_device` picks from `devices` for `card`;
    /// integrated when there is none. From the device's type, not from which /dev/dri
    /// nodes exist: card1 is commonly the only GPU (simpledrm took card0).
    [[nodiscard]] static gpu_tier probe_tier(const physical_device_list& devices,
                                             drm_node card);
    /// The physical devices Vulkan exposes, described for `choose_physical_device`.
    /// Creates and destroys an instance only — no logical device, so a software ICD never
    /// builds its JIT or worker threads just to be asked. `select_best` creates a lavapipe
    /// device for about 28 MB of heap; a caller that would not composite with it should
    /// not pay that to find out. Empty without Vulkan.
    [[nodiscard]] static physical_device_list probe_devices();
};

/// While alive, the Vulkan loader skips software drivers (`VK_LOADER_DRIVERS_DISABLE`), for
/// `device_selector::probe_devices` asked whether hardware Vulkan exists: a software
/// rasterizer cannot be its answer, and
/// loading one is most of the probe's cost (lavapipe initialises LLVM: about 15 of 20 ms on
/// a VM). Left alone when the environment already chooses drivers. It edits the process
/// environment: create and destroy it while no other thread can write the environment, and
/// create no Vulkan instance under it that outlives it.
class hardware_probe_scope {
public:
    hardware_probe_scope();
    ~hardware_probe_scope();
    hardware_probe_scope(const hardware_probe_scope&) = delete;
    hardware_probe_scope& operator=(const hardware_probe_scope&) = delete;

private:
    bool set_ = false;
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

/// The device's type and, with VK_EXT_physical_device_drm, its DRM nodes. The extension's
/// properties are read through `vkGetPhysicalDeviceProperties2`, core in Vulkan 1.1: the
/// instances here ask for 1.1, and a 1.0 device is described without nodes.
lx::gfx::physical_device_desc lx_gfx_describe(VkPhysicalDevice phys) {
    static_assert(static_cast<int>(lx::gfx::physical_device_kind::other) ==
                  VK_PHYSICAL_DEVICE_TYPE_OTHER);
    static_assert(static_cast<int>(lx::gfx::physical_device_kind::integrated_gpu) ==
                  VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
    static_assert(static_cast<int>(lx::gfx::physical_device_kind::discrete_gpu) ==
                  VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
    static_assert(static_cast<int>(lx::gfx::physical_device_kind::virtual_gpu) ==
                  VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU);
    static_assert(static_cast<int>(lx::gfx::physical_device_kind::cpu) ==
                  VK_PHYSICAL_DEVICE_TYPE_CPU);
    lx::gfx::physical_device_desc d{};
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    if (props.deviceType <= VK_PHYSICAL_DEVICE_TYPE_CPU)
        d.kind = static_cast<lx::gfx::physical_device_kind>(props.deviceType);
    if (props.apiVersion < VK_API_VERSION_1_1)
        return d;

    uint32_t ext_count = 0;
    if (vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, nullptr) != VK_SUCCESS)
        return d;
    std::vector<VkExtensionProperties> exts(ext_count);
    if (ext_count > 0 &&
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, exts.data()) < 0)
        return d;
    exts.resize(ext_count);
    if (!std::ranges::any_of(exts, [](const VkExtensionProperties& p) {
            return std::strcmp(p.extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0;
        }))
        return d;

    VkPhysicalDeviceDrmPropertiesEXT drm{};
    drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &drm;
    vkGetPhysicalDeviceProperties2(phys, &props2);
    d.has_drm = true;
    d.primary = {drm.hasPrimary == VK_TRUE, static_cast<unsigned>(drm.primaryMajor),
                 static_cast<unsigned>(drm.primaryMinor)};
    d.render = {drm.hasRender == VK_TRUE, static_cast<unsigned>(drm.renderMajor),
                static_cast<unsigned>(drm.renderMinor)};
    return d;
}

/// The instance's physical devices in enumeration order, at most `k_max_physical_devices`,
/// with `list` describing each.
void lx_gfx_enumerate(VkInstance instance,
                      VkPhysicalDevice (&devices)[lx::gfx::k_max_physical_devices],
                      lx::gfx::physical_device_list& list) {
    uint32_t n = lx::gfx::k_max_physical_devices;
    const VkResult r = vkEnumeratePhysicalDevices(instance, &n, devices);
    list.count = (r == VK_SUCCESS || r == VK_INCOMPLETE) ? n : 0;
    for (unsigned i = 0; i < list.count; ++i)
        list.devices[i] = lx_gfx_describe(devices[i]);
}

} // namespace
#endif

lx::gfx::device_choice lx::gfx::choose_physical_device(const physical_device_list& list,
                                                       drm_node card) {
    const std::span devices{list.devices, std::min(list.count, k_max_physical_devices)};
    const auto is = [&card](const drm_node& n) {
        return n.present && n.major == card.major && n.minor == card.minor;
    };
    device_choice choice{};
    if (card.present) {
        for (unsigned i = 0; i < devices.size(); ++i)
            if (devices[i].has_drm && is(devices[i].primary))
                return {i, device_match::primary_node};
        for (unsigned i = 0; i < devices.size(); ++i)
            if (devices[i].has_drm && is(devices[i].render))
                return {i, device_match::render_node};
        const bool any_drm =
            std::ranges::any_of(devices, [](const physical_device_desc& d) { return d.has_drm; });
        choice.match = any_drm ? device_match::no_match : device_match::no_extension;
    }
    for (unsigned i = 0; i < devices.size(); ++i) {
        if (devices[i].kind != physical_device_kind::cpu) {
            choice.index = i;
            break;
        }
    }
    return choice;
}

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
            return lx::make_error(lx::vulkan_err::device_lost, "vkCreateInstance failed");
        }
        d.instance_ = instance;

        // The physical device behind the scanout card: composite where the frame is
        // shown, and import client dma-bufs on the device that will scan them out.
        VkPhysicalDevice devices[k_max_physical_devices]{};
        physical_device_list described{};
        lx_gfx_enumerate(instance, devices, described);
        if (described.count == 0) {
            d.destroy_vulkan();
            return lx::make_error(lx::vulkan_err::device_lost, "no Vulkan physical devices");
        }
        const device_choice choice = choose_physical_device(described, info.card);
        const physical_device_desc& chosen = described.devices[choice.index];
        VkPhysicalDevice phys = devices[choice.index];
        d.physical_ = phys;
        d.info_.match = choice.match;
        d.info_.primary_node = chosen.primary;
        d.info_.render_node = chosen.render;
        d.info_.tier = tier_of(chosen.kind);

        const int family = lx_gfx_pick_graphics_family(phys);
        if (family < 0) {
            d.destroy_vulkan();
            return lx::make_error(lx::vulkan_err::device_lost, "no graphics queue family");
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

        const std::span exts{ext_props, ext_count};
        const auto has = [&](const char* name) {
            return std::ranges::any_of(exts, [&](const VkExtensionProperties& p) {
                return std::strcmp(p.extensionName, name) == 0;
            });
        };
        const bool has_ext_mem = has(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        const bool has_dmabuf = has(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        const bool has_modifier = has(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) &&
                                  has(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
        const bool has_foreign = has(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
        const bool has_timeline = has(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        const bool has_ext_sem_fd = has(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);

        if (!has_ext_mem || !has_dmabuf) {
            d.destroy_vulkan();
            return lx::make_error(lx::vulkan_err::import_failed,
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

        // VK_KHR_timeline_semaphore's semaphores are only valid with its feature enabled
        // (the timeline pool, a client swapchain's acquire and release points).
        VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timeline_features{};
        timeline_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR;
        bool timeline_enabled = false;
        if (has_timeline) {
            VkPhysicalDeviceFeatures2 features{};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = &timeline_features;
            vkGetPhysicalDeviceFeatures2(phys, &features);
            timeline_enabled = timeline_features.timelineSemaphore == VK_TRUE;
            timeline_features.pNext = nullptr;
        }

        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        if (timeline_enabled)
            dci.pNext = &timeline_features;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = enabled;
        dci.ppEnabledExtensionNames = extensions;

        VkDevice vkdev = VK_NULL_HANDLE;
        if (vkCreateDevice(phys, &dci, nullptr, &vkdev) != VK_SUCCESS) {
            d.destroy_vulkan();
            return lx::make_error(lx::vulkan_err::device_lost, "vkCreateDevice failed");
        }
        d.vk_device_ = vkdev;
        vkGetDeviceQueue(vkdev, d.queue_family_, 0, &d.queue_);

        VkCommandPoolCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = d.queue_family_;
        if (vkCreateCommandPool(vkdev, &cpci, nullptr, &d.command_pool_) != VK_SUCCESS) {
            d.destroy_vulkan();
            return lx::make_error(lx::vulkan_err::device_lost, "vkCreateCommandPool failed");
        }

        d.info_.supports_dmabuf_import = true;
        d.info_.supports_dmabuf_export = true;
        d.info_.supports_drm_format_modifier = has_modifier;
        d.info_.supports_queue_family_foreign = has_foreign;
        d.info_.supports_timeline_semaphore = timeline_enabled && has_ext_sem_fd;
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

lx::result<lx::gfx::device> lx::gfx::device_selector::select_best(drm_node card) {
    device_info info{};
    info.card = card;
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

lx::gfx::hardware_probe_scope::hardware_probe_scope() {
    if (std::getenv("VK_LOADER_DRIVERS_DISABLE") || std::getenv("VK_LOADER_DRIVERS_SELECT") ||
        std::getenv("VK_DRIVER_FILES") || std::getenv("VK_ICD_FILENAMES") ||
        std::getenv("VK_ADD_DRIVER_FILES"))
        return;
    // Mesa's lavapipe and Google's SwiftShader, matched on their manifest names.
    set_ = ::setenv("VK_LOADER_DRIVERS_DISABLE", "*lvp_icd*,*swiftshader*", 0) == 0;
}

lx::gfx::hardware_probe_scope::~hardware_probe_scope() {
    if (set_)
        ::unsetenv("VK_LOADER_DRIVERS_DISABLE");
}

lx::gfx::physical_device_list lx::gfx::device_selector::probe_devices() {
    physical_device_list list{};
#if defined(LUMEN_HAS_VULKAN)
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "lumen";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS)
        return list;
    VkPhysicalDevice devices[k_max_physical_devices]{};
    lx_gfx_enumerate(instance, devices, list);
    vkDestroyInstance(instance, nullptr);
#endif
    return list;
}

lx::gfx::gpu_tier lx::gfx::device_selector::probe_tier(const physical_device_list& devices,
                                                       drm_node card) {
    if (devices.count == 0)
        return gpu_tier::integrated;
    return tier_of(devices.devices[choose_physical_device(devices, card).index].kind);
}

lx::gfx::device_recovery::device_recovery(device& dev) : device_{&dev} {}

lx::result<void> lx::gfx::device_recovery::handle_device_lost() {
#if defined(LUMEN_HAS_VULKAN)
    needs_recovery_ = true;
    if (auto next = device_selector::select_best(device_->info().card); next) {
        *device_ = std::move(next).value();
        needs_recovery_ = false;
        return {};
    }
    return lx::make_error(lx::vulkan_err::device_lost, "device recovery failed");
#else
    return lx::not_implemented("lx::gfx::device_recovery::handle_device_lost");
#endif
}

bool lx::gfx::device_recovery::needs_recovery() const { return needs_recovery_; }
