module;

// The session lock (ext-session-lock-v1, staging, v1): who holds the lock, what the session
// shows, which lock surface has the keyboard, and each lock surface's configure bookkeeping.
// Pure and Wayland-free — resources are opaque pointers, every decision is returned as
// `lock_actions` for the compositor and the wire unit (wire_session_lock.cpp) to execute.
// UI affinity throughout. See docs/architecture.md, "Session lock". The member bodies are in
// session_lock.cpp: this interface carries declarations only (the reason is there).
//
//   unlocked ──lock──▶ awaiting_surfaces ──every output has a lock surface with content,
//                         │                  or 1 s──▶ locking
//                         │ destroy (gave up) ──▶ unlocked
//   locking ──a locked frame reached every output (a content flip of it, or nothing of the
//             session can be on screen: VT away, powered off, never lit)──▶ locked
//   locked  ──unlock_and_destroy──▶ unlocked
//   any locked state ──the locker died──▶ abandoned ──lock (anyone)──▶ locked, at once
//   any locked state ──logind Unlock──▶ unlocked (`finished` to a live lock object)
//
// Input leaves normal clients at the lock request (design L1), before anything is hidden; the
// scene is locked (normal content hidden, the lock layer shown, a solid colour behind it) from
// `locking` on, and `locked` is sent only once such a frame is on every output — never at the
// request, as the spec requires. A locker that dies keeps the session locked: the solid colour
// stays (dark red when abandoned, so the user sees why) until a new locker takes over — it is
// then `locked` at once, nothing of the session being visible — or logind unlocks.
//
// A locker that dies before `locked` (awaiting, locking) also leaves the session abandoned:
// the lock was asked for (an idle daemon, before-sleep, the user) and failing open would show
// the session to whoever is in front of it. Only an explicit `destroy` before `locked` — the
// locker gave up — returns to unlocked.

export module lx.compositor:session_lock;

import lx.foundation;

export namespace lx::compositor {

enum class lock_state : unsigned char {
    unlocked,
    /// A locker asked; input has left normal clients, which are still drawn (≤ 1 s).
    awaiting_surfaces,
    /// The scene is locked; waiting for a locked frame to reach every output.
    locking,
    /// `locked` was sent to the lock in charge.
    locked,
    /// Locked, and the locker is gone: the solid colour, until a new locker or logind.
    abandoned,
};

/// zlm_shell_v1.lock_state (v5) for a state.
[[nodiscard]] constexpr unsigned shell_lock_state(lock_state s) {
    switch (s) {
    case lock_state::unlocked: return 0;
    case lock_state::awaiting_surfaces:
    case lock_state::locking: return 1;
    case lock_state::locked: return 2;
    case lock_state::abandoned: return 3;
    }
    return 0;
}

[[nodiscard]] constexpr const char* lock_state_name(lock_state s) {
    switch (s) {
    case lock_state::unlocked: return "unlocked";
    case lock_state::awaiting_surfaces: return "awaiting_surfaces";
    case lock_state::locking: return "locking";
    case lock_state::locked: return "locked";
    case lock_state::abandoned: return "abandoned";
    }
    return "?";
}

/// What to do, as bits. The compositor executes them in this order: input_lock, scene_lock,
/// send_finished, send_locked, drop_surfaces, scene_unlock, input_unlock, refocus,
/// locked_hint, notify_shell, rearm_timer, activity.
struct lock_actions {
    enum : unsigned {
        /// Normal clients lose the pointer and the keyboard now: a move or resize ends as
        /// Escape would, a drag is cancelled, popups are dismissed, shell key grabs stop.
        input_lock = 1u << 0,
        /// Hide every normal layer, show the lock layer over the solid colour, note the
        /// frame that will be the first locked one, repaint everything.
        scene_lock = 1u << 1,
        scene_unlock = 1u << 2,
        input_unlock = 1u << 3,
        /// ext_session_lock_v1.locked to `lock`.
        send_locked = 1u << 4,
        /// ext_session_lock_v1.finished to `finished`.
        send_finished = 1u << 5,
        /// logind SetLockedHint(`hint`).
        locked_hint = 1u << 6,
        /// zlm_shell_v1.lock_state to every bound shell (the shell-visible state changed).
        notify_shell = 1u << 7,
        /// The keyboard (or the pointer) target among lock surfaces may have changed.
        refocus = 1u << 8,
        /// Every lock surface record is gone: their trees leave the lock layer, their
        /// objects go inert (`for_each_surface` before `clear_surfaces`).
        drop_surfaces = 1u << 9,
        /// The deadline moved (`deadline()`).
        rearm_timer = 1u << 10,
        /// An unlock: counts as user activity (design I2).
        activity = 1u << 11,
    };
    unsigned bits = 0;
    void* lock = nullptr;
    void* finished = nullptr;
    bool hint = false;

    [[nodiscard]] bool has(unsigned b) const { return (bits & b) != 0; }
    [[nodiscard]] bool empty() const { return bits == 0; }
    lock_actions& operator|=(const lock_actions& o) {
        bits |= o.bits;
        if (o.lock)
            lock = o.lock;
        if (o.finished)
            finished = o.finished;
        if (o.has(locked_hint))
            hint = o.hint;
        return *this;
    }
};

/// One ext_session_lock_surface_v1 of the lock in charge.
struct lock_surface_record {
    /// Never reused (0: free slot); the wire object keeps it and finds itself inert when the
    /// record is gone.
    unsigned id = 0;
    void* resource = nullptr;    ///< ext_session_lock_surface_v1
    void* surface_res = nullptr; ///< wl_surface
    lx::surface_id surface{};
    unsigned output = 0; ///< output slot
    /// Configures sent and not yet acked, oldest first.
    static constexpr unsigned k_max_configures = 16;
    unsigned serials[k_max_configures]{};
    lx::size2i sizes[k_max_configures]{};
    unsigned pending = 0;
    bool ever_acked = false;
    lx::size2i acked_size{};
    bool has_content = false;
    /// Order in which records got content (focus: the first one to get it).
    unsigned content_order = 0;
};

class session_lock {
public:
    using clock_ns = unsigned long long;
    /// Lock surfaces of the lock in charge (one per output; outputs are at most 16).
    static constexpr unsigned k_max_surfaces = 16;
    /// How long `awaiting_surfaces` waits for a lock surface with content on every output
    /// (niri: 1000 ms) before locking with the solid colour where one is missing.
    static constexpr clock_ns k_surface_wait_ns = 1'000'000'000ull;

    enum class ack_result : unsigned char { ok, invalid_serial };
    enum class commit_error : unsigned char { none, before_first_ack, null_buffer, dimensions_mismatch };

    [[nodiscard]] lock_state state() const { return state_; }
    /// Normal clients get no input (from the request on).
    [[nodiscard]] bool input_locked() const { return state_ != lock_state::unlocked; }
    /// Normal content is hidden and the lock layer shown.
    [[nodiscard]] bool scene_locked() const {
        return state_ == lock_state::locking || state_ == lock_state::locked ||
               state_ == lock_state::abandoned;
    }
    /// The ext_session_lock_v1 in charge; null when unlocked or abandoned.
    [[nodiscard]] void* current() const { return lock_; }

    /// Live output slots (bit per slot): what "every output" means for `awaiting_surfaces`.
    [[nodiscard]] lock_actions set_outputs(unsigned mask);
    [[nodiscard]] unsigned outputs() const { return outputs_; }

    /// ext_session_lock_manager_v1.lock: `lock_res` becomes the lock in charge (unlocked:
    /// awaiting surfaces; abandoned: locked at once) or is sent `finished` (another holds it).
    [[nodiscard]] lock_actions request(void* lock_res, clock_ns now);
    /// unlock_and_destroy from a lock that was sent `locked` (the wire checks that). Unlocks
    /// when it is the lock in charge; nothing when it was finished meanwhile.
    [[nodiscard]] lock_actions unlock(void* lock_res);
    /// The lock object went: `destroy` (`client_gone` false — the locker gave up before
    /// `locked`, or it was finished) or its client's teardown (`client_gone` true — the locker
    /// died: the session stays locked, abandoned).
    [[nodiscard]] lock_actions released(void* lock_res, bool client_gone);
    /// logind Unlock (`loginctl unlock-session`, polkit-gated): unlocked from any state, and
    /// `finished` to the lock in charge.
    [[nodiscard]] lock_actions force_unlock();
    /// The 1 s wait, at `now`.
    [[nodiscard]] lock_actions on_timer(clock_ns now);
    /// When `on_timer` is next due (0: nothing to wait for).
    [[nodiscard]] clock_ns deadline() const { return deadline_; }
    /// A locked frame is on every output (or nothing of the session can be): `locking` ends.
    [[nodiscard]] lock_actions locked_frame_shown();

    // ── Lock surfaces (of the lock in charge only; another lock's are inert) ──

    /// A new lock surface; its id, or 0 when the table is full.
    [[nodiscard]] unsigned add_surface(void* resource, void* surface_res, lx::surface_id surface,
                                       unsigned output);
    [[nodiscard]] const lock_surface_record* find(unsigned id) const;
    [[nodiscard]] const lock_surface_record* find_surface(lx::surface_id surface) const;
    /// A configure was sent. With 16 outstanding the oldest is forgotten (acking it is then
    /// `invalid_serial`; a client acks the newest anyway).
    void note_configure(unsigned id, unsigned serial, lx::size2i size);
    /// ack_configure: the serial must be outstanding; it and every older one are consumed.
    [[nodiscard]] ack_result ack(unsigned id, unsigned serial);
    /// wl_surface.commit of a lock surface, on the state the commit produces: never acked,
    /// no buffer, or a size other than the last acked configure's.
    [[nodiscard]] commit_error check_commit(unsigned id, bool has_buffer, lx::size2i size) const;
    /// The commit was applied: the surface shows content now (it always does — a null buffer
    /// is an error). May complete `awaiting_surfaces` and give it the keyboard.
    [[nodiscard]] lock_actions content(unsigned id);
    /// The lock surface object (or its wl_surface) went: its output shows the solid colour,
    /// and the keyboard moves to another lock surface.
    [[nodiscard]] lock_actions remove_surface(unsigned id);
    /// A press landed on lock surface `surface`: it takes the keyboard.
    [[nodiscard]] lock_actions clicked(lx::surface_id surface);
    /// The lock surface with the keyboard (null id when none).
    [[nodiscard]] lx::surface_id keyboard_surface() const;
    [[nodiscard]] void* keyboard_resource() const;

    template <typename Fn>
    void for_each_surface(Fn&& fn) const {
        for (const auto& r : surfaces_)
            if (r.id)
                fn(r);
    }
    [[nodiscard]] unsigned surface_count() const;
    void clear_surfaces();

private:
    [[nodiscard]] lock_surface_record* find_mut(unsigned id);
    [[nodiscard]] bool every_output_covered() const;
    /// awaiting → locking when every output has content.
    void maybe_complete(lock_actions& a);
    void enter_locking(lock_actions& a);
    void to_unlocked(lock_actions& a, bool was_scene_locked);
    void refocus_after_removal(lock_actions& a);

    lock_state state_ = lock_state::unlocked;
    void* lock_ = nullptr;
    unsigned outputs_ = 0;
    clock_ns deadline_ = 0;
    lock_surface_record surfaces_[k_max_surfaces]{};
    unsigned next_id_ = 1;
    unsigned next_content_ = 1;
    unsigned keyboard_ = 0;
    bool hint_ = false;
};

} // namespace lx::compositor
