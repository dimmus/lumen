module;

// The notification model, the body markup and image-data decoding (`:notifications`). See
// the partition.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

module lx.shell;

import std;
import lx.foundation;
import lx.text;
import lx.trace;
import :notifications;

namespace {

constexpr lx::trace::category log_notify{"shell.notify"};
constexpr std::size_t k_max_markup = 8192;
constexpr unsigned k_max_spans = 64;
constexpr unsigned k_max_links = 16;

[[nodiscard]] char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

[[nodiscard]] bool starts_with_nocase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (lower(s[i]) != prefix[i])
            return false;
    return true;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/// Decodes the entity at `s[i]` ('&'): appends it and advances `i` past ';'. False (nothing
/// consumed) when it is not one: the '&' is then text.
bool decode_entity(std::string_view s, std::size_t& i, std::string& out) {
    const std::size_t semi = s.find(';', i + 1);
    if (semi == std::string_view::npos || semi - i > 10)
        return false;
    const std::string_view name = s.substr(i + 1, semi - i - 1);
    if (name == "amp")
        out += '&';
    else if (name == "lt")
        out += '<';
    else if (name == "gt")
        out += '>';
    else if (name == "quot")
        out += '"';
    else if (name == "apos")
        out += '\'';
    else if (name.size() >= 2 && name[0] == '#') {
        std::uint32_t cp = 0;
        const bool hex = name[1] == 'x' || name[1] == 'X';
        const std::string_view digits = name.substr(hex ? 2 : 1);
        if (digits.empty())
            return false;
        for (const char c : digits) {
            unsigned v = 0;
            if (c >= '0' && c <= '9')
                v = static_cast<unsigned>(c - '0');
            else if (hex && lower(c) >= 'a' && lower(c) <= 'f')
                v = static_cast<unsigned>(lower(c) - 'a' + 10);
            else
                return false;
            cp = cp * (hex ? 16u : 10u) + v;
            if (cp > 0x10FFFF)
                return false;
        }
        append_utf8(out, cp);
    } else {
        return false;
    }
    i = semi + 1;
    return true;
}

std::string decode_text(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '&' && decode_entity(s, i, out))
            continue;
        out += s[i++];
    }
    return out;
}

/// The value of attribute `name` in a tag's attribute text, entities decoded.
std::optional<std::string> attribute(std::string_view attrs, std::string_view name) {
    std::size_t i = 0;
    while (i < attrs.size()) {
        while (i < attrs.size() && (attrs[i] == ' ' || attrs[i] == '\t' || attrs[i] == '\n'))
            ++i;
        const std::size_t k = i;
        while (i < attrs.size() && attrs[i] != '=' && attrs[i] != ' ' && attrs[i] != '/')
            ++i;
        const std::string_view key = attrs.substr(k, i - k);
        if (key.empty()) {
            ++i;
            continue;
        }
        if (i >= attrs.size() || attrs[i] != '=')
            continue; // a bare attribute
        ++i;
        std::string_view value;
        if (i < attrs.size() && (attrs[i] == '"' || attrs[i] == '\'')) {
            const char q = attrs[i++];
            const std::size_t end = attrs.find(q, i);
            const std::size_t stop = end == std::string_view::npos ? attrs.size() : end;
            value = attrs.substr(i, stop - i);
            i = stop + 1;
        } else {
            const std::size_t v = i;
            while (i < attrs.size() && attrs[i] != ' ' && attrs[i] != '/')
                ++i;
            value = attrs.substr(v, i - v);
        }
        bool same = key.size() == name.size();
        for (std::size_t j = 0; same && j < key.size(); ++j)
            same = lower(key[j]) == name[j];
        if (same)
            return decode_text(value);
    }
    return std::nullopt;
}

[[nodiscard]] long long ms_ns(unsigned ms) { return static_cast<long long>(ms) * 1'000'000LL; }

} // namespace

// ── Markup ───────────────────────────────────────────────────────────────────

bool lx::shell::link_allowed(std::string_view url) {
    return starts_with_nocase(url, "http://") || starts_with_nocase(url, "https://") ||
           starts_with_nocase(url, "mailto:");
}

void lx::shell::parse_body_markup(std::string_view in, rich_text& out) {
    out.text.clear();
    out.spans.clear();
    out.links.clear();
    if (in.size() > k_max_markup)
        in = in.substr(0, k_max_markup);
    struct open_tag {
        char tag; // 'b', 'i', 'u', 'a'
        int link; // 'a': its link (-1: not a link)
    };
    std::vector<open_tag> stack;
    std::size_t seg = 0;
    // The style of the text appended since `seg`, flushed into a span when it changes.
    const auto style_of = [&](bool& b, bool& it, bool& u, int& link) {
        b = it = u = false;
        link = -1;
        for (const open_tag& t : stack) {
            b = b || t.tag == 'b';
            it = it || t.tag == 'i';
            u = u || t.tag == 'u';
            if (t.tag == 'a' && t.link >= 0)
                link = t.link;
        }
    };
    const auto flush = [&] {
        if (out.text.size() > seg) {
            bool b, it, u;
            int link;
            style_of(b, it, u, link);
            if ((b || it || u || link >= 0) && out.spans.size() < k_max_spans) {
                lx::text::text_span s{};
                s.begin = static_cast<std::uint32_t>(seg);
                s.end = static_cast<std::uint32_t>(out.text.size());
                s.weight = b ? lx::text::weight::bold : lx::text::weight::normal;
                s.style = it ? lx::text::style::italic : lx::text::style::normal;
                s.underline = u || link >= 0;
                s.tag = static_cast<std::uint16_t>(link >= 0 ? 1 + link : 0);
                out.spans.push_back(s);
            }
        }
        seg = out.text.size();
    };
    std::size_t i = 0;
    while (i < in.size()) {
        const char c = in[i];
        if (c == '&') {
            if (!decode_entity(in, i, out.text))
                out.text += in[i++];
            continue;
        }
        if (c != '<') {
            out.text += c;
            ++i;
            continue;
        }
        const std::size_t close = in.find('>', i + 1);
        if (close == std::string_view::npos) {
            out.text += in.substr(i);
            break;
        }
        std::string_view body = in.substr(i + 1, close - i - 1);
        const bool closing = !body.empty() && body.front() == '/';
        if (closing)
            body.remove_prefix(1);
        bool self_closing = false;
        if (!body.empty() && body.back() == '/') {
            self_closing = true;
            body.remove_suffix(1);
        }
        std::size_t n = 0;
        while (n < body.size() && ((body[n] >= 'a' && body[n] <= 'z') || (body[n] >= 'A' && body[n] <= 'Z')))
            ++n;
        std::string name;
        for (std::size_t k = 0; k < n; ++k)
            name += lower(body[k]);
        const std::string_view attrs = body.substr(n);
        const bool known = name == "b" || name == "i" || name == "u" || name == "a" ||
                           name == "img" || name == "br";
        if (!known || (n < body.size() && body[n] != ' ' && body[n] != '\t' && body[n] != '\n')) {
            // Not markup this server knows: shown as the text it is.
            out.text += in.substr(i, close - i + 1);
            i = close + 1;
            continue;
        }
        i = close + 1;
        if (name == "br") {
            out.text += '\n';
            continue;
        }
        if (name == "img") {
            // Inline images are not drawn (no body-images): their alt text stands in.
            if (!closing)
                if (auto alt = attribute(attrs, "alt"); alt && !alt->empty())
                    out.text += *alt;
            continue;
        }
        const char tag = name[0];
        if (closing) {
            // The innermost open tag of that kind closes, and any opened inside it.
            for (std::size_t k = stack.size(); k-- > 0;) {
                if (stack[k].tag != tag)
                    continue;
                flush();
                stack.resize(k);
                break;
            }
            continue; // a stray closing tag is dropped
        }
        if (self_closing)
            continue;
        flush();
        open_tag t{tag, -1};
        if (tag == 'a') {
            if (auto href = attribute(attrs, "href");
                href && link_allowed(*href) && out.links.size() < k_max_links) {
                t.link = static_cast<int>(out.links.size());
                out.links.push_back(*href);
            }
        }
        if (stack.size() < 16)
            stack.push_back(t);
    }
    flush(); // tags left open close here
}

std::string lx::shell::markup_to_plain(std::string_view markup) {
    rich_text t;
    parse_body_markup(markup, t);
    return std::move(t.text);
}

// ── Images ───────────────────────────────────────────────────────────────────

const char* lx::shell::image_data_error_text(image_data_error e) {
    switch (e) {
    case image_data_error::none: return "ok";
    case image_data_error::bad_size: return "bad width, height or rowstride";
    case image_data_error::bad_format: return "not 8-bit RGB or RGBA";
    case image_data_error::short_data: return "fewer bytes than the rows need";
    }
    return "?";
}

void lx::shell::fit_square(const std::uint32_t* src, int w, int h, int size,
                           std::vector<std::uint32_t>& out) {
    out.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), 0u);
    if (!src || w <= 0 || h <= 0 || size <= 0)
        return;
    // Box filter (area average) into the fitted rect, centred.
    const double s = static_cast<double>(size) / (w > h ? w : h);
    const int tw = std::max(1, static_cast<int>(std::lround(w * s)));
    const int th = std::max(1, static_cast<int>(std::lround(h * s)));
    const int ox = (size - tw) / 2, oy = (size - th) / 2;
    for (int y = 0; y < th; ++y) {
        const int sy0 = y * h / th, sy1 = std::max(sy0 + 1, (y + 1) * h / th);
        for (int x = 0; x < tw; ++x) {
            const int sx0 = x * w / tw, sx1 = std::max(sx0 + 1, (x + 1) * w / tw);
            unsigned long long acc[4] = {0, 0, 0, 0};
            unsigned long long n = 0;
            for (int yy = sy0; yy < sy1 && yy < h; ++yy)
                for (int xx = sx0; xx < sx1 && xx < w; ++xx) {
                    const std::uint32_t p = src[static_cast<std::size_t>(yy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(xx)];
                    acc[0] += p >> 24;
                    acc[1] += (p >> 16) & 0xFF;
                    acc[2] += (p >> 8) & 0xFF;
                    acc[3] += p & 0xFF;
                    ++n;
                }
            if (!n)
                continue;
            const auto avg = [&](int k) { return static_cast<std::uint32_t>((acc[k] + n / 2) / n); };
            out[static_cast<std::size_t>(oy + y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(ox + x)] =
                (avg(0) << 24) | (avg(1) << 16) | (avg(2) << 8) | avg(3);
        }
    }
}

lx::shell::image_data_error lx::shell::decode_image_data(int width, int height, int rowstride,
                                                         bool has_alpha, int bits_per_sample,
                                                         int channels,
                                                         std::span<const std::uint8_t> data,
                                                         int max_size, notify_image& out) {
    out = {};
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || rowstride <= 0)
        return image_data_error::bad_size;
    if (bits_per_sample != 8 || channels != (has_alpha ? 4 : 3))
        return image_data_error::bad_format;
    if (static_cast<long long>(rowstride) < static_cast<long long>(width) * channels)
        return image_data_error::bad_size;
    // The last row need not be padded to the stride.
    const std::size_t need = static_cast<std::size_t>(rowstride) * static_cast<std::size_t>(height - 1) +
                             static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
    if (data.size() < need)
        return image_data_error::short_data;
    std::vector<std::uint32_t> argb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = data.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(rowstride);
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* p = row + static_cast<std::size_t>(x) * static_cast<std::size_t>(channels);
            const unsigned a = has_alpha ? p[3] : 255u;
            const unsigned r = (p[0] * a + 127u) / 255u;
            const unsigned g = (p[1] * a + 127u) / 255u;
            const unsigned b = (p[2] * a + 127u) / 255u;
            argb[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] =
                (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    const int longest = width > height ? width : height;
    out.size = longest < max_size ? longest : (max_size > 0 ? max_size : longest);
    fit_square(argb.data(), width, height, out.size, out.pixels);
    return image_data_error::none;
}

// ── The model ────────────────────────────────────────────────────────────────

void lx::shell::notification_center::configure(const notify_settings& s, long long now) {
    const bool dnd_before = dnd();
    settings_ = s;
    if (settings_.max_visible == 0)
        settings_.max_visible = 1;
    // A smaller history drops its oldest now.
    unsigned n = 0;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        if ((*it)->in_history && ++n > settings_.history) {
            (*it)->in_history = false;
            close_entry(**it, close_reason::expired);
        }
    drop_unused();
    update_running(now);
    if (dnd_before != dnd())
        ++rev_;
    ++rev_;
}

void lx::shell::notification_center::set_next_id(unsigned next) {
    next_id_ = next ? next : 1;
    reserved_until_ = next_id_;
}

unsigned lx::shell::notification_center::allocate_id() {
    const unsigned id = next_id_++;
    if (next_id_ == 0)
        next_id_ = 1;
    if (id >= reserved_until_ || reserved_until_ - id > k_id_block) {
        reserved_until_ = id + k_id_block;
        if (reserved_until_ < id)
            reserved_until_ = 0xFFFFFFFFu;
        if (sink_.reserve)
            sink_.reserve(reserved_until_, sink_.user);
    }
    return id;
}

long long lx::shell::notification_center::timeout_for(const notify_request& r) const {
    if (r.expire_timeout == 0)
        return 0;
    if (r.expire_timeout > 0)
        return static_cast<long long>(r.expire_timeout) * 1'000'000LL;
    switch (r.urgency) {
    case notify_urgency::low: return ms_ns(settings_.timeout_low_ms);
    case notify_urgency::critical: return ms_ns(settings_.critical_timeout_ms);
    case notify_urgency::normal: break;
    }
    return ms_ns(settings_.timeout_ms);
}

lx::shell::notification* lx::shell::notification_center::get(unsigned id) {
    for (auto& e : entries_)
        if (e->id == id)
            return e.get();
    return nullptr;
}

const lx::shell::notification* lx::shell::notification_center::find(unsigned id) const {
    for (const auto& e : entries_)
        if (e->id == id)
            return e.get();
    return nullptr;
}

unsigned lx::shell::notification_center::open_count() const {
    unsigned n = 0;
    for (const auto& e : entries_)
        n += e->open ? 1u : 0u;
    return n;
}

void lx::shell::notification_center::close_entry(notification& n, close_reason reason) {
    if (!n.open)
        return;
    n.open = false;
    n.running = false;
    if (sink_.closed)
        sink_.closed(n.id, reason, sink_.user);
}

void lx::shell::notification_center::drop_unused() {
    std::erase_if(entries_, [](const std::unique_ptr<notification>& e) {
        return !e->open && !e->in_popup && !e->in_history && !e->invoking;
    });
}

void lx::shell::notification_center::add_to_history(notification& n, long long) {
    if (n.req.transient) {
        n.in_history = false;
        return;
    }
    if (!n.in_history) {
        n.in_history = true;
        ++unread_;
    }
    // Bounded: the oldest go (an open one closes as expired).
    unsigned count = 0;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        notification& e = **it;
        if (!e.in_history)
            continue;
        if (++count <= settings_.history)
            continue;
        e.in_history = false;
        if (!e.in_popup)
            close_entry(e, close_reason::expired);
        if (unread_ > settings_.history)
            unread_ = settings_.history;
    }
}

void lx::shell::notification_center::update_running(long long now) {
    unsigned shown_n = 0;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        notification& e = **it;
        bool run = false;
        if (e.in_popup && !e.invoking && !locked_) {
            run = shown_n < settings_.max_visible && !hover_ && e.timeout_ns > 0;
            ++shown_n;
        }
        if (run && !e.running) {
            e.deadline_ns = now + (e.remaining_ns > 0 ? e.remaining_ns : 0);
            e.running = true;
        } else if (!run && e.running) {
            e.remaining_ns = e.deadline_ns > now ? e.deadline_ns - now : 0;
            e.running = false;
        }
    }
}

unsigned lx::shell::notification_center::notify(notify_request req, long long now, bool* merged) {
    if (merged)
        *merged = false;
    // The rate limit: past `rate_limit` in one second, a sender's new notifications replace
    // its newest open one.
    if (!req.sender.empty() && req.replaces_id == 0 && settings_.rate_limit > 0) {
        rate* r = nullptr;
        for (auto& c : rates_)
            if (c.sender == req.sender)
                r = &c;
        if (!r) {
            r = &rates_[0];
            for (auto& c : rates_)
                if (c.used < r->used)
                    r = &c;
            *r = rate{};
            r->sender = req.sender;
        }
        r->used = ++rate_clock_;
        if (now - r->window_ns >= 1'000'000'000LL) {
            r->window_ns = now;
            r->count = 0;
        }
        if (++r->count > settings_.rate_limit) {
            for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
                if ((*it)->open && !(*it)->invoking && (*it)->req.sender == req.sender) {
                    req.replaces_id = (*it)->id;
                    if (merged)
                        *merged = true;
                    if (!r->warned) {
                        r->warned = true;
                        log_notify.warn(
                            "notify: {} sends more than {} notifications a second; the excess "
                            "replaces its newest",
                            req.sender, settings_.rate_limit);
                    }
                    break;
                }
        }
    }
    if (req.replaces_id) {
        if (notification* n = get(req.replaces_id); n && n->open && !n->invoking) {
            // In place: the same id and position, the content and the timer anew.
            n->req = std::move(req);
            n->req.replaces_id = 0;
            parse_body_markup(n->req.body, n->body);
            n->timeout_ns = timeout_for(n->req);
            n->remaining_ns = n->timeout_ns;
            n->running = false;
            n->sender_gone = false;
            ++n->rev;
            ++rev_;
            update_running(now);
            return n->id;
        }
    }
    // Bounded: past k_max_open the oldest open one closes.
    if (open_count() >= k_max_open)
        for (auto& e : entries_)
            if (e->open && !e->invoking) {
                e->in_popup = false;
                e->in_history = false;
                close_entry(*e, close_reason::expired);
                break;
            }
    drop_unused();
    auto n = std::make_unique<notification>();
    n->id = allocate_id();
    n->req = std::move(req);
    n->req.replaces_id = 0;
    parse_body_markup(n->req.body, n->body);
    n->arrived_ns = now;
    n->timeout_ns = timeout_for(n->req);
    n->remaining_ns = n->timeout_ns;
    n->rev = 1;
    const unsigned id = n->id;
    notification& ref = *n;
    entries_.push_back(std::move(n));
    if (locked_) {
        ref.in_popup = true;
        ref.queued_locked = true;
    } else if (dnd() && ref.req.urgency != notify_urgency::critical) {
        add_to_history(ref, now);
        if (!ref.in_history) // transient, or no history: nothing to keep it for
            close_entry(ref, close_reason::expired);
    } else {
        ref.in_popup = true;
    }
    drop_unused();
    ++rev_;
    update_running(now);
    return id;
}

bool lx::shell::notification_center::close(unsigned id, close_reason reason, long long now) {
    notification* n = get(id);
    if (!n || !n->open)
        return false;
    n->in_popup = false;
    n->in_history = false;
    n->invoking = false;
    close_entry(*n, reason);
    drop_unused();
    ++rev_;
    update_running(now);
    return true;
}

bool lx::shell::notification_center::dismiss_newest(long long now) {
    const notification* s[1];
    if (shown(s, 1) == 0)
        return false;
    return dismiss(s[0]->id, now);
}

unsigned lx::shell::notification_center::dismiss_all(long long now) {
    unsigned n = 0;
    for (auto& e : entries_)
        if (e->in_popup && e->open && !e->invoking) {
            e->in_popup = false;
            e->in_history = false;
            close_entry(*e, close_reason::dismissed);
            ++n;
        }
    drop_unused();
    ++rev_;
    update_running(now);
    return n;
}

void lx::shell::notification_center::clear_history(long long now) {
    for (auto& e : entries_)
        if (e->in_history) {
            e->in_history = false;
            if (!e->in_popup && !e->invoking)
                close_entry(*e, close_reason::dismissed);
        }
    unread_ = 0;
    drop_unused();
    ++rev_;
    update_running(now);
}

bool lx::shell::notification_center::forget(unsigned id, long long now) {
    notification* n = get(id);
    if (!n || !n->in_history)
        return false;
    n->in_history = false;
    if (!n->in_popup && !n->invoking)
        close_entry(*n, close_reason::dismissed);
    drop_unused();
    ++rev_;
    update_running(now);
    return true;
}

bool lx::shell::notification_center::can_invoke(unsigned id, std::string_view key) const {
    const notification* n = find(id);
    if (!n || !n->open || n->sender_gone || n->invoking)
        return false;
    for (const notify_action& a : n->req.actions)
        if (a.key == key)
            return true;
    return false;
}

bool lx::shell::notification_center::begin_invoke(unsigned id, std::string_view key, long long now) {
    if (!can_invoke(id, key))
        return false;
    notification* n = get(id);
    n->invoking = true;
    n->in_popup = false;
    n->running = false;
    ++rev_;
    update_running(now);
    return true;
}

void lx::shell::notification_center::finish_invoke(unsigned id, long long now) {
    notification* n = get(id);
    if (!n)
        return;
    n->invoking = false;
    if (n->open && n->req.resident) {
        // "Resident": kept until the user or the sender removes it.
        add_to_history(*n, now);
    } else {
        n->in_history = false;
        close_entry(*n, close_reason::dismissed);
    }
    drop_unused();
    ++rev_;
    update_running(now);
}

bool lx::shell::notification_center::expire(long long now) {
    bool changed = false;
    for (auto& e : entries_) {
        if (!e->running || e->deadline_ns > now)
            continue;
        e->running = false;
        e->remaining_ns = 0;
        e->in_popup = false;
        add_to_history(*e, now);
        if (!e->req.resident || !e->in_history)
            close_entry(*e, close_reason::expired);
        changed = true;
    }
    if (changed) {
        drop_unused();
        ++rev_;
        update_running(now);
    }
    return changed;
}

long long lx::shell::notification_center::next_deadline() const {
    long long d = 0;
    for (const auto& e : entries_)
        if (e->running && (!d || e->deadline_ns < d))
            d = e->deadline_ns;
    return d;
}

void lx::shell::notification_center::set_hover(bool on, long long now) {
    if (hover_ == on)
        return;
    hover_ = on;
    update_running(now);
}

void lx::shell::notification_center::set_locked(bool locked, long long now) {
    if (locked_ == locked)
        return;
    locked_ = locked;
    if (!locked) {
        // The newest max-visible of what came while locked pop up; older ones go to the
        // history, unseen (open).
        unsigned kept = 0;
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
            notification& e = **it;
            if (!e.queued_locked)
                continue;
            e.queued_locked = false;
            if (!e.in_popup)
                continue;
            if (kept < settings_.max_visible) {
                ++kept;
                continue;
            }
            e.in_popup = false;
            add_to_history(e, now);
            if (!e.in_history)
                close_entry(e, close_reason::expired);
        }
        drop_unused();
    }
    ++rev_;
    update_running(now);
}

void lx::shell::notification_center::set_dnd(bool on, long long) {
    if (dnd_ == on)
        return;
    dnd_ = on;
    ++rev_;
}

void lx::shell::notification_center::set_capturing(bool on, long long) {
    if (capturing_ == on)
        return;
    const bool before = dnd();
    capturing_ = on;
    if (before != dnd())
        ++rev_;
}

void lx::shell::notification_center::sender_gone(std::string_view sender) {
    if (sender.empty())
        return;
    bool any = false;
    for (auto& e : entries_)
        if (e->open && e->req.sender == sender && !e->sender_gone) {
            e->sender_gone = true;
            any = true;
        }
    if (any)
        ++rev_;
}

bool lx::shell::notification_center::sender_has_actions(std::string_view sender) const {
    for (const auto& e : entries_)
        if (e->open && !e->sender_gone && e->req.sender == sender && !e->req.actions.empty())
            return true;
    return false;
}

unsigned lx::shell::notification_center::shown(const notification** out, unsigned cap) const {
    if (locked_)
        return 0;
    unsigned n = 0;
    for (auto it = entries_.rbegin(); it != entries_.rend() && n < cap && n < settings_.max_visible;
         ++it)
        if ((*it)->in_popup && !(*it)->invoking)
            out[n++] = it->get();
    return n;
}

unsigned lx::shell::notification_center::waiting() const {
    if (locked_)
        return 0;
    unsigned n = 0;
    for (const auto& e : entries_)
        n += e->in_popup && !e->invoking ? 1u : 0u;
    return n > settings_.max_visible ? n - settings_.max_visible : 0u;
}

unsigned lx::shell::notification_center::history(const notification** out, unsigned cap) const {
    unsigned n = 0;
    for (auto it = entries_.rbegin(); it != entries_.rend() && n < cap; ++it)
        if ((*it)->in_history)
            out[n++] = it->get();
    return n;
}

bool lx::shell::notification_center::critical_shown() const {
    const notification* s[16];
    const unsigned n = shown(s, 16);
    for (unsigned i = 0; i < n; ++i)
        if (s[i]->req.urgency == notify_urgency::critical)
            return true;
    return false;
}

// ── Persisted state ──────────────────────────────────────────────────────────

std::string lx::shell::shell_state_path() {
    // LUMEN_SHELL_STATE: another file (tests), or 0 for none.
    if (const char* over = std::getenv("LUMEN_SHELL_STATE"); over && *over)
        return std::string_view{over} == "0" ? std::string{} : std::string{over};
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt)
        return {};
    return std::string{rt} + "/lumen/shell-state";
}

lx::shell::shell_state lx::shell::load_shell_state(const char* path) {
    shell_state s{};
    if (!path || !*path)
        return s;
    FILE* f = std::fopen(path, "re");
    if (!f)
        return s;
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        unsigned v = 0;
        if (std::sscanf(line, "next-id %u", &v) == 1 && v > 0)
            s.next_id = v;
        else if (std::sscanf(line, "dnd %u", &v) == 1)
            s.dnd = v != 0;
    }
    std::fclose(f);
    return s;
}

bool lx::shell::save_shell_state(const char* path, const shell_state& s) {
    if (!path || !*path)
        return false;
    std::string dir{path};
    if (const std::size_t slash = dir.rfind('/'); slash != std::string::npos) {
        dir.resize(slash);
        (void)::mkdir(dir.c_str(), 0700);
    }
    const std::string tmp = std::string{path} + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    char buf[64];
    const int n = std::snprintf(buf, sizeof(buf), "next-id %u\ndnd %u\n", s.next_id, s.dnd ? 1u : 0u);
    const bool ok = n > 0 && ::write(fd, buf, static_cast<std::size_t>(n)) == n;
    ::close(fd);
    if (!ok || std::rename(tmp.c_str(), path) != 0) {
        (void)::unlink(tmp.c_str());
        return false;
    }
    return true;
}
