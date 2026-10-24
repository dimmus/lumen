module;

import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.trace;

export module lx.scene:snapshot;

import std;

export namespace lx::scene {

struct draw_command {
    lx::texture_id texture{};
    /// Source box in buffer pixels, before `buffer_xform` (wl_surface + wp_viewport). Empty
    /// means the whole texture. Fractional edges are a viewport crop.
    lx::rect2f src{};
    /// Output pixels (the scene's projection already applied).
    lx::rect2i dst{};
    lx::transform2d transform = lx::transform2d::identity();
    lx::rect2i clip{};
    lx::color tint = lx::color::rgb(1.f, 1.f, 1.f, 1.f);
    float opacity = 1.f;
    lx::blend_mode blend = lx::blend_mode::premultiplied;
    lx::color_space src_space = lx::color_space::srgb;
    lx::image_description_id image_desc{};
    lx::buffer_transform buffer_xform = lx::buffer_transform::normal;
    float scale = 1.f;
    lx::draw_sort_key sort_key{};
    /// `dst` differs from the (transformed) source size: the draw resamples. A hint for
    /// filtering; a 1:1 draw on whole pixels samples texel centres exactly.
    bool scaled = false;
    /// Fully covered by an opaque draw above it, or entirely off the output: every backend
    /// skips it (identical pixels). Set by the scene's visibility pass (`mark_occlusion`).
    bool occluded = false;
    /// A solid colour, no texture (`wp_single_pixel_buffer_v1`): `solid_color` premultiplied
    /// and encoded; `texture` is empty. See `gfx::blit_command::solid`.
    bool solid = false;
    lx::color solid_color = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
};

inline constexpr unsigned k_initial_draw_commands = 256;
inline constexpr unsigned k_max_draw_commands = 4096;
inline constexpr unsigned k_snapshot_slot_count = 3;
inline constexpr unsigned k_initial_primitives = 256;
inline constexpr unsigned k_initial_batches = 32;
/// The ref stream holds every primitive and every surface draw: the two caps add, so a
/// full primitive stream never pushes a surface out.
inline constexpr unsigned k_max_refs = lx::gfx::k_max_primitives + k_max_draw_commands;

/// One view's frame: the surface draws (`draw_command`, what the compositor's backends take
/// today) and, since ADR-010 Phase 0, the typed instance arrays of the UI primitives with
/// the sorted ref stream and batches over both.
///
/// Storage: every array is heap-backed and grows geometrically up to its cap, never
/// shrinks and is reused across frames — after the first frames of a session nothing here
/// allocates (the draw-command array always worked that way; the primitive arrays follow
/// it). The order grid is inline (64 KB, `gfx::order_grid`).
class draw_list {
public:
    draw_list();
    ~draw_list();
    draw_list(const draw_list&) = delete;
    draw_list& operator=(const draw_list&) = delete;

    /// Empties every list and the order grid's cells (the grid keeps its view size).
    void clear();
    /// `clear()`, with the order grid reset for a view of `view_px` pixels (an empty size
    /// is unbounded: 8192 × 8192 px). What a frame build calls first.
    void begin(lx::size2i view_px);
    /// A surface draw. Once the list carries a primitive stream (`orders()` was taken) it
    /// takes its order from the grid here, at push: it sorts over what was emitted before it
    /// and under what is emitted after. Before that its order stays unassigned — a
    /// surface-only list (the compositor's scene) never touches the grid.
    void push(draw_command cmd,
              lx::runtime::overflow_action overflow = lx::runtime::overflow_action::warn_and_drop);
    /// A surface draw whose order the emitter already took from `orders()`: it then sorts
    /// against the primitives emitted around it.
    void push(draw_command cmd, std::uint32_t order,
              lx::runtime::overflow_action overflow = lx::runtime::overflow_action::warn_and_drop);

    // ── Primitives (ADR-010 §4.2): the instance plus a ref at `order`, which the caller
    // takes from `orders().assign(bounds)`. `bounds` is the clipped output-pixel rect, what
    // the batch bounds are the union of. Overflow: past `gfx::k_max_primitives` the
    // primitive is dropped (`drop_oldest` is not offered for instances; it drops too).
    inline void push_quad(const lx::gfx::quad_instance& q, lx::rect2i bounds,
                          std::uint32_t order,
                          lx::runtime::overflow_action overflow =
                              lx::runtime::overflow_action::warn_and_drop) {
        if (lx::gfx::quad_instance* slot = append_quad(bounds, order, overflow))
            *slot = q;
    }
    /// `push_quad` without the copy: the instance slot to fill (every byte of it — its old
    /// contents are undefined), or null when the cap dropped it. Inline while the arrays
    /// have room, which is every steady frame: a UI emits thousands a frame from another
    /// module, and a call and a staged 80-byte copy each are a measurable share of G3.
    [[nodiscard]] inline lx::gfx::quad_instance*
    append_quad(lx::rect2i bounds, std::uint32_t order,
                lx::runtime::overflow_action overflow = lx::runtime::overflow_action::warn_and_drop) {
        // Locals, not members, between the stores: the arrays are `uint32_t` too, so the
        // compiler must otherwise reload every count after every store.
        const unsigned n = primitive_count_;
        const unsigned index = quads_.count;
        if (n < ref_capacity_ && n < lx::gfx::k_max_primitives && index < quads_.capacity)
            [[likely]] {
            refs_[n] = {order, lx::gfx::primitive_kind::quad, 0, 0, index};
            ref_bounds_[n] = bounds;
            ref_draw_pos_[n] = count_;
            quads_.count = index + 1;
            primitive_count_ = n + 1;
            ref_count_ = n + 1;
            stream_live_ = true;
            return quads_.items + index;
        }
        return append_quad_growing(bounds, order, overflow);
    }
    void push_shadow(const lx::gfx::shadow_instance& s, lx::rect2i bounds, std::uint32_t order,
                     lx::runtime::overflow_action overflow = lx::runtime::overflow_action::warn_and_drop);
    /// `kind` is `mono_sprite`, `poly_sprite` or `path`; anything else is dropped.
    void push_sprite(lx::gfx::primitive_kind kind, std::uint8_t page,
                     const lx::gfx::sprite_instance& s, lx::rect2i bounds, std::uint32_t order,
                     lx::runtime::overflow_action overflow = lx::runtime::overflow_action::warn_and_drop);
    void push_underline(const lx::gfx::underline_instance& u, lx::rect2i bounds,
                        std::uint32_t order,
                        lx::runtime::overflow_action overflow = lx::runtime::overflow_action::warn_and_drop);

    /// The overlap grid primitives take their order from (UI affinity, with the build).
    /// Taking it starts the frame's primitive stream: the surfaces pushed so far without an
    /// order take theirs now, back to front by sort key, so they sort under whatever is
    /// emitted next, and every later `push(cmd)` takes its order at push.
    [[nodiscard]] inline lx::gfx::order_grid& orders() {
        stream_live_ = true;
        if (orders_pending_) [[unlikely]]
            assign_pending_orders();
        return orders_;
    }
    [[nodiscard]] inline const lx::gfx::order_grid& orders() const { return orders_; }

    /// Sorts the surface draws (sort key, then texture: paint order), appends a `surface`
    /// ref per draw, sorts the ref stream by `(order, kind, page)` — stable: equal keys keep
    /// their push order — and cuts a batch at every change of kind or page. A draw still
    /// without an order (the stream was started by a primitive pushed at a literal order, not
    /// by `orders()`) takes one here, after every primitive. A list with no primitive stream
    /// (no primitive, no order taken: the compositor's scene) is only sorted — `refs()` and
    /// `batches()` stay empty, the backends read the draw commands.
    void batch_for_render();
    [[nodiscard]] unsigned size() const;
    [[nodiscard]] const draw_command* data() const;
    [[nodiscard]] draw_command* data();
    [[nodiscard]] unsigned capacity() const;
    /// Draws, primitives and batches dropped at a cap since `clear`.
    [[nodiscard]] unsigned overflow_drops() const;

    /// Primitives pushed (surfaces not counted).
    [[nodiscard]] unsigned primitive_count() const { return primitive_count_; }
    /// After `batch_for_render`: the sorted stream, surfaces included (empty before it, and
    /// for a list with no primitive stream).
    [[nodiscard]] std::span<const lx::gfx::primitive_ref> refs() const {
        return {stream_, sorted_count_};
    }
    [[nodiscard]] std::span<const lx::gfx::batch> batches() const { return {batches_, batch_count_}; }
    /// After `batch_for_render`: what a backend uploads (ADR-010 §4.4) — every primitive
    /// batch's instances, batch after batch, each batch's in its sorted order, packed into
    /// `ring`. A batch starts at a multiple of `align` bytes (a power of two; 0 and 1 pack
    /// tight). Refs naming consecutive instances are copied as one block, so a stream the
    /// sort did not permute is one copy per batch. `batch_offsets`, when given (one entry per
    /// `batches()`; extra entries untouched, missing ones skipped), receives each batch's
    /// byte offset; a `surface` batch copies nothing and gets the offset reached. Returns the
    /// bytes the stream needs: a batch that does not fit `ring` entirely is not written, and
    /// a result above `ring.size()` tells the caller the ring overflowed.
    [[nodiscard]] std::size_t encode_instances(std::span<std::byte> ring,
                                               std::span<std::uint32_t> batch_offsets = {},
                                               std::size_t align = 1) const;
    [[nodiscard]] std::span<const lx::gfx::quad_instance> quad_instances() const {
        return {quads_.items, quads_.count};
    }
    [[nodiscard]] std::span<const lx::gfx::shadow_instance> shadow_instances() const {
        return {shadows_.items, shadows_.count};
    }
    /// Shared by `mono_sprite`, `poly_sprite` and `path`.
    [[nodiscard]] std::span<const lx::gfx::sprite_instance> sprite_instances() const {
        return {sprites_.items, sprites_.count};
    }
    [[nodiscard]] std::span<const lx::gfx::underline_instance> underline_instances() const {
        return {underlines_.items, underlines_.count};
    }
    /// The primitives in push order (their kind and index into the typed arrays; the sorted
    /// stream is `refs()`), and per primitive how many draw commands had been pushed before
    /// it: where it sits in paint order among the surfaces, for the visibility pass
    /// (`scene::mark_occlusion`).
    [[nodiscard]] std::span<const lx::gfx::primitive_ref> pushed_primitives() const {
        return {refs_, primitive_count_};
    }
    [[nodiscard]] std::span<const std::uint32_t> primitive_draw_positions() const {
        return {ref_draw_pos_, primitive_count_};
    }
    /// Turns every primitive emitted into a `content` sized space into the pixels of an
    /// output of transform `t` (`scene_graph::build_draw_list_into` for a turned output, ADR
    /// 008 §C.4, as it turns the draw commands): bounds and clip of every kind and the
    /// stream's bounds, and for quads and shadows the per-corner radii and per-edge border
    /// widths, permuted to the corners and edges they land on, and a gradient's angle. A
    /// sprite's tile and an underline's direction are not turned (Phase 2 text). UI affinity,
    /// before `batch_for_render`; no allocation.
    void transform_primitives(lx::buffer_transform t, lx::size2i content);

private:
    friend class snapshot_buffer;
    static constexpr std::uint32_t k_order_unassigned = ~0u;

    /// A typed instance array with the growth policy above.
    template <typename T>
    struct instance_array {
        instance_array() = default;
        ~instance_array() { delete[] items; }
        instance_array(const instance_array&) = delete;
        instance_array& operator=(const instance_array&) = delete;
        /// Appends under `cap`; false (nothing written) at the cap.
        [[nodiscard]] bool push(const T& v, unsigned cap);
        T* items = nullptr;
        unsigned capacity = 0;
        unsigned count = 0;
    };

    /// `append_quad` past its inline fast path: grows the arrays, or drops at the cap.
    [[nodiscard]] lx::gfx::quad_instance* append_quad_growing(lx::rect2i bounds,
                                                              std::uint32_t order,
                                                              lx::runtime::overflow_action overflow);
    /// `orders()` starting the stream: the surfaces pushed before it take their orders.
    void assign_pending_orders();
    void ensure_capacity(unsigned need);
    void ensure_ref_capacity(unsigned need);
    void ensure_batch_capacity(unsigned need);
    void clear_lists();
    void push_command(const draw_command& cmd, std::uint32_t order,
                      lx::runtime::overflow_action overflow);
    void sort_commands();
    [[nodiscard]] bool admit_primitive(lx::runtime::overflow_action overflow);
    void push_ref(lx::gfx::primitive_kind kind, std::uint8_t page, unsigned index,
                  lx::rect2i bounds, std::uint32_t order);

    draw_command* commands_ = nullptr;
    /// Permutation scratch for `batch_for_render`, grown with `commands_`. Four bytes per
    /// command, reused every frame — the sort must not put a `draw_command[]` on the stack
    /// or allocate on the hot path.
    unsigned* order_ = nullptr;
    /// Per command: its order in the primitive stream, or `k_order_unassigned`.
    std::uint32_t* cmd_orders_ = nullptr;
    unsigned capacity_ = 0;
    unsigned count_ = 0;
    unsigned overflow_drops_ = 0;
    bool cap_warned_ = false;
    bool primitive_cap_warned_ = false;
    bool batch_cap_warned_ = false;
    /// This frame has a primitive stream: `orders()` was taken, or a primitive or an
    /// ordered surface was pushed. Until then nothing touches the grid or the ref stream.
    bool stream_live_ = false;
    /// A surface was pushed with its order unassigned before the stream started.
    bool orders_pending_ = false;

    instance_array<lx::gfx::quad_instance> quads_{};
    instance_array<lx::gfx::shadow_instance> shadows_{};
    instance_array<lx::gfx::sprite_instance> sprites_{};
    instance_array<lx::gfx::underline_instance> underlines_{};
    unsigned primitive_count_ = 0;

    /// The ref stream and its sort scratch, grown together: the refs in push order (the
    /// primitives first, then the surfaces `batch_for_render` appends), the clipped bounds
    /// per ref, one packed 64-bit key per ref and the radix sort's second key buffer, and
    /// the sorted stream (refs and their bounds) the sort writes into. Push order is never
    /// disturbed, so `batch_for_render` can run again on the same list.
    lx::gfx::primitive_ref* refs_ = nullptr;
    lx::rect2i* ref_bounds_ = nullptr;
    /// Per primitive (`refs_[0, primitive_count_)`): `count_` when it was pushed.
    std::uint32_t* ref_draw_pos_ = nullptr;
    std::uint64_t* ref_keys_ = nullptr;
    std::uint64_t* ref_keys_swap_ = nullptr;
    lx::gfx::primitive_ref* sorted_ = nullptr;
    /// The sorted stream `refs()` and `encode_instances` read: `sorted_`, or `refs_` itself
    /// when the stream is its push order (a one-kind stream: nothing to copy).
    const lx::gfx::primitive_ref* stream_ = nullptr;
    lx::rect2i* sorted_bounds_ = nullptr;
    unsigned ref_capacity_ = 0;
    unsigned ref_count_ = 0;
    unsigned sorted_count_ = 0;

    lx::gfx::batch* batches_ = nullptr;
    unsigned batch_capacity_ = 0;
    unsigned batch_count_ = 0;

    lx::gfx::order_grid orders_{};
};

class immutable_frame_snapshot {
public:
    [[nodiscard]] const draw_list& draws() const { return draws_; }
    /// The sorted primitive stream and its batches (`draw_list::refs` / `batches`).
    [[nodiscard]] std::span<const lx::gfx::primitive_ref> refs() const { return draws_.refs(); }
    [[nodiscard]] std::span<const lx::gfx::batch> batches() const { return draws_.batches(); }
    /// Bounding box of `damage_rects()`; empty when nothing changed or when full.
    [[nodiscard]] lx::rect2i damage() const { return damage_.bounds(); }
    /// What changed since the previous *published* snapshot — including the damage of any
    /// snapshot the render thread never acquired (see `snapshot_buffer::publish`). Empty
    /// means nothing changed: the frame need not be composited at all.
    [[nodiscard]] const lx::gfx::damage_region& damage_rects() const { return damage_; }
    [[nodiscard]] unsigned frame_index() const { return frame_index_; }
    [[nodiscard]] bool dropped() const { return dropped_; }
    /// What shows where no draw does: black, or the session lock's solid colour.
    [[nodiscard]] lx::color clear_color() const { return clear_; }

private:
    friend class snapshot_buffer;
    draw_list draws_{};
    lx::gfx::damage_region damage_{};
    lx::color clear_ = lx::color::rgb(0.f, 0.f, 0.f);
    unsigned frame_index_ = 0;
    unsigned slot_ = 0;
    bool dropped_ = false;
};

/// What the producer does when no slot is free to write into.
enum class backpressure_policy {
    /// Skip the frame immediately. Lowest producer cost, visible frame drops.
    drop_latest,
    /// Spin-wait (bounded) for the consumer to release a slot. No drops until the bound
    /// is hit; costs UI-thread time. Intended for latency experiments, not production.
    stall_ui,
    /// Use every slot before considering a drop; ignores `max_in_flight` for admission.
    /// With one consumer a slot is always free, so this never drops.
    triple_buffer,
};

struct snapshot_config {
    backpressure_policy backpressure = backpressure_policy::triple_buffer;
    /// Admission limit for `drop_latest` / `stall_ui`. Ignored by `triple_buffer`.
    unsigned max_in_flight = 2;
};

inline constexpr unsigned k_no_slot = ~0u;
/// Yield iterations before `stall_ui` gives up. Bounded so a stalled or crashed consumer
/// cannot wedge the producer thread forever.
inline constexpr unsigned k_stall_yield_budget = 1024;

class snapshot_buffer;

/// Exclusive right to write one slot, held across the whole frame build.
///
/// Slot validation happens when the lease is taken, not when it is published: the producer
/// mutates the slot's `draw_list` for the entire build, so checking afterwards checks the
/// wrong instant. An invalid lease means no slot was free — build nothing.
class write_lease {
public:
    write_lease() = default;

    write_lease(const write_lease&) = delete;
    write_lease& operator=(const write_lease&) = delete;
    write_lease(write_lease&& other) noexcept { *this = std::move(other); }
    write_lease& operator=(write_lease&& other) noexcept {
        if (this != &other) {
            owner_ = other.owner_;
            draws_ = other.draws_;
            slot_ = other.slot_;
            other.owner_ = nullptr;
            other.draws_ = nullptr;
            other.slot_ = k_no_slot;
        }
        return *this;
    }

    [[nodiscard]] bool valid() const { return draws_ != nullptr; }
    /// Precondition: `valid()`.
    [[nodiscard]] draw_list& draws() const { return *draws_; }
    [[nodiscard]] unsigned slot() const { return slot_; }

private:
    friend class snapshot_buffer;

    snapshot_buffer* owner_ = nullptr;
    draw_list* draws_ = nullptr;
    unsigned slot_ = k_no_slot;
};

class snapshot_buffer {
public:
    explicit snapshot_buffer(snapshot_config config = {});

    void set_config(snapshot_config config);
    [[nodiscard]] snapshot_config config() const;

    /// Reserves a slot no consumer can be holding, applying the backpressure policy.
    /// An invalid lease means the frame must be skipped — do not build a draw list.
    [[nodiscard]] write_lease begin_frame();
    /// Publishes a lease and consumes it. False if the lease was invalid or not ours.
    ///
    /// The mailbox keeps only the newest snapshot, so a snapshot the consumer never
    /// acquired would take its damage with it and leave stale pixels on screen. Publishing
    /// over an unconsumed snapshot therefore carries that snapshot's damage forward into
    /// this one. The race with a concurrent `acquire` can only over-report damage.
    [[nodiscard]] bool publish(write_lease& lease, const lx::gfx::damage_region& damage,
                               unsigned frame_index,
                               lx::color clear = lx::color::rgb(0.f, 0.f, 0.f));

    [[nodiscard]] const immutable_frame_snapshot& acquire();
    void release(const immutable_frame_snapshot& snapshot);

    [[nodiscard]] unsigned publish_count() const;
    [[nodiscard]] unsigned dropped_count() const;
    [[nodiscard]] unsigned in_flight_count() const;
    [[nodiscard]] bool would_stall() const;

private:
    /// A slot is writable only if it is neither the published slot nor held by a reader.
    /// Returns `k_no_slot` when every slot is busy — never a live slot.
    [[nodiscard]] unsigned find_free_slot() const;
    [[nodiscard]] bool admission_blocked() const;

    snapshot_config config_{};
    immutable_frame_snapshot buffers_[k_snapshot_slot_count]{};
    std::atomic<unsigned> readers_[k_snapshot_slot_count]{};
    /// Set by `acquire` once the consumer holds a slot; cleared when the slot is published.
    /// Starts true: nothing unpublished has damage to carry.
    std::atomic<bool> consumed_[k_snapshot_slot_count]{true, true, true};
    std::atomic<unsigned> read_index_{0};
    std::atomic<unsigned> publish_count_{0};
    std::atomic<unsigned> dropped_count_{0};
    std::atomic<unsigned> in_flight_{0};
};

} // namespace lx::scene


lx::scene::draw_list::draw_list() {
    capacity_ = k_initial_draw_commands;
    commands_ = new draw_command[capacity_];
    order_ = new unsigned[capacity_];
    cmd_orders_ = new std::uint32_t[capacity_];
}

lx::scene::draw_list::~draw_list() {
    delete[] commands_;
    delete[] order_;
    delete[] cmd_orders_;
    delete[] refs_;
    delete[] ref_bounds_;
    delete[] ref_draw_pos_;
    delete[] ref_keys_;
    delete[] ref_keys_swap_;
    delete[] sorted_;
    delete[] sorted_bounds_;
    delete[] batches_;
}

template <typename T>
bool lx::scene::draw_list::instance_array<T>::push(const T& v, unsigned cap) {
    if (count >= cap)
        return false;
    if (count >= capacity) {
        unsigned new_cap = capacity > 0 ? capacity * 2 : k_initial_primitives;
        if (new_cap > cap)
            new_cap = cap;
        T* next = new T[new_cap];
        for (unsigned i = 0; i < count; ++i)
            next[i] = items[i];
        delete[] items;
        items = next;
        capacity = new_cap;
    }
    items[count++] = v;
    return true;
}

namespace {

[[nodiscard]] lx::rect2i lx_scene_clipped(lx::rect2i r, lx::rect2i clip) {
    if (clip.width <= 0 || clip.height <= 0)
        return r;
    const int x0 = r.x > clip.x ? r.x : clip.x;
    const int y0 = r.y > clip.y ? r.y : clip.y;
    const int x1 = r.x + r.width < clip.x + clip.width ? r.x + r.width : clip.x + clip.width;
    const int y1 = r.y + r.height < clip.y + clip.height ? r.y + r.height : clip.y + clip.height;
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

/// Sorts `n` keys whose low 16 bits are their own position (unique, ascending in input
/// order) by an LSD radix sort over the bytes above those 16 bits, ping-ponging between
/// `keys` and `swap`, and writes the refs and bounds at those positions in sorted order to
/// `out_refs` / `out_bounds`. Each pass is stable and the input is in position order, so
/// the position bits never need a pass: the result is the full 64-bit ascending order. A
/// byte equal in every key (`varying` has its bits clear: the kind and page of a one-kind
/// stream, the order's high bytes) is skipped; at least one byte must vary. The last pass
/// places refs and bounds rather than keys — with a single pass, the usual case, it reads
/// them in push order instead of gathering them by position afterwards.
void lx_scene_radix_sort(std::uint64_t* keys, std::uint64_t* swap, unsigned n,
                         std::uint64_t varying, const lx::gfx::primitive_ref* refs,
                         const lx::rect2i* bounds, lx::gfx::primitive_ref* out_refs,
                         lx::rect2i* out_bounds) {
    unsigned shifts[6]{};
    unsigned passes = 0;
    for (unsigned shift = 16; shift < 64; shift += 8)
        if (((varying >> shift) & 0xFFu) != 0)
            shifts[passes++] = shift;
    std::uint64_t* src = keys;
    std::uint64_t* dst = swap;
    for (unsigned p = 0; p < passes; ++p) {
        const unsigned shift = shifts[p];
        std::uint32_t offsets[256]{};
        for (unsigned i = 0; i < n; ++i)
            ++offsets[(src[i] >> shift) & 0xFFu];
        std::uint32_t sum = 0;
        for (std::uint32_t& c : offsets) {
            const std::uint32_t count = c;
            c = sum;
            sum += count;
        }
        if (p + 1 == passes) {
            for (unsigned i = 0; i < n; ++i) {
                const std::uint64_t k = src[i];
                const std::uint32_t slot = offsets[(k >> shift) & 0xFFu]++;
                const auto pos = static_cast<unsigned>(k & 0xFFFFu);
                out_refs[slot] = refs[pos];
                out_bounds[slot] = bounds[pos];
            }
            return;
        }
        for (unsigned i = 0; i < n; ++i) {
            const std::uint64_t k = src[i];
            dst[offsets[(k >> shift) & 0xFFu]++] = k;
        }
        std::uint64_t* t = src;
        src = dst;
        dst = t;
    }
}

/// The instances `refs[0, count)` name, in that order, into `dst`: a run of consecutive
/// indices is one block copy, a lone instance a fixed-size one.
template <typename T>
void lx_scene_gather(std::byte* dst, const T* src, const lx::gfx::primitive_ref* refs,
                     unsigned count) {
    unsigned j = 0;
    while (j < count) {
        const std::uint32_t start = refs[j].index;
        unsigned run = 1;
        while (j + run < count && refs[j + run].index == start + run)
            ++run;
        if (run == 1)
            std::memcpy(dst, src + start, sizeof(T));
        else
            std::memcpy(dst, src + start, sizeof(T) * run);
        dst += sizeof(T) * run;
        j += run;
    }
}

[[nodiscard]] lx::rect2i lx_scene_union(lx::rect2i a, lx::rect2i b) {
    if (a.width <= 0 || a.height <= 0)
        return b;
    if (b.width <= 0 || b.height <= 0)
        return a;
    const int x0 = a.x < b.x ? a.x : b.x;
    const int y0 = a.y < b.y ? a.y : b.y;
    const int x1 = a.x + a.width > b.x + b.width ? a.x + a.width : b.x + b.width;
    const int y1 = a.y + a.height > b.y + b.height ? a.y + a.height : b.y + b.height;
    return {x0, y0, x1 - x0, y1 - y0};
}

} // namespace

void lx::scene::draw_list::ensure_capacity(unsigned need) {
    if (need <= capacity_)
        return;
    unsigned new_cap = capacity_ > 0 ? capacity_ : k_initial_draw_commands;
    while (new_cap < need && new_cap < k_max_draw_commands)
        new_cap *= 2;
    new_cap = new_cap > k_max_draw_commands ? k_max_draw_commands : new_cap;
    if (new_cap <= capacity_)
        return;
    auto* next = new draw_command[new_cap];
    for (unsigned i = 0; i < count_; ++i)
        next[i] = commands_[i];
    delete[] commands_;
    commands_ = next;

    auto* next_orders = new std::uint32_t[new_cap];
    for (unsigned i = 0; i < count_; ++i)
        next_orders[i] = cmd_orders_[i];
    delete[] cmd_orders_;
    cmd_orders_ = next_orders;

    // Scratch holds no state between calls, so grow it without copying.
    delete[] order_;
    order_ = new unsigned[new_cap];

    capacity_ = new_cap;
}

void lx::scene::draw_list::ensure_ref_capacity(unsigned need) {
    if (need <= ref_capacity_)
        return;
    unsigned new_cap = ref_capacity_ > 0 ? ref_capacity_ : k_initial_primitives;
    while (new_cap < need && new_cap < k_max_refs)
        new_cap *= 2;
    new_cap = new_cap > k_max_refs ? k_max_refs : new_cap;
    if (new_cap <= ref_capacity_)
        return;
    auto* refs = new lx::gfx::primitive_ref[new_cap];
    auto* bounds = new lx::rect2i[new_cap];
    auto* positions = new std::uint32_t[new_cap];
    for (unsigned i = 0; i < ref_count_; ++i) {
        refs[i] = refs_[i];
        bounds[i] = ref_bounds_[i];
    }
    for (unsigned i = 0; i < primitive_count_; ++i)
        positions[i] = ref_draw_pos_[i];
    delete[] refs_;
    delete[] ref_bounds_;
    delete[] ref_draw_pos_;
    refs_ = refs;
    ref_bounds_ = bounds;
    ref_draw_pos_ = positions;
    // Sort scratch and output hold nothing between calls.
    delete[] ref_keys_;
    delete[] ref_keys_swap_;
    delete[] sorted_;
    delete[] sorted_bounds_;
    ref_keys_ = new std::uint64_t[new_cap];
    ref_keys_swap_ = new std::uint64_t[new_cap];
    sorted_ = new lx::gfx::primitive_ref[new_cap];
    sorted_bounds_ = new lx::rect2i[new_cap];
    stream_ = sorted_;
    sorted_count_ = 0;
    ref_capacity_ = new_cap;
}

void lx::scene::draw_list::ensure_batch_capacity(unsigned need) {
    if (need <= batch_capacity_)
        return;
    unsigned new_cap = batch_capacity_ > 0 ? batch_capacity_ : k_initial_batches;
    while (new_cap < need && new_cap < lx::gfx::k_max_batches)
        new_cap *= 2;
    new_cap = new_cap > lx::gfx::k_max_batches ? lx::gfx::k_max_batches : new_cap;
    if (new_cap <= batch_capacity_)
        return;
    // `batch_for_render` grows this while it cuts: the batches already cut carry over.
    auto* next = new lx::gfx::batch[new_cap];
    for (unsigned i = 0; i < batch_count_; ++i)
        next[i] = batches_[i];
    delete[] batches_;
    batches_ = next;
    batch_capacity_ = new_cap;
}

void lx::scene::draw_list::clear_lists() {
    count_ = 0;
    // A cap warns once per overflow, not once per frame while it lasts (ADR-010 §4.4): the
    // warnings re-arm only after a frame that dropped nothing. Every drop is still counted.
    if (overflow_drops_ == 0) {
        cap_warned_ = false;
        primitive_cap_warned_ = false;
        batch_cap_warned_ = false;
    }
    overflow_drops_ = 0;
    stream_live_ = false;
    orders_pending_ = false;
    quads_.count = 0;
    shadows_.count = 0;
    sprites_.count = 0;
    underlines_.count = 0;
    primitive_count_ = 0;
    ref_count_ = 0;
    sorted_count_ = 0;
    batch_count_ = 0;
}

void lx::scene::draw_list::clear() {
    clear_lists();
    orders_.reset(orders_.view());
}

void lx::scene::draw_list::begin(lx::size2i view_px) {
    clear_lists();
    orders_.reset(view_px);
}

void lx::scene::draw_list::assign_pending_orders() {
    // The surfaces pushed before the stream started, back to front.
    orders_pending_ = false;
    sort_commands();
    for (unsigned i = 0; i < count_; ++i) {
        const draw_command& cmd = commands_[i];
        if (cmd_orders_[i] == k_order_unassigned)
            cmd_orders_[i] = orders_.assign(lx_scene_clipped(cmd.dst, cmd.clip));
    }
}

void lx::scene::draw_list::push(draw_command cmd, lx::runtime::overflow_action overflow) {
    if (stream_live_) {
        push_command(cmd, orders_.assign(lx_scene_clipped(cmd.dst, cmd.clip)), overflow);
        return;
    }
    orders_pending_ = true;
    push_command(cmd, k_order_unassigned, overflow);
}

void lx::scene::draw_list::push(draw_command cmd, std::uint32_t order,
                                lx::runtime::overflow_action overflow) {
    stream_live_ = true;
    push_command(cmd, order, overflow);
}

void lx::scene::draw_list::push_command(const draw_command& cmd, std::uint32_t order,
                                        lx::runtime::overflow_action overflow) {
    if (count_ < k_max_draw_commands) {
        ensure_capacity(count_ + 1);
        if (count_ < capacity_) {
            cmd_orders_[count_] = order;
            commands_[count_++] = cmd;
            return;
        }
    }
    switch (overflow) {
    case lx::runtime::overflow_action::drop_oldest:
        if (count_ > 0) {
            for (unsigned i = 1; i < count_; ++i) {
                commands_[i - 1] = commands_[i];
                cmd_orders_[i - 1] = cmd_orders_[i];
            }
            commands_[count_ - 1] = cmd;
            cmd_orders_[count_ - 1] = order;
        }
        break;
    case lx::runtime::overflow_action::reject:
        break;
    case lx::runtime::overflow_action::warn_and_drop:
        if (!cap_warned_) {
            cap_warned_ = true;
            lx::trace::logger::global().log(lx::trace::level::warn, "scene.draw_list",
                                            "draw command cap reached");
        }
        [[fallthrough]];
    case lx::runtime::overflow_action::drop_newest:
    default:
        ++overflow_drops_;
        break;
    }
}

bool lx::scene::draw_list::admit_primitive(lx::runtime::overflow_action overflow) {
    if (primitive_count_ < lx::gfx::k_max_primitives)
        return true;
    switch (overflow) {
    case lx::runtime::overflow_action::reject:
        break;
    case lx::runtime::overflow_action::warn_and_drop:
        if (!primitive_cap_warned_) {
            primitive_cap_warned_ = true;
            lx::trace::logger::global().log(lx::trace::level::warn, "scene.draw_list",
                                            "primitive cap reached");
        }
        [[fallthrough]];
    default:
        ++overflow_drops_;
        break;
    }
    return false;
}

void lx::scene::draw_list::push_ref(lx::gfx::primitive_kind kind, std::uint8_t page,
                                    unsigned index, lx::rect2i bounds, std::uint32_t order) {
    // Primitives occupy `refs_[0, primitive_count_)`; a push after `batch_for_render`
    // overwrites the surface refs it appended, which the next call appends again.
    stream_live_ = true;
    ensure_ref_capacity(primitive_count_ + 1);
    refs_[primitive_count_] = {order, kind, page, 0, index};
    ref_bounds_[primitive_count_] = bounds;
    ref_draw_pos_[primitive_count_] = count_;
    ++primitive_count_;
    ref_count_ = primitive_count_;
}

lx::gfx::quad_instance*
lx::scene::draw_list::append_quad_growing(lx::rect2i bounds, std::uint32_t order,
                                          lx::runtime::overflow_action overflow) {
    if (!admit_primitive(overflow) || !quads_.push({}, lx::gfx::k_max_primitives))
        return nullptr;
    push_ref(lx::gfx::primitive_kind::quad, 0, quads_.count - 1, bounds, order);
    return quads_.items + (quads_.count - 1);
}

void lx::scene::draw_list::push_shadow(const lx::gfx::shadow_instance& s, lx::rect2i bounds,
                                       std::uint32_t order,
                                       lx::runtime::overflow_action overflow) {
    if (!admit_primitive(overflow) || !shadows_.push(s, lx::gfx::k_max_primitives))
        return;
    push_ref(lx::gfx::primitive_kind::shadow, 0, shadows_.count - 1, bounds, order);
}

void lx::scene::draw_list::push_sprite(lx::gfx::primitive_kind kind, std::uint8_t page,
                                       const lx::gfx::sprite_instance& s, lx::rect2i bounds,
                                       std::uint32_t order,
                                       lx::runtime::overflow_action overflow) {
    if (kind != lx::gfx::primitive_kind::mono_sprite &&
        kind != lx::gfx::primitive_kind::poly_sprite && kind != lx::gfx::primitive_kind::path)
        return;
    if (!admit_primitive(overflow) || !sprites_.push(s, lx::gfx::k_max_primitives))
        return;
    push_ref(kind, page, sprites_.count - 1, bounds, order);
}

void lx::scene::draw_list::push_underline(const lx::gfx::underline_instance& u,
                                          lx::rect2i bounds, std::uint32_t order,
                                          lx::runtime::overflow_action overflow) {
    if (!admit_primitive(overflow) || !underlines_.push(u, lx::gfx::k_max_primitives))
        return;
    push_ref(lx::gfx::primitive_kind::underline, 0, underlines_.count - 1, bounds, order);
}

void lx::scene::draw_list::sort_commands() {
    if (count_ <= 1)
        return;

    if (!order_)
        return;

    const auto before = [this](unsigned a, unsigned b) {
        const auto& ca = commands_[a];
        const auto& cb = commands_[b];
        if (ca.sort_key.value != cb.sort_key.value)
            return ca.sort_key.value < cb.sort_key.value;
        return ca.texture.id() < cb.texture.id();
    };
    // Every emitter pushes in paint order (`draw_sort_key::make(layer, out.size())`), so the
    // list nearly always is sorted already: one pass and nothing else. The sort used to run
    // anyway, and `stable_sort` allocates its buffer on each call — the publish path's one
    // heap allocation per view per frame.
    bool sorted = true;
    for (unsigned i = 1; i < count_ && sorted; ++i)
        sorted = !before(i, i - 1);
    if (sorted) [[likely]]
        return;

    unsigned* indices = order_;
    for (unsigned i = 0; i < count_; ++i)
        indices[i] = i;

    // Strict painter's order: `sort_key.value` packs layer above z-order, so comparing it
    // whole orders by layer and then back-to-front within a layer. Texture id breaks ties
    // to batch state changes among draws that cannot overlap in z.
    //
    // Deliberately NOT partitioned opaque-first. That is the standard trick, but it needs
    // a depth buffer to stay correct, and this renderer has none — one color attachment,
    // no depth-stencil state. Without a depth test, hoisting opaque draws ahead of
    // translucent ones inverts z for any translucent surface sitting *behind* an opaque
    // one: the translucent surface would be painted last and blend over the window that
    // should have covered it. `scene::surface_node::emit` marks opaque content (no alpha,
    // or a full opaque region) as `blend_mode::opaque`, so an ordinary "translucent
    // terminal behind a solid window" stack hits this. The CPU backend's occlusion cull compounds it — it scans for
    // the topmost covering opaque draw and skips everything below, which is only sound if
    // the list really is in paint order.
    //
    // Restore the partition only alongside a depth buffer.
    // Ties keep their push order, as a stable sort would, without its allocation.
    std::sort(indices, indices + count_, [&](unsigned a, unsigned b) {
        if (before(a, b))
            return true;
        if (before(b, a))
            return false;
        return a < b;
    });

    // Apply the permutation in place, one cycle at a time: `indices[i]` names the command
    // that belongs at slot `i`, so walking a cycle moves each element straight to its
    // destination with a single element held aside. One `draw_command` of scratch instead
    // of a full second array. `indices` is consumed as it goes (entries are set to their
    // own slot), which is why it is rebuilt at the top of every call. The command's
    // assigned order travels with it.
    for (unsigned i = 0; i < count_; ++i) {
        if (indices[i] == i)
            continue;
        draw_command held = commands_[i];
        const std::uint32_t held_order = cmd_orders_[i];
        unsigned slot = i;
        for (;;) {
            const unsigned from = indices[slot];
            indices[slot] = slot;
            if (from == i)
                break;
            commands_[slot] = commands_[from];
            cmd_orders_[slot] = cmd_orders_[from];
            slot = from;
        }
        commands_[slot] = held;
        cmd_orders_[slot] = held_order;
    }
}

void lx::scene::draw_list::batch_for_render() {
    sort_commands();
    if (!stream_live_) {
        // Nothing to order the surfaces against, and nothing reads a surface-only stream:
        // the grid stays untouched, so the next reset clears nothing either.
        ref_count_ = 0;
        sorted_count_ = 0;
        batch_count_ = 0;
        return;
    }

    lx::trace::scoped_span span{"scene", "scene.order"};

    // The surface refs, after the primitives: one per draw in its (sorted) paint order. A
    // draw still without an order gets one from the grid now, so surfaces keep their
    // relative order among themselves, and later calls reuse it (idempotent).
    ref_count_ = primitive_count_;
    for (unsigned i = 0; i < count_; ++i) {
        const draw_command& cmd = commands_[i];
        const lx::rect2i r = lx_scene_clipped(cmd.dst, cmd.clip);
        if (cmd_orders_[i] == k_order_unassigned)
            cmd_orders_[i] = orders_.assign(r);
        ensure_ref_capacity(ref_count_ + 1);
        refs_[ref_count_] = {cmd_orders_[i], lx::gfx::primitive_kind::surface, 0, 0, i};
        ref_bounds_[ref_count_] = r;
        ++ref_count_;
    }

    // One kind on one page and no surface — only quads, only shadows or only underlines
    // (their page is always 0): the stream is one batch whatever the orders, and the push
    // order, the painter's order every order the grid hands out respects, is already a
    // valid order inside it. The sort exists to batch, so it has nothing to do: the stream
    // is the push order, the batch the union of every bound.
    const unsigned typed_kinds = (quads_.count != 0 ? 1u : 0u) + (shadows_.count != 0 ? 1u : 0u) +
                                 (underlines_.count != 0 ? 1u : 0u);
    if (count_ == 0 && sprites_.count == 0 && typed_kinds == 1) {
        // Pushes after this write past `sorted_count_`, so the span stays as sorted here.
        stream_ = refs_;
        sorted_count_ = ref_count_;
        int x0 = std::numeric_limits<int>::max();
        int y0 = std::numeric_limits<int>::max();
        int x1 = std::numeric_limits<int>::min();
        int y1 = std::numeric_limits<int>::min();
        for (unsigned i = 0; i < ref_count_; ++i) {
            const lx::rect2i b = ref_bounds_[i];
            if (b.width <= 0 || b.height <= 0)
                continue;
            x0 = b.x < x0 ? b.x : x0;
            y0 = b.y < y0 ? b.y : y0;
            x1 = b.x + b.width > x1 ? b.x + b.width : x1;
            y1 = b.y + b.height > y1 ? b.y + b.height : y1;
        }
        ensure_batch_capacity(1);
        batches_[0] = {refs_[0].kind, 0, 0, ref_count_,
                       x1 > x0 ? lx::rect2i{x0, y0, x1 - x0, y1 - y0} : lx::rect2i{}};
        batch_count_ = 1;
        if (lx::trace::timeline_enabled()) {
            lx::trace::timeline_counter("scene.primitives", primitive_count_);
            lx::trace::timeline_counter("scene.refs", sorted_count_);
            lx::trace::timeline_counter("scene.batches", batch_count_);
        }
        return;
    }

    // Stable sort by (order, kind, page) as one 64-bit key per ref, the push position in
    // the low bits: keys are unique, so the sorted keys name the source ref of every output
    // slot and the order is the stable one; `refs_` itself stays in push order. Building
    // the keys also tells whether they already ascend (a stream emitted in order: nothing
    // to sort) and which key bytes vary at all, so the radix sort (no comparison, no
    // allocation: two fixed key buffers) runs a pass only for those — typically one, the
    // order's low byte. Same result as a comparison sort of the full keys.
    std::uint64_t first_key = 0;
    std::uint64_t varying = 0;
    bool ascending = true;
    std::uint64_t previous = 0;
    for (unsigned i = 0; i < ref_count_; ++i) {
        const lx::gfx::primitive_ref& r = refs_[i];
        const std::uint64_t key =
            (static_cast<std::uint64_t>(r.order) << 32u) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(r.kind)) << 24u) |
            (static_cast<std::uint64_t>(r.page) << 16u) | i;
        if (i == 0)
            first_key = key;
        ascending = ascending && key >= previous;
        varying |= key ^ first_key;
        previous = key;
        ref_keys_[i] = key;
    }
    static_assert(k_max_refs <= 0xFFFFu, "the ref position must fit the key's low 16 bits");
    // The bounds of `stream_[i]`, for the batch union below.
    const lx::rect2i* sorted_bounds = ref_bounds_;
    if (ascending) {
        // Already in (order, kind, page) order: the sorted stream is the push order. With
        // no surface refs appended it is `refs_` itself — a later push writes past it —
        // otherwise a copy, since a push overwrites the surface refs.
        if (count_ == 0) {
            stream_ = refs_;
        } else {
            std::memcpy(sorted_, refs_, sizeof(lx::gfx::primitive_ref) * ref_count_);
            stream_ = sorted_;
        }
    } else {
        // Not ascending means some byte above the position bits varies: at least one pass.
        lx_scene_radix_sort(ref_keys_, ref_keys_swap_, ref_count_, varying, refs_, ref_bounds_,
                            sorted_, sorted_bounds_);
        stream_ = sorted_;
        sorted_bounds = sorted_bounds_;
    }
    sorted_count_ = ref_count_;

    // Cut runs: a new batch whenever the kind or the atlas page changes.
    batch_count_ = 0;
    lx::gfx::batch* current = nullptr;
    for (unsigned i = 0; i < sorted_count_; ++i) {
        const lx::gfx::primitive_ref& r = stream_[i];
        if (!current || current->kind != r.kind || current->page != r.page) {
            if (batch_count_ >= lx::gfx::k_max_batches) {
                if (!batch_cap_warned_) {
                    batch_cap_warned_ = true;
                    lx::trace::logger::global().log(lx::trace::level::warn, "scene.draw_list",
                                                    "batch cap reached");
                }
                overflow_drops_ += sorted_count_ - i;
                break;
            }
            ensure_batch_capacity(batch_count_ + 1);
            current = &batches_[batch_count_++];
            *current = {r.kind, r.page, i, 0, {}};
        }
        ++current->count;
        current->bounds = lx_scene_union(current->bounds, sorted_bounds[i]);
    }

    if (lx::trace::timeline_enabled()) {
        lx::trace::timeline_counter("scene.primitives", primitive_count_);
        lx::trace::timeline_counter("scene.refs", sorted_count_);
        lx::trace::timeline_counter("scene.batches", batch_count_);
    }
}

std::size_t lx::scene::draw_list::encode_instances(std::span<std::byte> ring,
                                                   std::span<std::uint32_t> batch_offsets,
                                                   std::size_t align) const {
    const std::size_t mask = align > 1 ? align - 1 : 0;
    std::size_t off = 0;
    for (unsigned b = 0; b < batch_count_; ++b) {
        const lx::gfx::batch& run = batches_[b];
        std::size_t stride = 0;
        switch (run.kind) {
        case lx::gfx::primitive_kind::quad:
            stride = sizeof(lx::gfx::quad_instance);
            break;
        case lx::gfx::primitive_kind::shadow:
            stride = sizeof(lx::gfx::shadow_instance);
            break;
        case lx::gfx::primitive_kind::path:
        case lx::gfx::primitive_kind::mono_sprite:
        case lx::gfx::primitive_kind::poly_sprite:
            stride = sizeof(lx::gfx::sprite_instance);
            break;
        case lx::gfx::primitive_kind::underline:
            stride = sizeof(lx::gfx::underline_instance);
            break;
        case lx::gfx::primitive_kind::surface:
            break;
        }
        if (stride != 0)
            off = (off + mask) & ~mask;
        if (b < batch_offsets.size())
            batch_offsets[b] = static_cast<std::uint32_t>(off);
        if (stride == 0)
            continue;
        const std::size_t bytes = stride * run.count;
        if (off + bytes <= ring.size()) {
            std::byte* dst = ring.data() + off;
            const lx::gfx::primitive_ref* refs = stream_ + run.first;
            switch (run.kind) {
            case lx::gfx::primitive_kind::quad:
                lx_scene_gather(dst, quads_.items, refs, run.count);
                break;
            case lx::gfx::primitive_kind::shadow:
                lx_scene_gather(dst, shadows_.items, refs, run.count);
                break;
            case lx::gfx::primitive_kind::underline:
                lx_scene_gather(dst, underlines_.items, refs, run.count);
                break;
            default:
                lx_scene_gather(dst, sprites_.items, refs, run.count);
                break;
            }
        }
        off += bytes;
    }
    return off;
}

namespace {

/// `gfx::transform_rect` for an x, y, w, h float rect (an empty one stays as it is: a clip
/// of zero size is no mask).
void lx_scene_transform_rectf(float* r, lx::buffer_transform t, int width, int height) {
    if (!(r[2] > 0.f) || !(r[3] > 0.f))
        return;
    const lx::gfx::source_basis b = lx::gfx::source_basis_for(t);
    const bool turned = b.dx[0] == 0.f;
    const double fw = turned ? height : width;
    const double fh = turned ? width : height;
    const auto map = [&](double x, double y, double& ox, double& oy) {
        ox = b.origin[0] * fw + b.dx[0] * x + b.dy[0] * y;
        oy = b.origin[1] * fh + b.dx[1] * x + b.dy[1] * y;
    };
    double ax = 0, ay = 0, bx = 0, by = 0;
    map(r[0], r[1], ax, ay);
    map(static_cast<double>(r[0]) + r[2], static_cast<double>(r[1]) + r[3], bx, by);
    r[0] = static_cast<float>(ax < bx ? ax : bx);
    r[1] = static_cast<float>(ay < by ? ay : by);
    r[2] = static_cast<float>(ax < bx ? bx - ax : ax - bx);
    r[3] = static_cast<float>(ay < by ? by - ay : ay - by);
}

/// Where each content-space corner (top-left, top-right, bottom-right, bottom-left) of a
/// box lands under `t`, as an output corner index in the same order.
void lx_scene_corner_map(lx::buffer_transform t, unsigned* to) {
    const lx::gfx::source_basis b = lx::gfx::source_basis_for(t);
    // The unit square's corners through the linear part; which output corner each is follows
    // from its signs relative to the square's mapped centre.
    static constexpr float cx[4] = {0.f, 1.f, 1.f, 0.f};
    static constexpr float cy[4] = {0.f, 0.f, 1.f, 1.f};
    for (unsigned k = 0; k < 4; ++k) {
        const float ox = b.dx[0] * (cx[k] - 0.5f) + b.dy[0] * (cy[k] - 0.5f);
        const float oy = b.dx[1] * (cx[k] - 0.5f) + b.dy[1] * (cy[k] - 0.5f);
        const bool right = ox > 0.f;
        const bool bottom = oy > 0.f;
        to[k] = !bottom ? (right ? 1u : 0u) : (right ? 2u : 3u);
    }
}

/// Edge `e` (top, right, bottom, left) runs from corner `e` to corner `e + 1`: the output
/// edge between the corners those land on.
[[nodiscard]] unsigned lx_scene_edge_of(unsigned a, unsigned b) {
    const unsigned lo = a < b ? a : b;
    const unsigned hi = a < b ? b : a;
    if (lo == 0 && hi == 3)
        return 3u; // bottom-left → top-left: the left edge
    return lo;     // {0,1} top, {1,2} right, {2,3} bottom
}

} // namespace

void lx::scene::draw_list::transform_primitives(lx::buffer_transform t, lx::size2i content) {
    if (t == lx::buffer_transform::normal || primitive_count_ == 0 || content.width <= 0 ||
        content.height <= 0)
        return;
    const int w = content.width;
    const int h = content.height;
    unsigned corner[4]{};
    lx_scene_corner_map(t, corner);
    unsigned edge[4]{};
    for (unsigned e = 0; e < 4; ++e)
        edge[e] = lx_scene_edge_of(corner[e], corner[(e + 1) % 4]);
    const lx::gfx::source_basis b = lx::gfx::source_basis_for(t);
    const auto permute = [](std::uint16_t* v, const unsigned* to) {
        std::uint16_t in[4] = {v[0], v[1], v[2], v[3]};
        for (unsigned k = 0; k < 4; ++k)
            v[to[k]] = in[k];
    };
    for (unsigned i = 0; i < quads_.count; ++i) {
        lx::gfx::quad_instance& q = quads_.items[i];
        lx_scene_transform_rectf(q.bounds, t, w, h);
        lx_scene_transform_rectf(q.clip, t, w, h);
        permute(q.radii, corner);
        permute(q.border_widths, edge);
        bool gradient = false;
        for (unsigned k = 0; k < 4; ++k)
            gradient = gradient || q.background[k] != q.background_end[k];
        if (gradient) {
            // The direction (cos θ, sin θ) through the linear part of the map.
            const float rad = lx::gfx::from_half(q.gradient_angle) * 3.14159265358979323846f / 180.f;
            const float vx = std::cos(rad);
            const float vy = std::sin(rad);
            const float ox = b.dx[0] * vx + b.dy[0] * vy;
            const float oy = b.dx[1] * vx + b.dy[1] * vy;
            float deg = std::atan2(oy, ox) * 180.f / 3.14159265358979323846f;
            if (deg < 0.f)
                deg += 360.f;
            q.gradient_angle = lx::gfx::to_half(deg);
        }
    }
    for (unsigned i = 0; i < shadows_.count; ++i) {
        lx::gfx::shadow_instance& sh = shadows_.items[i];
        lx_scene_transform_rectf(sh.bounds, t, w, h);
        lx_scene_transform_rectf(sh.clip, t, w, h);
        permute(sh.radii, corner);
    }
    // A sprite's tile is drawn turned with its box: the renderers sample it through the
    // transform a blit of a normal buffer would take on this output (flags bits 1-3).
    const auto sprite_xf = static_cast<std::uint8_t>(
        static_cast<unsigned>(lx::gfx::compose_output_transform(lx::buffer_transform::normal, t)) << 1u);
    for (unsigned i = 0; i < sprites_.count; ++i) {
        lx_scene_transform_rectf(sprites_.items[i].bounds, t, w, h);
        lx_scene_transform_rectf(sprites_.items[i].clip, t, w, h);
        sprites_.items[i].flags = static_cast<std::uint8_t>((sprites_.items[i].flags & 1u) | sprite_xf);
    }
    for (unsigned i = 0; i < underlines_.count; ++i) {
        lx_scene_transform_rectf(underlines_.items[i].bounds, t, w, h);
        lx_scene_transform_rectf(underlines_.items[i].clip, t, w, h);
    }
    for (unsigned i = 0; i < primitive_count_; ++i)
        ref_bounds_[i] = lx::gfx::transform_rect(ref_bounds_[i], t, w, h);
}

unsigned lx::scene::draw_list::size() const { return count_; }
unsigned lx::scene::draw_list::capacity() const { return capacity_; }
unsigned lx::scene::draw_list::overflow_drops() const { return overflow_drops_; }
const lx::scene::draw_command* lx::scene::draw_list::data() const { return commands_; }
lx::scene::draw_command* lx::scene::draw_list::data() { return commands_; }

lx::scene::snapshot_buffer::snapshot_buffer(snapshot_config config) : config_{config} {
    for (unsigned i = 0; i < k_snapshot_slot_count; ++i)
        buffers_[i].slot_ = i;
}

void lx::scene::snapshot_buffer::set_config(snapshot_config config) { config_ = config; }
lx::scene::snapshot_config lx::scene::snapshot_buffer::config() const { return config_; }

unsigned lx::scene::snapshot_buffer::find_free_slot() const {
    const unsigned current = read_index_.load(std::memory_order_acquire);
    for (unsigned i = 1; i < k_snapshot_slot_count; ++i) {
        const unsigned idx = (current + i) % k_snapshot_slot_count;
        if (readers_[idx].load(std::memory_order_acquire) == 0)
            return idx;
    }
    // Every other slot is leased to a reader. Say so rather than handing back a slot
    // somebody is reading — the caller drops the frame instead of tearing it.
    return k_no_slot;
}

bool lx::scene::snapshot_buffer::admission_blocked() const {
    if (config_.backpressure == backpressure_policy::triple_buffer)
        return false;
    return in_flight_.load(std::memory_order_acquire) >= config_.max_in_flight;
}

bool lx::scene::snapshot_buffer::would_stall() const { return admission_blocked(); }

lx::scene::write_lease lx::scene::snapshot_buffer::begin_frame() {
    write_lease lease{};

    switch (config_.backpressure) {
    case backpressure_policy::drop_latest:
        if (admission_blocked()) {
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
            return lease;
        }
        break;

    case backpressure_policy::stall_ui: {
        // Actually wait, which is what the policy name promises. Bounded: a wedged
        // consumer must degrade to a dropped frame, not a hung producer thread.
        unsigned spins = 0;
        while (admission_blocked() && spins < k_stall_yield_budget) {
            std::this_thread::yield();
            ++spins;
        }
        if (admission_blocked()) {
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
            return lease;
        }
        break;
    }

    case backpressure_policy::triple_buffer:
        // Admission is slot availability alone — that is what the third slot is for.
        break;
    }

    unsigned slot = find_free_slot();
    if (slot == k_no_slot && config_.backpressure == backpressure_policy::stall_ui) {
        unsigned spins = 0;
        while (slot == k_no_slot && spins < k_stall_yield_budget) {
            std::this_thread::yield();
            slot = find_free_slot();
            ++spins;
        }
    }

    if (slot == k_no_slot) {
        dropped_count_.fetch_add(1, std::memory_order_relaxed);
        return lease;
    }

    lease.owner_ = this;
    lease.slot_ = slot;
    lease.draws_ = &buffers_[slot].draws_;
    return lease;
}

bool lx::scene::snapshot_buffer::publish(write_lease& lease, const lx::gfx::damage_region& damage,
                                         unsigned frame_index, lx::color clear) {
    if (!lease.valid() || lease.owner_ != this)
        return false;

    const unsigned slot = lease.slot_;
    lease = write_lease{}; // a lease publishes at most once

    // Safe without re-validation: `begin_frame` reserved a slot that was neither published
    // nor held, and a reader can only take the slot `read_index_` names — which only this
    // thread moves, and only here.
    buffers_[slot].damage_ = damage;
    // Carry forward what the consumer never saw. Reading the previous slot is safe: only
    // this thread writes slots, and a reader only ever reads them.
    const unsigned prev = read_index_.load(std::memory_order_acquire);
    if (prev != slot && !consumed_[prev].load(std::memory_order_acquire))
        buffers_[slot].damage_.add_region(buffers_[prev].damage_);
    buffers_[slot].frame_index_ = frame_index;
    buffers_[slot].clear_ = clear;
    buffers_[slot].dropped_ = false;
    consumed_[slot].store(false, std::memory_order_release);

    read_index_.store(slot, std::memory_order_release);
    in_flight_.fetch_add(1, std::memory_order_acq_rel);
    publish_count_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

const lx::scene::immutable_frame_snapshot& lx::scene::snapshot_buffer::acquire() {
    for (;;) {
        const unsigned idx = read_index_.load(std::memory_order_acquire);
        readers_[idx].fetch_add(1, std::memory_order_acq_rel);
        if (read_index_.load(std::memory_order_acquire) == idx) {
            consumed_[idx].store(true, std::memory_order_release);
            return buffers_[idx];
        }
        readers_[idx].fetch_sub(1, std::memory_order_acq_rel);
    }
}

void lx::scene::snapshot_buffer::release(const immutable_frame_snapshot& snapshot) {
    const unsigned slot = snapshot.slot_;
    if (slot < k_snapshot_slot_count) {
        unsigned held = readers_[slot].load(std::memory_order_acquire);
        while (held > 0 && !readers_[slot].compare_exchange_weak(held, held - 1,
                                                                 std::memory_order_acq_rel,
                                                                 std::memory_order_acquire)) {
        }
    }

    unsigned prev = in_flight_.load(std::memory_order_acquire);
    while (prev > 0 &&
           !in_flight_.compare_exchange_weak(prev, prev - 1, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
    }
}

unsigned lx::scene::snapshot_buffer::publish_count() const {
    return publish_count_.load(std::memory_order_acquire);
}

unsigned lx::scene::snapshot_buffer::dropped_count() const {
    return dropped_count_.load(std::memory_order_acquire);
}

unsigned lx::scene::snapshot_buffer::in_flight_count() const {
    return in_flight_.load(std::memory_order_acquire);
}
