#pragma once

import std;
import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.session;

namespace lumen::dm {

struct kms_host_callbacks {
    // Centered form bounds in display coordinates. The borrowed paint target is local
    // to the form and writable only for the duration of paint().
    lx::result<void> (*configure)(void*, lx::rect2i form) = nullptr;
    lx::result<void> (*paint)(void*, const lx::gfx::pixel_surface&) = nullptr;
    void* user = nullptr;
    std::uint32_t background_xrgb = 0x00282c34;
    // Called on the loop after a current primary commit presents. Record only; do
    // not reenter the host from a KMS callback. Zero revisions are not reported.
    void (*presented)(void*, std::uint64_t form_revision) = nullptr;
};

// One output on the greeter's UI loop. The loop, session and callbacks outlive the host.
class kms_host {
public:
    kms_host(lx::runtime::event_loop&, lx::session::logind_session&, kms_host_callbacks);
    ~kms_host();
    kms_host(const kms_host&) = delete;
    kms_host& operator=(const kms_host&) = delete;

    [[nodiscard]] lx::result<void> open();
    // Call after logind dispatch. Normal pause keeps the DRM file description and the
    // memory named by its outstanding flip events alive. ResumeDevice returns that file.
    void suspend();
    [[nodiscard]] lx::result<void> resume();
    // Retain the visible primary only if it presented the caller's neutral revision.
    // Zero requests ordinary console restore.
    void close(std::uint64_t handoff_revision = 0);

    [[nodiscard]] bool owns_device(unsigned major, unsigned minor) const;
    void device_gone(unsigned major, unsigned minor);
    // Nonzero revisions identify the UI state that this repaint will render. A
    // redraw without a revision keeps the latest requested revision.
    void request_frame(std::uint64_t form_revision = 0);
    void move_pointer(int x, int y);
    [[nodiscard]] lx::rect2i form_rect() const;
    [[nodiscard]] lx::rect2i display_rect() const;
    [[nodiscard]] int scale() const;
    // A content flip for the current display epoch actually reached the display.
    [[nodiscard]] bool content_visible() const;

private:
    struct state;
    std::unique_ptr<state> state_;
};

} // namespace lumen::dm
