module;

import lx.foundation;
import lx.runtime;

export module lx.drm:atomic;

export namespace lx::drm {

class kms_device;

struct kms_damage_region {
    lx::rect2i rects[16]{};
    unsigned count = 0;
    bool full_frame = true;
};

struct atomic_commit_request {
    unsigned connector = 0;
    unsigned framebuffer_id = 0;
    kms_damage_region damage{};
    bool request_page_flip = true;
    bool async = false;
    /// Optional explicit-sync input fence (sync_file FD). Borrowed: the caller keeps
    /// ownership and must close it after the commit returns.
    int in_fence_fd = -1;
    /// When set, receives an owned sync_file FD for the commit completion fence, or -1.
    /// The caller must close it.
    int* out_fence_fd_ptr = nullptr;
};

struct page_flip_event {
    unsigned sequence = 0;
    lx::runtime::clock_time timestamp_ns = 0;
    kms_damage_region applied_damage{};
    bool presented = false;
};

using page_flip_handler = void (*)(page_flip_event event, void* user_data);

class kms_atomic_commit {
public:
    explicit kms_atomic_commit(kms_device& device);

    [[nodiscard]] lx::result<void> commit(const atomic_commit_request& request);
    void set_page_flip_handler(page_flip_handler handler, void* user_data = nullptr);
    void dispatch_events();

    [[nodiscard]] bool supports_atomic_damage() const;
    void build_damage_blob(const kms_damage_region& region);
    void set_framebuffer(unsigned fb_id);

private:
    [[nodiscard]] lx::result<void> ensure_props();
    void emit_flip(const atomic_commit_request& request, bool presented);

    kms_device* device_ = nullptr;
    page_flip_handler flip_handler_ = nullptr;
    void* flip_user_ = nullptr;
    bool atomic_damage_ = false;
    unsigned flip_sequence_ = 0;
    unsigned framebuffer_id_ = 0;
    unsigned damage_blob_id_ = 0;

    unsigned prop_crtc_id_ = 0;
    unsigned prop_fb_id_ = 0;
    unsigned prop_src_x_ = 0;
    unsigned prop_src_y_ = 0;
    unsigned prop_src_w_ = 0;
    unsigned prop_src_h_ = 0;
    unsigned prop_crtc_x_ = 0;
    unsigned prop_crtc_y_ = 0;
    unsigned prop_crtc_w_ = 0;
    unsigned prop_crtc_h_ = 0;
    unsigned prop_fb_damage_clips_ = 0;
    unsigned prop_in_fence_fd_ = 0;
    unsigned prop_out_fence_ptr_ = 0;
    bool props_ready_ = false;
};

} // namespace lx::drm
