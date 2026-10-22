// Screen capture on the Vulkan backend (design-app-compat-capture D.4): an output frame read
// back from the composited target into a client's shm buffer (wl_shm byte order, alpha
// opaque), a crop, and the pointer blended for paint_cursors; the asynchronous path — the
// copy submitted on render without a wait, the rows written by a worker, the slot's buffer
// reused frame after frame. Runs on whatever Vulkan device exists (lavapipe on a host
// without hardware Vulkan); skips without one.
#include "lumen_test.hpp"

#include <unistd.h>

import std;
import lx.foundation;
import lx.gfx;
import lx.compositor;

namespace {

constexpr unsigned k_w = 64;
constexpr unsigned k_h = 48;

struct fixture {
    lx::gfx::device device{};
    lx::gfx::vulkan_compositor vk{};
    lx::gfx::render_target target{};
    bool usable = false;
};

fixture* gpu() {
    static fixture f{};
    static bool tried = false;
    if (tried)
        return f.usable ? &f : nullptr;
    tried = true;
    auto selected = lx::gfx::device_selector::select_best();
    if (!selected) {
        std::printf("  (skip: no Vulkan device)\n");
        return nullptr;
    }
    f.device = std::move(selected).value();
    if (auto ok = f.vk.initialize(f.device.context()); !ok) {
        std::printf("  (skip: %s)\n", ok.get_error().message);
        return nullptr;
    }
    auto t = lx::gfx::render_target::create(f.device.context(), k_w, k_h);
    if (!t) {
        std::printf("  (skip: %s)\n", t.get_error().message);
        return nullptr;
    }
    f.target = std::move(t).value();
    // Left half red, right half blue.
    std::vector<unsigned char> rgba(k_w * k_h * 4u);
    for (unsigned y = 0; y < k_h; ++y)
        for (unsigned x = 0; x < k_w; ++x) {
            unsigned char* p = &rgba[(y * k_w + x) * 4u];
            const bool left = x < k_w / 2;
            p[0] = left ? 255 : 0;
            p[2] = left ? 0 : 255;
            p[3] = 255;
        }
    (void)f.vk.upload_rgba(0x4000'0101u, k_w, k_h, rgba.data());
    lx::gfx::blit_command cmd{};
    cmd.texture_id = 0x4000'0101u;
    cmd.dst = {0, 0, static_cast<int>(k_w), static_cast<int>(k_h)};
    auto c = f.vk.composite(f.target, lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1);
    if (c && c.value().out_fence_fd >= 0)
        ::close(c.value().out_fence_fd);
    f.usable = static_cast<bool>(c);
    return f.usable ? &f : nullptr;
}

struct rig {
    lx::compositor::capture_shm_guards guards{};
    lx::compositor::capture_channel channel{};
    lx::compositor::capture_renderer renderer{};
    explicit rig(fixture& f) {
        lx::compositor::capture_shm_guards::hooks h{};
        h.begin_access = [](void*) {};
        h.end_access = [](void*) {};
        h.unref_pool = [](void*) {};
        guards.bind(h);
        renderer.bind_vulkan(&f.vk);
        renderer.bind_guards(&guards);
        channel.sessions.store(1);
    }
};

} // namespace

LUMEN_TEST(vk_output_frame_is_read_back_into_shm_in_wl_shm_order) {
    fixture* f = gpu();
    if (!f)
        return;
    rig r{*f};
    std::vector<unsigned char> mem(k_w * k_h * 4u, 0x5A);
    lx::compositor::capture_job j{};
    j.kind = lx::compositor::capture_job_kind::output;
    j.session = 1;
    j.generation = 1;
    j.serial = 1;
    j.full = true;
    j.target.width = k_w;
    j.target.height = k_h;
    j.target.shm = r.guards.add(mem.data(), mem.data(), mem.data(), k_w * 4u, k_w, k_h, true);
    (void)r.channel.post(j);
    r.renderer.take(r.channel);
    lx::compositor::capture_frame_source src{};
    src.frame = 1;
    src.size = {static_cast<int>(k_w), static_cast<int>(k_h)};
    src.vk_target = &f->target;
    r.renderer.output_front(0, src, r.channel);
    lx::compositor::capture_completion c[4];
    const unsigned n = r.channel.drain(c, 4);
    LUMEN_CHECK(n == 1 && c[0].result == lx::compositor::capture_result::ready);
    const unsigned char* left = &mem[(20 * k_w + 10) * 4u];
    const unsigned char* right = &mem[(20 * k_w + 50) * 4u];
    // B, G, R, A in memory.
    LUMEN_CHECK(left[2] > 200 && left[0] < 60 && left[3] == 0xFF);
    LUMEN_CHECK(right[0] > 200 && right[2] < 60);
}

LUMEN_TEST(vk_crop_lands_at_the_buffer_origin) {
    fixture* f = gpu();
    if (!f)
        return;
    rig r{*f};
    std::vector<unsigned char> mem(8u * 8u * 4u, 0x5A);
    lx::compositor::capture_job j{};
    j.kind = lx::compositor::capture_job_kind::output;
    j.session = 1;
    j.generation = 1;
    j.serial = 1;
    j.full = true;
    j.crop = {28, 10, 8, 8};
    j.target.width = 8;
    j.target.height = 8;
    j.target.shm = r.guards.add(mem.data(), mem.data(), mem.data(), 32, 8, 8, true);
    (void)r.channel.post(j);
    r.renderer.take(r.channel);
    lx::compositor::capture_frame_source src{};
    src.frame = 1;
    src.size = {static_cast<int>(k_w), static_cast<int>(k_h)};
    src.vk_target = &f->target;
    r.renderer.output_front(0, src, r.channel);
    lx::compositor::capture_completion c[4];
    LUMEN_CHECK(r.channel.drain(c, 4) == 1 && c[0].result == lx::compositor::capture_result::ready);
    // Columns 28..31 are red, 32..35 blue.
    LUMEN_CHECK(mem[(2 * 8 + 1) * 4 + 2] > 200 && mem[(2 * 8 + 6) * 4 + 0] > 200);
}

namespace {

std::vector<std::thread> g_workers;
unsigned g_notified = 0;

bool spawn_worker(void (*fn)(void*), void* arg) {
    g_workers.emplace_back([fn, arg] { fn(arg); });
    return true;
}

void join_workers() {
    for (auto& t : g_workers)
        t.join();
    g_workers.clear();
}

} // namespace

LUMEN_TEST(vk_read_lands_on_a_worker_and_the_slot_is_reused) {
    fixture* f = gpu();
    if (!f)
        return;
    rig r{*f};
    r.renderer.bind_worker(&spawn_worker, [](void*) { ++g_notified; }, nullptr);
    for (unsigned frame = 1; frame <= 3; ++frame) {
        std::vector<unsigned char> mem(k_w * k_h * 4u, 0x5A);
        lx::compositor::capture_job j{};
        j.kind = lx::compositor::capture_job_kind::output;
        j.session = 2;
        j.generation = 1;
        j.serial = frame;
        j.full = true;
        j.target.width = k_w;
        j.target.height = k_h;
        j.target.shm = r.guards.add(mem.data(), mem.data(), mem.data(), k_w * 4u, k_w, k_h, true);
        (void)r.channel.post(j);
        r.renderer.take(r.channel);
        lx::compositor::capture_frame_source src{};
        src.frame = frame;
        src.size = {static_cast<int>(k_w), static_cast<int>(k_h)};
        src.vk_target = &f->target;
        const unsigned notified = g_notified;
        r.renderer.output_front(0, src, r.channel);
        // Render returned with the copy in flight; the worker delivers the completion.
        join_workers();
        LUMEN_CHECK(g_notified == notified + 1);
        lx::compositor::capture_completion c[4];
        const unsigned n = r.channel.drain(c, 4);
        LUMEN_CHECK(n == 1 && c[0].result == lx::compositor::capture_result::ready &&
                    c[0].serial == frame);
        const unsigned char* left = &mem[(20 * k_w + 10) * 4u];
        const unsigned char* right = &mem[(20 * k_w + 50) * 4u];
        LUMEN_CHECK(left[2] > 200 && left[0] < 60 && left[3] == 0xFF);
        LUMEN_CHECK(right[0] > 200 && right[2] < 60 && right[3] == 0xFF);
        r.guards.release(j.target.shm);
    }
    // The session's end frees its slot (and a new one of the index starts afresh).
    lx::compositor::capture_job forget{};
    forget.kind = lx::compositor::capture_job_kind::forget_session;
    forget.session = 2;
    (void)r.channel.post(forget);
    r.renderer.take(r.channel);
    LUMEN_CHECK(f->vk.read_back_wait(1, 0) == nullptr);
}

LUMEN_TEST(vk_read_back_async_copies_only_the_rects_asked_for) {
    fixture* f = gpu();
    if (!f)
        return;
    const lx::rect2i rects[2] = {{0, 0, 8, 4}, {40, 30, 10, 10}};
    LUMEN_CHECK(static_cast<bool>(f->vk.read_back_async(5, f->target, rects, 2)));
    const unsigned char* rows = f->vk.read_back_wait(5, 2'000'000'000ull);
    LUMEN_CHECK(rows != nullptr);
    if (rows) {
        // B, G, R, A at each rect's own place: red on the left, blue on the right.
        const unsigned char* a = rows + (2u * k_w + 3u) * 4u;
        const unsigned char* b = rows + (35u * k_w + 45u) * 4u;
        LUMEN_CHECK(a[2] > 200 && a[0] < 60);
        LUMEN_CHECK(b[0] > 200 && b[2] < 60);
    }
    // Reused at once (the previous read waited for).
    LUMEN_CHECK(static_cast<bool>(f->vk.read_back_async(5, f->target, nullptr, 0)));
    LUMEN_CHECK(f->vk.read_back_wait(5, 2'000'000'000ull) != nullptr);
    f->vk.read_back_free(5);
    LUMEN_CHECK(f->vk.read_back_wait(5, 0) == nullptr);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
