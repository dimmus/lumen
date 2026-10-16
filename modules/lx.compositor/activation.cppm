module;

#include <cerrno>
#include <cstddef>
#include <cstring>

#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>

import lx.foundation;
import lx.runtime;

export module lx.compositor:activation;

import :input_router;

export namespace lx::compositor {

/// What `xdg_activation_v1.activate` did (the `activate … result=` log line).
enum class activation_result : unsigned char {
    /// The window got the keyboard and was raised.
    granted,
    /// Refused: the window is marked `demands_attention`, which the shell shows.
    urgent,
    /// The window has not mapped yet: the same rules decide again when it does.
    pending,
    /// The surface is not a toplevel's (a popup, a layer or cursor surface, no role).
    ignored,
};

/// Why. `ok` and `already_focused` grant; every other reason but `not_toplevel` is urgent.
enum class activation_reason : unsigned char {
    ok,
    already_focused,
    /// The token was issued without xdg_activation_token_v1.set_serial (an urgency request,
    /// foot's bell).
    no_serial,
    /// The serial was never delivered to the issuer as an input or focus event.
    invalid_serial,
    /// The issuer had no keyboard focus and the serial is not its last button or key press.
    not_focused,
    unknown_token,
    expired,
    /// The user pressed a key or button in another client after the token was issued (the
    /// X11 user-time rule, as in Mutter).
    user_moved_on,
    not_toplevel,
    /// At map only: refused, but no window holds the keyboard — nothing to steal from, so
    /// the new window takes it as every new window does.
    nothing_focused,
    /// At map only: the token proves nothing either way (unknown, no serial, a serial that
    /// was not the issuer's input — GTK presents a self-issued token with serial 0 for every
    /// new window), so the window is treated as one that presented none: the new-window
    /// policy applies.
    new_window,
};

[[nodiscard]] const char* activation_result_name(activation_result r);
[[nodiscard]] const char* activation_reason_name(activation_reason r);

/// How long an issued token can activate: a cold start of a large application on a slow
/// machine has to fit (niri's 10 s does not, on this project's 2-core VM). The user-time rule
/// (`user_moved_on`), not the lifetime, is what stops a stale token stealing focus.
constexpr lx::runtime::clock_time k_activation_lifetime_ns = 30'000'000'000;

/// What the policy needs of a token, settled when it was issued. Small and copyable: a
/// toplevel that asks before it maps keeps one for the decision at map.
struct activation_grant {
    /// The token's first 8 characters, for logs only (a token is a capability).
    char prefix[9]{};
    lx::client_id issuer{};
    unsigned serial = 0;
    bool has_serial = false;
    /// Decided at commit (`decide_grantable`); `refusal` says why not.
    bool grantable = false;
    activation_reason refusal = activation_reason::ok;
    lx::runtime::clock_time created_ns = 0;
};

/// One issued token.
struct activation_token {
    /// 32 lowercase hex characters (128 random bits) and the NUL.
    char text[33]{};
    activation_grant grant{};
    /// The requesting surface (set_surface), cleared when it is destroyed. Logged only.
    lx::surface_id surface{};
    char app_id[128]{};
    bool used = false;
};

/// The facts of an xdg_activation_token_v1.commit.
struct activation_issue {
    bool has_serial = false;
    /// The serial is an input or focus serial delivered to the issuer (`input_serial_log`).
    bool serial_delivered = false;
    /// The issuer is the client with keyboard focus.
    bool issuer_has_keyboard = false;
    /// The serial is the issuer's last button press or last key press — what lets a
    /// launcher that never takes the keyboard (a layer-shell panel) issue from a click.
    bool serial_is_last_press = false;
};

/// `ok` when the token may later grant focus, else why not (`no_serial`, `invalid_serial`,
/// `not_focused`).
[[nodiscard]] activation_reason decide_grantable(const activation_issue& i);

/// What the decision needs at `activate` (or again at map), gathered by the caller.
struct activation_context {
    /// Null: the token is unknown (never issued, already used, or evicted).
    const activation_grant* token = nullptr;
    /// When the activate request arrived. A decision deferred to map keeps this time, so a
    /// token valid when it was presented stays valid however long the window takes to map.
    lx::runtime::clock_time now = 0;
    lx::runtime::clock_time lifetime = k_activation_lifetime_ns;
    lx::client_id target_client{};
    bool target_is_toplevel = false;
    bool target_is_mapped = false;
    bool target_already_focused = false;
    /// The toplevel holding the keyboard (the window manager's focus decision), if any.
    bool focused_exists = false;
    lx::client_id focused_client{};
    /// The newest key or button press delivered to the focused client; 0 when none.
    unsigned focused_last_user_serial = 0;
};

/// The focus-stealing-prevention policy, rows checked in order:
///   1 not a toplevel → ignored; 2 unknown token → urgent; 3 expired → urgent;
///   4 not mapped → pending; 5 already focused → granted; 6 no serial → urgent;
///   7 not grantable → urgent (the reason decided at commit);
///   8 the focused toplevel's client is neither issuer nor target and received a key or
///     button press after the token's serial → urgent (user_moved_on);
///   9 granted.
[[nodiscard]] activation_result decide_activation(const activation_context& c,
                                                  activation_reason& why);

/// The decision at map for a window that presented its token before mapping (`c` gathered at
/// map, `c.now` the time of the request). A new window takes the keyboard (the new-window
/// policy) unless the token is positive evidence that this would steal it: it was issued for
/// this launch but the user has since moved on (`user_moved_on`) or it went stale
/// (`expired`) — then urgent, and the window opens without the keyboard. A token that proves
/// nothing (`new_window`, `token_reason` says why it was refused) and an empty desk
/// (`nothing_focused`) grant. Never `pending`.
[[nodiscard]] activation_result decide_activation_at_map(const activation_context& c,
                                                         activation_reason& why,
                                                         activation_reason& token_reason);

/// 32 lowercase hex characters from 16 bytes of the kernel's CSPRNG (getrandom, falling back
/// to /dev/urandom). False when neither works: the caller then hands out a token that
/// matches nothing.
[[nodiscard]] bool make_activation_token_text(char (&out)[33]);

/// Issued tokens. Fixed table, UI affinity, no allocation. A full table evicts an expired
/// token, else the oldest. Tokens are single use (erased by the activation that presents
/// them) and outlive their issuer: a launcher may exit before what it launched maps.
class activation_tokens {
public:
    static constexpr unsigned k_max = 64;

    /// A fresh token with random text and `created_ns = now`; null when no randomness is
    /// available.
    [[nodiscard]] activation_token* create(lx::runtime::clock_time now,
                                           lx::runtime::clock_time lifetime);
    /// The token with exactly this text, or null. Compares every slot in constant time per
    /// slot, so the time taken says nothing about how much of a guess was right.
    [[nodiscard]] activation_token* find(const char* text);
    void erase(activation_token* t);
    /// The requesting surface is gone (tokens name it by id; nothing dangles either way).
    void forget_surface(lx::surface_id s);
    [[nodiscard]] unsigned count() const;

private:
    activation_token tokens_[k_max]{};
};

} // namespace lx::compositor

// ── implementation ─────────────────────────────────────────────────────────

const char* lx::compositor::activation_result_name(activation_result r) {
    switch (r) {
    case activation_result::granted: return "granted";
    case activation_result::urgent: return "urgent";
    case activation_result::pending: return "pending";
    case activation_result::ignored: return "ignored";
    }
    return "-";
}

const char* lx::compositor::activation_reason_name(activation_reason r) {
    switch (r) {
    case activation_reason::ok: return "ok";
    case activation_reason::already_focused: return "already_focused";
    case activation_reason::no_serial: return "no_serial";
    case activation_reason::invalid_serial: return "invalid_serial";
    case activation_reason::not_focused: return "not_focused";
    case activation_reason::unknown_token: return "unknown_token";
    case activation_reason::expired: return "expired";
    case activation_reason::user_moved_on: return "user_moved_on";
    case activation_reason::not_toplevel: return "not_toplevel";
    case activation_reason::nothing_focused: return "nothing_focused";
    case activation_reason::new_window: return "new_window";
    }
    return "-";
}

lx::compositor::activation_reason
lx::compositor::decide_grantable(const activation_issue& i) {
    if (!i.has_serial)
        return activation_reason::no_serial;
    if (!i.serial_delivered)
        return activation_reason::invalid_serial;
    if (i.issuer_has_keyboard || i.serial_is_last_press)
        return activation_reason::ok;
    return activation_reason::not_focused;
}

lx::compositor::activation_result
lx::compositor::decide_activation(const activation_context& c, activation_reason& why) {
    if (!c.target_is_toplevel) {
        why = activation_reason::not_toplevel;
        return activation_result::ignored;
    }
    if (!c.token) {
        why = activation_reason::unknown_token;
        return activation_result::urgent;
    }
    if (c.now - c.token->created_ns > c.lifetime) {
        why = activation_reason::expired;
        return activation_result::urgent;
    }
    if (!c.target_is_mapped) {
        why = activation_reason::ok;
        return activation_result::pending;
    }
    if (c.target_already_focused) {
        why = activation_reason::already_focused;
        return activation_result::granted;
    }
    if (!c.token->has_serial) {
        why = activation_reason::no_serial;
        return activation_result::urgent;
    }
    if (!c.token->grantable) {
        why = c.token->refusal == activation_reason::ok ? activation_reason::not_focused
                                                        : c.token->refusal;
        return activation_result::urgent;
    }
    // The user-time rule: once the user has pressed a key or a button in some other window
    // after the click that issued the token, that window is what the user is working in.
    // 0 is "no press ever delivered", not a serial.
    if (c.focused_exists && !(c.focused_client == c.token->issuer) &&
        !(c.focused_client == c.target_client) && c.focused_last_user_serial != 0 &&
        serial_newer(c.focused_last_user_serial, c.token->serial)) {
        why = activation_reason::user_moved_on;
        return activation_result::urgent;
    }
    why = activation_reason::ok;
    return activation_result::granted;
}

lx::compositor::activation_result
lx::compositor::decide_activation_at_map(const activation_context& c, activation_reason& why,
                                         activation_reason& token_reason) {
    activation_context mapped = c;
    mapped.target_is_mapped = true;
    activation_result r = decide_activation(mapped, why);
    token_reason = why;
    if (r != activation_result::urgent)
        return r;
    if (!c.focused_exists) {
        why = activation_reason::nothing_focused;
        return activation_result::granted;
    }
    if (why == activation_reason::user_moved_on || why == activation_reason::expired)
        return activation_result::urgent;
    why = activation_reason::new_window;
    return activation_result::granted;
}

namespace {

/// Fills `buf` from the kernel's CSPRNG. getrandom blocks only until the pool is first
/// initialised (long done by the time a compositor runs).
bool random_bytes(unsigned char* buf, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::getrandom(buf + got, n - got, 0);
        if (r > 0) {
            got += static_cast<std::size_t>(r);
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        break;
    }
    if (got == n)
        return true;
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    got = 0;
    while (got < n) {
        const ssize_t r = ::read(fd, buf + got, n - got);
        if (r > 0) {
            got += static_cast<std::size_t>(r);
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        break;
    }
    ::close(fd);
    return got == n;
}

bool expired(const lx::compositor::activation_token& t, lx::runtime::clock_time now,
             lx::runtime::clock_time lifetime) {
    return now - t.grant.created_ns > lifetime;
}

} // namespace

bool lx::compositor::make_activation_token_text(char (&out)[33]) {
    unsigned char bytes[16];
    if (!random_bytes(bytes, sizeof(bytes))) {
        out[0] = '\0';
        return false;
    }
    constexpr char k_hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) {
        out[2 * i] = k_hex[bytes[i] >> 4];
        out[2 * i + 1] = k_hex[bytes[i] & 0xF];
    }
    out[32] = '\0';
    return true;
}

lx::compositor::activation_token*
lx::compositor::activation_tokens::create(lx::runtime::clock_time now,
                                          lx::runtime::clock_time lifetime) {
    activation_token* slot = nullptr;
    activation_token* oldest = nullptr;
    for (auto& t : tokens_) {
        if (!t.used) {
            slot = &t;
            break;
        }
        if (!slot && expired(t, now, lifetime))
            slot = &t; // keep looking for a free one, but an expired one will do
        if (!oldest || t.grant.created_ns < oldest->grant.created_ns)
            oldest = &t;
    }
    if (!slot)
        slot = oldest;
    char text[33];
    if (!make_activation_token_text(text))
        return nullptr;
    *slot = {};
    std::memcpy(slot->text, text, sizeof(text));
    std::memcpy(slot->grant.prefix, text, 8);
    slot->grant.prefix[8] = '\0';
    slot->grant.created_ns = now;
    slot->used = true;
    return slot;
}

lx::compositor::activation_token* lx::compositor::activation_tokens::find(const char* text) {
    if (!text || std::strlen(text) != 32)
        return nullptr;
    activation_token* hit = nullptr;
    for (auto& t : tokens_) {
        unsigned char diff = t.used ? 0 : 1;
        for (unsigned i = 0; i < 32; ++i)
            diff |= static_cast<unsigned char>(t.text[i] ^ text[i]);
        if (diff == 0)
            hit = &t;
    }
    return hit;
}

void lx::compositor::activation_tokens::erase(activation_token* t) {
    if (t >= tokens_ && t < tokens_ + k_max)
        *t = {};
}

void lx::compositor::activation_tokens::forget_surface(lx::surface_id s) {
    if (!s)
        return;
    for (auto& t : tokens_)
        if (t.used && t.surface == s)
            t.surface = {};
}

unsigned lx::compositor::activation_tokens::count() const {
    unsigned n = 0;
    for (const auto& t : tokens_)
        n += t.used ? 1u : 0u;
    return n;
}
