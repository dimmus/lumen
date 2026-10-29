module;

// The window animations' bodies (`:window_animation`; why they live here: session_lock.cpp).

module lx.compositor;

import lx.foundation;
import lx.runtime;
import lx.scene;
import :window_animation;

import std;

namespace lx::compositor {

namespace {

/// 1/255: an opacity step no 8-bit output shows.
constexpr double k_alpha_threshold = 1.0 / 255.0;

[[nodiscard]] double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

[[nodiscard]] double omega0(double stiffness) {
    return std::sqrt(stiffness > 1e-6 ? stiffness : 1e-6);
}

[[nodiscard]] bool empty(lx::rect2i r) { return r.width <= 0 || r.height <= 0; }

} // namespace

spring_state spring_step(spring_state s, double t, double stiffness, double damping_ratio) {
    if (t <= 0.0)
        return s;
    const double w0 = omega0(stiffness);
    const double zeta = damping_ratio > 0.0 ? damping_ratio : 0.0;
    const double beta = zeta * w0;
    const double x0 = s.offset;
    const double v0 = s.velocity;
    const double env = std::exp(-beta * t);
    spring_state out{};
    if (std::abs(zeta - 1.0) < 1e-6) {
        // x = e^(−ω₀t)·(x₀ + (v₀ + ω₀x₀)·t)
        const double c = v0 + w0 * x0;
        out.offset = env * (x0 + c * t);
        out.velocity = env * (v0 - c * w0 * t);
    } else if (zeta < 1.0) {
        const double wd = w0 * std::sqrt(1.0 - zeta * zeta);
        const double c2 = (v0 + beta * x0) / wd;
        const double cs = std::cos(wd * t);
        const double sn = std::sin(wd * t);
        out.offset = env * (x0 * cs + c2 * sn);
        out.velocity = env * (v0 * cs - (x0 * wd + beta * c2) * sn);
    } else {
        const double wd = w0 * std::sqrt(zeta * zeta - 1.0);
        const double c2 = (v0 + beta * x0) / wd;
        const double ch = std::cosh(wd * t);
        const double sh = std::sinh(wd * t);
        out.offset = env * (x0 * ch + c2 * sh);
        out.velocity = env * (v0 * ch + (x0 * wd - beta * c2) * sh);
    }
    return out;
}

double spring_reach(spring_state s, double stiffness) {
    return std::hypot(s.offset, s.velocity / omega0(stiffness));
}

// ── spring_value ─────────────────────────────────────────────────────────────

void spring_value::launch(double from, double to) {
    value = from;
    velocity = 0.0;
    target = to;
    started = false;
    moving = from != to;
}

void spring_value::retarget(double to) {
    target = to;
    // Restarting the closed form from the state last shown follows the same trajectory the
    // old start would have (the motion has no memory beyond its state), now toward `to`.
    // One that had settled was sampled last frames ago: it starts again with the next frame.
    if (started && moving) {
        start = {value - target, velocity};
        start_ns = sampled_ns;
    } else {
        started = false;
    }
    moving = value != target || velocity != 0.0;
}

void spring_value::shift(double d) {
    value += d;
    retarget(target);
}

bool spring_value::sample(lx::runtime::clock_time now, const animation_config& c,
                          double threshold) {
    if (!moving)
        return false;
    if (!started) {
        started = true;
        start = {value - target, velocity};
        start_ns = now;
    }
    const double t = static_cast<double>(now - start_ns) * 1e-9;
    const spring_state s = spring_step(start, t, c.stiffness, c.damping_ratio);
    sampled_ns = now;
    if (spring_reach(s, c.stiffness) < threshold) {
        finish();
        return false;
    }
    value = target + s.offset;
    velocity = s.velocity;
    return true;
}

void spring_value::finish() {
    value = target;
    velocity = 0.0;
    moving = false;
}

// ── window_animator ──────────────────────────────────────────────────────────

void window_animator::bind(scene::scene_graph& scene, rest_fn rest, done_fn done, void* user) {
    scene_ = &scene;
    rest_ = rest;
    done_ = done;
    user_ = user;
}

void window_animator::set_config(const animation_config& c) {
    config_ = c;
    if (config_.enabled)
        return;
    for (entry& e : entries_)
        if (e.used)
            end(e, true);
}

void window_animator::set_pixel_scale(double scale) { pixel_scale_ = scale >= 1.0 ? scale : 1.0; }

window_animator::entry* window_animator::find(lx::toplevel_id id, bool ghost) {
    for (entry& e : entries_)
        if (e.used && e.id == id && e.ghost == ghost)
            return &e;
    return nullptr;
}

const window_animator::entry* window_animator::find(lx::toplevel_id id, bool ghost) const {
    for (const entry& e : entries_)
        if (e.used && e.id == id && e.ghost == ghost)
            return &e;
    return nullptr;
}

window_animator::entry* window_animator::take(lx::toplevel_id id, scene::container_node* box,
                                              const window_rest& r) {
    if (!config_.enabled || !box || !id || empty(r.frame))
        return nullptr;
    if (entry* e = find(id, false)) {
        e->box = box;
        e->rest = r;
        return e;
    }
    for (entry& e : entries_) {
        if (e.used)
            continue;
        e = {};
        e.used = true;
        e.id = id;
        e.box = box;
        e.rest = r;
        ++live_;
        return &e;
    }
    return nullptr;
}

bool window_animator::open(lx::toplevel_id id, scene::container_node* box, const window_rest& r) {
    // The window is back before its ghost faded: the ghost goes (it holds the content the new
    // map replaces).
    end_ghost(id);
    entry* e = take(id, box, r);
    if (!e)
        return false;
    e->alpha.launch(-1.0, 0.0);
    e->scale.launch(k_motion_scale - 1.0, 0.0);
    e->hide_at_end = false;
    return true;
}

bool window_animator::geometry(lx::toplevel_id id, scene::container_node* box,
                               lx::rect2i old_frame, const window_rest& r) {
    const entry* before = find(id, false);
    // What the visual was measured from: the rest last sampled while it animates, else the
    // frame it had.
    const lx::rect2i from = before ? before->rest.frame : old_frame;
    if (empty(from))
        return false;
    entry* e = take(id, box, r);
    if (!e)
        return false;
    // The visual stays where it was shown; only what it is measured from moves under it.
    e->dx.shift(static_cast<double>(from.x) - r.frame.x);
    e->dy.shift(static_cast<double>(from.y) - r.frame.y);
    e->dw.shift(static_cast<double>(from.width) - r.frame.width);
    e->dh.shift(static_cast<double>(from.height) - r.frame.height);
    return true;
}

bool window_animator::hide(lx::toplevel_id id, scene::container_node* box, const window_rest& r,
                           window_motion how, int view, double distance) {
    entry* e = take(id, box, r);
    if (!e)
        return false;
    if (how == window_motion::slide_out) {
        e->slide.retarget(distance);
        e->view = view;
        e->elsewhere_hidden = true;
    } else {
        e->alpha.retarget(-1.0);
        e->scale.retarget(k_motion_scale - 1.0);
    }
    e->hide_at_end = true;
    return true;
}

bool window_animator::show(lx::toplevel_id id, scene::container_node* box, const window_rest& r,
                           window_motion how, int view, double distance) {
    const bool was = find(id, false) != nullptr;
    entry* e = take(id, box, r);
    if (!e)
        return false;
    e->hide_at_end = false;
    if (how == window_motion::slide_in) {
        // Coming back while it slid away: from where it is now. Else from the side.
        if (was && e->slide.moving)
            e->slide.retarget(0.0);
        else
            e->slide.launch(distance, 0.0);
        e->view = view;
        e->elsewhere_hidden = false;
    } else {
        if (was && (e->alpha.moving || e->scale.moving)) {
            e->alpha.retarget(0.0);
            e->scale.retarget(0.0);
        } else {
            e->alpha.launch(-1.0, 0.0);
            e->scale.launch(k_motion_scale - 1.0, 0.0);
        }
    }
    return true;
}

bool window_animator::close(lx::toplevel_id id, scene::container_node* ghost,
                            const window_rest& r) {
    if (!config_.enabled || !ghost || !id || empty(r.frame))
        return false;
    // An older ghost of the same window is done.
    end_ghost(id);
    entry* e = find(id, false);
    if (!e) {
        e = take(id, ghost, r);
        if (!e)
            return false;
    }
    // The ghost takes over the window's motion: the window's own box is about to go.
    e->box->set_animation({});
    e->box = ghost;
    e->rest = r;
    e->ghost = true;
    e->hide_at_end = false;
    e->alpha.retarget(-1.0);
    e->scale.retarget(k_motion_scale - 1.0);
    return true;
}

void window_animator::stop(lx::toplevel_id id) {
    entry* e = find(id, false);
    if (!e)
        return;
    e->hide_at_end = false;
    end(*e, true);
}

void window_animator::end_ghost(lx::toplevel_id id) {
    if (entry* g = find(id, true))
        end(*g, true);
}

bool window_animator::animating(lx::toplevel_id id) const { return find(id, false) != nullptr; }

bool window_animator::is_ghost(const scene::scene_node* node) const {
    if (live_ == 0 || !node)
        return false;
    for (const entry& e : entries_)
        if (e.used && e.ghost && e.box == node)
            return true;
    return false;
}

void window_animator::damage(const entry& e, lx::rect2i r) const {
    if (!scene_ || empty(r))
        return;
    // One pixel more on every side: a resampled draw's filter reaches past its own pixels.
    if (e.view >= 0)
        scene_->note_damage_views(r, 1, 1u << static_cast<unsigned>(e.view));
    else
        scene_->note_damage(r, 1);
}

void window_animator::damage_captures(lx::rect2i r) const {
    if (!scene_ || empty(r))
        return;
    constexpr std::uint32_t captures = ((std::uint32_t{1} << scene::k_capture_views) - 1u)
                                       << scene::k_capture_view_base;
    scene_->note_damage_views(r, 1, captures);
}

void window_animator::end(entry& e, bool damage_rest) {
    if (e.drawn)
        damage(e, e.shown);
    if (e.box)
        e.box->set_animation({});
    const bool ghost = e.ghost;
    const lx::toplevel_id id = e.id;
    scene::container_node* box = e.box;
    // The rest, on every view (a confined motion drew it at rest elsewhere, or not at all).
    if (damage_rest && scene_ && !empty(e.rest.bounds))
        scene_->note_damage(e.rest.bounds, 1);
    if (!ghost && e.hide_at_end && box)
        box->set_visible(false);
    e = {};
    --live_;
    if (ghost && done_)
        done_(user_, id, box);
}

void window_animator::tick(lx::runtime::clock_time now_ns) {
    if (live_ == 0)
        return;
    if (now_ns < now_ns_)
        now_ns = now_ns_;
    now_ns_ = now_ns;
    const double px = 0.5 / pixel_scale_;
    for (entry& e : entries_) {
        if (!e.used)
            continue;
        if (!e.ghost && rest_) {
            window_rest r{};
            if (!rest_(user_, e.id, r) || empty(r.frame)) {
                end(e, true);
                continue;
            }
            e.rest = r;
        }
        const lx::rect2i R = e.rest.frame;
        // Scale is about the centre: half the longer side is how far a corner moves.
        const double half = (R.width > R.height ? R.width : R.height) * 0.5;
        bool moving = false;
        moving |= e.dx.sample(now_ns, config_, px);
        moving |= e.dy.sample(now_ns, config_, px);
        moving |= e.dw.sample(now_ns, config_, px);
        moving |= e.dh.sample(now_ns, config_, px);
        moving |= e.slide.sample(now_ns, config_, px);
        moving |= e.scale.sample(now_ns, config_, px / (half > 1.0 ? half : 1.0));
        moving |= e.alpha.sample(now_ns, config_, k_alpha_threshold);
        if (!moving) {
            end(e, true);
            continue;
        }
        const double cw = R.width + e.dw.value;
        const double ch = R.height + e.dh.value;
        const double cx = R.x + e.dx.value + cw * 0.5;
        const double cy = R.y + e.dy.value + ch * 0.5;
        const double s = 1.0 + e.scale.value > 0.01 ? 1.0 + e.scale.value : 0.01;
        const double vw = (cw > 1.0 ? cw : 1.0) * s;
        const double vh = (ch > 1.0 ? ch : 1.0) * s;
        scene::node_animation a{};
        a.active = true;
        a.from_x = R.x;
        a.from_y = R.y;
        a.to_x = cx - vw * 0.5 + e.slide.value;
        a.to_y = cy - vh * 0.5;
        a.scale_x = vw / R.width;
        a.scale_y = vh / R.height;
        a.opacity = static_cast<float>(clamp01(1.0 + e.alpha.value));
        a.view = e.view;
        a.elsewhere_hidden = e.elsewhere_hidden;
        a.leaving = e.ghost || e.hide_at_end;
        // Capture views draw the end state: one that starts or stops leaving changes there.
        if (a.leaving != (e.box->animation().active && e.box->animation().leaving))
            damage_captures(e.rest.bounds);
        e.box->set_animation(a);
        // What it draws, where it is drawn now (outward to whole pixels).
        const lx::rect2i B = e.rest.bounds;
        const double x0 = std::floor(a.to_x + (B.x - R.x) * a.scale_x);
        const double y0 = std::floor(a.to_y + (B.y - R.y) * a.scale_y);
        const double x1 =
            std::ceil(a.to_x + (static_cast<double>(B.x) + B.width - R.x) * a.scale_x);
        const double y1 =
            std::ceil(a.to_y + (static_cast<double>(B.y) + B.height - R.y) * a.scale_y);
        const lx::rect2i v{static_cast<int>(x0), static_cast<int>(y0),
                           static_cast<int>(x1 - x0), static_cast<int>(y1 - y0)};
        if (e.drawn)
            damage(e, e.shown);
        damage(e, v);
        e.shown = v;
        e.drawn = true;
    }
}

} // namespace lx::compositor
