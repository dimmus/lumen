module;

#if defined(LUMEN_HAS_VULKAN)
#include <vulkan/vulkan.h>
#endif

import lx.foundation;

export module lx.gfx:semaphore_pool;

export namespace lx::gfx {

struct timeline_semaphore_handle {
    void* vk_semaphore = nullptr;
    unsigned pool_index = 0;
};

/// Reuses Vulkan timeline semaphores instead of creating one per frame.
class timeline_semaphore_pool {
public:
    explicit timeline_semaphore_pool(void* vk_device = nullptr);

    void set_device(void* vk_device);
    [[nodiscard]] lx::result<timeline_semaphore_handle> acquire();
    void release(const timeline_semaphore_handle& handle);
    void reset();

private:
    static constexpr unsigned k_capacity = 32;

    void* device_ = nullptr;
    void* semaphores_[k_capacity]{};
    bool in_use_[k_capacity]{};
    unsigned count_ = 0;
};

} // namespace lx::gfx


lx::gfx::timeline_semaphore_pool::timeline_semaphore_pool(void* vk_device) : device_{vk_device} {}

void lx::gfx::timeline_semaphore_pool::set_device(void* vk_device) { device_ = vk_device; }

lx::result<lx::gfx::timeline_semaphore_handle>
lx::gfx::timeline_semaphore_pool::acquire() {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(device_);
    if (!dev)
        return lx::not_implemented("lx::gfx::timeline_semaphore_pool::acquire");

    for (unsigned i = 0; i < count_; ++i) {
        if (in_use_[i])
            continue;
        in_use_[i] = true;
        return timeline_semaphore_handle{semaphores_[i], i};
    }
    if (count_ >= k_capacity)
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::device_lost),
                              "timeline semaphore pool exhausted");

    VkSemaphoreTypeCreateInfo type_info{};
    type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type_info.initialValue = 0;

    // Timeline semaphores only interop over OPAQUE_FD (SYNC_FD is binary-only), and
    // vkGetSemaphoreFdKHR is legal only if the handle type is declared at creation.
    VkExportSemaphoreCreateInfo export_info{};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    export_info.pNext = &type_info;
    export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &export_info;

    VkSemaphore sem = VK_NULL_HANDLE;
    if (vkCreateSemaphore(dev, &sci, nullptr, &sem) != VK_SUCCESS)
        return lx::make_error(lx::error_domain::vulkan,
                              static_cast<int>(lx::vulkan_err::device_lost),
                              "vkCreateSemaphore failed");

    const unsigned index = count_++;
    semaphores_[index] = sem;
    in_use_[index] = true;
    return timeline_semaphore_handle{sem, index};
#else
    return lx::not_implemented("lx::gfx::timeline_semaphore_pool::acquire");
#endif
}

void lx::gfx::timeline_semaphore_pool::release(const timeline_semaphore_handle& handle) {
    if (handle.pool_index < count_)
        in_use_[handle.pool_index] = false;
}

void lx::gfx::timeline_semaphore_pool::reset() {
#if defined(LUMEN_HAS_VULKAN)
    auto dev = static_cast<VkDevice>(device_);
    if (dev) {
        for (unsigned i = 0; i < count_; ++i) {
            if (semaphores_[i])
                vkDestroySemaphore(dev, static_cast<VkSemaphore>(semaphores_[i]), nullptr);
        }
    }
#endif
    for (unsigned i = 0; i < k_capacity; ++i) {
        semaphores_[i] = nullptr;
        in_use_[i] = false;
    }
    count_ = 0;
}
