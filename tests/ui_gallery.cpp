// lumen-ui-gallery — every ADR-010 §5.6 component in its states, one image per theme (Phase 3's
// gate: the gallery reviewed against Zed side by side; docs/subsystems/ui-architecture.md §9
// holds the comparison).
//
//   lumen-ui-gallery [--theme one-dark|one-light] [--backend cpu|gl] [--out PATH]
//   lumen-ui-gallery --check [--dir DIR]
//
// One run draws the gallery at 1× under the compiled theme (`one-dark.lxtheme` /
// `one-light.lxtheme` from the build, else the built-in copy compiled from the same file)
// with the bundled fonts and Lucide icons, through `gfx::ui_cpu_renderer` or
// `gfx::ui_gl_renderer`, and writes it as a PNG. `--check` draws both themes on the CPU and
// compares them bit for bit with docs/images/ui-gallery-one-{dark,light}.png (`--dir`, else
// LUMEN_GALLERY_DIR), then on GL — when a hardware GLES 3.1 context exists — within G10's
// 3/255 of the CPU image; LUMEN_UI_GOLDEN_UPDATE=1 rewrites the two images instead (a
// reviewed change, as the golden references are).
//
// The gallery is panes of component trees, each laid out by lx.ui's flex engine at its rect
// and its open overlays laid out against that rect (a menu, a tooltip, toasts, a modal and
// its scrim), all drawn into one draw list. Hover, focus and pressed states are the node flags
// the pointer and focus manager set, set directly here; fades are at their end (reduced
// motion while it builds).

#include <fcntl.h>
#include <unistd.h>

#include "ui_golden.hpp"

import std;
import lx.foundation;
import lx.a11y;
import lx.gfx;
import lx.layout;
import lx.runtime;
import lx.scene;
import lx.text;
import lx.trace;
import lx.ui;

#ifndef LUMEN_GALLERY_DIR
#define LUMEN_GALLERY_DIR "docs/images"
#endif

namespace {

using namespace lx::ui;
using lx::layout::px;

constexpr int k_width = 1040;
constexpr int k_height = 1008;

/// The caches every pane draws with: the bundled fonts (ui_golden's text environment), one
/// atlas for glyphs and icons.
struct gallery_resources {
    std::unique_ptr<lx::gfx::atlas> atlas = std::make_unique<lx::gfx::atlas>();
    std::unique_ptr<lx::text::glyph_cache> glyphs = std::make_unique<lx::text::glyph_cache>(*atlas);
    std::unique_ptr<icon_cache> icons = std::make_unique<icon_cache>(*atlas);

    gallery_resources() {
        ui_resources r{};
        r.text = {ui_golden::text_fonts().engine(), glyphs.get(), nullptr};
        r.icons = icons.get();
        set_ui_resources(r);
    }
    ~gallery_resources() {
        set_ui_resources({});
        icons.reset();
        glyphs.reset();
    }
};

// ── Helpers ──

[[nodiscard]] ui_node* find_key(ui_node* n, element_key key) {
    if (!n)
        return nullptr;
    if (n->key() == key)
        return n;
    for (unsigned i = 0; i < n->child_count(); ++i)
        if (ui_node* hit = find_key(n->child_at(i), key))
            return hit;
    return nullptr;
}

[[nodiscard]] ui_node* find_type(ui_node* n, element_type_id type) {
    if (!n)
        return nullptr;
    if (n->type() == type)
        return n;
    for (unsigned i = 0; i < n->child_count(); ++i)
        if (ui_node* hit = find_type(n->child_at(i), type))
            return hit;
    return nullptr;
}

/// `c` in a box of style `s`.
[[nodiscard]] child boxed(const style& s, child c) { return div(s, {&c, 1u}); }

/// A pane: a titled surface box around `content`.
[[nodiscard]] child pane_box(const char* title, child content) {
    const child kids[2] = {label(title).size(label_size::small).color(tok::text_muted).single_line(),
                           content};
    return div(style{}.flex_col().size_full().gap(sp::b08).p(sp::b12)
                   .bg(tok::surface_background).border_1(tok::border_variant).rounded(radius::lg),
               kids);
}

/// One pane: `describe` laid out at `at`, its overlays against `at`, `prepare` run between
/// the layout and the draw (states), the tree and then its overlays emitted into `s`.
void draw_pane(draw_scope& s, lx::rect2i at, child (*describe)(),
               void (*prepare)(ui_node& root) = nullptr) {
    lx::runtime::memory_arena arena{512 * 1024};
    reconciler rec{};
    ui_node* root = nullptr;
    {
        build_context ctx{arena};
        build_scope scope{ctx};
        root = rec.apply(nullptr, describe());
        (void)rebuild_pending_reducers(ctx, rec);
    }
    if (!root)
        return;
    root->place(at);
    overlay_layer& layer = ui_overlays();
    layer.set_root(root);
    layer.set_focus(nullptr);
    layer.layout(at);
    if (prepare) {
        prepare(*root);
        // A state can open something (a list scrolled to its selection): lay out again.
        if (reducers_pending()) {
            build_context ctx{arena};
            build_scope scope{ctx};
            (void)rebuild_pending_reducers(ctx, rec);
        }
        root->place(at);
        layer.layout(at);
    }
    ui_damage().clear();
    root->emit(s);
    layer.emit(s);
    layer.set_root(nullptr);
    delete root;
}

// ── Pane 1: buttons and toggles in each state ──

constexpr const char* k_state_names[5] = {"Normal", "Hover", "Focused", "Disabled", "Selected"};
constexpr element_key k_state_row_key = 1000;

[[nodiscard]] child state_row(unsigned row) {
    const bool disabled = row == 3;
    const bool selected = row == 4;
    const child kids[] = {
        boxed(style{}.w(px(64.f)).flex_none(),
              label(k_state_names[row]).size(label_size::small).color(tok::text_muted).single_line()),
        button("Save").style(button_style::filled).disabled(disabled).selected(selected),
        button("Open").style(button_style::outlined).disabled(disabled).selected(selected),
        button("Ghost").icon(icon::check).disabled(disabled).selected(selected),
        button("Tinted").tint(tint_color::accent).disabled(disabled),
        icon_button(icon::settings).disabled(disabled).selected(selected),
        toggle_button("Grid").icon(icon::menu).selected(selected).disabled(disabled),
        checkbox(selected ? toggle_state::selected : row == 2 ? toggle_state::indeterminate
                                                              : toggle_state::unselected)
            .label("Wrap").disabled(disabled),
        toggle_switch(selected).label("Wi-Fi").disabled(disabled),
        tab("main.rs").selected(selected).disabled(disabled),
    };
    return div(style{}.flex_row().items_center().gap(sp::b08).h(px(30.f)), kids);
}

[[nodiscard]] child describe_states() {
    child rows[5];
    for (unsigned r = 0; r < 5; ++r) {
        const child content = state_row(r);
        rows[r] = with_children(describe<styled_node, styled_props>({style{}.flex_row()}, k_state_row_key + r),
                                {&content, 1u});
    }
    const child body = div(style{}.flex_col().gap(sp::b06), rows);
    return pane_box("Button (filled, outlined, ghost, tinted), icon button, toggle button, checkbox, switch, tab",
                    body);
}

void prepare_states(ui_node& root) {
    if (ui_node* hover = find_key(&root, k_state_row_key + 1))
        ui_golden::set_state(*hover, node_flag::interactive, node_flag::hovered);
    if (ui_node* focus = find_key(&root, k_state_row_key + 2))
        ui_golden::set_state(*focus, node_flag::focusable, node_flag::focused, node_flag::focus_ring);
}

// ── Pane 2: text, key bindings, avatars, dividers, progress ──

const char* const k_keys[2] = {"Ctrl-S", "Ctrl-Shift-P"};

[[nodiscard]] child describe_text() {
    const child heads[5] = {headline("XLarge").size(headline_size::xlarge), headline("Large").size(headline_size::large),
                            headline("Medium"), headline("Small").size(headline_size::small),
                            headline("XSmall").size(headline_size::xsmall)};
    const child labels1[5] = {label("Label"), label("Muted").color(tok::text_muted),
                              label("Accent").color(tok::text_accent), label("Placeholder").color(tok::text_placeholder),
                              label("Disabled").color(tok::text_disabled)};
    const child labels2[6] = {label("Error").color(tok::error), label("Warning").color(tok::warning),
                              label("Success").color(tok::success), label("Info").color(tok::info),
                              label("Bold").bold(), label("Italic").italic()};
    const child sizes[4] = {label("Large 16").size(label_size::large), label("Default 14"),
                            label("Small 12").size(label_size::small), label("XSmall 10").size(label_size::xsmall)};
    const child misc[] = {
        avatar("LM"), avatar("ZD").border(tok::border_focused), avatar(nullptr), vertical_divider(),
        key_binding(k_keys),
    };
    const child hint = key_binding_hint(std::span<const char* const>{k_keys, 1}, tok::surface_background)
                           .prefix("Save")
                           .suffix("to keep it");
    const child icons[] = {icon_view(icon::bell, 16.f, tok::icon), icon_view(icon::folder, 16.f, tok::icon_muted),
                           icon_view(icon::settings, 16.f, tok::icon_accent), icon_view(icon::circle_alert, 16.f, tok::error),
                           icon_view(icon::triangle_alert, 16.f, tok::warning), icon_view(icon::circle_check, 16.f, tok::success),
                           icon_view(icon::wifi, 14.f, tok::icon), icon_view(icon::battery, 12.f, tok::icon),
                           icon_view(icon::trash, 16.f, tok::icon_disabled)};
    const child meters[2] = {boxed(style{}.flex_1(), progress_bar(62.f, 100.f)),
                             boxed(style{}.flex_1(), progress_bar(120.f, 100.f))};
    const style row = style{}.flex_row().items_center().gap(sp::b08);
    const child kids[] = {
        div(style{row}.items_end(), heads),
        div(row, labels1),
        div(row, labels2),
        div(style{row}.items_end(), sizes),
        div(row, misc),
        hint,
        div(row, icons),
        divider(),
        div(row, meters),
    };
    return pane_box("Headline, label, icon, avatar, key binding, progress",
                    div(style{}.flex_col().gap(sp::b06), kids));
}

// ── Pane 3: tab bar, panel, status bar ──

[[nodiscard]] child describe_chrome() {
    static const char* const names[4] = {"main.rs", "lib.rs", "theme.rs", "ui_gallery.cpp"};
    child tabs[4];
    for (unsigned i = 0; i < 4; ++i)
        tabs[i] = tab(names[i]).position(i, 4, 1).end_slot(icon_view(icon::x, 12.f, tok::icon_muted));
    const child bar_end[2] = {icon_button(icon::plus).size(button_size::compact),
                              icon_button(icon::ellipsis).size(button_size::compact)};
    const child bar_start[1] = {icon_button(icon::arrow_left).size(button_size::compact)};
    const child tool_left[2] = {icon_view(icon::folder, 14.f, tok::icon_muted), label("Project").single_line()};
    const child tool_right[2] = {icon_button(icon::search).shape(icon_button_shape::square),
                                 icon_button(icon::ellipsis_vertical).shape(icon_button_shape::square)};
    const child body = label("A dock panel: a toolbar on toolbar.background over a body on panel.background.")
                           .color(tok::text_muted);
    const child status_left[3] = {button("main").icon(icon::folder).size(button_size::compact),
                                  icon_button(icon::triangle_alert).size(button_size::compact).icon_color(tok::warning),
                                  label("Ln 12, Col 4").color(tok::text_muted).size(label_size::small)};
    const child status_right[3] = {label("UTF-8").color(tok::text_muted).size(label_size::small),
                                   icon_button(icon::bell).size(button_size::compact),
                                   icon_button(icon::settings).size(button_size::compact)};
    const child kids[3] = {
        tab_bar(tabs).start(bar_start).end(bar_end),
        boxed(style{}.w_full().h(px(88.f)), panel(body).toolbar_start(tool_left).toolbar_end(tool_right)),
        status_bar(status_left, status_right),
    };
    return pane_box("Tab bar and tabs, panel with its toolbar, status bar",
                    div(style{}.flex_col().gap(sp::b08), kids));
}

// ── Pane 4: list, scroll area, text input ──

constexpr const char* k_files[] = {"main.rs", "lib.rs", "theme.rs", "overlay.rs", "scroll.rs", "list.rs",
                                   "input.rs", "popups.rs", "focus.rs", "root.rs", "node.rs", "style.rs",
                                   "flex.rs", "icon.rs", "text.rs", "gallery.rs"};
constexpr unsigned k_file_count = static_cast<unsigned>(std::size(k_files));
constexpr element_key k_list_key = 2000;

child file_row(void*, const list_row& r) {
    return list_item(k_files[r.index % k_file_count])
        .row(r)
        .start_slot(icon_view(icon::file, 14.f, tok::icon_muted))
        .end_slot(r.index % 5 == 2 ? icon_view(icon::circle_dot, 10.f, tok::modified) : nullptr);
}

[[nodiscard]] child describe_lists() {
    child paragraphs[10];
    for (unsigned i = 0; i < 10; ++i)
        paragraphs[i] = label(i % 2 ? "Scrolled content under a viewport; the Zed scrollbar shows while it overflows."
                                    : "A scroll area lays its content out once and draws it under the offset.")
                            .color(i % 3 ? tok::text : tok::text_muted);
    const child inputs[4] = {
        text_input().initial("Lumen text input").name("focused").layout(style{}.w(px(260.f))),
        text_input().placeholder("Search files").layout(style{}.w(px(260.f))),
        text_input().initial("Disabled field").disabled().layout(style{}.w(px(260.f))),
        checkbox(true).label("Match case"),
    };
    const child kids[3] = {
        list(k_file_count, file_row).layout(style{}.w(px(220.f)).h(px(132.f))).selected(2).key(k_list_key),
        scroll_area(style{}.w(px(360.f)).h(px(132.f)), paragraphs, scroll_axes::vertical,
                    style{}.flex_col().gap(sp::b04).pr(sp::b16)),
        div(style{}.flex_col().gap(sp::b08), inputs),
    };
    return pane_box("List and list items, scroll area, text input (selection, placeholder, disabled)",
                    div(style{}.flex_row().items_start().gap(sp::b16), kids));
}

void prepare_lists(ui_node& root) {
    if (ui_node* l = find_key(&root, k_list_key); l && l->type() == list_node::static_type()) {
        auto& list = *static_cast<list_node*>(l);
        if (ui_node* row = list.row_node(4))
            ui_golden::set_state(*row, node_flag::interactive, node_flag::hovered);
    }
    if (ui_node* s = find_type(&root, scroll_node::static_type()))
        static_cast<scroll_node*>(s)->scroll_to(1, 40.f);
    if (ui_node* n = ui_golden::find_named(&root, "focused"); n && n->type() == text_input_node::static_type()) {
        auto* in = static_cast<text_input_node*>(n);
        in->set_flag(node_flag::focused, true);
        in->set_selection(6, 10);
    }
}

// ── Panes 5–8: overlays ──

const char* const k_copy[1] = {"Ctrl-C"};
const char* const k_rename[1] = {"F2"};

[[nodiscard]] child describe_menu() {
    static menu_entry sub[2];
    static menu_entry entries[8];
    sub[0] = menu_item("Zoom in");
    sub[1] = menu_item("Zoom out");
    entries[0] = menu_header("Edit");
    entries[1] = menu_item("Copy").icon(icon::copy).keys(k_copy);
    entries[2] = menu_item("Rename").icon(icon::pencil).keys(k_rename);
    entries[3] = menu_item("Delete").icon(icon::trash).disabled();
    entries[4] = menu_separator();
    entries[5] = menu_submenu("View", sub);
    entries[6] = menu_item("Word wrap").toggle(true);
    entries[7] = menu_item("Soft tabs").toggle(false);
    const child trigger = button("Edit").style(button_style::filled).icon(icon::chevron_down, icon_position::end);
    const child kids[1] = {popover_menu(trigger, entries).open(true)};
    return pane_box("Popover menu", div(style{}.flex_row().items_start(), kids));
}

void prepare_menu(ui_node&) {
    overlay_node* o = ui_overlays().top();
    if (ui_node* rename = o ? ui_golden::find_named(o->content(), "Rename") : nullptr)
        rename->set_flag(node_flag::hovered, true);
}

[[nodiscard]] child describe_tooltip_pane() {
    const child kids[3] = {button("Save").style(button_style::filled).icon(icon::check)
                               .tooltip("Save the file", "Ctrl-S").name("save"),
                           icon_button(icon::settings), icon_button(icon::bell)};
    return pane_box("Tooltip (with its meta line)", div(style{}.flex_row().items_center().gap(sp::b08), kids));
}

void prepare_tooltip(ui_node& root) {
    ui_node* save = ui_golden::find_named(&root, "save");
    if (!save)
        return;
    save->set_flag(node_flag::hovered, true);
    if (overlay_node* tip = child_overlay(*save, overlay_kind::tooltip))
        (void)tip->open_at({save->bounds().x + 24, save->bounds().y + 14});
}

[[nodiscard]] child describe_toasts() {
    const child toasts[3] = {toast("Pushed 4 changes").status(status_kind::success).icon(icon::check).key(1),
                             toast("Disk almost full").status(status_kind::warning).icon(icon::triangle_alert).key(2),
                             toast("Build failed").status(status_kind::error).icon(icon::x).dismiss_button().key(3)};
    const child kids[2] = {label("Toasts stack at the bottom centre.").color(tok::text_muted), toast_stack(toasts)};
    return pane_box("Toast", div(style{}.flex_col().size_full(), kids));
}

[[nodiscard]] child describe_modal() {
    const child behind[2] = {label("Content behind the modal, under its scrim.").color(tok::text_muted),
                             button("Behind").style(button_style::outlined)};
    const child body = label("Discard the changes to three files?");
    const child kids[2] = {div(style{}.flex_col().gap(sp::b08), behind),
                           modal(body).title("Unsaved changes").open(true).width(216.f)
                               .footer(button("Cancel"), button("Discard").style(button_style::filled))};
    return pane_box("Modal", div(style{}.flex_col().size_full(), kids));
}

// ── The image ──

struct pane_spec {
    lx::rect2i at;
    child (*describe)();
    void (*prepare)(ui_node&);
};

constexpr pane_spec k_panes[] = {
    {{16, 16, 680, 262}, describe_states, prepare_states},
    {{712, 16, 312, 262}, describe_text, nullptr},
    {{16, 294, 1008, 222}, describe_chrome, nullptr},
    {{16, 532, 1008, 208}, describe_lists, prepare_lists},
    {{16, 756, 240, 236}, describe_menu, prepare_menu},
    {{272, 756, 240, 236}, describe_tooltip_pane, prepare_tooltip},
    {{528, 756, 240, 236}, describe_toasts, nullptr},
    {{784, 756, 240, 236}, describe_modal, nullptr},
};

[[nodiscard]] lx::color clear_color() { return active_theme().color(tok::background); }

void build_gallery(lx::scene::draw_list& out) {
    set_reduced_motion(true);
    out.begin({k_width, k_height});
    draw_scope s{out, {0, 0, k_width, k_height}};
    for (const pane_spec& p : k_panes)
        draw_pane(s, p.at, p.describe, p.prepare);
    out.batch_for_render();
    set_reduced_motion(false);
}

/// The theme `name` ("one-dark", "one-light"): the compiled file, else the built-in copy.
[[nodiscard]] bool use_theme(const char* name) {
    if (auto t = load_theme_file(name)) {
        set_active_theme(std::move(t).value());
        return true;
    }
    if (std::strcmp(name, "one-dark") == 0)
        set_active_builtin_theme(appearance::dark);
    else if (std::strcmp(name, "one-light") == 0)
        set_active_builtin_theme(appearance::light);
    else
        return false;
    std::printf("lumen-ui-gallery: %s.lxtheme not found, the built-in copy\n", name);
    return true;
}

[[nodiscard]] bool render_cpu(const lx::scene::draw_list& list, const lx::gfx::atlas& atlas,
                              std::vector<std::uint32_t>& out) {
    out.assign(static_cast<std::size_t>(k_width) * k_height, 0u);
    lx::gfx::pixel_surface s{};
    s.pixels = reinterpret_cast<unsigned char*>(out.data());
    s.width = k_width;
    s.height = k_height;
    s.stride = k_width * 4u;
    s.format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
    lx::gfx::damage_region full{};
    full.set_full();
    lx::gfx::ui_cpu_renderer r{};
    return static_cast<bool>(r.draw(s, ui_golden::stream_of(list), &atlas, full,
                                    ui_golden::premultiplied(clear_color())));
}

/// The render node's GLES 3.1 context (null: none usable, or a software renderer).
struct gl_context {
    int fd = -1;
    lx::gfx::egl_device device{};
    lx::gfx::ui_gl_renderer renderer{};
    ~gl_context() {
        if (fd >= 0)
            ::close(fd);
    }
};

[[nodiscard]] std::unique_ptr<gl_context> open_gl() {
    auto ctx = std::make_unique<gl_context>();
    ctx->fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (ctx->fd < 0)
        return nullptr;
    auto device = lx::gfx::egl_device::create(ctx->fd);
    if (!device)
        return nullptr;
    ctx->device = std::move(device).value();
    if (!ctx->renderer.initialize())
        return nullptr;
    return ctx;
}

[[nodiscard]] bool render_gl(gl_context& gl, const lx::scene::draw_list& list, lx::gfx::atlas& atlas,
                             std::vector<std::uint32_t>& out) {
    auto target = lx::gfx::ui_gl_target::create(k_width, k_height);
    if (!target)
        return false;
    lx::gfx::damage_region full{};
    full.set_full();
    if (!gl.renderer.draw(*target, ui_golden::stream_of(list), &atlas, full,
                          ui_golden::premultiplied(clear_color())))
        return false;
    out.assign(static_cast<std::size_t>(k_width) * k_height, 0u);
    return static_cast<bool>(target->read_back(out));
}

/// The gallery under `theme` on `backend` ("cpu", "gl").
[[nodiscard]] bool render(const char* theme, const char* backend, gl_context* gl,
                          std::vector<std::uint32_t>& out, unsigned* primitives = nullptr) {
    if (!use_theme(theme))
        return false;
    gallery_resources res{};
    auto list = std::make_unique<lx::scene::draw_list>();
    build_gallery(*list);
    if (primitives)
        *primitives = list->primitive_count();
    if (std::strcmp(backend, "gl") == 0)
        return gl && render_gl(*gl, *list, *res.atlas, out);
    return render_cpu(*list, *res.atlas, out);
}

[[nodiscard]] std::string image_path(const char* dir, const char* theme) {
    return std::string(dir) + "/ui-gallery-" + theme + ".png";
}

/// Largest channel difference between `a` and the RGB image `b`, and how many pixels pass
/// `tolerance`.
[[nodiscard]] std::pair<int, unsigned long long> compare(const std::vector<std::uint32_t>& a,
                                                         const ui_golden::rgb_image& b, int tolerance) {
    const ui_golden::rgb_image got = ui_golden::to_rgb(a.data(), k_width, k_height, k_width);
    if (b.width != k_width || b.height != k_height)
        return {256, ~0ull};
    int worst = 0;
    unsigned long long over = 0;
    for (std::size_t i = 0; i < got.rgb.size(); i += 3) {
        int px = 0;
        for (int c = 0; c < 3; ++c)
            px = std::max(px, std::abs(static_cast<int>(got.rgb[i + c]) - static_cast<int>(b.rgb[i + c])));
        worst = std::max(worst, px);
        if (px > tolerance)
            ++over;
    }
    return {worst, over};
}

int check(const char* dir) {
    const bool update = ui_golden::update_requested();
    auto gl = update ? nullptr : open_gl();
    if (gl && gl->renderer.caps().software)
        gl.reset();
    int failures = 0;
    for (const char* theme : {"one-dark", "one-light"}) {
        std::vector<std::uint32_t> cpu;
        unsigned primitives = 0;
        if (!render(theme, "cpu", nullptr, cpu, &primitives)) {
            std::printf("FAIL %s: the CPU renderer refused the gallery\n", theme);
            ++failures;
            continue;
        }
        const std::string path = image_path(dir, theme);
        if (update) {
            const bool ok = ui_golden::write_png(path, ui_golden::to_rgb(cpu.data(), k_width, k_height, k_width));
            std::printf("%s %s (%u primitives)\n", ok ? "wrote" : "FAIL writing", path.c_str(), primitives);
            failures += ok ? 0 : 1;
            continue;
        }
        ui_golden::rgb_image ref{};
        if (!ui_golden::read_png(path, ref)) {
            std::printf("FAIL %s: cannot read %s\n", theme, path.c_str());
            ++failures;
            continue;
        }
        const auto [cpu_err, cpu_over] = compare(cpu, ref, 0);
        std::printf("%-9s cpu: %u primitives, max channel error %d/255 vs %s\n", theme, primitives,
                    cpu_err, path.c_str());
        if (cpu_over != 0) {
            std::printf("FAIL %s: %llu pixels differ from %s\n", theme, cpu_over, path.c_str());
            ++failures;
        }
        if (gl) {
            std::vector<std::uint32_t> on_gl;
            if (!render(theme, "gl", gl.get(), on_gl)) {
                std::printf("FAIL %s: the GL renderer refused the gallery\n", theme);
                ++failures;
                continue;
            }
            const auto [gl_err, gl_over] = compare(on_gl, ui_golden::to_rgb(cpu.data(), k_width, k_height, k_width),
                                                   ui_golden::k_tolerance_vs_cpu);
            std::printf("%-9s gl:  max channel error %d/255 vs CPU (G10: <= %d)\n", theme, gl_err,
                        ui_golden::k_tolerance_vs_cpu);
            if (gl_over != 0) {
                std::printf("FAIL %s: %llu GL pixels past %d/255\n", theme, gl_over, ui_golden::k_tolerance_vs_cpu);
                ++failures;
            }
        } else {
            std::printf("%-9s gl:  skipped (no hardware GLES 3.1 context)\n", theme);
        }
    }
    return failures == 0 ? 0 : 1;
}

void usage() {
    std::fputs("usage: lumen-ui-gallery [--theme one-dark|one-light] [--backend cpu|gl] [--out PATH]\n"
               "       lumen-ui-gallery --check [--dir DIR]\n",
               stderr);
}

} // namespace

int main(int argc, char** argv) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::trace::logger::global().set_min_level(lx::trace::level::warn);
    const char* theme = "one-dark";
    const char* backend = "cpu";
    const char* out = nullptr;
    const char* dir = std::getenv("LUMEN_GALLERY_DIR");
    if (!dir || !*dir)
        dir = LUMEN_GALLERY_DIR;
    bool do_check = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (std::strcmp(a, "--check") == 0) {
            do_check = true;
        } else if (v && std::strcmp(a, "--theme") == 0) {
            theme = v;
            ++i;
        } else if (v && std::strcmp(a, "--backend") == 0) {
            backend = v;
            ++i;
        } else if (v && std::strcmp(a, "--out") == 0) {
            out = v;
            ++i;
        } else if (v && std::strcmp(a, "--dir") == 0) {
            dir = v;
            ++i;
        } else {
            usage();
            return 2;
        }
    }
    if (do_check)
        return check(dir);
    if (std::strcmp(backend, "cpu") != 0 && std::strcmp(backend, "gl") != 0) {
        usage();
        return 2;
    }
    std::unique_ptr<gl_context> gl;
    if (std::strcmp(backend, "gl") == 0) {
        gl = open_gl();
        if (!gl) {
            std::fprintf(stderr, "lumen-ui-gallery: no GLES 3.1 context on /dev/dri/renderD128\n");
            return 1;
        }
    }
    std::vector<std::uint32_t> px;
    unsigned primitives = 0;
    if (!render(theme, backend, gl.get(), px, &primitives)) {
        std::fprintf(stderr, "lumen-ui-gallery: cannot render the gallery (theme %s, backend %s)\n", theme,
                     backend);
        return 1;
    }
    const std::string path = out ? std::string(out) : image_path(".", theme);
    if (!ui_golden::write_png(path, ui_golden::to_rgb(px.data(), k_width, k_height, k_width))) {
        std::fprintf(stderr, "lumen-ui-gallery: cannot write %s\n", path.c_str());
        return 1;
    }
    std::printf("%s: %d x %d, %u primitives, theme %s, backend %s\n", path.c_str(), k_width, k_height,
                primitives, theme, backend);
    return 0;
}
