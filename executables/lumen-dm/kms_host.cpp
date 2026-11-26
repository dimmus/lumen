#include <errno.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

import lx.drm;
import lx.trace;

#include "kms_host.hpp"

namespace lumen::dm {
namespace {

namespace drm = lx::drm;
namespace rt = lx::runtime;

constexpr lx::trace::category log_kms{"dm.kms"};
constexpr auto k_format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
constexpr int k_canvas_width = 468;
constexpr int k_canvas_height = 360;
constexpr int k_cursor_width = 12;
constexpr int k_cursor_height = 19;
constexpr rt::clock_time k_flip_timeout = 500'000'000;

// A premultiplied ARGB arrow with a hotspot at (0, 0). The same sprite is used on
// a KMS cursor plane and on the primary damage path.
constexpr auto k_cursor = [] {
    std::array<std::uint32_t, k_cursor_width * k_cursor_height> image{};
    constexpr std::string_view rows[] = {
        "#",           "##",           "#.#",        "#..#",      "#...#",
        "#....#",      "#.....#",      "#......#",   "#.......#", "#........#",
        "#.........#", "#......#####", "#...#..#",   "#..##..#",  "#.#  #..#",
        "##   #..#",   "#     #..#",   "      #..#", "       ##",
    };
    for (unsigned y = 0; y < std::size(rows); ++y) {
        for (unsigned x = 0; x < rows[y].size(); ++x) {
            if (rows[y][x] != ' ')
                image[y * k_cursor_width + x] = rows[y][x] == '#' ? 0xff000000u : 0xffffffffu;
        }
    }
    return image;
}();

[[nodiscard]] constexpr lx::rect2i intersect(lx::rect2i a, lx::rect2i b) {
    const int x = std::max(a.x, b.x);
    const int y = std::max(a.y, b.y);
    return {x, y, std::max(0, std::min(a.x + a.width, b.x + b.width) - x),
            std::max(0, std::min(a.y + a.height, b.y + b.height) - y)};
}

[[nodiscard]] constexpr bool same_point(lx::point2i a, lx::point2i b) {
    return a.x == b.x && a.y == b.y;
}

// Add only the part not covered by previous rectangles. At most three input rects
// (form, old pointer, new pointer), so disjoint output fits the KMS clip array.
constexpr void add_clip(drm::kms_damage_region& damage, lx::rect2i r, unsigned previous) {
    if (r.size().empty() || damage.full_frame)
        return;
    for (unsigned i = 0; i < previous; ++i) {
        const auto hit = intersect(r, damage.rects[i]);
        if (hit.size().empty())
            continue;
        const int right = r.x + r.width;
        const int bottom = r.y + r.height;
        const int hit_right = hit.x + hit.width;
        const int hit_bottom = hit.y + hit.height;
        add_clip(damage, {r.x, r.y, r.width, hit.y - r.y}, previous);
        add_clip(damage, {r.x, hit_bottom, r.width, bottom - hit_bottom}, previous);
        add_clip(damage, {r.x, hit.y, hit.x - r.x, hit.height}, previous);
        add_clip(damage, {hit_right, hit.y, right - hit_right, hit.height}, previous);
        return;
    }
    if (damage.count == std::size(damage.rects)) {
        damage.full_frame = true;
        damage.count = 0;
    } else {
        damage.rects[damage.count++] = r;
    }
}

struct pixels_version {
    std::uint64_t form = 0;
    lx::point2i pointer{};
    bool software_pointer = false;
    std::uint64_t form_revision = 0;
};

[[nodiscard]] constexpr lx::rect2i cursor_rect(lx::point2i position) {
    return {position.x, position.y, k_cursor_width, k_cursor_height};
}

[[nodiscard]] constexpr drm::kms_damage_region pixel_damage(pixels_version old, pixels_version next,
                                                            lx::rect2i form, lx::rect2i output) {
    drm::kms_damage_region damage{};
    damage.full_frame = old.form == 0;
    if (damage.full_frame)
        return damage;
    if (old.form != next.form)
        add_clip(damage, form, damage.count);
    if (old.software_pointer != next.software_pointer || !same_point(old.pointer, next.pointer)) {
        if (old.software_pointer)
            add_clip(damage, intersect(cursor_rect(old.pointer), output), damage.count);
        if (next.software_pointer)
            add_clip(damage, intersect(cursor_rect(next.pointer), output), damage.count);
    }
    return damage;
}

// Verify distinct buffer age and visible age, clipped cursor damage, and overlap removal.
static_assert([] {
    constexpr lx::rect2i output{0, 0, 64, 64};
    constexpr lx::rect2i form{16, 16, 32, 32};
    constexpr pixels_version a{1, {20, 20}, true};
    constexpr pixels_version b{1, {60, 60}, true};
    const auto pointer = pixel_damage(a, b, form, output);
    const auto repaint = pixel_damage(a, {2, {60, 60}, true}, form, output);
    return pixel_damage({}, a, form, output).full_frame && !pointer.full_frame &&
           pointer.count == 2 && pointer.rects[1].width == 4 && pointer.rects[1].height == 4 &&
           repaint.count == 2 && repaint.rects[0].width == form.width &&
           pixel_damage(b, b, form, output).count == 0;
}());

[[nodiscard]] lx::result<drm::kms_pipeline_desc> choose_pipeline(const drm::kms_device& device,
                                                                 const drm::connector_probe& c) {
    drm::crtc_alloc_input in{};
    in.connector_count = 1;
    in.connectors[0].possible_crtcs = c.possible_crtcs;
    in.connectors[0].routed_crtc = device.crtc_index_of(c.current_crtc_id);
    in.crtc_count = device.crtc_count();
    in.plane_count = device.plane_count();
    for (unsigned i = 0; i < in.plane_count; ++i) {
        const auto p = device.plane(i);
        in.planes[i] = {p.kind, p.possible_crtcs, device.crtc_index_of(p.crtc_id),
                        p.kind == drm::plane_kind::primary ? p.xrgb8888 : p.argb8888};
    }
    drm::crtc_alloc_result out{};
    if (!drm::assign_crtcs(in, out, nullptr, nullptr))
        return lx::make_error(lx::drm_err::no_crtc, "no XRGB8888 greeter scanout pipeline");
    const auto crtc = device.crtc(static_cast<unsigned>(out.crtc[0]));
    const auto plane = device.plane(static_cast<unsigned>(out.primary_plane[0]));
    return drm::kms_pipeline_desc{crtc.id, crtc.index, plane.id, c.connector_id};
}

[[nodiscard]] const drm::connector_probe* preferred_connector(const drm::connector_scan& scan,
                                                              unsigned keep = 0) {
    const drm::connector_probe* best = nullptr;
    for (unsigned i = 0; i < scan.count; ++i) {
        const auto& c = scan.entries[i];
        if (c.status != drm::connector_status::connected || c.non_desktop || !c.preferred_mode() ||
            !c.preferred_mode()->valid())
            continue;
        if (keep && c.connector_id == keep)
            return &c;
        if (!best || (c.current_crtc_id && !best->current_crtc_id))
            best = &c;
    }
    return best;
}

// A denied or broken secondary card needn't prevent waiting for a display on one
// we could probe. Without any viable card, retain the actual error.
[[nodiscard]] constexpr std::optional<lx::error>
probe_failure(const std::optional<lx::error>& failure, bool viable_without_output) {
    return viable_without_output ? std::nullopt : failure;
}

static_assert([] {
    constexpr auto denied = lx::make_error(lx::io_err::permission_denied, "test card denied");
    return !probe_failure({}, false) && !probe_failure(denied, true) &&
           probe_failure(denied, false)->code == denied.code;
}());

[[nodiscard]] int output_scale(const drm::connector_probe& c, const drm::kms_mode& mode) {
    // At least 180 dpi on both axes, with room for the form. Ignore missing EDID sizes.
    return c.phys_w_mm && c.phys_h_mm && mode.width >= 2 * k_canvas_width &&
                   mode.height >= 2 * k_canvas_height &&
                   mode.width * 254ull >= c.phys_w_mm * 1800ull &&
                   mode.height * 254ull >= c.phys_h_mm * 1800ull
               ? 2
               : 1;
}

} // namespace

struct kms_host::state {
    state(rt::event_loop& event_loop, lx::session::logind_session& logind, kms_host_callbacks hooks)
        : loop{event_loop},
          session{logind},
          callbacks{hooks} {
        work.set_enabled(false);
    }

    rt::event_loop& loop;
    lx::session::logind_session& session;
    kms_host_callbacks callbacks;
    drm::kms_device card{};
    std::unique_ptr<drm::kms_pipeline> pipe;
    drm::kms_cursor_plane cursor{};
    drm::uevent_monitor monitor{};
    dev_t devnum = 0;
    rt::fd_source flips{-1,
                        {[](void* self) { static_cast<state*>(self)->dispatch_flips(); }, this}};
    rt::fd_source hotplug{-1,
                          {[](void* self) { static_cast<state*>(self)->hotplug_ready(); }, this}};
    rt::timer_source work{0, {[](void* self) { static_cast<state*>(self)->tick(); }, this}, false};
    struct scanout {
        drm::kms_dumb_framebuffer fb;
        pixels_version contents{};
    } buffers[2];
    std::vector<std::uint32_t> canvas;
    lx::rect2i display{};
    lx::rect2i form{};
    lx::point2i pointer{};
    int scale = 1;
    int front = -1;
    int pending = -1; // -1 for a cursor-only flip; kms_pipeline tracks whether one is pending.
    std::uint64_t generation = 0;
    std::uint64_t requested_revision = 0;
    std::uint64_t cached_revision = 0;
    std::uint64_t frame_bytes = 0;
    rt::clock_time flip_deadline = 0;
    bool started = false;
    bool paused = false;
    bool resume_due = false;
    bool recovered_stall = false;
    bool paint_due = true;
    bool probe_due = false;
    bool gone = false;
    bool first_frame_logged = false;
    bool pointer_set = false;
    bool visible_content = false;
    bool pending_current_content = false;

    void invalidate_visibility() { pending_current_content = visible_content = false; }

    void schedule() {
        if (!started || paused || (!pipe && !probe_due && !resume_due))
            return;
        // Leave the watchdog armed while a normal flip owns both buffers. Any dirt
        // was already latched by the caller; completion will schedule the next frame.
        if (pipe && pipe->flip_pending() && !gone)
            return;
        work.schedule_at(loop.now());
    }

    [[nodiscard]] lx::result<void> watch(dev_t number) {
        if (monitor.fd() >= 0 && monitor.card() == static_cast<unsigned long long>(number))
            return {};
        if (monitor.fd() >= 0)
            loop.remove_source(&hotplug);
        hotplug = {-1, {[](void* self) { static_cast<state*>(self)->hotplug_ready(); }, this}};
        monitor = {};
        auto opened = drm::uevent_monitor::open(number);
        if (!opened)
            return opened.get_error();
        monitor = std::move(*opened);
        hotplug.set_fd(monitor.fd());
        loop.add_source(&hotplug);
        return {};
    }

    void hotplug_ready() {
        const auto batch = monitor.drain();
        gone = gone || batch.card_removed;
        probe_due = probe_due || batch.hotplug || gone || (!pipe && batch.events > 0);
        if (probe_due)
            schedule();
    }

    void dispatch_flips() {
        if (card.card_fd() >= 0)
            card.dispatch_events();
    }

    void flipped(drm::page_flip_event event) {
        flip_deadline = 0;
        recovered_stall = false;
        if (event.content && pending >= 0) {
            front = pending;
            pending = -1;
            const bool current = std::exchange(pending_current_content, false) && !paused && !gone;
            if (current && event.presented) {
                visible_content = true;
                if (!first_frame_logged) {
                    first_frame_logged = true;
                    log_kms.info("first_scanout timestamp_ns={} bytes_written={} hw={}",
                                 event.timestamp_ns, frame_bytes, event.hw);
                }
                const auto revision = buffers[front].contents.form_revision;
                if (revision && callbacks.presented)
                    callbacks.presented(callbacks.user, revision);
            }
        }
        if (resume_due || probe_due || paint_due || cursor.dirty() ||
            (!cursor.valid() && front >= 0 &&
             !same_point(pointer, buffers[front].contents.pointer)))
            schedule();
        else
            work.set_enabled(false);
    }

    void retire(bool handoff = false, bool disconnected = false) {
        invalidate_visibility();
        if (card.card_fd() < 0)
            return;
        // No more drmHandleEvent calls on this file description. The kernel can still
        // queue an event with the soon-to-be-dead cookie, but it cannot dereference it.
        loop.remove_source(&flips);
        flips = {-1, {[](void* self) { static_cast<state*>(self)->dispatch_flips(); }, this}};
        if (handoff && card.supports_closefb() && card.is_master()) {
            // A pending flip may not yet have latched. Keep both the old front and the
            // requested FB alive without exporting their IDs beyond this file.
            const int last = pending >= 0 ? pending : front;
            if (last >= 0 && buffers[last].fb.hand_over() == drm::fb_released::closed) {
                if (front >= 0 && front != last)
                    (void)buffers[front].fb.hand_over();
                pipe->forget_saved_state();
            }
        }
        if (pipe && disconnected)
            pipe->forget_saved_state();
        pipe.reset(); // restore before removing the FB; cursor/scanout still own their objects
        cursor = {};
        for (auto& b : buffers)
            b = {};
        card = {};
        session.release_device(major(devnum), minor(devnum));
        devnum = 0;
        display = {};
        form = {};
        front = pending = -1;
        flip_deadline = 0;
    }

    [[nodiscard]] lx::result<void> configure(const drm::connector_probe& c,
                                             const drm::kms_pipeline_desc& desc) {
        invalidate_visibility();
        const auto& mode = *c.preferred_mode();
        for (auto& b : buffers) {
            auto fb = drm::kms_dumb_framebuffer::create(card, mode.width, mode.height, k_format);
            if (!fb)
                return fb.get_error();
            b.fb = std::move(*fb);
        }
        pipe = std::make_unique<drm::kms_pipeline>(card, desc);
        pipe->set_mode(mode);
        pipe->set_page_flip_handler([](drm::page_flip_event event,
                                       void* user) { static_cast<state*>(user)->flipped(event); },
                                    this);
        display = {0, 0, static_cast<int>(mode.width), static_cast<int>(mode.height)};
        scale = output_scale(c, mode);
        form.width = std::min(display.width, k_canvas_width * scale);
        form.height = std::min(display.height, k_canvas_height * scale);
        form.x = (display.width - form.width) / 2;
        form.y = (display.height - form.height) / 2;
        canvas.resize(static_cast<std::size_t>(form.width) * form.height);
        if (!pointer_set)
            pointer = {display.width / 2, display.height / 2};
        clamp_pointer();
        if (auto hw = drm::kms_cursor_plane::create(card.card_fd(), desc.crtc_id, desc.crtc_index,
                                                    k_cursor_width, k_cursor_height);
            hw && hw->stage_image(k_cursor.data(), k_cursor_width, k_cursor_height, k_cursor_width,
                                  0, 0)) {
            cursor = std::move(*hw);
            cursor.set_visible(true);
            cursor.set_position(pointer.x, pointer.y);
        }
        paint_due = true;
        return callbacks.configure(callbacks.user, form);
    }

    void clamp_pointer() {
        pointer.x = std::clamp(pointer.x, 0, std::max(0, display.width - 1));
        pointer.y = std::clamp(pointer.y, 0, std::max(0, display.height - 1));
    }

    [[nodiscard]] lx::result<void> acquire_or_wait() {
        // Subscribe before discovery so a connector appearing during the scan cannot
        // strand a displayless greeter. A zero devnum monitors every DRM card.
        if (auto watched = watch(0); !watched)
            return watched.get_error();
        const auto candidates = drm::find_cards();
        std::optional<lx::error> failure;
        bool viable_without_output = false;
        for (unsigned i = 0; i < candidates.count; ++i) {
            const auto& candidate = candidates.cards[i];
            struct stat node{};
            if (::stat(candidate.path, &node) != 0) {
                if (errno != ENOENT && errno != ENODEV)
                    failure = lx::make_error(lx::drm_err::open_failed, "cannot inspect DRM node");
                continue;
            }
            if (!S_ISCHR(node.st_mode)) {
                failure =
                    lx::make_error(lx::drm_err::open_failed, "DRM node is not a character device");
                continue;
            }
            bool inactive = false;
            auto fd = session.take_device_path(candidate.path, &inactive);
            if (!fd) {
                failure = fd.get_error();
                continue;
            }
            struct stat taken{};
            if (::fstat(fd->get(), &taken) != 0 || !S_ISCHR(taken.st_mode) ||
                taken.st_rdev != node.st_rdev ||
                !session.holds_device(major(taken.st_rdev), minor(taken.st_rdev))) {
                session.release_device(major(node.st_rdev), minor(node.st_rdev));
                failure = lx::make_error(lx::io_err::permission_denied,
                                         "greeter DRM fd is not held through logind");
                continue;
            }
            auto opened = drm::kms_device::open_fd(std::move(*fd));
            if (!opened) {
                session.release_device(major(node.st_rdev), minor(node.st_rdev));
                failure = opened.get_error();
                continue;
            }
            card = std::move(*opened);
            devnum = taken.st_rdev;
            flips.set_fd(card.card_fd());
            loop.add_source(&flips);
            if (inactive) {
                paused = true;
            } else if (!card.is_master()) {
                failure = lx::make_error(lx::drm_err::open_failed, "logind KMS fd has no master");
                retire();
                continue;
            } else if (const auto* c = preferred_connector(card.last_scan())) {
                auto desc = choose_pipeline(card, *c);
                if (!desc) {
                    viable_without_output = true;
                    retire();
                    continue; // No compatible output on this card.
                }
                if (auto configured = configure(*c, *desc); !configured) {
                    retire();
                    return configured.get_error();
                }
            } else {
                viable_without_output = true;
                retire();
                continue; // No connected output on this card.
            }
            if (auto watched = watch(devnum); !watched) {
                retire();
                return watched.get_error();
            }
            return {};
        }
        if (const auto fatal = probe_failure(failure, viable_without_output))
            return *fatal;
        // No usable output is recoverable. The wildcard watch is already installed.
        return {};
    }

    [[nodiscard]] bool pipeline_still_usable(const drm::connector_probe& c) const {
        const auto& d = pipe->desc();
        if (card.crtc_index_of(d.crtc_id) != static_cast<int>(d.crtc_index) ||
            !(c.possible_crtcs & (1u << d.crtc_index)))
            return false;
        for (unsigned i = 0; i < card.plane_count(); ++i) {
            const auto p = card.plane(i);
            if (p.id == d.primary_plane_id)
                return p.kind == drm::plane_kind::primary && p.xrgb8888 &&
                       (p.possible_crtcs & (1u << d.crtc_index));
        }
        return false;
    }

    [[nodiscard]] lx::result<void> reconfigure(bool resumed) {
        if (gone) {
            retire(false, true);
            gone = false;
        }
        if (card.card_fd() >= 0) {
            if (auto resources = card.enumerate_resources(); !resources)
                return resources.get_error();
            // probe_connectors records a copy; keep the large probe off the stack.
            auto scan = std::make_unique<drm::connector_scan>();
            if (auto probed = card.probe_connectors(0, *scan); !probed)
                return probed.get_error();
            const auto* c = preferred_connector(*scan, pipe ? pipe->connector_id() : 0);
            if (!pipe && c) {
                if (auto desc = choose_pipeline(card, *c); desc)
                    return configure(*c, *desc);
            }
            if (pipe && c && c->connector_id == pipe->connector_id() && pipeline_still_usable(*c) &&
                pipe->mode().same_timings(*c->preferred_mode()) &&
                scale == output_scale(*c, *c->preferred_mode())) {
                if (resumed || c->link_bad) {
                    invalidate_visibility();
                    pipe->prepare_resume();
                    cursor.invalidate();
                }
                return {};
            }
            // A different CRTC/connector or mode may need new properties and buffer sizes.
            // Reopen via logind rather than reusing a file with old cookie addresses in it.
            retire(false, !c || (pipe && c->connector_id != pipe->connector_id()));
        }
        return acquire_or_wait();
    }

    [[nodiscard]] lx::result<void> paint() {
        const lx::gfx::pixel_surface target{
            reinterpret_cast<unsigned char*>(canvas.data()), static_cast<unsigned>(form.width),
            static_cast<unsigned>(form.height), static_cast<unsigned>(form.width) * 4u, k_format};
        std::ranges::fill(canvas, callbacks.background_xrgb);
        paint_due = false;
        const auto revision = requested_revision;
        if (auto painted = callbacks.paint(callbacks.user, target); !painted) {
            paint_due = true;
            return painted.get_error();
        }
        cached_revision = revision;
        // Each successful repaint has its own generation, even with identical pixels.
        // A revision-only update still gets a primary flip and a completion.
        ++generation;
        return {};
    }

    // Copy sequential spans to WC memory; no reads from scanout, and no full-screen CPU
    // image. Rows outside the cursor interpolate only between fill and cached-form copy.
    void write_base(std::uint32_t* dst, int y, int left, int right) const {
        if (y < form.y || y >= form.y + form.height || right <= form.x ||
            left >= form.x + form.width) {
            std::fill_n(dst, right - left, callbacks.background_xrgb);
            return;
        }
        const int a = std::max(left, form.x);
        const int b = std::min(right, form.x + form.width);
        std::fill_n(dst, a - left, callbacks.background_xrgb);
        std::memcpy(dst + a - left, canvas.data() + (y - form.y) * form.width + a - form.x,
                    static_cast<std::size_t>(b - a) * sizeof(*dst));
        std::fill_n(dst + b - left, right - b, callbacks.background_xrgb);
    }

    [[nodiscard]] std::uint64_t update(scanout& b, pixels_version want) const {
        const auto damage = pixel_damage(b.contents, want, form, display);
        const auto sprite = want.software_pointer ? cursor_rect(want.pointer) : lx::rect2i{};
        std::uint64_t bytes = 0;
        for (unsigned i = 0; i < (damage.full_frame ? 1u : damage.count); ++i) {
            const auto r = damage.full_frame ? display : damage.rects[i];
            const auto overlap = intersect(r, sprite);
            for (int y = r.y; y < r.y + r.height; ++y) {
                auto* dst = reinterpret_cast<std::uint32_t*>(
                    b.fb.pixels() + static_cast<std::size_t>(y) * b.fb.stride());
                if (overlap.size().empty() || y < overlap.y || y >= overlap.y + overlap.height) {
                    write_base(dst + r.x, y, r.x, r.x + r.width);
                    continue;
                }
                const int end = overlap.x + overlap.width;
                write_base(dst + r.x, y, r.x, overlap.x);
                const auto* src =
                    k_cursor.data() + ((y - sprite.y) * k_cursor_width + overlap.x - sprite.x);
                for (int x = overlap.x; x < end; ++x, ++src) {
                    dst[x] = *src ? *src
                             : form.contains({x, y})
                                 ? canvas[(y - form.y) * form.width + x - form.x]
                                 : callbacks.background_xrgb;
                }
                write_base(dst + end, y, end, r.x + r.width);
            }
            bytes += static_cast<std::uint64_t>(r.width) * r.height * sizeof(std::uint32_t);
        }
        b.contents = want;
        return bytes;
    }

    [[nodiscard]] lx::result<void> submit() {
        if (paused || !pipe || pipe->flip_pending())
            return {};
        if (paint_due) {
            if (auto painted = paint(); !painted)
                return painted.get_error();
        }
        if (cursor.valid())
            cursor.set_position(pointer.x, pointer.y);
        const bool modeset = pipe->needs_modeset();
        for (int attempt = 0; attempt < 2; ++attempt) {
            const pixels_version want{generation, pointer, !cursor.valid(), cached_revision};
            const pixels_version visible = front < 0 ? pixels_version{} : buffers[front].contents;
            drm::atomic_commit_request req{};
            req.damage = pixel_damage(visible, want, form, display);
            req.update_primary = modeset || req.damage.full_frame || req.damage.count > 0;
            req.cursor = cursor.valid() && cursor.involves_crtc() ? &cursor : nullptr;
            if (!req.update_primary && !(req.cursor && req.cursor->dirty()))
                return {};
            const int back = front == 0 ? 1 : 0;
            if (req.update_primary) {
                req.framebuffer_id = buffers[back].fb.id();
                frame_bytes = update(buffers[back], want);
                if (!modeset && !req.damage.full_frame)
                    pipe->build_damage_blob(req.damage);
            }
            auto committed = pipe->commit(req);
            if (committed) {
                pending = req.update_primary ? back : -1;
                pending_current_content = req.update_primary;
                flip_deadline = loop.now() + k_flip_timeout;
                // Seed the second buffer only after the first submission is in the kernel:
                // the first keystroke should not pay for an unrelated full-screen write.
                if (req.update_primary && front < 0 && !buffers[1 - back].contents.form)
                    (void)update(buffers[1 - back], want);
                if (pipe->completion_needs_dispatch())
                    dispatch_flips();
                if (flip_deadline > 0)
                    work.schedule_at(flip_deadline);
                return {};
            }
            if (pipe->last_failure_lost_master()) {
                paused = true;
                invalidate_visibility();
                return {};
            }
            if (!req.cursor || !pipe->last_failure_rejected_state())
                return committed.get_error();
            // Some drivers expose a cursor plane but reject it at the viewport edge.
            // Removing its FB detaches the plane; retry with the bounded software sprite.
            cursor = {};
            if (modeset)
                pipe->prepare_resume();
        }
        return lx::make_error(lx::drm_err::page_flip_failed, "greeter flip retry exhausted");
    }

    void tick() {
        if (paused)
            return;
        if (pipe && pipe->flip_pending() && !gone) {
            if (loop.now() < flip_deadline) {
                work.schedule_at(flip_deadline);
                return;
            }
            // Never reuse the queued buffer when a driver lost a completion. Closing this
            // DRM file (including logind's copy) retires its outstanding event storage.
            log_kms.warn("greeter page flip timed out");
            if (recovered_stall) {
                loop.quit(1);
                return;
            }
            recovered_stall = true;
            retire();
            probe_due = true;
        }
        if (probe_due || resume_due) {
            probe_due = false;
            if (auto configured = reconfigure(std::exchange(resume_due, false)); !configured) {
                log_kms.error(configured.get_error());
                loop.quit(1);
                return;
            }
        }
        if (auto result = submit(); !result) {
            log_kms.error(result.get_error());
            loop.quit(1);
        }
    }
};

kms_host::kms_host(rt::event_loop& loop, lx::session::logind_session& session,
                   kms_host_callbacks callbacks)
    : state_{std::make_unique<state>(loop, session, callbacks)} {}

kms_host::~kms_host() {
    close();
}

lx::result<void> kms_host::open() {
    rt::assert_affinity(rt::affinity::ui);
    auto& s = *state_;
    if (s.started || !s.callbacks.configure || !s.callbacks.paint || !s.session.has_control())
        return lx::invalid_argument("invalid greeter KMS host startup");
    if (s.loop.source_capacity() - s.loop.source_count() < 3)
        return lx::make_error(lx::drm_err::open_failed, "greeter KMS event sources do not fit");
    s.started = true;
    s.loop.add_source(&s.work);
    auto result = s.acquire_or_wait();
    if (result)
        result = s.submit();
    if (!result)
        close();
    return result;
}

void kms_host::suspend() {
    rt::assert_affinity(rt::affinity::ui);
    auto& s = *state_;
    s.paused = true;
    s.invalidate_visibility();
    s.work.set_enabled(false);
}

lx::result<void> kms_host::resume() {
    rt::assert_affinity(rt::affinity::ui);
    auto& s = *state_;
    if (!s.started || !s.paused)
        return {};
    if (!s.gone && s.card.card_fd() >= 0 && !s.card.is_master())
        return {}; // Active changed before the matching ResumeDevice.
    s.paused = false;
    if (!s.gone)
        s.dispatch_flips();
    if (!s.gone && s.pipe && s.pipe->flip_pending()) {
        // Do not disown a flip and guess which of the two buffers is writable. Keep
        // both until completion, or until the watchdog retires the entire DRM file.
        s.resume_due = true;
        s.work.schedule_at(std::max(s.loop.now(), s.flip_deadline));
        return {};
    }
    s.flip_deadline = 0;
    s.resume_due = s.probe_due = false;
    if (auto configured = s.reconfigure(true); !configured)
        return configured.get_error();
    return s.submit();
}

void kms_host::close(bool handoff) {
    auto& s = *state_;
    if (!s.started)
        return;
    rt::assert_affinity(rt::affinity::ui);
    s.started = false;
    s.work.set_enabled(false);
    s.loop.remove_source(&s.work);
    if (s.monitor.fd() >= 0)
        s.loop.remove_source(&s.hotplug);
    s.monitor = {};
    s.retire(handoff);
    std::vector<std::uint32_t>{}.swap(s.canvas);
    s.paused = s.gone = s.probe_due = false;
}

bool kms_host::owns_device(unsigned major_number, unsigned minor_number) const {
    const auto& s = *state_;
    return s.card.card_fd() >= 0 && s.devnum == makedev(major_number, minor_number);
}

void kms_host::device_gone(unsigned major_number, unsigned minor_number) {
    rt::assert_affinity(rt::affinity::ui);
    if (owns_device(major_number, minor_number)) {
        state_->gone = state_->probe_due = true;
        state_->schedule();
    }
}

void kms_host::request_frame(std::uint64_t form_revision) {
    rt::assert_affinity(rt::affinity::ui);
    auto& s = *state_;
    if (form_revision)
        s.requested_revision = form_revision;
    s.paint_due = true;
    s.schedule();
}

void kms_host::move_pointer(int x, int y) {
    rt::assert_affinity(rt::affinity::ui);
    auto& s = *state_;
    const auto old = s.pointer;
    s.pointer = {x, y};
    if (!s.display.size().empty()) {
        s.pointer_set = true;
        s.clamp_pointer();
    }
    if (s.cursor.valid())
        s.cursor.set_position(s.pointer.x, s.pointer.y);
    if (!same_point(old, s.pointer))
        s.schedule();
}

lx::rect2i kms_host::form_rect() const {
    return state_->form;
}
lx::rect2i kms_host::display_rect() const {
    return state_->display;
}
int kms_host::scale() const {
    return state_->scale;
}

bool kms_host::content_visible() const {
    return state_->visible_content && !state_->paused && !state_->gone;
}

} // namespace lumen::dm
