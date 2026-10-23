module;

#include <stdio.h>

import lx.foundation;

export module lx.shell:task_model;

import std;

export namespace lx::shell {

// ── Task model: the shell's copy of the compositor's window list ──────────────
//
// Fed by zlm_policy_bridge_v1 (snapshot, then deltas) and read by the panel. Pure and
// Wayland-free: the bridge stores each zlm_toplevel_v1 proxy as an opaque pointer and the
// model never calls into it. Fixed storage — no event allocates — and typed handles with a
// generation, so a handle kept across a removal can never name the entry that reuses its
// slot.

inline constexpr unsigned k_max_tasks = 256;
inline constexpr unsigned k_task_app_id_bytes = 128;
/// Titles longer than 255 bytes are cut on a UTF-8 code point boundary.
inline constexpr unsigned k_task_title_bytes = 256;
inline constexpr unsigned k_task_icon_name_bytes = 128;
/// zlm_policy_bridge_v1.toplevel_state `modal` (v6: an effectively modal dialog).
inline constexpr unsigned k_task_state_modal = 5;
/// zlm_policy_bridge_v1.toplevel_state `minimized` (sent from v7).
inline constexpr unsigned k_task_state_minimized = 3;

struct task_handle {
    unsigned index = ~0u;
    unsigned gen = 0;
    [[nodiscard]] explicit operator bool() const { return index != ~0u; }
    friend bool operator==(task_handle, task_handle) = default;
};

struct task_entry {
    bool used = false;
    unsigned gen = 0;
    /// The zlm_toplevel_v1 proxy (opaque here).
    void* proxy = nullptr;
    char app_id[k_task_app_id_bytes]{};
    char title[k_task_title_bytes]{};
    /// zlm_policy_bridge_v1.toplevel_state (0 normal, 1 maximized, …).
    unsigned state = 0;
    lx::rect2i geometry{};
    /// Last stacking index the compositor reported (0 = bottom). Stored, not drawn: a task
    /// list is in creation order, not stacking order.
    unsigned stacking = 0;
    /// Bumped whenever app_id or title changes, so a renderer can tell "same label" from
    /// "same handle" without comparing strings.
    unsigned label_rev = 0;
    /// zlm_policy_bridge_v1.toplevel_flags (v2): `task_flag_*` bits.
    unsigned flags = 0;
    /// zlm v6 toplevel_icon: the icon-theme name ("" none). `icon_rev` changes with the
    /// name or the pixel buffers (which the bridge keeps), so a renderer re-resolves.
    char icon_name[k_task_icon_name_bytes]{};
    unsigned icon_rev = 0;
    /// zlm v6 toplevel_parent: the window this one is a dialog or child of.
    task_handle parent{};
    /// zlm v7 toplevel_workspace: its workspace, the shell's ext_workspace_handle_v1 (opaque;
    /// null: sticky, or not known). `workspace_known`: the compositor said.
    void* workspace = nullptr;
    bool workspace_known = false;
};

/// zlm_policy_bridge_v1.toplevel_flag values.
enum task_flag : unsigned {
    task_flag_not_responding = 0x1,
    task_flag_demands_attention = 0x2,
    /// zlm v7: shown on every workspace of its output.
    task_flag_sticky = 0x4,
};

/// What changed since the last `take_changes()` (a bit set).
enum task_change : unsigned {
    task_change_list = 1u << 0,     ///< an entry was added or removed, or a snapshot landed
    task_change_label = 1u << 1,    ///< app_id or title
    task_change_state = 1u << 2,
    task_change_geometry = 1u << 3,
    task_change_focus = 1u << 4,
    task_change_stacking = 1u << 5,
    task_change_flags = 1u << 6,    ///< not responding / demands attention
    task_change_icon = 1u << 7,     ///< icon name or pixels (v6)
    task_change_parent = 1u << 8,   ///< the parent (v6)
    task_change_workspace = 1u << 9, ///< the workspace (v7)
};

class task_model;

/// A modal dialog over a window that is in the list: a task bar shows the parent alone (its
/// entry activates the dialog anyway — the compositor redirects focus to it).
[[nodiscard]] bool task_is_modal_child(const task_model& model, const task_entry& e);

/// The output a window of `geometry` belongs to, for the task list of each output's panel
/// (ADR 008 M13, Plasma's "only tasks from the current screen"): the one among `outputs`
/// (logical rects) it overlaps most, ties to the lower index; -1 when it overlaps none.
[[nodiscard]] int task_output(lx::rect2i geometry, const lx::rect2i* outputs, unsigned count);

/// Whether a task list showing `active` (the workspace proxies active on its output) lists
/// `e` (W.7: the current workspace only, GNOME's and KDE's default): sticky windows, windows
/// on an active workspace, and windows whose workspace is not known (no ext-workspace, a
/// compositor before zlm v7) are listed; minimized windows are listed (drawn dimmed).
[[nodiscard]] bool task_on_shown_workspace(const task_entry& e, bool (*active)(const void* ws,
                                                                               void* user),
                                           void* user);

/// Copies `src` into `dst` (`cap` bytes including the terminator). A string that does not
/// fit is cut at the last code point boundary that does — never inside a UTF-8 sequence,
/// which a shaper would turn into U+FFFD. Returns true when it was cut.
bool copy_utf8(char* dst, unsigned cap, const char* src);

class task_model {
public:
    // ── Snapshot (zlm_policy_bridge_v1.snapshot_begin … snapshot_done) ──
    //
    // begin_snapshot() empties the model (each proxy it held goes to `drop`), entries are
    // added as they arrive, and end_snapshot() marks it complete. While a snapshot is in
    // progress `in_snapshot()` is true and a renderer keeps what it last drew, so a
    // half-delivered batch is never shown. Deltas that arrive meanwhile apply in order.
    void begin_snapshot(void (*drop)(void* proxy, void* user) = nullptr, void* user = nullptr);
    [[nodiscard]] task_handle snapshot_add(void* proxy, const char* app_id, const char* title,
                                           unsigned state, lx::rect2i geometry,
                                           unsigned stacking);
    void end_snapshot();
    [[nodiscard]] bool in_snapshot() const { return in_snapshot_; }
    /// True once a snapshot has completed (the model reflects the compositor).
    [[nodiscard]] bool synced() const { return synced_; }

    /// Empties the model (each proxy to `drop`) and forgets the snapshot: not synced.
    void clear(void (*drop)(void* proxy, void* user) = nullptr, void* user = nullptr);

    // ── Deltas ──
    /// toplevel_created: an entry with no label yet (toplevel_state follows). Null handle
    /// when the table is full.
    [[nodiscard]] task_handle add(void* proxy);
    /// toplevel_destroyed. False for a stale or unknown handle. A removed focused entry
    /// leaves the model with no focus.
    bool remove(task_handle h);
    /// toplevel_state. Returns the change bits it caused (0 for a stale handle or no change).
    unsigned set_state(task_handle h, const char* app_id, const char* title, unsigned state);
    bool set_geometry(task_handle h, lx::rect2i geometry);
    bool set_stacking(task_handle h, unsigned index);
    /// toplevel_flags. Returns the change bits it caused.
    unsigned set_flags(task_handle h, unsigned flags);
    /// focus_changed. A null or stale handle means no focus. The window focused goes to the
    /// front of the most-recently-used order (`mru`), at `now_ns` (the debounce's clock).
    void set_focus(task_handle h, long long now_ns = 0);
    /// Focus that lasted less than this before it moved on does not count for the MRU order
    /// (niri's `debounce-ms`: windows the pointer only passed over with focus-follows-mouse):
    /// that window goes back to where it was. 0 (click to focus): every focus counts.
    void set_focus_debounce(long long ns) { debounce_ns_ = ns; }
    /// The next focus of `h` was asked for explicitly (the switcher, a task-list click): it
    /// counts however briefly it lasts.
    void note_explicit_focus(task_handle h) { explicit_ = h; }
    /// toplevel_icon … toplevel_icon_done (v6): the name, and a new revision either way
    /// (the pixels changed with it). False for a stale handle.
    bool set_icon(task_handle h, const char* name);
    /// toplevel_parent (v6). A null or stale `parent` means none.
    bool set_parent(task_handle h, task_handle parent);
    /// toplevel_workspace (v7): `workspace` is the ext_workspace_handle_v1 (null: sticky,
    /// or none known yet).
    bool set_workspace(task_handle h, void* workspace);
    /// The workspace handle `workspace` was removed: no task names it any more.
    void forget_workspace(const void* workspace);

    // ── Queries ──
    [[nodiscard]] task_handle focused() const { return focused_; }
    [[nodiscard]] const task_entry* get(task_handle h) const;
    /// The live entry holding `proxy`; `index_hint` (the proxy's user data) is tried first.
    [[nodiscard]] task_handle find_proxy(unsigned index_hint, const void* proxy) const;
    [[nodiscard]] unsigned count() const { return static_cast<unsigned>(order_.size()); }
    /// Live entries in creation order (snapshot entries first, bottom-up as the compositor
    /// listed them). Returns how many were written.
    unsigned ordered(task_handle* out, unsigned cap) const;
    /// Live entries most recently focused first (the window switcher's order, design K.2): a
    /// snapshot seeds it from the stacking order (top first), each focus moves its window to
    /// the front, a new window enters at the back until it is focused. Returns how many.
    unsigned mru(task_handle* out, unsigned cap) const;
    /// Change bits accumulated since the last call; clears them.
    [[nodiscard]] unsigned take_changes();
    [[nodiscard]] unsigned pending_changes() const { return changes_; }

private:
    [[nodiscard]] task_entry* get_mut(task_handle h);
    [[nodiscard]] task_handle insert(void* proxy);

    task_entry entries_[k_max_tasks]{};
    std::inplace_vector<std::uint16_t, k_max_tasks> order_;
    /// Slot indices, most recently focused first.
    std::inplace_vector<std::uint16_t, k_max_tasks> mru_;
    task_handle focused_{};
    long long debounce_ns_ = 0;
    /// The front of `mru_` came there at `promoted_at_` from `promoted_from_`, and may go back
    /// (debounce) unless it was asked for (`explicit_`).
    bool tentative_ = false;
    unsigned promoted_from_ = 0;
    long long promoted_at_ = 0;
    task_handle explicit_{};
    /// Global, never reused: a handle from before a snapshot cannot match after it.
    unsigned next_gen_ = 1;
    unsigned changes_ = 0;
    bool in_snapshot_ = false;
    bool synced_ = false;
};

} // namespace lx::shell

bool lx::shell::copy_utf8(char* dst, unsigned cap, const char* src) {
    if (!dst || cap == 0)
        return false;
    if (!src)
        src = "";
    const std::size_t len = std::strlen(src);
    if (len < cap) {
        std::memcpy(dst, src, len + 1);
        return false;
    }
    // src[cut] is the first byte left out; while it continues a sequence, that sequence
    // began inside the kept part and has to go too.
    std::size_t cut = cap - 1;
    while (cut > 0 && (static_cast<unsigned char>(src[cut]) & 0xC0u) == 0x80u)
        --cut;
    std::memcpy(dst, src, cut);
    dst[cut] = '\0';
    return true;
}

void lx::shell::task_model::begin_snapshot(void (*drop)(void* proxy, void* user), void* user) {
    clear(drop, user);
    in_snapshot_ = true;
}

void lx::shell::task_model::clear(void (*drop)(void* proxy, void* user), void* user) {
    for (std::uint16_t idx : order_) {
        task_entry& e = entries_[idx];
        if (drop && e.proxy)
            drop(e.proxy, user);
        e = task_entry{};
    }
    order_.clear();
    mru_.clear();
    tentative_ = false;
    explicit_ = {};
    focused_ = {};
    in_snapshot_ = false;
    synced_ = false;
    changes_ |= task_change_list | task_change_focus;
}

lx::shell::task_handle lx::shell::task_model::snapshot_add(void* proxy, const char* app_id,
                                                           const char* title, unsigned state,
                                                           lx::rect2i geometry,
                                                           unsigned stacking) {
    const task_handle h = insert(proxy);
    if (task_entry* e = get_mut(h)) {
        (void)copy_utf8(e->app_id, sizeof(e->app_id), app_id);
        (void)copy_utf8(e->title, sizeof(e->title), title);
        e->state = state;
        e->geometry = geometry;
        e->stacking = stacking;
    }
    return h;
}

void lx::shell::task_model::end_snapshot() {
    in_snapshot_ = false;
    synced_ = true;
    changes_ |= task_change_list;
    // A shell that (re)started knows no focus history: the stacking order is the best record
    // of it (click to focus raises), top first.
    std::ranges::stable_sort(mru_, [&](std::uint16_t a, std::uint16_t b) {
        return entries_[a].stacking > entries_[b].stacking;
    });
    tentative_ = false;
}

lx::shell::task_handle lx::shell::task_model::insert(void* proxy) {
    if (order_.size() >= k_max_tasks)
        return {};
    for (unsigned i = 0; i < k_max_tasks; ++i) {
        task_entry& e = entries_[i];
        if (e.used)
            continue;
        e = task_entry{};
        e.used = true;
        e.gen = next_gen_++;
        e.proxy = proxy;
        order_.push_back(static_cast<std::uint16_t>(i));
        mru_.push_back(static_cast<std::uint16_t>(i));
        changes_ |= task_change_list;
        return task_handle{i, e.gen};
    }
    return {};
}

lx::shell::task_handle lx::shell::task_model::add(void* proxy) { return insert(proxy); }

bool lx::shell::task_model::remove(task_handle h) {
    task_entry* e = get_mut(h);
    if (!e)
        return false;
    if (const auto it = std::ranges::find(order_, h.index); it != order_.end())
        order_.erase(it);
    if (const auto it = std::ranges::find(mru_, h.index); it != mru_.end()) {
        if (it == mru_.begin())
            tentative_ = false;
        else if (tentative_ && static_cast<unsigned>(it - mru_.begin()) < promoted_from_)
            --promoted_from_;
        mru_.erase(it);
    }
    *e = task_entry{};
    changes_ |= task_change_list;
    if (focused_ == h) {
        focused_ = {};
        changes_ |= task_change_focus;
    }
    return true;
}

unsigned lx::shell::task_model::set_state(task_handle h, const char* app_id, const char* title,
                                          unsigned state) {
    task_entry* e = get_mut(h);
    if (!e)
        return 0;
    unsigned changed = 0;
    char buf[k_task_title_bytes];
    (void)copy_utf8(buf, sizeof(e->app_id), app_id);
    if (std::strcmp(buf, e->app_id) != 0) {
        std::memcpy(e->app_id, buf, std::strlen(buf) + 1);
        changed |= task_change_label;
    }
    (void)copy_utf8(buf, sizeof(buf), title);
    if (std::strcmp(buf, e->title) != 0) {
        std::memcpy(e->title, buf, std::strlen(buf) + 1);
        changed |= task_change_label;
    }
    if (changed & task_change_label)
        ++e->label_rev;
    if (e->state != state) {
        e->state = state;
        changed |= task_change_state;
    }
    changes_ |= changed;
    return changed;
}

bool lx::shell::task_model::set_geometry(task_handle h, lx::rect2i geometry) {
    task_entry* e = get_mut(h);
    if (!e)
        return false;
    if (e->geometry.x != geometry.x || e->geometry.y != geometry.y ||
        e->geometry.width != geometry.width || e->geometry.height != geometry.height) {
        e->geometry = geometry;
        changes_ |= task_change_geometry;
    }
    return true;
}

bool lx::shell::task_model::set_stacking(task_handle h, unsigned index) {
    task_entry* e = get_mut(h);
    if (!e)
        return false;
    if (e->stacking != index) {
        e->stacking = index;
        changes_ |= task_change_stacking;
    }
    return true;
}

unsigned lx::shell::task_model::set_flags(task_handle h, unsigned flags) {
    task_entry* e = get_mut(h);
    if (!e || e->flags == flags)
        return 0;
    e->flags = flags;
    changes_ |= task_change_flags;
    return task_change_flags;
}

bool lx::shell::task_model::set_icon(task_handle h, const char* name) {
    task_entry* e = get_mut(h);
    if (!e)
        return false;
    (void)copy_utf8(e->icon_name, sizeof(e->icon_name), name);
    ++e->icon_rev;
    changes_ |= task_change_icon;
    return true;
}

bool lx::shell::task_model::set_parent(task_handle h, task_handle parent) {
    task_entry* e = get_mut(h);
    if (!e)
        return false;
    const task_handle p = get(parent) ? parent : task_handle{};
    if (!(e->parent == p)) {
        e->parent = p;
        changes_ |= task_change_parent;
    }
    return true;
}

bool lx::shell::task_model::set_workspace(task_handle h, void* workspace) {
    task_entry* e = get_mut(h);
    if (!e)
        return false;
    if (e->workspace != workspace || !e->workspace_known) {
        e->workspace = workspace;
        e->workspace_known = true;
        changes_ |= task_change_workspace;
    }
    return true;
}

void lx::shell::task_model::forget_workspace(const void* workspace) {
    if (!workspace)
        return;
    for (std::uint16_t idx : order_) {
        task_entry& e = entries_[idx];
        if (e.workspace == workspace) {
            e.workspace = nullptr;
            e.workspace_known = false;
            changes_ |= task_change_workspace;
        }
    }
}

bool lx::shell::task_on_shown_workspace(const task_entry& e,
                                        bool (*active)(const void* ws, void* user), void* user) {
    if ((e.flags & task_flag_sticky) || !e.workspace_known || !e.workspace || !active)
        return true;
    return active(e.workspace, user);
}

bool lx::shell::task_is_modal_child(const task_model& model, const task_entry& e) {
    return e.state == k_task_state_modal && model.get(e.parent) != nullptr;
}

void lx::shell::task_model::set_focus(task_handle h, long long now_ns) {
    const task_handle next = get(h) ? h : task_handle{};
    if (next == focused_)
        return;
    focused_ = next;
    changes_ |= task_change_focus;
    const auto move_to = [&](unsigned from, unsigned to) {
        const std::uint16_t idx = mru_[from];
        if (from > to)
            for (unsigned i = from; i > to; --i)
                mru_[i] = mru_[i - 1];
        else
            for (unsigned i = from; i < to; ++i)
                mru_[i] = mru_[i + 1];
        mru_[to] = idx;
    };
    // The window at the front only passed by (debounced focus): back where it was.
    if (tentative_ && !mru_.empty() && now_ns - promoted_at_ < debounce_ns_ &&
        promoted_from_ < mru_.size())
        move_to(0, promoted_from_);
    tentative_ = false;
    if (!next)
        return;
    const auto it = std::ranges::find(mru_, next.index);
    if (it == mru_.end())
        return;
    const unsigned at = static_cast<unsigned>(it - mru_.begin());
    move_to(at, 0);
    tentative_ = debounce_ns_ > 0 && at > 0 && !(explicit_ == next);
    promoted_from_ = at;
    promoted_at_ = now_ns;
    explicit_ = {};
}

unsigned lx::shell::task_model::mru(task_handle* out, unsigned cap) const {
    unsigned n = 0;
    for (std::uint16_t idx : mru_) {
        if (n >= cap)
            break;
        out[n++] = task_handle{idx, entries_[idx].gen};
    }
    return n;
}

const lx::shell::task_entry* lx::shell::task_model::get(task_handle h) const {
    if (h.index >= k_max_tasks)
        return nullptr;
    const task_entry& e = entries_[h.index];
    return e.used && e.gen == h.gen ? &e : nullptr;
}

lx::shell::task_entry* lx::shell::task_model::get_mut(task_handle h) {
    return const_cast<task_entry*>(static_cast<const task_model*>(this)->get(h));
}

lx::shell::task_handle lx::shell::task_model::find_proxy(unsigned index_hint,
                                                         const void* proxy) const {
    if (!proxy)
        return {};
    if (index_hint < k_max_tasks && entries_[index_hint].used &&
        entries_[index_hint].proxy == proxy)
        return {index_hint, entries_[index_hint].gen};
    for (std::uint16_t idx : order_) {
        const task_entry& e = entries_[idx];
        if (e.proxy == proxy)
            return {idx, e.gen};
    }
    return {};
}

unsigned lx::shell::task_model::ordered(task_handle* out, unsigned cap) const {
    unsigned n = 0;
    for (std::uint16_t idx : order_) {
        if (n >= cap)
            break;
        out[n++] = task_handle{idx, entries_[idx].gen};
    }
    return n;
}

unsigned lx::shell::task_model::take_changes() {
    const unsigned c = changes_;
    changes_ = 0;
    return c;
}

int lx::shell::task_output(lx::rect2i g, const lx::rect2i* outputs, unsigned count) {
    int best = -1;
    long long best_area = 0;
    for (unsigned i = 0; i < count && outputs; ++i) {
        const lx::rect2i& o = outputs[i];
        const long long x0 = g.x > o.x ? g.x : o.x;
        const long long y0 = g.y > o.y ? g.y : o.y;
        const long long x1 = static_cast<long long>(g.x) + g.width < static_cast<long long>(o.x) + o.width
                                 ? static_cast<long long>(g.x) + g.width
                                 : static_cast<long long>(o.x) + o.width;
        const long long y1 = static_cast<long long>(g.y) + g.height < static_cast<long long>(o.y) + o.height
                                 ? static_cast<long long>(g.y) + g.height
                                 : static_cast<long long>(o.y) + o.height;
        const long long a = x1 > x0 && y1 > y0 ? (x1 - x0) * (y1 - y0) : 0;
        if (a > best_area) {
            best_area = a;
            best = static_cast<int>(i);
        }
    }
    return best;
}
