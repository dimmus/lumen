module;

#include <cstddef>
#include <sys/mman.h>
#include <unistd.h>

import lx.foundation;
import lx.sync;
import lx.trace;

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

/// Frame-local bump allocator — one anonymous mapping at construction, no heap after.
///
/// `capacity_bytes()` is the size of storage this arena actually owns, never the size it
/// was asked for: if the mapping fails the arena reports (and hands out) zero bytes rather
/// than bounds-checking against memory it does not have.
class memory_arena {
public:
    static constexpr unsigned k_default_capacity = 65536;

    explicit memory_arena(unsigned capacity_bytes = 0);
    ~memory_arena();

    memory_arena(const memory_arena&) = delete;
    memory_arena& operator=(const memory_arena&) = delete;
    memory_arena(memory_arena&& other) noexcept;
    memory_arena& operator=(memory_arena&& other) noexcept;

    /// Returns nullptr when the request does not fit; never returns unowned storage.
    /// `alignment` must be a power of two.
    [[nodiscard]] void* allocate(unsigned size, unsigned alignment = alignof(void*));
    void reset();
    [[nodiscard]] unsigned used_bytes() const;
    /// Bytes actually mapped — 0 if the mapping failed.
    [[nodiscard]] unsigned capacity_bytes() const;
    [[nodiscard]] bool would_overflow(unsigned size) const;
    /// False when backing storage could not be obtained.
    [[nodiscard]] bool valid() const;

private:
    void release();

    unsigned char* storage_ = nullptr;
    unsigned capacity_ = 0;
    unsigned mapped_bytes_ = 0;
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


lx::runtime::memory_arena::memory_arena(unsigned capacity_bytes) {
    const unsigned want = capacity_bytes > 0 ? capacity_bytes : k_default_capacity;

    const long page = ::sysconf(_SC_PAGESIZE);
    const unsigned page_size = page > 0 ? static_cast<unsigned>(page) : 4096u;
    // Round up to a whole page; refuse rather than wrap if the round-up overflows.
    if (want > 0xFFFFFFFFu - (page_size - 1)) {
        lx::trace::logger::global().log(lx::trace::level::error, "runtime.memory",
                                        "arena capacity too large to map");
        return;
    }
    const unsigned mapped = ((want + page_size - 1) / page_size) * page_size;

    void* base = ::mmap(nullptr, static_cast<std::size_t>(mapped), PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) {
        // Report zero capacity so allocate() refuses rather than handing out storage we
        // do not own. A caller that checks its result degrades; one that trusts a lied-about
        // capacity corrupts whatever follows the arena.
        lx::trace::logger::global().log(lx::trace::level::error, "runtime.memory",
                                        "arena mmap failed — arena has zero capacity");
        return;
    }

    storage_ = static_cast<unsigned char*>(base);
    mapped_bytes_ = mapped;
    capacity_ = mapped;
}

void lx::runtime::memory_arena::release() {
    if (storage_ && mapped_bytes_ > 0)
        ::munmap(storage_, static_cast<std::size_t>(mapped_bytes_));
    storage_ = nullptr;
    capacity_ = 0;
    mapped_bytes_ = 0;
    offset_ = 0;
}

lx::runtime::memory_arena::~memory_arena() { release(); }

lx::runtime::memory_arena::memory_arena(memory_arena&& other) noexcept
    : storage_{other.storage_},
      capacity_{other.capacity_},
      mapped_bytes_{other.mapped_bytes_},
      offset_{other.offset_} {
    other.storage_ = nullptr;
    other.capacity_ = 0;
    other.mapped_bytes_ = 0;
    other.offset_ = 0;
}

lx::runtime::memory_arena& lx::runtime::memory_arena::operator=(memory_arena&& other) noexcept {
    if (this == &other)
        return *this;
    release();
    storage_ = other.storage_;
    capacity_ = other.capacity_;
    mapped_bytes_ = other.mapped_bytes_;
    offset_ = other.offset_;
    other.storage_ = nullptr;
    other.capacity_ = 0;
    other.mapped_bytes_ = 0;
    other.offset_ = 0;
    return *this;
}

void* lx::runtime::memory_arena::allocate(unsigned size, unsigned alignment) {
    if (size == 0 || !storage_) return nullptr;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;

    // offset_ <= capacity_ always holds, so the round-up cannot wrap for sane alignments.
    if (offset_ > 0xFFFFFFFFu - (alignment - 1)) return nullptr;
    const unsigned aligned = (offset_ + alignment - 1) & ~(alignment - 1);
    if (aligned > capacity_ || size > capacity_ - aligned) return nullptr;

    void* ptr = storage_ + aligned;
    offset_ = aligned + size;
    return ptr;
}

void lx::runtime::memory_arena::reset() { offset_ = 0; }

unsigned lx::runtime::memory_arena::used_bytes() const { return offset_; }
unsigned lx::runtime::memory_arena::capacity_bytes() const { return capacity_; }
bool lx::runtime::memory_arena::valid() const { return storage_ != nullptr; }

bool lx::runtime::memory_arena::would_overflow(unsigned size) const {
    return offset_ > capacity_ || size > capacity_ - offset_;
}

lx::runtime::memory_budget_coordinator::memory_budget_coordinator(memory_budget_config config)
    : config_{config},
      ui_arena_{config.ui_arena_bytes},
      render_arena_{config.render_arena_bytes},
      worker_arena_{config.worker_arena_bytes},
      cold_arena_{config.cold_arena_bytes} {
    // Capacity comes from the arenas, not the config: pressure must be evaluated against
    // storage that exists. A failed mapping shows up as a zero-capacity pool here.
    pool_capacity_[0] = ui_arena_.capacity_bytes();
    pool_capacity_[1] = render_arena_.capacity_bytes();
    pool_capacity_[2] = worker_arena_.capacity_bytes();
    pool_capacity_[3] = cold_arena_.capacity_bytes();
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
