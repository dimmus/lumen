import std;
import lx.app;
import lx.trace;
import lx.ui;
import lumen.ui_demo;

namespace {

constexpr lx::trace::category log_demo{"ui_demo"};

void on_close(void* user, lx::app::window&) {
    static_cast<lx::application*>(user)->quit(0);
}

} // namespace

int main(int argc, char* argv[]) {
    lx::application app{argc, argv};
    for (int i = 1; i < argc; ++i) {
        const std::string_view option{argv[i]};
        if (option == "--help" || option == "-h") {
            std::println("usage: lumen-ui-demo [--theme NAME|PATH] [--socket NAME]");
            return 0;
        }
        if (option != "--theme" && option != "--socket") {
            log_demo.error("unknown argument '{}' (--help lists them)", option);
            return 2;
        }
        if (i + 1 == argc || std::string_view{argv[i + 1]}.starts_with("--") || !argv[i + 1][0]) {
            log_demo.error("{} requires a value", option);
            return 2;
        }
        if (option == "--theme")
            app.config().theme = argv[++i];
        else
            app.config().socket = argv[++i];
    }
    if (auto started = app.start(); !started) {
        log_demo.error(started.get_error());
        return 1;
    }
    log_demo.debug("theme appearance={}",
                   lx::ui::active_theme().appearance_mode() == lx::ui::appearance::dark ? "dark"
                                                                                        : "light");
    app.set_close_handler(&on_close, &app);

    lx::app::window_options options{};
    options.title = "Lumen UI Demo";
    options.app_id = "org.lumen.UIDemo";
    options.size = {980, 700};
    options.min_size = {340, 420};
    options.describe = &ui_demo::describe;
    lx::app::window window;
    if (auto opened = window.open(app, options); !opened) {
        log_demo.error(opened.get_error());
        return 1;
    }
    ui_demo::bind_root(&window.root());
    const int result = app.run();
    ui_demo::bind_root(nullptr);
    return result;
}
