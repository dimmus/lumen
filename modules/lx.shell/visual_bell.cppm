module;

// The visual bell (design-app-compat-capture B.3, B1): when a client rings xdg-system-bell-v1
// the compositor tells the shell (zlm_policy_bridge_v1 v6 `bell`), and the shell flashes the
// window — a translucent white overlay-layer surface over the window's rect for 120 ms — or,
// for a bell of no window, the panel strip of the active output. Mutter flashes the window
// or the screen the same way; nothing is audible.
//
// Cheap by construction: one 1×1 premultiplied ARGB shm buffer shared by every flash, scaled
// to the rect by wp_viewport (no pixels proportional to the window are ever written), an
// empty input region (clicks go through), no keyboard, exclusive zone -1 (placed exactly
// over the window, whatever panels reserve). At most four flashes at once; a fifth renews
// the oldest. Shell thread only; allocates nothing per flash beyond the protocol objects.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_SHELL_WAYLAND)
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#if defined(LUMEN_PROTOCOL_VIEWPORTER)
#include "viewporter-client-protocol.h"
#define LUMEN_BELL_VIEWPORT 1
#endif
#endif

import lx.foundation;
import lx.trace;
import lx.wayland.client;

export module lx.shell:visual_bell;

import std;

export namespace lx::shell {

struct visual_bell_globals {
    /// The connection its wl_surfaces and layer surfaces come from.
    lx::wayland::display* display = nullptr;
    void* compositor = nullptr;  // wl_compositor*
    void* shm = nullptr;         // wl_shm*
    void* viewporter = nullptr;  // wp_viewporter* (without it there is no flash)
};

class visual_bell {
public:
    static constexpr unsigned k_max = 4;
    static constexpr long long k_duration_ns = 120'000'000LL;

    visual_bell() = default;
    ~visual_bell();
    visual_bell(const visual_bell&) = delete;
    visual_bell& operator=(const visual_bell&) = delete;

    void init(visual_bell_globals g) { g_ = g; }
    /// Flashes `rect` (logical, relative to the output's origin) on output slot `output_slot`
    /// (`display::output_at`) until `now_ns` + 120 ms. False when it cannot (no viewporter,
    /// no layer shell, empty rect).
    bool flash(unsigned output_slot, lx::rect2i rect, long long now_ns);
    /// When the next flash ends (0: none showing).
    [[nodiscard]] long long deadline() const;
    /// Removes the flashes that are over.
    void expire(long long now_ns);
    void destroy_all();
    [[nodiscard]] unsigned showing() const;
    [[nodiscard]] unsigned total() const { return total_; }

    struct events;

private:
    friend struct events;
    struct flash_surface final : lx::wayland::layer_handler {
        visual_bell* owner = nullptr;
        lx::wayland::surface wl{};
        lx::wayland::layer_surface layer{};
        void* surface = nullptr;  // wl_surface* (wl.native() while showing)
        void* viewport = nullptr; // wp_viewport*
        lx::rect2i rect{};
        long long until_ns = 0;
        void on_configure(lx::wayland::layer_surface& l, const lx::wayland::layer_configure& c) override;
        void on_closed(lx::wayland::layer_surface& l) override;
    };
    void destroy(flash_surface& f);
    [[nodiscard]] bool ensure_buffer();

    visual_bell_globals g_{};
    flash_surface flashes_[k_max]{};
    void* buffer_ = nullptr; // wl_buffer*, 1×1
    unsigned total_ = 0;
};

} // namespace lx::shell

#if defined(LUMEN_SHELL_WAYLAND) && defined(LUMEN_BELL_VIEWPORT)

struct lx::shell::visual_bell::events {};

void lx::shell::visual_bell::flash_surface::on_configure(lx::wayland::layer_surface& l,
                                                         const lx::wayland::layer_configure& c) {
    l.ack_configure(c.serial);
    if (!surface || !owner->buffer_)
        return;
    auto* s = static_cast<wl_surface*>(surface);
    wl_surface_attach(s, static_cast<wl_buffer*>(owner->buffer_), 0, 0);
    wp_viewport_set_destination(static_cast<wp_viewport*>(viewport), rect.width, rect.height);
    wl_surface_damage_buffer(s, 0, 0, 1, 1);
    wl_surface_commit(s);
}

void lx::shell::visual_bell::flash_surface::on_closed(lx::wayland::layer_surface&) {
    owner->destroy(*this);
}

bool lx::shell::visual_bell::ensure_buffer() {
    if (buffer_)
        return true;
    const int fd = ::memfd_create("lumen-bell", MFD_CLOEXEC);
    if (fd < 0 || ::ftruncate(fd, 4) != 0) {
        if (fd >= 0)
            ::close(fd);
        return false;
    }
    // 35 % white, premultiplied: a flash that lightens what is under it without hiding it.
    const std::uint32_t px = 0x59595959u;
    (void)!::pwrite(fd, &px, sizeof(px), 0);
    wl_shm_pool* pool = wl_shm_create_pool(static_cast<wl_shm*>(g_.shm), fd, 4);
    ::close(fd);
    if (!pool)
        return false;
    buffer_ = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    return buffer_ != nullptr;
}

bool lx::shell::visual_bell::flash(unsigned output_slot, lx::rect2i rect, long long now_ns) {
    if (!g_.display || !g_.compositor || !g_.shm || !g_.viewporter || rect.width <= 0 ||
        rect.height <= 0 || !g_.display->has(lx::wayland::global_kind::layer_shell) ||
        !ensure_buffer())
        return false;
    flash_surface* slot = nullptr;
    for (auto& f : flashes_) {
        if (!f.surface) {
            slot = &f;
            break;
        }
        if (!slot || f.until_ns < slot->until_ns)
            slot = &f;
    }
    if (slot->surface)
        destroy(*slot);
    if (!slot->wl.create(*g_.display))
        return false;
    if (auto made = slot->layer.create(*g_.display, slot->wl,
                                       {.output_slot = output_slot,
                                        .level = lx::wayland::layer::overlay,
                                        .name_space = "lumen-bell"});
        !made) {
        slot->wl.destroy();
        return false;
    }
    auto* surface = static_cast<wl_surface*>(slot->wl.native());
    auto* viewport = wp_viewporter_get_viewport(static_cast<wp_viewporter*>(g_.viewporter), surface);
    slot->owner = this;
    slot->surface = surface;
    slot->viewport = viewport;
    slot->rect = rect;
    slot->until_ns = now_ns + k_duration_ns;
    slot->layer.set_handler(slot);
    slot->layer.set_anchor(lx::wayland::anchor_top | lx::wayland::anchor_left);
    slot->layer.set_margin(rect.y, 0, 0, rect.x);
    slot->layer.set_size({rect.width, rect.height});
    slot->layer.set_exclusive_zone(-1);
    (void)slot->layer.set_keyboard_interactivity(lx::wayland::keyboard_interactivity::none);
    // An empty input region: the flash never takes a click.
    if (auto* region = wl_compositor_create_region(static_cast<wl_compositor*>(g_.compositor))) {
        wl_surface_set_input_region(surface, region);
        wl_region_destroy(region);
    }
    wl_surface_commit(surface);
    ++total_;
    return true;
}

void lx::shell::visual_bell::destroy(flash_surface& f) {
    if (f.viewport)
        wp_viewport_destroy(static_cast<wp_viewport*>(f.viewport));
    f.layer.destroy();
    f.wl.destroy();
    f.owner = nullptr;
    f.surface = nullptr;
    f.viewport = nullptr;
    f.rect = {};
    f.until_ns = 0;
}

void lx::shell::visual_bell::destroy_all() {
    for (auto& f : flashes_)
        if (f.surface)
            destroy(f);
    if (buffer_) {
        wl_buffer_destroy(static_cast<wl_buffer*>(buffer_));
        buffer_ = nullptr;
    }
}

#else

struct lx::shell::visual_bell::events {};
bool lx::shell::visual_bell::ensure_buffer() { return false; }
void lx::shell::visual_bell::flash_surface::on_configure(lx::wayland::layer_surface&,
                                                         const lx::wayland::layer_configure&) {}
void lx::shell::visual_bell::flash_surface::on_closed(lx::wayland::layer_surface&) {}
bool lx::shell::visual_bell::flash(unsigned, lx::rect2i, long long) { return false; }
void lx::shell::visual_bell::destroy(flash_surface& f) {
    f.surface = nullptr;
    f.until_ns = 0;
}
void lx::shell::visual_bell::destroy_all() {}

#endif

// Not here: the display may be gone by now. The shell tears flashes down before it disconnects.
lx::shell::visual_bell::~visual_bell() = default;

long long lx::shell::visual_bell::deadline() const {
    long long d = 0;
    for (const auto& f : flashes_)
        if (f.surface && (!d || f.until_ns < d))
            d = f.until_ns;
    return d;
}

void lx::shell::visual_bell::expire(long long now_ns) {
    for (auto& f : flashes_)
        if (f.surface && f.until_ns <= now_ns)
            destroy(f);
}

unsigned lx::shell::visual_bell::showing() const {
    unsigned n = 0;
    for (const auto& f : flashes_)
        n += f.surface ? 1u : 0u;
    return n;
}
