// lx.config — Lumen's configuration file (design-session-and-wm Part C): the TOML subset
// reader, the schema and its defaults, the key-combination grammar, the action names, the
// layered locations, the parser with its diagnostics, and the inotify watch. Runtime layer:
// pure data and file reading, no Wayland, no threads. The compositor reads and watches the
// file; the shell parses the same bytes (handed over zlm_shell_v1 v7); lumen-ctl checks a
// file offline. Partitions:
//
//   :toml         TOML subset reader/writer (also outputs.toml)
//   :diagnostics  what was wrong, where; "did you mean" distance
//   :keys         "Super+Shift+Return", buttons, taps; reserved combinations
//   :actions      the action namespace (owner, argument, held, lock-safe, available)
//   :schema       config_snapshot, defaults, key descriptors, default bindings
//   :parse        layers → snapshot + diagnostics; effective bytes; keymap check
//   :locate       XDG layers, LUMEN_CONFIG, bounded reads
//   :watch        inotify on the layers' directories

export module lx.config;

export import :toml;
export import :diagnostics;
export import :keys;
export import :actions;
export import :schema;
export import :parse;
export import :locate;
export import :watch;
