module;

import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.wayland.server;

export module lx.compositor:buffer_lifecycle;

export namespace lx::compositor {

enum class buffer_lifecycle_state {
    none,
    imported,
    attached,
    in_flight,
    pending_release,
    released,
};

struct buffer_lifecycle_record {
    lx::client_id client{};
    lx::surface_id surface{};
    lx::buffer_id buffer{};
    lx::gfx::import_cache_key cache_key{};
    buffer_lifecycle_state state = buffer_lifecycle_state::none;
    unsigned frame_imported = 0;
    unsigned frame_last_used = 0;
    /// Opaque wl_resource* for the client buffer; released only from flush_pending_releases.
    void* wl_buffer = nullptr;
    /// The buffer is its surface's content: every later frame samples it again, so it is
    /// not released until a newer commit (or the surface's end) supersedes it.
    bool current = false;
    /// An explicit-sync release point the record took over from a destroyed wl_buffer
    /// (timeline 0: none), signalled when the record is released.
    unsigned sync_timeline = 0;
    unsigned long long sync_point = 0;
    /// Was current until the last `on_detach` of its surface: what `hold` keeps.
    bool detached = false;
    /// A closing window's ghost still draws it (`hold`): current until `release_hold`.
    bool held = false;
};

using wl_buffer_release_fn = void (*)(void* wl_buffer_resource, void* user_data);
/// Settles a release point a record took over (`on_buffer_destroyed`): `signal` when the
/// compositor is done with the buffer, otherwise only dropped (its client is gone).
using sync_release_fn = void (*)(unsigned timeline, unsigned long long point, bool signal,
                                 void* user_data);

/// Tracks import → use → release → wl_buffer.release → cache evict.
///
/// A dma-buf is sampled zero-copy (an EGLImage or VkImage over the client's memory) by every
/// composite that shows it, not only by the first: it stays *current* — never released —
/// while it is its surface's content. Once a newer commit or the surface's end supersedes
/// it, it is released when the render thread has completed the last frame that could
/// sample it (wlroots, Mutter and KWin hold a client buffer the same way).
///
/// **Thread affinity: UI only.** Every method mutates the record table and may send
/// `wl_buffer.release`, so it must run on the thread that owns Wayland dispatch. The render
/// thread never touches the tracker; it publishes a completed frame index that the UI
/// thread feeds to `retire_completed_frames()`.
class buffer_lifecycle_tracker {
public:
    explicit buffer_lifecycle_tracker(gfx::dmabuf_import_cache& cache);

    void set_release_callback(wl_buffer_release_fn fn, void* user_data = nullptr);
    void set_sync_release_callback(sync_release_fn fn, void* user_data = nullptr);

    [[nodiscard]] lx::result<void> on_import(lx::client_id client, lx::surface_id surface,
                                             lx::buffer_id buffer,
                                             const gfx::import_cache_key& key,
                                             void* wl_buffer);
    void on_attach(lx::surface_id surface);
    /// `surface` stops showing the dma-buf it has (a new buffer, a null attach, the
    /// surface's end): its records are released once the frames tagged on them complete.
    void on_detach(lx::surface_id surface);
    /// A closing window's ghost (window animations) keeps drawing what `surface` showed last
    /// although the surface no longer shows it (a null attach, its end): the dma-buf that was
    /// current before its last `on_detach` stays current — sampled, never released — until
    /// `release_hold`. A later commit of another buffer does not supersede it either; the
    /// same buffer committed again is the surface's once more (no longer held).
    void hold(lx::surface_id surface);
    /// The ghost is gone: what `hold` kept is released once the frames that sample it have
    /// completed, as `on_detach` would have.
    void release_hold(lx::surface_id surface);
    void on_present(lx::surface_id surface, unsigned frame_index);
    void on_render_done(lx::surface_id surface, unsigned frame_index);

    /// Mark every attached buffer in-flight, tagged with the frame whose snapshot will
    /// sample it; current buffers are tagged again (the snapshot samples them too). Call
    /// right after a snapshot for `frame_index` is published — an output's or a capture
    /// view's.
    void on_frame_submitted(unsigned frame_index);

    /// Release every superseded buffer the render thread has finished with, i.e. those
    /// tagged with a frame at or before `completed_frame`. Sends wl_buffer.release and drops
    /// the import cache reference.
    [[nodiscard]] lx::result<void> retire_completed_frames(long long completed_frame);

    /// Sends wl_buffer.release when safe — decrements import cache use_count.
    [[nodiscard]] lx::result<void> flush_pending_releases();

    /// The client destroyed the wl_buffer (explicitly or by disconnecting). Its records
    /// forget the resource (no wl_buffer.release is sent to it) but stay until no frame can
    /// sample the import any more; the first live one takes over the explicit-sync release
    /// point `sync_timeline`/`sync_point` (0: none), settled through the sync callback.
    /// True when a record took it — the caller must then neither signal nor drop it.
    [[nodiscard]] bool on_buffer_destroyed(void* wl_buffer, unsigned sync_timeline = 0,
                                           unsigned long long sync_point = 0);

    void on_client_disconnect(lx::client_id client);
    void on_memory_pressure(lx::runtime::memory_pressure level);

    [[nodiscard]] unsigned pending_release_count() const;
    /// A buffer is attached, or superseded and in flight: it is released only by a later
    /// frame, so the frame clock must keep running until it has been. A current buffer
    /// waits for a commit, not a frame.
    [[nodiscard]] bool has_unreleased_frames() const;
    [[nodiscard]] buffer_lifecycle_state state_for(lx::surface_id surface) const;

private:
    [[nodiscard]] int find_surface(lx::surface_id surface) const;
    [[nodiscard]] int find_record(lx::surface_id surface, lx::buffer_id buffer) const;
    [[nodiscard]] int alloc_record();

    gfx::dmabuf_import_cache* cache_ = nullptr;
    wl_buffer_release_fn release_fn_ = nullptr;
    void* release_user_ = nullptr;
    sync_release_fn sync_fn_ = nullptr;
    void* sync_user_ = nullptr;
    /// Releases record `i` now: wl_buffer.release (or its taken-over release point), the
    /// import cache reference.
    void release_record(unsigned i);
    static constexpr unsigned k_capacity = 1024;
    buffer_lifecycle_record records_[1024]{};
    unsigned count_ = 0;
};

} // namespace lx::compositor


int lx::compositor::buffer_lifecycle_tracker::find_surface(lx::surface_id surface) const {
    for (unsigned i = 0; i < count_; ++i) {
        if (records_[i].surface == surface &&
            records_[i].state != buffer_lifecycle_state::none &&
            records_[i].state != buffer_lifecycle_state::released)
            return static_cast<int>(i);
    }
    return -1;
}

int lx::compositor::buffer_lifecycle_tracker::find_record(lx::surface_id surface,
                                                          lx::buffer_id buffer) const {
    for (unsigned i = 0; i < count_; ++i) {
        if (records_[i].surface == surface && records_[i].buffer == buffer &&
            records_[i].state != buffer_lifecycle_state::none &&
            records_[i].state != buffer_lifecycle_state::released)
            return static_cast<int>(i);
    }
    return -1;
}

/// Released records are reusable, so a long-lived client cannot exhaust the table by
/// cycling buffers.
int lx::compositor::buffer_lifecycle_tracker::alloc_record() {
    for (unsigned i = 0; i < count_; ++i) {
        if (records_[i].state == buffer_lifecycle_state::none ||
            records_[i].state == buffer_lifecycle_state::released)
            return static_cast<int>(i);
    }
    if (count_ >= k_capacity)
        return -1;
    return static_cast<int>(count_++);
}

lx::compositor::buffer_lifecycle_tracker::buffer_lifecycle_tracker(gfx::dmabuf_import_cache& cache)
    : cache_{&cache} {}

void lx::compositor::buffer_lifecycle_tracker::set_release_callback(wl_buffer_release_fn fn,
                                                                    void* user_data) {
    release_fn_ = fn;
    release_user_ = user_data;
}

void lx::compositor::buffer_lifecycle_tracker::set_sync_release_callback(sync_release_fn fn,
                                                                         void* user_data) {
    sync_fn_ = fn;
    sync_user_ = user_data;
}

void lx::compositor::buffer_lifecycle_tracker::release_record(unsigned i) {
    buffer_lifecycle_record& rec = records_[i];
    if (rec.wl_buffer) {
        if (release_fn_)
            release_fn_(rec.wl_buffer, release_user_);
    } else if (rec.sync_timeline && sync_fn_) {
        sync_fn_(rec.sync_timeline, rec.sync_point, true, sync_user_);
    }
    rec.wl_buffer = nullptr;
    rec.sync_timeline = 0;
    rec.sync_point = 0;
    rec.current = false;
    rec.detached = false;
    rec.held = false;
    if (cache_)
        cache_->release(rec.cache_key);
    rec.state = buffer_lifecycle_state::released;
}

lx::result<void> lx::compositor::buffer_lifecycle_tracker::on_import(
    lx::client_id client, lx::surface_id surface, lx::buffer_id buffer,
    const gfx::import_cache_key& key, void* wl_buffer) {
    // Whatever else the surface showed is superseded by this buffer (a ghost's hold aside).
    for (unsigned i = 0; i < count_; ++i)
        if (records_[i].surface == surface && records_[i].buffer != buffer) {
            if (!records_[i].held)
                records_[i].current = false;
            records_[i].detached = false;
        }
    // Re-committing the same buffer refreshes the existing record. A *different* buffer
    // gets its own record: the previously attached one may still be in flight, and
    // dropping its wl_buffer here would mean the client never receives its release.
    if (const int existing = find_record(surface, buffer); existing >= 0) {
        auto& rec = records_[static_cast<unsigned>(existing)];
        rec.client = client;
        rec.cache_key = key;
        rec.wl_buffer = wl_buffer;
        rec.current = true;
        // The surface shows it again: no ghost's hold may end it while it is on screen.
        rec.held = false;
        rec.detached = false;
        // In flight already, it stays so (tagged with the frames that sample it); fresh
        // content goes out with the next published frame.
        if (rec.state != buffer_lifecycle_state::in_flight)
            rec.state = buffer_lifecycle_state::imported;
        return {};
    }

    const int slot = alloc_record();
    if (slot < 0)
        return lx::invalid_argument("lifecycle tracker full");
    buffer_lifecycle_record rec{};
    rec.client = client;
    rec.surface = surface;
    rec.buffer = buffer;
    rec.cache_key = key;
    rec.state = buffer_lifecycle_state::imported;
    rec.wl_buffer = wl_buffer;
    rec.current = true;
    records_[static_cast<unsigned>(slot)] = rec;
    return {};
}

void lx::compositor::buffer_lifecycle_tracker::on_attach(lx::surface_id surface) {
    // Promote the freshly imported record only; an older buffer for the same surface stays
    // in flight until the render thread is done with it.
    for (unsigned i = 0; i < count_; ++i) {
        if (records_[i].surface == surface &&
            records_[i].state == buffer_lifecycle_state::imported)
            records_[i].state = buffer_lifecycle_state::attached;
    }
}

void lx::compositor::buffer_lifecycle_tracker::on_detach(lx::surface_id surface) {
    for (unsigned i = 0; i < count_; ++i)
        if (records_[i].surface == surface) {
            records_[i].detached = records_[i].current;
            if (!records_[i].held)
                records_[i].current = false;
        }
}

void lx::compositor::buffer_lifecycle_tracker::hold(lx::surface_id surface) {
    for (unsigned i = 0; i < count_; ++i) {
        auto& rec = records_[i];
        if (rec.surface != surface || rec.state == buffer_lifecycle_state::none ||
            rec.state == buffer_lifecycle_state::released || !(rec.current || rec.detached))
            continue;
        rec.held = true;
        rec.current = true;
    }
}

void lx::compositor::buffer_lifecycle_tracker::release_hold(lx::surface_id surface) {
    for (unsigned i = 0; i < count_; ++i) {
        auto& rec = records_[i];
        if (rec.surface != surface || !rec.held)
            continue;
        rec.held = false;
        rec.current = false;
        rec.detached = false;
    }
}

void lx::compositor::buffer_lifecycle_tracker::on_present(lx::surface_id surface,
                                                            unsigned frame_index) {
    if (const int slot = find_surface(surface); slot >= 0) {
        auto& rec = records_[static_cast<unsigned>(slot)];
        rec.state = buffer_lifecycle_state::in_flight;
        rec.frame_last_used = frame_index;
    }
}

void lx::compositor::buffer_lifecycle_tracker::on_render_done(lx::surface_id surface,
                                                              unsigned frame_index) {
    if (const int slot = find_surface(surface); slot >= 0) {
        auto& rec = records_[static_cast<unsigned>(slot)];
        rec.state = buffer_lifecycle_state::pending_release;
        rec.frame_last_used = frame_index;
    }
}

void lx::compositor::buffer_lifecycle_tracker::on_frame_submitted(unsigned frame_index) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    for (unsigned i = 0; i < count_; ++i) {
        auto& rec = records_[i];
        const bool tag = rec.state == buffer_lifecycle_state::attached ||
                         (rec.current && rec.state == buffer_lifecycle_state::in_flight);
        if (!tag)
            continue;
        rec.state = buffer_lifecycle_state::in_flight;
        // Never backwards: a capture view and an output publish the same frame index.
        if (static_cast<int>(frame_index - rec.frame_last_used) > 0 ||
            rec.frame_last_used == 0)
            rec.frame_last_used = frame_index;
    }
}

lx::result<void>
lx::compositor::buffer_lifecycle_tracker::retire_completed_frames(long long completed_frame) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (completed_frame < 0)
        return {};
    for (unsigned i = 0; i < count_; ++i) {
        if (records_[i].state != buffer_lifecycle_state::in_flight || records_[i].current)
            continue;
        if (static_cast<long long>(records_[i].frame_last_used) > completed_frame)
            continue;
        records_[i].state = buffer_lifecycle_state::pending_release;
    }
    return flush_pending_releases();
}

lx::result<void> lx::compositor::buffer_lifecycle_tracker::flush_pending_releases() {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    for (unsigned i = 0; i < count_; ++i)
        if (records_[i].state == buffer_lifecycle_state::pending_release)
            release_record(i);
    return {};
}

bool lx::compositor::buffer_lifecycle_tracker::on_buffer_destroyed(void* wl_buffer,
                                                                   unsigned sync_timeline,
                                                                   unsigned long long sync_point) {
    if (!wl_buffer)
        return false;
    bool adopted = false;
    for (unsigned i = 0; i < count_; ++i) {
        auto& rec = records_[i];
        if (rec.wl_buffer != wl_buffer)
            continue;
        rec.wl_buffer = nullptr;
        if (rec.state == buffer_lifecycle_state::none ||
            rec.state == buffer_lifecycle_state::released)
            continue;
        // Its import is still what the surface shows, or what a submitted frame reads: the
        // record stays until that is over, and settles the release point then.
        if (sync_timeline && !adopted) {
            rec.sync_timeline = sync_timeline;
            rec.sync_point = sync_point;
            adopted = true;
        }
    }
    return adopted;
}

void lx::compositor::buffer_lifecycle_tracker::on_client_disconnect(lx::client_id client) {
    if (cache_) cache_->evict_client(client);
    for (unsigned i = 0; i < count_; ++i) {
        auto& rec = records_[i];
        if (rec.client != client)
            continue;
        // The wl_resource dies with the client, so it must not be released — just forget it.
        if (rec.sync_timeline && sync_fn_)
            sync_fn_(rec.sync_timeline, rec.sync_point, false, sync_user_);
        rec.sync_timeline = 0;
        rec.sync_point = 0;
        rec.wl_buffer = nullptr;
        rec.current = false;
        rec.state = buffer_lifecycle_state::released;
    }
}

void lx::compositor::buffer_lifecycle_tracker::on_memory_pressure(
    lx::runtime::memory_pressure level) {
    if (level == lx::runtime::memory_pressure::normal) return;
    for (unsigned i = 0; i < count_; ++i) {
        // A current buffer is on screen: releasing it frees nothing (the import stays) and
        // would let the client write into what the next composite reads.
        if (records_[i].current)
            continue;
        if (records_[i].state == buffer_lifecycle_state::pending_release ||
            (level == lx::runtime::memory_pressure::critical &&
             records_[i].state == buffer_lifecycle_state::in_flight))
            release_record(i);
    }
}

unsigned lx::compositor::buffer_lifecycle_tracker::pending_release_count() const {
    unsigned n = 0;
    for (unsigned i = 0; i < count_; ++i)
        if (records_[i].state == buffer_lifecycle_state::pending_release) ++n;
    return n;
}

bool lx::compositor::buffer_lifecycle_tracker::has_unreleased_frames() const {
    for (unsigned i = 0; i < count_; ++i) {
        const auto s = records_[i].state;
        if (s == buffer_lifecycle_state::attached ||
            (s == buffer_lifecycle_state::in_flight && !records_[i].current))
            return true;
    }
    return false;
}

lx::compositor::buffer_lifecycle_state
lx::compositor::buffer_lifecycle_tracker::state_for(lx::surface_id surface) const {
    if (const int slot = find_surface(surface); slot >= 0)
        return records_[static_cast<unsigned>(slot)].state;
    return buffer_lifecycle_state::none;
}
