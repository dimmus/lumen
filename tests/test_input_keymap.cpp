// Keyboard state is testable without any hardware, which is why it is a separate partition
// from the libinput device layer. These run on any machine with xkbcommon data installed.
#include "lumen_test.hpp"

#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

import lx.foundation;
import lx.input;

namespace {

// evdev keycodes, as libinput reports them. xkb's are these plus eight, which the keymap
// applies internally — a test that hardcoded the xkb values would hide that conversion.
constexpr unsigned k_key_a = 30;
constexpr unsigned k_key_b = 48;
constexpr unsigned k_key_1 = 2;
constexpr unsigned k_key_leftshift = 42;
constexpr unsigned k_key_leftctrl = 29;
constexpr unsigned k_key_capslock = 58;
constexpr unsigned k_key_leftmeta = 125;

[[nodiscard]] bool compile_us(lx::input::keyboard_keymap& km) {
    lx::input::keymap_config cfg{};
    cfg.layout = "us";
    auto r = km.compile(cfg);
    if (!r) {
        std::printf("SKIP: no xkb keymap data available: %s\n", r.get_error().message);
        return false;
    }
    return true;
}

} // namespace

LUMEN_TEST(keymap_compiles_and_reports_valid) {
    lx::input::keyboard_keymap km{};
    LUMEN_CHECK(!km.valid()); // nothing before compile
    if (!compile_us(km))
        return;
    LUMEN_CHECK(km.valid());
}

// The whole point of xkb: a keycode means different things depending on state. Previously
// the compositor sent a hardcoded US string and tracked nothing, so this was unanswerable.
LUMEN_TEST(keymap_resolves_keysyms_through_modifier_state) {
    lx::input::keyboard_keymap km{};
    if (!compile_us(km))
        return;

    char buf[8]{};
    LUMEN_CHECK(km.utf8(k_key_a, buf, sizeof(buf)) == 1);
    LUMEN_CHECK(buf[0] == 'a');

    // Hold shift: the same keycode now produces uppercase.
    LUMEN_CHECK(km.update_key(k_key_leftshift, true)); // modifiers changed
    LUMEN_CHECK(km.named().shift);
    std::memset(buf, 0, sizeof(buf));
    LUMEN_CHECK(km.utf8(k_key_a, buf, sizeof(buf)) == 1);
    LUMEN_CHECK(buf[0] == 'A');

    // Release: back to lowercase, and the modifier state changed again.
    LUMEN_CHECK(km.update_key(k_key_leftshift, false));
    LUMEN_CHECK(!km.named().shift);
    std::memset(buf, 0, sizeof(buf));
    LUMEN_CHECK(km.utf8(k_key_a, buf, sizeof(buf)) == 1);
    LUMEN_CHECK(buf[0] == 'a');
}

// A plain letter must not be reported as a modifier change — clients redraw on those, so
// firing one per keystroke is both wasteful and visible.
LUMEN_TEST(keymap_reports_modifier_changes_only_when_they_change) {
    lx::input::keyboard_keymap km{};
    if (!compile_us(km))
        return;

    LUMEN_CHECK(!km.update_key(k_key_a, true));
    LUMEN_CHECK(!km.update_key(k_key_a, false));
    LUMEN_CHECK(!km.update_key(k_key_b, true));

    LUMEN_CHECK(km.update_key(k_key_leftctrl, true));  // ctrl down — changed
    LUMEN_CHECK(!km.update_key(k_key_a, true));        // letter while held — unchanged
    LUMEN_CHECK(km.update_key(k_key_leftctrl, false)); // ctrl up — changed
}

// Named lookups are what compositor keybindings use: "is Super held" without knowing which
// bit the compiled keymap assigned to it.
LUMEN_TEST(keymap_resolves_named_modifiers) {
    lx::input::keyboard_keymap km{};
    if (!compile_us(km))
        return;

    LUMEN_CHECK(!km.named().ctrl);
    LUMEN_CHECK(!km.named().super);

    (void)km.update_key(k_key_leftctrl, true);
    (void)km.update_key(k_key_leftmeta, true);
    const auto held = km.named();
    LUMEN_CHECK(held.ctrl);
    LUMEN_CHECK(held.super);
    LUMEN_CHECK(!held.alt);
    LUMEN_CHECK(!held.shift);
}

// Caps lock is *locked*, not held: it stays active after the key is released. Treating it
// like shift is a classic bug — it makes caps lock work only while the key is down.
LUMEN_TEST(keymap_tracks_locked_modifiers_separately_from_held) {
    lx::input::keyboard_keymap km{};
    if (!compile_us(km))
        return;

    (void)km.update_key(k_key_capslock, true);
    (void)km.update_key(k_key_capslock, false);

    LUMEN_CHECK(km.named().caps_lock);
    LUMEN_CHECK(km.modifiers().locked != 0); // locked, not depressed

    char buf[8]{};
    LUMEN_CHECK(km.utf8(k_key_a, buf, sizeof(buf)) == 1);
    LUMEN_CHECK(buf[0] == 'A'); // still capitalised with the key released
}

// The serialized masks are what wl_keyboard.modifiers carries. Depressed and locked must
// not be conflated: a client that sees caps as depressed will release it on the next key.
LUMEN_TEST(keymap_serializes_masks_for_the_protocol) {
    lx::input::keyboard_keymap km{};
    if (!compile_us(km))
        return;

    const auto idle = km.modifiers();
    LUMEN_CHECK(idle.depressed == 0 && idle.latched == 0 && idle.locked == 0);

    (void)km.update_key(k_key_leftshift, true);
    const auto shifted = km.modifiers();
    LUMEN_CHECK(shifted.depressed != 0);
    LUMEN_CHECK(shifted.locked == 0);

    // A mask pushed in from elsewhere must take effect too — this is the path used when
    // another component owns the state.
    lx::input::keyboard_keymap other{};
    if (!compile_us(other))
        return;
    LUMEN_CHECK(other.update_mask(shifted));
    LUMEN_CHECK(other.named().shift);
}

// Layouts other than US must actually work. The old hardcoded keymap made this impossible,
// which is the single most user-visible consequence of not having xkb server-side.
LUMEN_TEST(keymap_honors_a_non_us_layout) {
    lx::input::keyboard_keymap us{};
    if (!compile_us(us))
        return;

    lx::input::keyboard_keymap de{};
    lx::input::keymap_config cfg{};
    cfg.layout = "de";
    if (!de.compile(cfg)) {
        std::printf("SKIP: no 'de' layout data\n");
        return;
    }

    // On a German layout the key at QWERTY's 'y' position produces 'z'.
    constexpr unsigned k_key_y = 21;
    char us_buf[8]{};
    char de_buf[8]{};
    LUMEN_CHECK(us.utf8(k_key_y, us_buf, sizeof(us_buf)) == 1);
    LUMEN_CHECK(de.utf8(k_key_y, de_buf, sizeof(de_buf)) == 1);
    LUMEN_CHECK(us_buf[0] == 'y');
    LUMEN_CHECK(de_buf[0] == 'z');
}

LUMEN_TEST(keymap_rejects_a_bogus_layout) {
    lx::input::keyboard_keymap km{};
    lx::input::keymap_config cfg{};
    cfg.layout = "definitely-not-a-layout";
    // Either it fails to compile, or xkb falls back — but it must not claim a valid state
    // it does not have.
    auto r = km.compile(cfg);
    if (!r)
        LUMEN_CHECK(!km.valid());
}

// Clients repeat keys themselves from repeat_info, but the compositor needs this for its
// own bindings — and modifiers must never repeat.
LUMEN_TEST(keymap_knows_which_keys_repeat) {
    lx::input::keyboard_keymap km{};
    if (!compile_us(km))
        return;
    LUMEN_CHECK(km.repeats(k_key_a));
    LUMEN_CHECK(km.repeats(k_key_1));
    LUMEN_CHECK(!km.repeats(k_key_leftshift));
    LUMEN_CHECK(!km.repeats(k_key_leftctrl));
}

// The keymap reaches clients as a sealed fd. Sealing matters: clients map it shared, and
// without the seals a buggy or hostile one could resize it under everyone else.
LUMEN_TEST(keymap_exports_a_sealed_fd_clients_can_map) {
    lx::input::keyboard_keymap km{};
    if (!compile_us(km))
        return;

    unsigned size = 0;
    auto fd = km.keymap_fd(size);
    LUMEN_CHECK(static_cast<bool>(fd));
    LUMEN_CHECK(size > 0);

    const int raw = fd.value().get();
    LUMEN_CHECK(raw >= 0);

    void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, raw, 0);
    LUMEN_CHECK(map != MAP_FAILED);
    if (map != MAP_FAILED) {
        const auto* text = static_cast<const char*>(map);
        // A real compiled keymap, not the stub string the compositor used to send.
        LUMEN_CHECK(std::strstr(text, "xkb_keymap") != nullptr);
        LUMEN_CHECK(std::strstr(text, "xkb_keycodes") != nullptr);
        LUMEN_CHECK(std::strstr(text, "xkb_symbols") != nullptr);
        LUMEN_CHECK(text[size - 1] == '\0'); // clients expect it NUL-terminated
        ::munmap(map, size);
    }
}

// A moved-from keymap must not still own the xkb state, or both copies free it.
LUMEN_TEST(keymap_move_transfers_ownership) {
    lx::input::keyboard_keymap km{};
    if (!compile_us(km))
        return;
    (void)km.update_key(k_key_leftshift, true);

    lx::input::keyboard_keymap moved{std::move(km)};
    LUMEN_CHECK(moved.valid());
    LUMEN_CHECK(moved.named().shift); // state travelled with it
    LUMEN_CHECK(!km.valid());         // NOLINT(bugprone-use-after-move)
}

// ── Seat ────────────────────────────────────────────────────────────────────────────

// Serials gate popup grabs, drag starts and activation. A repeated serial gets a grab
// rejected; a fabricated one can get it wrongly accepted.
LUMEN_TEST(seat_serials_are_monotonic) {
    lx::input::seat s{};
    unsigned previous = s.last_serial();
    for (int i = 0; i < 64; ++i) {
        const unsigned next = s.next_serial();
        LUMEN_CHECK(next > previous);
        LUMEN_CHECK(next == s.last_serial());
        previous = next;
    }
}

// wl_keyboard.enter carries the keys already held. Miscounting leaves the newly focused
// client with keys stuck down forever.
LUMEN_TEST(seat_tracks_which_keys_are_held) {
    lx::input::seat s{};
    LUMEN_CHECK(s.pressed_count() == 0);

    s.note_key(k_key_a, true);
    s.note_key(k_key_b, true);
    LUMEN_CHECK(s.pressed_count() == 2);

    // A repeated press must not double-count — libinput will not send one, but a resumed
    // device or a replayed event can.
    s.note_key(k_key_a, true);
    LUMEN_CHECK(s.pressed_count() == 2);

    s.note_key(k_key_a, false);
    LUMEN_CHECK(s.pressed_count() == 1);
    LUMEN_CHECK(s.pressed_keys()[0] == k_key_b);

    // Releasing something never pressed must not underflow the count.
    s.note_key(k_key_1, false);
    LUMEN_CHECK(s.pressed_count() == 1);

    s.note_key(k_key_b, false);
    LUMEN_CHECK(s.pressed_count() == 0);
}

LUMEN_TEST(seat_constrains_the_pointer_to_a_region) {
    lx::input::seat s{};
    s.set_pointer_position(-50.0, 5000.0);
    s.constrain_pointer({0, 0, 1920, 1080});
    LUMEN_CHECK(s.pointer_x() == 0.0);
    LUMEN_CHECK(s.pointer_y() == 1079.0);

    s.set_pointer_position(500.0, 500.0);
    s.constrain_pointer({0, 0, 1920, 1080});
    LUMEN_CHECK(s.pointer_x() == 500.0); // inside, untouched

    // An empty region is not a reason to teleport the pointer to the origin.
    s.constrain_pointer({0, 0, 0, 0});
    LUMEN_CHECK(s.pointer_x() == 500.0);
}

LUMEN_TEST(seat_tracks_focus_independently_for_pointer_and_keyboard) {
    lx::input::seat s{};
    LUMEN_CHECK(!s.keyboard_focus());
    LUMEN_CHECK(!s.pointer_focus());

    // Focus-follows-mouse and click-to-focus both need these to move separately.
    s.set_keyboard_focus(lx::surface_id{7});
    s.set_pointer_focus(lx::surface_id{9});
    LUMEN_CHECK(s.keyboard_focus() == lx::surface_id{7});
    LUMEN_CHECK(s.pointer_focus() == lx::surface_id{9});
}

// The compiled keymap must survive being moved. input_manager::open() builds the manager
// as a local and returns it by value, so it is moved at least twice before the caller sees
// it — and a seat left out of the move list arrives with no keymap. Nothing crashes; keys
// simply stop having meaning, which is why this was only visible in a diagnostic log.
LUMEN_TEST(seat_keymap_survives_being_moved) {
    lx::input::seat original{};
    if (!compile_us(original.keymap()))
        return;
    (void)original.keymap().update_key(k_key_leftshift, true);
    original.note_key(k_key_a, true);
    (void)original.next_serial();
    const unsigned serial_before = original.last_serial();

    lx::input::seat moved{std::move(original)};

    LUMEN_CHECK(moved.keymap().valid());
    LUMEN_CHECK(moved.keymap().named().shift);  // live xkb state travelled
    LUMEN_CHECK(moved.pressed_count() == 1);    // held keys travelled
    LUMEN_CHECK(moved.last_serial() == serial_before);

    char buf[8]{};
    LUMEN_CHECK(moved.keymap().utf8(k_key_a, buf, sizeof(buf)) == 1);
    LUMEN_CHECK(buf[0] == 'A'); // still shifted, so the keymap is genuinely usable

    // Move-assignment has to carry it too — that is the path input_manager takes when the
    // compositor stores the opened manager.
    lx::input::seat assigned{};
    assigned = std::move(moved);
    LUMEN_CHECK(assigned.keymap().valid());
    LUMEN_CHECK(assigned.keymap().named().shift);
    LUMEN_CHECK(assigned.pressed_count() == 1);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
