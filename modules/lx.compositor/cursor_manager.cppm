module;

import lx.foundation;
import lx.gfx;
import lx.input;
import lx.scene;

export module lx.compositor:cursor;

export namespace lx::compositor {

enum class cursor_kind {
    default_arrow,
    pointer,
    text,
    move,
    resize_ns,
    resize_ew,
    resize_nesw,
    resize_nwse,
    not_allowed,
    custom,
};

struct cursor_image {
    lx::cursor_id id{};
    lx::size2i hotspot{};
    lx::size2i size{};
    lx::texture_id texture{};
};

/// Compositor-owned cursor surface — drawn above client content each frame.
class cursor_manager {
public:
    void set_theme_path(const char* path);
    void set_cursor(cursor_kind kind);
    void set_custom(lx::cursor_id id, lx::texture_id texture, lx::point2i hotspot);

    void set_position(lx::point2i pos);
    [[nodiscard]] lx::point2i position() const;

    void attach_to_scene(lx::scene::scene_graph& scene);
    /// Tracks the seat's pointer. Takes the seat rather than a standalone pointer object
    /// because the seat is what owns the position now — it is the thing libinput updates.
    void update_from_seat(const lx::input::seat& seat);

    [[nodiscard]] cursor_kind active_kind() const;
    [[nodiscard]] cursor_image active_image() const;

private:
    const char* theme_path_ = nullptr;
    cursor_kind kind_ = cursor_kind::default_arrow;
    lx::point2i position_{};
    cursor_image custom_{};
};

} // namespace lx::compositor


void lx::compositor::cursor_manager::set_theme_path(const char* path) { theme_path_ = path; }
void lx::compositor::cursor_manager::set_cursor(cursor_kind kind) { kind_ = kind; }

void lx::compositor::cursor_manager::set_custom(lx::cursor_id id, lx::texture_id texture,
                                                lx::point2i hotspot) {
    custom_.id = id;
    custom_.texture = texture;
    custom_.hotspot = {hotspot.x, hotspot.y};
    kind_ = cursor_kind::custom;
}

void lx::compositor::cursor_manager::set_position(lx::point2i pos) { position_ = pos; }
lx::point2i lx::compositor::cursor_manager::position() const { return position_; }

void lx::compositor::cursor_manager::attach_to_scene(lx::scene::scene_graph&) {}
void lx::compositor::cursor_manager::update_from_seat(const lx::input::seat& seat) {
    position_ = {static_cast<int>(seat.pointer_x()), static_cast<int>(seat.pointer_y())};
}

lx::compositor::cursor_kind lx::compositor::cursor_manager::active_kind() const { return kind_; }

lx::compositor::cursor_image lx::compositor::cursor_manager::active_image() const {
    if (kind_ == cursor_kind::custom) return custom_;
    return {};
}
