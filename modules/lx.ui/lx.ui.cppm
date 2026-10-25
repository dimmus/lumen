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
// style value and its builder, the theme compiler and the base theme it refines off.
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

// ADR-010 §7 Phase 3: focus over the retained tree, pointer state, handlers, timers and
// motion; controls and the first half of §5.6's components.
export import :focus;
export import :interact;
export import :control;
export import :components;
export import :clipboard;
// ADR-010 §5.6, the rest: overlays laid out against the window and drawn after the tree,
// scroll areas with Zed's scrollbar, virtualised lists, the text input and its IME interface,
// menus, popovers, modals and toasts.
export import :overlay;
export import :scroll;
export import :list;
export import :text_input;
export import :popups;

export namespace lx::ui {
// Widget types exported via :widgets partition.
} // namespace lx::ui
