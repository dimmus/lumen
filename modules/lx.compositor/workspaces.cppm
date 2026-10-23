module;

// Workspaces (design-session-and-wm Part W): per output, ordered, one active per output.
// Dynamic by default — always exactly one empty workspace at the end of each output's list,
// and an empty workspace that is not active goes (GNOME, niri); static as an option — a fixed
// count per output, named from the configuration. Pure and Wayland-free, UI affinity, fixed
// tables: no event allocates.
//
// The model holds workspaces, their groups (one per live output, in slot order) and a count
// of member windows; which window is on which workspace lives in the window manager's
// records (`toplevel_record::workspace`). Whenever the model moves windows between
// workspaces (a merge on hotplug or on a smaller static count) or a workspace between
// outputs (ext-workspace `assign`), it tells the window manager through `workspace_sink`,
// which updates its records and their geometry. The window manager is the model's only
// caller; ext-workspace (wire_workspace.cpp) and the shell bridge read it.
//
// Hotplug (W.6): an output that goes hands its active workspace's windows to the fallback
// output's active workspace (nothing visible vanishes) and appends its other non-empty
// workspaces to the fallback's list (static: merged into the same-index workspace), each
// remembering the gone output's identifier and its place there. When that output returns,
// those workspaces go back in their order, windows that were merged go to a workspace at
// their old index, and the workspace that was active there is active again
// (`output_added` → `home_workspace` per returning window → `settle`). With no output left
// the workspaces wait, stranded, for the next one. The member bodies are in workspaces.cpp
// (Clang 23: bodies in a partition interface miscompile; see session_lock.cpp).

export module lx.compositor:workspaces;

import lx.foundation;

import std;

export namespace lx::compositor {

inline constexpr unsigned k_max_workspaces = 64;
inline constexpr unsigned k_workspaces_per_output = 16;
inline constexpr unsigned k_workspace_groups = 16;
inline constexpr unsigned k_workspace_name_bytes = 32;
inline constexpr unsigned k_workspace_identifier_bytes = 208;
/// Outputs that went, remembered for their return (which workspace was active there).
inline constexpr unsigned k_gone_outputs = 8;

/// config.toml [workspaces], as the model needs it.
struct workspace_config {
    /// One empty workspace always last, empty inactive ones go; false: `count` per output.
    bool dynamic = true;
    unsigned count = 4;
    char names[k_workspaces_per_output][k_workspace_name_bytes]{};
    unsigned name_count = 0;
    /// A granted activation of a window on an inactive workspace switches there.
    bool switch_on_activate = true;
    /// Moving the focused window to a workspace switches with it.
    bool move_follows = true;
};

struct workspace_record {
    lx::workspace_id id{};
    /// The output slot of its group; -1 while stranded (no output left).
    int output = -1;
    /// 0-based position in its group.
    unsigned index = 0;
    char name[k_workspace_name_bytes]{};
    /// A static workspace named by the configuration: ext-workspace announces an `id` for it.
    bool named = false;
    /// Moved off an output that went: that output's identifier ("" none) and its 1-based
    /// position there. Cleared when it goes back, or when the user moves it.
    char home[k_workspace_identifier_bytes]{};
    unsigned home_index = 0;
    /// The window that last had the keyboard here (W.1).
    lx::toplevel_id focus_memory{};
    /// Member windows (sticky ones are members of none).
    unsigned windows = 0;
    bool active = false;
    /// A member window demands attention.
    bool urgent = false;
    /// While an output returns (`output_added` … `settle`): the 1-based position the
    /// workspace had there; 0 otherwise.
    unsigned origin_index = 0;
};

struct workspace_group {
    bool live = false;
    /// The output's id: a new output in the same slot is a new group.
    unsigned long long key = 0;
    /// Never reused within a run; what the wire keys its group handles by.
    unsigned serial = 0;
    char identifier[k_workspace_identifier_bytes]{};
    /// The connector name ("Virtual-1"), for ids of named workspaces.
    char output_name[32]{};
    lx::workspace_id order[k_workspaces_per_output]{};
    unsigned count = 0;
    /// `settle` activates the workspace with this origin index (0: none pending).
    unsigned pending_active = 0;
};

/// Told when the model moves windows (the window manager updates its records).
struct workspace_sink {
    /// Every window of `from` belongs to `to` now (`from` is going).
    void (*windows_moved)(void* user, lx::workspace_id from, lx::workspace_id to) = nullptr;
    /// Workspace `ws` moved from output `from_slot` to `to_slot` (its windows follow).
    void (*workspace_moved)(void* user, lx::workspace_id ws, int from_slot, int to_slot) = nullptr;
    void* user = nullptr;
};

class workspace_model {
public:
    // ── configuration ──
    /// Applies a new [workspaces] (mode, count, names): every group follows (static count
    /// smaller: the windows of removed workspaces go to the last one kept).
    void configure(const workspace_config& c, const workspace_sink& sink);
    [[nodiscard]] const workspace_config& config() const { return config_; }

    // ── outputs ──
    /// Output `slot` (id `key`, EDID identifier, connector name) is live: its group, made of
    /// the stranded workspaces if any wait, else of the workspaces whose home it is (and,
    /// dynamic, one empty one; static, `count`). Windows that return go to
    /// `home_workspace`; `settle` ends the return. No-op when the group exists for `key`.
    void output_added(unsigned slot, unsigned long long key, const char* identifier,
                      const char* output_name, const workspace_sink& sink);
    /// The workspace at 1-based `index_1based` of the returning output `slot` for a window
    /// that was there: an existing one that came back from that position, else a new one
    /// placed among them by position (static: the one at that index, clamped).
    [[nodiscard]] lx::workspace_id home_workspace(unsigned slot, unsigned index_1based);
    /// The return is over: the workspace that was active there is active again, positions
    /// are forgotten, dynamic invariants restored.
    void settle(unsigned slot);
    /// Output `slot` goes (W.6). `fallback` -1: no output is left; the workspaces strand.
    /// `identifier` non-empty: what is active there is remembered for its return.
    void output_removed(unsigned slot, int fallback, const workspace_sink& sink);
    [[nodiscard]] bool has_group(unsigned slot) const {
        return slot < k_workspace_groups && groups_[slot].live;
    }
    [[nodiscard]] const workspace_group& group(unsigned slot) const;

    // ── queries ──
    [[nodiscard]] const workspace_record* find(lx::workspace_id id) const;
    /// The active workspace of output `slot` (null without a group).
    [[nodiscard]] lx::workspace_id active(unsigned slot) const;
    [[nodiscard]] bool is_active(lx::workspace_id id) const;
    /// The 1-based `index_1based` workspace of `slot`: dynamic, an index past the end is
    /// the last (empty) one, as in niri; static, none.
    [[nodiscard]] lx::workspace_id by_index(unsigned slot, unsigned index_1based) const;
    /// The workspace `dir` (±1) positions away in its group; none at either end (no wrap).
    [[nodiscard]] lx::workspace_id neighbour(lx::workspace_id id, int dir) const;
    /// Every workspace (groups in slot order, each in its order; stranded ones last).
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (unsigned s = 0; s < k_workspace_groups; ++s)
            if (groups_[s].live)
                for (unsigned i = 0; i < groups_[s].count; ++i)
                    if (const workspace_record* w = find(groups_[s].order[i]))
                        fn(*w);
        for (unsigned i = 0; i < stranded_count_; ++i)
            if (const workspace_record* w = find(stranded_[i]))
                fn(*w);
    }
    [[nodiscard]] unsigned count() const { return used_; }
    /// Bumped by every change a client can see (names, order, state, groups).
    [[nodiscard]] unsigned generation() const { return generation_; }

    // ── operations ──
    /// Makes `id` the active workspace of its output; the one left goes when it is empty
    /// (dynamic). False when it already was, or is unknown or stranded.
    bool activate(lx::workspace_id id);
    void window_added(lx::workspace_id id);
    /// A member left (closed, made sticky, moved): an empty, inactive workspace goes
    /// (dynamic).
    void window_removed(lx::workspace_id id);
    /// ext-workspace `assign`: `id` moves to the end of output `slot`'s list (before the
    /// trailing empty one), its windows with it. Its old output activates a neighbour if it
    /// was active there. False when nothing changed (same output, unknown, full).
    bool assign(lx::workspace_id id, unsigned slot, const workspace_sink& sink);
    void set_focus_memory(lx::workspace_id id, lx::toplevel_id window);
    [[nodiscard]] lx::toplevel_id focus_memory(lx::workspace_id id) const;
    /// A window went: no workspace remembers it.
    void forget_window(lx::toplevel_id window);
    void set_urgent(lx::workspace_id id, bool on);
    /// The user placed `id` (it no longer goes back to an output it was moved off).
    void clear_home(lx::workspace_id id);

private:
    [[nodiscard]] workspace_record* find_mut(lx::workspace_id id);
    [[nodiscard]] workspace_record* create(int slot);
    void destroy(lx::workspace_id id);
    /// Takes `id` out of its group's order (or the stranded list).
    void unlink(lx::workspace_id id);
    void insert(unsigned slot, unsigned pos, lx::workspace_id id);
    /// Dynamic: empty inactive workspaces go, except the last; the last is empty. Every
    /// group: positions, names, exactly one active.
    void normalize(unsigned slot);
    void rename(unsigned slot);
    /// Moves every window of `from` into `to` (counts, the sink) and destroys `from`.
    void merge(lx::workspace_id from, lx::workspace_id to, const workspace_sink& sink);
    void changed() { ++generation_; }

    workspace_config config_{};
    workspace_record records_[k_max_workspaces]{};
    bool used_slots_[k_max_workspaces]{};
    unsigned used_ = 0;
    unsigned long long next_id_ = 1;
    workspace_group groups_[k_workspace_groups]{};
    unsigned next_serial_ = 1;
    lx::workspace_id stranded_[k_max_workspaces]{};
    unsigned stranded_count_ = 0;
    /// The stranded workspace that was active (adopted as active by the next output).
    lx::workspace_id stranded_active_{};
    struct gone_output {
        char identifier[k_workspace_identifier_bytes]{};
        /// 1-based position of the workspace that was active there.
        unsigned active_index = 0;
    };
    gone_output gone_[k_gone_outputs]{};
    unsigned gone_next_ = 0;
    unsigned generation_ = 1;
};

} // namespace lx::compositor
