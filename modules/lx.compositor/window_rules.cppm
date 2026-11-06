export module lx.compositor:window_rules;

import std;
import lx.foundation;

export namespace lx::compositor {

/// Exact, case-sensitive app-ID match. Absent fields leave the client's defaults alone.
struct window_rule {
    std::string app_id;
    std::optional<unsigned> workspace_index;
    std::optional<std::string> output_name;
    std::optional<lx::rect2i> geometry;
    /// Wire bits: 1=maximized, 2=fullscreen, 4=floating, 8=tiled.
    std::optional<unsigned> state_flags;
    std::optional<bool> server_side;
};

/// UI-affinity store shared by Varlink and the privileged Wayland interface. Edits persist
/// before becoming visible; failed reloads leave the previous generation intact.
class window_rule_store {
public:
    static constexpr unsigned k_capacity = 256;
    static constexpr std::size_t k_max_file_size = 1u << 20;
    [[nodiscard]] static lx::result<void> validate(const window_rule& rule);
    [[nodiscard]] std::span<const window_rule> rules() const { return rules_; }
    [[nodiscard]] const window_rule* find(std::string_view app_id) const;
    /// A full replacement for one app. An empty path is in-memory only (unit tests).
    [[nodiscard]] lx::result<void> set(window_rule rule);
    [[nodiscard]] lx::result<void> clear(std::string_view app_id);
    [[nodiscard]] lx::result<void> clear_all();
    [[nodiscard]] lx::result<void> load(std::string path);
    [[nodiscard]] lx::result<void> load_default();
    [[nodiscard]] lx::result<void> reload();
    [[nodiscard]] lx::result<void> parse(std::string_view text);
    [[nodiscard]] std::string serialize() const;
private:
    [[nodiscard]] lx::result<void> save() const;
    std::vector<window_rule> rules_;
    std::string path_;
};

} // namespace lx::compositor
