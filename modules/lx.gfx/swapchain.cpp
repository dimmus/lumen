module;

// Bodies of `lx.gfx:swapchain` shared by the tiers: the format choice, the render node, the
// tier choice, the image ring (buffer age, release tracking, resize) and the cpu tier. The GL
// and Vulkan halves are swapchain_gl.cpp and swapchain_vk.cpp.

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#if defined(LUMEN_HAS_DRM)
#include <xf86drm.h>
#endif

module lx.gfx;

import std;

import lx.foundation;
import lx.runtime;
import lx.trace;
import :damage;
import :cpu_renderer;
import :drm_timeline;
import :swapchain;

namespace lx::gfx {

namespace {

constexpr lx::trace::category log_swapchain{"gfx.swapchain"};

void copy_text(char* dst, std::size_t cap, const char* src) {
    if (!dst || cap == 0)
        return;
    std::size_t n = 0;
    if (src)
        for (; src[n] && n + 1 < cap; ++n)
            dst[n] = src[n];
    dst[n] = '\0';
}

} // namespace

// ── formats ─────────────────────────────────────────────────────────────────────

client_tier parse_client_tier(const char* name) {
    if (!name)
        return client_tier::none;
    if (std::strcmp(name, "vulkan") == 0 || std::strcmp(name, "vk") == 0)
        return client_tier::vulkan;
    if (std::strcmp(name, "gl") == 0 || std::strcmp(name, "gles") == 0)
        return client_tier::gl;
    if (std::strcmp(name, "cpu") == 0 || std::strcmp(name, "shm") == 0)
        return client_tier::cpu;
    return client_tier::none;
}

bool format_choice::has(std::uint64_t modifier) const {
    for (unsigned i = 0; i < modifier_count; ++i)
        if (modifiers[i] == modifier)
            return true;
    return false;
}

lx::result<format_choice> choose_client_format(const client_feedback& feedback,
                                               const format_request& request,
                                               std::uint64_t device) {
    const std::uint64_t want = device ? device : feedback.main_device;
    std::uint32_t fourccs[4]{};
    unsigned n = 0;
    if (request.ten_bit) {
        if (request.opaque)
            fourccs[n++] = k_fourcc_xrgb2101010;
        fourccs[n++] = k_fourcc_argb2101010;
    }
    if (request.opaque)
        fourccs[n++] = k_fourcc_xrgb8888;
    fourccs[n++] = k_fourcc_argb8888;

    // Passes: scanout-flagged tranches first (when asked), then all of them, in the
    // compositor's order. A tranche's target must be the device the client renders on:
    // buffers of a tranche are to be allocated there (linux-dmabuf v5).
    for (int pass = request.prefer_scanout ? 0 : 1; pass < 2; ++pass) {
        for (std::uint32_t fourcc : std::span{fourccs, n}) {
            for (const client_tranche& t : feedback.tranches) {
                if (pass == 0 && !t.scanout)
                    continue;
                if (want && t.target_device && t.target_device != want &&
                    !same_drm_device(t.target_device, want))
                    continue;
                format_choice c{};
                for (const client_format& f : t.formats) {
                    if (f.fourcc != fourcc || c.has(f.modifier))
                        continue;
                    if (c.modifier_count < k_client_max_modifiers)
                        c.modifiers[c.modifier_count++] = f.modifier;
                    else
                        ++c.modifiers_dropped;
                }
                if (c.modifier_count == 0)
                    continue;
                c.fourcc = fourcc;
                c.device = t.target_device ? t.target_device : want;
                c.scanout = t.scanout;
                return c;
            }
        }
    }
    return lx::make_error(lx::drm_err::no_format,
                          "no feedback tranche offers ARGB8888/XRGB8888 for the device");
}

// ── render node ─────────────────────────────────────────────────────────────────

bool same_drm_device(std::uint64_t a, std::uint64_t b) {
    if (a == b)
        return true;
#if defined(LUMEN_HAS_DRM)
    if (!a || !b)
        return false;
    drmDevicePtr da = nullptr;
    drmDevicePtr db = nullptr;
    bool same = false;
    if (drmGetDeviceFromDevId(static_cast<dev_t>(a), 0, &da) == 0 &&
        drmGetDeviceFromDevId(static_cast<dev_t>(b), 0, &db) == 0)
        same = drmDevicesEqual(da, db) != 0;
    if (da)
        drmFreeDevice(&da);
    if (db)
        drmFreeDevice(&db);
    return same;
#else
    return false;
#endif
}

lx::result<render_node> open_render_node(std::uint64_t main_device) {
    render_node out{};
    const auto open_path = [&](const char* path, bool matched) {
        const int fd = ::open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            return false;
        struct stat st {};
        if (::fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) {
            ::close(fd);
            return false;
        }
        out.fd.reset(fd);
        out.dev = st.st_rdev;
        out.matched = matched;
        copy_text(out.path, sizeof(out.path), path);
        return true;
    };
#if defined(LUMEN_HAS_DRM)
    if (main_device) {
        drmDevicePtr dev = nullptr;
        if (drmGetDeviceFromDevId(static_cast<dev_t>(main_device), 0, &dev) == 0 && dev) {
            const bool has_render = (dev->available_nodes & (1 << DRM_NODE_RENDER)) != 0;
            const bool ok = has_render && open_path(dev->nodes[DRM_NODE_RENDER], true);
            drmFreeDevice(&dev);
            if (ok)
                return out;
        }
    }
#endif
    // No device named (a headless compositor), or one this process cannot see: the first
    // render node, reported as unmatched.
    for (int minor = 128; minor < 192; ++minor) {
        char path[32];
        std::snprintf(path, sizeof(path), "/dev/dri/renderD%d", minor);
        if (open_path(path, false))
            return out;
    }
    return lx::make_error(lx::drm_err::open_failed, "no DRM render node");
}

// ── client_gpu ──────────────────────────────────────────────────────────────────

client_gpu::~client_gpu() { destroy(); }

void client_gpu::refuse(client_tier t, const char* why) {
    copy_text(refused_[static_cast<unsigned>(t)], sizeof(refused_[0]), why);
}

const char* client_gpu::refusal(client_tier t) const {
    return refused_[static_cast<unsigned>(t) & 3u];
}

lx::result<void> client_gpu::create(const client_feedback& feedback,
                                    const client_gpu_options& options) {
    destroy();
    client_tier force = options.force;
    const char* source = "settings";
    if (force == client_tier::none && options.read_environment) {
        if (const char* env = std::getenv("LUMEN_UI_BACKEND"); env && *env) {
            force = parse_client_tier(env);
            source = "LUMEN_UI_BACKEND";
            if (force == client_tier::none)
                log_swapchain.warn("LUMEN_UI_BACKEND={}: not vulkan, gl or cpu — ignored", env);
        }
    }
    forced_ = force != client_tier::none;
    if (forced_)
        log_swapchain.info("client tier forced: {} ({}{})", client_tier_name(force), source,
                           options.allow_software ? ", software allowed" : "");

    if (force != client_tier::cpu) {
        const std::uint64_t dev = options.device_override ? options.device_override
                                                          : feedback.main_device;
        if (!dev && !forced_) {
            // A compositor that names no device composites without one (headless): a GPU
            // buffer would only be copied back. Its clients draw on the CPU.
            refuse(client_tier::vulkan, "the compositor names no main device");
            refuse(client_tier::gl, "the compositor names no main device");
        } else if (auto node = open_render_node(dev); node) {
            node_ = std::move(node).value();
            timeline_ = drm_timeline_supported(node_.fd.get());
        } else {
            refuse(client_tier::vulkan, "no render node");
            refuse(client_tier::gl, "no render node");
            if (forced_)
                return node.get_error();
        }
    }

    if (node_.fd.get() >= 0) {
        if (force == client_tier::none || force == client_tier::vulkan) {
            auto vk = try_vulkan(force == client_tier::vulkan, options.allow_software);
            if (vk) {
                tier_ = client_tier::vulkan;
            } else if (force == client_tier::vulkan) {
                log_swapchain.error(vk.get_error());
                return vk.get_error();
            }
        }
        if (tier_ == client_tier::none &&
            (force == client_tier::none || force == client_tier::gl)) {
            auto gl = try_gl(force == client_tier::gl, options.allow_software);
            if (gl) {
                tier_ = client_tier::gl;
            } else if (force == client_tier::gl) {
                log_swapchain.error(gl.get_error());
                return gl.get_error();
            }
        }
    }
    if (tier_ == client_tier::none) {
        tier_ = client_tier::cpu;
        copy_text(name_, sizeof(name_), "cpu");
        node_ = {};
        timeline_ = false;
    }
    log_swapchain.info(
        "client tier {}: {}{} on {} ({}) — vulkan: {} gl: {} explicit sync: {}",
        client_tier_name(tier_), name_, software_ ? " (software)" : "",
        node_.path[0] ? node_.path : "-",
        node_.path[0] ? (node_.matched ? "the device asked for" : "the first render node")
                      : "no node",
        refusal(client_tier::vulkan)[0] ? refusal(client_tier::vulkan)
                                        : (tier_ == client_tier::vulkan ? "taken" : "not tried"),
        refusal(client_tier::gl)[0] ? refusal(client_tier::gl)
                                    : (tier_ == client_tier::gl ? "taken" : "not tried"),
        timeline_ ? "timeline syncobjs" : "none");
    return {};
}

// ── client_swapchain: the ring ──────────────────────────────────────────────────

client_swapchain::~client_swapchain() { destroy(); }

lx::result<void> client_swapchain::create(client_gpu& gpu, const swapchain_options& options) {
    destroy();
    if (gpu.tier() == client_tier::none)
        return lx::invalid_argument("client_swapchain::create: no device");
    if (options.width == 0 || options.height == 0 || options.width > 16384 ||
        options.height > 16384)
        return lx::invalid_argument("client_swapchain::create: bad size");
    const bool dmabuf = gpu.tier() != client_tier::cpu;
    if (dmabuf && options.format.modifier_count == 0)
        return lx::invalid_argument("client_swapchain::create: no format for a dma-buf tier");

    gpu_ = &gpu;
    options_ = options;
    width_ = options.width;
    height_ = options.height;
    count_ = options.images ? options.images
                            : (dmabuf ? k_swapchain_images : k_shm_swapchain_images);
    if (count_ < 2)
        count_ = 2;
    if (count_ > k_swapchain_images)
        count_ = k_swapchain_images;
    fourcc_ = dmabuf ? options.format.fourcc
                     : (options.opaque ? k_fourcc_xrgb8888 : k_fourcc_argb8888);

    // Explicit sync: the window asked, the tier can export a GPU completion, and the kernel
    // timeline exists (or the caller brought its own ops). Final after the first image (the
    // Vulkan tier learns only then whether its semaphores are syncobjs).
    ops_ = options.timeline.fd_to_handle ? options.timeline
                                         : drm_timeline_ops_for(gpu.drm_fd());
    const bool kernel = options.timeline.fd_to_handle != nullptr || gpu.timeline_syncobj();
    explicit_ = dmabuf && options.explicit_sync && kernel && ops_.query && ops_.eventfd &&
                ops_.fd_to_handle;
    explicit_settled_ = false;
    if (gpu.tier() == client_tier::gl) {
        auto* egl = gpu.egl();
        native_fence_ = egl && egl->supports_native_fence() && !options.disable_native_fence;
        if (explicit_ && (!native_fence_ || !ops_.create || !ops_.handle_to_fd ||
                          !ops_.import_sync_file)) {
            log_swapchain.info("explicit sync unavailable on GL ({}): implicit sync and "
                               "wl_buffer.release",
                               native_fence_ ? "no syncobj create/export"
                               : options.disable_native_fence
                                   ? "native fence turned off"
                                   : "no EGL_ANDROID_native_fence_sync");
            explicit_ = false;
        }
    }
    if (explicit_) {
        const int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (efd < 0) {
            explicit_ = false;
        } else {
            release_fd_.reset(efd);
        }
    }
    if (dmabuf && options.explicit_sync && !explicit_ && !kernel)
        log_swapchain.info("explicit sync unavailable (no timeline syncobjs on {}): implicit "
                           "sync and wl_buffer.release",
                           gpu.node().path[0] ? gpu.node().path : "the device");
    tier_ = gpu.tier();
    if (tier_ == client_tier::vulkan) {
        if (auto ready = make_vk_pool(); !ready) {
            tier_ = client_tier::none;
            return ready.get_error();
        }
    }
    stats_.allocations = 1;
    return {};
}

void client_swapchain::destroy() {
    if (tier_ == client_tier::none && !gpu_)
        return;
    if (tier_ == client_tier::vulkan)
        vk_wait_device();
    for (slot& s : slots_)
        if (s.state != slot_state::empty)
            destroy_image(s);
    for (unsigned i = 0; i < retired_count_; ++i)
        destroy_image(retired_[i]);
    retired_count_ = 0;
    if (tier_ == client_tier::vulkan)
        destroy_vk_pool();
    for (shm_block& b : blocks_) {
        if (b.map)
            ::munmap(b.map, b.bytes);
        b = {};
    }
    current_block_ = -1;
    release_fd_.reset();
    tier_ = client_tier::none;
    gpu_ = nullptr;
    explicit_ = false;
    explicit_settled_ = false;
    frame_ = 0;
    last_presented_ = nullptr;
    for (auto& f : history_frame_)
        f = 0;
}

client_swapchain::slot* client_swapchain::find(std::uint32_t id) {
    for (slot& s : slots_)
        if (s.state != slot_state::empty && s.image.id == id)
            return &s;
    return nullptr;
}

lx::result<void> client_swapchain::make_image(slot& s, unsigned index) {
    s = slot{};
    s.image.id = next_id_++;
    s.image.slot = index;
    s.image.width = width_;
    s.image.height = height_;
    s.image.fourcc = fourcc_;
    lx::result<void> made = lx::not_implemented("lx::gfx::client_swapchain::make_image");
    switch (tier_) {
    case client_tier::cpu: made = make_shm(s); break;
    case client_tier::gl: made = make_gl(s); break;
    case client_tier::vulkan: made = make_vk(s); break;
    case client_tier::none: break;
    }
    if (!made) {
        // The tier's destroy undoes whatever part was made; nobody heard of the image.
        destroy_native(s);
        s = slot{};
        return made.get_error();
    }
    if (explicit_ && s.timeline.get() >= 0) {
        s.image.timeline_fd = s.timeline.get();
        explicit_settled_ = true; // from here on explicit sync never downgrades
    }
    s.state = slot_state::free;
    ++stats_.images_created;
    if (listener_)
        listener_->on_image_created(s.image);
    return {};
}

void client_swapchain::destroy_image(slot& s) {
    if (s.state == slot_state::empty)
        return;
    const std::uint32_t id = s.image.id;
    if (listener_ && id)
        listener_->on_image_destroyed(id);
    destroy_native(s);
    if (last_presented_ == &s.image)
        last_presented_ = nullptr;
    s = slot{};
    ++stats_.images_destroyed;
}

void client_swapchain::destroy_native(slot& s) {
    switch (tier_) {
    case client_tier::cpu:
        if (s.image.pixels.pixels)
            unref_shm_block(s.pool);
        break;
    case client_tier::gl: destroy_gl(s); break;
    case client_tier::vulkan: destroy_vk(s); break;
    case client_tier::none: break;
    }
    if (s.timeline_handle && ops_.destroy)
        ops_.destroy(ops_.user, s.timeline_handle);
    s.timeline_handle = 0;
}

void client_swapchain::retire(slot& s) {
    if (retired_count_ == k_swapchain_retired) {
        // overflow_action::warn_and_drop: the oldest goes now. The compositor keeps its own
        // reference to the memory (its import or mapping), so only our wl_buffer goes early.
        if (stats_.retired_overflow++ == 0)
            log_swapchain.warn("{} resized images still with the compositor: destroying the "
                               "oldest before its release (reported once)",
                               k_swapchain_retired);
        destroy_image(retired_[0]);
        for (unsigned i = 1; i < retired_count_; ++i)
            retired_[i - 1] = std::move(retired_[i]);
        retired_[--retired_count_] = slot{};
    }
    if (last_presented_ == &s.image)
        last_presented_ = nullptr;
    retired_[retired_count_++] = std::move(s);
    s = slot{};
}

void client_swapchain::free_retired(std::uint32_t id) {
    for (unsigned i = 0; i < retired_count_; ++i) {
        if (retired_[i].image.id != id)
            continue;
        destroy_image(retired_[i]);
        for (unsigned j = i + 1; j < retired_count_; ++j)
            retired_[j - 1] = std::move(retired_[j]);
        retired_[--retired_count_] = slot{};
        return;
    }
}

bool client_swapchain::release_signalled(const slot& s) const {
    if (!s.timeline_handle || !ops_.query || s.image.release_point == 0)
        return false;
    bool done = false;
    return ops_.query(ops_.user, s.timeline_handle, s.image.release_point, &done) == 0 && done;
}

lx::result<swapchain_image*> client_swapchain::acquire() {
    lx::runtime::assert_affinity(owner_);
    if (tier_ == client_tier::none)
        return lx::invalid_argument("client_swapchain::acquire: not created");
    slot* best = nullptr;
    const auto pick = [&] {
        for (unsigned i = 0; i < count_; ++i) {
            slot& s = slots_[i];
            if (s.state != slot_state::free)
                continue;
            if (tier_ == client_tier::vulkan && !vk_idle(s))
                continue;
            // The newest content: the smallest age, the least to repaint.
            if (!best || s.last_frame > best->last_frame)
                best = &s;
        }
    };
    pick();
    if (!best && explicit_ && collect_releases() > 0)
        pick();
    if (!best) {
        for (unsigned i = 0; i < count_; ++i) {
            if (slots_[i].state != slot_state::empty)
                continue;
            if (auto made = make_image(slots_[i], i); !made)
                return made.get_error();
            best = &slots_[i];
            break;
        }
    }
    if (!best) {
        ++stats_.acquire_none;
        return static_cast<swapchain_image*>(nullptr);
    }
    lx::result<void> ready{};
    if (tier_ == client_tier::gl)
        ready = acquire_gl(*best);
    else if (tier_ == client_tier::vulkan)
        ready = acquire_vk(*best);
    if (!ready)
        return ready.get_error();
    best->state = slot_state::acquired;
    best->image.age = best->last_frame ? static_cast<unsigned>(frame_ + 1 - best->last_frame) : 0;
    if (explicit_) {
        best->image.acquire_point = 2 * (best->uses + 1) - 1;
        best->image.release_point = 2 * (best->uses + 1);
    }
    return &best->image;
}

void client_swapchain::add_age_damage(const swapchain_image& image, damage_region& out) const {
    if (image.slot >= k_swapchain_images) {
        out.set_full();
        return;
    }
    const slot& s = slots_[image.slot];
    if (image.age == 0 || s.last_frame == 0 || image.age > k_swapchain_damage_history) {
        out.set_full();
        return;
    }
    for (std::uint64_t f = s.last_frame + 1; f <= frame_; ++f) {
        const unsigned i = static_cast<unsigned>(f % k_swapchain_damage_history);
        if (history_frame_[i] != f) {
            out.set_full();
            return;
        }
        out.add_region(history_[i]);
    }
}

lx::result<void> client_swapchain::present(swapchain_image& image, const damage_region& damage) {
    lx::runtime::assert_affinity(owner_);
    if (image.slot >= k_swapchain_images || slots_[image.slot].image.id != image.id ||
        slots_[image.slot].state != slot_state::acquired)
        return lx::invalid_argument("client_swapchain::present: image not acquired");
    slot& s = slots_[image.slot];
    lx::result<void> done{};
    if (tier_ == client_tier::gl)
        done = present_gl(s);
    else if (tier_ == client_tier::vulkan)
        done = present_vk(s);
    if (!done) {
        // Nothing reached the compositor; the content is undefined now.
        s.state = slot_state::free;
        s.last_frame = 0;
        return done.get_error();
    }
    ++frame_;
    const unsigned h = static_cast<unsigned>(frame_ % k_swapchain_damage_history);
    history_frame_[h] = frame_;
    history_[h] = damage;
    s.last_frame = frame_;
    s.state = slot_state::busy;
    ++stats_.presents;
    if (explicit_) {
        ++s.uses;
        ++stats_.explicit_presents;
        // Wake the loop once the compositor is done with the image.
        if (ops_.eventfd(ops_.user, s.timeline_handle, s.image.release_point,
                         release_fd_.get()) != 0 &&
            !warned_fence_) {
            warned_fence_ = true;
            log_swapchain.warn("SYNCOBJ_EVENTFD failed: release points are polled per acquire "
                               "(reported once)");
        }
    }
    last_presented_ = &s.image;
    return {};
}

void client_swapchain::cancel(swapchain_image& image) {
    if (image.slot < k_swapchain_images && slots_[image.slot].image.id == image.id &&
        slots_[image.slot].state == slot_state::acquired)
        slots_[image.slot].state = slot_state::free;
}

void client_swapchain::release(std::uint32_t id) {
    if (explicit_)
        return;
    if (slot* s = find(id); s) {
        if (s->state == slot_state::busy)
            s->state = slot_state::free;
        return;
    }
    free_retired(id);
}

unsigned client_swapchain::collect_releases() {
    if (!explicit_)
        return 0;
    std::uint64_t drained = 0;
    while (::read(release_fd_.get(), &drained, sizeof(drained)) == sizeof(drained)) {
    }
    unsigned freed = 0;
    for (unsigned i = 0; i < count_; ++i) {
        slot& s = slots_[i];
        if (s.state == slot_state::busy && release_signalled(s)) {
            s.state = slot_state::free;
            ++freed;
        }
    }
    for (unsigned i = 0; i < retired_count_;) {
        if (release_signalled(retired_[i])) {
            free_retired(retired_[i].image.id);
            ++freed;
            continue;
        }
        ++i;
    }
    return freed;
}

lx::result<bool> client_swapchain::resize(unsigned width, unsigned height) {
    lx::runtime::assert_affinity(owner_);
    if (tier_ == client_tier::none)
        return lx::invalid_argument("client_swapchain::resize: not created");
    if (width == 0 || height == 0 || width > 16384 || height > 16384)
        return lx::invalid_argument("client_swapchain::resize: bad size");
    if (width == width_ && height == height_)
        return false;
    for (slot& s : slots_) {
        switch (s.state) {
        case slot_state::empty: break;
        case slot_state::free:
        case slot_state::acquired: destroy_image(s); break;
        case slot_state::busy: retire(s); break;
        }
    }
    if (current_block_ >= 0) {
        const unsigned b = static_cast<unsigned>(current_block_);
        current_block_ = -1;
        if (blocks_[b].refs == 0)
            unref_shm_block(b);
    }
    width_ = width;
    height_ = height;
    ++stats_.allocations;
    // The history describes the old images' content; the new ones start at age 0.
    for (auto& f : history_frame_)
        f = 0;
    return true;
}

unsigned client_swapchain::busy() const {
    unsigned n = 0;
    for (const slot& s : slots_)
        n += s.state == slot_state::busy ? 1u : 0u;
    return n;
}

unsigned client_swapchain::retired() const { return retired_count_; }

lx::result<std::uint64_t> client_swapchain::wait_presented() {
    switch (tier_) {
    case client_tier::gl: return wait_gl();
    case client_tier::vulkan: return wait_vk();
    case client_tier::cpu: return std::uint64_t{0};
    case client_tier::none: break;
    }
    return lx::invalid_argument("client_swapchain::wait_presented: not created");
}

// ── the cpu tier ────────────────────────────────────────────────────────────────

lx::result<unsigned> client_swapchain::ensure_shm_block() {
    if (current_block_ >= 0)
        return static_cast<unsigned>(current_block_);
    unsigned index = k_shm_blocks;
    for (unsigned i = 0; i < k_shm_blocks; ++i) {
        if (!blocks_[i].map) {
            index = i;
            break;
        }
    }
    if (index == k_shm_blocks)
        return lx::make_error(lx::wayland_err::object_failed, "shm swapchain: no free block");
    const std::size_t stride = static_cast<std::size_t>(width_) * 4u;
    const std::size_t bytes = stride * height_ * count_;
    const int fd = ::memfd_create("lumen-swapchain", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return lx::make_error(lx::wayland_err::object_failed, "memfd_create failed");
    lx::unique_fd owned{fd};
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0)
        return lx::make_error(lx::wayland_err::object_failed, "ftruncate of the shm pool failed");
    // The compositor maps it: a shrink would SIGBUS it, and the size never changes (a new
    // size is a new block), so grow and further seals are closed too.
    (void)::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
    void* map = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
        return lx::make_error(lx::wayland_err::object_failed, "mmap of the shm pool failed");
    shm_block& b = blocks_[index];
    b.fd = std::move(owned);
    b.map = static_cast<std::byte*>(map);
    b.bytes = bytes;
    b.id = next_pool_id_++;
    b.refs = 0;
    current_block_ = static_cast<int>(index);
    return index;
}

void client_swapchain::unref_shm_block(unsigned index) {
    if (index >= k_shm_blocks)
        return;
    shm_block& b = blocks_[index];
    if (b.refs > 0)
        --b.refs;
    if (b.refs == 0 && static_cast<int>(index) != current_block_ && b.map) {
        ::munmap(b.map, b.bytes);
        b = {};
    }
}

lx::result<void> client_swapchain::make_shm(slot& s) {
    auto block = ensure_shm_block();
    if (!block)
        return block.get_error();
    shm_block& b = blocks_[block.value()];
    const std::size_t stride = static_cast<std::size_t>(width_) * 4u;
    const std::size_t offset = stride * height_ * s.image.slot;
    ++b.refs;
    s.pool = block.value();
    s.image.modifier = k_modifier_linear;
    s.image.plane_count = 1;
    s.image.planes[0] = {b.fd.get(), static_cast<std::uint32_t>(offset),
                         static_cast<std::uint32_t>(stride)};
    s.image.pool_bytes = b.bytes;
    s.image.pool_id = b.id;
    s.image.pixels.pixels = reinterpret_cast<unsigned char*>(b.map + offset);
    s.image.pixels.width = width_;
    s.image.pixels.height = height_;
    s.image.pixels.stride = static_cast<unsigned>(stride);
    s.image.pixels.format = fourcc_;
    return {};
}

} // namespace lx::gfx
