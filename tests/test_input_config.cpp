// Pointer device settings (config.toml [input.touchpad], [input.mouse], [input.trackpoint],
// [[input.device]]; design-session-and-wm C.5): the configuration → libinput setter calls,
// through a recording device_ops table (no device): a touchpad gets tapping, a mouse never
// does; "default" restores the device's default rather than leaving the last value; what a
// device lacks is skipped and reported; a per-device override reaches only that device.
#include "lumen_test.hpp"

import std;
import lx.input;
import lx.config;
import lx.compositor;

namespace {

/// One fake device: what it supports, and every setter call it received.
struct fake_device {
    int tap_fingers = 0;
    bool natural = true;
    unsigned scroll_methods = 0;
    unsigned default_scroll = 0;
    unsigned click_methods = 0;
    unsigned default_click = 0;
    bool accel = true;
    unsigned profiles = 3; // flat | adaptive
    unsigned default_profile = 2;
    bool left_handed = true;
    bool dwt = false;
    bool middle = true;
    int three_fingers = 0;
    std::vector<std::string> calls;
};

fake_device& dev(void* d) { return *static_cast<fake_device*>(d); }
void rec(void* d, const char* what, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s=%g", what, v);
    dev(d).calls.emplace_back(buf);
}

lx::input::device_ops fake_ops() {
    lx::input::device_ops o{};
    o.tap_finger_count = [](void* d) { return dev(d).tap_fingers; };
    o.set_tap = [](void* d, bool on) { rec(d, "tap", on); };
    o.set_tap_button_map = [](void* d, unsigned m) { rec(d, "tap-map", m); };
    o.set_drag = [](void* d, bool on) { rec(d, "drag", on); };
    o.set_drag_lock = [](void* d, bool on) { rec(d, "drag-lock", on); };
    o.three_finger_drag_fingers = [](void* d) { return dev(d).three_fingers; };
    o.set_three_finger_drag = [](void* d, bool on) { rec(d, "3fg", on); };
    o.natural_scroll_supported = [](void* d) { return dev(d).natural; };
    o.set_natural_scroll = [](void* d, bool on) { rec(d, "natural", on); };
    o.scroll_methods = [](void* d) { return dev(d).scroll_methods; };
    o.default_scroll_method = [](void* d) { return dev(d).default_scroll; };
    o.set_scroll_method = [](void* d, unsigned m) { rec(d, "scroll", m); };
    o.click_methods = [](void* d) { return dev(d).click_methods; };
    o.default_click_method = [](void* d) { return dev(d).default_click; };
    o.set_click_method = [](void* d, unsigned m) { rec(d, "click", m); };
    o.accel_supported = [](void* d) { return dev(d).accel; };
    o.accel_profiles = [](void* d) { return dev(d).profiles; };
    o.default_accel_profile = [](void* d) { return dev(d).default_profile; };
    o.set_accel_profile = [](void* d, unsigned p) { rec(d, "profile", p); };
    o.set_accel_speed = [](void* d, double s) { rec(d, "speed", s); };
    o.left_handed_supported = [](void* d) { return dev(d).left_handed; };
    o.set_left_handed = [](void* d, bool on) { rec(d, "left", on); };
    o.dwt_supported = [](void* d) { return dev(d).dwt; };
    o.set_dwt = [](void* d, bool on) { rec(d, "dwt", on); };
    o.middle_emulation_supported = [](void* d) { return dev(d).middle; };
    o.set_middle_emulation = [](void* d, bool on) { rec(d, "middle", on); };
    return o;
}

bool called(const fake_device& d, const char* call) {
    return std::ranges::find(d.calls, std::string(call)) != d.calls.end();
}
bool called_prefix(const fake_device& d, const char* prefix) {
    return std::ranges::any_of(d.calls, [&](const std::string& c) { return c.starts_with(prefix); });
}

lx::config::config_snapshot parse(const char* text) {
    lx::config::config_snapshot c = lx::config::default_config();
    lx::config::config_text t{};
    (void)t.add_layer("/test/config.toml", text);
    lx::config::diagnostics d{};
    LUMEN_CHECK(lx::config::parse_config(t, c, d, nullptr));
    return c;
}

} // namespace

LUMEN_TEST(touchpad_defaults_tap_natural_dwt) {
    fake_device tp{.tap_fingers = 3, .scroll_methods = 3, .default_scroll = 1,
                   .click_methods = 3, .default_click = 1, .dwt = true};
    const auto c = lx::compositor::pointer_config_of(lx::config::default_config(), "Touchpad",
                                                     lx::input::device_class::touchpad);
    lx::input::apply_report r{};
    lx::input::apply_pointer_config(&tp, fake_ops(), lx::input::device_class::touchpad, c, r);
    LUMEN_CHECK(called(tp, "tap=1") && called(tp, "natural=1") && called(tp, "dwt=1"));
    LUMEN_CHECK(called(tp, "drag=1") && called(tp, "drag-lock=0") && called(tp, "tap-map=0"));
    // "default" scroll/click methods and accel profile restore the device's defaults.
    LUMEN_CHECK(called(tp, "scroll=1") && called(tp, "click=1") && called(tp, "profile=2"));
    LUMEN_CHECK(called(tp, "speed=0"));
    LUMEN_CHECK(std::strstr(r.applied, "tap=on") && r.applied_count > 0);
}

LUMEN_TEST(a_mouse_is_never_asked_to_tap) {
    fake_device mouse{.tap_fingers = 0};
    const auto cfg = parse("[input.mouse]\nnatural-scroll = true\naccel-profile = \"flat\"\n"
                           "accel-speed = -0.5\nleft-handed = true\n[input.touchpad]\ntap = true\n");
    const auto c = lx::compositor::pointer_config_of(cfg, "USB Mouse", lx::input::device_class::mouse);
    lx::input::apply_report r{};
    lx::input::apply_pointer_config(&mouse, fake_ops(), lx::input::device_class::mouse, c, r);
    LUMEN_CHECK(!called_prefix(mouse, "tap") && !called_prefix(mouse, "dwt") &&
                !called_prefix(mouse, "scroll="));
    LUMEN_CHECK(called(mouse, "natural=1") && called(mouse, "profile=1") &&
                called(mouse, "speed=-0.5") && called(mouse, "left=1"));
}

LUMEN_TEST(unsupported_settings_are_skipped_and_reported) {
    // A VM's absolute pointer: no acceleration, no natural scrolling, no left-handed.
    fake_device vm{.natural = false, .accel = false, .left_handed = false, .middle = false};
    const auto cfg = parse("[input.mouse]\nnatural-scroll = true\nleft-handed = true\n");
    const auto c = lx::compositor::pointer_config_of(cfg, "VMware Absolute",
                                                     lx::input::device_class::mouse);
    lx::input::apply_report r{};
    lx::input::apply_pointer_config(&vm, fake_ops(), lx::input::device_class::mouse, c, r);
    LUMEN_CHECK(vm.calls.empty());
    LUMEN_CHECK(std::strstr(r.unsupported, "natural-scroll") && std::strstr(r.unsupported, "accel") &&
                std::strstr(r.unsupported, "left-handed") && r.applied_count == 0);
    // A flat profile the device does not offer.
    fake_device adaptive_only{.profiles = 2};
    const auto flat = lx::compositor::pointer_config_of(parse("[input.mouse]\naccel-profile = \"flat\"\n"),
                                                        "M", lx::input::device_class::mouse);
    lx::input::apply_pointer_config(&adaptive_only, fake_ops(), lx::input::device_class::mouse,
                                    flat, r);
    LUMEN_CHECK(!called_prefix(adaptive_only, "profile") && std::strstr(r.unsupported, "accel-profile"));
}

LUMEN_TEST(per_device_override_reaches_only_that_device) {
    const auto cfg = parse("[input.mouse]\naccel-speed = 0.2\n"
                           "[[input.device]]\nname = \"Logitech USB Receiver\"\naccel-speed = -0.3\n"
                           "natural-scroll = true\n");
    const auto logi = lx::compositor::pointer_config_of(cfg, "Logitech USB Receiver",
                                                        lx::input::device_class::mouse);
    const auto other = lx::compositor::pointer_config_of(cfg, "Other Mouse",
                                                         lx::input::device_class::mouse);
    LUMEN_CHECK(logi.accel_speed == -0.3 && logi.natural_scroll);
    LUMEN_CHECK(other.accel_speed == 0.2 && !other.natural_scroll);
}

LUMEN_TEST(trackpoint_and_scroll_factor) {
    const auto cfg = parse("[input.trackpoint]\naccel-speed = 0.5\nscroll-factor = 2.0\n");
    const auto c = lx::compositor::pointer_config_of(cfg, "TPPS/2 IBM TrackPoint",
                                                     lx::input::device_class::trackpoint);
    LUMEN_CHECK(c.accel_speed == 0.5 && c.scroll_factor == 2.0);
    fake_device tp{};
    lx::input::apply_report r{};
    lx::input::apply_pointer_config(&tp, fake_ops(), lx::input::device_class::trackpoint, c, r);
    // A trackpoint's section has no natural-scroll or handedness: left as the device has them.
    LUMEN_CHECK(!called_prefix(tp, "natural") && !called_prefix(tp, "left") &&
                called(tp, "speed=0.5") && std::strstr(r.applied, "scroll-factor=2.00"));
}

LUMEN_TEST(other_devices_get_nothing) {
    fake_device kb{};
    lx::input::apply_report r{};
    lx::input::apply_pointer_config(&kb, fake_ops(), lx::input::device_class::other,
                                    lx::input::pointer_config{}, r);
    LUMEN_CHECK(kb.calls.empty() && r.applied_count == 0);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
