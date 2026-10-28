module;

// lx.wayland.client:data — data devices on a seat, their offers and this client's sources,
// for both selections: the clipboard (wl_data_device, which also carries drag and drop) and
// the primary selection (zwp_primary_selection_device_v1). The kind is chosen at creation;
// the objects are the same to their user. An offer keeps the mime types it was announced
// with in a fixed table; the device keeps the offer being announced, the selection, the
// drag's over this client and a dropped one until its data is read. The data itself moves
// through pipes the caller makes and reads or writes on its own loop. Bodies in data.cpp.

export module lx.wayland.client:data;

import std;

import lx.foundation;
import :types;
import :seat;
import :display;
import :surface;

export namespace lx::wayland {

/// Mime types kept per offer, and bytes per type with the NUL; past either the type is
/// counted in `dropped_mimes` and not kept.
inline constexpr unsigned k_offer_mimes = 16;
inline constexpr unsigned k_mime_bytes = 96;

/// One offer (wl_data_offer or zwp_primary_selection_offer_v1). Owned by its data_device.
/// UI affinity.
class data_offer {
public:
    data_offer() = default;
    ~data_offer();
    data_offer(const data_offer&) = delete;
    data_offer& operator=(const data_offer&) = delete;

    [[nodiscard]] selection_kind kind() const { return kind_; }
    [[nodiscard]] unsigned mime_count() const { return count_; }
    [[nodiscard]] const char* mime_at(unsigned i) const { return i < count_ ? mimes_[i] : nullptr; }
    [[nodiscard]] bool has_mime(const char* mime) const;
    [[nodiscard]] unsigned dropped_mimes() const { return dropped_; }
    /// The source writes `mime` into `fd` (the write end of a pipe; marshalling duplicates
    /// it, so the caller closes its own copy after).
    void receive(const char* mime, int fd);

    // Drag and drop (a clipboard device's drag offer; no-ops otherwise).
    /// The type this client would take at the pointer (null: none), answering `serial`
    /// (the enter's).
    void accept(std::uint32_t serial, const char* mime);
    /// The actions this client takes and the one it prefers (`k_dnd_*`; version 3).
    void set_actions(std::uint32_t actions, std::uint32_t preferred);
    /// The source's actions and the one the compositor chose (`k_dnd_*`).
    [[nodiscard]] std::uint32_t source_actions() const { return source_actions_; }
    [[nodiscard]] std::uint32_t action() const { return action_; }
    [[nodiscard]] void* native() const { return proxy_; }

    // Event entry points.
    void on_offer(const char* mime);
    void on_source_actions(std::uint32_t actions) { source_actions_ = actions; }
    void on_action(std::uint32_t action) { action_ = action; }

private:
    friend class data_device;
    void adopt(void* proxy, selection_kind kind, std::uint32_t version);
    void destroy();
    /// Drag and drop's end for the target (version 3): the data was taken.
    void finish();

    void* proxy_ = nullptr;
    selection_kind kind_ = selection_kind::clipboard;
    std::uint32_t version_ = 0;
    unsigned count_ = 0;
    unsigned dropped_ = 0;
    std::uint32_t source_actions_ = k_dnd_none;
    std::uint32_t action_ = k_dnd_none;
    bool accepted_ = false;
    char mimes_[k_offer_mimes][k_mime_bytes]{};
};

/// One source (wl_data_source or zwp_primary_selection_source_v1): what this client offers.
/// Used once (one set_selection or start_drag). UI affinity.
class data_source {
public:
    data_source() = default;
    ~data_source();
    data_source(const data_source&) = delete;
    data_source& operator=(const data_source&) = delete;

    [[nodiscard]] lx::result<void> create(display& d,
                                          selection_kind kind = selection_kind::clipboard);
    void destroy();
    [[nodiscard]] bool valid() const { return proxy_ != nullptr; }
    [[nodiscard]] selection_kind kind() const { return kind_; }
    void set_handler(data_source_handler* h) { handler_ = h; }
    /// Announces a mime type; before it is used.
    void offer(const char* mime);
    /// The drag-and-drop actions it allows (`k_dnd_*`; a clipboard source of version 3,
    /// before start_drag).
    void set_actions(std::uint32_t actions);
    [[nodiscard]] void* native() const { return proxy_; }

    // Event entry points.
    void on_send(const char* mime, int fd);
    void on_cancelled();
    void on_target(const char* mime);
    void on_action(std::uint32_t action);
    void on_drop_performed();
    void on_dnd_finished();

private:
    void* proxy_ = nullptr;
    selection_kind kind_ = selection_kind::clipboard;
    data_source_handler* handler_ = nullptr;
};

/// A data device on one seat (wl_data_device or zwp_primary_selection_device_v1). UI
/// affinity. Destroyed before the display disconnects.
class data_device {
public:
    /// The offer being announced, the selection, a drag's, a dropped one being read.
    static constexpr unsigned k_offers = 5;

    data_device() = default;
    ~data_device();
    data_device(const data_device&) = delete;
    data_device& operator=(const data_device&) = delete;

    /// Fails with `wayland_err::missing_global` without the kind's manager.
    [[nodiscard]] lx::result<void> create(display& d, seat& s,
                                          selection_kind kind = selection_kind::clipboard);
    void destroy();
    [[nodiscard]] bool valid() const { return proxy_ != nullptr; }
    [[nodiscard]] selection_kind kind() const { return kind_; }
    void set_handler(data_device_handler* h) { handler_ = h; }

    /// Makes `source` (null: none) this seat's selection of the device's kind. `serial` is
    /// the serial of the input event that asked for it (the compositor refuses another,
    /// cancelling the source).
    void set_selection(data_source* source, std::uint32_t serial);
    /// The current selection's offer; null when there is none.
    [[nodiscard]] data_offer* selection() const { return selection_; }

    /// Starts dragging `source` from `origin` (a clipboard device): `serial` is the press of
    /// the button still held (the compositor refuses another, cancelling the source). No
    /// icon: the pointer shows the drag.
    void start_drag(data_source& source, surface& origin, std::uint32_t serial);
    /// The drag over this client's surfaces (null: none) and the dropped offer being read.
    [[nodiscard]] data_offer* drag_offer() const { return drag_; }
    [[nodiscard]] data_offer* dropped_offer() const { return dropped_; }
    /// The dropped offer was read (or given up): `finish` it when it was taken (version 3)
    /// and destroy it.
    void finish_drop(bool taken);
    /// Offers destroyed at once because every slot was held (never expected).
    [[nodiscard]] unsigned dropped_offers() const { return dropped_count_; }

    // Event entry points.
    void on_data_offer(void* proxy);
    void on_enter(std::uint32_t serial, void* wl_surface, double x, double y, void* proxy);
    void on_motion(double x, double y);
    void on_leave();
    void on_drop();
    void on_selection(void* proxy);

private:
    [[nodiscard]] data_offer* find(const void* proxy);
    void retire(data_offer*& slot);

    void* proxy_ = nullptr;
    display* display_ = nullptr;
    data_device_handler* handler_ = nullptr;
    selection_kind kind_ = selection_kind::clipboard;
    data_offer offers_[k_offers]{};
    data_offer* selection_ = nullptr;
    data_offer* drag_ = nullptr;
    data_offer* dropped_ = nullptr;
    std::uint32_t version_ = 0;
    unsigned dropped_count_ = 0;
};

} // namespace lx::wayland
