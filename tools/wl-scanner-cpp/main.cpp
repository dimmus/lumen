// wl-scanner-cpp — Wayland XML → C++26 module bindings + dispatch metadata

#include "scanner.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

struct Options {
    fs::path input;
    fs::path output_dir;
    std::string module_name;
};

static Options parse_args(int argc, char* argv[]) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc)
            o.input = argv[++i];
        else if (arg == "--output-dir" && i + 1 < argc)
            o.output_dir = argv[++i];
        else if (arg == "--module" && i + 1 < argc)
            o.module_name = argv[++i];
        else if (arg == "--help") {
            std::cout << "Usage: wl-scanner-cpp --input FILE --module NAME --output-dir DIR\n";
            std::exit(0);
        }
    }
    if (o.input.empty() || o.module_name.empty() || o.output_dir.empty()) {
        std::cerr << "Missing required arguments. Try --help\n";
        std::exit(1);
    }
    return o;
}

static std::string read_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot open: " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main(int argc, char* argv[]) {
    try {
        const auto opts = parse_args(argc, argv);
        const auto xml = read_file(opts.input);
        const auto proto = wlgen::parse_protocol_xml(xml);

        fs::create_directories(opts.output_dir);

        const fs::path cppm_path = opts.output_dir / (opts.module_name + ".cppm");
        const fs::path dispatch_path =
            opts.output_dir / (opts.module_name + ".dispatch.cppm");
        const fs::path cpp_path = opts.output_dir / (opts.module_name + ".gen.cpp");

        wlgen::generate_cppm(cppm_path, opts.module_name, proto);
        wlgen::generate_dispatch_cppm(dispatch_path, opts.module_name, proto);
        wlgen::generate_gen_cpp(cpp_path, opts.module_name, proto);

        std::cout << "Generated " << opts.module_name << " (" << proto.interfaces.size()
                  << " interfaces, dispatch metadata)\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "wl-scanner-cpp: " << ex.what() << '\n';
        return 1;
    }
}
