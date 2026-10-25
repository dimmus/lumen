// lumen-theme: compiles a theme family in Zed's JSON schema into Lumen's .lxtheme format
// (ADR-010 §4.8) through lx::ui::theme_compiler. The build runs it over the bundled
// assets/themes/one/one.json (One Dark, One Light); `--emit-cpp` regenerates
// modules/lx.ui/theme_builtin.cpp, the copies compiled into lx.ui:
//
//   ./build/release/lumen-theme --emit-cpp modules/lx.ui/theme_builtin.cpp \
//       assets/themes/one/one.json

#include <stdio.h>

import std;
import lx.foundation;
import lx.ui;

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: lumen-theme <theme.json> <out.lxtheme> [--theme NAME]\n"
                 "                   [--density compact|default|comfortable] [--quiet|--verbose]\n"
                 "       lumen-theme --emit-cpp <out.cpp> <theme.json> [--dark NAME] [--light NAME]\n"
                 "       lumen-theme --dump <in.lxtheme>\n");
}

[[nodiscard]] std::string hex(std::uint32_t rgba) {
    char b[16];
    std::snprintf(b, sizeof(b), "#%02x%02x%02x%02x", rgba & 0xFFu, (rgba >> 8) & 0xFFu,
                  (rgba >> 16) & 0xFFu, rgba >> 24);
    return b;
}

int dump(const char* path) {
    auto t = lx::ui::theme::load(path);
    if (!t) {
        std::fprintf(stderr, "lumen-theme: %s: %s\n", path, lx::format_error(t.get_error()));
        return 1;
    }
    std::printf("%s (%s), %zu bytes\n", t->name(),
                t->appearance_mode() == lx::ui::appearance::light ? "light" : "dark",
                t->bytes().size());
    for (unsigned i = 0; i < lx::ui::k_token_count; ++i) {
        const auto tok = static_cast<lx::ui::tok>(i);
        std::printf("  %-48.*s %s\n", static_cast<int>(lx::ui::token_key(tok).size()),
                    lx::ui::token_key(tok).data(), hex(t->rgba8(tok)).c_str());
    }
    return 0;
}

int emit_cpp(int argc, char** argv) {
    if (argc < 4) {
        usage();
        return 2;
    }
    const char* out = argv[2];
    const char* in = argv[3];
    const char* dark = "One Dark";
    const char* light = "One Light";
    for (int i = 4; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--dark") == 0)
            dark = argv[i + 1];
        else if (std::strcmp(argv[i], "--light") == 0)
            light = argv[i + 1];
    }
    std::string json;
    if (FILE* f = std::fopen(in, "rb")) {
        char buf[65536];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            json.append(buf, n);
        std::fclose(f);
    } else {
        std::fprintf(stderr, "lumen-theme: cannot read %s\n", in);
        return 1;
    }
    const lx::ui::theme_compiler compiler{};
    std::vector<std::uint8_t> dark_bytes;
    std::vector<std::uint8_t> light_bytes;
    auto d = compiler.compile_json(json, dark, dark_bytes);
    auto l = compiler.compile_json(json, light, light_bytes);
    if (!d || !l) {
        std::fprintf(stderr, "lumen-theme: %s: %s\n", in,
                     lx::format_error(!d ? d.get_error() : l.get_error()));
        return 1;
    }
    std::string origin = "assets/themes/one/one.json";
    if (const char* slash = std::strstr(in, "assets/"))
        origin = slash;
    origin += " (\"";
    origin += dark;
    origin += "\", \"";
    origin += light;
    origin += "\")";
    const std::string src = lx::ui::builtin_theme_source(dark_bytes, light_bytes, origin);
    FILE* f = std::fopen(out, "wb");
    if (!f || std::fwrite(src.data(), 1, src.size(), f) != src.size()) {
        std::fprintf(stderr, "lumen-theme: cannot write %s\n", out);
        if (f)
            std::fclose(f);
        return 1;
    }
    std::fclose(f);
    std::printf("lumen-theme: wrote %s (%zu + %zu bytes)\n", out, dark_bytes.size(),
                light_bytes.size());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--emit-cpp") == 0)
        return emit_cpp(argc, argv);
    if (argc == 3 && std::strcmp(argv[1], "--dump") == 0)
        return dump(argv[2]);
    if (argc < 3) {
        usage();
        return 2;
    }
    lx::ui::theme_source source{argv[1], argv[2], ""};
    lx::ui::compile_options options{};
    bool quiet = false;
    bool verbose = false;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
            source.theme_name = argv[++i];
        } else if (std::strcmp(argv[i], "--density") == 0 && i + 1 < argc) {
            const char* d = argv[++i];
            if (std::strcmp(d, "compact") == 0)
                options.default_density = lx::ui::density::compact;
            else if (std::strcmp(d, "comfortable") == 0)
                options.default_density = lx::ui::density::comfortable;
            else
                options.default_density = lx::ui::density::standard;
        } else if (std::strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else {
            usage();
            return 2;
        }
    }
    const lx::ui::theme_compiler compiler{options};
    auto r = compiler.compile(source);
    if (!r) {
        if (compiler.last_error_offset() != 0)
            std::fprintf(stderr, "lumen-theme: %s: %s (at byte %zu)\n", source.input_path,
                         lx::format_error(r.get_error()), compiler.last_error_offset());
        else
            std::fprintf(stderr, "lumen-theme: %s: %s\n", source.input_path,
                         lx::format_error(r.get_error()));
        return 1;
    }
    if (!quiet) {
        std::printf("lumen-theme: %s -> %s: \"%s\" (%s), %zu bytes; %u tokens from the file, "
                    "%u derived, %u from Zed's base; %u keys ignored, %u invalid colours\n",
                    source.input_path, source.output_path, r->name.c_str(),
                    r->mode == lx::ui::appearance::light ? "light" : "dark", r->bytes,
                    r->tokens_from_source, r->tokens_derived, r->tokens_from_base, r->ignored_keys,
                    r->invalid_colors);
        if (verbose) {
            for (const std::string& f : r->fallbacks)
                std::printf("  fallback %s\n", f.c_str());
            for (const std::string& k : r->ignored)
                std::printf("  ignored  %s\n", k.c_str());
        }
    }
    return 0;
}
