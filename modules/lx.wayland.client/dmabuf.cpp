module;

// Bodies of lx.wayland.client:dmabuf. The format table is a memfd the compositor seals: 16 B
// entries (u32 format, u32 padding, u64 modifier), mapped MAP_PRIVATE read-only as the
// protocol requires, kept until the next table or destroy.

#include "client_glue.h"

module lx.wayland.client;

import std;

import lx.foundation;
import lx.trace;

namespace lx::wayland {

namespace {
constexpr lx::trace::category log_dmabuf{"wayland.client.dmabuf"};
constexpr std::size_t k_table_entry = 16;
} // namespace

bool dmabuf_feedback_snapshot::supports(std::uint32_t format, std::uint64_t modifier) const {
    for (const auto& t : list())
        for (const auto& e : t.entries())
            if (e.format == format && e.modifier == modifier)
                return true;
    return false;
}

const dmabuf_feedback_snapshot& dmabuf_feedback::snapshot() const {
    static const dmabuf_feedback_snapshot empty{};
    return current_ ? *current_ : empty;
}

bool dmabuf_feedback::received() const { return current_ && current_->generation != 0; }

dmabuf_feedback::~dmabuf_feedback() { destroy(); }

void dmabuf_feedback::unmap_table() {
    if (table_)
        ::munmap(const_cast<unsigned char*>(table_), table_bytes_);
    table_ = nullptr;
    table_bytes_ = 0;
}

void dmabuf_feedback::on_format_table(int fd, std::uint32_t size) {
    unmap_table();
    if (size >= k_table_entry) {
        void* p = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p != MAP_FAILED) {
            table_ = static_cast<const unsigned char*>(p);
            table_bytes_ = size;
        } else {
            log_dmabuf.warn("format table of {} B could not be mapped: {}", size,
                            std::strerror(errno));
        }
    }
    ::close(fd);
}

void dmabuf_feedback::on_main_device(std::uint64_t dev) {
    if (pending_)
        pending_->main_device = dev;
}

void dmabuf_feedback::on_tranche_target_device(std::uint64_t dev) {
    if (!pending_)
        return;
    if (!tranche_open_) {
        tranche_open_ = true;
        if (pending_->tranche_count < k_max_dmabuf_tranches)
            pending_->tranches[pending_->tranche_count] = {};
    }
    if (pending_->tranche_count < k_max_dmabuf_tranches)
        pending_->tranches[pending_->tranche_count].target_device = dev;
}

void dmabuf_feedback::on_tranche_flags(std::uint32_t flags) {
    if (!pending_)
        return;
    if (!tranche_open_) {
        tranche_open_ = true;
        if (pending_->tranche_count < k_max_dmabuf_tranches)
            pending_->tranches[pending_->tranche_count] = {};
    }
    if (pending_->tranche_count < k_max_dmabuf_tranches)
        pending_->tranches[pending_->tranche_count].flags = flags;
}

void dmabuf_feedback::on_tranche_formats(const std::uint16_t* indices, std::size_t count) {
    if (!pending_)
        return;
    if (!tranche_open_) {
        tranche_open_ = true;
        if (pending_->tranche_count < k_max_dmabuf_tranches)
            pending_->tranches[pending_->tranche_count] = {};
    }
    if (pending_->tranche_count >= k_max_dmabuf_tranches)
        return; // counted at tranche_done
    dmabuf_tranche& t = pending_->tranches[pending_->tranche_count];
    const std::size_t entries = table_bytes_ / k_table_entry;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint16_t idx = indices[i];
        if (!table_ || idx >= entries) {
            ++pending_->bad_indices;
            continue;
        }
        if (t.count >= k_max_tranche_formats) {
            // overflow_action::warn_and_drop: the compositor lists its preference first.
            ++t.dropped;
            continue;
        }
        const unsigned char* e = table_ + std::size_t{idx} * k_table_entry;
        std::uint32_t format = 0;
        std::uint64_t modifier = 0;
        std::memcpy(&format, e, sizeof(format));
        std::memcpy(&modifier, e + 8, sizeof(modifier));
        t.formats[t.count++] = {format, modifier};
    }
}

void dmabuf_feedback::on_tranche_done() {
    if (!pending_)
        return;
    tranche_open_ = false;
    if (pending_->tranche_count < k_max_dmabuf_tranches)
        ++pending_->tranche_count;
    else
        ++pending_->tranches_dropped;
}

void dmabuf_feedback::on_done() {
    if (!pending_ || !current_)
        return;
    pending_->table_entries = static_cast<std::uint32_t>(table_bytes_ / k_table_entry);
    pending_->generation = current_->generation + 1;
    std::uint32_t dropped = pending_->tranches_dropped;
    for (const auto& t : pending_->list())
        dropped += t.dropped;
    if (dropped && !warned_) {
        warned_ = true;
        log_dmabuf.warn("feedback past the fixed caps ({} tranches × {} formats): {} dropped",
                        k_max_dmabuf_tranches, k_max_tranche_formats, dropped);
    }
    // The finished feedback becomes current by swapping the two blocks: no copy, no
    // allocation. The next feedback starts from empty tranches but keeps the main device
    // (a later `done` may resend only what changed — main_device is resent each time in
    // practice, but the table is not).
    std::swap(current_, pending_);
    const std::uint64_t main_device = current_->main_device;
    pending_->generation = 0;
    pending_->tranche_count = 0;
    pending_->tranches_dropped = 0;
    pending_->bad_indices = 0;
    pending_->main_device = main_device;
    tranche_open_ = false;
    log_dmabuf.debug("feedback {}: main device {:#x}, {} table entries, {} tranches",
                     current_->generation, current_->main_device, current_->table_entries,
                     current_->tranche_count);
    if (handler_)
        handler_->on_feedback(*this, *current_);
}

#if LX_WLC_REAL

namespace {

std::uint64_t read_dev(const wl_array* a) {
    std::uint64_t dev = 0;
    if (a && a->size >= sizeof(dev))
        std::memcpy(&dev, a->data, sizeof(dev));
    else if (a && a->size == sizeof(std::uint32_t)) {
        std::uint32_t d32 = 0;
        std::memcpy(&d32, a->data, sizeof(d32));
        dev = d32;
    }
    return dev;
}

const zwp_linux_dmabuf_feedback_v1_listener k_feedback_listener = {
    .done = [](void* data,
               zwp_linux_dmabuf_feedback_v1*) { static_cast<dmabuf_feedback*>(data)->on_done(); },
    .format_table =
        [](void* data, zwp_linux_dmabuf_feedback_v1*, std::int32_t fd, std::uint32_t size) {
            static_cast<dmabuf_feedback*>(data)->on_format_table(fd, size);
        },
    .main_device =
        [](void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* dev) {
            static_cast<dmabuf_feedback*>(data)->on_main_device(read_dev(dev));
        },
    .tranche_done =
        [](void* data, zwp_linux_dmabuf_feedback_v1*) {
            static_cast<dmabuf_feedback*>(data)->on_tranche_done();
        },
    .tranche_target_device =
        [](void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* dev) {
            static_cast<dmabuf_feedback*>(data)->on_tranche_target_device(read_dev(dev));
        },
    .tranche_formats =
        [](void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* indices) {
            static_cast<dmabuf_feedback*>(data)->on_tranche_formats(
                static_cast<const std::uint16_t*>(indices->data),
                indices->size / sizeof(std::uint16_t));
        },
    .tranche_flags =
        [](void* data, zwp_linux_dmabuf_feedback_v1*, std::uint32_t flags) {
            static_cast<dmabuf_feedback*>(data)->on_tranche_flags(flags);
        },
};

} // namespace

lx::result<void> dmabuf_feedback::create(display& d, surface* target) {
    d.check_affinity();
    if (proxy_)
        return lx::invalid_argument("lx::wayland::dmabuf_feedback::create: already created");
    auto* manager = static_cast<zwp_linux_dmabuf_v1*>(d.native_global(global_kind::dmabuf));
    if (!manager || d.version(global_kind::dmabuf) < 4)
        return lx::make_error(lx::wayland_err::missing_global,
                              "zwp_linux_dmabuf_v1 v4 (feedback) is not available");
    if (!current_)
        current_ = std::make_unique<dmabuf_feedback_snapshot>();
    if (!pending_)
        pending_ = std::make_unique<dmabuf_feedback_snapshot>();
    proxy_ = target ? zwp_linux_dmabuf_v1_get_surface_feedback(
                          manager, static_cast<wl_surface*>(target->native()))
                    : zwp_linux_dmabuf_v1_get_default_feedback(manager);
    if (!proxy_)
        return lx::make_error(lx::wayland_err::object_failed, "dmabuf feedback request failed");
    wl_proxy_set_tag(reinterpret_cast<wl_proxy*>(proxy_), &lx_wlc_tag);
    zwp_linux_dmabuf_feedback_v1_add_listener(proxy_, &k_feedback_listener, this);
    return {};
}

void dmabuf_feedback::destroy() {
    if (proxy_)
        zwp_linux_dmabuf_feedback_v1_destroy(proxy_);
    proxy_ = nullptr;
    unmap_table();
}

#else

lx::result<void> dmabuf_feedback::create(display&, surface*) {
    return lx::not_implemented("lx::wayland::dmabuf_feedback::create");
}
void dmabuf_feedback::destroy() { unmap_table(); }

#endif

} // namespace lx::wayland
