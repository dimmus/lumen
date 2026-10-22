export module lx.compositor:data_transfer;

import std;

export namespace lx::compositor {

/// The mime types a data source offers (wl_data_source, zwp_primary_selection_source_v1),
/// stored inline: filled once per `offer` request, read in place by every delivery. A
/// selection is re-offered on every change of the focused client, so the list is read far
/// more often than it is written — and never allocates either way.
///
/// One `offer` request cannot carry more than a Wayland message (4096 bytes), and the arena
/// is that size: a source fills it only by offering many types, which is bounded too.
class mime_list {
public:
    static constexpr unsigned k_bytes = 4096;
    static constexpr unsigned k_max = 64;

    /// False when full, or when the string is empty or does not fit. A duplicate is ignored
    /// and reported as success: the type is offered either way.
    bool add(const char* mime);
    [[nodiscard]] bool contains(const char* mime) const;
    [[nodiscard]] unsigned count() const { return count_; }
    /// NUL-terminated, inside the arena; valid until the next `clear`.
    [[nodiscard]] const char* at(unsigned i) const {
        return i < count_ ? bytes_ + offset_[i] : nullptr;
    }
    /// Counters only: the arena is overwritten by the next `add`, never cleared.
    void clear() {
        used_ = 0;
        count_ = 0;
    }

private:
    char bytes_[k_bytes]; // deliberately not zero-initialized: a reset is two stores
    std::uint16_t offset_[k_max];
    std::uint16_t used_ = 0;
    std::uint8_t count_ = 0;
};

/// wl_data_device_manager.dnd_action bits.
enum dnd_action_bits : unsigned { dnd_none = 0, dnd_copy = 1, dnd_move = 2, dnd_ask = 4 };

/// Only the three defined bits (wl_data_source/wl_data_offer `invalid_action_mask`).
[[nodiscard]] constexpr bool valid_dnd_mask(unsigned m) { return (m & ~7u) == 0; }
/// None or exactly one defined action (wl_data_offer `invalid_action` for `preferred`).
[[nodiscard]] constexpr bool single_dnd_action(unsigned a) {
    return a == 0 || (valid_dnd_mask(a) && (a & (a - 1)) == 0);
}

/// The action the user asks for with modifiers held during a drag: Shift → move,
/// Ctrl → copy, Alt → ask, checked in that order (Mutter; the protocol's advice). None
/// when no modifier is held.
[[nodiscard]] unsigned user_dnd_action(bool shift, bool ctrl, bool alt);

/// The negotiated drag-and-drop action (wl_data_device_manager.dnd_action "the selected
/// action"): of the actions both sides accept, the user's if available, else the
/// destination's preferred one if available, else the lowest set bit; none when the two
/// sides share nothing. Callers pass `dnd_copy` for a side bound below version 3, which
/// cannot negotiate and is defined to mean copy.
[[nodiscard]] unsigned negotiate_dnd_action(unsigned source, unsigned offer, unsigned preferred,
                                            unsigned user);

} // namespace lx::compositor

bool lx::compositor::mime_list::add(const char* mime) {
    if (!mime || !mime[0])
        return false;
    if (contains(mime))
        return true;
    const std::size_t len = std::strlen(mime);
    if (count_ >= k_max || len + 1 > k_bytes - used_)
        return false;
    std::memcpy(bytes_ + used_, mime, len + 1);
    offset_[count_++] = used_;
    used_ = static_cast<std::uint16_t>(used_ + len + 1);
    return true;
}

bool lx::compositor::mime_list::contains(const char* mime) const {
    if (!mime)
        return false;
    for (unsigned i = 0; i < count_; ++i)
        if (std::strcmp(bytes_ + offset_[i], mime) == 0)
            return true;
    return false;
}

unsigned lx::compositor::user_dnd_action(bool shift, bool ctrl, bool alt) {
    if (shift)
        return dnd_move;
    if (ctrl)
        return dnd_copy;
    if (alt)
        return dnd_ask;
    return dnd_none;
}

unsigned lx::compositor::negotiate_dnd_action(unsigned source, unsigned offer,
                                              unsigned preferred, unsigned user) {
    const unsigned available = source & offer & 7u;
    if (available == 0)
        return dnd_none;
    if (user != 0 && (user & available) == user)
        return user;
    if (preferred != 0 && (preferred & available) == preferred)
        return preferred;
    return available & (~available + 1u); // lowest set bit
}
