module;

import lx.foundation;

export module lx.ui:clipboard;

export namespace lx::ui {

/// Where a copy goes beyond the process (lx.app's Wayland clipboard). UI affinity.
using clipboard_sink = void (*)(void* user, const char* utf8);

/// The text a paste takes and a copy leaves. Without a sink it is the process's own;
/// with one (lx.app), a copy is also handed to the system's clipboard, and another
/// client's copy arrives through `set_system_text`. UI affinity.
class clipboard {
public:
    [[nodiscard]] static clipboard& global();

    /// A copy in this process. `utf8` must stay valid until the next set (the clipboard
    /// keeps the pointer); the sink is told.
    void set_text(const char* utf8);
    [[nodiscard]] const char* text() const;
    /// Another client's copy, read by the sink's owner: what `text` returns now. The sink
    /// is not told. `utf8` must stay valid until the next set.
    void set_system_text(const char* utf8);
    /// Null detaches it.
    void set_sink(clipboard_sink sink, void* user);

    void set_primary_selection(const char* utf8);
    [[nodiscard]] const char* primary_selection() const;

private:
    const char* text_ = "";
    const char* primary_ = "";
    clipboard_sink sink_ = nullptr;
    void* sink_user_ = nullptr;
};

class drag_drop_session {
public:
    void begin(const char* mime_type, const char* payload);
    void end();
    [[nodiscard]] bool active() const;

private:
    bool active_ = false;
    const char* mime_ = "";
    const char* payload_ = "";
};

} // namespace lx::ui


lx::ui::clipboard& lx::ui::clipboard::global() {
    static clipboard instance{};
    return instance;
}

void lx::ui::clipboard::set_text(const char* utf8) {
    text_ = utf8 ? utf8 : "";
    if (sink_)
        sink_(sink_user_, text_);
}
const char* lx::ui::clipboard::text() const { return text_; }
void lx::ui::clipboard::set_system_text(const char* utf8) { text_ = utf8 ? utf8 : ""; }
void lx::ui::clipboard::set_sink(clipboard_sink sink, void* user) {
    sink_ = sink;
    sink_user_ = sink ? user : nullptr;
}
void lx::ui::clipboard::set_primary_selection(const char* utf8) { primary_ = utf8 ? utf8 : ""; }
const char* lx::ui::clipboard::primary_selection() const { return primary_; }

void lx::ui::drag_drop_session::begin(const char* mime_type, const char* payload) {
    mime_ = mime_type;
    payload_ = payload;
    active_ = true;
}

void lx::ui::drag_drop_session::end() { active_ = false; }
bool lx::ui::drag_drop_session::active() const { return active_; }
