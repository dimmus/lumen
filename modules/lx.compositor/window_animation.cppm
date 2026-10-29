module;

// Window animations (config.toml [animations], off by default): a window maps fading in from
// slightly smaller, closes fading out to slightly smaller (a ghost of its last frame: the
// client's objects are gone), minimizes and comes back the same way, slides with a workspace
// switch, and moves and resizes visually from where it was when the window manager places it
// (maximize, restore, snap, tiling). Every value rides a damped spring (unit mass, stiffness
// and damping ratio from the configuration), evaluated in closed form from the instant it was
// released — a frame's pose depends on its time alone, never on how many frames ran before.
//
// Visual only: the window manager's geometry, hit-testing, configures and the client never see
// an animation (the scene maps the window's draws, `scene::node_animation`). UI affinity
// throughout; a fixed table, no allocation; nothing at all happens while disabled or idle. The
// member bodies are in window_animation.cpp (why: session_lock.cpp).

export module lx.compositor:window_animation;

import lx.foundation;
import lx.runtime;
import lx.scene;

import std;

export namespace lx::compositor {

/// config.toml [animations].
struct animation_config {
    bool enabled = false;
    /// The springs' stiffness k (unit mass): ω₀ = √k rad/s.
    double stiffness = 800.0;
    /// ζ: 1 is critically damped (the fastest approach without overshoot), below 1 bounces,
    /// above 1 creeps.
    double damping_ratio = 1.0;
};

/// A spring's displacement from its target and its velocity (units, units/s).
struct spring_state {
    double offset = 0.0;
    double velocity = 0.0;
};

/// The state `seconds` after `s` of an unforced damped oscillator x'' + 2ζω₀x' + ω₀²x = 0
/// (ω₀ = √stiffness), in closed form: critically damped (|ζ − 1| < 1e-6), under- or
/// overdamped. `seconds` ≤ 0 returns `s`.
[[nodiscard]] spring_state spring_step(spring_state s, double seconds, double stiffness,
                                       double damping_ratio);
/// An upper bound on how far from its target a spring in state `s` can still get:
/// √(x² + v²/ω₀²), its energy as a displacement — the energy of a damped spring never grows,
/// so nothing it does later goes further.
[[nodiscard]] double spring_reach(spring_state s, double stiffness);

/// One animated value: released toward `target`, sampled at frame times. It starts at the
/// first sample after `launch` (a motion begun between frames starts with the frame that
/// first shows it) and stops, landing exactly on its target, at the first sample whose
/// `spring_reach` is below the threshold given — half a pixel, 1/255 of opacity.
struct spring_value {
    double value = 0.0;
    double velocity = 0.0;
    double target = 0.0;
    bool moving = false;
    bool started = false;
    /// Where the closed form starts from: value − target and velocity at `start_ns`.
    spring_state start{};
    lx::runtime::clock_time start_ns = 0;
    /// When `value` and `velocity` were last sampled.
    lx::runtime::clock_time sampled_ns = 0;

    /// From `from` at rest toward `to`.
    void launch(double from, double to);
    /// Toward `to` from where it is, at the velocity it has (continuous).
    void retarget(double to);
    /// The value jumps by `d`, the velocity kept (what it is measured from moved under it).
    void shift(double d);
    /// The value at `now`; true while it still moves.
    bool sample(lx::runtime::clock_time now, const animation_config& c, double threshold);
    /// On its target, at rest.
    void finish();
};

/// What a window's motion is measured against, both logical: the frame rect at rest (the
/// visual is that rect moved, resized, scaled about its centre and faded) and the bounds of
/// everything the window draws at rest (CSD shadows, a server-side frame) — what is damaged.
struct window_rest {
    lx::rect2i frame{};
    lx::rect2i bounds{};
};

/// What a window does.
enum class window_motion : unsigned char {
    open,      ///< fade in from `k_motion_scale`
    close,     ///< fade out to `k_motion_scale` (a ghost)
    minimize,  ///< fade out to `k_motion_scale`, then hidden
    restore,   ///< fade in from `k_motion_scale` (unminimized)
    slide_out, ///< a workspace switch away: off its output sideways, then hidden
    slide_in,  ///< a workspace switch to it: in from the side
};

/// How much smaller a window is when it opens and closes.
inline constexpr double k_motion_scale = 0.9;

/// The window animations. UI affinity.
class window_animator {
public:
    /// Windows animating at once; a motion beyond it is not animated.
    static constexpr unsigned k_max = 64;
    /// A window's rest now (its geometry may change while it animates); false when it has
    /// none (the motion ends).
    using rest_fn = bool (*)(void* user, lx::toplevel_id id, window_rest& out);
    /// A close motion ended: its ghost container (the caller's) can go.
    using done_fn = void (*)(void* user, lx::toplevel_id id, scene::container_node* ghost);

    void bind(scene::scene_graph& scene, rest_fn rest, done_fn done, void* user);
    /// Disabled: every motion ends where it was going, at once.
    void set_config(const animation_config& c);
    [[nodiscard]] const animation_config& config() const { return config_; }
    [[nodiscard]] bool enabled() const { return config_.enabled; }
    /// Something moves: the frame clock has to keep running.
    [[nodiscard]] bool active() const { return live_ != 0; }
    [[nodiscard]] unsigned count() const { return live_; }
    /// Output pixels per logical pixel on the densest output: the settle threshold is half
    /// of one of those.
    void set_pixel_scale(double scale);

    /// Window `id`'s tree container `box` (an anchor) mapped: it fades in from
    /// `k_motion_scale`. A ghost of `id` still closing goes at once.
    bool open(lx::toplevel_id id, scene::container_node* box, const window_rest& r);
    /// The window manager placed `id` anew (maximize, restore, snap, tiling) from frame
    /// `old_frame`: it moves and resizes there from where it is shown now.
    bool geometry(lx::toplevel_id id, scene::container_node* box, lx::rect2i old_frame,
                  const window_rest& r);
    /// `minimize` or `slide_out` (`distance`: logical px it travels, its sign the direction,
    /// confined to output view `view`). `box` stays visible until the motion ends; then it
    /// is hidden. False: not animated (the caller hides it).
    bool hide(lx::toplevel_id id, scene::container_node* box, const window_rest& r,
              window_motion how, int view, double distance);
    /// `restore` or `slide_in` (from `distance` px aside, on `view`): `box` is visible.
    bool show(lx::toplevel_id id, scene::container_node* box, const window_rest& r,
              window_motion how, int view, double distance);
    /// Window `id` unmapped: `ghost` (an anchor container in the window layer holding copies
    /// of what it drew, at rest at `r`) fades out, taking over whatever `id` was doing, and is
    /// handed back through `done` when it is gone. False: not animated (the caller frees it).
    bool close(lx::toplevel_id id, scene::container_node* ghost, const window_rest& r);
    /// `id`'s motion ends now, its box drawn at rest (a grab starts, it goes fullscreen, it
    /// unmaps without a ghost, a visibility change that is not animated). No end action:
    /// the caller decides the box's visibility.
    void stop(lx::toplevel_id id);
    /// A closing ghost of `id` goes now, its content let go (before `id` makes a new one).
    void end_ghost(lx::toplevel_id id);
    [[nodiscard]] bool animating(lx::toplevel_id id) const;
    /// `node` is a closing window's ghost (in the window layer, but not a window).
    [[nodiscard]] bool is_ghost(const scene::scene_node* node) const;

    /// Once per tick, before the scene is published: every motion sampled at `now_ns` (the
    /// frame's predicted presentation; never earlier than the last), each box's
    /// `node_animation` set, old and new visual bounds damaged; a settled motion lands on
    /// its rest (or hides its box, or hands its ghost back). O(motions), no allocation.
    void tick(lx::runtime::clock_time now_ns);
    /// The time of the last tick.
    [[nodiscard]] lx::runtime::clock_time now() const { return now_ns_; }

private:
    struct entry {
        lx::toplevel_id id{};
        scene::container_node* box = nullptr;
        bool used = false;
        bool ghost = false;
        bool hide_at_end = false;
        bool elsewhere_hidden = false;
        int view = -1;
        window_rest rest{};
        /// Visual frame rect − rest frame rect (logical px).
        spring_value dx{};
        spring_value dy{};
        spring_value dw{};
        spring_value dh{};
        /// Visual scale − 1, about the visual rect's centre.
        spring_value scale{};
        /// Visual opacity − 1.
        spring_value alpha{};
        /// Horizontal offset (logical px): a workspace slide.
        spring_value slide{};
        /// The visual bounds damaged last (logical); `drawn` once there is one.
        lx::rect2i shown{};
        bool drawn = false;
    };

    [[nodiscard]] entry* find(lx::toplevel_id id, bool ghost);
    [[nodiscard]] const entry* find(lx::toplevel_id id, bool ghost) const;
    [[nodiscard]] entry* take(lx::toplevel_id id, scene::container_node* box,
                              const window_rest& r);
    void damage(const entry& e, lx::rect2i logical) const;
    /// `logical` on the capture views (what they draw of a leaving window changed).
    void damage_captures(lx::rect2i logical) const;
    /// The motion is over: the box at rest (hidden at `hide_at_end`), a ghost handed back.
    void end(entry& e, bool damage_rest);

    scene::scene_graph* scene_ = nullptr;
    rest_fn rest_ = nullptr;
    done_fn done_ = nullptr;
    void* user_ = nullptr;
    animation_config config_{};
    double pixel_scale_ = 1.0;
    lx::runtime::clock_time now_ns_ = 0;
    entry entries_[k_max]{};
    unsigned live_ = 0;
};

} // namespace lx::compositor
