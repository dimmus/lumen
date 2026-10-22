module;

#include <stdio.h>

// The content-update queue of wl_surface.commit (wayland.xml since 1.24: "A commit request
// atomically creates a Content Update ... placed at the end of a per-surface queue"), and the
// readiness poller that clears its fence blockers. Wayland-free: it holds indices, blockers
// and fds; the wire (wire_surface.cpp) keeps each update's payload in a parallel pool. The
// bodies are in content_update.cpp (the Clang 23 partition rule, see session_lock.cpp).
//
// Model (design-app-compat-capture.md §3):
// - A commit of a surface that is not effectively synchronized makes a DCU. When the
//   surface's queue is empty and nothing blocks it, it is applied at once and never enters
//   this pool (the fast path, today's cost). Otherwise it takes a record here and waits.
// - Synchronized descendants' cached state (their merged SCUs) is *sealed* into the DCU as
//   child records, so the whole graph applies in one step.
// - Blockers: fence waits (explicit-sync acquire points, dma-buf implicit write fences),
//   counted per record and cleared by the poller; the fifo barrier and the commit-timing
//   target, which are conditions on the compositor's state and so evaluated when a queue is
//   tried (`cu_ready`), never stored as bits that could go stale.
// - Only the head of a surface's queue can apply: updates apply in commit order.

export module lx.compositor:content_update;

import std;

import lx.foundation;

export namespace lx::compositor {

/// Queued updates (with their sealed children) in all.
inline constexpr unsigned k_cu_pool = 512;
/// Surfaces a queue can belong to (the wire's surface table).
inline constexpr unsigned k_cu_surfaces = 256;
/// Queued DCUs per surface. Deep enough for a client that schedules seconds of frames ahead
/// with commit-timing (weston-simple-timing queues ~110); beyond it the newest two merge —
/// the older then carries the newer's content and waits for both sets of blockers, so
/// nothing is ever shown early, only a frame dropped (design-app-compat-capture Q5).
inline constexpr unsigned k_cu_per_surface = 128;
/// Queued records per client: one client cannot starve the others of the pool.
inline constexpr unsigned k_cu_per_client = 128;
/// Records only clients holding at most `k_cu_light` may take: several clients far ahead of
/// the display cannot drain the pool for one that queues a frame or two behind its fences
/// (an update with an unsignalled explicit-sync acquire point has nowhere else to wait).
inline constexpr unsigned k_cu_reserve = 64;
inline constexpr unsigned k_cu_light = 8;
/// commit-timing-v1: an update whose target lies at most this far after the predicted
/// presentation of the frame it would join is ready (absorbs flip-timestamp jitter; the
/// update is still never shown a refresh before its target).
inline constexpr unsigned long long k_cu_time_slack_ns = 500'000;

struct cu_record {
    /// Owner surface (wire slot + 1).
    unsigned surface = 0;
    /// Owner client id (for the per-client cap).
    unsigned client = 0;
    /// Outstanding fence waits of this record alone (poller entries).
    unsigned waits = 0;
    /// An explicit-sync acquire point of this update (or of a record sealed into it) has no
    /// poller entry (the poller was full, an eventfd could not be armed): it blocks the
    /// update like a wait, and is queried again until it signals or an entry is armed.
    bool recheck = false;
    /// Next DCU of the same surface, in commit order (0: last). DCUs only.
    unsigned next = 0;
    /// Sealed synchronized-descendant records (a flat list under the DCU, in tree preorder).
    unsigned first_child = 0;
    unsigned next_sibling = 0;
    /// The DCU this record was sealed into (0: it is a DCU).
    unsigned parent = 0;
    /// Bumped on every allocation: a poller wake or job naming an older generation is stale.
    unsigned generation = 0;
    /// commit-timing-v1 target, CLOCK_MONOTONIC ns (0: none).
    unsigned long long target_ns = 0;
    /// fifo-v1 wait_barrier.
    bool fifo_wait = false;
    bool used = false;
};

/// The pool and the per-surface queues. UI affinity; fixed storage, no allocation, O(1)
/// queue operations (the per-client count walks the queued records — only on the blocked
/// path).
class content_queue {
public:
    content_queue();

    /// A free record for `surface` (slot + 1) of `client`, or 0 when the pool is exhausted.
    [[nodiscard]] unsigned alloc(unsigned surface, unsigned client);
    /// Returns one record (and only that one: the caller frees sealed children itself).
    void release(unsigned cu);
    /// Appends a DCU to its surface's queue.
    void push(unsigned cu);
    /// Seals `child` (an SCU record) into DCU `parent`, after the children already there.
    void seal(unsigned parent, unsigned child);
    /// Takes a sealed record out of its DCU (its surface went away before the DCU applied).
    void unseal(unsigned child);
    /// Removes the head of `surface`'s queue and returns it (0: empty).
    unsigned pop(unsigned surface);

    [[nodiscard]] unsigned head(unsigned surface) const;
    [[nodiscard]] unsigned tail(unsigned surface) const;
    [[nodiscard]] unsigned depth(unsigned surface) const;
    /// Records (DCUs and sealed children) `client` holds.
    [[nodiscard]] unsigned client_count(unsigned client) const;
    [[nodiscard]] unsigned free_count() const { return free_count_; }
    /// Records in use (0: nothing queued anywhere, the common case).
    [[nodiscard]] unsigned used_count() const { return k_cu_pool - free_count_; }
    /// No fence wait left on `cu` or any record sealed into it.
    [[nodiscard]] bool fences_clear(unsigned cu) const;
    [[nodiscard]] cu_record& at(unsigned cu) { return pool_[cu <= k_cu_pool ? cu : 0]; }
    [[nodiscard]] const cu_record& at(unsigned cu) const { return pool_[cu <= k_cu_pool ? cu : 0]; }
    /// Surface slots + 1 with a non-empty queue, written to `out` (up to `cap`); the count.
    unsigned queued_surfaces(unsigned* out, unsigned cap) const;

private:
    cu_record pool_[k_cu_pool + 1]{}; // [0] is the null record
    unsigned free_next_[k_cu_pool + 1]{};
    unsigned free_head_ = 0;
    unsigned free_count_ = 0;
    unsigned head_[k_cu_surfaces + 1]{};
    unsigned tail_[k_cu_surfaces + 1]{};
    unsigned depth_[k_cu_surfaces + 1]{};
    std::uint64_t queued_mask_[(k_cu_surfaces + 1 + 63) / 64]{};
    unsigned generation_ = 0;
};

/// What the compositor knows about a queued update's surface right now.
struct cu_conditions {
    /// On screen on a lit output (not occluded, not off every output, not hidden by the
    /// lock, not paused, not dark). Hidden surfaces ignore fifo and timing (the protocols
    /// allow it; frame callbacks throttle them).
    bool shown = false;
    /// The surface's fifo barrier is set.
    bool barrier = false;
    /// When content applied now would first be presented (0: unknown — the target is then
    /// taken as reached rather than waited on forever).
    unsigned long long predicted_ns = 0;
};

/// Whether a client holding `held` records may take `need` more from a pool with `free`
/// left: never past `k_cu_per_client`, and past `k_cu_light` only while `k_cu_reserve`
/// records stay free for the clients that hold few.
[[nodiscard]] bool cu_room_for(unsigned free, unsigned held, unsigned need);

/// A commit-timing target is reached when it lies no further than `k_cu_time_slack_ns`
/// after `predicted_ns`.
[[nodiscard]] bool cu_time_reached(unsigned long long target_ns, unsigned long long predicted_ns);
/// The update may apply now: its fences (and its sealed children's) have signalled, and —
/// for a shown surface — no fifo barrier holds it and its target time is reached.
[[nodiscard]] bool cu_ready(const cu_record& r, bool fences_clear, const cu_conditions& c);

/// Waits on fence fds without blocking the UI thread: one epoll fd the event loop watches as
/// a single source (the loop has a fixed number of sources; a fence each would exhaust it).
/// Each entry owns its fd (a sync_file or an eventfd) and names the update it blocks, with
/// that record's generation so a wake for a reused record is ignored. Fixed storage.
class readiness_poller {
public:
    static constexpr unsigned k_max = 384;
    /// Entries content updates may not take: other units' waits (a screen capture's copy
    /// fence, one per capture session at most) always find one.
    static constexpr unsigned k_reserved = 64;
    struct wake {
        unsigned cu = 0;
        unsigned generation = 0;
    };

    readiness_poller() = default;
    ~readiness_poller();
    readiness_poller(const readiness_poller&) = delete;
    readiness_poller& operator=(const readiness_poller&) = delete;

    [[nodiscard]] lx::result<void> open();
    [[nodiscard]] int fd() const { return epfd_; }
    /// Takes ownership of `fd` (closed in every path). False when no entry is free (outside
    /// the reserve unless `reserved`) or epoll refuses it: the caller must not wait on it
    /// then.
    [[nodiscard]] bool add(int fd, unsigned cu, unsigned generation, bool reserved = false);
    /// Entries `add` can still hand out (to content updates unless `reserved`): content
    /// updates hold at most `k_max - k_reserved` whatever other units hold.
    [[nodiscard]] unsigned available(bool reserved = false) const {
        const unsigned total = used_ < k_max ? k_max - used_ : 0;
        if (reserved)
            return total;
        const unsigned mine = used_ - reserved_used_;
        const unsigned share = mine < k_max - k_reserved ? k_max - k_reserved - mine : 0;
        return share < total ? share : total;
    }
    /// Drops (closes) every wait of `cu`; returns how many there were.
    unsigned drop(unsigned cu);
    /// The signalled waits (each closed and removed), up to `cap`; their count.
    unsigned drain(wake* out, unsigned cap);
    [[nodiscard]] unsigned in_use() const { return used_; }

private:
    struct entry {
        int fd = -1;
        unsigned cu = 0;
        unsigned generation = 0;
        bool reserved = false;
    };
    void remove(unsigned index);
    entry entries_[k_max]{};
    unsigned used_ = 0;
    /// Of `used_`, the entries added with `reserved`.
    unsigned reserved_used_ = 0;
    int epfd_ = -1;
};

} // namespace lx::compositor
