module;

// Bodies of lx.wayland.client:data — wl_data_device, wl_data_offer, wl_data_source. The
// offers live in the device's fixed slots: no allocation per event.

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
    if (n == 0 || n >= k_mime_bytes || count_ >= k_offer_mimes || has_mime(mime)) {
        if (n != 0 && !has_mime(mime))
            ++dropped_;
        return;
    }
    std::memcpy(mimes_[count_], mime, n + 1);
    ++count_;
}

#if LX_WLC_REAL

namespace {

const wl_data_offer_listener k_offer_listener = {
    .offer = [](void* data, wl_data_offer*,
                const char* mime) { static_cast<data_offer*>(data)->on_offer(mime); },
    // Drag-and-drop negotiation: not taken.
    .source_actions = [](void*, wl_data_offer*, std::uint32_t) {},
    .action = [](void*, wl_data_offer*, std::uint32_t) {},
};

const wl_data_source_listener k_source_listener = {
    .target = [](void*, wl_data_source*, const char*) {},
    .send = [](void* data, wl_data_source*, const char* mime,
               std::int32_t fd) { static_cast<data_source*>(data)->on_send(mime, fd); },
    .cancelled = [](void* data,
                    wl_data_source*) { static_cast<data_source*>(data)->on_cancelled(); },
    .dnd_drop_performed = [](void*, wl_data_source*) {},
    .dnd_finished = [](void*, wl_data_source*) {},
    .action = [](void*, wl_data_source*, std::uint32_t) {},
};

const wl_data_device_listener k_device_listener = {
    .data_offer =
        [](void* data, wl_data_device*, wl_data_offer* offer) {
            static_cast<data_device*>(data)->on_data_offer(offer);
        },
    .enter = [](void* data, wl_data_device*, std::uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t,
                wl_data_offer* offer) { static_cast<data_device*>(data)->on_enter(offer); },
    .leave = [](void* data, wl_data_device*) { static_cast<data_device*>(data)->on_leave(); },
    .motion = [](void*, wl_data_device*, std::uint32_t, wl_fixed_t, wl_fixed_t) {},
    .drop = [](void* data, wl_data_device*) { static_cast<data_device*>(data)->on_drop(); },
    .selection = [](void* data, wl_data_device*,
                    wl_data_offer* offer) { static_cast<data_device*>(data)->on_selection(offer); },
};

} // namespace

// ── data_offer ──

data_offer::~data_offer() {
    destroy();
}

void data_offer::adopt(wl_data_offer* proxy) {
    destroy();
    proxy_ = proxy;
    wl_proxy_set_tag(reinterpret_cast<wl_proxy*>(proxy_), &lx_wlc_tag);
    wl_data_offer_add_listener(proxy_, &k_offer_listener, this);
}

void data_offer::destroy() {
    if (proxy_)
        wl_data_offer_destroy(proxy_);
    proxy_ = nullptr;
    count_ = 0;
    dropped_ = 0;
}

void data_offer::receive(const char* mime, int fd) {
    if (proxy_ && mime && fd >= 0)
        wl_data_offer_receive(proxy_, mime, fd);
}

// ── data_source ──

data_source::~data_source() {
    destroy();
}

lx::result<void> data_source::create(display& d) {
    d.check_affinity();
    if (proxy_)
        return lx::invalid_argument("lx::wayland::data_source::create: already created");
    auto* manager =
        static_cast<wl_data_device_manager*>(d.native_global(global_kind::data_device_manager));
    if (!manager)
        return lx::make_error(lx::wayland_err::missing_global,
                              "wl_data_device_manager is not available");
    proxy_ = wl_data_device_manager_create_data_source(manager);
    if (!proxy_)
        return lx::make_error(lx::wayland_err::object_failed, "create_data_source failed");
    wl_proxy_set_tag(reinterpret_cast<wl_proxy*>(proxy_), &lx_wlc_tag);
    wl_data_source_add_listener(proxy_, &k_source_listener, this);
    return {};
}

void data_source::destroy() {
    if (proxy_)
        wl_data_source_destroy(proxy_);
    proxy_ = nullptr;
}

void data_source::offer(const char* mime) {
    if (proxy_ && mime && *mime)
        wl_data_source_offer(proxy_, mime);
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

// ── data_device ──

data_device::~data_device() {
    destroy();
}

lx::result<void> data_device::create(display& d, seat& s) {
    d.check_affinity();
    if (proxy_)
        return lx::invalid_argument("lx::wayland::data_device::create: already created");
    auto* manager =
        static_cast<wl_data_device_manager*>(d.native_global(global_kind::data_device_manager));
    if (!manager || !s.bound())
        return lx::make_error(lx::wayland_err::missing_global,
                              "wl_data_device_manager is not available");
    proxy_ = wl_data_device_manager_get_data_device(manager, static_cast<wl_seat*>(s.native()));
    if (!proxy_)
        return lx::make_error(lx::wayland_err::object_failed, "get_data_device failed");
    display_ = &d;
    version_ = wl_data_device_get_version(proxy_);
    wl_proxy_set_tag(reinterpret_cast<wl_proxy*>(proxy_), &lx_wlc_tag);
    wl_data_device_add_listener(proxy_, &k_device_listener, this);
    return {};
}

void data_device::destroy() {
    for (data_offer& o : offers_)
        o.destroy();
    selection_ = drag_ = nullptr;
    if (proxy_) {
        if (version_ >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION)
            wl_data_device_release(proxy_);
        else
            wl_data_device_destroy(proxy_);
    }
    proxy_ = nullptr;
    display_ = nullptr;
}

void data_device::set_selection(data_source* source, std::uint32_t serial) {
    if (!proxy_)
        return;
    wl_data_device_set_selection(
        proxy_, source ? static_cast<wl_data_source*>(source->native()) : nullptr, serial);
}

data_offer* data_device::find(const wl_data_offer* proxy) {
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

void data_device::on_data_offer(wl_data_offer* proxy) {
    if (!proxy)
        return;
    // A free slot, else one held by neither the selection nor a drag (an offer announced
    // and never used, which the protocol does not expect).
    data_offer* slot = nullptr;
    for (data_offer& o : offers_)
        if (!o.proxy_) {
            slot = &o;
            break;
        }
    if (!slot)
        for (data_offer& o : offers_)
            if (&o != selection_ && &o != drag_) {
                slot = &o;
                break;
            }
    if (!slot) {
        ++dropped_;
        log_data.warn("data_device: no slot for a new offer; destroyed");
        wl_data_offer_destroy(proxy);
        return;
    }
    slot->adopt(proxy);
}

void data_device::on_enter(wl_data_offer* proxy) {
    // Drag and drop is not taken: the offer is kept only to be destroyed on leave or drop.
    data_offer* o = find(proxy);
    if (drag_ && drag_ != o)
        retire(drag_);
    drag_ = o;
}

void data_device::on_leave() {
    retire(drag_);
}

void data_device::on_drop() {
    retire(drag_);
}

void data_device::on_selection(wl_data_offer* proxy) {
    data_offer* o = find(proxy);
    if (selection_ && selection_ != o)
        retire(selection_);
    selection_ = o;
    if (handler_)
        handler_->on_selection(*this, selection_);
}

#else // !LX_WLC_REAL

data_offer::~data_offer() = default;
void data_offer::adopt(wl_data_offer*) {}
void data_offer::destroy() {}
void data_offer::receive(const char*, int) {}

data_source::~data_source() = default;
lx::result<void> data_source::create(display&) {
    return lx::not_implemented("lx::wayland::data_source::create");
}
void data_source::destroy() {}
void data_source::offer(const char*) {}
void data_source::on_send(const char*, int fd) {
    if (fd >= 0)
        ::close(fd);
}
void data_source::on_cancelled() {}

data_device::~data_device() = default;
lx::result<void> data_device::create(display&, seat&) {
    return lx::not_implemented("lx::wayland::data_device::create");
}
void data_device::destroy() {}
void data_device::set_selection(data_source*, std::uint32_t) {}
data_offer* data_device::find(const wl_data_offer*) {
    return nullptr;
}
void data_device::retire(data_offer*& slot) {
    slot = nullptr;
}
void data_device::on_data_offer(wl_data_offer*) {}
void data_device::on_enter(wl_data_offer*) {}
void data_device::on_leave() {}
void data_device::on_drop() {}
void data_device::on_selection(wl_data_offer*) {}

#endif // LX_WLC_REAL

} // namespace lx::wayland
