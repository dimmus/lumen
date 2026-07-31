module;

#if defined(LUMEN_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif
#if defined(LUMEN_HAS_DRM)
#include <xf86drm.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

import lx.foundation;

export module lx.gfx:syncobj;

import :semaphore_pool;

export namespace lx::gfx {

struct syncobj_timeline {
    lx::unique_fd drm_fd{};
    unsigned handle = 0;
    unsigned long long point = 0;
};

class syncobj_bridge {
public:
    syncobj_bridge() = default;
    syncobj_bridge(void* vk_device, int drm_fd);

    void set_drm_fd(int drm_fd);
    void set_device(void* vk_device);

    /// Imports the timeline's DRM syncobj into a pooled Vulkan timeline semaphore and
    /// caches the binding, so repeated calls for one syncobj reuse a single semaphore.
    /// Returns the VkSemaphore, which callers can place directly in submit infos.
    [[nodiscard]] lx::result<void*> import_timeline_semaphore(const syncobj_timeline& timeline);
    [[nodiscard]] lx::result<void> wait_at_point(const syncobj_timeline& timeline,
                                                unsigned long long timeout_ns);
    [[nodiscard]] lx::result<void> signal_at_point(const syncobj_timeline& timeline);
    /// Drops the cached binding for a syncobj handle and returns its semaphore to the pool.
    void forget(unsigned handle);

    [[nodiscard]] timeline_semaphore_pool& semaphore_pool();

private:
    [[nodiscard]] int find_binding(unsigned handle) const;
    [[nodiscard]] int resolve_drm_fd(const syncobj_timeline& timeline) const;

    static constexpr unsigned k_max_bindings = 32;
    struct binding {
        unsigned handle = 0;
        timeline_semaphore_handle semaphore{};
    };

    void* device_ = nullptr;
    int drm_fd_ = -1;
    timeline_semaphore_pool pool_{};
    binding bindings_[k_max_bindings]{};
    unsigned binding_count_ = 0;
};

} // namespace lx::gfx


lx::gfx::syncobj_bridge::syncobj_bridge(void* dev, int fd) : device_{dev}, drm_fd_{fd} {
    pool_.set_device(dev);
}

void lx::gfx::syncobj_bridge::set_drm_fd(int fd) { drm_fd_ = fd; }

void lx::gfx::syncobj_bridge::set_device(void* dev) {
    device_ = dev;
    pool_.set_device(dev);
}

lx::gfx::timeline_semaphore_pool& lx::gfx::syncobj_bridge::semaphore_pool() { return pool_; }

int lx::gfx::syncobj_bridge::find_binding(unsigned handle) const {
    for (unsigned i = 0; i < binding_count_; ++i)
        if (bindings_[i].handle == handle) return static_cast<int>(i);
    return -1;
}

int lx::gfx::syncobj_bridge::resolve_drm_fd(const syncobj_timeline& timeline) const {
    return timeline.drm_fd.get() >= 0 ? timeline.drm_fd.get() : drm_fd_;
}

void lx::gfx::syncobj_bridge::forget(unsigned handle) {
    const int slot = find_binding(handle);
    if (slot < 0)
        return;
    pool_.release(bindings_[static_cast<unsigned>(slot)].semaphore);
    bindings_[static_cast<unsigned>(slot)] = bindings_[binding_count_ - 1];
    bindings_[--binding_count_] = {};
}

lx::result<void*> lx::gfx::syncobj_bridge::import_timeline_semaphore(
    const syncobj_timeline& timeline) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_DRM)
    auto dev = static_cast<VkDevice>(device_);
    const int fd = resolve_drm_fd(timeline);
    if (!dev || fd < 0 || timeline.handle == 0)
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "syncobj import needs a device, DRM fd and handle");

    if (const int existing = find_binding(timeline.handle); existing >= 0)
        return bindings_[static_cast<unsigned>(existing)].semaphore.vk_semaphore;

    if (binding_count_ >= k_max_bindings)
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::sync_failed),
                              "syncobj binding table full");

    const auto import_fn = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        vkGetDeviceProcAddr(dev, "vkImportSemaphoreFdKHR"));
    if (!import_fn)
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "vkImportSemaphoreFdKHR unavailable");

    int syncobj_fd = -1;
    if (drmSyncobjHandleToFD(fd, timeline.handle, &syncobj_fd) != 0 || syncobj_fd < 0)
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::sync_failed),
                              "drmSyncobjHandleToFD failed");

    auto acquired = pool_.acquire();
    if (!acquired) {
        ::close(syncobj_fd);
        return acquired.get_error();
    }
    const auto handle = acquired.value();

    VkImportSemaphoreFdInfoKHR import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    import_info.semaphore = static_cast<VkSemaphore>(handle.vk_semaphore);
    // A DRM timeline syncobj is an opaque payload, not a sync_file.
    import_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    import_info.fd = syncobj_fd;

    if (import_fn(dev, &import_info) != VK_SUCCESS) {
        // Vulkan only takes ownership of the fd on success.
        ::close(syncobj_fd);
        pool_.release(handle);
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "vkImportSemaphoreFdKHR failed");
    }

    bindings_[binding_count_++] = {timeline.handle, handle};
    return handle.vk_semaphore;
#else
    (void)timeline;
    return lx::not_implemented("lx::gfx::syncobj_bridge::import_timeline_semaphore");
#endif
}

lx::result<void> lx::gfx::syncobj_bridge::wait_at_point(const syncobj_timeline& timeline,
                                                        unsigned long long timeout_ns) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_DRM)
    auto dev = static_cast<VkDevice>(device_);
    if (!dev)
        return lx::make_error(lx::error_domain::invalid_argument, 0, "syncobj wait needs a device");

    auto imported = import_timeline_semaphore(timeline);
    if (!imported)
        return imported.get_error();
    auto sem = static_cast<VkSemaphore>(imported.value());

    const auto wait_fn = reinterpret_cast<PFN_vkWaitSemaphores>(
        vkGetDeviceProcAddr(dev, "vkWaitSemaphores"));
    if (!wait_fn)
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "vkWaitSemaphores unavailable");

    const uint64_t value = timeline.point;
    VkSemaphoreWaitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait.semaphoreCount = 1;
    wait.pSemaphores = &sem;
    wait.pValues = &value;

    if (wait_fn(dev, &wait, timeout_ns) != VK_SUCCESS)
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::sync_failed),
                              "timeline wait did not reach the requested point");
    return {};
#else
    (void)timeline;
    (void)timeout_ns;
    return lx::not_implemented("lx::gfx::syncobj_bridge::wait_at_point");
#endif
}

lx::result<void> lx::gfx::syncobj_bridge::signal_at_point(const syncobj_timeline& timeline) {
#if defined(LUMEN_HAS_VULKAN) && defined(LUMEN_HAS_DRM)
    auto dev = static_cast<VkDevice>(device_);
    const int fd = resolve_drm_fd(timeline);
    if (!dev || fd < 0 || timeline.handle == 0)
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "syncobj signal needs a device, DRM fd and handle");

    auto acquired = pool_.acquire();
    if (!acquired)
        return acquired.get_error();
    const auto handle = acquired.value();
    auto sem = static_cast<VkSemaphore>(handle.vk_semaphore);

    const auto signal_fn = reinterpret_cast<PFN_vkSignalSemaphore>(
        vkGetDeviceProcAddr(dev, "vkSignalSemaphore"));
    const auto export_fn = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        vkGetDeviceProcAddr(dev, "vkGetSemaphoreFdKHR"));
    if (!signal_fn || !export_fn) {
        pool_.release(handle);
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "timeline semaphore signal/export unavailable");
    }

    VkSemaphoreSignalInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    signal.semaphore = sem;
    signal.value = timeline.point;
    if (signal_fn(dev, &signal) != VK_SUCCESS) {
        pool_.release(handle);
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "vkSignalSemaphore failed");
    }

    VkSemaphoreGetFdInfoKHR export_info{};
    export_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    export_info.semaphore = sem;
    export_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    int opaque_fd = -1;
    if (export_fn(dev, &export_info, &opaque_fd) != VK_SUCCESS || opaque_fd < 0) {
        pool_.release(handle);
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::import_failed),
                              "vkGetSemaphoreFdKHR failed");
    }

    // Timeline-to-timeline handoff: adopt the Vulkan payload as a temporary syncobj, then
    // transfer the point into the caller's syncobj. A sync_file import would not work here
    // because sync_files carry only binary state.
    uint32_t staging = 0;
    if (drmSyncobjFDToHandle(fd, opaque_fd, &staging) != 0) {
        ::close(opaque_fd);
        pool_.release(handle);
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::sync_failed),
                              "drmSyncobjFDToHandle failed");
    }
    ::close(opaque_fd);

    const int transferred =
        drmSyncobjTransfer(fd, timeline.handle, timeline.point, staging, timeline.point, 0);
    (void)drmSyncobjDestroy(fd, staging);
    pool_.release(handle);

    if (transferred != 0)
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::sync_failed),
                              "drmSyncobjTransfer failed");
    return {};
#else
    (void)timeline;
    return lx::not_implemented("lx::gfx::syncobj_bridge::signal_at_point");
#endif
}
