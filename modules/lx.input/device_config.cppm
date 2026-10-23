module;

// Pointer device settings (design-session-and-wm C.2, C.5): what the configuration asks of a
// touchpad, mouse or trackpoint, and the one routine that applies it through libinput's
// `libinput_device_config_*` calls. The calls go through a table of function pointers on an
// opaque device so the decisions (what is supported, what "default" restores, what is
// skipped) are tested without a device (tests/test_input_config.cpp); lx.input fills the
// table with libinput's own functions. Free of libinput's headers.

export module lx.input:device_config;

import std;

export namespace lx::input {

enum class device_class : unsigned char {
    /// No pointer: a keyboard, a switch, a tablet pad.
    other,
    /// A pointer that can tap (libinput reports tap fingers): a touchpad.
    touchpad,
    /// A pointing stick (udev ID_INPUT_POINTINGSTICK).
    trackpoint,
    /// Any other pointer: a mouse, a trackball, a VM's absolute pointer.
    mouse,
};

[[nodiscard]] constexpr const char* device_class_name(device_class c) {
    switch (c) {
    case device_class::touchpad: return "touchpad";
    case device_class::trackpoint: return "trackpoint";
    case device_class::mouse: return "mouse";
    case device_class::other: break;
    }
    return "other";
}

/// What to set. Enumerations: 0 is "the device's default" (restored, not left as it was, so a
/// setting removed from the file goes back).
struct pointer_config {
    bool tap = false;
    unsigned tap_button_map = 0; ///< 0 lrm, 1 lmr
    bool drag = true;
    bool drag_lock = false;
    bool three_finger_drag = false;
    bool natural_scroll = false;
    unsigned scroll_method = 0; ///< 0 default, 1 two-finger, 2 edge, 3 on-button-down, 4 none
    unsigned click_method = 0;  ///< 0 default, 1 button-areas, 2 clickfinger
    unsigned accel_profile = 0; ///< 0 default, 1 adaptive, 2 flat
    double accel_speed = 0.0;
    bool left_handed = false;
    bool disable_while_typing = true;
    bool middle_emulation = false;
    /// Applied by the compositor to scroll values, not by libinput.
    double scroll_factor = 1.0;
};

/// libinput's device configuration, as calls on an opaque device. Every `*_supported`/count
/// query decides whether its setter is called at all.
struct device_ops {
    int (*tap_finger_count)(void* dev) = nullptr;
    void (*set_tap)(void* dev, bool on) = nullptr;
    void (*set_tap_button_map)(void* dev, unsigned map) = nullptr;
    void (*set_drag)(void* dev, bool on) = nullptr;
    void (*set_drag_lock)(void* dev, bool on) = nullptr;
    /// 0 when the device (or libinput) has no three-finger drag.
    int (*three_finger_drag_fingers)(void* dev) = nullptr;
    void (*set_three_finger_drag)(void* dev, bool on) = nullptr;
    bool (*natural_scroll_supported)(void* dev) = nullptr;
    void (*set_natural_scroll)(void* dev, bool on) = nullptr;
    /// Bit mask: 1 two-finger, 2 edge, 4 on-button-down (libinput's values).
    unsigned (*scroll_methods)(void* dev) = nullptr;
    unsigned (*default_scroll_method)(void* dev) = nullptr;
    /// `method` as libinput's value (0 none).
    void (*set_scroll_method)(void* dev, unsigned method) = nullptr;
    /// Bit mask: 1 button-areas, 2 clickfinger.
    unsigned (*click_methods)(void* dev) = nullptr;
    unsigned (*default_click_method)(void* dev) = nullptr;
    void (*set_click_method)(void* dev, unsigned method) = nullptr;
    bool (*accel_supported)(void* dev) = nullptr;
    /// Bit mask: 1 flat, 2 adaptive (libinput's values).
    unsigned (*accel_profiles)(void* dev) = nullptr;
    unsigned (*default_accel_profile)(void* dev) = nullptr;
    void (*set_accel_profile)(void* dev, unsigned profile) = nullptr;
    void (*set_accel_speed)(void* dev, double speed) = nullptr;
    bool (*left_handed_supported)(void* dev) = nullptr;
    void (*set_left_handed)(void* dev, bool on) = nullptr;
    bool (*dwt_supported)(void* dev) = nullptr;
    void (*set_dwt)(void* dev, bool on) = nullptr;
    bool (*middle_emulation_supported)(void* dev) = nullptr;
    void (*set_middle_emulation)(void* dev, bool on) = nullptr;
};

/// What `apply_pointer_config` did, as two short texts for the log: the settings applied
/// ("tap=on accel-profile=flat …") and those the device does not have.
struct apply_report {
    char applied[256]{};
    char unsupported[192]{};
    unsigned applied_count = 0;
    unsigned unsupported_count = 0;
};

/// Applies `c` to `dev` for a device of class `kind`: the settings that class takes (a
/// mouse is never asked to tap), each only where the device supports it.
void apply_pointer_config(void* dev, const device_ops& ops, device_class kind,
                          const pointer_config& c, apply_report& report);

} // namespace lx::input

namespace {

void note(char* buf, std::size_t cap, unsigned& count, const char* text) {
    const std::size_t n = std::strlen(buf);
    if (n + std::strlen(text) + 2 < cap)
        std::snprintf(buf + n, cap - n, "%s%s", n ? " " : "", text);
    ++count;
}

} // namespace

void lx::input::apply_pointer_config(void* dev, const device_ops& ops, device_class kind,
                                     const pointer_config& c, apply_report& r) {
    r = apply_report{};
    if (!dev || kind == device_class::other)
        return;
    char t[64];
    const auto applied = [&](const char* text) { note(r.applied, sizeof(r.applied), r.applied_count, text); };
    const auto missing = [&](const char* name) {
        note(r.unsupported, sizeof(r.unsupported), r.unsupported_count, name);
    };
    const bool touchpad = kind == device_class::touchpad;

    // Tapping, and what it brings: touchpads only.
    if (touchpad) {
        if (ops.tap_finger_count && ops.tap_finger_count(dev) > 0) {
            if (ops.set_tap)
                ops.set_tap(dev, c.tap);
            if (ops.set_tap_button_map)
                ops.set_tap_button_map(dev, c.tap_button_map);
            if (ops.set_drag)
                ops.set_drag(dev, c.drag);
            if (ops.set_drag_lock)
                ops.set_drag_lock(dev, c.drag_lock);
            std::snprintf(t, sizeof(t), "tap=%s tap-button-map=%s drag=%s drag-lock=%s",
                          c.tap ? "on" : "off", c.tap_button_map ? "lmr" : "lrm",
                          c.drag ? "on" : "off", c.drag_lock ? "on" : "off");
            applied(t);
        } else {
            missing("tap");
        }
        if (ops.three_finger_drag_fingers && ops.three_finger_drag_fingers(dev) >= 3) {
            if (ops.set_three_finger_drag)
                ops.set_three_finger_drag(dev, c.three_finger_drag);
            applied(c.three_finger_drag ? "three-finger-drag=on" : "three-finger-drag=off");
        } else if (c.three_finger_drag) {
            missing("three-finger-drag");
        }
        if (ops.dwt_supported && ops.dwt_supported(dev)) {
            if (ops.set_dwt)
                ops.set_dwt(dev, c.disable_while_typing);
            applied(c.disable_while_typing ? "disable-while-typing=on" : "disable-while-typing=off");
        } else {
            missing("disable-while-typing");
        }
        const unsigned methods = ops.scroll_methods ? ops.scroll_methods(dev) : 0u;
        // Our 1 two-finger, 2 edge, 3 on-button-down, 4 none → libinput's 1, 2, 4, 0.
        constexpr unsigned k_scroll_li[] = {0, 1, 2, 4, 0};
        if (c.scroll_method == 0) {
            if (ops.default_scroll_method && ops.set_scroll_method && methods)
                ops.set_scroll_method(dev, ops.default_scroll_method(dev));
        } else if (c.scroll_method < 5) {
            const unsigned want = k_scroll_li[c.scroll_method];
            if (want == 0 || (methods & want)) {
                if (ops.set_scroll_method)
                    ops.set_scroll_method(dev, want);
                constexpr const char* k_names[] = {"default", "two-finger", "edge",
                                                   "on-button-down", "none"};
                std::snprintf(t, sizeof(t), "scroll-method=%s", k_names[c.scroll_method]);
                applied(t);
            } else {
                missing("scroll-method");
            }
        }
        const unsigned clicks = ops.click_methods ? ops.click_methods(dev) : 0u;
        if (c.click_method == 0) {
            if (ops.default_click_method && ops.set_click_method && clicks)
                ops.set_click_method(dev, ops.default_click_method(dev));
        } else if (c.click_method <= 2) {
            const unsigned want = c.click_method == 1 ? 1u : 2u;
            if (clicks & want) {
                if (ops.set_click_method)
                    ops.set_click_method(dev, want);
                applied(c.click_method == 1 ? "click-method=button-areas" : "click-method=clickfinger");
            } else {
                missing("click-method");
            }
        }
    }

    // Every pointer: scrolling direction, acceleration, handedness, middle emulation (each a
    // device may lack — a VM's absolute pointer has no acceleration).
    if (kind != device_class::trackpoint || c.natural_scroll) {
        if (ops.natural_scroll_supported && ops.natural_scroll_supported(dev)) {
            if (ops.set_natural_scroll)
                ops.set_natural_scroll(dev, c.natural_scroll);
            applied(c.natural_scroll ? "natural-scroll=on" : "natural-scroll=off");
        } else {
            missing("natural-scroll");
        }
    }
    if (ops.accel_supported && ops.accel_supported(dev)) {
        const unsigned profiles = ops.accel_profiles ? ops.accel_profiles(dev) : 0u;
        // Our 1 adaptive, 2 flat → libinput's 2, 1.
        unsigned want = c.accel_profile == 1 ? 2u : c.accel_profile == 2 ? 1u : 0u;
        if (want == 0 && ops.default_accel_profile)
            want = ops.default_accel_profile(dev);
        if (want && (profiles & want) && ops.set_accel_profile)
            ops.set_accel_profile(dev, want);
        else if (c.accel_profile != 0)
            missing("accel-profile");
        if (ops.set_accel_speed)
            ops.set_accel_speed(dev, c.accel_speed);
        std::snprintf(t, sizeof(t), "accel-profile=%s accel-speed=%.2f",
                      c.accel_profile == 1   ? "adaptive"
                      : c.accel_profile == 2 ? "flat"
                                             : "default",
                      c.accel_speed);
        applied(t);
    } else {
        missing("accel");
    }
    if (kind != device_class::trackpoint) {
        if (ops.left_handed_supported && ops.left_handed_supported(dev)) {
            if (ops.set_left_handed)
                ops.set_left_handed(dev, c.left_handed);
            applied(c.left_handed ? "left-handed=on" : "left-handed=off");
        } else if (c.left_handed) {
            missing("left-handed");
        }
        if (ops.middle_emulation_supported && ops.middle_emulation_supported(dev)) {
            if (ops.set_middle_emulation)
                ops.set_middle_emulation(dev, c.middle_emulation);
            applied(c.middle_emulation ? "middle-emulation=on" : "middle-emulation=off");
        } else if (c.middle_emulation) {
            missing("middle-emulation");
        }
    }
    if (c.scroll_factor != 1.0) {
        std::snprintf(t, sizeof(t), "scroll-factor=%.2f", c.scroll_factor);
        applied(t);
    }
}
