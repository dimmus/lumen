module;

// wp_presentation feedback bookkeeping (presentation-time, stable, v2): which content update
// each wp_presentation_feedback object belongs to, when it was sampled into a frame, and
// which flip presented it — or why it was discarded. Pure and Wayland-free: resources are
// opaque pointers, events leave through a `feedback_sink` the wire unit implements
// (wire_presentation.cpp). UI affinity throughout.
//
// The model (design P1, the wlroots "sampled → output commit → present" chain keyed by
// frame index):
//   pending     created on the surface's pending state (a per-update list)
//   queued      its update became current (`on_apply`); waiting to be on screen
//   sampled(N)  published frame N showed the surface (`on_publish`, the visibility pass)
//   superseded  sampled, then a newer update of the surface became current at frame M
// A content flip of frame F presents a sampled feedback with N <= F only when its surface is
// on screen in F itself: the render thread composites the newest snapshot, so frames
// between N and F may never have been shown, and F may cover or move the surface. Every
// publish after N records the surface's visibility (a 64-frame bitmask per slot). If the
// newer update was already in F (superseded at M <= F), the feedback was never seen and is
// discarded. A surface hidden in F keeps its feedback for a later flip that shows it, until
// a newer update supersedes it (wlroots keeps unrendered feedback the same way). A queued
// update replaced before it was sampled is discarded at once.

#include <initializer_list>

export module lx.compositor:presentation_feedback;

import lx.foundation;

export namespace lx::compositor {

/// wp_presentation_feedback.kind bits.
inline constexpr unsigned k_present_vsync = 0x1;
inline constexpr unsigned k_present_hw_clock = 0x2;
inline constexpr unsigned k_present_hw_completion = 0x4;
inline constexpr unsigned k_present_zero_copy = 0x8;

/// What one presentation reports (wp_presentation_feedback.presented).
struct present_info {
    /// CLOCK_MONOTONIC (wp_presentation.clock_id).
    unsigned long long timestamp_ns = 0;
    /// The output's refresh period; 0 when unknown.
    unsigned refresh_ns = 0;
    /// The output's vertical retrace counter (MSC); 0 when there is none.
    unsigned long long seq = 0;
    unsigned flags = 0;
};

/// Where the table's events go. Called after the slot is already free, so a sink that
/// destroys the resource (whose destructor calls `forget_resource`) finds nothing left.
struct feedback_sink {
    void (*presented)(void* user, void* resource, lx::surface_id surface,
                      const present_info& info) = nullptr;
    void (*discarded)(void* user, void* resource) = nullptr;
    void* user = nullptr;
};

/// Whether `surface` was on screen in published frame `frame` (the scene's visibility pass).
using surface_visible_fn = bool (*)(void* user, lx::surface_id surface, unsigned frame);

class presentation_feedback_table {
public:
    /// Live feedback objects across all clients.
    static constexpr unsigned k_max = 1024;
    /// Feedback objects one content update may carry; more are discarded at creation.
    static constexpr unsigned k_max_per_update = 16;
    /// A per-update list (a surface's pending or cached state): index + 1 of its first
    /// slot, 0 when empty. Owned by the caller's surface state.
    using list = unsigned;

    enum class state : unsigned char { free, pending, queued, sampled, superseded };

    presentation_feedback_table();

    /// A new feedback for the update being built in `pending`. Returns its handle (index +
    /// 1), kept as the resource's user data for `forget_resource`; 0 when the table or the
    /// update is full — the caller discards the object at once.
    [[nodiscard]] unsigned create(void* resource, lx::surface_id surface, list& pending);
    /// wl_surface.commit, pending → cached. A cached update still waiting (a synchronized
    /// subsurface) is replaced by this one, never applied: its feedback is discarded.
    void on_merge(list& pending, list& cached, const feedback_sink& sink);
    /// cached → current: the update becomes the surface's content in snapshot
    /// `building_frame` and its feedback is queued. The update it replaces: queued feedback
    /// is discarded (never sampled), sampled feedback becomes superseded at `building_frame`.
    void on_apply(lx::surface_id surface, list& cached, unsigned building_frame,
                  const feedback_sink& sink);
    /// Frame `frame` was published: queued feedback whose surface it showed is sampled, and
    /// sampled feedback records whether its surface is on screen in `frame`.
    void on_publish(unsigned frame, surface_visible_fn visible, void* user);
    /// A content flip of the composite of frame `frame` completed (see the class comment).
    void on_flip(unsigned frame, const present_info& info, const feedback_sink& sink);
    /// The surface is destroyed or unmapped: its queued, sampled and superseded feedback is
    /// discarded (its pending and cached lists stay with the surface state — see
    /// `discard_list`).
    void on_surface_gone(lx::surface_id surface, const feedback_sink& sink);
    /// Discards every feedback of one per-update list (a destroyed surface's pending and
    /// cached state) and empties it.
    void discard_list(list& l, const feedback_sink& sink);
    /// The display stopped (VT switched away, powered off): nothing in flight will be
    /// presented. Queued, sampled and superseded feedback is discarded.
    void discard_unpresented(const feedback_sink& sink);
    /// While stopped (the 1 Hz paused tick): queued feedback is discarded, so a client
    /// pacing by it falls back to its own timer instead of waiting for a display that is off.
    void discard_queued(const feedback_sink& sink);
    /// The resource's destructor (client disconnect): the slot forgets it. A pending slot
    /// stays linked in its list as dead and is dropped silently when the list is next
    /// processed; a queued or sampled one is freed now. No-op when `handle` no longer names
    /// `resource` (the table freed it already).
    void forget_resource(unsigned handle, void* resource);

    [[nodiscard]] unsigned in_use() const { return in_use_; }
    [[nodiscard]] unsigned count(state s) const;
    /// Feedback refused because the table or its update was full.
    [[nodiscard]] unsigned overflows() const { return overflows_; }

private:
    struct slot {
        void* resource = nullptr;
        lx::surface_id surface{};
        unsigned sampled_frame = 0;
        unsigned superseded_frame = 0;
        /// Visibility since sampling: bit k is "on screen in frame `seen_base` + k". Frames
        /// before `seen_base` were dropped from the window and count as not shown.
        unsigned long long seen_mask = 0;
        unsigned seen_base = 0;
        /// The newest frame whose visibility was recorded; later frames (published while
        /// nothing was recorded) carry its answer forward.
        unsigned seen_last = 0;
        /// Per-update list (pending) or free list: index + 1, 0 = end.
        unsigned next = 0;
        /// Global list (queued or sampled/superseded): index + 1, 0 = end.
        unsigned gprev = 0;
        unsigned gnext = 0;
        state st = state::free;
    };

    [[nodiscard]] unsigned alloc();
    /// Whether the slot's surface was on screen in frame `frame` (sampled or superseded).
    [[nodiscard]] bool shown_in(const slot& s, unsigned frame) const;
    /// Records the slot's visibility in published frame `frame`.
    static void note_seen(slot& s, unsigned frame, bool visible);
    void release(unsigned idx);
    void link(unsigned& head, unsigned idx);
    void unlink(unsigned& head, unsigned idx);
    /// Frees the slot, then reports it discarded (when it still has a resource).
    void discard(unsigned idx, const feedback_sink& sink);

    slot slots_[k_max]{};
    unsigned free_head_ = 0;
    unsigned queued_head_ = 0;
    unsigned sampled_head_ = 0;
    unsigned in_use_ = 0;
    unsigned overflows_ = 0;
};

} // namespace lx::compositor

namespace lx::compositor {

namespace {

/// `a` is at or after `b`, on a wrapping frame counter.
[[nodiscard]] bool at_or_after(unsigned a, unsigned b) {
    return static_cast<int>(a - b) >= 0;
}

} // namespace

presentation_feedback_table::presentation_feedback_table() {
    for (unsigned i = 0; i < k_max; ++i)
        slots_[i].next = i + 1 < k_max ? i + 2 : 0;
    free_head_ = 1;
}

unsigned presentation_feedback_table::alloc() {
    if (!free_head_)
        return 0;
    const unsigned h = free_head_;
    slot& s = slots_[h - 1];
    free_head_ = s.next;
    s = {};
    ++in_use_;
    return h;
}

void presentation_feedback_table::release(unsigned idx) {
    slot& s = slots_[idx];
    s = {};
    s.next = free_head_;
    free_head_ = idx + 1;
    --in_use_;
}

void presentation_feedback_table::link(unsigned& head, unsigned idx) {
    slot& s = slots_[idx];
    s.gprev = 0;
    s.gnext = head;
    if (head)
        slots_[head - 1].gprev = idx + 1;
    head = idx + 1;
}

void presentation_feedback_table::unlink(unsigned& head, unsigned idx) {
    slot& s = slots_[idx];
    if (s.gprev)
        slots_[s.gprev - 1].gnext = s.gnext;
    else
        head = s.gnext;
    if (s.gnext)
        slots_[s.gnext - 1].gprev = s.gprev;
    s.gprev = s.gnext = 0;
}

void presentation_feedback_table::discard(unsigned idx, const feedback_sink& sink) {
    void* resource = slots_[idx].resource;
    release(idx);
    if (resource && sink.discarded)
        sink.discarded(sink.user, resource);
}

unsigned presentation_feedback_table::create(void* resource, lx::surface_id surface,
                                             list& pending) {
    unsigned length = 0;
    unsigned tail = 0;
    for (unsigned h = pending; h; h = slots_[h - 1].next) {
        ++length;
        tail = h;
    }
    if (length >= k_max_per_update) {
        ++overflows_;
        return 0;
    }
    const unsigned h = alloc();
    if (!h) {
        ++overflows_;
        return 0;
    }
    slot& s = slots_[h - 1];
    s.resource = resource;
    s.surface = surface;
    s.st = state::pending;
    // Appended: feedback of one update is reported in the order it was requested.
    if (tail)
        slots_[tail - 1].next = h;
    else
        pending = h;
    return h;
}

void presentation_feedback_table::discard_list(list& l, const feedback_sink& sink) {
    unsigned h = l;
    l = 0;
    while (h) {
        const unsigned next = slots_[h - 1].next;
        discard(h - 1, sink);
        h = next;
    }
}

void presentation_feedback_table::on_merge(list& pending, list& cached,
                                           const feedback_sink& sink) {
    if (cached)
        discard_list(cached, sink);
    cached = pending;
    pending = 0;
}

void presentation_feedback_table::on_apply(lx::surface_id surface, list& cached,
                                           unsigned building_frame, const feedback_sink& sink) {
    // The update this one replaces. Only the surface's feedback is in these lists; they hold
    // what is in flight, which is a handful.
    for (unsigned h = queued_head_; h;) {
        const unsigned next = slots_[h - 1].gnext;
        if (slots_[h - 1].surface == surface) {
            unlink(queued_head_, h - 1);
            discard(h - 1, sink);
        }
        h = next;
    }
    for (unsigned h = sampled_head_; h; h = slots_[h - 1].gnext) {
        slot& s = slots_[h - 1];
        if (s.surface == surface && s.st == state::sampled) {
            s.st = state::superseded;
            s.superseded_frame = building_frame;
        }
    }
    unsigned h = cached;
    cached = 0;
    while (h) {
        const unsigned next = slots_[h - 1].next;
        slot& s = slots_[h - 1];
        s.next = 0;
        if (!s.resource) {
            // Its client destroyed it (disconnect) while it was pending.
            release(h - 1);
        } else {
            s.st = state::queued;
            link(queued_head_, h - 1);
        }
        h = next;
    }
}

void presentation_feedback_table::note_seen(slot& s, unsigned frame, bool visible) {
    if (!at_or_after(frame, s.seen_base))
        return;
    unsigned d = frame - s.seen_base;
    if (d >= 64) {
        // Slide the window: the oldest frames are long past any flip still to come.
        const unsigned shift = d - 63;
        s.seen_mask = shift >= 64 ? 0 : s.seen_mask >> shift;
        s.seen_base += shift;
        d = 63;
    }
    if (visible)
        s.seen_mask |= 1ull << d;
    else
        s.seen_mask &= ~(1ull << d);
    s.seen_last = frame;
}

bool presentation_feedback_table::shown_in(const slot& s, unsigned frame) const {
    if (!at_or_after(frame, s.seen_base))
        return false;
    // Not recorded since (published while stopped, or not at all yet): as last recorded.
    if (at_or_after(frame, s.seen_last))
        frame = s.seen_last;
    const unsigned d = frame - s.seen_base;
    return d < 64 && ((s.seen_mask >> d) & 1ull) != 0;
}

void presentation_feedback_table::on_publish(unsigned frame, surface_visible_fn visible,
                                             void* user) {
    if (!visible)
        return;
    // Already sampled: whether this frame still shows the surface. A superseded update is
    // not the surface's content from its superseding frame on; nothing to record there.
    for (unsigned h = sampled_head_; h; h = slots_[h - 1].gnext) {
        slot& s = slots_[h - 1];
        if (s.st == state::superseded && at_or_after(frame, s.superseded_frame))
            continue;
        note_seen(s, frame, visible(user, s.surface, frame));
    }
    for (unsigned h = queued_head_; h;) {
        const unsigned next = slots_[h - 1].gnext;
        slot& s = slots_[h - 1];
        if (visible(user, s.surface, frame)) {
            unlink(queued_head_, h - 1);
            s.st = state::sampled;
            s.sampled_frame = frame;
            s.seen_base = frame;
            s.seen_last = frame;
            s.seen_mask = 1;
            link(sampled_head_, h - 1);
        }
        h = next;
    }
}

void presentation_feedback_table::on_flip(unsigned frame, const present_info& info,
                                          const feedback_sink& sink) {
    for (unsigned h = sampled_head_; h;) {
        const unsigned next = slots_[h - 1].gnext;
        slot& s = slots_[h - 1];
        if (s.st == state::superseded && at_or_after(frame, s.superseded_frame)) {
            // The newer update was already in this frame: this one never reached the screen
            // after its sampling (or it would have been presented by that flip).
            unlink(sampled_head_, h - 1);
            discard(h - 1, sink);
        } else if (at_or_after(frame, s.sampled_frame) && shown_in(s, frame)) {
            // On screen in the frame that flipped. One sampled earlier but hidden in this
            // frame (covered, moved off the output) waits for a flip that shows it.
            void* resource = s.resource;
            const lx::surface_id surface = s.surface;
            unlink(sampled_head_, h - 1);
            release(h - 1);
            if (sink.presented)
                sink.presented(sink.user, resource, surface, info);
        }
        h = next;
    }
}

void presentation_feedback_table::on_surface_gone(lx::surface_id surface,
                                                  const feedback_sink& sink) {
    for (unsigned* head : {&queued_head_, &sampled_head_}) {
        for (unsigned h = *head; h;) {
            const unsigned next = slots_[h - 1].gnext;
            if (slots_[h - 1].surface == surface) {
                unlink(*head, h - 1);
                discard(h - 1, sink);
            }
            h = next;
        }
    }
}

void presentation_feedback_table::discard_unpresented(const feedback_sink& sink) {
    discard_queued(sink);
    while (sampled_head_) {
        const unsigned idx = sampled_head_ - 1;
        unlink(sampled_head_, idx);
        discard(idx, sink);
    }
}

void presentation_feedback_table::discard_queued(const feedback_sink& sink) {
    while (queued_head_) {
        const unsigned idx = queued_head_ - 1;
        unlink(queued_head_, idx);
        discard(idx, sink);
    }
}

void presentation_feedback_table::forget_resource(unsigned handle, void* resource) {
    if (!handle || handle > k_max || !resource)
        return;
    const unsigned idx = handle - 1;
    slot& s = slots_[idx];
    if (s.st == state::free || s.resource != resource)
        return;
    switch (s.st) {
    case state::pending:
        // Linked in a per-update list only its surface state knows: dropped when that list
        // is applied, merged over or discarded.
        s.resource = nullptr;
        return;
    case state::queued:
        unlink(queued_head_, idx);
        break;
    case state::sampled:
    case state::superseded:
        unlink(sampled_head_, idx);
        break;
    case state::free:
        return;
    }
    release(idx);
}

unsigned presentation_feedback_table::count(state st) const {
    unsigned n = 0;
    for (const slot& s : slots_)
        if (s.st == st)
            ++n;
    return n;
}

} // namespace lx::compositor
