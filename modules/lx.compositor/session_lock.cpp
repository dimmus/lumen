module;

// ext-session-lock-v1's state machine (`:session_lock`), defined here rather than in the
// partition: the partition's interface then carries declarations only. Compiled into the
// partition, these bodies (their copies of foundation value types) made Clang 23 merge the
// implicit assignment operators of those types badly in every unit importing the module —
// `surface_pending`'s became invalid in wire_surface.cpp ("cannot compile this l-value
// expression yet"). Recorded in docs/architecture.md, "Session lock".

module lx.compositor;

import lx.foundation;
import :session_lock;

namespace lx::compositor {

lock_actions session_lock::set_outputs(unsigned mask) {
    lock_actions a{};
    outputs_ = mask;
    // An output that went may have been the last one without content.
    if (state_ == lock_state::awaiting_surfaces)
        maybe_complete(a);
    return a;
}

lock_actions session_lock::request(void* lock_res, clock_ns now) {
    lock_actions a{};
    if (!lock_res)
        return a;
    switch (state_) {
    case lock_state::unlocked:
        lock_ = lock_res;
        state_ = lock_state::awaiting_surfaces;
        deadline_ = now + k_surface_wait_ns;
        a.bits |= lock_actions::input_lock | lock_actions::notify_shell | lock_actions::rearm_timer;
        // No output: nothing to wait for (and nothing to show).
        maybe_complete(a);
        return a;
    case lock_state::abandoned:
        // A new locker takes over a lock whose client died. Nothing of the session is on
        // screen (the solid colour is), so it is locked at once.
        lock_ = lock_res;
        state_ = lock_state::locked;
        a.bits |= lock_actions::send_locked | lock_actions::notify_shell;
        a.lock = lock_res;
        return a;
    case lock_state::awaiting_surfaces:
    case lock_state::locking:
    case lock_state::locked:
        // Another locker holds it: "finished ... because there is already another
        // ext_session_lock_v1 object held by a client".
        a.bits |= lock_actions::send_finished;
        a.finished = lock_res;
        return a;
    }
    return a;
}

lock_actions session_lock::unlock(void* lock_res) {
    lock_actions a{};
    if (!lock_res || lock_res != lock_ || state_ != lock_state::locked)
        return a;
    to_unlocked(a, true);
    a.bits |= lock_actions::activity;
    return a;
}

lock_actions session_lock::released(void* lock_res, bool client_gone) {
    lock_actions a{};
    if (!lock_res || lock_res != lock_)
        return a; // finished earlier, or never in charge
    lock_ = nullptr;
    switch (state_) {
    case lock_state::unlocked:
    case lock_state::abandoned:
        return a;
    case lock_state::awaiting_surfaces:
    case lock_state::locking:
        if (!client_gone) {
            // The locker gave up before locking (the destroy request is allowed then).
            to_unlocked(a, state_ == lock_state::locking);
            return a;
        }
        [[fallthrough]];
    case lock_state::locked:
        // The locker died: the session stays locked (the spec forbids unlocking), on the
        // solid colour. A destroy request after `locked` never gets here — it is
        // invalid_destroy, and the client dies of it.
        if (!scene_locked())
            a.bits |= lock_actions::scene_lock; // died before the scene locked: lock it now
        if (!hint_) {
            hint_ = true;
            a.bits |= lock_actions::locked_hint;
            a.hint = true;
        }
        state_ = lock_state::abandoned;
        deadline_ = 0;
        a.bits |= lock_actions::notify_shell | lock_actions::drop_surfaces |
                  lock_actions::refocus | lock_actions::rearm_timer;
        keyboard_ = 0;
        return a;
    }
    return a;
}

lock_actions session_lock::force_unlock() {
    lock_actions a{};
    if (state_ == lock_state::unlocked)
        return a;
    if (lock_) {
        a.bits |= lock_actions::send_finished;
        a.finished = lock_;
    }
    to_unlocked(a, scene_locked());
    a.bits |= lock_actions::activity;
    return a;
}

lock_actions session_lock::on_timer(clock_ns now) {
    lock_actions a{};
    if (state_ != lock_state::awaiting_surfaces || deadline_ == 0 || now < deadline_)
        return a;
    deadline_ = 0;
    a.bits |= lock_actions::rearm_timer;
    enter_locking(a);
    return a;
}

lock_actions session_lock::locked_frame_shown() {
    lock_actions a{};
    if (state_ != lock_state::locking)
        return a;
    state_ = lock_state::locked;
    a.bits |= lock_actions::send_locked | lock_actions::notify_shell;
    a.lock = lock_;
    if (!hint_) {
        hint_ = true;
        a.bits |= lock_actions::locked_hint;
        a.hint = true;
    }
    return a;
}

unsigned session_lock::add_surface(void* resource, void* surface_res, lx::surface_id surface,
                                   unsigned output) {
    for (auto& r : surfaces_) {
        if (r.id)
            continue;
        r = {};
        r.id = next_id_++;
        if (next_id_ == 0)
            next_id_ = 1;
        r.resource = resource;
        r.surface_res = surface_res;
        r.surface = surface;
        r.output = output;
        return r.id;
    }
    return 0;
}

lock_surface_record* session_lock::find_mut(unsigned id) {
    if (!id)
        return nullptr;
    for (auto& r : surfaces_)
        if (r.id == id)
            return &r;
    return nullptr;
}

const lock_surface_record* session_lock::find(unsigned id) const {
    return const_cast<session_lock*>(this)->find_mut(id);
}

const lock_surface_record* session_lock::find_surface(lx::surface_id surface) const {
    if (!surface)
        return nullptr;
    for (const auto& r : surfaces_)
        if (r.id && r.surface == surface)
            return &r;
    return nullptr;
}

void session_lock::note_configure(unsigned id, unsigned serial, lx::size2i size) {
    lock_surface_record* r = find_mut(id);
    if (!r)
        return;
    if (r->pending == lock_surface_record::k_max_configures) {
        for (unsigned i = 1; i < r->pending; ++i) {
            r->serials[i - 1] = r->serials[i];
            r->sizes[i - 1] = r->sizes[i];
        }
        --r->pending;
    }
    r->serials[r->pending] = serial;
    r->sizes[r->pending] = size;
    ++r->pending;
}

session_lock::ack_result session_lock::ack(unsigned id, unsigned serial) {
    lock_surface_record* r = find_mut(id);
    if (!r)
        return ack_result::ok; // inert: nothing to check against
    for (unsigned i = 0; i < r->pending; ++i) {
        if (r->serials[i] != serial)
            continue;
        r->acked_size = r->sizes[i];
        r->ever_acked = true;
        // This configure and every older one are consumed: acking them again, or acking
        // one older than the last acked, is invalid_serial.
        const unsigned drop = i + 1;
        for (unsigned j = drop; j < r->pending; ++j) {
            r->serials[j - drop] = r->serials[j];
            r->sizes[j - drop] = r->sizes[j];
        }
        r->pending -= drop;
        return ack_result::ok;
    }
    return ack_result::invalid_serial;
}

session_lock::commit_error session_lock::check_commit(unsigned id, bool has_buffer,
                                                      lx::size2i size) const {
    const lock_surface_record* r = find(id);
    if (!r)
        return commit_error::none; // inert
    if (!r->ever_acked)
        return commit_error::before_first_ack;
    if (!has_buffer)
        return commit_error::null_buffer;
    if (size.width != r->acked_size.width || size.height != r->acked_size.height)
        return commit_error::dimensions_mismatch;
    return commit_error::none;
}

lock_actions session_lock::content(unsigned id) {
    lock_actions a{};
    lock_surface_record* r = find_mut(id);
    if (!r || r->has_content)
        return a;
    r->has_content = true;
    r->content_order = next_content_++;
    // "Give the first lock surface created keyboard focus" — the first one that can show
    // something, so a key typed before it maps is not lost to nobody.
    if (!keyboard_) {
        keyboard_ = id;
        a.bits |= lock_actions::refocus;
    }
    if (state_ == lock_state::awaiting_surfaces)
        maybe_complete(a);
    return a;
}

lock_actions session_lock::remove_surface(unsigned id) {
    lock_actions a{};
    lock_surface_record* r = find_mut(id);
    if (!r)
        return a;
    *r = {};
    if (keyboard_ == id) {
        keyboard_ = 0;
        refocus_after_removal(a);
    }
    // The pointer may have been on it.
    a.bits |= lock_actions::refocus;
    return a;
}

lock_actions session_lock::clicked(lx::surface_id surface) {
    lock_actions a{};
    const lock_surface_record* r = find_surface(surface);
    if (!r || !r->has_content || r->id == keyboard_)
        return a;
    keyboard_ = r->id;
    a.bits |= lock_actions::refocus;
    return a;
}

lx::surface_id session_lock::keyboard_surface() const {
    const lock_surface_record* r = find(keyboard_);
    return r ? r->surface : lx::surface_id{};
}

void* session_lock::keyboard_resource() const {
    const lock_surface_record* r = find(keyboard_);
    return r ? r->surface_res : nullptr;
}

unsigned session_lock::surface_count() const {
    unsigned n = 0;
    for (const auto& r : surfaces_)
        if (r.id)
            ++n;
    return n;
}

void session_lock::clear_surfaces() {
    for (auto& r : surfaces_)
        r = {};
    keyboard_ = 0;
}

bool session_lock::every_output_covered() const {
    for (unsigned slot = 0; slot < 32; ++slot) {
        if (!(outputs_ & (1u << slot)))
            continue;
        bool covered = false;
        for (const auto& r : surfaces_)
            if (r.id && r.output == slot && r.has_content)
                covered = true;
        if (!covered)
            return false;
    }
    return true;
}

void session_lock::maybe_complete(lock_actions& a) {
    if (state_ == lock_state::awaiting_surfaces && every_output_covered()) {
        deadline_ = 0;
        a.bits |= lock_actions::rearm_timer;
        enter_locking(a);
    }
}

void session_lock::enter_locking(lock_actions& a) {
    state_ = lock_state::locking;
    a.bits |= lock_actions::scene_lock | lock_actions::refocus;
}

void session_lock::to_unlocked(lock_actions& a, bool was_scene_locked) {
    state_ = lock_state::unlocked;
    lock_ = nullptr;
    deadline_ = 0;
    keyboard_ = 0;
    a.bits |= lock_actions::input_unlock | lock_actions::notify_shell |
              lock_actions::drop_surfaces | lock_actions::rearm_timer;
    if (was_scene_locked)
        a.bits |= lock_actions::scene_unlock;
    if (hint_) {
        hint_ = false;
        a.bits |= lock_actions::locked_hint;
        a.hint = false;
    }
}

void session_lock::refocus_after_removal(lock_actions& a) {
    unsigned best = 0;
    unsigned order = ~0u;
    for (const auto& r : surfaces_) {
        if (r.id && r.has_content && r.content_order < order) {
            order = r.content_order;
            best = r.id;
        }
    }
    keyboard_ = best;
    a.bits |= lock_actions::refocus;
}

} // namespace lx::compositor
