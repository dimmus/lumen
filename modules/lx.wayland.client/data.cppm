module;

// lx.wayland.client:data — wl_data_device on a seat, its offers and this client's sources:
// the clipboard of the core protocol. An offer keeps the mime types it was announced with
// in a fixed table; the device keeps the offer being announced, the selection and a drag's
// (drag and drop is not taken: a drag's offer is destroyed when it leaves or drops). The
// data itself moves through pipes the caller makes and reads or writes on its own loop.
// Bodies in data.cpp.

struct wl_data_device;
struct wl_data_offer;
struct wl_data_source;

export module lx.wayland.client:data;

import std;

import lx.foundation;
import :types;
import :seat;
import :display;

export namespace lx::wayland {

/// Mime types kept per offer, and bytes per type with the NUL; past either the type is
/// counted in `dropped_mimes` and not kept.
inline constexpr unsigned k_offer_mimes = 16;
inline constexpr unsigned k_mime_bytes = 96;

/// One wl_data_offer. Owned by its data_device. UI affinity.
class data_offer {
public:
    data_offer() = default;
    ~data_offer();
    data_offer(const data_offer&) = delete;
    data_offer& operator=(const data_offer&) = delete;

    [[nodiscard]] unsigned mime_count() const { return count_; }
    [[nodiscard]] const char* mime_at(unsigned i) const { return i < count_ ? mimes_[i] : nullptr; }
    [[nodiscard]] bool has_mime(const char* mime) const;
    [[nodiscard]] unsigned dropped_mimes() const { return dropped_; }
    /// wl_data_offer.receive: the source is asked to write `mime` into `fd` (the write end
    /// of a pipe; marshalling duplicates it, so the caller closes its own copy after).
    void receive(const char* mime, int fd);
    [[nodiscard]] void* native() const { return proxy_; }

    // Event entry points.
    void on_offer(const char* mime);

private:
    friend class data_device;
    void adopt(wl_data_offer* proxy);
    void destroy();

    wl_data_offer* proxy_ = nullptr;
    unsigned count_ = 0;
    unsigned dropped_ = 0;
    char mimes_[k_offer_mimes][k_mime_bytes]{};
};

/// One wl_data_source: what this client offers. Used once (one set_selection). UI affinity.
class data_source {
public:
    data_source() = default;
    ~data_source();
    data_source(const data_source&) = delete;
    data_source& operator=(const data_source&) = delete;

    [[nodiscard]] lx::result<void> create(display& d);
    void destroy();
    [[nodiscard]] bool valid() const { return proxy_ != nullptr; }
    void set_handler(data_source_handler* h) { handler_ = h; }
    /// Announces a mime type; before it is used.
    void offer(const char* mime);
    [[nodiscard]] void* native() const { return proxy_; }

    // Event entry points.
    void on_send(const char* mime, int fd);
    void on_cancelled();

private:
    wl_data_source* proxy_ = nullptr;
    data_source_handler* handler_ = nullptr;
};

/// wl_data_device on one seat. UI affinity. Destroyed before the display disconnects.
class data_device {
public:
    /// The offer being announced, the selection, a drag's, and one spare for an offer
    /// announced while the others are held.
    static constexpr unsigned k_offers = 4;

    data_device() = default;
    ~data_device();
    data_device(const data_device&) = delete;
    data_device& operator=(const data_device&) = delete;

    /// Fails with `wayland_err::missing_global` without wl_data_device_manager.
    [[nodiscard]] lx::result<void> create(display& d, seat& s);
    void destroy();
    [[nodiscard]] bool valid() const { return proxy_ != nullptr; }
    void set_handler(data_device_handler* h) { handler_ = h; }

    /// Makes `source` (null: none) this seat's selection. `serial` is the serial of the
    /// input event that asked for it (the compositor refuses another, cancelling the source).
    void set_selection(data_source* source, std::uint32_t serial);
    /// The current selection's offer; null when there is none.
    [[nodiscard]] data_offer* selection() const { return selection_; }
    /// Offers destroyed at once because every slot was held (never expected).
    [[nodiscard]] unsigned dropped_offers() const { return dropped_; }

    // Event entry points.
    void on_data_offer(wl_data_offer* proxy);
    void on_enter(wl_data_offer* proxy);
    void on_leave();
    void on_drop();
    void on_selection(wl_data_offer* proxy);

private:
    [[nodiscard]] data_offer* find(const wl_data_offer* proxy);
    void retire(data_offer*& slot);

    wl_data_device* proxy_ = nullptr;
    display* display_ = nullptr;
    data_device_handler* handler_ = nullptr;
    data_offer offers_[k_offers]{};
    data_offer* selection_ = nullptr;
    data_offer* drag_ = nullptr;
    std::uint32_t version_ = 0;
    unsigned dropped_ = 0;
};

} // namespace lx::wayland
