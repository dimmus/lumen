module;

import lx.foundation;
import std;

export module lx.ui:clipboard;

export namespace lx::ui {

/// The clipboard (copy, cut, paste) or the primary selection (what was last selected,
/// pasted with the middle button).
enum class clipboard_kind : unsigned char { clipboard, primary };

/// Where a copy or a selection goes beyond the process (lx.app's Wayland data devices).
/// UI affinity.
using clipboard_sink = void (*)(void* user, clipboard_kind kind, const char* utf8);

/// The text a paste takes and a copy leaves, for both selections. Without a sink it is the
/// process's own; with one (lx.app), a copy or a selection is also handed to the system, and
/// another client's arrives through `set_system_text`. UI affinity.
class clipboard {
public:
    [[nodiscard]] static clipboard& global();

    /// A copy in this process. `utf8` must stay valid until the next set (the clipboard
    /// keeps the pointer); the sink is told.
    void set_text(const char* utf8);
    [[nodiscard]] const char* text() const;
    /// What a field selected, as `set_text` does for the primary selection.
    void set_primary_selection(const char* utf8);
    [[nodiscard]] const char* primary_selection() const;
    /// Another client's text for `kind`, read by the sink's owner: what `text` or
    /// `primary_selection` returns now. The sink is not told. `utf8` must stay valid until
    /// the next set.
    void set_system_text(const char* utf8, clipboard_kind kind = clipboard_kind::clipboard);
    /// Null detaches it.
    void set_sink(clipboard_sink sink, void* user);

private:
    const char* text_ = "";
    const char* primary_ = "";
    clipboard_sink sink_ = nullptr;
    void* sink_user_ = nullptr;
};

/// A node text can be dragged out of (lx.ui:text_input): told how its drag ended.
class drag_source {
public:
    /// `moved`: the destination took the text as a move — the source deletes it.
    virtual void on_drag_end(bool moved) = 0;

protected:
    ~drag_source() = default;
};

/// What a drag carries: text, a list of files (URIs), or both. `ask` offers the copy /
/// move / ask action set (the drop target then asks the user which to take); otherwise
/// copy and move. The strings must stay valid until the drag ends.
struct drag_payload {
    const char* text = "";
    std::span<const std::string_view> uris{};
    bool ask = false;
};

/// `uris` as a `text/uri-list` body: one `file://` line per URI, CRLF-terminated (RFC
/// 2483 §5). The buffer must stay valid until the drag ends.
[[nodiscard]] std::string format_uri_list(std::span<const std::string_view> uris);

/// The URIs in a `text/uri-list` body (comments `#…` skipped, CRLF or LF ends, `file:`
/// prefixes kept as they came).
[[nodiscard]] std::vector<std::string> parse_uri_list(std::string_view body);

/// Starts a drag (lx.app's data device); false when it cannot.
using drag_starter = bool (*)(void* user, const drag_payload& payload);

/// Drag and drop of text out of the UI: a node starts it through the starter lx.app sets,
/// and hears how it ended. One drag at a time. UI affinity.
class drag_service {
public:
    [[nodiscard]] static drag_service& global();

    /// Null detaches it.
    void set_starter(drag_starter starter, void* user);
    /// False without a starter, while another drag runs, or when the starter refused.
    /// The payload's strings must stay valid until the drag ends.
    bool start(const drag_payload& payload, drag_source& owner);
    /// A text-only drag (a `drag_payload` of just `text`).
    bool start_text(const char* utf8, drag_source& owner);
    /// The dropped offer negotiated `ask`: the drop target took the drop and the user's
    /// choice arrives here (lx.app forwards it to the offer). No drag running: ignored.
    using choose_fn = void (*)(void* user, std::uint32_t action);
    void set_chooser(choose_fn chooser, void* user);
    void choose_dnd_action(std::uint32_t action);
    [[nodiscard]] bool active() const { return active_; }
    /// The text being dragged ("" when none).
    [[nodiscard]] const char* text() const { return active_ ? text_ : ""; }
    /// The files being dragged (empty when none).
    [[nodiscard]] const std::vector<std::string>& uris() const { return uris_; }
    /// lx.app: the drag ended — dropped as a move, dropped as a copy, or cancelled.
    void end(bool moved);
    /// The owner is going away: it hears nothing more.
    void forget(drag_source& owner);

private:
    drag_starter starter_ = nullptr;
    void* starter_user_ = nullptr;
    choose_fn chooser_ = nullptr;
    void* chooser_user_ = nullptr;
    std::vector<std::string> uris_{};
    drag_source* owner_ = nullptr;
    const char* text_ = "";
    bool active_ = false;
};

} // namespace lx::ui


lx::ui::clipboard& lx::ui::clipboard::global() {
    static clipboard instance{};
    return instance;
}

void lx::ui::clipboard::set_text(const char* utf8) {
    text_ = utf8 ? utf8 : "";
    if (sink_)
        sink_(sink_user_, clipboard_kind::clipboard, text_);
}
const char* lx::ui::clipboard::text() const { return text_; }
void lx::ui::clipboard::set_primary_selection(const char* utf8) {
    primary_ = utf8 ? utf8 : "";
    if (sink_)
        sink_(sink_user_, clipboard_kind::primary, primary_);
}
const char* lx::ui::clipboard::primary_selection() const { return primary_; }
void lx::ui::clipboard::set_system_text(const char* utf8, clipboard_kind kind) {
    (kind == clipboard_kind::primary ? primary_ : text_) = utf8 ? utf8 : "";
}
void lx::ui::clipboard::set_sink(clipboard_sink sink, void* user) {
    sink_ = sink;
    sink_user_ = sink ? user : nullptr;
}

lx::ui::drag_service& lx::ui::drag_service::global() {
    static drag_service instance{};
    return instance;
}

void lx::ui::drag_service::set_starter(drag_starter starter, void* user) {
    starter_ = starter;
    starter_user_ = starter ? user : nullptr;
}

void lx::ui::drag_service::set_chooser(choose_fn chooser, void* user) {
    chooser_ = chooser;
    chooser_user_ = chooser ? user : nullptr;
}

void lx::ui::drag_service::choose_dnd_action(std::uint32_t action) {
    if (chooser_)
        chooser_(chooser_user_, action);
}

std::string lx::ui::format_uri_list(std::span<const std::string_view> uris) {
    // RFC 2483 §5: one URI per line, CRLF-terminated. A URI with no scheme is a local
    // path: it goes out as file:// (an absolute path only).
    std::string out;
    for (const std::string_view u : uris) {
        if (u.find(':') != std::string_view::npos || u.empty() || u[0] != '/')
            out += u;
        else
            out += "file://" + std::string{u};
        out += "\r\n";
    }
    return out;
}

std::vector<std::string> lx::ui::parse_uri_list(std::string_view body) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < body.size()) {
        const std::size_t end = std::min(body.find('\n', at), body.size());
        std::string_view line = body.substr(at, end - at);
        if (line.ends_with("\r"))
            line.remove_suffix(1);
        at = end + 1;
        if (line.empty() || line[0] == '#')
            continue;
        out.emplace_back(line);
    }
    return out;
}

bool lx::ui::drag_service::start(const drag_payload& payload, drag_source& owner) {
    if (!starter_ || active_)
        return false;
    // Active before the starter runs: it may hear of the end at once (a refused serial).
    active_ = true;
    owner_ = &owner;
    text_ = payload.text ? payload.text : "";
    uris_.assign(payload.uris.begin(), payload.uris.end());
    if (!starter_(starter_user_, payload)) {
        active_ = false;
        owner_ = nullptr;
        text_ = "";
        uris_.clear();
        return false;
    }
    return true;
}

bool lx::ui::drag_service::start_text(const char* utf8, drag_source& owner) {
    if (!starter_ || active_ || !utf8 || !*utf8)
        return false;
    // Active before the starter runs: it may hear of the end at once (a refused serial).
    active_ = true;
    owner_ = &owner;
    text_ = utf8;
    uris_.clear();
    const drag_payload payload{.text = utf8};
    if (!starter_(starter_user_, payload)) {
        active_ = false;
        owner_ = nullptr;
        text_ = "";
        return false;
    }
    return true;
}

void lx::ui::drag_service::end(bool moved) {
    if (!active_)
        return;
    drag_source* owner = owner_;
    active_ = false;
    owner_ = nullptr;
    text_ = "";
    if (owner)
        owner->on_drag_end(moved);
}

void lx::ui::drag_service::forget(drag_source& owner) {
    if (owner_ == &owner)
        owner_ = nullptr;
}
