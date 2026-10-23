module;

// Reading a configuration (design-session-and-wm C.1, C.6): the layers' bytes as one buffer,
// each layer parsed on its own over the defaults, key by key; a syntax error rejects the
// whole configuration (the caller keeps the previous one), a bad value rejects only that key
// (it keeps its previous value), an unknown key is a warning with a "did you mean".
//
// The effective bytes are what the compositor hands the shell (zlm_shell_v1 v7 `config`):
// layers concatenated, each preceded by a `# lumen-layer: PATH` line, so the shell parses
// exactly what the compositor parsed and line numbers map back to the files. Bodies in
// parse.cpp (Clang 23 module-partition workaround).

export module lx.config:parse;

import std;
import :diagnostics;
import :schema;

export namespace lx::config {

/// The line that starts each layer in the effective bytes.
inline constexpr std::string_view k_layer_marker = "# lumen-layer: ";
/// A layer larger than this is refused (the configuration with it).
inline constexpr std::size_t k_max_layer_bytes = 256u * 1024u;
inline constexpr unsigned k_max_layers = 8;

struct layer_info {
    char path[512]{};
    /// Byte range of the file's content in `config_text::bytes`.
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct config_text {
    std::string bytes{};
    std::inplace_vector<layer_info, k_max_layers> layers{};

    /// Appends a layer (`content` the file's bytes), its marker line first.
    [[nodiscard]] bool add_layer(std::string_view path, std::string_view content);
    [[nodiscard]] const char* path_of(unsigned layer) const {
        return layer < layers.size() ? layers[layer].path : nullptr;
    }
};

/// The layers of effective bytes (as `add_layer` wrote them). Text before the first marker
/// is a layer with an empty path.
[[nodiscard]] config_text split_layers(std::string_view bytes);

/// Parses every layer over the defaults into `out`. False on a syntax error (in any layer):
/// `out` is left as it was and `diag` says where. `previous` (the configuration in use, or
/// null at start-up) is what a key with a bad value keeps.
[[nodiscard]] bool parse_config(const config_text& text, config_snapshot& out, diagnostics& diag,
                                const config_snapshot* previous = nullptr);

/// Compiles the keyboard section's keymap with xkbcommon, as the compositor will: an error
/// in `diag` (with xkbcommon's own first complaint) when it does not compile. For
/// `lumen-ctl config check`; the compositor compiles it for real (lx.input). False without
/// xkbcommon (nothing checked).
bool check_keymap(const keyboard_settings& k, diagnostics& diag);

/// The value of `key` in `c` as the file would write it ("0.3", "\"us,ru\"", "adaptive").
void format_value(const key_desc& key, const config_snapshot& c, char* out, std::size_t cap);

} // namespace lx::config
