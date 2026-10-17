module;

// The persistent output configuration (ADR 008 C3, §C.5): `$XDG_CONFIG_HOME/lumen/outputs.toml`,
// written by the compositor after each successful configuration a client applied
// (wlr-randr, kanshi, a settings app) and read at start and at every hotplug, before the
// first modeset of the outputs it covers.
//
// Two levels, as KWin 6's `kwinoutputconfig.json`:
//   - per monitor (`[[output]]`, keyed by the identifier "make model serial", ADR 008 M1):
//     what belongs to the panel — its mode (or "preferred": follow the preferred mode, H3),
//     scale, transform and adaptive sync;
//   - per setup (`[[setup]]`, keyed by the sorted identifiers of the monitors connected
//     together): which of them are enabled and where each one is. A position means
//     something only relative to the other monitors present (Mutter's and KWin's lesson).
// A monitor whose connector has `hotplug_mode_update` (a VM's virtual monitor: the host
// owns its size and place) stores neither mode nor position (Mutter).
//
// kanshi stays the dynamic layer on top: it applies its profile through
// zwlr_output_manager_v1 after it starts; Lumen restored the last applied state, usually
// kanshi's own, so that apply finds nothing to change (C4). Precedence: a client's apply >
// the setup > the monitor > defaults (`--scale`, sway's HiDPI rule, preferred mode, auto
// position). `LUMEN_OUTPUT_CONFIG=0` turns reading and writing off; any other value is the
// file to use instead of the XDG one (tests, smokes).
//
// The model and its TOML mapping are pure (`test_output_config_store`); the file I/O is the
// `output_config_file` class: the read at start on UI (a few hundred bytes), the save
// serialised on UI and written on the worker (write, fsync, rename: never a half-written
// file), debounced 2 s so a burst of applies (kanshi) writes once. A file that fails to
// parse is logged once with its line and left alone until the next save, which renames it
// to `outputs.toml.bad` first. Member bodies are in output_config_store.cpp.

#include <atomic>
#include <cstddef>
#include <string>

import lx.foundation;

export module lx.compositor:output_config_store;

import :toml_lite;

export namespace lx::compositor {

/// What is remembered of one monitor.
struct monitor_config {
    static constexpr unsigned k_id = 208;
    char identifier[k_id]{};
    /// No `mode` key: the monitor's mode is not remembered (follow defaults).
    bool has_mode = false;
    /// `mode = "preferred"`: follow the connector's preferred mode (H3).
    bool mode_preferred = false;
    unsigned width = 0;
    unsigned height = 0;
    /// mHz; 0: any refresh of that size (the highest is taken).
    unsigned refresh_mhz = 0;
    bool has_scale = false;
    double scale = 1.0;
    bool has_transform = false;
    /// wl_output.transform (0..7).
    unsigned transform = 0;
    bool has_adaptive_sync = false;
    bool adaptive_sync = false;
};

/// One monitor's part of a setup.
struct setup_output_config {
    char identifier[monitor_config::k_id]{};
    bool enabled = true;
    /// No `position`: placed by the automatic layout.
    bool has_position = false;
    int x = 0;
    int y = 0;
};

/// One set of monitors connected together.
struct setup_config {
    static constexpr unsigned k_max_outputs = 16;
    setup_output_config outputs[k_max_outputs]{};
    unsigned count = 0;
};

/// The whole file. ~250 KB at capacity: allocated once (`output_config_file` holds it on the
/// heap), never on the stack.
struct output_config {
    static constexpr unsigned k_max_monitors = 64;
    static constexpr unsigned k_max_setups = 64;
    static constexpr int k_version = 1;
    /// Least recently used first: recording moves an entry to the end; when full, the first
    /// is dropped (a monitor not seen for a long time).
    monitor_config monitors[k_max_monitors]{};
    unsigned monitor_count = 0;
    setup_config setups[k_max_setups]{};
    unsigned setup_count = 0;
    /// Entries past the capacity that a parse dropped (warned once).
    unsigned dropped = 0;
};

/// "normal", "90", "180", "270", "flipped", "flipped-90", "flipped-180", "flipped-270" (sway's
/// and kanshi's spellings) ↔ wl_output.transform. -1 for anything else.
[[nodiscard]] int transform_from_name(const char* name);
[[nodiscard]] const char* transform_name(unsigned transform);

/// Parses `text` into `out` (replaced). False with the line and reason of the first error;
/// `out` then holds what was read before it, and must not be written back.
[[nodiscard]] bool parse_output_config(const char* text, std::size_t length, output_config& out,
                                       toml::parse_error& err);
/// The file's text.
[[nodiscard]] std::string serialize_output_config(const output_config& config);

/// The monitor entry for `identifier`, null when none.
[[nodiscard]] const monitor_config* find_monitor(const output_config& config, const char* identifier);
/// The setup whose monitors are exactly `ids` (`count` of them, any order), null when none.
[[nodiscard]] const setup_config* find_setup(const output_config& config, const char* const* ids,
                                             unsigned count);
/// Records `m` (replacing the entry of its identifier) as the most recently used. True when
/// the stored entry changed.
bool record_monitor(output_config& config, const monitor_config& m);
/// Records `s` (replacing the setup of the same monitors, order-insensitive) as the most
/// recently used. True when the stored setup changed.
bool record_setup(output_config& config, const setup_config& s);

/// Where the file is: `LUMEN_OUTPUT_CONFIG` (a path, or "0" for none), else
/// `$XDG_CONFIG_HOME/lumen/outputs.toml`, else `$HOME/.config/lumen/outputs.toml`. False when
/// persistence is off or no location can be derived.
[[nodiscard]] bool output_config_path(char* out, std::size_t size);

/// The file behind the model: load at start, debounced atomic saves on the worker. UI
/// affinity except the write task.
class output_config_file {
public:
    output_config_file();
    ~output_config_file();
    output_config_file(const output_config_file&) = delete;
    output_config_file& operator=(const output_config_file&) = delete;

    /// Resolves the path and reads the file (absent: empty). Logs a parse error once.
    /// False when persistence is off.
    bool load();
    [[nodiscard]] bool enabled() const { return path_[0] != '\0'; }
    [[nodiscard]] const char* path() const { return path_; }
    [[nodiscard]] output_config& config() { return *config_; }
    [[nodiscard]] const output_config& config() const { return *config_; }
    /// The file failed to parse at load: it is renamed to `.bad` by the next save.
    [[nodiscard]] bool bad_file() const { return bad_; }

    /// A save is wanted at `now_ns + k_debounce_ns` (a later call moves it).
    void schedule_save(unsigned long long now_ns);
    /// When the pending save is due (0: none).
    [[nodiscard]] unsigned long long save_due_ns() const { return save_due_ns_; }
    /// Runs a due save: serialises on the caller (UI) and hands the bytes to the worker
    /// (`post_worker`; null or refused: written here). A save still being written defers
    /// this one to the next call. Returns true when a write was started.
    bool run_due_save(unsigned long long now_ns, bool (*post_worker)(void (*fn)(void*), void* arg));
    /// Writes now, synchronously (exit with a save pending).
    void flush();
    /// Saves written (diagnostics, tests).
    [[nodiscard]] unsigned saves() const { return saves_.load(std::memory_order_acquire); }
    static constexpr unsigned long long k_debounce_ns = 2'000'000'000ull;

private:
    static void write_task(void* self);
    void write_now();

    char path_[1024]{};
    output_config* config_ = nullptr;
    bool bad_ = false;
    unsigned long long save_due_ns_ = 0;
    /// The text being written by the worker; owned here, untouched by UI while `writing_`.
    std::string pending_;
    std::atomic<bool> writing_{false};
    std::atomic<unsigned> saves_{0};
};

} // namespace lx::compositor
