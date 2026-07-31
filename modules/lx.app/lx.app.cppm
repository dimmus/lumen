module;

import lx.foundation;
import lx.runtime;
import lx.ui.builder;
import lx.wayland.client;

export module lx.app;

export namespace lx::app {

struct settings {
    const char* theme_path = nullptr;
    bool enable_vsync = true;
    float ui_scale = 1.f;
};

class clipboard {
public:
    void set_text(const char* text);
    [[nodiscard]] const char* text() const;
};

class application {
public:
    application(int argc, char* argv[]);
    ~application();

    [[nodiscard]] int run();
    void quit(int code = 0);

    [[nodiscard]] lx::runtime::event_loop& event_loop();
    [[nodiscard]] settings& config();
    [[nodiscard]] clipboard& clipboard_service();

    [[nodiscard]] ui::window_builder window();
    [[nodiscard]] lx::wayland::display& display();

private:
    lx::runtime::event_loop loop_{};
    settings settings_{};
    clipboard clipboard_{};
    lx::wayland::display display_{};
    int argc_ = 0;
    char** argv_ = nullptr;
};

} // namespace lx::app

// Convenience alias — must be exported for importers of lx.app.
export namespace lx {
using app::application;
}

module :private;

lx::app::application::application(int argc, char* argv[])
    : argc_{argc}, argv_{argv} {}
lx::app::application::~application() = default;
int lx::app::application::run() { return loop_.run(); }
void lx::app::application::quit(int code) { loop_.quit(code); }
lx::runtime::event_loop& lx::app::application::event_loop() { return loop_; }
lx::app::settings& lx::app::application::config() { return settings_; }
lx::app::clipboard& lx::app::application::clipboard_service() { return clipboard_; }
lx::ui::window_builder lx::app::application::window() { return lx::ui::make_window(); }
lx::wayland::display& lx::app::application::display() { return display_; }
void lx::app::clipboard::set_text(const char*) {}
const char* lx::app::clipboard::text() const { return ""; }
