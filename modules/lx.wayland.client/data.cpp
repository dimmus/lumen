module;

// Bodies of lx.wayland.client:data — wl_data_device / zwp_primary_selection_device_v1, their
// offers and sources. The offers live in the device's fixed slots: no allocation per event.

#include "client_glue.h"

module lx.wayland.client;

import std;

import lx.foundation;
import lx.trace;

namespace lx::wayland {

namespace {

constexpr lx::trace::category log_data{"wayland.client.data"};

} // namespace

bool data_offer::has_mime(const char* mime) const {
    if (!mime)
        return false;
    for (unsigned i = 0; i < count_; ++i)
        if (std::strcmp(mimes_[i], mime) == 0)
            return true;
    return false;
}

void data_offer::on_offer(const char* mime) {
    const std::size_t n = mime ? std::strlen(mime) : 0;
    if (n == 0 || has_mime(mime))
        return;
    if (n >= k_mime_bytes || count_ >= k_offer_mimes) {
        ++dropped_;
        return;
    }
    std::memcpy(mimes_[count_], mime, n + 1);
    ++count_;
}

#if LX_WLC_REAL

namespace {

// ── listeners: user data is the C++ object ──

const wl_data_offer_listener k_offer_listener = {
    .offer = [](void* data, wl_data_offer*,
                const char* mime) { static_cast<data_offer*>(data)->on_offer(mime); },
    .source_actions = [](void* data, wl_data_offer*,
                         std::uint32_t a) { static_cast<data_offer*>(data)->on_source_actions(a); },
    .action = [](void* data, wl_data_offer*,
                 std::uint32_t a) { static_cast<data_offer*>(data)->on_action(a); },
};

const zwp_primary_selection_offer_v1_listener k_primary_offer_listener = {
    .offer = [](void* data, zwp_primary_selection_offer_v1*,
                const char* mime) { static_cast<data_offer*>(data)->on_offer(mime); },
};

const wl_data_source_listener k_source_listener = {
    .target = [](void* data, wl_data_source*,
                 const char* mime) { static_cast<data_source*>(data)->on_target(mime); },
    .send = [](void* data, wl_data_source*, const char* mime,
               std::int32_t fd) { static_cast<data_source*>(data)->on_send(mime, fd); },
    .cancelled = [](void* data,
                    wl_data_source*) { static_cast<data_source*>(data)->on_cancelled(); },
    .dnd_drop_performed =
        [](void* data, wl_data_source*) { static_cast<data_source*>(data)->on_drop_performed(); },
    .dnd_finished = [](void* data,
                       wl_data_source*) { static_cast<data_source*>(data)->on_dnd_finished(); },
    .action = [](void* data, wl_data_source*,
                 std::uint32_t a) { static_cast<data_source*>(data)->on_action(a); },
};

const zwp_primary_selection_source_v1_listener k_primary_source_listener = {
    .send = [](void* data, zwp_primary_selection_source_v1*, const char* mime,
               std::int32_t fd) { static_cast<data_source*>(data)->on_send(mime, fd); },
    .cancelled =
        [](void* data, zwp_primary_selection_source_v1*) {
            static_cast<data_source*>(data)->on_cancelled();
        },
};

const wl_data_device_listener k_device_listener = {
    .data_offer =
        [](void* data, wl_data_device*, wl_data_offer* offer) {
            static_cast<data_device*>(data)->on_data_offer(offer);
        },
    .enter =
        [](void* data, wl_data_device*, std::uint32_t serial, wl_surface* s, wl_fixed_t x,
           wl_fixed_t y, wl_data_offer* offer) {
            static_cast<data_device*>(data)->on_enter(serial, s, wl_fixed_to_double(x),
                                                      wl_fixed_to_double(y), offer);
        },
    .leave = [](void* data, wl_data_device*) { static_cast<data_device*>(data)->on_leave(); },
    .motion =
        [](void* data, wl_data_device*, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
            static_cast<data_device*>(data)->on_motion(wl_fixed_to_double(x),
                                                       wl_fixed_to_double(y));
        },
    .drop = [](void* data, wl_data_device*) { static_cast<data_device*>(data)->on_drop(); },
    .selection = [](void* data, wl_data_device*,
                    wl_data_offer* offer) { static_cast<data_device*>(data)->on_selection(offer); },
};

const zwp_primary_selection_device_v1_listener k_primary_device_listener = {
    .data_offer =
        [](void* data, zwp_primary_selection_device_v1*, zwp_primary_selection_offer_v1* offer) {
            static_cast<data_device*>(data)->on_data_offer(offer);
        },
    .selection =
        [](void* data, zwp_primary_selection_device_v1*, zwp_primary_selection_offer_v1* offer) {
            static_cast<data_device*>(data)->on_selection(offer);
        },
};

[[nodiscard]] bool clip(selection_kind k) {
    return k == selection_kind::clipboard;
}

} // namespace

// ── data_offer ──

data_offer::~data_offer() {
    destroy();
}

void data_offer::adopt(void* proxy, selection_kind kind, std::uint32_t version) {
    destroy();
    proxy_ = proxy;
    kind_ = kind;
    version_ = version;
    wl_proxy_set_tag(static_cast<wl_proxy*>(proxy_), &lx_wlc_tag);
    if (clip(kind))
        wl_data_offer_add_listener(static_cast<wl_data_offer*>(proxy_), &k_offer_listener, this);
    else
        zwp_primary_selection_offer_v1_add_listener(
            static_cast<zwp_primary_selection_offer_v1*>(proxy_), &k_primary_offer_listener, this);
}

void data_offer::destroy() {
    if (proxy_) {
        if (clip(kind_))
            wl_data_offer_destroy(static_cast<wl_data_offer*>(proxy_));
        else
            zwp_primary_selection_offer_v1_destroy(
                static_cast<zwp_primary_selection_offer_v1*>(proxy_));
    }
    proxy_ = nullptr;
    count_ = 0;
    dropped_ = 0;
    source_actions_ = action_ = k_dnd_none;
    accepted_ = false;
}

void data_offer::receive(const char* mime, int fd) {
    if (!proxy_ || !mime || fd < 0)
        return;
    if (clip(kind_))
        wl_data_offer_receive(static_cast<wl_data_offer*>(proxy_), mime, fd);
    else
        zwp_primary_selection_offer_v1_receive(static_cast<zwp_primary_selection_offer_v1*>(proxy_),
                                               mime, fd);
}

void data_offer::accept(std::uint32_t serial, const char* mime) {
    if (!proxy_ || !clip(kind_))
        return;
    wl_data_offer_accept(static_cast<wl_data_offer*>(proxy_), serial, mime);
    accepted_ = mime != nullptr;
}

void data_offer::set_actions(std::uint32_t actions, std::uint32_t preferred) {
    if (proxy_ && clip(kind_) && version_ >= WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION)
        wl_data_offer_set_actions(static_cast<wl_data_offer*>(proxy_), actions, preferred);
}

void data_offer::finish() {
    if (proxy_ && clip(kind_) && accepted_ && version_ >= WL_DATA_OFFER_FINISH_SINCE_VERSION)
        wl_data_offer_finish(static_cast<wl_data_offer*>(proxy_));
}

// ── data_source ──

data_source::~data_source() {
    destroy();
}

lx::result<void> data_source::create(display& d, selection_kind kind) {
    d.check_affinity();
    if (proxy_)
        return lx::invalid_argument("lx::wayland::data_source::create: already created");
    kind_ = kind;
    if (clip(kind)) {
        auto* m =
            static_cast<wl_data_device_manager*>(d.native_global(global_kind::data_device_manager));
        if (!m)
            return lx::make_error(lx::wayland_err::missing_global,
                                  "wl_data_device_manager is not available");
        auto* p = wl_data_device_manager_create_data_source(m);
        if (p)
            wl_data_source_add_listener(p, &k_source_listener, this);
        proxy_ = p;
    } else {
        auto* m = static_cast<zwp_primary_selection_device_manager_v1*>(
            d.native_global(global_kind::primary_selection));
        if (!m)
            return lx::make_error(lx::wayland_err::missing_global,
                                  "zwp_primary_selection_device_manager_v1 is not available");
        auto* p = zwp_primary_selection_device_manager_v1_create_source(m);
        if (p)
            zwp_primary_selection_source_v1_add_listener(p, &k_primary_source_listener, this);
        proxy_ = p;
    }
    if (!proxy_)
        return lx::make_error(lx::wayland_err::object_failed, "create_data_source failed");
    wl_proxy_set_tag(static_cast<wl_proxy*>(proxy_), &lx_wlc_tag);
    return {};
}

void data_source::destroy() {
    if (proxy_) {
        if (clip(kind_))
            wl_data_source_destroy(static_cast<wl_data_source*>(proxy_));
        else
            zwp_primary_selection_source_v1_destroy(
                static_cast<zwp_primary_selection_source_v1*>(proxy_));
    }
    proxy_ = nullptr;
}

void data_source::offer(const char* mime) {
    if (!proxy_ || !mime || !*mime)
        return;
    if (clip(kind_))
        wl_data_source_offer(static_cast<wl_data_source*>(proxy_), mime);
    else
        zwp_primary_selection_source_v1_offer(static_cast<zwp_primary_selection_source_v1*>(proxy_),
                                              mime);
}

void data_source::set_actions(std::uint32_t actions) {
    if (proxy_ && clip(kind_) &&
        wl_data_source_get_version(static_cast<wl_data_source*>(proxy_)) >=
            WL_DATA_SOURCE_SET_ACTIONS_SINCE_VERSION)
        wl_data_source_set_actions(static_cast<wl_data_source*>(proxy_), actions);
}

void data_source::on_send(const char* mime, int fd) {
    if (handler_)
        handler_->on_send(*this, mime ? mime : "", fd);
    else if (fd >= 0)
        ::close(fd);
}

void data_source::on_cancelled() {
    if (handler_)
        handler_->on_cancelled(*this);
}

void data_source::on_target(const char* mime) {
    if (handler_)
        handler_->on_target(*this, mime);
}

void data_source::on_action(std::uint32_t action) {
    if (handler_)
        handler_->on_action(*this, action);
}

void data_source::on_drop_performed() {
    if (handler_)
        handler_->on_drop_performed(*this);
}

void data_source::on_dnd_finished() {
    if (handler_)
        handler_->on_dnd_finished(*this);
}

// ── data_device ──

data_device::~data_device() {
    destroy();
}

lx::result<void> data_device::create(display& d, seat& s, selection_kind kind) {
    d.check_affinity();
    if (proxy_)
        return lx::invalid_argument("lx::wayland::data_device::create: already created");
    if (!s.bound())
        return lx::make_error(lx::wayland_err::missing_global, "no seat for a data device");
    kind_ = kind;
    auto* seat_proxy = static_cast<wl_seat*>(s.native());
    if (clip(kind)) {
        auto* m =
            static_cast<wl_data_device_manager*>(d.native_global(global_kind::data_device_manager));
        if (!m)
            return lx::make_error(lx::wayland_err::missing_global,
                                  "wl_data_device_manager is not available");
        auto* p = wl_data_device_manager_get_data_device(m, seat_proxy);
        if (p) {
            version_ = wl_data_device_get_version(p);
            wl_data_device_add_listener(p, &k_device_listener, this);
        }
        proxy_ = p;
    } else {
        auto* m = static_cast<zwp_primary_selection_device_manager_v1*>(
            d.native_global(global_kind::primary_selection));
        if (!m)
            return lx::make_error(lx::wayland_err::missing_global,
                                  "zwp_primary_selection_device_manager_v1 is not available");
        auto* p = zwp_primary_selection_device_manager_v1_get_device(m, seat_proxy);
        if (p) {
            version_ = zwp_primary_selection_device_v1_get_version(p);
            zwp_primary_selection_device_v1_add_listener(p, &k_primary_device_listener, this);
        }
        proxy_ = p;
    }
    if (!proxy_)
        return lx::make_error(lx::wayland_err::object_failed, "get_data_device failed");
    display_ = &d;
    wl_proxy_set_tag(static_cast<wl_proxy*>(proxy_), &lx_wlc_tag);
    return {};
}

void data_device::destroy() {
    for (data_offer& o : offers_)
        o.destroy();
    selection_ = drag_ = dropped_ = nullptr;
    if (proxy_) {
        if (!clip(kind_))
            zwp_primary_selection_device_v1_destroy(
                static_cast<zwp_primary_selection_device_v1*>(proxy_));
        else if (version_ >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION)
            wl_data_device_release(static_cast<wl_data_device*>(proxy_));
        else
            wl_data_device_destroy(static_cast<wl_data_device*>(proxy_));
    }
    proxy_ = nullptr;
    display_ = nullptr;
}

void data_device::set_selection(data_source* source, std::uint32_t serial) {
    if (!proxy_ || (source && source->kind() != kind_))
        return;
    void* src = source ? source->native() : nullptr;
    if (clip(kind_))
        wl_data_device_set_selection(static_cast<wl_data_device*>(proxy_),
                                     static_cast<wl_data_source*>(src), serial);
    else
        zwp_primary_selection_device_v1_set_selection(
            static_cast<zwp_primary_selection_device_v1*>(proxy_),
            static_cast<zwp_primary_selection_source_v1*>(src), serial);
}

void data_device::start_drag(data_source& source, surface& origin, std::uint32_t serial) {
    if (!proxy_ || !clip(kind_) || !source.valid() || source.kind() != kind_)
        return;
    wl_data_device_start_drag(static_cast<wl_data_device*>(proxy_),
                              static_cast<wl_data_source*>(source.native()),
                              static_cast<wl_surface*>(origin.native()), nullptr, serial);
}

void data_device::finish_drop(bool taken) {
    if (!dropped_)
        return;
    if (taken)
        dropped_->finish();
    retire(dropped_);
}

data_offer* data_device::find(const void* proxy) {
    if (!proxy)
        return nullptr;
    for (data_offer& o : offers_)
        if (o.proxy_ == proxy)
            return &o;
    return nullptr;
}

void data_device::retire(data_offer*& slot) {
    if (slot)
        slot->destroy();
    slot = nullptr;
}

void data_device::on_data_offer(void* proxy) {
    if (!proxy)
        return;
    // A free slot, else one held by none of the selection, a drag or a drop (an offer
    // announced and never used, which the protocol does not expect).
    data_offer* slot = nullptr;
    for (data_offer& o : offers_)
        if (!o.proxy_) {
            slot = &o;
            break;
        }
    if (!slot)
        for (data_offer& o : offers_)
            if (&o != selection_ && &o != drag_ && &o != dropped_) {
                slot = &o;
                break;
            }
    if (!slot) {
        ++dropped_count_;
        log_data.warn("data_device: no slot for a new offer; destroyed");
        if (clip(kind_))
            wl_data_offer_destroy(static_cast<wl_data_offer*>(proxy));
        else
            zwp_primary_selection_offer_v1_destroy(
                static_cast<zwp_primary_selection_offer_v1*>(proxy));
        return;
    }
    slot->adopt(proxy, kind_, version_);
}

void data_device::on_enter(std::uint32_t serial, void* wl_surface, double x, double y,
                           void* proxy) {
    data_offer* o = find(proxy);
    if (drag_ && drag_ != o)
        retire(drag_);
    drag_ = o;
    if (handler_)
        handler_->on_drag_enter(*this, drag_, surface::from_native(wl_surface), x, y, serial);
}

void data_device::on_motion(double x, double y) {
    if (handler_)
        handler_->on_drag_motion(*this, x, y);
}

void data_device::on_leave() {
    if (handler_)
        handler_->on_drag_leave(*this);
    retire(drag_);
}

void data_device::on_drop() {
    // The offer moves to the dropped slot: it lives until `finish_drop`, while a new drag
    // may enter. A drop with no offer (another client's internal drag) has nothing to read.
    if (dropped_ && dropped_ != drag_)
        retire(dropped_);
    dropped_ = drag_;
    drag_ = nullptr;
    if (handler_)
        handler_->on_drop(*this, dropped_);
    else
        retire(dropped_);
}

void data_device::on_selection(void* proxy) {
    data_offer* o = find(proxy);
    if (selection_ && selection_ != o)
        retire(selection_);
    selection_ = o;
    if (handler_)
        handler_->on_selection(*this, selection_);
}

#else // !LX_WLC_REAL

data_offer::~data_offer() = default;
void data_offer::adopt(void*, selection_kind, std::uint32_t) {}
void data_offer::destroy() {}
void data_offer::receive(const char*, int) {}
void data_offer::accept(std::uint32_t, const char*) {}
void data_offer::set_actions(std::uint32_t, std::uint32_t) {}
void data_offer::finish() {}

data_source::~data_source() = default;
lx::result<void> data_source::create(display&, selection_kind) {
    return lx::not_implemented("lx::wayland::data_source::create");
}
void data_source::destroy() {}
void data_source::offer(const char*) {}
void data_source::set_actions(std::uint32_t) {}
void data_source::on_send(const char*, int fd) {
    if (fd >= 0)
        ::close(fd);
}
void data_source::on_cancelled() {}
void data_source::on_target(const char*) {}
void data_source::on_action(std::uint32_t) {}
void data_source::on_drop_performed() {}
void data_source::on_dnd_finished() {}

data_device::~data_device() = default;
lx::result<void> data_device::create(display&, seat&, selection_kind) {
    return lx::not_implemented("lx::wayland::data_device::create");
}
void data_device::destroy() {}
void data_device::set_selection(data_source*, std::uint32_t) {}
void data_device::start_drag(data_source&, surface&, std::uint32_t) {}
void data_device::finish_drop(bool) {}
data_offer* data_device::find(const void*) {
    return nullptr;
}
void data_device::retire(data_offer*& slot) {
    slot = nullptr;
}
void data_device::on_data_offer(void*) {}
void data_device::on_enter(std::uint32_t, void*, double, double, void*) {}
void data_device::on_motion(double, double) {}
void data_device::on_leave() {}
void data_device::on_drop() {}
void data_device::on_selection(void*) {}

#endif // LX_WLC_REAL

} // namespace lx::wayland
