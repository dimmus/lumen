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
};

using wl_buffer_release_fn = void (*)(void* wl_buffer_resource, void* user_data);

/// Tracks import → use → release → wl_buffer.release → cache evict.
///
/// **Thread affinity: UI only.** Every method mutates the record table and may send
/// `wl_buffer.release`, so it must run on the thread that owns Wayland dispatch. The render
/// thread never touches the tracker; it publishes a completed frame index that the UI
/// thread feeds to `retire_completed_frames()`.
class buffer_lifecycle_tracker {
public:
    explicit buffer_lifecycle_tracker(gfx::dmabuf_import_cache& cache);

    void set_release_callback(wl_buffer_release_fn fn, void* user_data = nullptr);

    [[nodiscard]] lx::result<void> on_import(lx::client_id client, lx::surface_id surface,
                                             lx::buffer_id buffer,
                                             const gfx::import_cache_key& key,
                                             void* wl_buffer);
    void on_attach(lx::surface_id surface);
    void on_present(lx::surface_id surface, unsigned frame_index);
    void on_render_done(lx::surface_id surface, unsigned frame_index);

    /// Mark every attached buffer in-flight, tagged with the frame whose snapshot will
    /// sample it. Call right after the snapshot for `frame_index` is published.
    void on_frame_submitted(unsigned frame_index);

    /// Release every buffer the render thread has finished with, i.e. those tagged with a
    /// frame at or before `completed_frame`. Sends wl_buffer.release and drops the import
    /// cache reference.
    [[nodiscard]] lx::result<void> retire_completed_frames(long long completed_frame);

    /// Sends wl_buffer.release when safe — decrements import cache use_count.
    [[nodiscard]] lx::result<void> flush_pending_releases();

    /// The client destroyed the wl_buffer (explicitly or by disconnecting). Drop the record
    /// without sending a release — the wl_resource is already gone.
    void on_buffer_destroyed(void* wl_buffer);

    void on_client_disconnect(lx::client_id client);
    void on_memory_pressure(lx::runtime::memory_pressure level);

    [[nodiscard]] unsigned pending_release_count() const;
    [[nodiscard]] buffer_lifecycle_state state_for(lx::surface_id surface) const;

private:
    [[nodiscard]] int find_surface(lx::surface_id surface) const;
    [[nodiscard]] int find_record(lx::surface_id surface, lx::buffer_id buffer) const;
    [[nodiscard]] int alloc_record();

    gfx::dmabuf_import_cache* cache_ = nullptr;
    wl_buffer_release_fn release_fn_ = nullptr;
    void* release_user_ = nullptr;
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

lx::result<void> lx::compositor::buffer_lifecycle_tracker::on_import(
    lx::client_id client, lx::surface_id surface, lx::buffer_id buffer,
    const gfx::import_cache_key& key, void* wl_buffer) {
    // Re-committing the same buffer refreshes the existing record. A *different* buffer
    // gets its own record: the previously attached one may still be in flight, and
    // dropping its wl_buffer here would mean the client never receives its release.
    if (const int existing = find_record(surface, buffer); existing >= 0) {
        auto& rec = records_[static_cast<unsigned>(existing)];
        rec.client = client;
        rec.cache_key = key;
        rec.wl_buffer = wl_buffer;
        rec.state = buffer_lifecycle_state::imported;
        return {};
    }

    const int slot = alloc_record();
    if (slot < 0)
        return lx::make_error(lx::error_domain::invalid_argument, 0, "lifecycle tracker full");
    records_[static_cast<unsigned>(slot)] = {
        client, surface, buffer, key, buffer_lifecycle_state::imported, 0, 0, wl_buffer};
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
        if (records_[i].state != buffer_lifecycle_state::attached)
            continue;
        records_[i].state = buffer_lifecycle_state::in_flight;
        records_[i].frame_last_used = frame_index;
    }
}

lx::result<void>
lx::compositor::buffer_lifecycle_tracker::retire_completed_frames(long long completed_frame) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (completed_frame < 0)
        return {};
    for (unsigned i = 0; i < count_; ++i) {
        if (records_[i].state != buffer_lifecycle_state::in_flight)
            continue;
        if (static_cast<long long>(records_[i].frame_last_used) > completed_frame)
            continue;
        records_[i].state = buffer_lifecycle_state::pending_release;
    }
    return flush_pending_releases();
}

lx::result<void> lx::compositor::buffer_lifecycle_tracker::flush_pending_releases() {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    for (unsigned i = 0; i < count_; ++i) {
        if (records_[i].state != buffer_lifecycle_state::pending_release)
            continue;
        if (release_fn_ && records_[i].wl_buffer)
            release_fn_(records_[i].wl_buffer, release_user_);
        records_[i].wl_buffer = nullptr;
        if (cache_)
            cache_->release(records_[i].cache_key);
        records_[i].state = buffer_lifecycle_state::released;
    }
    return {};
}

void lx::compositor::buffer_lifecycle_tracker::on_buffer_destroyed(void* wl_buffer) {
    if (!wl_buffer)
        return;
    for (unsigned i = 0; i < count_; ++i) {
        if (records_[i].wl_buffer != wl_buffer)
            continue;
        records_[i].wl_buffer = nullptr;
        if (cache_)
            cache_->release(records_[i].cache_key);
        records_[i].state = buffer_lifecycle_state::released;
    }
}

void lx::compositor::buffer_lifecycle_tracker::on_client_disconnect(lx::client_id client) {
    if (cache_) cache_->evict_client(client);
    for (unsigned i = 0; i < count_; ++i) {
        if (records_[i].client != client)
            continue;
        // The wl_resource dies with the client, so it must not be released — just forget it.
        records_[i].wl_buffer = nullptr;
        records_[i].state = buffer_lifecycle_state::released;
    }
}

void lx::compositor::buffer_lifecycle_tracker::on_memory_pressure(
    lx::runtime::memory_pressure level) {
    if (level == lx::runtime::memory_pressure::normal) return;
    for (unsigned i = 0; i < count_; ++i) {
        if (records_[i].state == buffer_lifecycle_state::pending_release ||
            (level == lx::runtime::memory_pressure::critical &&
             records_[i].state == buffer_lifecycle_state::in_flight)) {
            if (release_fn_ && records_[i].wl_buffer)
                release_fn_(records_[i].wl_buffer, release_user_);
            records_[i].wl_buffer = nullptr;
            if (cache_) cache_->release(records_[i].cache_key);
            records_[i].state = buffer_lifecycle_state::released;
        }
    }
}

unsigned lx::compositor::buffer_lifecycle_tracker::pending_release_count() const {
    unsigned n = 0;
    for (unsigned i = 0; i < count_; ++i)
        if (records_[i].state == buffer_lifecycle_state::pending_release) ++n;
    return n;
}

lx::compositor::buffer_lifecycle_state
lx::compositor::buffer_lifecycle_tracker::state_for(lx::surface_id surface) const {
    if (const int slot = find_surface(surface); slot >= 0)
        return records_[static_cast<unsigned>(slot)].state;
    return buffer_lifecycle_state::none;
}
