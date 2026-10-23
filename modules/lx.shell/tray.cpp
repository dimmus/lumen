module;

// The tray's pure parts (`:tray`): registration strings, pixmaps, the watcher's registry and
// dbusmenu entries. See the partition.

module lx.shell;

import std;
import lx.foundation;
import :tray;
import :menu_popup;
import :notifications;

bool lx::shell::parse_sni_service(std::string_view registered, std::string_view sender,
                                  std::string& bus, std::string& path) {
    bus.clear();
    path.clear();
    if (registered.empty() || registered.size() > 512)
        return false;
    if (registered.front() == '/') {
        // libappindicator: an object path; the item is the caller's.
        if (sender.empty())
            return false;
        bus = std::string{sender};
        path = std::string{registered};
        return true;
    }
    const std::size_t slash = registered.find('/');
    if (slash == std::string_view::npos) {
        bus = std::string{registered};
        path = "/StatusNotifierItem";
    } else {
        bus = std::string{registered.substr(0, slash)};
        path = std::string{registered.substr(slash)};
    }
    if (bus.empty() || path.size() < 1)
        return false;
    // A bus name: a unique name (":1.42") or a well-known one with at least one dot.
    if (bus[0] != ':' && bus.find('.') == std::string::npos)
        return false;
    for (const char c : bus)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-' || c == ':'))
            return false;
    return true;
}

bool lx::shell::convert_sni_pixmap(int width, int height, std::span<const std::uint8_t> data,
                                   sni_pixmap& out) {
    out = {};
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024)
        return false;
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (data.size() < n * 4)
        return false;
    out.width = width;
    out.height = height;
    out.argb.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        // Network byte order: A, R, G, B. Straight alpha (the spec says nothing; every
        // implementation sends unpremultiplied).
        const unsigned a = data[i * 4], r = data[i * 4 + 1], g = data[i * 4 + 2],
                       b = data[i * 4 + 3];
        out.argb[i] = (a << 24) | (((r * a + 127u) / 255u) << 16) |
                      (((g * a + 127u) / 255u) << 8) | ((b * a + 127u) / 255u);
    }
    return true;
}

const lx::shell::sni_pixmap* lx::shell::choose_pixmap(const std::vector<sni_pixmap>& set, int px) {
    const sni_pixmap* best = nullptr;
    for (const sni_pixmap& p : set) {
        const int s = p.width > p.height ? p.width : p.height;
        if (!best) {
            best = &p;
            continue;
        }
        const int bs = best->width > best->height ? best->width : best->height;
        // Prefer the smallest that is at least px; else the largest.
        if ((s >= px && (bs < px || s < bs)) || (bs < px && s > bs))
            best = &p;
    }
    return best;
}

void lx::shell::pixmap_to_icon(const sni_pixmap& p, int px, std::vector<std::uint32_t>& out) {
    if (p.width <= 0 || p.height <= 0 || px <= 0) {
        out.assign(static_cast<std::size_t>(px > 0 ? px * px : 0), 0u);
        return;
    }
    const int edge = p.width > p.height ? p.width : p.height;
    if (edge == px) {
        fit_square(p.argb.data(), p.width, p.height, px, out);
        return;
    }
    // Into a square first (centred), then resampled: bilinear up, area down.
    std::vector<std::uint32_t> square;
    fit_square(p.argb.data(), p.width, p.height, edge, square);
    out.assign(static_cast<std::size_t>(px) * static_cast<std::size_t>(px), 0u);
    resample_icon(square.data(), edge, out.data(), px);
}

bool lx::shell::sni_registry::add_item(const std::string& service) {
    if (std::find(items_.begin(), items_.end(), service) != items_.end() ||
        items_.size() >= k_max_tray_items)
        return false;
    items_.push_back(service);
    return true;
}

bool lx::shell::sni_registry::remove_item(const std::string& service) {
    return std::erase(items_, service) > 0;
}

std::vector<std::string> lx::shell::sni_registry::remove_bus(std::string_view bus) {
    std::vector<std::string> gone;
    std::erase_if(items_, [&](const std::string& s) {
        const std::size_t slash = s.find('/');
        const std::string_view b = std::string_view{s}.substr(0, slash);
        if (b != bus)
            return false;
        gone.push_back(s);
        return true;
    });
    return gone;
}

bool lx::shell::sni_registry::add_host(const std::string& name) {
    if (std::find(hosts_.begin(), hosts_.end(), name) != hosts_.end() || hosts_.size() >= 16)
        return false;
    hosts_.push_back(name);
    return true;
}

bool lx::shell::sni_registry::remove_host(std::string_view name) {
    return std::erase_if(hosts_, [&](const std::string& h) { return h == name; }) > 0;
}

lx::shell::menu_entry lx::shell::dbusmenu_entry(int id, const dbusmenu_props& p) {
    menu_entry e{};
    e.id = id;
    e.label = p.label;
    e.enabled = p.enabled;
    e.separator = p.separator;
    e.icon_name = p.icon_name;
    e.shortcut = p.shortcut;
    e.submenu = p.submenu;
    if (p.toggle_type == "checkmark") {
        e.toggle_kind = menu_entry::toggle::check;
        e.toggled = p.toggle_state == 1;
    } else if (p.toggle_type == "radio") {
        e.toggle_kind = menu_entry::toggle::radio;
        e.toggled = p.toggle_state == 1;
    }
    return e;
}
