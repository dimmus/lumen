module;

#include <atomic>
#include <cstddef>
#include <utility>

export module lx.sync:queue;

export namespace lx::sync {

/// Single-producer single-consumer lock-free ring buffer.
/// Producer and consumer must each run on exactly one thread.
template<typename T, std::size_t Capacity>
class spsc_queue {
public:
    static_assert(Capacity >= 2 && Capacity <= 65536, "Capacity must be 2..65536");

    [[nodiscard]] bool try_push(T value) {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = (head + 1) % Capacity;
        if (next == tail_.load(std::memory_order_acquire))
            return false;
        slots_[head] = std::move(value);
        head_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out) {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        out = std::move(slots_[tail]);
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
    T slots_[Capacity]{};
};

} // namespace lx::sync
