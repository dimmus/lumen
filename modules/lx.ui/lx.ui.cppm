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
export import :style;
export import :theme.compile;

// Declarative core: descriptors are reconciled onto retained nodes, decorators compose
// them, reducers own state. See docs/subsystems/ui-architecture.md.
export import :element;
export import :node;
export import :invalidate;
export import :reconcile;
export import :decorator;
export import :reducer;
export import :widgets_decl;
export import :root;

// Legacy retained-widget API, superseded by :node. Kept until callers migrate.
export import :widgets;
export import :widget_node;

export import :focus;
export import :clipboard;

export namespace lx::ui {
// Widget types exported via :widgets partition.
} // namespace lx::ui
