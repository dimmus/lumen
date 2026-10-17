module;

// Text input and input methods (text-input-unstable-v3 v2, input-method-unstable-v2): the
// relay between the focused client's zwp_text_input_v3 objects and the seat's one
// zwp_input_method_v2, and where the input method's popups go. Pure and Wayland-free:
// resources and clients are opaque pointers, every event leaves through a `text_input_sink`
// the wire units implement (wire_text_input.cpp, wire_input_method.cpp). UI affinity
// throughout. See docs/architecture.md, "Text input and input methods". The member bodies are
// in text_input.cpp: this interface carries declarations only (the reason is in
// session_lock.cpp — Clang 23 mis-merges foundation value types copied in partition bodies).
//
// Focus (TI1). Text-input focus is the keyboard focus, and only while an input method is
// bound (sway): `enter` goes to every text input of the focused surface's client when both
// hold, `leave` when either stops. A toolkit treats an entered text input as "an input method
// is here" and routes text through it; entered without one, GTK and Qt lose their own
// compose / dead-key handling for nothing. When the input method goes, text inputs get
// `leave` (which also clears any preedit they show) and get `enter` again when one returns.
// After `leave`, requests are ignored until the next `enter` — commits are still counted,
// because `done` must carry the client's own commit count.
//
// State (TI2). Requests edit the pending state; `enable` and `disable` reset it (the spec's
// "resets all state" and "all state information is invalidated"); `commit` makes it current.
// The cause resets to input_method after each commit. A text input is *active* when it is
// enabled and focused; an enable while another text input of the seat is enabled is ignored.
// Version 2's cursor rectangle is two-stage: the text-input commit stages it, the next
// wl_surface.commit of the focused surface applies it (`surface_committed`), so the popup
// moves in the same frame as the text it points at; enable/disable and focus changes drop
// both. Version 1 applies it at the text-input commit.
//
// The input method (TI3, TI4). Becoming active: `activate`, then the state, then `done`;
// a state commit while active: the state again (surrounding text and the cause reset at
// every IM `done`, so they are always resent — sway does the same), then `done`; losing
// activity: `deactivate`, `done`. A text input enabled again while already enabled (a new
// field) is a `deactivate` and an `activate` in one `done`: the input method resets its
// context (fcitx5 releases and re-takes its grab on exactly that). The input method's commit
// counts only when its serial equals the `done`s sent; otherwise its pending state is dropped
// (wlroots). A valid commit sends the active text input the preedit, commit string and
// deletion that were set, then `done(commits)`.
//
// Popups (TI5). A zwp_input_popup_surface_v2 is shown while the input method is active and
// the popup has content, placed at the active text input's cursor rectangle in global
// logical coordinates through `:positioner`: below and right of it, flipped above / to its
// left (sway's rule) when that does not fit the output, slid as a last resort.
// `text_input_rectangle` tells the popup where the cursor rectangle is relative to itself,
// on every change. Without a cursor rectangle the anchor is the whole surface (sway); a v2
// text input whose first rectangle is still staged has its popups wait for the commit that
// applies it, so they never appear in one place and jump to another. The relay asks the
// sink where the focused surface is
// (`surface_rect`); the compositor maps the popup in the IME popup layer.

export module lx.compositor:text_input;

import lx.foundation;

export namespace lx::compositor {

/// Bytes of text either protocol carries in one string (their own limit, under libwayland's
/// 4096-byte message).
inline constexpr unsigned k_ime_text_max = 4000;

/// zwp_text_input_v3 state, double-buffered by its commit (pending → current).
struct text_input_state {
    bool enabled = false;
    bool has_surrounding = false;
    unsigned cursor = 0;
    unsigned anchor = 0;
    unsigned change_cause = 0;
    bool has_content_type = false;
    unsigned hint = 0;
    unsigned purpose = 0;
    bool has_cursor_rect = false;
    lx::rect2i cursor_rect{};
    /// v2 set_available_actions: one bit per zwp_text_input_v3.action value.
    unsigned actions = 0;
    unsigned surrounding_len = 0;
    char surrounding[k_ime_text_max + 1]{};
};

/// One zwp_text_input_v3. Allocated by the wire unit per get_text_input (client-driven,
/// never per frame), registered with the relay for its lifetime.
struct text_input_record {
    void* resource = nullptr;
    /// The owning wl_client*.
    void* client = nullptr;
    unsigned version = 1;
    /// The wl_surface it was sent `enter` for; null after `leave` (or before any enter).
    void* focus = nullptr;
    /// An enable or disable request since the last commit.
    bool toggled = false;
    text_input_state pending{};
    text_input_state current{};
    /// Commit requests so far: the serial of every `done` (the spec's count).
    unsigned commits = 0;
    /// v2: the cursor rectangle the last commit staged, applied at the focused surface's
    /// next wl_surface.commit.
    bool rect_staged = false;
    bool staged_has_rect = false;
    lx::rect2i staged_rect{};
    text_input_record* prev = nullptr;
    text_input_record* next = nullptr;
    bool linked = false;
};

/// A string the input method set, double-buffered by its commit.
struct ime_string {
    bool set = false;
    unsigned len = 0;
    char text[k_ime_text_max + 1]{};
};

/// zwp_input_method_v2. Allocated per get_input_method.
struct input_method_record {
    void* resource = nullptr;
    void* client = nullptr;
    /// The seat's input method. False for one that was sent `unavailable` (another was bound
    /// already) and for one the relay has let go of: every request is then ignored.
    bool live = false;
    /// `activate` was sent and no `deactivate` since.
    bool active = false;
    /// `done` events sent: what a commit's serial must equal.
    unsigned dones = 0;
    ime_string preedit{};
    int preedit_begin = 0;
    int preedit_end = 0;
    ime_string commit{};
    bool has_delete = false;
    unsigned delete_before = 0;
    unsigned delete_after = 0;
};

/// zwp_input_popup_surface_v2. Allocated per get_input_popup_surface.
struct input_popup_record {
    void* resource = nullptr;
    /// Its wl_surface resource.
    void* surface = nullptr;
    /// The input method it belongs to; null once that is gone (inert: never shown again).
    input_method_record* im = nullptr;
    bool has_content = false;
    lx::size2i size{};
    /// Shown at `origin` (global logical), as the sink was last told.
    bool shown = false;
    lx::point2i origin{};
    bool rect_sent = false;
    lx::rect2i sent_rect{};
    input_popup_record* prev = nullptr;
    input_popup_record* next = nullptr;
    bool linked = false;
};

/// Where the relay's decisions go (the wire units; tests record them).
struct text_input_sink {
    void* user = nullptr;
    // zwp_text_input_v3
    void (*enter)(void* user, void* text_input, void* surface) = nullptr;
    void (*leave)(void* user, void* text_input, void* surface) = nullptr;
    /// `text` null: no preedit (never sent by the relay today; kept for symmetry).
    void (*preedit)(void* user, void* text_input, const char* text, int begin, int end) = nullptr;
    void (*commit_string)(void* user, void* text_input, const char* text) = nullptr;
    void (*delete_surrounding)(void* user, void* text_input, unsigned before,
                               unsigned after) = nullptr;
    void (*done)(void* user, void* text_input, unsigned serial) = nullptr;
    // zwp_input_method_v2
    void (*im_activate)(void* user, void* im) = nullptr;
    void (*im_deactivate)(void* user, void* im) = nullptr;
    void (*im_surrounding)(void* user, void* im, const char* text, unsigned cursor,
                           unsigned anchor) = nullptr;
    void (*im_change_cause)(void* user, void* im, unsigned cause) = nullptr;
    void (*im_content_type)(void* user, void* im, unsigned hint, unsigned purpose) = nullptr;
    void (*im_done)(void* user, void* im) = nullptr;
    /// Keys go to the input method's keyboard grab (true) or to the focused client (false):
    /// the input method became active or stopped being so (design T2).
    void (*routing)(void* user, bool to_input_method) = nullptr;
    /// Where `surface` is (global logical rect of its node) and the logical rect of the
    /// output to keep popups on. False when it is not on screen (no popup is shown then).
    bool (*surface_rect)(void* user, void* surface, lx::rect2i& global, lx::rect2i& bounds) = nullptr;
    // zwp_input_popup_surface_v2
    void (*popup_show)(void* user, void* popup, void* surface, lx::point2i origin) = nullptr;
    void (*popup_hide)(void* user, void* popup, void* surface) = nullptr;
    void (*popup_rect)(void* user, void* popup, lx::rect2i rect) = nullptr;
};

/// Popup placement: the popup's global logical origin and the cursor rectangle relative to it.
struct input_popup_placement {
    lx::point2i origin{};
    lx::rect2i text_rect{};
};

/// Places a popup of `size` at `anchor` (the cursor rectangle, global logical; zero-sized when
/// the text input has none) within `bounds` (the output's logical rect): below and starting at
/// its left edge, flipped above and/or to end at its right edge when that does not fit (sway),
/// slid inside as the last resort. Through `:positioner`'s tested constraint code.
[[nodiscard]] input_popup_placement place_input_popup(lx::rect2i anchor, lx::size2i size,
                                                      lx::rect2i bounds);

/// The longest prefix of `text` (`len` bytes) that is at most `max` bytes and does not end
/// inside a UTF-8 sequence.
[[nodiscard]] unsigned utf8_truncate(const char* text, unsigned len, unsigned max);

/// The text-input ↔ input-method relay of the seat. UI affinity.
class text_input_relay {
public:
    void set_sink(const text_input_sink& sink);

    // ── keyboard focus ──

    /// The keyboard focus moved to `surface` (a wl_surface resource of `client`; null: none).
    void keyboard_focus(void* surface, void* client);
    /// The focused wl_surface is being destroyed: text inputs lose it without `leave`.
    void focus_destroyed(void* surface);

    // ── zwp_text_input_v3 ──

    /// A new text input (get_text_input). It gets `enter` at once when its client has the
    /// keyboard focus and an input method is bound.
    void add(text_input_record& ti);
    /// Destroyed: an active text input deactivates the input method.
    void remove(text_input_record& ti);
    void enable(text_input_record& ti);
    void disable(text_input_record& ti);
    /// `text` (`len` bytes, truncated to k_ime_text_max at a UTF-8 boundary); `cursor` and
    /// `anchor` are clamped into it.
    void set_surrounding(text_input_record& ti, const char* text, unsigned len, int cursor,
                         int anchor);
    void set_change_cause(text_input_record& ti, unsigned cause);
    void set_content_type(text_input_record& ti, unsigned hint, unsigned purpose);
    void set_cursor_rect(text_input_record& ti, lx::rect2i rect);
    void set_actions(text_input_record& ti, unsigned actions);
    void commit(text_input_record& ti);
    /// wl_surface.commit of `surface` was applied: v2 staged cursor rectangles of text inputs
    /// focused on it become current.
    void surface_committed(void* surface);
    /// `surface` moved on screen, or the outputs changed: popups follow.
    void surface_moved(void* surface);
    void outputs_changed();

    // ── zwp_input_method_v2 ──

    /// A new input method. False when the seat has one already: the caller sends
    /// `unavailable`, and the record stays not live.
    [[nodiscard]] bool add_input_method(input_method_record& im);
    /// Destroyed (or its client is gone): focused text inputs get `leave`, popups go inert.
    void remove_input_method(input_method_record& im);
    void im_commit_string(input_method_record& im, const char* text, unsigned len);
    void im_preedit(input_method_record& im, const char* text, unsigned len, int begin, int end);
    void im_delete_surrounding(input_method_record& im, unsigned before, unsigned after);
    void im_commit(input_method_record& im, unsigned serial);

    // ── zwp_input_popup_surface_v2 ──

    void add_popup(input_popup_record& popup, input_method_record& im);
    /// Destroyed (or its wl_surface is): hidden if shown.
    void remove_popup(input_popup_record& popup);
    /// The popup's wl_surface committed: `has_content` and its logical size.
    void popup_committed(input_popup_record& popup, bool has_content, lx::size2i size);

    // ── queries ──

    [[nodiscard]] text_input_record* active() const { return active_; }
    [[nodiscard]] input_method_record* input_method() const { return im_; }
    /// Keys go to the input method's grab.
    [[nodiscard]] bool routing() const { return routing_; }
    [[nodiscard]] void* focus() const { return focus_; }
    [[nodiscard]] unsigned text_input_count() const { return ti_count_; }
    [[nodiscard]] unsigned popup_count() const { return popup_count_; }
    /// Every popup record, newest first (the hit-test walks the shown ones).
    template<class Fn>
    void for_each_popup(Fn&& fn) const {
        for (input_popup_record* p = popups_; p; p = p->next)
            fn(*p);
    }

private:
    void send_enter(text_input_record& ti, void* surface);
    void send_leave(text_input_record& ti);
    void update_active(text_input_record* reenabled);
    void send_state(const text_input_record& ti);
    void im_done();
    void set_routing(bool on);
    void update_popups();
    void hide_popup(input_popup_record& p);
    [[nodiscard]] bool other_enabled(const text_input_record& ti) const;

    text_input_sink sink_{};
    text_input_record* tis_ = nullptr;
    unsigned ti_count_ = 0;
    input_popup_record* popups_ = nullptr;
    unsigned popup_count_ = 0;
    input_method_record* im_ = nullptr;
    text_input_record* active_ = nullptr;
    void* focus_ = nullptr;
    void* focus_client_ = nullptr;
    bool routing_ = false;
};

} // namespace lx::compositor
