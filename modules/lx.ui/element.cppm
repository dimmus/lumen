module;

import lx.foundation;
import lx.runtime;
import lx.trace;

export module lx.ui:element;

import std;

namespace {

constexpr lx::trace::category log_ui{"ui"};

} // namespace

export namespace lx::ui {

class ui_node;

/// Stable per-widget-type identity used to decide whether a retained node can be
/// reused for a new descriptor. Each node type returns its own value from `type()`.
using element_type_id = unsigned;

/// Hands out a fresh type id. Node types call this once from a function-local static:
///
///     static element_type_id static_type() {
///         static const element_type_id id = next_element_type_id();
///         return id;
///     }
///
/// Ids must never collide: reconciliation takes `apply` from the descriptor, so two
/// types sharing an id would let one node be cast to the other.
[[nodiscard]] element_type_id next_element_type_id();

/// Caller-supplied identity that keeps a retained node paired with the same logical
/// item across reorders (list rows, tabs). Zero means "match by sibling position".
using element_key = unsigned long long;

/// Descriptors are plain data so they can live in the UI frame arena with no
/// destructor pass: `construct` mints the retained node, `apply` copies props onto it.
struct element {
    element_type_id type = 0;
    element_key key = 0;
    const void* props = nullptr;
    ui_node* (*construct)() = nullptr;
    void (*apply)(ui_node& target, const void* props) = nullptr;
    const element* const* children = nullptr;
    unsigned child_count = 0;
};

/// A descriptor reference. Build functions return these; decorators compose them.
using child = const element*;

/// Deferred subtree. Overlays and collapsed panels store a thunk so their contents
/// are only described when actually shown.
using slot = child (*)(void* context);

/// Per-rebuild allocation scope. All descriptor storage comes from the UI frame arena
/// and dies at the next `reset_frame_arenas(memory_pool::ui)`; retained nodes do not.
class build_context {
public:
    explicit build_context(lx::runtime::memory_arena& arena);

    /// Allocate `T` in the frame arena. Returns nullptr once the arena is exhausted;
    /// callers propagate the null rather than growing without bound.
    template<typename T>
    [[nodiscard]] T* alloc() {
        void* raw = arena_ ? arena_->allocate(static_cast<unsigned>(sizeof(T)),
                                              static_cast<unsigned>(alignof(T)))
                           : nullptr;
        if (!raw) {
            note_overflow();
            return nullptr;
        }
        return new (raw) T{};
    }

    [[nodiscard]] const element** alloc_children(unsigned count);
    /// `count` bytes of the frame arena (text a description formats: a list row's label).
    /// Null once the arena is exhausted.
    [[nodiscard]] char* alloc_chars(unsigned count);

    /// True when any allocation failed this frame; the caller should keep the previous
    /// retained tree instead of reconciling against a truncated description.
    [[nodiscard]] bool overflowed() const;
    void reset_overflow();
    /// Bytes of the frame arena in use (0: nothing has described into it this frame).
    [[nodiscard]] unsigned used_bytes() const;

    /// UI-thread current context, valid only for the duration of one rebuild. Lets
    /// `operator|` allocate wrapper descriptors without threading a parameter through
    /// every decorator signature.
    [[nodiscard]] static build_context* current();
    static void set_current(build_context* ctx);

private:
    void note_overflow();

    lx::runtime::memory_arena* arena_ = nullptr;
    bool overflowed_ = false;
};

namespace detail {

/// An output iterator writing into `[at, end)` and counting every character it is given.
struct bounded_chars {
    using iterator_category = std::output_iterator_tag;
    using value_type = void;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = void;

    char* at = nullptr;
    char* end = nullptr;
    std::size_t* count = nullptr;

    bounded_chars& operator*() { return *this; }
    bounded_chars& operator++() { return *this; }
    bounded_chars operator++(int) { return *this; }
    bounded_chars& operator=(char c) {
        if (at < end)
            *at++ = c;
        ++*count;
        return *this;
    }
};

} // namespace detail

/// `fmt` formatted into the current build's frame arena, NUL-terminated: text that lives
/// as long as the description (a label copies it when applied). "" outside a build or once
/// the arena is exhausted. No heap allocation.
template<typename... Args>
[[nodiscard]] const char* frame_format(std::format_string<Args...> fmt, Args&&... args) {
    build_context* ctx = build_context::current();
    if (!ctx)
        return "";
    // One formatting pass into the stack for the usual short label; a second one only for
    // text longer than that.
    char local[128];
    std::size_t n = 0;
    const auto format_args = std::make_format_args(args...);
    (void)std::vformat_to(detail::bounded_chars{local, local + sizeof local, &n}, fmt.get(),
                          format_args);
    char* out = ctx->alloc_chars(static_cast<unsigned>(n + 1));
    if (!out)
        return "";
    if (n <= sizeof local)
        std::memcpy(out, local, n);
    else
        (void)std::vformat_to(out, fmt.get(), format_args);
    out[n] = '\0';
    return out;
}

/// Sets the current build context for one rebuild and restores the previous one.
class build_scope {
public:
    explicit build_scope(build_context& ctx);
    ~build_scope();

    build_scope(const build_scope&) = delete;
    build_scope& operator=(const build_scope&) = delete;

private:
    build_context* previous_ = nullptr;
};

} // namespace lx::ui

namespace {
/// UI affinity owns rebuilds, so a plain pointer is sufficient — there is never a
/// concurrent rebuild on another thread.
lx::ui::build_context* g_current_build_context = nullptr;

/// Zero stays reserved for "unset" so a default-constructed element never matches.
lx::ui::element_type_id g_next_type_id = 1;
} // namespace

lx::ui::element_type_id lx::ui::next_element_type_id() { return g_next_type_id++; }

lx::ui::build_context::build_context(lx::runtime::memory_arena& arena) : arena_{&arena} {}

const lx::ui::element** lx::ui::build_context::alloc_children(unsigned count) {
    if (count == 0)
        return nullptr;
    void* raw = arena_ ? arena_->allocate(static_cast<unsigned>(sizeof(const element*)) * count,
                                          static_cast<unsigned>(alignof(const element*)))
                       : nullptr;
    if (!raw) {
        note_overflow();
        return nullptr;
    }
    auto** slots = static_cast<const element**>(raw);
    for (unsigned i = 0; i < count; ++i)
        slots[i] = nullptr;
    return slots;
}

char* lx::ui::build_context::alloc_chars(unsigned count) {
    void* raw = arena_ && count ? arena_->allocate(count, 1) : nullptr;
    if (!raw) {
        note_overflow();
        return nullptr;
    }
    return static_cast<char*>(raw);
}

bool lx::ui::build_context::overflowed() const { return overflowed_; }
unsigned lx::ui::build_context::used_bytes() const { return arena_ ? arena_->used_bytes() : 0u; }
void lx::ui::build_context::reset_overflow() { overflowed_ = false; }

void lx::ui::build_context::note_overflow() {
    if (overflowed_)
        return;
    overflowed_ = true;
    log_ui.warn("UI frame arena exhausted — keeping previous tree");
}

lx::ui::build_context* lx::ui::build_context::current() { return g_current_build_context; }
void lx::ui::build_context::set_current(build_context* ctx) { g_current_build_context = ctx; }

lx::ui::build_scope::build_scope(build_context& ctx)
    : previous_{build_context::current()} {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    build_context::set_current(&ctx);
}

lx::ui::build_scope::~build_scope() { build_context::set_current(previous_); }
