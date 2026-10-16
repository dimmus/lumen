// xdg-activation's focus-stealing-prevention policy (`decide_activation`, `decide_grantable`)
// row by row, serial wraparound, the lifetime boundary, the token table (eviction, single
// use, constant-time lookup by exact text) and the token text (format, uniqueness).
#include <cstring>
#include <set>
#include <string>

#include "lumen_test.hpp"

import lx.foundation;
import lx.runtime;
import lx.compositor;

using lx::compositor::activation_context;
using lx::compositor::activation_grant;
using lx::compositor::activation_issue;
using lx::compositor::activation_reason;
using lx::compositor::activation_result;
using lx::compositor::activation_tokens;
using lx::compositor::decide_activation;
using lx::compositor::decide_grantable;

namespace {

constexpr long long k_s = 1'000'000'000;

const lx::client_id issuer{1};
const lx::client_id target{2};
const lx::client_id other{3};

/// A grantable token issued at t=100 s with serial 50.
activation_grant good_grant() {
    activation_grant g{};
    g.issuer = issuer;
    g.serial = 50;
    g.has_serial = true;
    g.grantable = true;
    g.created_ns = 100 * k_s;
    return g;
}

/// A mapped, unfocused toplevel of `target`, activated 1 s after issue, while the issuer's
/// window has the keyboard.
activation_context base(const activation_grant& g) {
    activation_context c{};
    c.token = &g;
    c.now = 101 * k_s;
    c.target_client = target;
    c.target_is_toplevel = true;
    c.target_is_mapped = true;
    c.focused_exists = true;
    c.focused_client = issuer;
    c.focused_last_user_serial = 50;
    return c;
}

activation_result decide(const activation_context& c, activation_reason& why) {
    return decide_activation(c, why);
}

} // namespace

LUMEN_TEST(grantable_at_issue) {
    activation_issue i{};
    LUMEN_CHECK(decide_grantable(i) == activation_reason::no_serial);
    i.has_serial = true;
    LUMEN_CHECK(decide_grantable(i) == activation_reason::invalid_serial);
    i.serial_delivered = true;
    LUMEN_CHECK(decide_grantable(i) == activation_reason::not_focused);
    i.issuer_has_keyboard = true;
    LUMEN_CHECK(decide_grantable(i) == activation_reason::ok);
    // A launcher that never holds the keyboard (a layer-shell panel): its own click.
    i.issuer_has_keyboard = false;
    i.serial_is_last_press = true;
    LUMEN_CHECK(decide_grantable(i) == activation_reason::ok);
    // A delivered serial that is not a press does not count without the keyboard; one never
    // delivered does not count with it.
    i.serial_delivered = false;
    i.issuer_has_keyboard = true;
    LUMEN_CHECK(decide_grantable(i) == activation_reason::invalid_serial);
}

LUMEN_TEST(policy_rows_in_order) {
    const activation_grant g = good_grant();
    activation_reason why{};

    // Row 9: the baseline is granted.
    activation_context c = base(g);
    LUMEN_CHECK(decide(c, why) == activation_result::granted && why == activation_reason::ok);

    // Row 1: not a toplevel → ignored, before anything else (even an unknown token).
    c = base(g);
    c.target_is_toplevel = false;
    c.token = nullptr;
    LUMEN_CHECK(decide(c, why) == activation_result::ignored &&
                why == activation_reason::not_toplevel);

    // Row 2: unknown token → urgent (also for an unmapped window: row 2 precedes row 4).
    c = base(g);
    c.token = nullptr;
    c.target_is_mapped = false;
    LUMEN_CHECK(decide(c, why) == activation_result::urgent &&
                why == activation_reason::unknown_token);

    // Row 3: expired → urgent, before the pending row.
    c = base(g);
    c.now = g.created_ns + lx::compositor::k_activation_lifetime_ns + 1;
    c.target_is_mapped = false;
    LUMEN_CHECK(decide(c, why) == activation_result::urgent && why == activation_reason::expired);

    // Row 4: not mapped → pending, even with a token that would be refused later.
    activation_grant refused = good_grant();
    refused.has_serial = false;
    c = base(refused);
    c.target_is_mapped = false;
    LUMEN_CHECK(decide(c, why) == activation_result::pending);

    // Row 5: already focused → granted (a raise), even without a serial.
    c = base(refused);
    c.target_already_focused = true;
    LUMEN_CHECK(decide(c, why) == activation_result::granted &&
                why == activation_reason::already_focused);

    // Row 6: no serial → urgent (foot's bell).
    c = base(refused);
    LUMEN_CHECK(decide(c, why) == activation_result::urgent && why == activation_reason::no_serial);

    // Row 7: not grantable → urgent, with the reason decided at issue.
    activation_grant not_focused = good_grant();
    not_focused.grantable = false;
    not_focused.refusal = activation_reason::not_focused;
    c = base(not_focused);
    LUMEN_CHECK(decide(c, why) == activation_result::urgent &&
                why == activation_reason::not_focused);
    activation_grant invalid = good_grant();
    invalid.grantable = false;
    invalid.refusal = activation_reason::invalid_serial;
    c = base(invalid);
    LUMEN_CHECK(decide(c, why) == activation_result::urgent &&
                why == activation_reason::invalid_serial);

    // Row 8: a third client got a press after the token's serial → urgent.
    c = base(g);
    c.focused_client = other;
    c.focused_last_user_serial = 51;
    LUMEN_CHECK(decide(c, why) == activation_result::urgent &&
                why == activation_reason::user_moved_on);
    // … but not one before it, nor the same serial.
    c.focused_last_user_serial = 49;
    LUMEN_CHECK(decide(c, why) == activation_result::granted);
    c.focused_last_user_serial = 50;
    LUMEN_CHECK(decide(c, why) == activation_result::granted);
    // A focused client that never got a press: 0 is "none", not a serial.
    c.focused_last_user_serial = 0;
    LUMEN_CHECK(decide(c, why) == activation_result::granted);
    // The issuer or the target being focused never counts as moving on.
    c = base(g);
    c.focused_client = issuer;
    c.focused_last_user_serial = 900;
    LUMEN_CHECK(decide(c, why) == activation_result::granted);
    c.focused_client = target;
    LUMEN_CHECK(decide(c, why) == activation_result::granted);
    // Nothing focused at all.
    c = base(g);
    c.focused_exists = false;
    c.focused_client = other;
    c.focused_last_user_serial = 900;
    LUMEN_CHECK(decide(c, why) == activation_result::granted);
}

LUMEN_TEST(decision_at_map) {
    using lx::compositor::decide_activation_at_map;
    const activation_grant g = good_grant();
    activation_reason why{}, token{};

    // Evaluated as mapped, whatever the context says; a good token grants.
    activation_context c = base(g);
    c.target_is_mapped = false;
    LUMEN_CHECK(decide_activation_at_map(c, why, token) == activation_result::granted &&
                why == activation_reason::ok && token == activation_reason::ok);

    // Positive evidence of stealing: the user moved on, or the token went stale → urgent.
    c = base(g);
    c.focused_client = other;
    c.focused_last_user_serial = 60;
    LUMEN_CHECK(decide_activation_at_map(c, why, token) == activation_result::urgent &&
                why == activation_reason::user_moved_on);
    c = base(g);
    c.now = g.created_ns + lx::compositor::k_activation_lifetime_ns + 1;
    LUMEN_CHECK(decide_activation_at_map(c, why, token) == activation_result::urgent &&
                why == activation_reason::expired);

    // Tokens that prove nothing: the new-window policy (granted), the refusal reported.
    c = base(g);
    c.token = nullptr;
    LUMEN_CHECK(decide_activation_at_map(c, why, token) == activation_result::granted &&
                why == activation_reason::new_window && token == activation_reason::unknown_token);
    activation_grant self = good_grant(); // GTK at startup: serial 0, never delivered
    self.serial = 0;
    self.grantable = false;
    self.refusal = activation_reason::invalid_serial;
    c = base(self);
    LUMEN_CHECK(decide_activation_at_map(c, why, token) == activation_result::granted &&
                why == activation_reason::new_window && token == activation_reason::invalid_serial);
    activation_grant bell = good_grant();
    bell.has_serial = false;
    c = base(bell);
    LUMEN_CHECK(decide_activation_at_map(c, why, token) == activation_result::granted &&
                token == activation_reason::no_serial);

    // Nothing focused: nothing to steal from, even with a stale token.
    c = base(g);
    c.focused_exists = false;
    c.now = g.created_ns + lx::compositor::k_activation_lifetime_ns + 1;
    LUMEN_CHECK(decide_activation_at_map(c, why, token) == activation_result::granted &&
                why == activation_reason::nothing_focused && token == activation_reason::expired);

    // Not a toplevel stays ignored.
    c = base(g);
    c.target_is_toplevel = false;
    LUMEN_CHECK(decide_activation_at_map(c, why, token) == activation_result::ignored);
}

LUMEN_TEST(policy_serial_wraparound) {
    activation_grant g = good_grant();
    g.serial = 0xFFFF'FFF0u;
    activation_reason why{};
    activation_context c = base(g);
    c.focused_client = other;
    // Pressed after the token, across the wrap.
    c.focused_last_user_serial = 5;
    LUMEN_CHECK(decide(c, why) == activation_result::urgent &&
                why == activation_reason::user_moved_on);
    // Before the token, just below it.
    c.focused_last_user_serial = 0xFFFF'FFE0u;
    LUMEN_CHECK(decide(c, why) == activation_result::granted);
}

LUMEN_TEST(policy_lifetime_boundary) {
    const activation_grant g = good_grant();
    activation_reason why{};
    activation_context c = base(g);
    c.now = g.created_ns + lx::compositor::k_activation_lifetime_ns;
    LUMEN_CHECK(decide(c, why) == activation_result::granted);
    c.now += 1;
    LUMEN_CHECK(decide(c, why) == activation_result::urgent && why == activation_reason::expired);
    // A custom lifetime.
    c.lifetime = 60 * k_s;
    LUMEN_CHECK(decide(c, why) == activation_result::granted);
}

LUMEN_TEST(token_text_format_and_uniqueness) {
    std::set<std::string> seen;
    for (int i = 0; i < 10000; ++i) {
        char text[33];
        LUMEN_CHECK(lx::compositor::make_activation_token_text(text));
        LUMEN_CHECK(std::strlen(text) == 32);
        for (int k = 0; k < 32; ++k)
            LUMEN_CHECK((text[k] >= '0' && text[k] <= '9') || (text[k] >= 'a' && text[k] <= 'f'));
        seen.insert(text);
    }
    LUMEN_CHECK(seen.size() == 10000);
}

LUMEN_TEST(token_table_find_erase_and_eviction) {
    static activation_tokens tokens; // ~14 KiB: not on the stack
    tokens = activation_tokens{};
    const long long life = lx::compositor::k_activation_lifetime_ns;

    auto* a = tokens.create(1 * k_s, life);
    LUMEN_CHECK(a && a->used && a->grant.created_ns == 1 * k_s);
    LUMEN_CHECK(std::strncmp(a->grant.prefix, a->text, 8) == 0 && a->grant.prefix[8] == '\0');
    char text_a[33];
    std::memcpy(text_a, a->text, sizeof(text_a));
    LUMEN_CHECK(tokens.find(text_a) == a);
    // Exact text only: a prefix, a longer string, null, and a wrong last character miss.
    char partial[33];
    std::memcpy(partial, text_a, sizeof(partial));
    partial[31] = partial[31] == 'a' ? 'b' : 'a';
    LUMEN_CHECK(tokens.find(partial) == nullptr);
    LUMEN_CHECK(tokens.find(std::string(text_a, 8).c_str()) == nullptr);
    LUMEN_CHECK(tokens.find((std::string(text_a) + "0").c_str()) == nullptr);
    LUMEN_CHECK(tokens.find(nullptr) == nullptr);
    LUMEN_CHECK(tokens.find("deadbeef") == nullptr);

    // forget_surface clears the requester by id.
    a->surface = lx::surface_id{7};
    tokens.forget_surface(lx::surface_id{7});
    LUMEN_CHECK(!a->surface);

    // Single use: erased, it is unknown.
    tokens.erase(a);
    LUMEN_CHECK(tokens.find(text_a) == nullptr && tokens.count() == 0);

    // Fill the table at t = 10..73 s; the next one evicts the oldest (t = 10 s).
    char texts[activation_tokens::k_max][33]{};
    for (unsigned i = 0; i < activation_tokens::k_max; ++i) {
        auto* t = tokens.create((10 + i) * k_s, life);
        LUMEN_CHECK(t != nullptr);
        std::memcpy(texts[i], t->text, 33);
    }
    LUMEN_CHECK(tokens.count() == activation_tokens::k_max);
    auto* n = tokens.create(74 * k_s, life);
    LUMEN_CHECK(n && tokens.count() == activation_tokens::k_max);
    LUMEN_CHECK(tokens.find(texts[0]) == nullptr);
    for (unsigned i = 1; i < activation_tokens::k_max; ++i)
        LUMEN_CHECK(tokens.find(texts[i]) != nullptr);
    char newest[33];
    std::memcpy(newest, n->text, sizeof(newest));

    // Later, tokens from t <= 40 s have expired: a new one replaces one of those, and every
    // token still within its lifetime survives.
    const long long later = 40 * k_s + life + 1;
    auto* e = tokens.create(later, life);
    LUMEN_CHECK(e && e->grant.created_ns == later && tokens.count() == activation_tokens::k_max);
    unsigned live_kept = 0;
    for (unsigned i = 31; i < activation_tokens::k_max; ++i) // t = 41..73 s
        live_kept += tokens.find(texts[i]) != nullptr ? 1u : 0u;
    LUMEN_CHECK(live_kept == activation_tokens::k_max - 31);
    LUMEN_CHECK(tokens.find(newest) != nullptr);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
