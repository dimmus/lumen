#include <cstdio>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: lumen-theme <input.theme> <output.ltheme>\n");
        return 1;
    }

    // P1: invoke lx::ui::theme_compiler via linked module
    std::fprintf(stdout, "lumen-theme: stub compile %s -> %s\n", argv[1], argv[2]);
    return 0;
}
