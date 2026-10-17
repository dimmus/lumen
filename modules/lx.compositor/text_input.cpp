module;

// The text-input ↔ input-method relay (`:text_input`), defined here rather than in the
// partition: the partition's interface then carries declarations only (see session_lock.cpp
// for the Clang 23 defect that makes this the rule for partitions holding foundation value
// types in compiled bodies). The rules are in text_input.cppm.

#include <cstring>

module lx.compositor;

import lx.foundation;
import :positioner;
import :text_input;

namespace lx::compositor {

namespace {

void reset_state(text_input_state& s) {
    s.enabled = false;
    s.has_surrounding = false;
    s.cursor = 0;
    s.anchor = 0;
    s.change_cause = 0;
    s.has_content_type = false;
    s.hint = 0;
    s.purpose = 0;
    s.has_cursor_rect = false;
    s.cursor_rect = {};
    s.actions = 0;
    s.surrounding_len = 0;
    s.surrounding[0] = '\0';
}

/// pending → current: the scalars and only the text bytes in use (not the 4 KiB buffer).
void copy_state(text_input_state& dst, const text_input_state& src) {
    dst.enabled = src.enabled;
    dst.has_surrounding = src.has_surrounding;
    dst.cursor = src.cursor;
    dst.anchor = src.anchor;
    dst.change_cause = src.change_cause;
    dst.has_content_type = src.has_content_type;
    dst.hint = src.hint;
    dst.purpose = src.purpose;
    dst.has_cursor_rect = src.has_cursor_rect;
    dst.cursor_rect = src.cursor_rect;
    dst.actions = src.actions;
    dst.surrounding_len = src.surrounding_len;
    std::memcpy(dst.surrounding, src.surrounding, src.surrounding_len);
    dst.surrounding[src.surrounding_len] = '\0';
}

void reset_im_pending(input_method_record& im) {
    im.preedit.set = false;
    im.preedit.len = 0;
    im.preedit.text[0] = '\0';
    im.preedit_begin = 0;
    im.preedit_end = 0;
    im.commit.set = false;
    im.commit.len = 0;
    im.commit.text[0] = '\0';
    im.has_delete = false;
    im.delete_before = 0;
    im.delete_after = 0;
}

void set_string(ime_string& out, const char* text, unsigned len) {
    const unsigned n = text ? utf8_truncate(text, len, k_ime_text_max) : 0;
    if (n)
        std::memcpy(out.text, text, n);
    out.text[n] = '\0';
    out.len = n;
    out.set = true;
}

[[nodiscard]] bool same_rect(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

template<class T>
void link_front(T*& head, T& item) {
    item.prev = nullptr;
    item.next = head;
    if (head)
        head->prev = &item;
    head = &item;
    item.linked = true;
}

template<class T>
void unlink(T*& head, T& item) {
    if (item.prev)
        item.prev->next = item.next;
    else
        head = item.next;
    if (item.next)
        item.next->prev = item.prev;
    item.prev = nullptr;
    item.next = nullptr;
    item.linked = false;
}

} // namespace

unsigned utf8_truncate(const char* text, unsigned len, unsigned max) {
    if (len <= max)
        return len;
    unsigned n = max;
    // text[n] is the first byte left out: while it continues a sequence, the sequence it
    // belongs to started inside the prefix and is cut — drop it whole.
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0u) == 0x80u)
        --n;
    return n;
}

input_popup_placement place_input_popup(lx::rect2i anchor, lx::size2i size, lx::rect2i bounds) {
    positioner_rules r{};
    r.size = size;
    r.size_set = true;
    r.anchor_rect = anchor;
    r.anchor_rect_set = true;
    r.anchor = 6;  // bottom_left: below the cursor, starting at its left edge
    r.gravity = 8; // bottom_right: the popup grows right and down from there
    r.adjustment = k_adjust_flip_x | k_adjust_flip_y | k_adjust_slide_x | k_adjust_slide_y;
    const positioner_result placed = positioner_place(r, bounds);
    input_popup_placement out{};
    out.origin = {placed.rect.x, placed.rect.y};
    out.text_rect = {anchor.x - placed.rect.x, anchor.y - placed.rect.y, anchor.width,
                     anchor.height};
    return out;
}

void text_input_relay::set_sink(const text_input_sink& sink) { sink_ = sink; }

// ── keyboard focus ──────────────────────────────────────────────────────────

void text_input_relay::send_enter(text_input_record& ti, void* surface) {
    ti.focus = surface;
    ti.toggled = false;
    ti.rect_staged = false;
    reset_state(ti.pending);
    reset_state(ti.current);
    if (sink_.enter)
        sink_.enter(sink_.user, ti.resource, surface);
}

void text_input_relay::send_leave(text_input_record& ti) {
    void* surface = ti.focus;
    ti.focus = nullptr;
    ti.toggled = false;
    ti.rect_staged = false;
    reset_state(ti.pending);
    reset_state(ti.current);
    if (sink_.leave && surface)
        sink_.leave(sink_.user, ti.resource, surface);
}

void text_input_relay::keyboard_focus(void* surface, void* client) {
    focus_ = surface;
    focus_client_ = surface ? client : nullptr;
    for (text_input_record* ti = tis_; ti; ti = ti->next)
        if (ti->focus && ti->focus != surface)
            send_leave(*ti);
    if (im_ && surface)
        for (text_input_record* ti = tis_; ti; ti = ti->next)
            if (ti->client == client && ti->focus != surface)
                send_enter(*ti, surface);
    update_active(nullptr);
}

void text_input_relay::focus_destroyed(void* surface) {
    if (!surface)
        return;
    for (text_input_record* ti = tis_; ti; ti = ti->next) {
        if (ti->focus != surface)
            continue;
        // No `leave`: the surface it would name is gone.
        ti->focus = nullptr;
        ti->toggled = false;
        ti->rect_staged = false;
        reset_state(ti->pending);
        reset_state(ti->current);
    }
    if (focus_ == surface) {
        focus_ = nullptr;
        focus_client_ = nullptr;
    }
    update_active(nullptr);
}

// ── zwp_text_input_v3 ───────────────────────────────────────────────────────

void text_input_relay::add(text_input_record& ti) {
    if (ti.linked)
        return;
    link_front(tis_, ti);
    ++ti_count_;
    if (im_ && focus_ && ti.client == focus_client_)
        send_enter(ti, focus_);
}

void text_input_relay::remove(text_input_record& ti) {
    if (!ti.linked)
        return;
    unlink(tis_, ti);
    --ti_count_;
    ti.focus = nullptr;
    ti.current.enabled = false;
    if (active_ == &ti)
        update_active(nullptr);
}

void text_input_relay::enable(text_input_record& ti) {
    if (!ti.focus)
        return; // after leave, requests are ignored until the next enter
    // "This request resets all state associated with previous enable, disable,
    // set_surrounding_text, set_text_change_cause, set_content_type, and set_cursor_rectangle".
    reset_state(ti.pending);
    ti.pending.enabled = true;
    ti.toggled = true;
}

void text_input_relay::disable(text_input_record& ti) {
    if (!ti.focus)
        return;
    // "After an enter event or disable request all state information is invalidated".
    reset_state(ti.pending);
    ti.toggled = true;
}

void text_input_relay::set_surrounding(text_input_record& ti, const char* text, unsigned len,
                                       int cursor, int anchor) {
    if (!ti.focus)
        return;
    const unsigned n = text ? utf8_truncate(text, len, k_ime_text_max) : 0;
    if (n)
        std::memcpy(ti.pending.surrounding, text, n);
    ti.pending.surrounding[n] = '\0';
    ti.pending.surrounding_len = n;
    const auto clamp = [n](int v) {
        return v < 0 ? 0u : static_cast<unsigned>(v) > n ? n : static_cast<unsigned>(v);
    };
    ti.pending.cursor = clamp(cursor);
    ti.pending.anchor = clamp(anchor);
    ti.pending.has_surrounding = true;
}

void text_input_relay::set_change_cause(text_input_record& ti, unsigned cause) {
    if (ti.focus)
        ti.pending.change_cause = cause;
}

void text_input_relay::set_content_type(text_input_record& ti, unsigned hint, unsigned purpose) {
    if (!ti.focus)
        return;
    ti.pending.has_content_type = true;
    ti.pending.hint = hint;
    ti.pending.purpose = purpose;
}

void text_input_relay::set_cursor_rect(text_input_record& ti, lx::rect2i rect) {
    if (!ti.focus)
        return;
    ti.pending.has_cursor_rect = true;
    ti.pending.cursor_rect = rect;
}

void text_input_relay::set_actions(text_input_record& ti, unsigned actions) {
    if (ti.focus)
        ti.pending.actions = actions;
}

bool text_input_relay::other_enabled(const text_input_record& ti) const {
    for (const text_input_record* o = tis_; o; o = o->next)
        if (o != &ti && o->current.enabled)
            return true;
    return false;
}

void text_input_relay::commit(text_input_record& ti) {
    // Counted whatever happens next: `done` carries the client's own count.
    ++ti.commits;
    const bool toggled = ti.toggled;
    ti.toggled = false;
    if (!ti.focus)
        return; // ignored until the next enter
    const bool was = ti.current.enabled;
    // "Requests to enable a text input when another text input is enabled on the same seat
    // must be ignored by compositor."
    if (ti.pending.enabled && !was && other_enabled(ti))
        ti.pending.enabled = false;
    const bool applied_has = ti.current.has_cursor_rect;
    const lx::rect2i applied = ti.current.cursor_rect;
    copy_state(ti.current, ti.pending);
    // "It must be applied and reset to initial at the next commit."
    ti.pending.change_cause = 0;
    bool rect_changed = false;
    if (ti.version >= 2) {
        // Stage 1 of 2: the rectangle waits for the focused surface's next commit; the one
        // applied stays — unless this commit carried an enable or disable, which drops both.
        bool keep_has = applied_has;
        lx::rect2i keep = applied;
        if (toggled) {
            keep_has = false;
            keep = {};
            ti.rect_staged = false;
        }
        ti.current.has_cursor_rect = keep_has;
        ti.current.cursor_rect = keep;
        if (ti.pending.has_cursor_rect) {
            ti.rect_staged = true;
            ti.staged_has_rect = true;
            ti.staged_rect = ti.pending.cursor_rect;
        }
        rect_changed = keep_has != applied_has || !same_rect(keep, applied);
    } else {
        rect_changed = ti.current.has_cursor_rect != applied_has ||
                       !same_rect(ti.current.cursor_rect, applied);
    }
    const bool reenabled = toggled && was && ti.current.enabled;
    if (active_ == &ti && ti.current.enabled && !reenabled) {
        // A state update of the active text input: the state again, then done.
        if (im_ && im_->active) {
            send_state(ti);
            im_done();
        }
        if (rect_changed)
            update_popups();
        return;
    }
    update_active(reenabled ? &ti : nullptr);
}

void text_input_relay::surface_committed(void* surface) {
    if (!tis_ || !surface)
        return;
    for (text_input_record* ti = tis_; ti; ti = ti->next) {
        if (ti->focus != surface || !ti->rect_staged)
            continue;
        ti->rect_staged = false;
        const bool changed = ti->current.has_cursor_rect != ti->staged_has_rect ||
                             !same_rect(ti->current.cursor_rect, ti->staged_rect);
        ti->current.has_cursor_rect = ti->staged_has_rect;
        ti->current.cursor_rect = ti->staged_rect;
        if (changed && ti == active_)
            update_popups();
    }
}

void text_input_relay::surface_moved(void* surface) {
    if (popups_ && active_ && active_->focus == surface)
        update_popups();
}

void text_input_relay::outputs_changed() {
    if (popups_)
        update_popups();
}

// ── the input method ────────────────────────────────────────────────────────

void text_input_relay::send_state(const text_input_record& ti) {
    if (!im_)
        return;
    const text_input_state& c = ti.current;
    // Surrounding text and the cause reset to their initial values at every IM done, so they
    // go out with every done (sway); the content type stays, but costs nothing to repeat.
    if (c.has_surrounding && sink_.im_surrounding)
        sink_.im_surrounding(sink_.user, im_->resource, c.surrounding, c.cursor, c.anchor);
    if (sink_.im_change_cause)
        sink_.im_change_cause(sink_.user, im_->resource, c.change_cause);
    if (c.has_content_type && sink_.im_content_type)
        sink_.im_content_type(sink_.user, im_->resource, c.hint, c.purpose);
}

void text_input_relay::im_done() {
    if (!im_)
        return;
    if (sink_.im_done)
        sink_.im_done(sink_.user, im_->resource);
    ++im_->dones;
}

void text_input_relay::set_routing(bool on) {
    if (routing_ == on)
        return;
    routing_ = on;
    if (sink_.routing)
        sink_.routing(sink_.user, on);
}

void text_input_relay::update_active(text_input_record* reenabled) {
    text_input_record* cand = nullptr;
    if (im_ && focus_)
        for (text_input_record* ti = tis_; ti; ti = ti->next)
            if (ti->current.enabled && ti->focus == focus_) {
                cand = ti;
                break;
            }
    if (cand == active_ && !reenabled)
        return;
    if (im_ && im_->active) {
        if (sink_.im_deactivate)
            sink_.im_deactivate(sink_.user, im_->resource);
        im_->active = false;
        // A switch straight to another text input (or the same one, enabled again) is one
        // done: the input method sees its context end and a new one begin.
        if (!cand)
            im_done();
    }
    active_ = cand;
    if (active_ && im_) {
        if (sink_.im_activate)
            sink_.im_activate(sink_.user, im_->resource);
        im_->active = true;
        send_state(*active_);
        im_done();
    }
    set_routing(im_ && im_->active);
    update_popups();
}

bool text_input_relay::add_input_method(input_method_record& im) {
    if (im_) {
        im.live = false;
        return false;
    }
    im_ = &im;
    im.live = true;
    im.active = false;
    im.dones = 0;
    reset_im_pending(im);
    // Text-input focus exists from now on: the focused client's text inputs are entered.
    if (focus_)
        for (text_input_record* ti = tis_; ti; ti = ti->next)
            if (ti->client == focus_client_ && ti->focus != focus_)
                send_enter(*ti, focus_);
    update_active(nullptr);
    return true;
}

void text_input_relay::remove_input_method(input_method_record& im) {
    const bool was_live = im.live;
    im.live = false;
    im.active = false;
    if (&im != im_ || !was_live)
        return;
    for (input_popup_record* p = popups_; p; p = p->next)
        if (p->im == &im) {
            hide_popup(*p);
            p->im = nullptr;
        }
    im_ = nullptr;
    // No input method: no text-input focus. `leave` also tells the client to drop its
    // preedit; the surface stays at hand for an input method that comes back.
    for (text_input_record* ti = tis_; ti; ti = ti->next)
        if (ti->focus)
            send_leave(*ti);
    active_ = nullptr;
    set_routing(false);
}

void text_input_relay::im_commit_string(input_method_record& im, const char* text, unsigned len) {
    if (im.live)
        set_string(im.commit, text, len);
}

void text_input_relay::im_preedit(input_method_record& im, const char* text, unsigned len,
                                  int begin, int end) {
    if (!im.live)
        return;
    set_string(im.preedit, text, len);
    im.preedit_begin = begin;
    im.preedit_end = end;
}

void text_input_relay::im_delete_surrounding(input_method_record& im, unsigned before,
                                             unsigned after) {
    if (!im.live)
        return;
    im.has_delete = true;
    im.delete_before = before;
    im.delete_after = after;
}

void text_input_relay::im_commit(input_method_record& im, unsigned serial) {
    if (!im.live)
        return;
    // "When the compositor receives a commit request with a serial different than the number
    // of past done events, it must proceed as normal, except it should not change the current
    // state" — its pending state is dropped (wlroots).
    if (serial != im.dones) {
        reset_im_pending(im);
        return;
    }
    text_input_record* ti = (&im == im_ && im.active) ? active_ : nullptr;
    if (ti) {
        if (im.preedit.set && sink_.preedit)
            sink_.preedit(sink_.user, ti->resource, im.preedit.text, im.preedit_begin,
                          im.preedit_end);
        if (im.commit.set && sink_.commit_string)
            sink_.commit_string(sink_.user, ti->resource, im.commit.text);
        if (im.has_delete && sink_.delete_surrounding)
            sink_.delete_surrounding(sink_.user, ti->resource, im.delete_before, im.delete_after);
        if (sink_.done)
            sink_.done(sink_.user, ti->resource, ti->commits);
    }
    reset_im_pending(im);
}

// ── popups ──────────────────────────────────────────────────────────────────

void text_input_relay::add_popup(input_popup_record& popup, input_method_record& im) {
    if (popup.linked)
        return;
    link_front(popups_, popup);
    ++popup_count_;
    popup.im = (im.live && &im == im_) ? &im : nullptr;
    popup.shown = false;
    popup.rect_sent = false;
}

void text_input_relay::remove_popup(input_popup_record& popup) {
    if (!popup.linked)
        return;
    hide_popup(popup);
    unlink(popups_, popup);
    --popup_count_;
    popup.im = nullptr;
}

void text_input_relay::popup_committed(input_popup_record& popup, bool has_content,
                                       lx::size2i size) {
    popup.has_content = has_content;
    popup.size = size;
    if (popup.linked)
        update_popups();
}

void text_input_relay::hide_popup(input_popup_record& p) {
    if (!p.shown)
        return;
    p.shown = false;
    p.rect_sent = false;
    if (sink_.popup_hide)
        sink_.popup_hide(sink_.user, p.resource, p.surface);
}

void text_input_relay::update_popups() {
    if (!popups_)
        return;
    bool show = im_ && im_->active && active_ && active_->focus;
    lx::rect2i srect{};
    lx::rect2i bounds{};
    if (show)
        show = sink_.surface_rect && sink_.surface_rect(sink_.user, active_->focus, srect, bounds);
    lx::rect2i anchor{};
    bool has_rect = false;
    // A v2 text input whose first rectangle is staged has its popup wait for the surface
    // commit that applies it: shown below the whole surface for one frame, it would jump.
    if (show && !active_->current.has_cursor_rect && active_->rect_staged &&
        active_->staged_has_rect)
        show = false;
    if (show) {
        const text_input_state& c = active_->current;
        has_rect = c.has_cursor_rect;
        // Without a cursor rectangle, the whole surface (sway): the popup goes below it.
        anchor = has_rect ? lx::rect2i{srect.x + c.cursor_rect.x, srect.y + c.cursor_rect.y,
                                       c.cursor_rect.width, c.cursor_rect.height}
                          : srect;
    }
    for (input_popup_record* p = popups_; p; p = p->next) {
        const bool want = show && p->im && p->im == im_ && p->has_content &&
                          p->size.width > 0 && p->size.height > 0;
        if (!want) {
            hide_popup(*p);
            continue;
        }
        const input_popup_placement at = place_input_popup(anchor, p->size, bounds);
        if (!p->shown || p->origin.x != at.origin.x || p->origin.y != at.origin.y) {
            p->shown = true;
            p->origin = at.origin;
            if (sink_.popup_show)
                sink_.popup_show(sink_.user, p->resource, p->surface, at.origin);
        }
        if (has_rect && (!p->rect_sent || !same_rect(p->sent_rect, at.text_rect))) {
            p->rect_sent = true;
            p->sent_rect = at.text_rect;
            if (sink_.popup_rect)
                sink_.popup_rect(sink_.user, p->resource, at.text_rect);
        }
    }
}

} // namespace lx::compositor
