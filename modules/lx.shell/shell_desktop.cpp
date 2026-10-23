module;

// lumen-shell's notifications and tray, wired into the shell (design-session-and-wm Part N):
// the user bus started after the first panel frame, the popups' output, clicks on them, the
// panel's tray cells and notification indicator, their menus and tooltips, and the state
// kept across a shell restart. The parts themselves live in :user_bus, :notifications,
// :notify_view, :menu_popup and :tray.

#include <stdint.h>
#include <time.h>
#include <unistd.h>

module lx.shell;

import std;
import lx.foundation;
import lx.trace;
import lx.config;

namespace {

constexpr const char* k_desktop_category = "shell.notify";
constexpr const char* k_tray_log = "shell.tray";
constexpr const char* k_notifications_name = "org.freedesktop.Notifications";
/// A tray item's tooltip appears after this much hovering (GNOME, KDE).
constexpr long long k_tooltip_delay_ns = 500'000'000LL;

/// `s` quoted for a desktop entry's Exec (the launcher expands field codes there).
std::string exec_quote(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '`' || c == '$' || c == '\\')
            out += '\\';
        if (c == '%')
            out += '%';
        out += c;
    }
    out += '"';
    return out;
}

/// A menu label shown as is (no mnemonic: '_' doubled).
std::string literal_label(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (c == '\n' || c == '\r') {
            out += ' ';
            continue;
        }
        out += c;
        if (c == '_')
            out += '_';
    }
    if (out.size() > 120) {
        out.resize(117);
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0u) == 0x80u)
            out.pop_back();
        if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0u)
            out.pop_back();
        out += "\xE2\x80\xA6";
    }
    return out;
}

/// The file an IconThemePath directory holds for `name` (direct, or a hicolor-like tree).
std::string find_in_theme_path(const std::string& dir, const std::string& name, int px) {
    if (dir.empty() || name.empty() || name.find('/') != std::string::npos)
        return {};
    std::error_code ec;
    for (const char* ext : {".png", ".svg"}) {
        const std::string direct = dir + "/" + name + ext;
        if (std::filesystem::is_regular_file(direct, ec))
            return direct;
    }
    // <dir>[/hicolor]/<N>x<N>/<context>/<name>.png, the nearest size at least px; scalable.
    std::string best;
    int best_size = 0;
    for (const char* theme : {"/hicolor", ""}) {
        const std::filesystem::path root = dir + theme;
        if (!std::filesystem::is_directory(root, ec))
            continue;
        unsigned seen = 0;
        for (const auto& size_dir : std::filesystem::directory_iterator(root, ec)) {
            if (++seen > 64)
                break;
            const std::string sz = size_dir.path().filename().string();
            int s = 0;
            if (sz == "scalable")
                s = 100000;
            else if (std::sscanf(sz.c_str(), "%dx%*d", &s) != 1)
                continue;
            for (const auto& ctx : std::filesystem::directory_iterator(size_dir.path(), ec)) {
                const std::string file =
                    ctx.path().string() + "/" + name + (s == 100000 ? ".svg" : ".png");
                if (!std::filesystem::is_regular_file(file, ec))
                    continue;
                const bool better = best.empty() || (s >= px && (best_size < px || s < best_size)) ||
                                    (best_size < px && s > best_size);
                if (better) {
                    best = file;
                    best_size = s;
                }
            }
        }
        if (!best.empty())
            break;
    }
    return best;
}

/// Percent-decoding of a file:// URI's path.
std::string uri_path(std::string_view uri) {
    std::string_view p = uri.substr(7); // after file://
    if (const std::size_t slash = p.find('/'); slash != 0 && slash != std::string_view::npos)
        p = p.substr(slash); // file://host/path
    std::string out;
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '%' && i + 2 < p.size() + 0 && std::isxdigit(static_cast<unsigned char>(p[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(p[i + 2]))) {
            out += static_cast<char>(std::stoi(std::string{p.substr(i + 1, 2)}, nullptr, 16));
            i += 2;
            continue;
        }
        out += p[i];
    }
    return out;
}

} // namespace

#if defined(LUMEN_SHELL_WAYLAND)

bool lx::shell::shell_app::notifications_owned() const {
    return desktop_started_ && note_server_.attached() && bus_.owns(k_notifications_name);
}

lx::shell::shell_app::output_record* lx::shell::shell_app::output_by_name(unsigned name) {
    for (auto& o : outputs_)
        if (o->name == name && o->done)
            return o.get();
    return nullptr;
}

void lx::shell::shell_app::save_state() {
    if (!state_path_.empty() && !save_shell_state(state_path_.c_str(), state_))
        lx::trace::logger::global().logf(lx::trace::level::debug, k_desktop_category,
                                         "cannot write {}", state_path_);
}

void lx::shell::shell_app::start_desktop_services() {
    if (desktop_started_)
        return;
    desktop_started_ = true;
    // What a previous shell left: the next notification id (ids are never handed out twice
    // in a session, S.7) and do-not-disturb.
    state_path_ = shell_state_path();
    state_ = load_shell_state(state_path_.c_str());
    notes_.set_next_id(state_.next_id);
    notes_.set_dnd(state_.dnd, now_ns());
    notes_.set_sink({.closed =
                         [](unsigned id, close_reason r, void* user) {
                             static_cast<shell_app*>(user)->note_server_.emit_closed(id, r);
                         },
                     .reserve =
                         [](unsigned next, void* user) {
                             auto& a = *static_cast<shell_app*>(user);
                             a.state_.next_id = next;
                             a.save_state();
                         },
                     .user = this});
    if (file_config_)
        apply_notify_config(*file_config_);
    else
        apply_notify_config(lx::config::default_config());
    note_view_.init({.compositor = compositor_,
                     .shm = shm_,
                     .layer_shell = layer_shell_,
                     .viewporter = viewporter_,
                     .fractional = fractional_},
                    [](const notification& n, int px, void* user) {
                        return static_cast<shell_app*>(user)->notification_icon(n, px);
                    },
                    this);
    const menu_globals mg{.compositor = compositor_,
                          .shm = shm_,
                          .wm_base = wm_base_,
                          .seat = seat_,
                          .viewporter = viewporter_,
                          .fractional = fractional_};
    menu_.init(mg,
               [](const char* name, int px, void* user) {
                   return static_cast<shell_app*>(user)->image_icon(name, px);
               },
               this);
    tooltip_.init(mg);
    if (!bus_.open())
        return;
    // Launches go over the same connection (one fd in the poll set).
    apps_.set_shared_bus(bus_.bus());
    bus_.add_lost_handler(
        [](void* user) {
            auto& a = *static_cast<shell_app*>(user);
            a.menu_.close();
            a.tooltip_.close();
            a.apps_.bus_lost();
            a.note_server_.detach();
            a.tray_.detach();
            for (auto& o : a.outputs_)
                if (o->bar)
                    o->bar->widgets_changed();
        },
        this);
    if (note_server_.attach(bus_.bus(), notes_))
        (void)bus_.request_name(k_notifications_name, nullptr, nullptr);
    (void)tray_.attach(bus_);
    for (auto& o : outputs_)
        if (o->bar)
            attach_widgets(*o);
    lx::trace::logger::global().logf(lx::trace::level::info, k_desktop_category,
                                     "notifications and tray on the user bus as {} (next id {}, "
                                     "do not disturb {})",
                                     bus_.unique_name(), notes_.next_id(),
                                     notes_.dnd_manual() ? "on" : "off");
}

void lx::shell::shell_app::stop_desktop_services() {
    if (!desktop_started_)
        return;
    menu_.close();
    tooltip_.close();
    note_view_.close();
    // Notifications still open are lost with the shell (no NotificationClosed: the spec
    // allows it; a restarted shell does not know them).
    note_server_.detach();
    tray_.detach();
    bus_.close();
    desktop_started_ = false;
}

void lx::shell::shell_app::apply_notify_config(const lx::config::config_snapshot& c) {
    const auto& n = c.notifications;
    notify_settings s{};
    s.timeout_ms = static_cast<unsigned>(n.timeout) * 1000u;
    s.timeout_low_ms = static_cast<unsigned>(n.timeout_low) * 1000u;
    s.critical_timeout_ms = static_cast<unsigned>(n.critical_timeout) * 1000u;
    s.max_visible = static_cast<unsigned>(n.max_visible);
    s.history = static_cast<unsigned>(n.history);
    s.dnd_while_sharing = n.dnd_while_sharing;
    notes_.configure(s, now_ns());
    notify_position p = notify_position::top_right;
    switch (n.position) {
    case lx::config::notification_position::top_right: p = notify_position::top_right; break;
    case lx::config::notification_position::top_center: p = notify_position::top_center; break;
    case lx::config::notification_position::top_left: p = notify_position::top_left; break;
    case lx::config::notification_position::bottom_right: p = notify_position::bottom_right; break;
    case lx::config::notification_position::bottom_left: p = notify_position::bottom_left; break;
    }
    note_view_.set_position(p);
}

lx::shell::shell_app::output_record* lx::shell::shell_app::notify_output() {
    if (output_record* o = output_by_name(notify_output_))
        return o;
    output_record* o = launcher_output();
    notify_output_ = o ? o->name : 0;
    return o;
}

void lx::shell::shell_app::attach_widgets(output_record& o) {
    if (!o.bar)
        return;
    o.bar->set_widgets(
        {.fill_tray =
             [](panel_tray_input& in, void* user) {
                 auto& a = *static_cast<shell_app*>(user);
                 in.count = 0;
                 for (unsigned i = 0; i < a.tray_.count() && in.count < k_max_tray_cells; ++i)
                     if (const tray_item* it = a.tray_.item(i); it && !it->hidden())
                         in.items[in.count++] = {it->key, it->rev};
             },
         .tray_icon = [](unsigned key, int px,
                         void* user) { return static_cast<shell_app*>(user)->tray_icon(key, px); },
         .tray_letter =
             [](unsigned key, void* user) {
                 const tray_item* it = static_cast<shell_app*>(user)->tray_.find(key);
                 const std::string& s = it ? (!it->title.empty() ? it->title : it->id) : std::string{};
                 return s.empty() ? '?' : s[0];
             },
         .bell =
             [](void* user) {
                 auto& a = *static_cast<shell_app*>(user);
                 return panel_bell{a.notifications_owned(), a.notes_.dnd(), a.notes_.unread()};
             },
         .user = this},
        [](const panel_widget_event& e, void* user) {
            auto& r = *static_cast<output_record*>(user);
            r.app->on_widget(e, r);
        },
        &o);
}

void lx::shell::shell_app::desktop_after_dispatch() {
    if (!desktop_started_)
        return;
    const long long now = now_ns();
    tray_.after_dispatch();
    // Do-not-disturb while the screen is captured (KDE's screen-sharing rule).
    notes_.set_capturing(bridge_.capture_active(), now);
    if (notes_.expire(now) || deadline_[t_notify] != notes_.next_deadline())
        set_timer(t_notify, notes_.next_deadline());
    // The popups on the active output, kept there while any shows.
    const notification* shown[1];
    if (notes_.shown(shown, 1) == 0)
        notify_output_ = 0;
    output_record* o = notes_.shown(shown, 1) ? notify_output() : nullptr;
    note_view_.update(notes_, o ? o->output : nullptr);
    if (!note_view_.is_open()) {
        // Gone under the pointer (its last card dismissed): no leave comes for a destroyed
        // surface, and a hover left set would stop every timer.
        notes_.set_hover(false, now);
        if (pointer_target_ == pointer_target::notify)
            pointer_target_ = pointer_target::none;
    }
    note_view_.after_dispatch();
    menu_.after_dispatch();
    tooltip_.after_dispatch();
    const bool dnd = notes_.dnd();
    if (dnd != dnd_seen_) {
        dnd_seen_ = dnd;
        lx::trace::logger::global().logf(lx::trace::level::info, k_desktop_category,
                                         "notifications: do not disturb {}{}", dnd ? "on" : "off",
                                         dnd && !notes_.dnd_manual() ? " (screen captured)" : "");
    }
    if (tray_.rev() != tray_rev_seen_ || notes_.rev() != notes_rev_seen_) {
        for (auto& out : outputs_)
            if (out->bar)
                out->bar->widgets_changed();
        // An item that went takes its menu and tooltip along.
        if (menu_kind_ == menu_kind::tray && !tray_.find(menu_key_))
            menu_.close();
        if (tooltip_key_ && !tray_.find(tooltip_key_)) {
            tooltip_.close();
            set_timer(t_tooltip, 0);
            tooltip_key_ = 0;
        }
        tray_rev_seen_ = tray_.rev();
        notes_rev_seen_ = notes_.rev();
    }
}

// ── Icons ────────────────────────────────────────────────────────────────────

const std::uint32_t* lx::shell::shell_app::image_icon(std::string_view what, int px) {
    if (what.empty() || px <= 0)
        return nullptr;
    std::string name{what};
    if (what.starts_with("file://"))
        name = uri_path(what);
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", name.c_str());
    // Icon names and PNG/SVG files: the icon theme (cached by name and size).
    if (const std::uint32_t* p = icons_.lookup(buf, px))
        return p;
    if (name.empty() || name[0] != '/')
        return nullptr;
    // Any other image file (JPEG, WebP — an avatar): decoded once per size, kept in a
    // small cache.
    for (auto& e : images_)
        if (e.px == px && e.what == name) {
            e.used = ++image_clock_;
            return e.pixels.empty() ? nullptr : e.pixels.data();
        }
    image_entry entry{};
    entry.what = name;
    entry.px = px;
    entry.used = ++image_clock_;
    std::vector<std::uint32_t> img;
    int w = 0, h = 0;
    if (decode_background(name, px, px, background_fit::fit, img, w, h) && w > 0 && h > 0)
        fit_square(img.data(), w, h, px, entry.pixels);
    if (images_.size() >= 16) {
        auto oldest = std::min_element(images_.begin(), images_.end(),
                                       [](const image_entry& a, const image_entry& b) {
                                           return a.used < b.used;
                                       });
        *oldest = std::move(entry);
        return oldest->pixels.empty() ? nullptr : oldest->pixels.data();
    }
    images_.push_back(std::move(entry));
    return images_.back().pixels.empty() ? nullptr : images_.back().pixels.data();
}

const std::uint32_t* lx::shell::shell_app::notification_icon(const notification& n, int px) {
    // Spec 1.2: image-data (drawn by the view) > image-path > app_icon; then the sender's
    // desktop entry's Icon=.
    if (!n.req.image_path.empty())
        if (const std::uint32_t* p = image_icon(n.req.image_path, px))
            return p;
    if (!n.req.app_icon.empty())
        if (const std::uint32_t* p = image_icon(n.req.app_icon, px))
            return p;
    if (!n.req.desktop_entry.empty()) {
        ensure_catalog();
        const desktop_entry* d = catalog_.find(n.req.desktop_entry);
        if (!d)
            d = catalog_.find(n.req.desktop_entry + ".desktop");
        if (d && !d->icon.empty())
            return image_icon(d->icon, px);
    }
    return nullptr;
}

const std::uint32_t* lx::shell::shell_app::tray_icon(unsigned key, int px) {
    const tray_item* it = tray_.find(key);
    if (!it)
        return nullptr;
    for (auto& c : tray_icons_)
        if (c.key == key && c.rev == it->rev && c.px == px)
            return c.pixels.empty() ? nullptr : c.pixels.data();
    tray_icon_entry* entry = nullptr;
    for (auto& c : tray_icons_)
        if (c.key == key)
            entry = &c;
    if (!entry) {
        // Entries of items that went are reused first.
        for (auto& c : tray_icons_)
            if (!tray_.find(c.key))
                entry = &c;
        if (!entry) {
            tray_icons_.push_back({});
            entry = &tray_icons_.back();
        }
    }
    entry->key = key;
    entry->rev = it->rev;
    entry->px = px;
    entry->pixels.clear();
    const bool attention = it->status == "NeedsAttention";
    const std::string& name =
        attention && !it->attention_icon_name.empty() ? it->attention_icon_name : it->icon_name;
    const std::vector<sni_pixmap>& pixmaps =
        attention && !it->attention.empty() ? it->attention : it->icon;
    const std::uint32_t* base = nullptr;
    if (!name.empty()) {
        // IconThemePath first (an application's own icons), then the theme.
        const std::string file = find_in_theme_path(it->theme_path, name, px);
        base = image_icon(file.empty() ? std::string_view{name} : std::string_view{file}, px);
    }
    if (base) {
        entry->pixels.assign(base, base + static_cast<std::ptrdiff_t>(px) * px);
    } else if (const sni_pixmap* p = choose_pixmap(pixmaps, px)) {
        pixmap_to_icon(*p, px, entry->pixels);
    } else {
        return nullptr;
    }
    // The overlay, composited over (premultiplied source-over).
    std::vector<std::uint32_t> overlay;
    if (!it->overlay_icon_name.empty()) {
        if (const std::uint32_t* o = image_icon(it->overlay_icon_name, px))
            overlay.assign(o, o + static_cast<std::ptrdiff_t>(px) * px);
    } else if (const sni_pixmap* p = choose_pixmap(it->overlay, px)) {
        pixmap_to_icon(*p, px, overlay);
    }
    for (std::size_t i = 0; i < overlay.size() && i < entry->pixels.size(); ++i) {
        const std::uint32_t s = overlay[i], d = entry->pixels[i];
        const unsigned ia = 255u - (s >> 24);
        std::uint32_t out = 0;
        for (unsigned sh = 0; sh < 32; sh += 8) {
            const unsigned v = ((s >> sh) & 0xFFu) + (((d >> sh) & 0xFFu) * ia + 127u) / 255u;
            out |= (v > 255u ? 255u : v) << sh;
        }
        entry->pixels[i] = out;
    }
    return entry->pixels.data();
}

// ── Notifications ────────────────────────────────────────────────────────────

void lx::shell::shell_app::post_internal(const char* summary, const char* body,
                                         notify_urgency urgency, unsigned& slot) {
    notify_request r{};
    r.app_name = "Lumen";
    r.app_icon = urgency == notify_urgency::critical ? "dialog-error" : "dialog-warning";
    r.summary = summary ? summary : "";
    // Shown as text: the markup characters escaped.
    for (const char* p = body ? body : ""; *p; ++p) {
        if (*p == '<')
            r.body += "&lt;";
        else if (*p == '>')
            r.body += "&gt;";
        else if (*p == '&')
            r.body += "&amp;";
        else
            r.body += *p;
    }
    r.urgency = urgency;
    r.replaces_id = slot;
    // Until dismissed: a configuration problem is worth reading.
    r.expire_timeout = 0;
    const unsigned replaced = slot;
    slot = notes_.notify(std::move(r), now_ns());
    lx::trace::logger::global().logf(lx::trace::level::info, k_desktop_category,
                                     "notify id={}{} app=\"Lumen\" (the shell's own) summary=\"{}\"",
                                     slot, replaced == slot ? " (replaced)" : "",
                                     summary ? summary : "");
}

void lx::shell::shell_app::on_notify_click(const notify_click& c, unsigned serial) {
    const long long now = now_ns();
    switch (c.what) {
    case notify_click::kind::none: return;
    case notify_click::kind::dismiss:
        (void)notes_.dismiss(c.id, now);
        return;
    case notify_click::kind::invoke: {
        const notification* n = notes_.find(c.id);
        if (!n || !notes_.begin_invoke(c.id, c.arg, now))
            return;
        // ActivationToken before ActionInvoked: the application may raise its window.
        pending_token t{};
        t.what = pending_token::purpose::notify_action;
        t.id = c.id;
        t.key = c.arg;
        const std::string app = n->req.desktop_entry;
        if (!request_token(std::move(t), serial, note_view_.surface(), app.c_str()))
            note_server_.action_invoked(c.id, c.arg.c_str(), nullptr, now);
        return;
    }
    case notify_click::kind::link: {
        if (!link_allowed(c.arg))
            return;
        // xdg-open in its own unit, as every launch; the link only on a click.
        const std::string exec = std::string{"xdg-open "} + exec_quote(c.arg);
        desktop_entry e{};
        e.id = "xdg-open";
        e.name = "a link";
        e.exec = exec;
        const char* home = std::getenv("HOME");
        auto p = std::make_unique<prepared_launch>();
        ensure_catalog();
        const launch_failure f = prepare_entry_launch(e, catalog_, home ? home : "/", *p);
        if (f != launch_failure::none) {
            report_failure("xdg-open", "a link", f, nullptr, 0, nullptr);
            return;
        }
        lx::trace::logger::global().logf(lx::trace::level::info, k_desktop_category,
                                         "notify id={} link opened", c.id);
        begin_launch(std::move(p), serial, note_view_.surface());
        return;
    }
    case notify_click::kind::more: {
        const notification* n = notes_.find(c.id);
        if (!n || !note_view_.layer())
            return;
        // The actions that did not fit, in a menu under the button.
        std::vector<menu_entry> items;
        menu_actions_.clear();
        int buttons[k_card_buttons];
        const unsigned nb = notify_button_actions(*n, buttons, k_card_buttons);
        for (unsigned i = 0; i < n->req.actions.size(); ++i) {
            const notify_action& a = n->req.actions[i];
            bool shown = a.key == "default";
            for (unsigned b = 0; b < nb; ++b)
                shown = shown || buttons[b] == static_cast<int>(i);
            if (shown)
                continue;
            menu_entry e{};
            e.label = literal_label(a.label);
            e.id = static_cast<int>(menu_actions_.size());
            menu_actions_.push_back({c.id, static_cast<int>(i)});
            items.push_back(std::move(e));
        }
        if (items.empty())
            return;
        tooltip_.close();
        menu_kind_ = menu_kind::actions;
        menu_key_ = c.id;
        menu_output_ = notify_output_;
        (void)menu_.open(
            note_view_.layer(), c.anchor, std::move(items), serial,
            [](int id, void* user) { static_cast<shell_app*>(user)->on_menu_activate(id); },
            [](void* user) { static_cast<shell_app*>(user)->on_menu_closed(); },
            nullptr, this, "notification-actions");
        return;
    }
    }
}

void lx::shell::shell_app::toggle_dnd(const char* why) {
    const bool on = !notes_.dnd_manual();
    notes_.set_dnd(on, now_ns());
    state_.dnd = on;
    save_state();
    lx::trace::logger::global().logf(lx::trace::level::info, k_desktop_category,
                                     "notifications: do not disturb {} ({})", on ? "set" : "cleared",
                                     why);
}

void lx::shell::shell_app::open_history(output_record& o, const panel_widget_event& e) {
    if (!o.bar || !o.bar->layer())
        return;
    std::vector<menu_entry> items;
    menu_actions_.clear();
    menu_entry dnd{};
    dnd.label = "_Do not disturb";
    dnd.id = -1;
    dnd.toggle_kind = menu_entry::toggle::check;
    dnd.toggled = notes_.dnd_manual();
    items.push_back(dnd);
    const notification* hist[64];
    const unsigned n = notes_.history(hist, 64);
    menu_entry clear{};
    clear.label = "_Clear all";
    clear.id = -2;
    clear.enabled = n > 0;
    items.push_back(clear);
    menu_entry sep{};
    sep.separator = true;
    items.push_back(sep);
    if (n == 0) {
        menu_entry none{};
        none.label = "No notifications";
        none.header = true;
        items.push_back(none);
    }
    const long long now_mono = now_ns();
    const std::time_t wall = std::time(nullptr);
    for (unsigned i = 0; i < n && i < 24; ++i) {
        const notification& note = *hist[i];
        // When it came: the wall clock now, less how long ago (monotonic).
        const std::time_t at = wall - static_cast<std::time_t>((now_mono - note.arrived_ns) / 1'000'000'000LL);
        std::tm tm{};
        localtime_r(&at, &tm);
        char when[8];
        std::snprintf(when, sizeof(when), "%02d:%02d", tm.tm_hour, tm.tm_min);
        std::string label = std::string{when} + "  ";
        if (!note.req.app_name.empty())
            label += note.req.app_name + ": ";
        label += note.req.summary.empty() ? markup_to_plain(note.req.body) : note.req.summary;
        menu_entry row{};
        row.label = literal_label(label);
        // Actions stay invokable while it is open and its sender is there.
        bool any_action = false;
        for (unsigned k = 0; k < note.req.actions.size(); ++k)
            any_action = any_action || notes_.can_invoke(note.id, note.req.actions[k].key);
        if (any_action) {
            row.submenu = true;
            for (unsigned k = 0; k < note.req.actions.size(); ++k) {
                const notify_action& a = note.req.actions[k];
                menu_entry act{};
                act.label = literal_label(a.key == "default" ? std::string{"Open"} : a.label);
                act.id = static_cast<int>(menu_actions_.size());
                menu_actions_.push_back({note.id, static_cast<int>(k)});
                row.children.push_back(std::move(act));
            }
            menu_entry rm{};
            rm.label = "Remove";
            rm.id = static_cast<int>(menu_actions_.size());
            menu_actions_.push_back({note.id, -2});
            row.children.push_back(std::move(rm));
            row.id = -3;
        } else {
            // A click removes it from the history.
            row.id = static_cast<int>(menu_actions_.size());
            menu_actions_.push_back({note.id, -2});
        }
        items.push_back(std::move(row));
    }
    tooltip_.close();
    set_timer(t_tooltip, 0);
    notes_.mark_read();
    o.bar->set_menu_keyboard(true);
    menu_kind_ = menu_kind::history;
    menu_key_ = 0;
    menu_output_ = o.name;
    if (!menu_.open(
            o.bar->layer(), e.cell, std::move(items), e.serial,
            [](int id, void* user) { static_cast<shell_app*>(user)->on_menu_activate(id); },
            [](void* user) { static_cast<shell_app*>(user)->on_menu_closed(); }, nullptr, this,
            "notification-history")) {
        o.bar->set_menu_keyboard(false);
        menu_kind_ = menu_kind::none;
    }
}

void lx::shell::shell_app::on_menu_activate(int id) {
    const long long now = now_ns();
    switch (menu_kind_) {
    case menu_kind::tray:
        tray_.menu_event(menu_key_, id, "clicked");
        return;
    case menu_kind::history:
    case menu_kind::actions: {
        if (id == -1) {
            toggle_dnd("the history menu");
            return;
        }
        if (id == -2) {
            notes_.clear_history(now);
            return;
        }
        if (id < 0 || static_cast<std::size_t>(id) >= menu_actions_.size())
            return;
        const auto [note, action] = menu_actions_[static_cast<std::size_t>(id)];
        if (action == -2) {
            (void)notes_.forget(note, now);
            return;
        }
        const notification* n = notes_.find(note);
        if (!n || action < 0 || static_cast<std::size_t>(action) >= n->req.actions.size())
            return;
        const std::string key = n->req.actions[static_cast<std::size_t>(action)].key;
        if (!notes_.begin_invoke(note, key, now))
            return;
        pending_token t{};
        t.what = pending_token::purpose::notify_action;
        t.id = note;
        t.key = key;
        const std::string app = n->req.desktop_entry;
        if (!request_token(std::move(t), last_press_serial_, nullptr, app.c_str()))
            note_server_.action_invoked(note, key.c_str(), nullptr, now);
        return;
    }
    case menu_kind::none: return;
    }
}

void lx::shell::shell_app::on_menu_closed() {
    if (menu_kind_ == menu_kind::tray) {
        tray_.menu_event(menu_key_, 0, "closed");
        tray_.set_menu_shown(0);
    }
    if (output_record* o = output_by_name(menu_output_); o && o->bar)
        o->bar->set_menu_keyboard(false);
    if (pointer_target_ == pointer_target::menu)
        pointer_target_ = pointer_target::none;
    menu_kind_ = menu_kind::none;
    menu_key_ = 0;
    menu_actions_.clear();
}

// ── The tray ─────────────────────────────────────────────────────────────────

void lx::shell::shell_app::open_tray_menu(unsigned key, std::vector<menu_entry> items) {
    if (menu_.is_open() && menu_kind_ == menu_kind::tray && menu_key_ == key) {
        // The layout changed while shown (LayoutUpdated).
        menu_.replace_items(std::move(items));
        return;
    }
    if (pending_menu_key_ != key)
        return; // asked for something else since
    pending_menu_key_ = 0;
    output_record* o = output_by_name(pending_menu_output_);
    if (!o || !o->bar || !o->bar->layer() || items.empty() || !tray_.find(key))
        return;
    o->bar->set_menu_keyboard(true);
    menu_kind_ = menu_kind::tray;
    menu_key_ = key;
    menu_output_ = o->name;
    if (!menu_.open(
            o->bar->layer(), pending_menu_cell_, std::move(items), pending_menu_serial_,
            [](int id, void* user) { static_cast<shell_app*>(user)->on_menu_activate(id); },
            [](void* user) { static_cast<shell_app*>(user)->on_menu_closed(); },
            [](int id, void* user) {
                auto& a = *static_cast<shell_app*>(user);
                a.tray_.menu_about_to_show(a.menu_key_, id);
                a.tray_.menu_event(a.menu_key_, id, "opened");
            },
            this, "tray")) {
        o->bar->set_menu_keyboard(false);
        menu_kind_ = menu_kind::none;
        return;
    }
    tray_.set_menu_shown(key);
    tray_.menu_event(key, 0, "opened");
}

void lx::shell::shell_app::on_widget(const panel_widget_event& e, output_record& o) {
    constexpr unsigned k_left = 0x110, k_right = 0x111, k_middle = 0x112;
    switch (e.what) {
    case panel_widget_event::kind::bell_click:
        if (e.button == k_middle)
            toggle_dnd("a middle click on the indicator"); // KDE's gesture
        else if (menu_.is_open() && menu_kind_ == menu_kind::history)
            menu_.close();
        else
            open_history(o, e);
        return;
    case panel_widget_event::kind::tray_hover:
        tooltip_.close();
        tooltip_key_ = e.key;
        tooltip_output_ = o.name;
        tooltip_cell_ = e.cell;
        set_timer(t_tooltip, e.key && !menu_.is_open() ? now_ns() + k_tooltip_delay_ns : 0);
        return;
    case panel_widget_event::kind::tray_scroll:
        // KDE's units: 120 per notch, positive away from the user (the panel's steps are
        // positive down).
        tray_.scroll(e.key, -e.delta * 120, false);
        return;
    case panel_widget_event::kind::tray_click: break;
    }
    const tray_item* it = tray_.find(e.key);
    if (!it)
        return;
    tooltip_.close();
    set_timer(t_tooltip, 0);
    // Screen coordinates for Activate / ContextMenu: the cell's centre in the layout.
    const int x = o.logical.x + e.cell.x + e.cell.width / 2;
    const int y = o.logical.y + e.cell.y + e.cell.height;
    const bool menu = (e.button == k_right || (e.button == k_left && it->item_is_menu)) &&
                      it->has_menu();
    if (menu) {
        pending_menu_key_ = e.key;
        pending_menu_output_ = o.name;
        pending_menu_cell_ = e.cell;
        pending_menu_serial_ = e.serial;
        if (!tray_.fetch_menu(
                e.key,
                [](unsigned key, std::vector<menu_entry> items, void* user) {
                    static_cast<shell_app*>(user)->open_tray_menu(key, std::move(items));
                },
                this))
            pending_menu_key_ = 0;
        return;
    }
    if (e.button == k_right) {
        tray_.context_menu(e.key, x, y);
        return;
    }
    if (e.button != k_left && e.button != k_middle)
        return;
    // A token first (ProvideXdgActivationToken), so the application may raise its window.
    pending_token t{};
    t.what = pending_token::purpose::tray_activate;
    t.id = e.key;
    t.x = x;
    t.y = y;
    t.secondary = e.button == k_middle;
    if (!request_token(std::move(t), e.serial, o.bar ? o.bar->surface() : nullptr, it->id.c_str()))
        tray_.activate(e.key, x, y, nullptr, e.button == k_middle);
}

void lx::shell::shell_app::show_tooltip() {
    const tray_item* it = tray_.find(tooltip_key_);
    output_record* o = output_by_name(tooltip_output_);
    if (!it || !o || !o->bar || !o->bar->layer() || menu_.is_open())
        return;
    const std::string& title = !it->tooltip_title.empty() ? it->tooltip_title
                               : !it->title.empty()       ? it->title
                                                          : it->id;
    if (tooltip_.open(o->bar->layer(), tooltip_cell_, title, it->tooltip_text) &&
        lx::trace::logger::global().min_level() <= lx::trace::level::debug)
        lx::trace::logger::global().logf(lx::trace::level::debug, k_tray_log,
                                         "tray: tooltip \"{}\"", title);
}

#else // !LUMEN_SHELL_WAYLAND

bool lx::shell::shell_app::notifications_owned() const { return false; }
void lx::shell::shell_app::start_desktop_services() {}
void lx::shell::shell_app::stop_desktop_services() {}
void lx::shell::shell_app::desktop_after_dispatch() {}
void lx::shell::shell_app::apply_notify_config(const lx::config::config_snapshot&) {}

#endif
