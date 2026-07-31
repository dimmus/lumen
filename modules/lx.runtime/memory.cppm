module;

import lx.foundation;
import lx.sync;

export module lx.runtime:memory;

export namespace lx::runtime {

/// Per-subsystem bump arenas — reset each frame on UI/render/worker tick.
enum class memory_pool { ui, render, worker, cold };

enum class memory_pressure { normal, moderate, critical };

/// Action when a fixed-capacity structure is full.
enum class overflow_action {
    reject,       ///< Return error / skip push
    drop_newest,  ///< Ignore incoming entry
    drop_oldest,  ///< Evict oldest, accept new
    warn_and_drop ///< Log warn, then drop_newest
};

struct memory_budget_config {
    unsigned ui_arena_bytes = 4 * 1024 * 1024;
    unsigned render_arena_bytes = 8 * 1024 * 1024;
    unsigned worker_arena_bytes = 4 * 1024 * 1024;
    unsigned cold_arena_bytes = 512 * 1024;
    unsigned pressure_moderate_used_pct = 75;
    unsigned pressure_critical_used_pct = 90;
};

using memory_pressure_handler = void (*)(memory_pressure level, void* user_data);

/// Frame-local bump allocator — no heap after init.
class memory_arena {
public:
    explicit memory_arena(unsigned capacity_bytes = 0);

    [[nodiscard]] void* allocate(unsigned size, unsigned alignment = alignof(void*));
    void reset();
    [[nodiscard]] unsigned used_bytes() const;
    [[nodiscard]] unsigned capacity_bytes() const;
    [[nodiscard]] bool would_overflow(unsigned size) const;

private:
    static constexpr unsigned k_inline_capacity = 65536;
    unsigned char inline_storage_[k_inline_capacity]{};
    unsigned char* storage_ = inline_storage_;
    unsigned capacity_ = k_inline_capacity;
    unsigned offset_ = 0;
};

/// Global memory budget coordinator — pressure callbacks fan out to subscribers.
class memory_budget_coordinator {
public:
    explicit memory_budget_coordinator(memory_budget_config config = {});

    void set_config(memory_budget_config config);
    [[nodiscard]] memory_budget_config config() const;

    [[nodiscard]] memory_arena& arena(memory_pool pool);
    void reset_frame_arenas(memory_pool pool);

    void set_pressure_handler(memory_pressure_handler handler, void* user_data = nullptr);
    void report_usage(memory_pool pool, unsigned used_bytes, unsigned capacity_bytes);
    void on_external_pressure(memory_pressure level);

    [[nodiscard]] memory_pressure current_pressure() const;
    [[nodiscard]] unsigned frame_index() const;

    /// Called once per UI frame — evaluates thresholds and fires handlers.
    void tick(unsigned frame_index);

private:
    void evaluate_pressure();

    memory_budget_config config_{};
    memory_arena ui_arena_{};
    memory_arena render_arena_{};
    memory_arena worker_arena_{};
    memory_arena cold_arena_{};
    memory_pressure_handler handler_ = nullptr;
    void* handler_user_ = nullptr;
    memory_pressure pressure_ = memory_pressure::normal;
    unsigned frame_index_ = 0;
    unsigned pool_used_[4]{};
    unsigned pool_capacity_[4]{};
};

} // namespace lx::runtime


lx::runtime::memory_arena::memory_arena(unsigned capacity_bytes)
    : capacity_{capacity_bytes > 0 ? capacity_bytes : k_inline_capacity} {
    if (capacity_bytes > k_inline_capacity) {
        /* P0: mmap or static pool backing for large arenas */
        capacity_ = capacity_bytes;
    }
}

void* lx::runtime::memory_arena::allocate(unsigned size, unsigned alignment) {
    if (size == 0) return nullptr;
    unsigned aligned = (offset_ + alignment - 1) & ~(alignment - 1);
    if (aligned + size > capacity_) return nullptr;
    void* ptr = storage_ + aligned;
    offset_ = aligned + size;
    return ptr;
}

void lx::runtime::memory_arena::reset() { offset_ = 0; }

unsigned lx::runtime::memory_arena::used_bytes() const { return offset_; }
unsigned lx::runtime::memory_arena::capacity_bytes() const { return capacity_; }

bool lx::runtime::memory_arena::would_overflow(unsigned size) const {
    return offset_ + size > capacity_;
}

lx::runtime::memory_budget_coordinator::memory_budget_coordinator(memory_budget_config config)
    : config_{config},
      ui_arena_{config.ui_arena_bytes},
      render_arena_{config.render_arena_bytes},
      worker_arena_{config.worker_arena_bytes},
      cold_arena_{config.cold_arena_bytes} {
    pool_capacity_[0] = config.ui_arena_bytes;
    pool_capacity_[1] = config.render_arena_bytes;
    pool_capacity_[2] = config.worker_arena_bytes;
    pool_capacity_[3] = config.cold_arena_bytes;
}

void lx::runtime::memory_budget_coordinator::set_config(memory_budget_config config) {
    config_ = config;
}

lx::runtime::memory_budget_config lx::runtime::memory_budget_coordinator::config() const {
    return config_;
}

lx::runtime::memory_arena& lx::runtime::memory_budget_coordinator::arena(memory_pool pool) {
    switch (pool) {
    case memory_pool::render: return render_arena_;
    case memory_pool::worker: return worker_arena_;
    case memory_pool::cold: return cold_arena_;
    default: return ui_arena_;
    }
}

void lx::runtime::memory_budget_coordinator::reset_frame_arenas(memory_pool pool) {
    arena(pool).reset();
}

void lx::runtime::memory_budget_coordinator::set_pressure_handler(
    memory_pressure_handler handler, void* user_data) {
    handler_ = handler;
    handler_user_ = user_data;
}

void lx::runtime::memory_budget_coordinator::report_usage(memory_pool pool,
                                                          unsigned used,
                                                          unsigned capacity) {
    const unsigned idx = static_cast<unsigned>(pool);
    if (idx < 4) {
        pool_used_[idx] = used;
        if (capacity > 0) pool_capacity_[idx] = capacity;
    }
    evaluate_pressure();
}

void lx::runtime::memory_budget_coordinator::on_external_pressure(memory_pressure level) {
    if (static_cast<unsigned>(level) > static_cast<unsigned>(pressure_))
        pressure_ = level;
    if (handler_) handler_(pressure_, handler_user_);
}

void lx::runtime::memory_budget_coordinator::evaluate_pressure() {
    unsigned total_used = 0;
    unsigned total_cap = 0;
    for (unsigned i = 0; i < 4; ++i) {
        total_used += pool_used_[i];
        total_cap += pool_capacity_[i];
    }
    if (total_cap == 0) return;

    const unsigned pct = (total_used * 100) / total_cap;
    const memory_pressure prev = pressure_;
    if (pct >= config_.pressure_critical_used_pct)
        pressure_ = memory_pressure::critical;
    else if (pct >= config_.pressure_moderate_used_pct)
        pressure_ = memory_pressure::moderate;
    else
        pressure_ = memory_pressure::normal;

    if (pressure_ != prev && handler_) handler_(pressure_, handler_user_);
}

lx::runtime::memory_pressure lx::runtime::memory_budget_coordinator::current_pressure() const {
    return pressure_;
}

unsigned lx::runtime::memory_budget_coordinator::frame_index() const { return frame_index_; }

void lx::runtime::memory_budget_coordinator::tick(unsigned frame_index) {
    frame_index_ = frame_index;
    report_usage(memory_pool::ui, ui_arena_.used_bytes(), ui_arena_.capacity_bytes());
    report_usage(memory_pool::render, render_arena_.used_bytes(), render_arena_.capacity_bytes());
    report_usage(memory_pool::worker, worker_arena_.used_bytes(), worker_arena_.capacity_bytes());
    report_usage(memory_pool::cold, cold_arena_.used_bytes(), cold_arena_.capacity_bytes());
}
