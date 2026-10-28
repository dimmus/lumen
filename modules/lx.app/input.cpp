module;

// Bodies of lx.app:input.

#if defined(LUMEN_HAS_XKBCOMMON)
#include <xkbcommon/xkbcommon.h>
#endif

module lx.app;

import std;

import lx.foundation;
import lx.runtime;
import lx.trace;
import lx.ui;
import lx.wayland.client;

namespace lx::app {

namespace {

constexpr lx::trace::category log_input{"app.input"};

// evdev buttons (linux/input-event-codes.h).
constexpr std::uint32_t k_btn_left = 0x110;
constexpr std::uint32_t k_btn_right = 0x111;
constexpr std::uint32_t k_btn_middle = 0x112;

// xkb keysyms (xkbcommon-keysyms.h) the UI names.
constexpr std::uint32_t k_xk_backspace = 0xff08;
constexpr std::uint32_t k_xk_tab = 0xff09;
constexpr std::uint32_t k_xk_return = 0xff0d;
constexpr std::uint32_t k_xk_escape = 0xff1b;
constexpr std::uint32_t k_xk_home = 0xff50;
constexpr std::uint32_t k_xk_left = 0xff51;
constexpr std::uint32_t k_xk_up = 0xff52;
constexpr std::uint32_t k_xk_right = 0xff53;
constexpr std::uint32_t k_xk_down = 0xff54;
constexpr std::uint32_t k_xk_page_up = 0xff55;
constexpr std::uint32_t k_xk_page_down = 0xff56;
constexpr std::uint32_t k_xk_end = 0xff57;
constexpr std::uint32_t k_xk_kp_enter = 0xff8d;
constexpr std::uint32_t k_xk_kp_home = 0xff95;
constexpr std::uint32_t k_xk_kp_left = 0xff96;
constexpr std::uint32_t k_xk_kp_up = 0xff97;
constexpr std::uint32_t k_xk_kp_right = 0xff98;
constexpr std::uint32_t k_xk_kp_down = 0xff99;
constexpr std::uint32_t k_xk_kp_page_up = 0xff9a;
constexpr std::uint32_t k_xk_kp_page_down = 0xff9b;
constexpr std::uint32_t k_xk_kp_end = 0xff9c;
constexpr std::uint32_t k_xk_kp_delete = 0xff9f;
constexpr std::uint32_t k_xk_delete = 0xffff;
constexpr std::uint32_t k_xk_iso_left_tab = 0xfe20;
constexpr std::uint32_t k_xk_space = 0x20;

[[nodiscard]] lx::ui::key ui_key(std::uint32_t keysym) {
    using lx::ui::key;
    switch (keysym) {
    case k_xk_tab:
    case k_xk_iso_left_tab: return key::tab;
    case k_xk_backspace: return key::backspace;
    case k_xk_return:
    case k_xk_kp_enter: return key::enter;
    case k_xk_escape: return key::escape;
    case k_xk_left:
    case k_xk_kp_left: return key::left;
    case k_xk_right:
    case k_xk_kp_right: return key::right;
    case k_xk_up:
    case k_xk_kp_up: return key::up;
    case k_xk_down:
    case k_xk_kp_down: return key::down;
    case k_xk_space: return key::space;
    case k_xk_home:
    case k_xk_kp_home: return key::home;
    case k_xk_end:
    case k_xk_kp_end: return key::end;
    case k_xk_page_up:
    case k_xk_kp_page_up: return key::page_up;
    case k_xk_page_down:
    case k_xk_kp_page_down: return key::page_down;
    case k_xk_delete:
    case k_xk_kp_delete: return key::delete_forward;
    default: return key::unknown;
    }
}

/// lx.wayland.client's modifier bits → lx.ui's (xkb's real modifier masks).
[[nodiscard]] unsigned ui_modifiers(std::uint32_t m) {
    unsigned out = 0;
    if (m & lx::wayland::mod_shift)
        out |= lx::ui::k_mod_shift;
    if (m & lx::wayland::mod_ctrl)
        out |= lx::ui::k_mod_ctrl;
    if (m & lx::wayland::mod_alt)
        out |= lx::ui::k_mod_alt;
    if (m & lx::wayland::mod_super)
        out |= lx::ui::k_mod_super;
    return out;
}

/// The first code point of UTF-8 `s` (0 for none or a malformed lead).
[[nodiscard]] char32_t first_codepoint(const char* s, unsigned n) {
    if (n == 0)
        return 0;
    const auto b = [&](unsigned i) { return static_cast<std::uint8_t>(s[i]); };
    const std::uint8_t c = b(0);
    if (c < 0x80)
        return c;
    if ((c & 0xe0) == 0xc0 && n >= 2)
        return (char32_t{c & 0x1fu} << 6) | (b(1) & 0x3fu);
    if ((c & 0xf0) == 0xe0 && n >= 3)
        return (char32_t{c & 0x0fu} << 12) | (char32_t{b(1) & 0x3fu} << 6) | (b(2) & 0x3fu);
    if ((c & 0xf8) == 0xf0 && n >= 4)
        return (char32_t{c & 0x07u} << 18) | (char32_t{b(1) & 0x3fu} << 12) |
               (char32_t{b(2) & 0x3fu} << 6) | (b(3) & 0x3fu);
    return 0;
}

/// What the key types: xkb's text when it is a character (it follows the layout, the level
/// and the lock state); under ctrl that text is a control code, so the keysym's character
/// instead (ctrl-c reads 'c'), which is what lx.ui's shortcuts look at.
[[nodiscard]] char32_t key_codepoint(const lx::wayland::key_event& e) {
    char32_t cp = first_codepoint(e.utf8, e.utf8_len);
    if (cp >= 0x20 && cp != 0x7f)
        return cp;
#if defined(LUMEN_HAS_XKBCOMMON)
    cp = xkb_keysym_to_utf32(e.keysym);
#else
    // Latin-1 keysyms are their code points, Unicode keysyms carry theirs.
    if ((e.keysym >= 0x20 && e.keysym <= 0x7e) || (e.keysym >= 0xa0 && e.keysym <= 0xff))
        cp = e.keysym;
    else if ((e.keysym & 0xff000000u) == 0x01000000u)
        cp = e.keysym & 0x00ffffffu;
    else
        cp = 0;
#endif
    return cp >= 0x20 && cp != 0x7f ? cp : 0;
}

[[nodiscard]] lx::wayland::cursor_shape wayland_shape(lx::ui::cursor_style c) {
    using lx::ui::cursor_style;
    using lx::wayland::cursor_shape;
    switch (c) {
    case cursor_style::pointer: return cursor_shape::pointer;
    case cursor_style::text: return cursor_shape::text;
    case cursor_style::move: return cursor_shape::move;
    case cursor_style::grab: return cursor_shape::grab;
    case cursor_style::grabbing: return cursor_shape::grabbing;
    case cursor_style::not_allowed: return cursor_shape::not_allowed;
    case cursor_style::ew_resize: return cursor_shape::ew_resize;
    case cursor_style::ns_resize: return cursor_shape::ns_resize;
    case cursor_style::crosshair: return cursor_shape::crosshair;
    case cursor_style::automatic:
    case cursor_style::arrow: break;
    }
    return cursor_shape::default_;
}

[[nodiscard]] lx::point2i surface_point(double x, double y) {
    return {static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y))};
}

} // namespace

input_router::~input_router() { unbind(); }

std::uint64_t input_router::now_ns() const {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

void input_router::bind(lx::wayland::display& display, lx::wayland::surface& surface,
                        lx::ui::ui_root& root) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    display_ = &display;
    surface_ = &surface;
    root_ = &root;
    seat_ = display.default_seat();
    if (seat_) {
        repeat_rate_ = seat_->repeat_rate();
        repeat_delay_ms_ = seat_->repeat_delay_ms();
    }
    if (seat_ && display.has(lx::wayland::global_kind::text_input)) {
        if (auto made = text_input_.create(display, *seat_); made)
            text_input_.set_handler(this);
        else
            log_input.warn("zwp_text_input_v3 unavailable: {}", lx::format_error(made.get_error()));
    }
}

void input_router::unbind() {
    if (saved_focus_)
        saved_focus_ = nullptr;
    text_input_.destroy();
    ime_enabled_ = false;
    ime_focus_ = false;
    ime_target_ = nullptr;
    root_ = nullptr;
    surface_ = nullptr;
    seat_ = nullptr;
    display_ = nullptr;
    repeat_next_ns_ = 0;
    repeat_key_ = 0;
}

void input_router::node_destroyed(lx::ui::ui_node& node) {
    if (saved_focus_ == &node)
        saved_focus_ = nullptr;
}

// ── pointer ──

void input_router::on_pointer(lx::wayland::seat& seat, const lx::wayland::pointer_frame& f) {
    (void)seat;
    if (!root_)
        return;
    ++stats_.pointer_frames;
    using pf = lx::wayland::pointer_frame;
    if (f.mask & pf::has_leave && !(f.mask & pf::has_enter)) {
        // Gone: what the pointer hovered is no longer hovered; a motion still waiting with it.
        motion_pending_ = false;
        inside_ = false;
        root_->pointer_leave();
        return;
    }
    if (f.mask & pf::has_enter) {
        inside_ = true;
        // A new enter serial: the shape must be sent again (the compositor keeps none).
        cursor_sent_ = false;
    }
    if (f.mask & (pf::has_enter | pf::has_motion)) {
        ++stats_.motions;
        if (motion_pending_)
            ++stats_.motions_coalesced;
        pending_ = surface_point(f.x, f.y);
        motion_pending_ = true;
    }
    if (f.mask & pf::has_button) {
        // A press lands where the pointer is now, not where the last frame's hit test was.
        flush_motion();
        lx::ui::mouse_button b{};
        bool known = true;
        switch (f.button) {
        case k_btn_left: b = lx::ui::mouse_button::left; break;
        case k_btn_right: b = lx::ui::mouse_button::right; break;
        case k_btn_middle: b = lx::ui::mouse_button::middle; break;
        default: known = false; break;
        }
        if (known) {
            ++stats_.buttons;
            (void)root_->pointer_button(b, f.pressed);
            update_cursor();
        }
    }
    if (f.mask & (pf::has_axis | pf::has_axis_value120)) {
        flush_motion();
        // Detents (value120) scroll by lines, as GTK and Firefox do; continuous sources
        // (touchpads) by the distance they report, already in surface px.
        const float line = lx::ui::ui_rem_px() * 1.25f;
        float d[2]{};
        for (unsigned axis = 0; axis < 2; ++axis) {
            if ((f.mask & pf::has_axis_value120) && f.value120[axis] != 0)
                d[axis] = static_cast<float>(f.value120[axis]) / 120.f * k_wheel_lines * line;
            else
                d[axis] = static_cast<float>(f.axis_value[axis]);
        }
        if (d[0] != 0.f || d[1] != 0.f) {
            ++stats_.axes;
            (void)root_->pointer_axis(d[1], d[0]);
            update_cursor();
        }
    }
}

void input_router::flush_motion() {
    if (!motion_pending_ || !root_)
        return;
    motion_pending_ = false;
    ++stats_.hit_tests;
    (void)root_->pointer_move(pending_);
    update_cursor();
}

void input_router::update_cursor() {
    if (!inside_ || !seat_ || !root_)
        return;
    const lx::ui::cursor_style want = lx::ui::resolve_cursor(root_->pointer().hovered());
    if (cursor_sent_ && want == cursor_)
        return;
    cursor_ = want;
    cursor_sent_ = true;
    if (seat_->set_cursor_shape(wayland_shape(want))) {
        ++stats_.cursor_shapes;
    } else if (!cursor_warned_) {
        cursor_warned_ = true;
        log_input.info("no wp_cursor_shape_v1: the compositor keeps its own cursor over the window");
    }
}

// ── touch: a pointer for v1 ──

void input_router::on_touch(lx::wayland::seat& seat, const lx::wayland::touch_event& e) {
    (void)seat;
    if (!root_)
        return;
    using kind = lx::wayland::touch_event::kind;
    switch (e.type) {
    case kind::down:
        if (touch_id_ >= 0)
            return;
        touch_id_ = e.id;
        ++stats_.touches;
        inside_ = true;
        pending_ = surface_point(e.x, e.y);
        motion_pending_ = true;
        flush_motion();
        (void)root_->pointer_button(lx::ui::mouse_button::left, true);
        return;
    case kind::motion:
        if (e.id != touch_id_)
            return;
        if (motion_pending_)
            ++stats_.motions_coalesced;
        pending_ = surface_point(e.x, e.y);
        motion_pending_ = true;
        return;
    case kind::up:
        if (e.id != touch_id_)
            return;
        flush_motion();
        (void)root_->pointer_button(lx::ui::mouse_button::left, false);
        root_->pointer_leave();
        touch_id_ = -1;
        inside_ = false;
        return;
    case kind::cancel:
        if (touch_id_ < 0)
            return;
        motion_pending_ = false;
        root_->pointer_leave();
        touch_id_ = -1;
        inside_ = false;
        return;
    default:
        return;
    }
}

// ── keyboard ──

void input_router::on_keyboard_focus(lx::wayland::seat& seat, bool entered) {
    (void)seat;
    if (!root_)
        return;
    lx::ui::focus_manager& focus = root_->focus();
    if (!entered) {
        // The window lost the keyboard: no repeat survives it, and the focused node blurs
        // (its caret stops blinking, its ring goes) — kept to give it back on enter, as every
        // desktop toolkit restores a window's focus.
        repeat_next_ns_ = 0;
        repeat_key_ = 0;
        if (lx::ui::ui_node* n = focus.focused()) {
            saved_focus_ = n;
            saved_ring_ = focus.focus_visible();
            n->mark_referenced();
            focus.blur();
        }
        return;
    }
    if (saved_focus_) {
        lx::ui::ui_node* n = saved_focus_;
        saved_focus_ = nullptr;
        (void)focus.focus(*n, saved_ring_ ? lx::ui::focus_cause::keyboard
                                          : lx::ui::focus_cause::programmatic);
    }
}

void input_router::deliver_key(const lx::wayland::key_event& e, bool pressed) {
    lx::ui::key_event k{};
    k.key = ui_key(e.keysym);
    k.keycode = e.key + 8; // xkb keycode
    k.pressed = pressed;
    k.modifiers = ui_modifiers(e.modifiers);
    k.codepoint = pressed ? key_codepoint(e) : 0;
    (void)root_->key(k);
}

void input_router::on_key(lx::wayland::seat& seat, const lx::wayland::key_event& e) {
    (void)seat;
    if (!root_)
        return;
    // A key acts where the pointer is now (a shift-click after a move, a scroll by keys).
    flush_motion();
    using lx::wayland::key_state;
    if (e.state == key_state::released) {
        ++stats_.keys;
        if (e.key == repeat_key_) {
            repeat_key_ = 0;
            repeat_next_ns_ = 0;
        }
        deliver_key(e, false);
        return;
    }
    if (e.state == key_state::repeated) {
        // wl_keyboard v10: the compositor repeats; the client must not.
        compositor_repeats_ = true;
        ++stats_.repeats;
        deliver_key(e, true);
        return;
    }
    ++stats_.keys;
    // The user's own edit: whatever a stale done held back goes out with it.
    ime_deferred_ = false;
    deliver_key(e, true);
    if (e.repeats && repeat_rate_ > 0 && !compositor_repeats_) {
        repeat_key_ = e.key;
        repeat_event_ = e;
        repeat_next_ns_ = now_ns() + static_cast<std::uint64_t>(std::max(repeat_delay_ms_, 0)) * 1'000'000ull;
    } else if (repeat_key_ != 0) {
        // Another key pressed: the held one stops repeating (every toolkit does this).
        repeat_key_ = 0;
        repeat_next_ns_ = 0;
    }
}

void input_router::on_repeat_due(std::uint64_t now) {
    if (!root_ || repeat_key_ == 0 || repeat_next_ns_ == 0 || now < repeat_next_ns_)
        return;
    ++stats_.repeats;
    deliver_key(repeat_event_, true);
    const std::uint64_t period = 1'000'000'000ull / static_cast<std::uint64_t>(repeat_rate_);
    // From the deadline, so the rate holds; a late wake delivers one repeat, not a burst.
    repeat_next_ns_ += period;
    if (repeat_next_ns_ <= now)
        repeat_next_ns_ = now + period;
}

void input_router::on_modifiers(lx::wayland::seat& seat, std::uint32_t modifiers) {
    (void)seat;
    modifiers_ = ui_modifiers(modifiers);
    if (root_)
        root_->pointer().set_modifiers(modifiers_);
}

void input_router::on_repeat_info(lx::wayland::seat& seat, std::int32_t rate, std::int32_t delay_ms) {
    (void)seat;
    repeat_rate_ = rate;
    repeat_delay_ms_ = delay_ms;
    if (rate <= 0) {
        repeat_key_ = 0;
        repeat_next_ns_ = 0;
    }
}

// ── text input ──

void input_router::on_focus(lx::wayland::text_input& t, lx::wayland::surface* target, bool entered) {
    (void)t;
    if (target != surface_)
        return;
    ime_focus_ = entered;
    if (!entered) {
        // The compositor took the text input away: enabled state is reset by the protocol.
        if (ime_enabled_) {
            text_input_.disable();
            text_input_.commit();
            ++stats_.ime_commits;
        }
        ime_enabled_ = false;
        // Only through the root: the field the session began with may be gone.
        if (lx::ui::ime_target* live = root_ ? root_->ime() : nullptr; live && live == ime_target_)
            live->ime_reset();
        ime_target_ = nullptr;
        return;
    }
    sync_text_input(true);
}

void input_router::on_done(lx::wayland::text_input& t, const lx::wayland::text_input_done& d) {
    ++stats_.ime_dones;
    log_input.debug("text-input done serial={} commits={} current={} preedit={} commit={}", d.serial,
                    t.commit_count(), d.current, d.has_preedit, d.has_commit);
    if (!root_)
        return;
    lx::ui::ime_target* target = root_->ime();
    if (!target || target != ime_target_)
        return;
    lx::ui::ime_update u{};
    if (d.has_preedit) {
        u.preedit = d.preedit;
        u.preedit_cursor_begin = d.preedit_cursor_begin;
        u.preedit_cursor_end = d.preedit_cursor_end;
    }
    if (d.has_commit)
        u.commit = d.commit;
    if (d.has_delete) {
        u.delete_before = d.delete_before;
        u.delete_after = d.delete_after;
    }
    target->ime_apply(u);
    ++stats_.ime_applied;
    if (d.current) {
        // The input method saw our latest state: tell it what its text did (and anything held
        // back since a stale done).
        ime_deferred_ = false;
        change_cause_ = 0;
        sync_text_input(false);
        change_cause_ = 1;
        return;
    }
    // Not current (a commit of ours crossed the input method's): applied, but the state it
    // produced is held until a done with a matching serial (the protocol's rule) — or until
    // the user's own next key, an edit of the client's that goes out regardless.
    ++stats_.ime_stale;
    ime_deferred_ = true;
}

void input_router::after_frame() {
    if (!ime_deferred_)
        sync_text_input(false);
}

void input_router::sync_text_input(bool force) {
    if (!text_input_.valid() || !root_)
        return;
    lx::ui::ime_target* target = ime_focus_ ? root_->ime() : nullptr;
    if (target != ime_target_) {
        // The focused field changed: the old one's session ends, a new one starts.
        if (ime_enabled_) {
            text_input_.disable();
            text_input_.commit();
            ++stats_.ime_commits;
            ime_enabled_ = false;
        }
        ime_target_ = target;
        force = true;
    }
    if (!target)
        return;
    const lx::ui::ime_state q = target->ime_query();
    const std::size_t n = std::min<std::size_t>(q.surrounding.size(), sizeof(sent_text_));
    const bool changed = force || !ime_enabled_ || n != sent_size_ || q.cursor != sent_cursor_ ||
                         q.anchor != sent_anchor_ || q.cursor_rect.x != sent_rect_.x ||
                         q.cursor_rect.y != sent_rect_.y || q.cursor_rect.width != sent_rect_.width ||
                         q.cursor_rect.height != sent_rect_.height ||
                         std::memcmp(sent_text_, q.surrounding.data(), n) != 0;
    if (!changed)
        return;
    if (!ime_enabled_) {
        text_input_.enable();
        text_input_.set_content_type(0, 0); // hint none, purpose normal
        ime_enabled_ = true;
    }
    text_input_.set_surrounding_text(q.surrounding, static_cast<std::int32_t>(q.cursor),
                                     static_cast<std::int32_t>(q.anchor));
    text_input_.set_text_change_cause(change_cause_);
    text_input_.set_cursor_rectangle(q.cursor_rect);
    text_input_.commit();
    ++stats_.ime_commits;
    std::memcpy(sent_text_, q.surrounding.data(), n);
    sent_size_ = static_cast<std::uint32_t>(n);
    sent_cursor_ = q.cursor;
    sent_anchor_ = q.anchor;
    sent_rect_ = q.cursor_rect;
}

} // namespace lx::app
