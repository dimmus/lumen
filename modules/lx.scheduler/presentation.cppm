module;

import lx.foundation;
import lx.runtime;
import lx.drm;

export module lx.scheduler:presentation;

export namespace lx::scheduler {

/// wp_presentation / DRM page-flip timing feedback (P1 replaces timer vsync).
struct presentation_feedback {
    lx::runtime::clock_time compositor_clock_ns = 0;
    lx::runtime::clock_time target_present_ns = 0;
    lx::runtime::clock_time actual_present_ns = 0;
    unsigned refresh_nanos = 16'666'667;
    /// The CRTC's vblank counter at the flip (MSC); 0 when there is none (a synthetic
    /// completion).
    unsigned seq = 0;
    lx::rect2i kms_damage{};
    /// The timestamp and `seq` come from the kernel's flip event (hardware vblank); false
    /// for a blocking modeset's completion, sampled in software.
    bool hw_clock = false;
    /// The flip completed (scanout switched to the committed state).
    bool vsync = true;
    /// `actual_present_ns` and `seq` are the CRTC's vblank time and counter, not a time the
    /// driver sampled when it sent the event (a driver without vblank support).
    bool vblank_clock = false;
    bool kms_damage_valid = false;
    /// The commit carried a frame (primary plane), not only the cursor plane.
    bool content = false;
    /// `drm::atomic_commit_request::user_tag` of that commit: the compositor's frame index
    /// of the composite it scanned out (0 = none).
    unsigned tag = 0;
    unsigned crtc_id = 0;
};

using presentation_handler = void (*)(presentation_feedback feedback, void* user_data);

class presentation_tracker {
public:
    void set_handler(presentation_handler handler, void* user_data = nullptr);

    /// Wire from one pipeline's page-flip handler (one tracker per output).
    void bind_page_flip(lx::drm::kms_pipeline& pipeline);

    /// Called when wp_presentation.feedback or DRM page-flip event arrives.
    void on_feedback(presentation_feedback feedback);

    /// Bridge: DRM page_flip_event → presentation_feedback.
    void on_page_flip(lx::drm::page_flip_event event);

    /// Called from render thread after present().
    void on_present_submitted(unsigned frame_index, lx::runtime::clock_time submit_ns);

    [[nodiscard]] presentation_feedback last_feedback() const;
    [[nodiscard]] double measured_fps() const;
    [[nodiscard]] double frame_latency_ms() const;

private:
    static void page_flip_bridge(lx::drm::page_flip_event event, void* user_data);

    presentation_handler handler_ = nullptr;
    void* user_data_ = nullptr;
    presentation_feedback last_{};
    double measured_fps_ = 0.0;
    double frame_latency_ms_ = 0.0;
    lx::runtime::clock_time last_submit_ns_ = 0;
};

} // namespace lx::scheduler


void lx::scheduler::presentation_tracker::set_handler(presentation_handler handler,
                                                      void* user_data) {
    handler_ = handler;
    user_data_ = user_data;
}

void lx::scheduler::presentation_tracker::bind_page_flip(lx::drm::kms_pipeline& pipeline) {
    pipeline.set_page_flip_handler(&presentation_tracker::page_flip_bridge, this);
}

void lx::scheduler::presentation_tracker::page_flip_bridge(lx::drm::page_flip_event event,
                                                               void* user_data) {
    if (user_data)
        static_cast<presentation_tracker*>(user_data)->on_page_flip(event);
}

void lx::scheduler::presentation_tracker::on_page_flip(lx::drm::page_flip_event event) {
    presentation_feedback fb{};
    fb.actual_present_ns = event.timestamp_ns;
    fb.compositor_clock_ns = event.timestamp_ns;
    fb.seq = event.sequence;
    fb.hw_clock = event.hw;
    fb.vblank_clock = event.vblank_timestamp;
    fb.vsync = event.presented;
    fb.content = event.content;
    fb.tag = event.tag;
    fb.crtc_id = event.crtc_id;
    if (!event.applied_damage.full_frame && event.applied_damage.count > 0) {
        fb.kms_damage_valid = true;
        fb.kms_damage = event.applied_damage.rects[0];
    }
    if (last_submit_ns_ > 0 && event.timestamp_ns > last_submit_ns_)
        frame_latency_ms_ = static_cast<double>(event.timestamp_ns - last_submit_ns_) / 1'000'000.0;
    on_feedback(fb);
}

void lx::scheduler::presentation_tracker::on_feedback(presentation_feedback feedback) {
    last_ = feedback;
    if (feedback.refresh_nanos > 0)
        measured_fps_ = 1'000'000'000.0 / static_cast<double>(feedback.refresh_nanos);
    if (handler_) handler_(feedback, user_data_);
}

void lx::scheduler::presentation_tracker::on_present_submitted(unsigned frame_index,
                                                               lx::runtime::clock_time submit_ns) {
    (void)frame_index;
    last_submit_ns_ = submit_ns;
}

lx::scheduler::presentation_feedback lx::scheduler::presentation_tracker::last_feedback() const {
    return last_;
}

double lx::scheduler::presentation_tracker::measured_fps() const { return measured_fps_; }

double lx::scheduler::presentation_tracker::frame_latency_ms() const { return frame_latency_ms_; }
