module;

// Bodies of lx.app:window: open/close, the pixel path, configure and scale, the frame loop.

#include <time.h>

module lx.app;

import std;

import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.scene;
import lx.text;
import lx.trace;
import lx.ui;
import lx.wayland.client;

namespace lx::app {

namespace {

constexpr lx::trace::category log_window{"app.window"};

[[nodiscard]] lx::gfx::ui_stream stream_of(const lx::scene::draw_list& d) {
    lx::gfx::ui_stream s{};
    s.refs = d.refs();
    s.batches = d.batches();
    s.quads = d.quad_instances();
    s.shadows = d.shadow_instances();
    s.sprites = d.sprite_instances();
    s.underlines = d.underline_instances();
    return s;
}

[[nodiscard]] int ceil_scaled(int logical, float scale) {
    if (scale == 1.f)
        return logical;
    // The tolerance keeps an exact product (640 × 1.25) from rounding up a pixel on float
    // error.
    return static_cast<int>(std::ceil(static_cast<double>(logical) * scale - 1e-6));
}

} // namespace

/// The window as an lx::runtime::event_source: due when a frame should be drawn now; no fd,
/// no timeout of its own (GTK's frame clock is a GSource the same way).
struct window::frame_source final : public lx::runtime::event_source {
    explicit frame_source(window& w) : owner{w} {}
    bool prepare(lx::runtime::clock_time& timeout) override {
        const bool due = owner.frame_due();
        timeout = due ? 0 : -1;
        return due;
    }
    bool check() override { return owner.frame_due(); }
    bool dispatch() override {
        owner.run_frame();
        return true;
    }
    window& owner;
};

window::window() = default;

window::~window() { close(); }

std::uint64_t window::thread_cpu_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

std::uint64_t window::now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

lx::result<void> window::open(window_host& host, const window_options& options) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (open_)
        return lx::invalid_argument("lx::app::window::open: already open");
    if (!options.describe)
        return lx::invalid_argument("lx::app::window::open: no describe function");
    lx::wayland::display& d = host.host_display();
    if (!d.connected())
        return lx::make_error(lx::wayland_err::connect_failed,
                              "lx::app::window::open: the application is not connected");
    host_ = &host;
    options_ = options;
    timeline_ = {};
    timeline_.opened_ns = now_ns();
    stats_ = {};

    if (auto failed = surface_.create(d); !failed)
        return failed.get_error();
    surface_.set_handler(this);
    // Fractional scale needs both: the scale and a viewport to put the larger buffer in the
    // logical size (otherwise integer buffer_scale).
    if (d.has(lx::wayland::global_kind::fractional_scale) &&
        d.has(lx::wayland::global_kind::viewporter)) {
        fractional_ = surface_.enable_fractional_scale().ok() && surface_.enable_viewport().ok();
        viewport_ = fractional_;
    }
    lx::wayland::toplevel_options to{};
    to.title = options.title;
    to.app_id = options.app_id;
    to.server_side_decorations = options.server_side_decorations;
    if (auto made = toplevel_.create(d, surface_, to); !made) {
        surface_.destroy();
        return made;
    }
    toplevel_.set_handler(this);
    if (options.min_size.width > 0 || options.min_size.height > 0)
        toplevel_.set_min_size(options.min_size);

    // The UI: one atlas per window (its renderer uploads it), glyphs and icons in it.
    atlas_ = std::make_unique<lx::gfx::atlas>();
    glyphs_ = std::make_unique<lx::text::glyph_cache>(*atlas_);
    icons_ = std::make_unique<lx::ui::icon_cache>(*atlas_);
    if (host.host_settings().async_icons)
        icons_->set_async(host.host_worker());
    lx::ui::ui_resources resources{};
    resources.text = {{}, glyphs_.get(), nullptr};
    resources.icons = icons_.get();
    lx::ui::set_ui_resources(resources);
    arena_ = std::make_unique<lx::runtime::memory_arena>(static_cast<unsigned>(options.arena_bytes));
    root_ = std::make_unique<lx::ui::ui_root>(*arena_);
    root_->set_describer(options.describe);
    // A client keeps its last frame (the buffer's age is known): emit only what it repaints.
    root_->set_emit_scope(lx::ui::emit_scope::damage);
    draws_ = std::make_unique<lx::scene::draw_list>();

    input_.bind(d, surface_, *root_);
    source_ = std::make_unique<frame_source>(*this);
    host.host_loop().add_source(source_.get());
    open_ = true;
    host.host_attach(*this);
    // The initial commit (no buffer) goes out first: the compositor's first configure travels
    // while the GPU tier initialises (EGL / Vulkan device creation, tens of ms).
    (void)surface_.commit();
    ++stats_.commits;
    (void)d.flush();
    const std::uint64_t t0 = now_ns();
    if (auto made = make_pixel_path(); !made) {
        log_window.error(made.get_error());
        close();
        return made;
    }
    log_window.debug("pixel path: {} in {} µs", lx::gfx::client_tier_name(gpu_.tier()),
                     (now_ns() - t0) / 1000);
    return {};
}

void window::close() {
    if (host_ && open_)
        host_->host_detach(*this);
    if (host_ && source_)
        host_->host_loop().remove_source(source_.get());
    if (host_ && release_source_)
        host_->host_loop().remove_source(release_source_.get());
    source_.reset();
    release_source_.reset();
    input_.unbind();
    if (vk_)
        vk_->wait_idle();
    vk_.reset();
    gl_.reset();
    last_image_ = nullptr;
    swapchain_.destroy();
    for (buffer_slot& b : buffers_) {
        b.buffer.destroy();
        b.timeline.destroy();
        b.id = 0;
    }
    toplevel_.destroy();
    surface_.destroy();
    root_.reset();
    arena_.reset();
    draws_.reset();
    if (icons_ || glyphs_)
        lx::ui::set_ui_resources({});
    icons_.reset();
    glyphs_.reset();
    atlas_.reset();
    gpu_.destroy();
    open_ = false;
    configured_ = false;
    configure_pending_ = false;
    scale_pending_ = false;
    waiting_release_ = false;
    syncobj_ = false;
    broken_ = false;
    fractional_ = viewport_ = false;
    logical_ = buffer_ = viewport_dst_ = {};
    scale_ = 1.f;
    buffer_scale_ = 1;
    host_ = nullptr;
}

// ── the pixel path (ADR-010 §4.10) ──

lx::result<void> window::make_pixel_path() {
    lx::wayland::display& d = host_->host_display();
    // The compositor's dma-buf feedback (default; the main device and its tranches), copied
    // into the plain values lx.gfx takes.
    std::vector<lx::gfx::client_format> formats;
    std::vector<lx::gfx::client_tranche> tranches;
    lx::gfx::client_feedback feedback{};
    if (lx::wayland::dmabuf_feedback* fb = d.default_feedback(); fb && fb->received()) {
        const lx::wayland::dmabuf_feedback_snapshot& s = fb->snapshot();
        std::size_t total = 0;
        for (const auto& t : s.list())
            total += t.count;
        formats.reserve(total);
        for (const auto& t : s.list()) {
            const std::size_t first = formats.size();
            for (const auto& f : t.entries())
                formats.push_back({f.format, f.modifier});
            tranches.push_back({t.target_device, t.scanout(),
                                std::span<const lx::gfx::client_format>{formats.data() + first, t.count}});
        }
        feedback = {s.main_device, tranches};
    }
    const settings& cfg = host_->host_settings();
    lx::gfx::client_gpu_options o{};
    o.force = cfg.backend;
    o.allow_software = cfg.allow_software_gpu;
    auto made = gpu_.create(feedback, o);
    if (!made && cfg.backend != lx::gfx::client_tier::none) {
        // A forced tier this host cannot run: the automatic choice, as GTK falls back from
        // an unavailable GSK_RENDERER.
        log_window.warn("{} tier unavailable ({}), choosing automatically",
                        lx::gfx::client_tier_name(cfg.backend), lx::format_error(made.get_error()));
        o.force = lx::gfx::client_tier::none;
        o.read_environment = false;
        made = gpu_.create(feedback, o);
    }
    if (!made)
        return made.get_error();
    format_ = {};
    if (gpu_.tier() != lx::gfx::client_tier::cpu) {
        auto chosen = lx::gfx::choose_client_format(feedback, {.opaque = options_.opaque}, 0);
        if (!chosen) {
            log_window.warn("no dma-buf format for the {} tier ({}): wl_shm",
                            lx::gfx::client_tier_name(gpu_.tier()),
                            lx::format_error(chosen.get_error()));
            gpu_.destroy();
            if (auto failed = gpu_.create(feedback, {.force = lx::gfx::client_tier::cpu}); !failed)
                return failed.get_error();
        } else {
            format_ = chosen.value();
        }
    }
    swapchain_.set_listener(this);
    swapchain_.set_owner_affinity(lx::runtime::affinity::ui);
    return {};
}

lx::result<void> window::render(lx::gfx::swapchain_image& image,
                                const lx::gfx::damage_region& repaint) {
    const lx::gfx::ui_stream stream = stream_of(*draws_);
    // Each repainted rect starts from the window background (premultiplied, encoded).
    lx::color bg = lx::ui::active_theme().color(lx::ui::tok::background);
    bg = lx::color::rgb(bg.r * bg.a, bg.g * bg.a, bg.b * bg.a, bg.a);
    switch (gpu_.tier()) {
    case lx::gfx::client_tier::gl: {
        auto drawn = gl_->draw(image.gl_framebuffer, image.width, image.height, stream,
                               atlas_.get(), repaint, bg);
        if (!drawn)
            return drawn.get_error();
        return {};
    }
    case lx::gfx::client_tier::vulkan: {
        auto drawn = vk_->draw(image.vk, stream, atlas_.get(), repaint, bg);
        if (!drawn)
            return drawn.get_error();
        return {};
    }
    default: {
        auto drawn = cpu_.draw(image.pixels, stream, atlas_.get(), repaint, bg);
        if (!drawn)
            return drawn.get_error();
        return {};
    }
    }
}

lx::result<void> window::apply_geometry() {
    lx::wayland::display& d = host_->host_display();
    // The size: the configure's, else what the window has (the app's default at first).
    lx::size2i size = logical_.width > 0 ? logical_ : options_.size;
    if (configure_pending_) {
        if (pending_cfg_.size.width > 0)
            size.width = pending_cfg_.size.width;
        if (pending_cfg_.size.height > 0)
            size.height = pending_cfg_.size.height;
    }
    size.width = std::max({size.width, options_.min_size.width, 1});
    size.height = std::max({size.height, options_.min_size.height, 1});

    // The scale: fractional (1/120) with a viewport, else the integer buffer scale.
    const std::uint32_t s120 = surface_.preferred_scale120();
    float scale = 1.f;
    int bscale = 1;
    if (fractional_ && viewport_) {
        scale = static_cast<float>(s120) / 120.f;
    } else {
        bscale = static_cast<int>((s120 + 119) / 120);
        if (d.version(lx::wayland::global_kind::compositor) < 6) {
            // No preferred_buffer_scale before wl_compositor v6: the largest scale of the
            // outputs the surface is on, as GTK does.
            const std::uint32_t mask = surface_.outputs();
            for (unsigned slot = 0; slot < lx::wayland::k_max_outputs; ++slot)
                if (mask & (1u << slot))
                    if (const lx::wayland::output* o = d.output_at(slot))
                        bscale = std::max(bscale, static_cast<int>(o->info().scale));
        }
        bscale = std::max(bscale, 1);
        scale = static_cast<float>(bscale);
    }
    const lx::size2i buffer{ceil_scaled(size.width, scale), ceil_scaled(size.height, scale)};

    if (size.width != logical_.width || size.height != logical_.height) {
        root_->set_bounds({0, 0, size.width, size.height});
        if (options_.opaque) {
            const lx::rect2i opaque[1] = {{0, 0, size.width, size.height}};
            surface_.set_opaque_region(opaque);
        }
        logical_ = size;
    }
    if (scale != scale_ || !swapchain_.valid()) {
        if (scale != scale_)
            ++stats_.scale_changes;
        root_->set_scale(scale);
        scale_ = scale;
    }
    if (viewport_) {
        if (viewport_dst_.width != size.width || viewport_dst_.height != size.height) {
            surface_.set_viewport_destination(size);
            viewport_dst_ = size;
        }
    } else if (bscale != buffer_scale_) {
        surface_.set_buffer_scale(bscale);
        buffer_scale_ = bscale;
    }

    if (!swapchain_.valid()) {
        lx::gfx::swapchain_options so{};
        so.width = static_cast<unsigned>(buffer.width);
        so.height = static_cast<unsigned>(buffer.height);
        so.format = format_;
        so.opaque = options_.opaque;
        so.explicit_sync = d.has(lx::wayland::global_kind::syncobj);
        if (auto failed = swapchain_.create(gpu_, so); !failed)
            return failed.get_error();
        ++stats_.reallocations;
        if (gpu_.tier() == lx::gfx::client_tier::gl && !gl_) {
            gl_ = std::make_unique<lx::gfx::ui_gl_renderer>();
            // No program built ahead: the first draw builds what its stream needs (a hello
            // window: quads and glyphs), each program ≈ 1.8 MiB of driver memory on SVGA3D
            // and its compile the larger part of a cold first frame.
            lx::gfx::ui_gl_options go{};
            go.prebuild_kinds = 0;
            if (auto failed = gl_->initialize(go); !failed)
                return failed.get_error();
            gl_->set_owner_affinity(lx::runtime::affinity::ui);
        } else if (gpu_.tier() == lx::gfx::client_tier::vulkan && !vk_) {
            vk_ = std::make_unique<lx::gfx::ui_vk_renderer>();
            lx::gfx::ui_vk_options vo{};
            vo.format = lx::gfx::k_ui_vk_bgra8_unorm;
            if (auto failed = vk_->initialize(gpu_.vulkan_context(), vo); !failed)
                return failed.get_error();
            vk_->set_owner_affinity(lx::runtime::affinity::ui);
        }
        if (const int fd = swapchain_.release_fd(); fd >= 0 && !release_source_) {
            // Explicit sync: release points signal an eventfd, not wl_buffer.release.
            release_source_ = std::make_unique<lx::runtime::fd_source>(
                fd, lx::runtime::task{[this] { collect_releases(); }});
            host_->host_loop().add_source(release_source_.get());
        }
    } else if (buffer.width != buffer_.width || buffer.height != buffer_.height) {
        last_image_ = nullptr;
        auto resized = swapchain_.resize(static_cast<unsigned>(buffer.width),
                                         static_cast<unsigned>(buffer.height));
        if (!resized)
            return resized.get_error();
        if (resized.value())
            ++stats_.reallocations;
    }
    buffer_ = buffer;
    scale_pending_ = false;
    return {};
}

void window::collect_releases() {
    if (swapchain_.collect_releases() > 0)
        waiting_release_ = false;
}

window::buffer_slot* window::slot_of(std::uint32_t id) {
    for (buffer_slot& b : buffers_)
        if (b.id == id && id != 0)
            return &b;
    return nullptr;
}

void window::on_image_created(const lx::gfx::swapchain_image& image) {
    buffer_slot* slot = nullptr;
    for (buffer_slot& b : buffers_)
        if (b.id == 0) {
            slot = &b;
            break;
        }
    if (!slot) {
        log_window.error("swapchain image {}: no buffer slot left", image.id);
        return;
    }
    lx::wayland::display& d = host_->host_display();
    const lx::size2i size{static_cast<int>(image.width), static_cast<int>(image.height)};
    lx::result<void> made{};
    if (image.pixels.pixels) {
        // wl_shm over the swapchain's memfd: a pool per image, gone once its buffer exists.
        lx::wayland::shm_pool pool;
        made = pool.create(d, image.planes[0].fd, image.pool_bytes);
        if (made)
            made = slot->buffer.create_shm(pool, image.planes[0].offset, size,
                                           static_cast<std::int32_t>(image.planes[0].stride),
                                           image.fourcc == lx::gfx::k_fourcc_xrgb8888 ? 1u : 0u);
        pool.destroy();
    } else {
        lx::wayland::dmabuf_plane planes[4]{};
        for (unsigned p = 0; p < image.plane_count && p < 4; ++p)
            planes[p] = {image.planes[p].fd, image.planes[p].offset, image.planes[p].stride};
        made = slot->buffer.create_dmabuf(d, std::span{planes, image.plane_count}, size,
                                          image.fourcc, image.modifier);
    }
    if (!made) {
        log_window.error(made.get_error());
        return;
    }
    if (image.timeline_fd >= 0)
        if (auto t = slot->timeline.create(d, image.timeline_fd); !t)
            log_window.error(t.get_error());
    slot->buffer.set_handler(this);
    slot->id = image.id;
}

void window::on_image_destroyed(std::uint32_t id) {
    if (buffer_slot* slot = slot_of(id)) {
        slot->buffer.destroy();
        slot->timeline.destroy();
        slot->id = 0;
    }
}

void window::on_release(lx::wayland::buffer& b) {
    for (buffer_slot& s : buffers_)
        if (&s.buffer == &b && s.id != 0) {
            swapchain_.release(s.id);
            waiting_release_ = false;
            return;
        }
}

void window::on_failed(lx::wayland::buffer& b) {
    (void)b;
    // The compositor could not import a dma-buf: fail loudly once; the window stops drawing
    // rather than committing buffers it cannot show.
    if (!broken_)
        log_window.error("the compositor refused a {} buffer (zwp_linux_buffer_params_v1.failed)",
                         lx::gfx::client_tier_name(gpu_.tier()));
    broken_ = true;
}

// ── configure, scale, callbacks ──

void window::on_configure(lx::wayland::toplevel& t, const lx::wayland::toplevel_configure& c) {
    (void)t;
    ++stats_.configures;
    if (timeline_.first_configure_ns == 0)
        timeline_.first_configure_ns = now_ns();
    pending_cfg_ = c;
    configure_pending_ = true;
}

void window::on_close(lx::wayland::toplevel& t) {
    (void)t;
    if (host_)
        host_->host_close_requested(*this);
}

void window::on_frame(lx::wayland::surface& s, std::uint32_t time_ms) {
    (void)s;
    (void)time_ms;
    ++stats_.frame_callbacks;
}

void window::on_preferred_scale(lx::wayland::surface& s, std::uint32_t scale120) {
    (void)s;
    (void)scale120;
    // Answered with a frame at the new scale (at once: the content is wrong until then).
    scale_pending_ = true;
}

void window::on_presentation(lx::wayland::surface& s, const lx::wayland::presentation_event& e) {
    (void)s;
    const std::uint64_t n = std::min<std::uint64_t>(ring_next_, k_frame_ring);
    for (std::uint64_t i = 1; i <= n; ++i) {
        frame_record& r = ring_[(ring_next_ - i) % k_frame_ring];
        if (r.commit_id != e.commit_id)
            continue;
        r.presented = e.presented;
        r.discarded = !e.presented;
        r.presented_ns = e.time_ns;
        r.refresh_ns = e.refresh_ns;
        r.sequence = e.sequence;
        r.flags = e.flags;
        break;
    }
    if (e.presented) {
        ++stats_.presented;
        if (timeline_.first_presented_ns == 0)
            timeline_.first_presented_ns =
                host_ && host_->host_display().presentation_clock() == 1 ? e.time_ns : now_ns();
    } else {
        ++stats_.discarded;
    }
}

const frame_record& window::frame_at(unsigned i) const {
    const std::uint64_t n = std::min<std::uint64_t>(ring_next_, k_frame_ring);
    const std::uint64_t first = ring_next_ - n;
    return ring_[(first + i) % k_frame_ring];
}

unsigned window::frame_count() const {
    return static_cast<unsigned>(std::min<std::uint64_t>(ring_next_, k_frame_ring));
}

const lx::gfx::pixel_surface* window::cpu_pixels() const {
    if (gpu_.tier() != lx::gfx::client_tier::cpu || !last_image_ || !last_image_->pixels.pixels)
        return nullptr;
    return &last_image_->pixels;
}

std::uint64_t window::next_deadline_ns(std::uint64_t now) const {
    if (!open_)
        return 0;
    std::uint64_t out = input_.repeat_deadline_ns();
    // A UI timer already due is the frame loop's (now, or at the frame callback it waits
    // for): arming the timer for the past would only spin.
    if (const std::uint64_t ui = root_->next_deadline_ns(); ui > now && (out == 0 || ui < out))
        out = ui;
    return out;
}

void window::on_deadline(std::uint64_t now) { input_.on_repeat_due(now); }

// ── the frame loop (ADR-010 §4.9) ──

bool window::frame_due() const {
    if (!open_ || broken_ || waiting_release_)
        return false;
    if (configure_pending_ || (configured_ && scale_pending_))
        return true;
    if (!configured_)
        return false;
    // Throttled: the outstanding callback is the next frame.
    if (surface_.frame_pending())
        return false;
    return input_.motion_pending() || continuous_ || root_->wants_frame();
}

void window::run_frame() {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    frame_start_ns_ = now_ns();
    frame_start_cpu_ns_ = thread_cpu_ns();
    // The frame's one hit test: the latest pointer position.
    input_.flush_motion();
    frame_input_ns_ = now_ns() - frame_start_ns_;
    if (!configure_pending_ && !scale_pending_ && !continuous_ && !root_->wants_frame())
        return;
    (void)draw();
}

void window::ack_pending() {
    if (!configure_pending_)
        return;
    toplevel_.ack_configure(pending_cfg_.serial);
    configure_pending_ = false;
    configured_ = true;
    ++stats_.acks;
}

void window::restore_damage() {
    // §4.9: the ledger is cleared only by a successful commit.
    lx::ui::damage_ledger& ledger = root_->damage();
    const lx::ui::damage_ledger& lost = root_->frame_damage();
    if (lost.count() == 0)
        ledger.add(root_->bounds());
    for (unsigned i = 0; i < lost.count(); ++i)
        ledger.add(lost.rects()[i]);
    ledger.request(lx::ui::invalidation::paint);
}

bool window::draw() {
    // The first frame's stages, logged once at debug (§8.2 "Start-up": where G7's time goes).
    const bool first = stats_.frames == 0;
    const std::uint64_t t_begin = now_ns();
    if (auto g = apply_geometry(); !g) {
        log_window.error(g.get_error());
        broken_ = true;
        return false;
    }
    const std::uint64_t t_geometry = now_ns();
    auto acquired = swapchain_.acquire();
    if (!acquired) {
        log_window.error(acquired.get_error());
        ++stats_.failures;
        broken_ = true;
        return false;
    }
    if (!acquired.value()) {
        // Every image is with the compositor: draw once one comes back.
        waiting_release_ = true;
        ++stats_.no_image;
        return false;
    }
    lx::gfx::swapchain_image& image = *acquired.value();
    if (swapchain_.explicit_sync() && !syncobj_) {
        // Explicit sync settles with the first image (lx.gfx:swapchain); the syncobj surface
        // exists from then on, and every commit with a buffer carries its two points.
        if (auto e = surface_.enable_syncobj(); e)
            syncobj_ = true;
        else
            log_window.error(e.get_error());
    }
    const std::uint64_t t_acquired = now_ns();
    atlas_->begin_frame(++atlas_frame_);

    // What the image lacks: the damage of the frames since it was last drawn.
    lx::gfx::damage_region age{};
    swapchain_.add_age_damage(image, age);
    if (age.full || image.age == 0) {
        root_->add_repaint(root_->bounds());
    } else {
        for (unsigned i = 0; i < age.count; ++i)
            root_->add_repaint(lx::ui::unproject_rect(age.rects[i], scale_));
    }
    if (continuous_)
        root_->damage().add(root_->bounds());

    const std::uint64_t t_ticking = now_ns();
    const lx::ui::layout_counters layout_before = lx::ui::ui_layout_counters();
    if (!root_->tick(*draws_)) {
        swapchain_.cancel(image);
        ++stats_.empty_ticks;
        if (configure_pending_) {
            // A configure that changed nothing the UI draws (a state bit): acked, committed.
            ack_pending();
            (void)surface_.commit();
            ++stats_.commits;
        }
        return false;
    }
    const std::uint64_t t_ticked = now_ns();
    draws_->batch_for_render();
    const std::uint64_t t_ordered = now_ns();

    // Device damage: what changed (the compositor is told) and what is repainted.
    const lx::rect2i whole{0, 0, buffer_.width, buffer_.height};
    lx::gfx::damage_region change{};
    lx::gfx::damage_region repaint{};
    const lx::ui::damage_ledger& changed = root_->frame_damage();
    if (changed.count() == 0)
        change.set_full();
    for (unsigned i = 0; i < changed.count(); ++i)
        change.add(lx::ui::project_rect(changed.rects()[i], scale_));
    const lx::ui::damage_ledger& painted = root_->frame_repaint();
    for (unsigned i = 0; i < painted.count(); ++i)
        repaint.add(lx::ui::project_rect(painted.rects()[i], scale_));
    if (change.full || image.age == 0)
        repaint.set_full();
    change.clip(whole);
    repaint.clip(whole);

    const std::uint64_t t_render = now_ns();
    if (auto r = render(image, repaint); !r) {
        log_window.error(r.get_error());
        restore_damage();
        swapchain_.cancel(image);
        ++stats_.failures;
        broken_ = true;
        return false;
    }
    const std::uint64_t t_rendered = now_ns();
    if (auto p = swapchain_.present(image, change); !p) {
        log_window.error(p.get_error());
        restore_damage();
        swapchain_.cancel(image);
        ++stats_.failures;
        broken_ = true;
        return false;
    }
    const std::uint64_t t_presented = now_ns();
    buffer_slot* slot = slot_of(image.id);
    if (!slot) {
        log_window.error("swapchain image {} has no wl_buffer", image.id);
        restore_damage();
        ++stats_.failures;
        broken_ = true;
        return false;
    }
    if (first && log_window.enabled(lx::trace::level::debug)) {
        const std::uint64_t t_end = now_ns();
        const lx::ui::ui_root::stage_times st = root_->last_stage_times();
        log_window.debug("first frame {}×{} ({}): geometry+swapchain+acquire {} µs, tick {} µs "
                         "(describe {} reconcile {} layout {} emit {}), render+present {} µs",
                         buffer_.width, buffer_.height, lx::gfx::client_tier_name(gpu_.tier()),
                         (t_ticking - t_begin) / 1000, (t_render - t_ticking) / 1000,
                         st.describe / 1000, st.reconcile / 1000, st.layout / 1000, st.emit / 1000,
                         (t_end - t_render) / 1000);
    }
    // The configure this frame answers: acked right before the commit that shows it.
    ack_pending();
    surface_.attach(&slot->buffer);
    for (unsigned i = 0; i < change.count; ++i)
        surface_.damage_buffer(change.rects[i]);
    if (syncobj_ && slot->timeline.valid()) {
        surface_.set_acquire_point(slot->timeline, image.acquire_point);
        surface_.set_release_point(slot->timeline, image.release_point);
    }
    // The next frame's throttle: it fires once; with nothing left to draw it asks for no more.
    if (!surface_.frame_pending()) {
        surface_.request_frame();
        ++stats_.frame_requests;
    }
    (void)surface_.request_presentation_feedback();
    const std::uint64_t commit_id = surface_.commit();
    const std::uint64_t committed = now_ns();
    ++stats_.commits;
    ++stats_.frames;
    if (timeline_.first_commit_ns == 0)
        timeline_.first_commit_ns = committed;

    frame_record& rec = ring_[ring_next_++ % k_frame_ring];
    rec = {};
    rec.commit_id = commit_id;
    rec.commit_ns = committed;
    rec.start_ns = frame_start_ns_;
    rec.cpu_ns = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(thread_cpu_ns() - frame_start_cpu_ns_, 0xFFFF'FFFFu));
    rec.damage_rects = static_cast<std::uint16_t>(change.count);
    rec.width = static_cast<std::uint16_t>(buffer_.width);
    rec.height = static_cast<std::uint16_t>(buffer_.height);
    {
        const lx::ui::ui_root::stage_times st = root_->last_stage_times();
        const auto ns32 = [](std::uint64_t ns) {
            return static_cast<std::uint32_t>(std::min<std::uint64_t>(ns, 0xFFFF'FFFFu));
        };
        const lx::ui::layout_counters& lc = lx::ui::ui_layout_counters();
        rec.input_ns = ns32(frame_input_ns_);
        rec.damage_px = static_cast<std::uint32_t>(std::min<std::uint64_t>(change.area(), 0xFFFF'FFFFu));
        rec.geometry_ns = ns32(t_geometry - t_begin);
        rec.acquire_ns = ns32(t_acquired - t_geometry);
        rec.tick_ns = ns32(t_ticked - t_ticking);
        rec.describe_ns = ns32(st.describe);
        rec.reconcile_ns = ns32(st.reconcile);
        rec.layout_ns = ns32(st.layout);
        rec.emit_ns = ns32(st.emit);
        rec.order_ns = ns32(t_ordered - t_ticked);
        rec.render_ns = ns32(t_rendered - t_render);
        rec.present_ns = ns32(t_presented - t_rendered);
        rec.measures = static_cast<std::uint32_t>(lc.measures - layout_before.measures);
        rec.arranges = static_cast<std::uint32_t>(lc.arranges - layout_before.arranges);
    }
    last_damage_ = change;
    stats_.last_damage_rects = change.count;
    stats_.last_repaint_rects = repaint.count;
    stats_.last_damage_px = change.area();
    stats_.last_damage_bounds = change.bounds();
    last_image_ = &image;
    // The field's caret moved or its text changed with this frame: the input method hears.
    input_.after_frame();
    return true;
}

} // namespace lx::app
