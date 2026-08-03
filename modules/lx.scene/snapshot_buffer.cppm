module;

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <new>
#include <thread>
#include <utility>

import lx.foundation;
import lx.runtime;
import lx.trace;

export module lx.scene:snapshot;

export namespace lx::scene {

struct draw_command {
    lx::texture_id texture{};
    lx::rect2i src{};
    lx::rect2i dst{};
    lx::transform2d transform = lx::transform2d::identity();
    lx::rect2i clip{};
    lx::color tint = lx::color::rgb(1.f, 1.f, 1.f, 1.f);
    float opacity = 1.f;
    lx::blend_mode blend = lx::blend_mode::premultiplied;
    lx::color_space src_space = lx::color_space::srgb;
    lx::image_description_id image_desc{};
    lx::buffer_transform buffer_xform = lx::buffer_transform::normal;
    float scale = 1.f;
    lx::draw_sort_key sort_key{};
};

inline constexpr unsigned k_initial_draw_commands = 256;
inline constexpr unsigned k_max_draw_commands = 4096;
inline constexpr unsigned k_snapshot_slot_count = 3;

class draw_list {
public:
    draw_list();
    ~draw_list();
    draw_list(const draw_list&) = delete;
    draw_list& operator=(const draw_list&) = delete;

    void clear();
    void push(draw_command cmd,
              lx::runtime::overflow_action overflow = lx::runtime::overflow_action::warn_and_drop);
    /// Partition opaque/translucent batches; preserve paint order within translucent.
    void batch_for_render();
    [[nodiscard]] unsigned size() const;
    [[nodiscard]] const draw_command* data() const;
    [[nodiscard]] draw_command* data();
    [[nodiscard]] unsigned capacity() const;
    [[nodiscard]] unsigned overflow_drops() const;

private:
    friend class snapshot_buffer;
    void ensure_capacity(unsigned need);

    draw_command* commands_ = nullptr;
    unsigned capacity_ = 0;
    unsigned count_ = 0;
    unsigned overflow_drops_ = 0;
    bool cap_warned_ = false;
};

class immutable_frame_snapshot {
public:
    [[nodiscard]] const draw_list& draws() const { return draws_; }
    [[nodiscard]] lx::rect2i damage() const { return damage_; }
    [[nodiscard]] unsigned frame_index() const { return frame_index_; }
    [[nodiscard]] bool dropped() const { return dropped_; }

private:
    friend class snapshot_buffer;
    draw_list draws_{};
    lx::rect2i damage_{};
    unsigned frame_index_ = 0;
    unsigned slot_ = 0;
    bool dropped_ = false;
};

/// What the producer does when no slot is free to write into.
enum class backpressure_policy {
    /// Skip the frame immediately. Lowest producer cost, visible frame drops.
    drop_latest,
    /// Spin-wait (bounded) for the consumer to release a slot. No drops until the bound
    /// is hit; costs UI-thread time. Intended for latency experiments, not production.
    stall_ui,
    /// Use every slot before considering a drop; ignores `max_in_flight` for admission.
    /// With one consumer a slot is always free, so this never drops.
    triple_buffer,
};

struct snapshot_config {
    backpressure_policy backpressure = backpressure_policy::triple_buffer;
    /// Admission limit for `drop_latest` / `stall_ui`. Ignored by `triple_buffer`.
    unsigned max_in_flight = 2;
};

inline constexpr unsigned k_no_slot = ~0u;
/// Yield iterations before `stall_ui` gives up. Bounded so a stalled or crashed consumer
/// cannot wedge the producer thread forever.
inline constexpr unsigned k_stall_yield_budget = 1024;

class snapshot_buffer;

/// Exclusive right to write one slot, held across the whole frame build.
///
/// Slot validation happens when the lease is taken, not when it is published: the producer
/// mutates the slot's `draw_list` for the entire build, so checking afterwards checks the
/// wrong instant. An invalid lease means no slot was free — build nothing.
class write_lease {
public:
    write_lease() = default;

    write_lease(const write_lease&) = delete;
    write_lease& operator=(const write_lease&) = delete;
    write_lease(write_lease&& other) noexcept { *this = std::move(other); }
    write_lease& operator=(write_lease&& other) noexcept {
        if (this != &other) {
            owner_ = other.owner_;
            draws_ = other.draws_;
            slot_ = other.slot_;
            other.owner_ = nullptr;
            other.draws_ = nullptr;
            other.slot_ = k_no_slot;
        }
        return *this;
    }

    [[nodiscard]] bool valid() const { return draws_ != nullptr; }
    /// Precondition: `valid()`.
    [[nodiscard]] draw_list& draws() const { return *draws_; }
    [[nodiscard]] unsigned slot() const { return slot_; }

private:
    friend class snapshot_buffer;

    snapshot_buffer* owner_ = nullptr;
    draw_list* draws_ = nullptr;
    unsigned slot_ = k_no_slot;
};

class snapshot_buffer {
public:
    explicit snapshot_buffer(snapshot_config config = {});

    void set_config(snapshot_config config);
    [[nodiscard]] snapshot_config config() const;

    /// Reserves a slot no consumer can be holding, applying the backpressure policy.
    /// An invalid lease means the frame must be skipped — do not build a draw list.
    [[nodiscard]] write_lease begin_frame();
    /// Publishes a lease and consumes it. False if the lease was invalid or not ours.
    [[nodiscard]] bool publish(write_lease& lease, lx::rect2i damage, unsigned frame_index);

    [[nodiscard]] const immutable_frame_snapshot& acquire();
    void release(const immutable_frame_snapshot& snapshot);

    [[nodiscard]] unsigned publish_count() const;
    [[nodiscard]] unsigned dropped_count() const;
    [[nodiscard]] unsigned in_flight_count() const;
    [[nodiscard]] bool would_stall() const;

private:
    /// A slot is writable only if it is neither the published slot nor held by a reader.
    /// Returns `k_no_slot` when every slot is busy — never a live slot.
    [[nodiscard]] unsigned find_free_slot() const;
    [[nodiscard]] bool admission_blocked() const;

    snapshot_config config_{};
    immutable_frame_snapshot buffers_[k_snapshot_slot_count]{};
    std::atomic<unsigned> readers_[k_snapshot_slot_count]{};
    std::atomic<unsigned> read_index_{0};
    std::atomic<unsigned> publish_count_{0};
    std::atomic<unsigned> dropped_count_{0};
    std::atomic<unsigned> in_flight_{0};
};

} // namespace lx::scene


lx::scene::draw_list::draw_list() {
    capacity_ = k_initial_draw_commands;
    commands_ = new draw_command[capacity_];
}

lx::scene::draw_list::~draw_list() { delete[] commands_; }

void lx::scene::draw_list::ensure_capacity(unsigned need) {
    if (need <= capacity_)
        return;
    unsigned new_cap = capacity_ > 0 ? capacity_ : k_initial_draw_commands;
    while (new_cap < need && new_cap < k_max_draw_commands)
        new_cap *= 2;
    new_cap = new_cap > k_max_draw_commands ? k_max_draw_commands : new_cap;
    if (new_cap <= capacity_)
        return;
    auto* next = new draw_command[new_cap];
    for (unsigned i = 0; i < count_; ++i)
        next[i] = commands_[i];
    delete[] commands_;
    commands_ = next;
    capacity_ = new_cap;
}

void lx::scene::draw_list::clear() {
    count_ = 0;
    overflow_drops_ = 0;
    cap_warned_ = false;
}

void lx::scene::draw_list::push(draw_command cmd, lx::runtime::overflow_action overflow) {
    if (count_ < k_max_draw_commands) {
        ensure_capacity(count_ + 1);
        if (count_ < capacity_) {
            commands_[count_++] = cmd;
            return;
        }
    }
    switch (overflow) {
    case lx::runtime::overflow_action::drop_oldest:
        if (count_ > 0) {
            for (unsigned i = 1; i < count_; ++i)
                commands_[i - 1] = commands_[i];
            commands_[count_ - 1] = cmd;
        }
        break;
    case lx::runtime::overflow_action::reject:
        break;
    case lx::runtime::overflow_action::warn_and_drop:
        if (!cap_warned_) {
            cap_warned_ = true;
            lx::trace::logger::global().log(lx::trace::level::warn, "scene.draw_list",
                                            "draw command cap reached");
        }
        [[fallthrough]];
    case lx::runtime::overflow_action::drop_newest:
    default:
        ++overflow_drops_;
        break;
    }
}

void lx::scene::draw_list::batch_for_render() {
    if (count_ <= 1)
        return;

    unsigned indices[k_max_draw_commands];
    for (unsigned i = 0; i < count_; ++i)
        indices[i] = i;

    std::stable_sort(indices, indices + count_, [this](unsigned a, unsigned b) {
        const auto& ca = commands_[a];
        const auto& cb = commands_[b];
        const bool opaque_a = ca.blend == lx::blend_mode::opaque;
        const bool opaque_b = cb.blend == lx::blend_mode::opaque;
        if (opaque_a != opaque_b)
            return opaque_a; // opaque batch first
        if (ca.sort_key.value != cb.sort_key.value)
            return ca.sort_key.value < cb.sort_key.value;
        if (opaque_a) {
            // Front-to-back within opaque batch for early-Z.
            return ca.sort_key.z_order() > cb.sort_key.z_order();
        }
        // Back-to-front within translucent batch.
        if (ca.sort_key.z_order() != cb.sort_key.z_order())
            return ca.sort_key.z_order() < cb.sort_key.z_order();
        return ca.texture.id() < cb.texture.id();
    });

    draw_command tmp[k_max_draw_commands];
    for (unsigned i = 0; i < count_; ++i)
        tmp[i] = commands_[indices[i]];
    for (unsigned i = 0; i < count_; ++i)
        commands_[i] = tmp[i];
}

unsigned lx::scene::draw_list::size() const { return count_; }
unsigned lx::scene::draw_list::capacity() const { return capacity_; }
unsigned lx::scene::draw_list::overflow_drops() const { return overflow_drops_; }
const lx::scene::draw_command* lx::scene::draw_list::data() const { return commands_; }
lx::scene::draw_command* lx::scene::draw_list::data() { return commands_; }

lx::scene::snapshot_buffer::snapshot_buffer(snapshot_config config) : config_{config} {
    for (unsigned i = 0; i < k_snapshot_slot_count; ++i)
        buffers_[i].slot_ = i;
}

void lx::scene::snapshot_buffer::set_config(snapshot_config config) { config_ = config; }
lx::scene::snapshot_config lx::scene::snapshot_buffer::config() const { return config_; }

unsigned lx::scene::snapshot_buffer::find_free_slot() const {
    const unsigned current = read_index_.load(std::memory_order_acquire);
    for (unsigned i = 1; i < k_snapshot_slot_count; ++i) {
        const unsigned idx = (current + i) % k_snapshot_slot_count;
        if (readers_[idx].load(std::memory_order_acquire) == 0)
            return idx;
    }
    // Every other slot is leased to a reader. Say so rather than handing back a slot
    // somebody is reading — the caller drops the frame instead of tearing it.
    return k_no_slot;
}

bool lx::scene::snapshot_buffer::admission_blocked() const {
    if (config_.backpressure == backpressure_policy::triple_buffer)
        return false;
    return in_flight_.load(std::memory_order_acquire) >= config_.max_in_flight;
}

bool lx::scene::snapshot_buffer::would_stall() const { return admission_blocked(); }

lx::scene::write_lease lx::scene::snapshot_buffer::begin_frame() {
    write_lease lease{};

    switch (config_.backpressure) {
    case backpressure_policy::drop_latest:
        if (admission_blocked()) {
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
            return lease;
        }
        break;

    case backpressure_policy::stall_ui: {
        // Actually wait, which is what the policy name promises. Bounded: a wedged
        // consumer must degrade to a dropped frame, not a hung producer thread.
        unsigned spins = 0;
        while (admission_blocked() && spins < k_stall_yield_budget) {
            std::this_thread::yield();
            ++spins;
        }
        if (admission_blocked()) {
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
            return lease;
        }
        break;
    }

    case backpressure_policy::triple_buffer:
        // Admission is slot availability alone — that is what the third slot is for.
        break;
    }

    unsigned slot = find_free_slot();
    if (slot == k_no_slot && config_.backpressure == backpressure_policy::stall_ui) {
        unsigned spins = 0;
        while (slot == k_no_slot && spins < k_stall_yield_budget) {
            std::this_thread::yield();
            slot = find_free_slot();
            ++spins;
        }
    }

    if (slot == k_no_slot) {
        dropped_count_.fetch_add(1, std::memory_order_relaxed);
        return lease;
    }

    lease.owner_ = this;
    lease.slot_ = slot;
    lease.draws_ = &buffers_[slot].draws_;
    return lease;
}

bool lx::scene::snapshot_buffer::publish(write_lease& lease, lx::rect2i damage,
                                         unsigned frame_index) {
    if (!lease.valid() || lease.owner_ != this)
        return false;

    const unsigned slot = lease.slot_;
    lease = write_lease{}; // a lease publishes at most once

    // Safe without re-validation: `begin_frame` reserved a slot that was neither published
    // nor held, and a reader can only take the slot `read_index_` names — which only this
    // thread moves, and only here.
    buffers_[slot].damage_ = damage;
    buffers_[slot].frame_index_ = frame_index;
    buffers_[slot].dropped_ = false;

    read_index_.store(slot, std::memory_order_release);
    in_flight_.fetch_add(1, std::memory_order_acq_rel);
    publish_count_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

const lx::scene::immutable_frame_snapshot& lx::scene::snapshot_buffer::acquire() {
    for (;;) {
        const unsigned idx = read_index_.load(std::memory_order_acquire);
        readers_[idx].fetch_add(1, std::memory_order_acq_rel);
        if (read_index_.load(std::memory_order_acquire) == idx)
            return buffers_[idx];
        readers_[idx].fetch_sub(1, std::memory_order_acq_rel);
    }
}

void lx::scene::snapshot_buffer::release(const immutable_frame_snapshot& snapshot) {
    const unsigned slot = snapshot.slot_;
    if (slot < k_snapshot_slot_count) {
        unsigned held = readers_[slot].load(std::memory_order_acquire);
        while (held > 0 && !readers_[slot].compare_exchange_weak(held, held - 1,
                                                                 std::memory_order_acq_rel,
                                                                 std::memory_order_acquire)) {
        }
    }

    unsigned prev = in_flight_.load(std::memory_order_acquire);
    while (prev > 0 &&
           !in_flight_.compare_exchange_weak(prev, prev - 1, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
    }
}

unsigned lx::scene::snapshot_buffer::publish_count() const {
    return publish_count_.load(std::memory_order_acquire);
}

unsigned lx::scene::snapshot_buffer::dropped_count() const {
    return dropped_count_.load(std::memory_order_acquire);
}

unsigned lx::scene::snapshot_buffer::in_flight_count() const {
    return in_flight_.load(std::memory_order_acquire);
}
