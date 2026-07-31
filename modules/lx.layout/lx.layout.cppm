module;

import lx.foundation;

export module lx.layout;

export namespace lx::layout {

enum class axis { horizontal, vertical };
enum class align { start, center, end, stretch, space_between, space_around };

struct constraints {
    lx::size2i min{};
    lx::size2i max{99999, 99999};
};

class layout_node {
public:
    virtual ~layout_node() = default;
    [[nodiscard]] virtual lx::size2i measure(constraints c) = 0;
    virtual void layout(lx::rect2i bounds) = 0;
    virtual void mark_dirty();
    [[nodiscard]] lx::rect2i bounds() const;

protected:
    lx::rect2i bounds_{};
    bool dirty_ = true;
};

class flex_node : public layout_node {
public:
    axis direction = axis::vertical;
    int spacing = 0;
    align main_align = align::start;
    align cross_align = align::stretch;

    void add(layout_node* child);
    lx::size2i measure(constraints c) override;
    void layout(lx::rect2i bounds) override;

private:
    layout_node* children_[64]{};
    unsigned child_count_ = 0;
};

class grid_node : public layout_node {
public:
    int columns = 1;
    int row_spacing = 0;
    int column_spacing = 0;

    void add(layout_node* child, int col_span = 1);
    lx::size2i measure(constraints c) override;
    void layout(lx::rect2i bounds) override;

private:
    layout_node* children_[64]{};
    unsigned spans_[64]{};
    unsigned child_count_ = 0;
};

class layout_engine {
public:
    void solve(layout_node& root, constraints c);
    void incremental_solve(layout_node& dirty_root, constraints c);
};

} // namespace lx::layout
