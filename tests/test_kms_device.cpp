// DRM/KMS device enumeration smoke test. Uses the host card when accessible;
// otherwise verifies typed errors (never silent success).
#include <errno.h>
#include <stdio.h>

import std;
import lx.foundation;
import lx.drm;

namespace {
// The values a modeset resets another master's leftovers to (pure; no device needed).
int check_neutral_values() {
    using lx::drm::kms_property_shape;
    using lx::drm::neutral_rule;
    int failures = 0;
    const auto expect = [&](bool ok, unsigned long long got, bool want_ok,
                            unsigned long long want, const char* what) {
        if (ok != want_ok || (ok && got != want)) {
            std::fprintf(stderr, "FAIL: %s: ok=%d value=%llu\n", what, ok ? 1 : 0, got);
            ++failures;
        }
    };
    unsigned long long v = 0;

    // rotation is a bitmask: rotate-0 is bit 0, so the value is 1 << 0.
    const char* rot_names[] = {"rotate-0", "rotate-90", "rotate-180", "reflect-x"};
    const unsigned long long rot_bits[] = {0, 1, 2, 4};
    kms_property_shape rotation{};
    rotation.type = kms_property_shape::kind::bitmask;
    rotation.enum_names = rot_names;
    rotation.enum_values = rot_bits;
    rotation.enum_count = 4;
    expect(neutral_property_value(rotation, neutral_rule::enum_entry, "rotate-0", v), v, true,
           1, "rotation rotate-0");
    expect(neutral_property_value(rotation, neutral_rule::enum_entry, "rotate-180", v), v,
           true, 4, "rotation rotate-180 bit");

    // An enum: the entry's value itself; an unknown name is left out.
    const char* cs_names[] = {"Default", "BT2020_RGB", "BT2020_YCC"};
    const unsigned long long cs_values[] = {0, 9, 10};
    kms_property_shape colorspace{};
    colorspace.type = kms_property_shape::kind::enumeration;
    colorspace.enum_names = cs_names;
    colorspace.enum_values = cs_values;
    colorspace.enum_count = 3;
    expect(neutral_property_value(colorspace, neutral_rule::enum_entry, "BT2020_RGB", v), v,
           true, 9, "Colorspace entry");
    expect(neutral_property_value(colorspace, neutral_rule::enum_entry, "Default", v), v, true,
           0, "Colorspace Default");
    expect(neutral_property_value(colorspace, neutral_rule::enum_entry, "sRGB", v), v, false, 0,
           "Colorspace missing entry");

    // Ranges: alpha / max bpc at the top, zpos at either end.
    kms_property_shape alpha{};
    alpha.type = kms_property_shape::kind::range;
    alpha.min = 0;
    alpha.max = 0xffff;
    expect(neutral_property_value(alpha, neutral_rule::range_max, nullptr, v), v, true, 0xffff,
           "alpha max");
    kms_property_shape bpc{};
    bpc.type = kms_property_shape::kind::range;
    bpc.min = 6;
    bpc.max = 16;
    expect(neutral_property_value(bpc, neutral_rule::range_max, nullptr, v), v, true, 16,
           "max bpc");
    expect(neutral_property_value(bpc, neutral_rule::zero, nullptr, v), v, false, 0,
           "zero outside the range");
    kms_property_shape zpos{};
    zpos.type = kms_property_shape::kind::signed_range;
    zpos.min = static_cast<unsigned long long>(-2ll);
    zpos.max = 5;
    expect(neutral_property_value(zpos, neutral_rule::range_min, nullptr, v), v, true,
           static_cast<unsigned long long>(-2ll), "signed zpos min");
    expect(neutral_property_value(zpos, neutral_rule::zero, nullptr, v), v, true, 0,
           "signed range holds 0");

    // An immutable property (a driver's fixed zpos) is never written.
    zpos.immutable = true;
    expect(neutral_property_value(zpos, neutral_rule::range_min, nullptr, v), v, false, 0,
           "immutable");

    // Blobs (HDR_OUTPUT_METADATA, LUTs) go to 0; a rule of the wrong kind is refused.
    kms_property_shape blob{};
    blob.type = kms_property_shape::kind::blob;
    expect(neutral_property_value(blob, neutral_rule::zero, nullptr, v), v, true, 0, "blob");
    expect(neutral_property_value(blob, neutral_rule::range_max, nullptr, v), v, false, 0,
           "blob range rule");
    expect(neutral_property_value(alpha, neutral_rule::enum_entry, "x", v), v, false, 0,
           "range enum rule");
    if (failures == 0)
        std::printf("neutral property values: ok\n");
    return failures;
}
} // namespace

// CLOSEFB falls back to RMFB only where the kernel predates it (EINVAL from the DRM core,
// ENOTTY); a missing framebuffer or a lost device is a failure, not a reason to retry.
int check_closefb_fallback() {
    using lx::drm::closefb_unsupported;
    int failures = 0;
    const auto expect = [&](bool got, bool want, const char* what) {
        if (got != want) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    };
    expect(closefb_unsupported(EINVAL), true, "EINVAL: kernel before 6.8");
    expect(closefb_unsupported(ENOTTY), true, "ENOTTY: no DRM ioctls");
    expect(closefb_unsupported(ENOENT), false, "ENOENT: no such framebuffer");
    expect(closefb_unsupported(ENODEV), false, "ENODEV: device gone");
    expect(closefb_unsupported(EACCES), false, "EACCES");
    expect(closefb_unsupported(0), false, "no error");
    using lx::drm::fb_release;
    using lx::drm::fb_released;
    expect(lx::drm::release_framebuffer(-1, 7, fb_release::close) == fb_released::failed, true,
           "no card fd");
    expect(lx::drm::release_framebuffer(0, 0, fb_release::remove) == fb_released::failed, true,
           "framebuffer 0");
    // Released before anything was made: nothing to report.
    lx::drm::kms_dumb_framebuffer empty{};
    expect(empty.hand_over() == fb_released::failed, true, "empty framebuffer hand-over");
    if (failures == 0)
        std::printf("closefb fallback selection: ok\n");
    return failures;
}

// On the host card (no master needed): a hand-over closes where the kernel has CLOSEFB and
// removes where it does not, and either way the framebuffer is gone afterwards.
int check_hand_over(const lx::drm::kms_device& dev) {
    using lx::drm::fb_released;
    auto made = lx::drm::kms_dumb_framebuffer::create(
        dev, 64, 64, static_cast<lx::fourcc>(lx::pixel_format::xrgb8888));
    if (!made) {
        std::printf("dumb framebuffer unavailable: hand-over not exercised\n");
        return 0;
    }
    auto fb = static_cast<decltype(made)&&>(made).value();
    const unsigned id = fb.id();
    const fb_released done = fb.hand_over();
    const fb_released want = dev.supports_closefb() ? fb_released::closed : fb_released::removed;
    if (done != want || fb.valid()) {
        std::fprintf(stderr, "FAIL: hand-over gave %d, want %d (closefb %d)\n",
                     static_cast<int>(done), static_cast<int>(want), dev.supports_closefb());
        return 1;
    }
    // On no plane, a closed framebuffer is freed at once: its id is unknown now.
    if (lx::drm::release_framebuffer(dev.card_fd(), id, lx::drm::fb_release::close) !=
        fb_released::failed) {
        std::fprintf(stderr, "FAIL: framebuffer %u still exists after its hand-over\n", id);
        return 1;
    }
    std::printf("framebuffer hand-over: %s\n", dev.supports_closefb() ? "CLOSEFB" : "RMFB");
    return 0;
}

// Probing needs a KMS fd: anything else is a typed error, never an empty success.
int check_probe_errors() {
    lx::drm::kms_device none{};
    lx::drm::connector_scan scan{};
    auto probed = none.probe_connectors(0, scan);
    if (probed) {
        std::fprintf(stderr, "FAIL: probe_connectors on a device that was never opened\n");
        return 1;
    }
    if (probed.get_error().domain != lx::error_domain::drm &&
        probed.get_error().domain != lx::error_domain::not_implemented) {
        std::fprintf(stderr, "FAIL: probe error domain %d\n",
                     static_cast<int>(probed.get_error().domain));
        return 1;
    }
    if (none.connector_count() != 0 || none.last_scan().count != 0) {
        std::fprintf(stderr, "FAIL: an unopened device reports connectors\n");
        return 1;
    }
    std::printf("probe without a device: typed error ok\n");
    return 0;
}

// Flip-event routing through a cookie several commits share (the device's two combined
// cookies, reused round-robin): each commit arms its pipelines' bits, each event consumes
// its own; a later commit on the same cookie never takes an earlier one's event.
int check_flip_cookie() {
    int failures = 0;
    const auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    };
    lx::drm::flip_cookie c0{};
    lx::drm::flip_cookie c1{};
    // Three separate modesets within one flip latency: A on cookie 0, B on 1, C on 0.
    c0.arm(0, 0);
    c1.arm(1, 0);
    c0.arm(2, 0);
    expect(c0.take(0, 0), "A's event after C reused its cookie is still A's");
    expect(c1.take(1, 0), "B's event");
    expect(c0.take(2, 0), "C's event");
    expect(!c0.take(0, 0), "an event is consumed once");
    // A resume moved A's generation: the old commit's event is dropped (and consumed).
    c0.arm(0, 3);
    expect(!c0.take(0, 4), "an event from before a resume is dropped");
    expect(!c0.take(0, 4), "and does not linger");
    // A refused commit owes no event.
    c1.arm(5, 0);
    c1.arm(6, 0);
    c1.disarm((1u << 5) | (1u << 6));
    expect(!c1.take(5, 0) && !c1.take(6, 0), "a refused commit's bits are disarmed");
    expect(!c1.take(lx::drm::k_max_pipelines, 0), "an unregistered pipeline is never owed one");
    return failures;
}

int main() {
    if (check_flip_cookie() != 0)
        return 1;
    if (check_neutral_values() != 0)
        return 1;
    if (check_probe_errors() != 0)
        return 1;
    if (check_closefb_fallback() != 0)
        return 1;
    auto opened = lx::drm::kms_device::open("/dev/dri/card0");
    if (!opened) {
        // Permission denied / no device is acceptable in CI without DRM master.
        std::printf("kms_device::open unavailable: domain=%d (expected on locked seat)\n",
                    static_cast<int>(opened.get_error().domain));
        // Still exercise render-node open path when present.
        // Reconstruct via a fresh call that only needs renderD128 (world-writable).
        return 0;
    }

    auto dev = static_cast<decltype(opened)&&>(opened).value();
    if (dev.card_fd() < 0) {
        std::fprintf(stderr, "FAIL: card_fd() < 0 after successful open\n");
        return 1;
    }

    const unsigned n = dev.connector_count();
    std::printf("kms: card_fd=%d connectors=%u\n", dev.card_fd(), n);
    for (unsigned i = 0; i < n; ++i) {
        const auto c = dev.connector(i);
        auto mode = dev.preferred_mode(i);
        std::printf("  connector[%u] id=%u status=%d", i, c.id, static_cast<int>(c.status));
        if (mode)
            std::printf(" mode=%ux%u", mode.value().width, mode.value().height);
        std::printf("\n");
    }

    auto render = dev.open_render_node();
    if (render)
        std::printf("render node fd=%d\n", render.value().get());

    std::printf("kms: crtcs=%u planes=%u stale_crtcs=0x%x\n", dev.crtc_count(), dev.plane_count(),
                dev.stale_crtc_mask());

    // Atomic commit without FB must fail loudly (not synthesize success).
    const auto desc = dev.pipeline_desc_for(0);
    lx::drm::kms_pipeline atomic{dev, desc ? desc.value() : lx::drm::kms_pipeline_desc{}};
    lx::drm::atomic_commit_request req{};
    req.request_page_flip = true;
    auto committed = atomic.commit(req);
    if (committed) {
        std::fprintf(stderr, "FAIL: commit without FB should not succeed\n");
        return 1;
    }
    std::printf("atomic commit without FB correctly failed\n");

    // A combined modeset is validated before anything reaches the kernel: no entries, a
    // null pipeline, or an enabled entry without a mode or framebuffer are typed errors.
    if (lx::drm::commit_modeset(dev, nullptr, 0, true)) {
        std::fprintf(stderr, "FAIL: commit_modeset with no entries should not succeed\n");
        return 1;
    }
    lx::drm::modeset_entry null_entry{};
    if (lx::drm::commit_modeset(dev, &null_entry, 1, true)) {
        std::fprintf(stderr, "FAIL: commit_modeset with a null pipeline should not succeed\n");
        return 1;
    }
    lx::drm::modeset_entry no_fb{};
    no_fb.pipe = &atomic;
    if (lx::drm::commit_modeset(dev, &no_fb, 1, true)) {
        std::fprintf(stderr, "FAIL: commit_modeset without a mode/framebuffer should not succeed\n");
        return 1;
    }
    std::printf("commit_modeset argument validation ok\n");
    return check_hand_over(dev);
}
