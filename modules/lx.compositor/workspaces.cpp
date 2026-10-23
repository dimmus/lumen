module;

// The workspace model's member bodies (`:workspaces`; why they live here: session_lock.cpp).

module lx.compositor;

import lx.foundation;
import :workspaces;

import std;

namespace lx::compositor {

namespace {

void copy_text(char* dst, unsigned cap, const char* src) {
    std::snprintf(dst, cap, "%s", src ? src : "");
}

} // namespace

const workspace_group& workspace_model::group(unsigned slot) const {
    static const workspace_group k_none{};
    return slot < k_workspace_groups ? groups_[slot] : k_none;
}

workspace_record* workspace_model::find_mut(lx::workspace_id id) {
    if (!id)
        return nullptr;
    for (unsigned i = 0; i < k_max_workspaces; ++i)
        if (used_slots_[i] && records_[i].id == id)
            return &records_[i];
    return nullptr;
}

const workspace_record* workspace_model::find(lx::workspace_id id) const {
    return const_cast<workspace_model*>(this)->find_mut(id);
}

workspace_record* workspace_model::create(int slot) {
    for (unsigned i = 0; i < k_max_workspaces; ++i) {
        if (used_slots_[i])
            continue;
        used_slots_[i] = true;
        ++used_;
        workspace_record& w = records_[i];
        w = {};
        w.id = lx::workspace_id{next_id_++};
        w.output = slot;
        changed();
        return &w;
    }
    return nullptr;
}

void workspace_model::destroy(lx::workspace_id id) {
    unlink(id);
    for (unsigned i = 0; i < k_max_workspaces; ++i) {
        if (used_slots_[i] && records_[i].id == id) {
            records_[i] = {};
            used_slots_[i] = false;
            --used_;
            changed();
            return;
        }
    }
}

void workspace_model::unlink(lx::workspace_id id) {
    for (auto& g : groups_) {
        if (!g.live)
            continue;
        for (unsigned i = 0; i < g.count; ++i) {
            if (g.order[i] != id)
                continue;
            for (unsigned j = i + 1; j < g.count; ++j)
                g.order[j - 1] = g.order[j];
            g.order[--g.count] = {};
            changed();
            return;
        }
    }
    for (unsigned i = 0; i < stranded_count_; ++i) {
        if (stranded_[i] != id)
            continue;
        for (unsigned j = i + 1; j < stranded_count_; ++j)
            stranded_[j - 1] = stranded_[j];
        stranded_[--stranded_count_] = {};
        if (stranded_active_ == id)
            stranded_active_ = {};
        return;
    }
}

void workspace_model::insert(unsigned slot, unsigned pos, lx::workspace_id id) {
    workspace_group& g = groups_[slot];
    if (g.count >= k_workspaces_per_output)
        return;
    if (pos > g.count)
        pos = g.count;
    for (unsigned j = g.count; j > pos; --j)
        g.order[j] = g.order[j - 1];
    g.order[pos] = id;
    ++g.count;
    if (workspace_record* w = find_mut(id))
        w->output = static_cast<int>(slot);
    changed();
}

void workspace_model::rename(unsigned slot) {
    workspace_group& g = groups_[slot];
    for (unsigned i = 0; i < g.count; ++i) {
        workspace_record* w = find_mut(g.order[i]);
        if (!w)
            continue;
        char name[k_workspace_name_bytes];
        bool named = false;
        if (!config_.dynamic && i < config_.name_count && config_.names[i][0]) {
            copy_text(name, sizeof(name), config_.names[i]);
            named = true;
        } else {
            std::snprintf(name, sizeof(name), "%u", i + 1);
        }
        if (w->index != i || std::strcmp(w->name, name) != 0 || w->named != named) {
            w->index = i;
            std::memcpy(w->name, name, sizeof(name));
            w->named = named;
            changed();
        }
        w->output = static_cast<int>(slot);
    }
}

void workspace_model::normalize(unsigned slot) {
    if (slot >= k_workspace_groups || !groups_[slot].live)
        return;
    workspace_group& g = groups_[slot];
    if (config_.dynamic) {
        // Empty, inactive and not last: gone (the last one is the trailing empty workspace).
        for (unsigned i = 0; i + 1 < g.count;) {
            const workspace_record* w = find(g.order[i]);
            if (w && w->windows == 0 && !w->active && w->origin_index == 0) {
                destroy(g.order[i]);
                continue;
            }
            ++i;
        }
        // The last is empty: one more when it is not.
        const workspace_record* last = g.count ? find(g.order[g.count - 1]) : nullptr;
        if ((!last || last->windows > 0) && g.count < k_workspaces_per_output) {
            if (workspace_record* w = create(static_cast<int>(slot)))
                insert(slot, g.count, w->id);
        }
    } else {
        // Static: exactly `count` (never fewer; a merge makes more impossible).
        while (g.count < config_.count && g.count < k_workspaces_per_output) {
            workspace_record* w = create(static_cast<int>(slot));
            if (!w)
                break;
            insert(slot, g.count, w->id);
        }
    }
    // Exactly one active: the first when none is.
    bool any = false;
    for (unsigned i = 0; i < g.count; ++i)
        if (const workspace_record* w = find(g.order[i]); w && w->active) {
            if (any) {
                find_mut(g.order[i])->active = false;
                changed();
            }
            any = true;
        }
    if (!any && g.count > 0)
        if (workspace_record* w = find_mut(g.order[0])) {
            w->active = true;
            changed();
        }
    rename(slot);
}

void workspace_model::merge(lx::workspace_id from, lx::workspace_id to,
                            const workspace_sink& sink) {
    workspace_record* f = find_mut(from);
    workspace_record* t = find_mut(to);
    if (!f || !t || from == to)
        return;
    t->windows += f->windows;
    if (f->urgent)
        t->urgent = true;
    if (f->windows > 0 && sink.windows_moved)
        sink.windows_moved(sink.user, from, to);
    destroy(from);
}

void workspace_model::configure(const workspace_config& c, const workspace_sink& sink) {
    config_ = c;
    if (config_.count < 1)
        config_.count = 1;
    if (config_.count > k_workspaces_per_output)
        config_.count = k_workspaces_per_output;
    if (config_.name_count > k_workspaces_per_output)
        config_.name_count = k_workspaces_per_output;
    for (unsigned s = 0; s < k_workspace_groups; ++s) {
        workspace_group& g = groups_[s];
        if (!g.live)
            continue;
        if (!config_.dynamic) {
            // A smaller count: the windows of the workspaces past it go to the last one kept,
            // and the active one with them when it was among those.
            while (g.count > config_.count) {
                const lx::workspace_id gone = g.order[g.count - 1];
                const lx::workspace_id keep = g.order[config_.count - 1];
                const workspace_record* w = find(gone);
                const bool was_active = w && w->active;
                merge(gone, keep, sink);
                if (was_active)
                    if (workspace_record* k = find_mut(keep)) {
                        k->active = true;
                        changed();
                    }
            }
        }
        normalize(s);
    }
    changed();
}

void workspace_model::output_added(unsigned slot, unsigned long long key, const char* identifier,
                                   const char* output_name, const workspace_sink& sink) {
    if (slot >= k_workspace_groups)
        return;
    workspace_group& g = groups_[slot];
    if (g.live && g.key == key)
        return;
    if (g.live) // a different output reused the slot without the old one going: drop it
        output_removed(slot, -1, sink);
    g = {};
    g.live = true;
    g.key = key;
    g.serial = next_serial_++;
    copy_text(g.identifier, sizeof(g.identifier), identifier);
    copy_text(g.output_name, sizeof(g.output_name), output_name);
    changed();

    if (stranded_count_ > 0) {
        // Taken off the stranded list first: merge() → destroy() → unlink() edits that list,
        // and walking it by index while it shifts would skip every other overflow entry.
        lx::workspace_id waiting[k_max_workspaces];
        const unsigned n = stranded_count_;
        const lx::workspace_id was_active = stranded_active_;
        for (unsigned i = 0; i < n; ++i) {
            waiting[i] = stranded_[i];
            stranded_[i] = {};
        }
        stranded_count_ = 0;
        stranded_active_ = {};
        // The workspaces that waited for an output, in their order, the active one active.
        unsigned i = 0;
        for (; i < n && g.count < k_workspaces_per_output; ++i) {
            g.order[g.count++] = waiting[i];
            if (workspace_record* w = find_mut(waiting[i])) {
                w->output = static_cast<int>(slot);
                w->active = waiting[i] == was_active;
            }
        }
        // More than fit: merged into the last, which is shown when one of them was.
        const lx::workspace_id last = g.order[g.count - 1];
        for (; i < n; ++i) {
            if (waiting[i] == was_active) {
                for (unsigned j = 0; j < g.count; ++j)
                    if (workspace_record* w = find_mut(g.order[j]))
                        w->active = g.order[j] == last;
            }
            merge(waiting[i], last, sink);
        }
        changed();
        normalize(slot);
        return;
    }

    // Its own workspaces come back (dynamic: they were appended to another output), ordered
    // by the position they had here.
    if (g.identifier[0]) {
        lx::workspace_id back[k_workspaces_per_output];
        unsigned n = 0;
        for (unsigned i = 0; i < k_max_workspaces && n < k_workspaces_per_output; ++i) {
            if (!used_slots_[i] || std::strcmp(records_[i].home, g.identifier) != 0)
                continue;
            back[n++] = records_[i].id;
        }
        std::sort(back, back + n, [&](lx::workspace_id a, lx::workspace_id b) {
            return find(a)->home_index < find(b)->home_index;
        });
        for (unsigned i = 0; i < n; ++i) {
            workspace_record* w = find_mut(back[i]);
            const int from = w->output;
            const bool was_active = w->active;
            const unsigned origin = w->home_index;
            unlink(back[i]);
            w->active = false;
            w->home[0] = '\0';
            w->home_index = 0;
            w->origin_index = origin > 0 ? origin : 1;
            g.order[g.count++] = back[i];
            w->output = static_cast<int>(slot);
            if (from >= 0 && static_cast<unsigned>(from) < k_workspace_groups &&
                groups_[from].live) {
                if (was_active && groups_[from].count > 0)
                    if (workspace_record* a = find_mut(groups_[from].order[0]))
                        a->active = true;
                normalize(static_cast<unsigned>(from));
            }
            if (sink.workspace_moved)
                sink.workspace_moved(sink.user, back[i], from, static_cast<int>(slot));
        }
        for (unsigned i = 0; i < k_gone_outputs; ++i) {
            if (gone_[i].identifier[0] && std::strcmp(gone_[i].identifier, g.identifier) == 0) {
                g.pending_active = gone_[i].active_index;
                gone_[i] = {};
            }
        }
    }
    if (g.count == 0 && config_.dynamic) {
        if (workspace_record* w = create(static_cast<int>(slot)))
            g.order[g.count++] = w->id;
    }
    // Static: `count` of them, by position (normalize fills up).
    normalize(slot);
}

lx::workspace_id workspace_model::home_workspace(unsigned slot, unsigned index_1based) {
    if (slot >= k_workspace_groups || !groups_[slot].live)
        return {};
    workspace_group& g = groups_[slot];
    if (index_1based == 0)
        index_1based = 1;
    if (!config_.dynamic) {
        const unsigned i = index_1based <= g.count ? index_1based - 1 : g.count - 1;
        if (workspace_record* w = g.count ? find_mut(g.order[i]) : nullptr) {
            if (w->origin_index == 0)
                w->origin_index = i + 1;
            return w->id;
        }
        return {};
    }
    unsigned pos = g.count;
    for (unsigned i = 0; i < g.count; ++i) {
        const workspace_record* w = find(g.order[i]);
        if (!w || w->origin_index == 0)
            continue;
        if (w->origin_index == index_1based)
            return w->id;
        if (w->origin_index > index_1based && pos == g.count)
            pos = i;
    }
    // Before the trailing empty one (which has no origin) when it comes last.
    if (pos == g.count && g.count > 0) {
        const workspace_record* last = find(g.order[g.count - 1]);
        if (last && last->origin_index == 0 && last->windows == 0)
            pos = g.count - 1;
    }
    if (g.count >= k_workspaces_per_output) {
        const workspace_record* w = find(g.order[g.count - 1]);
        return w ? w->id : lx::workspace_id{};
    }
    workspace_record* w = create(static_cast<int>(slot));
    if (!w)
        return {};
    w->origin_index = index_1based;
    insert(slot, pos, w->id);
    rename(slot);
    return w->id;
}

void workspace_model::settle(unsigned slot) {
    if (slot >= k_workspace_groups || !groups_[slot].live)
        return;
    workspace_group& g = groups_[slot];
    if (g.pending_active) {
        lx::workspace_id want{};
        for (unsigned i = 0; i < g.count; ++i)
            if (const workspace_record* w = find(g.order[i]); w && w->origin_index == g.pending_active)
                want = w->id;
        if (want) {
            for (unsigned i = 0; i < g.count; ++i)
                if (workspace_record* w = find_mut(g.order[i]))
                    w->active = w->id == want;
            changed();
        }
        g.pending_active = 0;
    }
    for (unsigned i = 0; i < g.count; ++i)
        if (workspace_record* w = find_mut(g.order[i]))
            w->origin_index = 0;
    normalize(slot);
}

void workspace_model::output_removed(unsigned slot, int fallback, const workspace_sink& sink) {
    if (slot >= k_workspace_groups || !groups_[slot].live)
        return;
    workspace_group& g = groups_[slot];
    if (fallback >= 0 && (static_cast<unsigned>(fallback) >= k_workspace_groups ||
                          !groups_[fallback].live || static_cast<unsigned>(fallback) == slot))
        fallback = -1;
    unsigned active_pos = 0;
    for (unsigned i = 0; i < g.count; ++i)
        if (const workspace_record* w = find(g.order[i]); w && w->active)
            active_pos = i + 1;
    if (g.identifier[0]) {
        // Remembered for its return (the oldest entry gives way).
        unsigned at = k_gone_outputs;
        for (unsigned i = 0; i < k_gone_outputs; ++i)
            if (std::strcmp(gone_[i].identifier, g.identifier) == 0)
                at = i;
        if (at == k_gone_outputs)
            at = gone_next_++ % k_gone_outputs;
        copy_text(gone_[at].identifier, sizeof(gone_[at].identifier), g.identifier);
        gone_[at].active_index = active_pos;
    }
    lx::workspace_id order[k_workspaces_per_output];
    const unsigned n = g.count;
    for (unsigned i = 0; i < n; ++i)
        order[i] = g.order[i];
    char identifier[k_workspace_identifier_bytes];
    copy_text(identifier, sizeof(identifier), g.identifier);
    g = {};
    changed();

    if (fallback < 0) {
        // Nowhere to go: they wait for the next output, as the windows do.
        for (unsigned i = 0; i < n && stranded_count_ < k_max_workspaces; ++i) {
            stranded_[stranded_count_++] = order[i];
            if (workspace_record* w = find_mut(order[i])) {
                if (w->active)
                    stranded_active_ = w->id;
                w->output = -1;
            }
        }
        return;
    }
    const unsigned fb = static_cast<unsigned>(fallback);
    for (unsigned i = 0; i < n; ++i) {
        workspace_record* w = find_mut(order[i]);
        if (!w)
            continue;
        w->output = -1;
        if (w->windows == 0) {
            destroy(w->id);
            continue;
        }
        if (w->active) {
            // Visible windows stay visible: they join what the fallback shows.
            merge(w->id, active(fb), sink);
            continue;
        }
        if (!config_.dynamic) {
            // Static: into the fallback's workspace at the same position.
            workspace_group& f = groups_[fb];
            const unsigned at = i < f.count ? i : f.count - 1;
            merge(w->id, f.order[at], sink);
            continue;
        }
        workspace_group& f = groups_[fb];
        if (f.count >= k_workspaces_per_output) {
            merge(w->id, active(fb), sink);
            continue;
        }
        if (!w->home[0]) {
            copy_text(w->home, sizeof(w->home), identifier);
            w->home_index = i + 1;
        }
        // Before the trailing empty one.
        unsigned pos = f.count;
        if (pos > 0) {
            const workspace_record* last = find(f.order[pos - 1]);
            if (last && last->windows == 0 && !last->active)
                --pos;
        }
        insert(fb, pos, w->id);
    }
    normalize(fb);
}

lx::workspace_id workspace_model::active(unsigned slot) const {
    if (slot >= k_workspace_groups || !groups_[slot].live)
        return {};
    const workspace_group& g = groups_[slot];
    for (unsigned i = 0; i < g.count; ++i)
        if (const workspace_record* w = find(g.order[i]); w && w->active)
            return w->id;
    return g.count ? g.order[0] : lx::workspace_id{};
}

bool workspace_model::is_active(lx::workspace_id id) const {
    // A stranded workspace keeps its flag: what was shown stays shown while an output
    // configuration replaces the last output.
    const workspace_record* w = find(id);
    return w && w->active;
}

lx::workspace_id workspace_model::by_index(unsigned slot, unsigned index_1based) const {
    if (slot >= k_workspace_groups || !groups_[slot].live || index_1based == 0)
        return {};
    const workspace_group& g = groups_[slot];
    if (index_1based <= g.count)
        return g.order[index_1based - 1];
    return config_.dynamic && g.count ? g.order[g.count - 1] : lx::workspace_id{};
}

lx::workspace_id workspace_model::neighbour(lx::workspace_id id, int dir) const {
    const workspace_record* w = find(id);
    if (!w || w->output < 0 || dir == 0)
        return {};
    const workspace_group& g = groups_[static_cast<unsigned>(w->output)];
    const long long at = static_cast<long long>(w->index) + (dir > 0 ? 1 : -1);
    if (at < 0 || at >= static_cast<long long>(g.count))
        return {};
    return g.order[static_cast<unsigned>(at)];
}

bool workspace_model::activate(lx::workspace_id id) {
    workspace_record* w = find_mut(id);
    if (!w || w->output < 0 || w->active)
        return false;
    const unsigned slot = static_cast<unsigned>(w->output);
    workspace_group& g = groups_[slot];
    for (unsigned i = 0; i < g.count; ++i)
        if (workspace_record* o = find_mut(g.order[i]))
            o->active = o->id == id;
    changed();
    normalize(slot);
    return true;
}

void workspace_model::window_added(lx::workspace_id id) {
    workspace_record* w = find_mut(id);
    if (!w)
        return;
    ++w->windows;
    changed();
    if (w->output >= 0)
        normalize(static_cast<unsigned>(w->output));
}

void workspace_model::window_removed(lx::workspace_id id) {
    workspace_record* w = find_mut(id);
    if (!w || w->windows == 0)
        return;
    --w->windows;
    changed();
    if (w->output >= 0)
        normalize(static_cast<unsigned>(w->output));
}

bool workspace_model::assign(lx::workspace_id id, unsigned slot, const workspace_sink& sink) {
    workspace_record* w = find_mut(id);
    if (!w || w->output < 0 || slot >= k_workspace_groups || !groups_[slot].live ||
        static_cast<unsigned>(w->output) == slot ||
        groups_[slot].count >= k_workspaces_per_output)
        return false;
    const unsigned from = static_cast<unsigned>(w->output);
    // Its old output shows a neighbour now (the previous one, else the next).
    if (w->active) {
        lx::workspace_id next = neighbour(id, -1);
        if (!next)
            next = neighbour(id, 1);
        w->active = false;
        if (workspace_record* n = find_mut(next))
            n->active = true;
    }
    unlink(id);
    w->home[0] = '\0';
    w->home_index = 0;
    workspace_group& g = groups_[slot];
    unsigned pos = g.count;
    if (pos > 0) {
        const workspace_record* last = find(g.order[pos - 1]);
        if (last && last->windows == 0 && !last->active && config_.dynamic)
            --pos;
    }
    insert(slot, pos, id);
    if (sink.workspace_moved)
        sink.workspace_moved(sink.user, id, static_cast<int>(from), static_cast<int>(slot));
    normalize(from);
    normalize(slot);
    return true;
}

void workspace_model::set_focus_memory(lx::workspace_id id, lx::toplevel_id window) {
    if (workspace_record* w = find_mut(id))
        w->focus_memory = window;
}

lx::toplevel_id workspace_model::focus_memory(lx::workspace_id id) const {
    const workspace_record* w = find(id);
    return w ? w->focus_memory : lx::toplevel_id{};
}

void workspace_model::forget_window(lx::toplevel_id window) {
    for (unsigned i = 0; i < k_max_workspaces; ++i)
        if (used_slots_[i] && records_[i].focus_memory == window)
            records_[i].focus_memory = {};
}

void workspace_model::set_urgent(lx::workspace_id id, bool on) {
    workspace_record* w = find_mut(id);
    if (!w || w->urgent == on)
        return;
    w->urgent = on;
    changed();
}

void workspace_model::clear_home(lx::workspace_id id) {
    if (workspace_record* w = find_mut(id)) {
        w->home[0] = '\0';
        w->home_index = 0;
    }
}

} // namespace lx::compositor
