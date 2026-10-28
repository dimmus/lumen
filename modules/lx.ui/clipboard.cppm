module;

import lx.foundation;

export module lx.ui:clipboard;

export namespace lx::ui {

class clipboard {
public:
    [[nodiscard]] static clipboard& global();

    void set_text(const char* utf8);
    [[nodiscard]] const char* text() const;

    void set_primary_selection(const char* utf8);
    [[nodiscard]] const char* primary_selection() const;

private:
    const char* text_ = "";
    const char* primary_ = "";
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

void lx::ui::clipboard::set_text(const char* utf8) { text_ = utf8 ? utf8 : ""; }
const char* lx::ui::clipboard::text() const { return text_; }
void lx::ui::clipboard::set_primary_selection(const char* utf8) { primary_ = utf8 ? utf8 : ""; }
const char* lx::ui::clipboard::primary_selection() const { return primary_; }

void lx::ui::drag_drop_session::begin(const char* mime_type, const char* payload) {
    mime_ = mime_type;
    payload_ = payload;
    active_ = true;
}

void lx::ui::drag_drop_session::end() { active_ = false; }
bool lx::ui::drag_drop_session::active() const { return active_; }
