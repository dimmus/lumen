module;

import lx.foundation;

export module lx.layout;

// ADR-010 §4.7: the flexbox engine lx.ui's containers use (`:flex`), the size constraints
// its measure contract passes down, and the alignment lx.ui's `align` decorator takes.
export import :flex;
export import :grid;

export namespace lx::layout {

enum class align { start, center, end, stretch, space_between, space_around };

struct constraints {
    lx::size2i min{};
    lx::size2i max{99999, 99999};
};

} // namespace lx::layout
