module;

// xdg-toplevel-icon-v1's icon store (design-app-compat-capture B.2, I1). Wayland-free: the
// wire (wire_toplevel_icon.cpp) hands it pixels it read from a client's wl_shm buffer at
// set_icon; this keeps them as sealed memfds the shell maps read-only (zlm_policy_bridge_v1
// v6 toplevel_icon_buffer) — the compositor never decodes or draws an icon, and the pixels
// cross the process boundary as an fd, not over the wire.
//
// Why a copy at set_icon: "After making this call, the xdg_toplevel_icon_v1 provided as
// 'icon' can be destroyed by the client without 'toplevel' losing its icon", and nothing
// keeps the client's buffers alive after that. Bounded: at most `k_icon_max_images` images
// per icon, edges up to `k_icon_max_edge`, `k_icon_table_max` icons and
// `k_icon_pixel_budget` bytes of pixels in all; beyond the budget only the image nearest
// the panel's size is kept. Entries are refcounted: a toplevel's applied and staged icons
// hold one reference each. UI affinity. The bodies are in toplevel_icon.cpp (the Clang 23
// partition rule, see session_lock.cpp).

#include <cstdint>

export module lx.compositor:toplevel_icon;

export namespace lx::compositor {

inline constexpr unsigned k_icon_max_images = 4;
inline constexpr int k_icon_max_edge = 256;
inline constexpr unsigned k_icon_table_max = 128;
inline constexpr unsigned long long k_icon_pixel_budget = 16ull << 20;
inline constexpr unsigned k_icon_name_bytes = 256;
/// The edge (buffer pixels) the shell draws most: a 16 px panel icon at scale 2.
inline constexpr int k_icon_preferred_edge = 32;

/// One buffer an icon object offers: its edge in buffer pixels and its scale.
struct icon_candidate {
    int size = 0;
    int scale = 1;
};

/// Which of `n` candidates an icon keeps (indices into `in`, written to `out`, at most
/// `k_icon_max_images`; the count is returned). Edges outside 1…`k_icon_max_edge` are
/// dropped; the rest are ranked by distance from `k_icon_preferred_edge` (ties: the larger
/// edge, then the earlier candidate). When the ranked set does not fit `budget_bytes`
/// (4 bytes per pixel), only the best-ranked candidate that fits is kept — none when even
/// that one does not.
unsigned select_icon_images(const icon_candidate* in, unsigned n, unsigned long long budget_bytes,
                            unsigned* out);

struct icon_image {
    int fd = -1; ///< sealed memfd: size × size premultiplied ARGB8888, stride size × 4
    int size = 0;
    int scale = 1;
};

struct icon_entry {
    char name[k_icon_name_bytes]{};
    icon_image images[k_icon_max_images]{};
    unsigned count = 0;
    unsigned refs = 0;
    bool used = false;
};

class toplevel_icon_table {
public:
    toplevel_icon_table() = default;
    ~toplevel_icon_table();
    toplevel_icon_table(const toplevel_icon_table&) = delete;
    toplevel_icon_table& operator=(const toplevel_icon_table&) = delete;

    /// A new entry holding one reference, named `name` (cut to 255 bytes; null or "" for
    /// none). 0 when the table is full.
    [[nodiscard]] unsigned create(const char* name);
    /// Copies a `size`² image (rows `stride` bytes apart, ARGB8888 premultiplied — or
    /// XRGB8888 with `opaque`, whose alpha becomes 0xff) into a sealed memfd of entry `id`.
    /// False when the entry is full, the edge is out of range, the budget is spent or the
    /// memfd could not be made.
    bool add_image(unsigned id, const void* pixels, unsigned stride, int size, int scale,
                   bool opaque);
    void ref(unsigned id);
    /// Drops a reference; the last one closes the entry's memfds.
    void unref(unsigned id);
    [[nodiscard]] const icon_entry* get(unsigned id) const;
    /// Pixel bytes held by every live entry, and what is left of the budget.
    [[nodiscard]] unsigned long long pixel_bytes() const { return bytes_; }
    [[nodiscard]] unsigned long long budget_left() const {
        return bytes_ < k_icon_pixel_budget ? k_icon_pixel_budget - bytes_ : 0;
    }
    [[nodiscard]] unsigned live() const { return live_; }

private:
    icon_entry entries_[k_icon_table_max]{};
    unsigned long long bytes_ = 0;
    unsigned live_ = 0;
};

} // namespace lx::compositor
