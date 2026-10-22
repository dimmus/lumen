// The text-input ↔ input-method relay (`lx.compositor:text_input`): focus (enter/leave follow
// the keyboard, only while an input method is bound), the double-buffered text-input state and
// its commit count, activation of the input method (state then done; deactivate; a re-enable
// as deactivate + activate in one done), the input method's commit serial rule, one input
// method per seat, version 2's two-stage cursor rectangle, popup placement and visibility, and
// UTF-8-safe truncation. Pure: the sink records every event as text.
#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.compositor;

using lx::compositor::input_method_record;
using lx::compositor::input_popup_record;
using lx::compositor::text_input_record;
using lx::compositor::text_input_relay;
using lx::compositor::text_input_sink;

namespace {

void* res(unsigned long long n) { return reinterpret_cast<void*>(n * 16); }
unsigned long long id_of(void* p) { return reinterpret_cast<unsigned long long>(p) / 16; }

/// Every event, one per line: "enter ti=1 s=10", "im activate", "done ti=1 serial=2", …
struct recorder {
    std::string log;
    bool routing = false;
    lx::rect2i surface{100, 200, 400, 300};
    lx::rect2i bounds{0, 0, 1000, 800};
    bool on_screen = true;

    void add(const char* fmt, unsigned long long a = 0, long long b = 0, long long c = 0,
             long long d = 0, long long e = 0) {
        char line[160];
        std::snprintf(line, sizeof(line), fmt, a, b, c, d, e);
        log += line;
        log += '\n';
    }
    std::string take() {
        std::string out;
        out.swap(log);
        return out;
    }
};

recorder* self(void* u) { return static_cast<recorder*>(u); }

text_input_sink make_sink(recorder& r) {
    text_input_sink s{};
    s.user = &r;
    s.enter = [](void* u, void* ti, void* sf) {
        self(u)->add("enter ti=%llu s=%lld", id_of(ti), static_cast<long long>(id_of(sf)));
    };
    s.leave = [](void* u, void* ti, void* sf) {
        self(u)->add("leave ti=%llu s=%lld", id_of(ti), static_cast<long long>(id_of(sf)));
    };
    s.preedit = [](void* u, void* ti, const char* text, int b, int e) {
        self(u)->log += "preedit ti=" + std::to_string(id_of(ti)) + " \"" + (text ? text : "") +
                        "\" " + std::to_string(b) + " " + std::to_string(e) + "\n";
    };
    s.commit_string = [](void* u, void* ti, const char* text) {
        self(u)->log += "commit ti=" + std::to_string(id_of(ti)) + " \"" + (text ? text : "") + "\"\n";
    };
    s.delete_surrounding = [](void* u, void* ti, unsigned b, unsigned a) {
        self(u)->add("delete ti=%llu %lld %lld", id_of(ti), b, a);
    };
    s.done = [](void* u, void* ti, unsigned serial) {
        self(u)->add("done ti=%llu serial=%lld", id_of(ti), serial);
    };
    s.im_activate = [](void* u, void*) { self(u)->add("im activate"); };
    s.im_deactivate = [](void* u, void*) { self(u)->add("im deactivate"); };
    s.im_surrounding = [](void* u, void*, const char* text, unsigned c, unsigned a) {
        self(u)->log += std::string("im surrounding \"") + text + "\" " + std::to_string(c) +
                        " " + std::to_string(a) + "\n";
    };
    s.im_change_cause = [](void* u, void*, unsigned c) { self(u)->add("im cause %llu", c); };
    s.im_content_type = [](void* u, void*, unsigned h, unsigned p) {
        self(u)->add("im content_type %llu %lld", h, p);
    };
    s.im_done = [](void* u, void*) { self(u)->add("im done"); };
    s.routing = [](void* u, bool on) {
        self(u)->routing = on;
        self(u)->add("routing %llu", on ? 1u : 0u);
    };
    s.surface_rect = [](void* u, void*, lx::rect2i& g, lx::rect2i& b) {
        g = self(u)->surface;
        b = self(u)->bounds;
        return self(u)->on_screen;
    };
    s.popup_show = [](void* u, void* p, void*, lx::point2i o) {
        self(u)->add("popup show p=%llu %lld,%lld", id_of(p), o.x, o.y);
    };
    s.popup_hide = [](void* u, void* p, void*) { self(u)->add("popup hide p=%llu", id_of(p)); };
    s.popup_rect = [](void* u, void* p, lx::rect2i r) {
        self(u)->add("popup rect p=%llu %lld,%lld %lldx%lld", id_of(p), r.x, r.y, r.width,
                     r.height);
    };
    return s;
}

void make_ti(text_input_record& ti, unsigned long long n, unsigned long long client,
             unsigned version = 2) {
    ti.resource = res(n);
    ti.client = res(client);
    ti.version = version;
}

void make_im(input_method_record& im, unsigned long long n) {
    im.resource = res(n);
    im.client = res(90);
}

void surrounding(text_input_relay& r, text_input_record& ti, const char* text, int c, int a) {
    r.set_surrounding(ti, text, static_cast<unsigned>(std::strlen(text)), c, a);
}

/// A relay with an input method (res 50) and text input 1 (client 7) focused on surface 10.
struct fixture {
    recorder rec{};
    text_input_relay relay{};
    input_method_record im{};
    text_input_record ti{};

    fixture() {
        relay.set_sink(make_sink(rec));
        make_im(im, 50);
        make_ti(ti, 1, 7);
        relay.add(ti);
        LUMEN_CHECK(relay.add_input_method(im));
        relay.keyboard_focus(res(10), res(7));
        LUMEN_CHECK(rec.take() == "enter ti=1 s=10\n");
    }
    void enable_and_commit() {
        relay.enable(ti);
        surrounding(relay, ti, "hello", 5, 5);
        relay.set_content_type(ti, 0x200, 13);
        relay.commit(ti);
    }
};

} // namespace

LUMEN_TEST(focus_follows_the_keyboard_only_while_an_input_method_is_bound) {
    recorder rec{};
    text_input_relay r{};
    r.set_sink(make_sink(rec));
    text_input_record a{};
    text_input_record b{};
    make_ti(a, 1, 7);
    make_ti(b, 2, 8);
    r.add(a);
    r.add(b);
    // No input method: no text-input focus (sway) — the toolkit keeps its own key handling.
    r.keyboard_focus(res(10), res(7));
    LUMEN_CHECK(rec.take().empty());
    input_method_record im{};
    make_im(im, 50);
    LUMEN_CHECK(r.add_input_method(im));
    LUMEN_CHECK(rec.take() == "enter ti=1 s=10\n");
    // Focus moves to another client's surface: leave, then enter there.
    r.keyboard_focus(res(11), res(8));
    LUMEN_CHECK(rec.take() == "leave ti=1 s=10\nenter ti=2 s=11\n");
    // A text input created while its client is focused is entered at once.
    text_input_record c{};
    make_ti(c, 3, 8);
    r.add(c);
    LUMEN_CHECK(rec.take() == "enter ti=3 s=11\n");
    // The input method goes: leave (which also clears preedit); it comes back: enter again.
    r.remove_input_method(im);
    LUMEN_CHECK(rec.take() == "leave ti=3 s=11\nleave ti=2 s=11\n");
    input_method_record im2{};
    make_im(im2, 51);
    LUMEN_CHECK(r.add_input_method(im2));
    LUMEN_CHECK(rec.take() == "enter ti=3 s=11\nenter ti=2 s=11\n");
    // The focused surface is destroyed: no leave (it is gone), focus is empty.
    r.focus_destroyed(res(11));
    LUMEN_CHECK(rec.take().empty());
    LUMEN_CHECK(r.focus() == nullptr && b.focus == nullptr && c.focus == nullptr);
    r.remove(a);
    r.remove(b);
    r.remove(c);
    LUMEN_CHECK(r.text_input_count() == 0);
}

LUMEN_TEST(enable_activates_with_state_then_done_and_disable_deactivates) {
    fixture f{};
    f.enable_and_commit();
    LUMEN_CHECK(f.rec.take() == "im activate\nim surrounding \"hello\" 5 5\nim cause 0\n"
                                "im content_type 512 13\nim done\nrouting 1\n");
    LUMEN_CHECK(f.relay.active() == &f.ti && f.relay.routing() && f.im.dones == 1);
    // A state commit while active: the state again (surrounding and cause reset at every IM
    // done, so they are always resent), then done.
    surrounding(f.relay, f.ti, "hello!", 6, 6);
    f.relay.set_change_cause(f.ti, 1);
    f.relay.commit(f.ti);
    LUMEN_CHECK(f.rec.take() == "im surrounding \"hello!\" 6 6\nim cause 1\n"
                                "im content_type 512 13\nim done\n");
    // The cause resets to input_method after each commit.
    f.relay.commit(f.ti);
    LUMEN_CHECK(f.rec.take() == "im surrounding \"hello!\" 6 6\nim cause 0\n"
                                "im content_type 512 13\nim done\n");
    LUMEN_CHECK(f.im.dones == 3);
    f.relay.disable(f.ti);
    f.relay.commit(f.ti);
    LUMEN_CHECK(f.rec.take() == "im deactivate\nim done\nrouting 0\n");
    LUMEN_CHECK(f.relay.active() == nullptr && !f.relay.routing() && f.im.dones == 4);
    LUMEN_CHECK(f.ti.commits == 4);
}

LUMEN_TEST(reenable_is_one_done_and_focus_loss_deactivates) {
    fixture f{};
    f.enable_and_commit();
    (void)f.rec.take();
    // A new field in the same surface: enable again while enabled.
    f.relay.enable(f.ti);
    f.relay.commit(f.ti);
    LUMEN_CHECK(f.rec.take() == "im deactivate\nim activate\nim cause 0\nim done\n");
    // Keyboard focus leaves: the text input's leave, then the input method's deactivate.
    f.relay.keyboard_focus(res(12), res(9));
    LUMEN_CHECK(f.rec.take() == "leave ti=1 s=10\nim deactivate\nim done\nrouting 0\n");
}

LUMEN_TEST(requests_after_leave_are_ignored_but_commits_are_counted) {
    fixture f{};
    f.relay.keyboard_focus(nullptr, nullptr);
    LUMEN_CHECK(f.rec.take() == "leave ti=1 s=10\n");
    f.relay.enable(f.ti);
    f.relay.commit(f.ti);
    f.relay.commit(f.ti);
    LUMEN_CHECK(f.rec.take().empty() && f.relay.active() == nullptr && !f.ti.current.enabled);
    f.relay.keyboard_focus(res(10), res(7));
    LUMEN_CHECK(f.rec.take() == "enter ti=1 s=10\n");
    f.enable_and_commit();
    (void)f.rec.take();
    // The input method commits text: the text input's done carries its own commit count (3).
    f.relay.im_commit_string(f.im, "x", 1);
    f.relay.im_commit(f.im, f.im.dones);
    LUMEN_CHECK(f.rec.take() == "commit ti=1 \"x\"\ndone ti=1 serial=3\n");
}

LUMEN_TEST(input_method_commit_serial_rule) {
    fixture f{};
    f.enable_and_commit();
    (void)f.rec.take();
    LUMEN_CHECK(f.im.dones == 1);
    // A stale serial: pending is dropped, nothing reaches the text input.
    f.relay.im_preedit(f.im, "ni", 2, 2, 2);
    f.relay.im_commit(f.im, 0);
    LUMEN_CHECK(f.rec.take().empty());
    // …and the next valid commit does not resurrect it.
    f.relay.im_commit(f.im, 1);
    LUMEN_CHECK(f.rec.take() == "done ti=1 serial=1\n");
    // preedit, commit string, deletion — in the spec's order — then done.
    f.relay.im_delete_surrounding(f.im, 2, 0);
    f.relay.im_commit_string(f.im, "你好", 6);
    f.relay.im_preedit(f.im, "a", 1, 1, 1);
    f.relay.im_commit(f.im, 1);
    LUMEN_CHECK(f.rec.take() ==
                "preedit ti=1 \"a\" 1 1\ncommit ti=1 \"你好\"\ndelete ti=1 2 0\ndone ti=1 serial=1\n");
    // Inactive: a commit is accepted and has no effect.
    f.relay.disable(f.ti);
    f.relay.commit(f.ti);
    (void)f.rec.take();
    f.relay.im_commit_string(f.im, "y", 1);
    f.relay.im_commit(f.im, f.im.dones);
    LUMEN_CHECK(f.rec.take().empty());
}

LUMEN_TEST(one_input_method_per_seat) {
    fixture f{};
    input_method_record second{};
    make_im(second, 51);
    LUMEN_CHECK(!f.relay.add_input_method(second));
    LUMEN_CHECK(!second.live && f.relay.input_method() == &f.im);
    // Its requests are ignored, its removal changes nothing.
    f.relay.im_commit_string(second, "z", 1);
    f.relay.im_commit(second, 0);
    f.relay.remove_input_method(second);
    LUMEN_CHECK(f.rec.take().empty() && f.relay.input_method() == &f.im);
}

LUMEN_TEST(second_enable_on_the_seat_is_ignored) {
    fixture f{};
    text_input_record other{};
    make_ti(other, 2, 7);
    f.relay.add(other);
    LUMEN_CHECK(f.rec.take() == "enter ti=2 s=10\n");
    f.enable_and_commit();
    (void)f.rec.take();
    f.relay.enable(other);
    f.relay.commit(other);
    LUMEN_CHECK(!other.current.enabled && f.relay.active() == &f.ti);
    LUMEN_CHECK(f.rec.take().empty());
    // Destroying the active text input deactivates the input method.
    f.relay.remove(f.ti);
    LUMEN_CHECK(f.rec.take() == "im deactivate\nim done\nrouting 0\n");
    f.relay.remove(other);
}

LUMEN_TEST(cursor_rectangle_is_two_stage_in_v2_and_immediate_in_v1) {
    fixture f{};
    f.relay.enable(f.ti);
    f.relay.set_cursor_rect(f.ti, {10, 20, 2, 16});
    f.relay.commit(f.ti);
    // Staged, not applied: the popup must move with the text the client is about to show.
    LUMEN_CHECK(!f.ti.current.has_cursor_rect && f.ti.rect_staged);
    f.relay.surface_committed(res(11)); // another surface: nothing
    LUMEN_CHECK(!f.ti.current.has_cursor_rect);
    f.relay.surface_committed(res(10));
    LUMEN_CHECK(f.ti.current.has_cursor_rect && f.ti.current.cursor_rect.x == 10 &&
                f.ti.current.cursor_rect.height == 16 && !f.ti.rect_staged);
    // A committed disable drops both stages.
    f.relay.set_cursor_rect(f.ti, {30, 20, 2, 16});
    f.relay.commit(f.ti);
    f.relay.disable(f.ti);
    f.relay.commit(f.ti);
    LUMEN_CHECK(!f.ti.current.has_cursor_rect && !f.ti.rect_staged);

    text_input_record v1{};
    make_ti(v1, 3, 7, 1);
    f.relay.remove(f.ti);
    f.relay.add(v1);
    f.relay.enable(v1);
    f.relay.set_cursor_rect(v1, {5, 6, 1, 12});
    f.relay.commit(v1);
    LUMEN_CHECK(v1.current.has_cursor_rect && v1.current.cursor_rect.y == 6 && !v1.rect_staged);
    f.relay.remove(v1);
}

LUMEN_TEST(popup_placement_below_flipped_and_slid) {
    using lx::compositor::place_input_popup;
    const lx::rect2i out{0, 0, 1000, 800};
    // Below the cursor, starting at its left edge.
    auto p = place_input_popup({100, 100, 2, 20}, {120, 40}, out);
    LUMEN_CHECK(p.origin.x == 100 && p.origin.y == 120);
    LUMEN_CHECK(p.text_rect.x == 0 && p.text_rect.y == -20 && p.text_rect.width == 2 &&
                p.text_rect.height == 20);
    // No room below: flipped above.
    p = place_input_popup({100, 770, 2, 20}, {120, 40}, out);
    LUMEN_CHECK(p.origin.x == 100 && p.origin.y == 730);
    LUMEN_CHECK(p.text_rect.y == 40);
    // No room right: flipped to end at the cursor's right edge (sway).
    p = place_input_popup({950, 100, 2, 20}, {120, 40}, out);
    LUMEN_CHECK(p.origin.x == 952 - 120 && p.origin.y == 120);
    // Neither side fits: slid inside the output.
    p = place_input_popup({10, 100, 2, 20}, {990, 40}, out);
    LUMEN_CHECK(p.origin.x == 10);
    p = place_input_popup({500, 100, 2, 20}, {990, 40}, out);
    LUMEN_CHECK(p.origin.x == 10 && p.origin.x + 990 <= 1000);
}

LUMEN_TEST(popups_show_while_active_at_the_cursor_and_follow_it) {
    fixture f{};
    input_popup_record pop{};
    pop.resource = res(60);
    pop.surface = res(61);
    f.relay.add_popup(pop, f.im);
    f.relay.popup_committed(pop, true, {120, 40});
    // Inactive: not shown.
    LUMEN_CHECK(f.rec.take().empty() && !pop.shown);
    f.relay.enable(f.ti);
    f.relay.set_cursor_rect(f.ti, {10, 20, 2, 16});
    f.relay.commit(f.ti);
    // Active, its first rectangle staged: the popup waits for the commit that applies it.
    std::string log = f.rec.take();
    LUMEN_CHECK(log.find("popup show") == std::string::npos);
    // The surface commits: the rectangle applies, the popup shows below the cursor and is
    // told where the cursor is relative to itself.
    f.relay.surface_committed(res(10));
    LUMEN_CHECK(f.rec.take() == "popup show p=60 110,236\npopup rect p=60 0,-16 2x16\n");
    // The window moves: the popup follows.
    f.rec.surface = {200, 200, 400, 300};
    f.relay.surface_moved(res(10));
    LUMEN_CHECK(f.rec.take() == "popup show p=60 210,236\n");
    // It grows taller than the room below and above: slid up inside the output, and told
    // where the cursor now is relative to it.
    f.relay.popup_committed(pop, true, {120, 600});
    LUMEN_CHECK(f.rec.take() == "popup show p=60 210,200\npopup rect p=60 0,20 2x16\n");
    // Enabled again without a rectangle: anchored to the whole surface (sway); 600 px tall,
    // it fits neither below nor above and is slid up inside the output.
    f.relay.enable(f.ti);
    f.relay.commit(f.ti);
    log = f.rec.take();
    LUMEN_CHECK(log.find("popup show p=60 200,200\n") != std::string::npos);
    // Deactivated: hidden.
    f.relay.disable(f.ti);
    f.relay.commit(f.ti);
    log = f.rec.take();
    LUMEN_CHECK(log.find("popup hide p=60") != std::string::npos);
    LUMEN_CHECK(!pop.shown);
    // The input method goes: the popup is inert and never shown again.
    f.relay.remove_input_method(f.im);
    LUMEN_CHECK(pop.im == nullptr);
    f.relay.remove_popup(pop);
    LUMEN_CHECK(f.relay.popup_count() == 0);
}

LUMEN_TEST(text_is_cut_at_utf8_boundaries_and_cursor_clamped) {
    using lx::compositor::utf8_truncate;
    // "é" is two bytes (0xc3 0xa9): a cut after its first byte drops it whole.
    const char text[] = "a\xc3\xa9z";
    LUMEN_CHECK(utf8_truncate(text, 4, 2) == 1);
    LUMEN_CHECK(utf8_truncate(text, 4, 3) == 3);
    LUMEN_CHECK(utf8_truncate(text, 4, 10) == 4);
    // A 4-byte character at the 4000-byte limit.
    std::string big(3998, 'x');
    big += "\xf0\x9f\x98\x80"; // U+1F600, 4 bytes: 4002 in all
    LUMEN_CHECK(utf8_truncate(big.data(), static_cast<unsigned>(big.size()), 4000) == 3998);
    fixture f{};
    f.relay.enable(f.ti);
    f.relay.set_surrounding(f.ti, big.data(), static_cast<unsigned>(big.size()), 5000, -3);
    LUMEN_CHECK(f.ti.pending.surrounding_len == 3998 && f.ti.pending.cursor == 3998 &&
                f.ti.pending.anchor == 0 && f.ti.pending.surrounding[3998] == '\0');
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
