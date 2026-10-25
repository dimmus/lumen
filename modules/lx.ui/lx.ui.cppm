module;

import lx.foundation;
import lx.scene;
import lx.layout;
import lx.text;
import lx.runtime;
import lx.gfx;
import lx.wayland.client;
import lx.a11y;

export module lx.ui;

export import :events;
// ADR-010 §4.8: compiled themes (tokens, the .lxtheme format, the active theme), the packed
// style value and its builder, the theme compiler and Zed's base values it refines off.
export import :theme;
export import :theme_defaults;
export import :style;
export import :theme.compile;

// Declarative core: descriptors are reconciled onto retained nodes, decorators compose
// them, reducers own state. See docs/subsystems/ui-architecture.md.
export import :element;
export import :node;
export import :invalidate;
export import :reconcile;
export import :decorator;
// ADR-010 §4.7: flex containers over lx.layout:flex.
export import :flex;
export import :reducer;
export import :widgets_decl;
export import :root;
// ADR-010 Phase 2: a retained text layout drawn as atlas glyph sprites.
export import :text_node;
// ADR-010 §4.8, §5.5: styled flex boxes, Lucide icons through the atlas.
export import :styled;
export import :icon;

// Legacy retained-widget API, superseded by :node. Kept until callers migrate.
export import :widgets;
export import :widget_node;

export import :focus;
export import :clipboard;

export namespace lx::ui {
// Widget types exported via :widgets partition.
} // namespace lx::ui
