import lx.app;
import lx.foundation;
import lx.ui;
import lx.runtime;
import lx.scene;

namespace {

void on_hello_press() {}

lx::ui::child build_hello(lx::ui::build_context&) {
    return lx::ui::box(lx::color::rgb(0.12f, 0.12f, 0.16f),
                       lx::ui::button("Hello Lumen", &on_hello_press,
                                      lx::color::rgb(0.25f, 0.35f, 0.55f)));
}

} // namespace

int main(int argc, char* argv[]) {
    lx::application application{argc, argv};
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);

    lx::runtime::memory_arena arena{65536};

    lx::ui::ui_root root{arena};
    root.set_describer(build_hello);
    root.set_bounds({0, 0, 640, 480});

    lx::scene::draw_list draws;
    root.tick(draws);

    return application.run();
}
