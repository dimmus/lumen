export module lx.sync:queue;

import std;

export namespace lx::sync {

/// Single-producer single-consumer lock-free ring buffer.
/// Producer and consumer must each run on exactly one thread.
template<typename T, std::size_t Capacity>
class spsc_queue {
public:
    static_assert(Capacity >= 2 && Capacity <= 65536, "Capacity must be 2..65536");

    // User-provided, so a value-initialised queue (`spsc_queue q{}`) is not zero-filled:
    // the slots are written as they are first used, and pages of a ring that never wraps
    // (most queues, most of the time) stay untouched. A 1024-task queue is 64 KiB.
    spsc_queue() noexcept {}
    spsc_queue(const spsc_queue&) = delete;
    spsc_queue& operator=(const spsc_queue&) = delete;
    ~spsc_queue() {
        for (auto t = tail_.load(std::memory_order_relaxed); t != head_.load(std::memory_order_relaxed);
             t = (t + 1) % Capacity)
            std::destroy_at(&slots_[t].value);
    }

    [[nodiscard]] bool try_push(T value) {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = (head + 1) % Capacity;
        if (next == tail_.load(std::memory_order_acquire))
            return false;
        std::construct_at(&slots_[head].value, std::move(value));
        head_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out) {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        out = std::move(slots_[tail].value);
        std::destroy_at(&slots_[tail].value);
        tail_.store((tail + 1) % Capacity, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t capacity() const { return Capacity - 1; }

private:
    // libstdc++ often withholds std::hardware_destructive_interference_size (ABI-sensitive);
    // 64 is the universal x86_64/aarch64 cache-line width for this purpose.
    static constexpr std::size_t k_cache_line = 64;
    alignas(k_cache_line) std::atomic<std::size_t> head_{0};
    alignas(k_cache_line) std::atomic<std::size_t> tail_{0};
    /// Storage only: an element exists from its push to its pop. Its own cache line, so
    /// the producer writing the first slots does not invalidate the consumer's `tail_`.
    union slot {
        T value;
        slot() noexcept {}
        ~slot() {}
    };
    alignas(k_cache_line) slot slots_[Capacity];
};

} // namespace lx::sync
