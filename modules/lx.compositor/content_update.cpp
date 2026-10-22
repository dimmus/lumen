module;

// Bodies of `:content_update` (the queue, the readiness policy and the poller). See
// content_update.cppm for the model.

#include <cerrno>
#include <cstdint>

#if defined(__linux__)
#include <sys/epoll.h>
#include <unistd.h>
#endif

module lx.compositor;

import lx.foundation;
import :content_update;

namespace lx::compositor {

// ── content_queue ──────────────────────────────────────────────────────────

content_queue::content_queue() {
    // Free list in index order, so the first allocations are the low, warm records.
    free_head_ = 1;
    for (unsigned i = 1; i <= k_cu_pool; ++i)
        free_next_[i] = i < k_cu_pool ? i + 1 : 0;
    free_count_ = k_cu_pool;
}

unsigned content_queue::alloc(unsigned surface, unsigned client) {
    if (free_head_ == 0 || surface == 0 || surface > k_cu_surfaces)
        return 0;
    const unsigned cu = free_head_;
    free_head_ = free_next_[cu];
    free_next_[cu] = 0;
    --free_count_;
    cu_record& r = pool_[cu];
    r = {};
    r.used = true;
    r.surface = surface;
    r.client = client;
    r.generation = ++generation_ ? generation_ : ++generation_;
    return cu;
}

void content_queue::release(unsigned cu) {
    if (cu == 0 || cu > k_cu_pool || !pool_[cu].used)
        return;
    const unsigned generation = pool_[cu].generation;
    pool_[cu] = {};
    pool_[cu].generation = generation;
    free_next_[cu] = free_head_;
    free_head_ = cu;
    ++free_count_;
}

void content_queue::push(unsigned cu) {
    if (cu == 0 || cu > k_cu_pool || !pool_[cu].used)
        return;
    const unsigned s = pool_[cu].surface;
    pool_[cu].next = 0;
    if (tail_[s])
        pool_[tail_[s]].next = cu;
    else
        head_[s] = cu;
    tail_[s] = cu;
    ++depth_[s];
    queued_mask_[s / 64] |= 1ull << (s % 64);
}

void content_queue::seal(unsigned parent, unsigned child) {
    if (parent == 0 || child == 0 || parent > k_cu_pool || child > k_cu_pool)
        return;
    cu_record& c = pool_[child];
    c.parent = parent;
    c.next_sibling = 0;
    unsigned* link = &pool_[parent].first_child;
    while (*link)
        link = &pool_[*link].next_sibling;
    *link = child;
}

void content_queue::unseal(unsigned child) {
    if (child == 0 || child > k_cu_pool || pool_[child].parent == 0)
        return;
    unsigned* link = &pool_[pool_[child].parent].first_child;
    while (*link && *link != child)
        link = &pool_[*link].next_sibling;
    if (*link == child)
        *link = pool_[child].next_sibling;
    pool_[child].parent = 0;
    pool_[child].next_sibling = 0;
}

unsigned content_queue::pop(unsigned surface) {
    if (surface == 0 || surface > k_cu_surfaces || head_[surface] == 0)
        return 0;
    const unsigned cu = head_[surface];
    head_[surface] = pool_[cu].next;
    pool_[cu].next = 0;
    if (head_[surface] == 0) {
        tail_[surface] = 0;
        queued_mask_[surface / 64] &= ~(1ull << (surface % 64));
    }
    --depth_[surface];
    return cu;
}

unsigned content_queue::head(unsigned surface) const {
    return surface && surface <= k_cu_surfaces ? head_[surface] : 0;
}

unsigned content_queue::tail(unsigned surface) const {
    return surface && surface <= k_cu_surfaces ? tail_[surface] : 0;
}

unsigned content_queue::depth(unsigned surface) const {
    return surface && surface <= k_cu_surfaces ? depth_[surface] : 0;
}

unsigned content_queue::client_count(unsigned client) const {
    if (free_count_ == k_cu_pool)
        return 0;
    unsigned n = 0;
    for (unsigned i = 1; i <= k_cu_pool; ++i)
        n += pool_[i].used && pool_[i].client == client ? 1u : 0u;
    return n;
}

bool content_queue::fences_clear(unsigned cu) const {
    if (cu == 0 || cu > k_cu_pool)
        return true;
    if (pool_[cu].waits != 0 || pool_[cu].recheck)
        return false;
    for (unsigned c = pool_[cu].first_child; c; c = pool_[c].next_sibling)
        if (pool_[c].waits != 0 || pool_[c].recheck)
            return false;
    return true;
}

unsigned content_queue::queued_surfaces(unsigned* out, unsigned cap) const {
    unsigned n = 0;
    for (unsigned w = 0; w < sizeof(queued_mask_) / sizeof(queued_mask_[0]); ++w) {
        std::uint64_t bits = queued_mask_[w];
        while (bits && n < cap) {
            const unsigned bit = static_cast<unsigned>(__builtin_ctzll(bits));
            bits &= bits - 1;
            out[n++] = w * 64 + bit;
        }
    }
    return n;
}

// ── readiness policy ───────────────────────────────────────────────────────

bool cu_room_for(unsigned free, unsigned held, unsigned need) {
    if (need == 0)
        return true;
    if (held + need > k_cu_per_client || free < need)
        return false;
    return held + need <= k_cu_light || free - need >= k_cu_reserve;
}

bool cu_time_reached(unsigned long long target_ns, unsigned long long predicted_ns) {
    if (target_ns == 0 || predicted_ns == 0)
        return true;
    return target_ns <= predicted_ns + k_cu_time_slack_ns;
}

bool cu_ready(const cu_record& r, bool fences_clear, const cu_conditions& c) {
    if (!fences_clear)
        return false;
    if (!c.shown)
        return true; // fifo and timing are ignored while the surface is not on screen
    if (r.fifo_wait && c.barrier)
        return false;
    return cu_time_reached(r.target_ns, c.predicted_ns);
}

// ── readiness_poller ───────────────────────────────────────────────────────

readiness_poller::~readiness_poller() {
#if defined(__linux__)
    for (unsigned i = 0; i < k_max; ++i)
        if (entries_[i].fd >= 0)
            ::close(entries_[i].fd);
    if (epfd_ >= 0)
        ::close(epfd_);
#endif
}

lx::result<void> readiness_poller::open() {
#if defined(__linux__)
    if (epfd_ >= 0)
        return {};
    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0)
        return lx::make_error(lx::error_domain::io, errno, "readiness poller: epoll_create1 failed");
    return {};
#else
    return lx::not_implemented("lx::compositor::readiness_poller::open");
#endif
}

bool readiness_poller::add(int fd, unsigned cu, unsigned generation, bool reserved) {
#if defined(__linux__)
    if (fd < 0)
        return false;
    if (epfd_ < 0 || available(reserved) == 0) {
        ::close(fd);
        return false;
    }
    for (unsigned i = 0; i < k_max; ++i) {
        entry& e = entries_[i];
        if (e.fd >= 0)
            continue;
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.u32 = i;
        if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
            ::close(fd);
            return false;
        }
        e.fd = fd;
        e.cu = cu;
        e.generation = generation;
        e.reserved = reserved;
        ++used_;
        if (reserved)
            ++reserved_used_;
        return true;
    }
    ::close(fd);
    return false;
#else
    (void)fd;
    (void)cu;
    (void)generation;
    (void)reserved;
    return false;
#endif
}

void readiness_poller::remove(unsigned index) {
#if defined(__linux__)
    entry& e = entries_[index];
    if (e.fd < 0)
        return;
    // Closing the fd removes it from the epoll set (it holds the only reference).
    ::close(e.fd);
    if (e.reserved)
        --reserved_used_;
    e = {};
    --used_;
#else
    (void)index;
#endif
}

unsigned readiness_poller::drop(unsigned cu) {
    if (used_ == 0 || cu == 0)
        return 0;
    unsigned n = 0;
    for (unsigned i = 0; i < k_max; ++i)
        if (entries_[i].fd >= 0 && entries_[i].cu == cu) {
            remove(i);
            ++n;
        }
    return n;
}

unsigned readiness_poller::drain(wake* out, unsigned cap) {
#if defined(__linux__)
    if (epfd_ < 0 || used_ == 0 || cap == 0)
        return 0;
    epoll_event events[32];
    const int max = static_cast<int>(cap < 32 ? cap : 32);
    const int n = ::epoll_wait(epfd_, events, max, 0);
    unsigned got = 0;
    for (int i = 0; i < n; ++i) {
        const unsigned index = events[i].data.u32;
        if (index >= k_max || entries_[index].fd < 0)
            continue;
        out[got].cu = entries_[index].cu;
        out[got].generation = entries_[index].generation;
        ++got;
        remove(index);
    }
    return got;
#else
    (void)out;
    (void)cap;
    return 0;
#endif
}

} // namespace lx::compositor
