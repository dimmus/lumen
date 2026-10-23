// lumen-shell's notifications (design-session-and-wm N.2–N.4): the model (ids, replaces_id,
// timeouts by urgency, expiry and its pauses, close reasons, do-not-disturb and critical,
// the lock queue, the history bound, transient, the rate limit, invoking actions), the body
// markup subset, image-data validation, the wrapped body layout, the popup stack's layout,
// and the D-Bus server over a socketpair (signatures, capabilities, the hints read,
// CloseNotification's error, ActivationToken before ActionInvoked before NotificationClosed).
#include "lumen_test.hpp"

#if defined(LUMEN_HAS_LOGIND)
#include <systemd/sd-bus.h>
#include <sys/socket.h>
#endif

import std;
import lx.foundation;
import lx.text;
import lx.shell;

namespace {

using namespace lx::shell;

constexpr long long k_s = 1'000'000'000LL;

struct closed_log {
    std::vector<std::pair<unsigned, unsigned>> events;
    unsigned reserved = 0;
};

notification_center make_center(closed_log& log, notify_settings s = {}) {
    notification_center c;
    c.set_sink({.closed =
                    [](unsigned id, close_reason r, void* user) {
                        static_cast<closed_log*>(user)->events.push_back({id, static_cast<unsigned>(r)});
                    },
                .reserve = [](unsigned next, void* user) { static_cast<closed_log*>(user)->reserved = next; },
                .user = &log});
    c.configure(s, 0);
    return c;
}

notify_request req(const char* summary, notify_urgency u = notify_urgency::normal, int timeout = -1) {
    notify_request r{};
    r.app_name = "test";
    r.summary = summary;
    r.urgency = u;
    r.expire_timeout = timeout;
    return r;
}

std::string shown_of(const notification_center& c) {
    const notification* s[16];
    const unsigned n = c.shown(s, 16);
    std::string out;
    for (unsigned i = 0; i < n; ++i) {
        if (i)
            out += ' ';
        out += s[i]->req.summary;
    }
    return out;
}

std::string history_of(const notification_center& c) {
    const notification* s[64];
    const unsigned n = c.history(s, 64);
    std::string out;
    for (unsigned i = 0; i < n; ++i) {
        if (i)
            out += ' ';
        out += s[i]->req.summary;
        out += s[i]->open ? "+" : "-";
    }
    return out;
}

} // namespace

LUMEN_TEST(ids_and_replacement) {
    closed_log log;
    notification_center c = make_center(log);
    c.set_next_id(1);
    const unsigned a = c.notify(req("a"), 0);
    const unsigned b = c.notify(req("b"), 0);
    LUMEN_CHECK(a == 1 && b == 2);
    // The first id reserved a block (persisted by the shell).
    LUMEN_CHECK(log.reserved == 1 + notification_center::k_id_block);
    // replaces_id of an open one: same id, same place, content and rev anew.
    const unsigned rev = c.find(a)->rev;
    notify_request r = req("a2");
    r.replaces_id = a;
    LUMEN_CHECK(c.notify(std::move(r), k_s) == a);
    LUMEN_CHECK(c.find(a)->req.summary == "a2" && c.find(a)->rev == rev + 1);
    LUMEN_CHECK(shown_of(c) == "b a2");
    // An unknown replaces_id: a new notification with a new id.
    notify_request u = req("c");
    u.replaces_id = 999;
    LUMEN_CHECK(c.notify(std::move(u), k_s) == 3);
    // A restarted shell continues after the persisted block.
    notification_center d = make_center(log);
    d.set_next_id(log.reserved);
    LUMEN_CHECK(d.notify(req("x"), 0) == 1 + notification_center::k_id_block);
}

LUMEN_TEST(timeouts_by_urgency) {
    closed_log log;
    notify_settings s{};
    s.timeout_ms = 5000;
    s.timeout_low_ms = 3000;
    s.critical_timeout_ms = 0;
    s.max_visible = 5;
    notification_center c = make_center(log, s);
    const unsigned n = c.notify(req("normal"), 0);
    const unsigned l = c.notify(req("low", notify_urgency::low), 0);
    const unsigned cr = c.notify(req("critical", notify_urgency::critical), 0);
    const unsigned never = c.notify(req("never", notify_urgency::normal, 0), 0);
    const unsigned ms = c.notify(req("ms", notify_urgency::normal, 1500), 0);
    LUMEN_CHECK(c.next_deadline() == 1500'000'000LL);
    LUMEN_CHECK(c.expire(1500'000'000LL));
    LUMEN_CHECK(log.events.size() == 1 && log.events[0] == std::make_pair(ms, 1u));
    (void)c.expire(3 * k_s);
    (void)c.expire(5 * k_s);
    LUMEN_CHECK(!c.find(l)->open && !c.find(n)->open);
    // Critical with critical-timeout 0, and an explicit 0: never.
    (void)c.expire(1000 * k_s);
    LUMEN_CHECK(c.find(cr)->open && c.find(never)->open);
    LUMEN_CHECK(shown_of(c) == "never critical");
    // Expired ones are in the history (by arrival), closed.
    LUMEN_CHECK(history_of(c) == "ms- low- normal-"); // newest arrival first
}

LUMEN_TEST(hover_and_lock_pause) {
    closed_log log;
    notification_center c = make_center(log);
    const unsigned a = c.notify(req("a"), 0);
    c.set_hover(true, 2 * k_s);
    LUMEN_CHECK(c.next_deadline() == 0);
    LUMEN_CHECK(!c.expire(10 * k_s));
    c.set_hover(false, 10 * k_s);
    // 3 s were left when the pointer came.
    LUMEN_CHECK(c.next_deadline() == 13 * k_s);
    c.set_locked(true, 11 * k_s);
    LUMEN_CHECK(shown_of(c).empty() && c.next_deadline() == 0);
    // Arriving while locked: queued; at unlock the newest max-visible pop up, older go to
    // the history (open, unseen).
    for (const char* s : {"q1", "q2", "q3", "q4", "q5"})
        (void)c.notify(req(s), 12 * k_s);
    LUMEN_CHECK(shown_of(c).empty());
    c.set_locked(false, 20 * k_s);
    LUMEN_CHECK(shown_of(c) == "q5 q4 q3");
    LUMEN_CHECK(history_of(c) == "q2+ q1+");
    // The pre-lock one waits behind the newer ones; its timer is not running.
    LUMEN_CHECK(c.find(a)->in_popup && !c.find(a)->running && c.waiting() == 1);
    LUMEN_CHECK(c.unread() == 2);
    c.mark_read();
    LUMEN_CHECK(c.unread() == 0);
}

LUMEN_TEST(close_reasons) {
    closed_log log;
    notification_center c = make_center(log);
    const unsigned a = c.notify(req("a"), 0);
    const unsigned b = c.notify(req("b"), 0);
    LUMEN_CHECK(c.close(a, close_reason::closed, 0));
    LUMEN_CHECK(!c.close(a, close_reason::closed, 0)); // closed already: the D-Bus error
    LUMEN_CHECK(!c.close(4242, close_reason::closed, 0));
    LUMEN_CHECK(c.dismiss(b, 0));
    LUMEN_CHECK(log.events.size() == 2 && log.events[0] == std::make_pair(a, 3u) &&
                log.events[1] == std::make_pair(b, 2u));
    // Neither is kept: the sender withdrew one, the user dismissed the other.
    LUMEN_CHECK(history_of(c).empty() && !c.find(a) && !c.find(b));
    (void)c.notify(req("x"), 0);
    (void)c.notify(req("y"), 0);
    LUMEN_CHECK(c.dismiss_newest(0) && shown_of(c) == "x");
    LUMEN_CHECK(c.dismiss_all(0) == 1 && shown_of(c).empty());
}

LUMEN_TEST(do_not_disturb_and_critical) {
    closed_log log;
    notification_center c = make_center(log);
    c.set_dnd(true, 0);
    (void)c.notify(req("quiet"), 0);
    (void)c.notify(req("loud", notify_urgency::critical), 0);
    LUMEN_CHECK(shown_of(c) == "loud");
    LUMEN_CHECK(history_of(c) == "quiet+");
    LUMEN_CHECK(c.unread() == 1);
    c.set_dnd(false, 0);
    // Automatic while the screen is captured (dnd-while-sharing), not otherwise.
    c.set_capturing(true, 0);
    LUMEN_CHECK(c.dnd() && !c.dnd_manual());
    notify_settings s = c.settings();
    s.dnd_while_sharing = false;
    c.configure(s, 0);
    LUMEN_CHECK(!c.dnd());
}

LUMEN_TEST(invoking_actions) {
    closed_log log;
    notification_center c = make_center(log);
    notify_request r = req("a");
    r.actions = {{"default", ""}, {"ok", "OK"}};
    const unsigned a = c.notify(std::move(r), 0);
    LUMEN_CHECK(c.can_invoke(a, "ok") && c.can_invoke(a, "default") && !c.can_invoke(a, "nope"));
    LUMEN_CHECK(c.begin_invoke(a, "ok", 0));
    // Hidden at once, still open until the signal went out.
    LUMEN_CHECK(shown_of(c).empty() && c.find(a)->open && log.events.empty());
    c.finish_invoke(a, 0);
    LUMEN_CHECK(log.events.size() == 1 && log.events[0] == std::make_pair(a, 2u));
    // Resident: kept, open, in the history.
    notify_request res = req("res");
    res.actions = {{"ok", "OK"}};
    res.resident = true;
    const unsigned b = c.notify(std::move(res), 0);
    LUMEN_CHECK(c.begin_invoke(b, "ok", 0));
    c.finish_invoke(b, 0);
    LUMEN_CHECK(c.find(b) && c.find(b)->open && history_of(c) == "res+");
    // Its sender left: nothing can be invoked.
    notify_request g = req("gone");
    g.sender = ":1.7";
    g.actions = {{"ok", "OK"}};
    const unsigned gid = c.notify(std::move(g), 0);
    LUMEN_CHECK(c.sender_has_actions(":1.7"));
    c.sender_gone(":1.7");
    LUMEN_CHECK(!c.can_invoke(gid, "ok") && !c.sender_has_actions(":1.7"));
}

LUMEN_TEST(history_bound_and_transient) {
    closed_log log;
    notify_settings s{};
    s.history = 3;
    s.max_visible = 1;
    notification_center c = make_center(log, s);
    c.set_dnd(true, 0);
    for (const char* n : {"h1", "h2", "h3", "h4"})
        (void)c.notify(req(n), 0);
    // The oldest left the history and was closed (expired).
    LUMEN_CHECK(history_of(c) == "h4+ h3+ h2+");
    LUMEN_CHECK(log.events.size() == 1 && log.events[0].second == 1u);
    c.set_dnd(false, 0);
    notify_request t = req("transient", notify_urgency::normal, 1000);
    t.transient = true;
    (void)c.notify(std::move(t), 0);
    (void)c.expire(2 * k_s);
    LUMEN_CHECK(history_of(c) == "h4+ h3+ h2+");
    c.clear_history(2 * k_s);
    LUMEN_CHECK(history_of(c).empty() && c.open_count() == 0);
}

LUMEN_TEST(stack_and_rate_limit) {
    closed_log log;
    notification_center c = make_center(log);
    for (const char* n : {"a", "b", "c", "d"})
        (void)c.notify(req(n), 0);
    LUMEN_CHECK(shown_of(c) == "d c b" && c.waiting() == 1);
    // Only the shown ones run.
    LUMEN_CHECK(!c.find(1)->running && c.find(4)->running);
    // A flood from one sender: past 20 a second, merged into its newest.
    notification_center f = make_center(log);
    unsigned last = 0;
    for (int i = 0; i < 30; ++i) {
        notify_request r = req("flood");
        r.sender = ":1.9";
        bool merged = false;
        last = f.notify(std::move(r), 0, &merged);
        LUMEN_CHECK(merged == (i >= 20));
    }
    LUMEN_CHECK(last == 20 && f.open_count() == 20);
    // A second later the window starts again.
    notify_request r = req("later");
    r.sender = ":1.9";
    LUMEN_CHECK(f.notify(std::move(r), 2 * k_s) == 21);
}

LUMEN_TEST(markup_subset) {
    rich_text t;
    parse_body_markup("a <b>bold</b> <i>it <u>both</u></i> &amp; &lt;x&gt; &#65;&#x42;", t);
    LUMEN_CHECK(t.text == "a bold it both & <x> AB");
    LUMEN_CHECK(t.spans.size() == 3);
    LUMEN_CHECK(t.spans[0].begin == 2 && t.spans[0].end == 6 &&
                t.spans[0].weight == lx::text::weight::bold);
    LUMEN_CHECK(t.spans[1].style == lx::text::style::italic && !t.spans[1].underline);
    LUMEN_CHECK(t.spans[2].style == lx::text::style::italic && t.spans[2].underline);
    // Links: only http, https, mailto; the text stays either way.
    parse_body_markup("<a href=\"https://x.org/?a=1&amp;b=2\">site</a> "
                      "<a href='javascript:alert(1)'>bad</a> <a href=mailto:m@x.org>mail</a>",
                      t);
    LUMEN_CHECK(t.text == "site bad mail");
    LUMEN_CHECK(t.links.size() == 2 && t.links[0] == "https://x.org/?a=1&b=2" &&
                t.links[1] == "mailto:m@x.org");
    LUMEN_CHECK(t.spans.size() == 2 && t.spans[0].tag == 1 && t.spans[1].tag == 2 &&
                t.spans[0].underline);
    // Unknown tags are text; an open tag closes at the end; a stray closing tag goes.
    parse_body_markup("<span>x</span> <b>open</i> end", t);
    LUMEN_CHECK(t.text == "<span>x</span> open end");
    LUMEN_CHECK(t.spans.size() == 1 && t.spans[0].end == t.text.size());
    // img: its alt text; br: a line break; a lone '&' and '<' are text.
    parse_body_markup("<img src=\"x.png\" alt=\"[pic]\"/> a<br/>b & c < d", t);
    LUMEN_CHECK(t.text == "[pic] a\nb & c < d");
    LUMEN_CHECK(markup_to_plain("<b>x</b>&amp;y") == "x&y");
    LUMEN_CHECK(link_allowed("HTTPS://X") && !link_allowed("file:///etc/passwd"));
}

LUMEN_TEST(image_data) {
    // 2x1 RGBA, the rowstride padded: premultiplied ARGB, fitted into a 2x2 square.
    const std::uint8_t rgba[] = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0};
    notify_image img;
    LUMEN_CHECK(decode_image_data(2, 1, 10, true, 8, 4, rgba, 96, img) == image_data_error::none);
    LUMEN_CHECK(img.size == 2 && img.pixels.size() == 4);
    LUMEN_CHECK(img.pixels[0] == 0xFFFF0000u);               // red
    LUMEN_CHECK(img.pixels[1] == 0x80008000u);               // half green, premultiplied
    LUMEN_CHECK(img.pixels[2] == 0u && img.pixels[3] == 0u); // the square's rest: transparent
    // RGB: 3 channels, opaque.
    const std::uint8_t rgb[] = {1, 2, 3};
    LUMEN_CHECK(decode_image_data(1, 1, 3, false, 8, 3, rgb, 96, img) == image_data_error::none &&
                img.pixels[0] == 0xFF010203u);
    // The last row may be unpadded; shorter is refused.
    std::vector<std::uint8_t> two_rows(16 + 8, 0x7F);
    LUMEN_CHECK(decode_image_data(2, 2, 16, true, 8, 4, two_rows, 96, img) == image_data_error::none);
    LUMEN_CHECK(decode_image_data(2, 2, 16, true, 8, 4, std::span(two_rows).first(20), 96, img) ==
                image_data_error::short_data);
    LUMEN_CHECK(decode_image_data(2, 2, 16, true, 16, 4, two_rows, 96, img) ==
                image_data_error::bad_format);
    LUMEN_CHECK(decode_image_data(2, 2, 16, false, 8, 4, two_rows, 96, img) ==
                image_data_error::bad_format);
    LUMEN_CHECK(decode_image_data(0, 2, 16, true, 8, 4, two_rows, 96, img) ==
                image_data_error::bad_size);
    LUMEN_CHECK(decode_image_data(5000, 1, 20000, true, 8, 4, two_rows, 96, img) ==
                image_data_error::bad_size);
    // A large one is kept at the maximum size.
    std::vector<std::uint8_t> big(512 * 256 * 3, 0xC0);
    LUMEN_CHECK(decode_image_data(512, 256, 512 * 3, false, 8, 3, big, 96, img) ==
                    image_data_error::none &&
                img.size == 96 && img.pixels[48 * 96 + 48] == 0xFFC0C0C0u && img.pixels[0] == 0u);
}

LUMEN_TEST(wrapped_body) {
    lx::text::wrapped_text w{};
    const lx::text::font_desc f{"Sans", 10.f, lx::text::weight::normal, lx::text::style::normal};
    const std::string text = "The quick brown fox jumps over the lazy dog, again and again and again "
                             "until the paragraph needs more lines than it is given.";
    auto r = lx::text::wrap_text(text, {}, f, 200, 3, w);
    if (!r)
        return; // no HarfBuzz/FreeType or no font here: the layout is not testable
    LUMEN_CHECK(w.line_count == 3 && w.truncated);
    for (unsigned i = 0; i < w.line_count; ++i)
        LUMEN_CHECK(w.lines[i].width_px <= 200);
    const lx::text::wrap_line& last = w.lines[2];
    LUMEN_CHECK(last.count > 0 && w.pieces[last.first + last.count - 1].ellipsis);
    // A hard break, then a word wider than the line: broken between clusters.
    r = lx::text::wrap_text("ab\ncdefghijklmnopqrstuvwxyzcdefghijklmnopqrstuvwxyz", {}, f, 60, 8, w);
    LUMEN_CHECK(r && w.line_count >= 3 && !w.truncated);
    LUMEN_CHECK(w.pieces[w.lines[0].first].end == 2);
    for (unsigned i = 0; i < w.line_count; ++i)
        LUMEN_CHECK(w.lines[i].width_px <= 60);
    // CJK breaks between characters.
    r = lx::text::wrap_text("\xE4\xB8\x80\xE4\xBA\x8C\xE4\xB8\x89\xE5\x9B\x9B\xE4\xBA\x94\xE5\x85\xAD"
                            "\xE4\xB8\x83\xE5\x85\xAB\xE4\xB9\x9D\xE5\x8D\x81",
                            {}, f, 40, 10, w);
    LUMEN_CHECK(r && w.line_count >= 2);
    // Styled spans keep their pieces apart; a piece is found by its position.
    const lx::text::text_span spans[] = {{0, 4, lx::text::weight::bold, lx::text::style::normal,
                                          false, 0},
                                         {5, 9, lx::text::weight::normal, lx::text::style::italic,
                                          true, 1}};
    r = lx::text::wrap_text("bold link tail", spans, f, 400, 2, w);
    LUMEN_CHECK(r && w.line_count == 1 && w.piece_count == 3);
    LUMEN_CHECK(w.pieces[0].span == 0 && w.pieces[1].span == 1 && w.pieces[2].span == 0xFFFF);
    const int mid = w.pieces[1].x_px + w.pieces[1].width_px / 2;
    LUMEN_CHECK(lx::text::wrapped_piece_at(w, mid, 2) == 1);
    LUMEN_CHECK(lx::text::wrapped_piece_at(w, mid, w.line_height_px * 2) == -1);
}

LUMEN_TEST(popup_stack_layout) {
    closed_log log;
    notification_center c = make_center(log);
    notify_request a = req("with actions");
    a.actions = {{"default", ""}, {"a1", "One"}, {"a2", "Two"}, {"a3", "Three"}, {"a4", "Four"}};
    a.body = "body";
    (void)c.notify(std::move(a), 0);
    (void)c.notify(req("plain"), 0);
    const notification* s[4];
    const unsigned n = c.shown(s, 4);
    notify_layout l{};
    layout_notifications(
        s, n, 0, notify_position::top_right, [](const notification&, int, void*) { return 34; },
        nullptr, [](const notification&, void*) { return false; }, l);
    LUMEN_CHECK(l.count == 2 && l.width == k_card_width);
    // Newest at the top; the card with four actions shows two buttons and "More…".
    LUMEN_CHECK(l.cards[0].box.y == 0 && l.cards[1].box.y == l.cards[0].box.height + k_card_gap);
    const card_layout& acts = l.cards[1];
    LUMEN_CHECK(acts.buttons == 3 && acts.button_action[0] == 1 && acts.button_action[1] == 2 &&
                acts.button_action[2] == -1);
    LUMEN_CHECK(acts.body.height == 34 && acts.button[0].y > acts.body.y + 34 - 1);
    LUMEN_CHECK(l.height == l.cards[1].box.y + l.cards[1].box.height);
    // Bottom positions: the newest at the bottom, "+n more" above.
    layout_notifications(
        s, n, 2, notify_position::bottom_left, [](const notification&, int, void*) { return 0; },
        nullptr, nullptr, l);
    LUMEN_CHECK(l.more == 2 && l.more_box.y == 0 && l.cards[0].box.y > l.cards[1].box.y);
}

#if defined(LUMEN_HAS_LOGIND)

namespace {

struct peer {
    sd_bus* server = nullptr;
    sd_bus* client = nullptr;
    peer() {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
            throw lumen_test::assertion_error("socketpair");
        sd_id128_t id{};
        (void)sd_id128_randomize(&id);
        sd_bus_new(&server);
        sd_bus_set_fd(server, fds[0], fds[0]);
        sd_bus_set_server(server, 1, id);
        sd_bus_start(server);
        sd_bus_new(&client);
        sd_bus_set_fd(client, fds[1], fds[1]);
        sd_bus_start(client);
    }
    ~peer() {
        sd_bus_flush_close_unref(client);
        sd_bus_flush_close_unref(server);
    }
    void pump() {
        for (int i = 0; i < 200; ++i) {
            const int a = sd_bus_process(server, nullptr);
            const int b = sd_bus_process(client, nullptr);
            if (a <= 0 && b <= 0)
                return;
        }
    }
};

struct reply_box {
    bool done = false;
    std::string error;
    sd_bus_message* msg = nullptr;
    ~reply_box() { sd_bus_message_unref(msg); }
};

int on_reply(sd_bus_message* m, void* data, sd_bus_error*) {
    auto* box = static_cast<reply_box*>(data);
    box->done = true;
    if (sd_bus_message_is_method_error(m, nullptr))
        box->error = sd_bus_message_get_error(m)->name;
    else
        box->msg = sd_bus_message_ref(m);
    return 0;
}

std::vector<std::string> g_signals;

int on_signal(sd_bus_message* m, void*, sd_bus_error*) {
    std::string s = sd_bus_message_get_member(m);
    std::uint32_t id = 0;
    if (sd_bus_message_read(m, "u", &id) > 0) {
        s += " " + std::to_string(id);
        char t = 0;
        (void)sd_bus_message_peek_type(m, &t, nullptr);
        if (t == 'u') {
            std::uint32_t v = 0;
            (void)sd_bus_message_read(m, "u", &v);
            s += " " + std::to_string(v);
        } else if (t == 's') {
            const char* v = nullptr;
            (void)sd_bus_message_read(m, "s", &v);
            s += std::string{" "} + (v ? v : "");
        }
    }
    g_signals.push_back(s);
    return 0;
}

constexpr const char* k_dest = "org.freedesktop.Notifications";
constexpr const char* k_path = "/org/freedesktop/Notifications";

} // namespace

LUMEN_TEST(dbus_server) {
    peer p;
    closed_log log;
    notification_center center = make_center(log);
    notification_server server;
    server.set_clock([] { return 0LL; });
    LUMEN_CHECK(server.attach(p.server, center));
    center.set_sink({.closed = [](unsigned id, close_reason r,
                                  void* user) { static_cast<notification_server*>(user)->emit_closed(id, r); },
                     .reserve = nullptr,
                     .user = &server});
    sd_bus_slot* match = nullptr;
    LUMEN_CHECK(sd_bus_add_match(p.client, &match,
                                 "type='signal',interface='org.freedesktop.Notifications'",
                                 on_signal, nullptr) >= 0);
    g_signals.clear();

    // GetServerInformation, GetCapabilities.
    {
        reply_box box;
        LUMEN_CHECK(sd_bus_call_method_async(p.client, nullptr, k_dest, k_path, k_dest,
                                             "GetServerInformation", on_reply, &box, "") >= 0);
        p.pump();
        const char *name = nullptr, *vendor = nullptr, *version = nullptr, *spec = nullptr;
        LUMEN_CHECK(box.msg && sd_bus_message_read(box.msg, "ssss", &name, &vendor, &version, &spec) > 0);
        LUMEN_CHECK(std::string{name} == "Lumen" && std::string{spec} == "1.2");
    }
    {
        reply_box box;
        LUMEN_CHECK(sd_bus_call_method_async(p.client, nullptr, k_dest, k_path, k_dest,
                                             "GetCapabilities", on_reply, &box, "") >= 0);
        p.pump();
        std::vector<std::string> caps;
        LUMEN_CHECK(box.msg && sd_bus_message_enter_container(box.msg, 'a', "s") > 0);
        const char* c = nullptr;
        while (sd_bus_message_read(box.msg, "s", &c) > 0)
            caps.push_back(c);
        const auto has = [&](const char* s) { return std::find(caps.begin(), caps.end(), s) != caps.end(); };
        LUMEN_CHECK(has("actions") && has("body") && has("body-markup") && has("body-hyperlinks") &&
                    has("icon-static") && has("persistence") && !has("sound") && !has("body-images"));
    }

    // Notify with actions and hints: urgency (byte), category, image-data, resident.
    unsigned id = 0;
    {
        sd_bus_message* call = nullptr;
        LUMEN_CHECK(sd_bus_message_new_method_call(p.client, &call, k_dest, k_path, k_dest, "Notify") >= 0);
        LUMEN_CHECK(sd_bus_message_append(call, "susss", "app", 0u, "icon", "Summary", "<b>Body</b>") >= 0);
        LUMEN_CHECK(sd_bus_message_append(call, "as", 4, "default", "", "ok", "OK") >= 0);
        LUMEN_CHECK(sd_bus_message_open_container(call, 'a', "{sv}") >= 0);
        LUMEN_CHECK(sd_bus_message_append(call, "{sv}", "urgency", "y", 2) >= 0);
        LUMEN_CHECK(sd_bus_message_append(call, "{sv}", "category", "s", "im.received") >= 0);
        LUMEN_CHECK(sd_bus_message_append(call, "{sv}", "x", "i", 10) >= 0);
        LUMEN_CHECK(sd_bus_message_open_container(call, 'e', "sv") >= 0);
        LUMEN_CHECK(sd_bus_message_append(call, "s", "image-data") >= 0);
        LUMEN_CHECK(sd_bus_message_open_container(call, 'v', "(iiibiiay)") >= 0);
        LUMEN_CHECK(sd_bus_message_open_container(call, 'r', "iiibiiay") >= 0);
        const std::uint8_t px[] = {10, 20, 30, 255};
        LUMEN_CHECK(sd_bus_message_append(call, "iiibii", 1, 1, 4, 1, 8, 4) >= 0);
        LUMEN_CHECK(sd_bus_message_append_array(call, 'y', px, sizeof(px)) >= 0);
        LUMEN_CHECK(sd_bus_message_close_container(call) >= 0);
        LUMEN_CHECK(sd_bus_message_close_container(call) >= 0);
        LUMEN_CHECK(sd_bus_message_close_container(call) >= 0);
        LUMEN_CHECK(sd_bus_message_close_container(call) >= 0);
        LUMEN_CHECK(sd_bus_message_append(call, "i", -1) >= 0);
        reply_box box;
        LUMEN_CHECK(sd_bus_call_async(p.client, nullptr, call, on_reply, &box, 0) >= 0);
        sd_bus_message_unref(call);
        p.pump();
        LUMEN_CHECK(box.msg && sd_bus_message_read(box.msg, "u", &id) > 0 && id > 0);
    }
    const notification* n = center.find(id);
    LUMEN_CHECK(n && n->req.urgency == notify_urgency::critical && n->req.category == "im.received");
    LUMEN_CHECK(n->req.actions.size() == 2 && n->req.actions[1].key == "ok");
    LUMEN_CHECK(n->req.image.size == 1 && n->req.image.pixels[0] == 0xFF0A141Eu);
    LUMEN_CHECK(n->body.text == "Body" && n->body.spans.size() == 1);
    LUMEN_CHECK(server.take_changed());

    // An action: ActivationToken, ActionInvoked, then NotificationClosed(2), in that order.
    LUMEN_CHECK(center.begin_invoke(id, "ok", 0));
    server.action_invoked(id, "ok", "tok123", 0);
    p.pump();
    LUMEN_CHECK(g_signals.size() == 3);
    LUMEN_CHECK(g_signals[0] == "ActivationToken " + std::to_string(id) + " tok123");
    LUMEN_CHECK(g_signals[1] == "ActionInvoked " + std::to_string(id) + " ok");
    LUMEN_CHECK(g_signals[2] == "NotificationClosed " + std::to_string(id) + " 2");

    // CloseNotification: reason 3; an unknown (or closed) id: an error.
    unsigned id2 = 0;
    {
        reply_box box;
        LUMEN_CHECK(sd_bus_call_method_async(p.client, nullptr, k_dest, k_path, k_dest, "Notify",
                                             on_reply, &box, "susssasa{sv}i", "app", 0u, "", "two",
                                             "", 0, 0, 5000) >= 0);
        p.pump();
        LUMEN_CHECK(box.msg && sd_bus_message_read(box.msg, "u", &id2) > 0 && id2 == id + 1);
    }
    g_signals.clear();
    {
        reply_box box;
        LUMEN_CHECK(sd_bus_call_method_async(p.client, nullptr, k_dest, k_path, k_dest,
                                             "CloseNotification", on_reply, &box, "u", id2) >= 0);
        p.pump();
        LUMEN_CHECK(box.done && box.error.empty());
        LUMEN_CHECK(g_signals.size() == 1 && g_signals[0] == "NotificationClosed " + std::to_string(id2) + " 3");
    }
    {
        reply_box box;
        LUMEN_CHECK(sd_bus_call_method_async(p.client, nullptr, k_dest, k_path, k_dest,
                                             "CloseNotification", on_reply, &box, "u", id2) >= 0);
        p.pump();
        LUMEN_CHECK(box.done && !box.error.empty());
    }
    // replaces_id over the bus: the same id comes back.
    {
        reply_box box;
        LUMEN_CHECK(sd_bus_call_method_async(p.client, nullptr, k_dest, k_path, k_dest, "Notify",
                                             on_reply, &box, "susssasa{sv}i", "app", id2 + 1, "",
                                             "three", "", 0, 0, -1) >= 0);
        p.pump();
        unsigned id3 = 0;
        LUMEN_CHECK(box.msg && sd_bus_message_read(box.msg, "u", &id3) > 0 && id3 == id2 + 1);
        reply_box again;
        LUMEN_CHECK(sd_bus_call_method_async(p.client, nullptr, k_dest, k_path, k_dest, "Notify",
                                             on_reply, &again, "susssasa{sv}i", "app", id3, "",
                                             "three b", "", 0, 0, -1) >= 0);
        p.pump();
        unsigned id4 = 0;
        LUMEN_CHECK(again.msg && sd_bus_message_read(again.msg, "u", &id4) > 0 && id4 == id3);
        LUMEN_CHECK(center.find(id3)->req.summary == "three b");
    }
    sd_bus_slot_unref(match);
    server.detach();
}

#endif

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
