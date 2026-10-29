module;

// color-management-v1 (staging): wp_color_manager_v1 and its objects.
//   - Image descriptions are parametric (`gfx::color_description`), immutable and copied by
//     value: a surface, a pending state and every description object hold their own copy,
//     so nothing here is reference counted. Every ready description has a fresh 64-bit
//     identity (never recycled); the outputs' share one.
//   - What is offered follows the composite backend (`p0_protocol_context::color_management`,
//     every backend draws the same transform: lx.gfx color.cppm). With it: the perceptual
//     intent; features parametric, set_primaries, set_tf_power, set_luminances,
//     set_mastering_display_primaries and (version 3) windows_bt2100; transfer functions
//     gamma22, ext_linear, st2084_pq and compound_power_2_4 (version 1: srgb instead, which
//     later versions deprecate); all ten named primaries of version 1. Without it (the
//     headless path) only the intent: clients can still read the outputs' description.
//     Not offered: ICC profiles (no ICC parser), Windows-scRGB (every client buffer format
//     Lumen accepts is 8-bit, so there is no buffer that could carry scRGB's range), HLG,
//     BT.1886, extended target volumes and intents other than perceptual.
//   - Every output's description is SDR sRGB: BT.709 primaries, the piecewise sRGB encoding
//     the composite writes (compound_power_2_4; `srgb` to version 1), luminances 0.2 / 80 /
//     80 cd/m². It is what a surface's feedback prefers, and it never changes, so
//     image_description_changed and preferred_changed are never sent.
//   - set_image_description is double-buffered (wl_surface.commit, the surface's cached
//     state for synchronized subsurfaces); the applied description becomes the surface's
//     `gfx::color_transform` in the scene (wire_surface.cpp).
//
// Every error:
//   wp_color_manager_v1.surface_exists                    a second get_surface for a surface
//   wp_color_manager_v1.unsupported_feature               create_icc_creator,
//                                                         create_windows_scrgb, and what the
//                                                         backend does not draw
//   wp_color_management_surface_v1.inert                  set/unset after the wl_surface went
//   wp_color_management_surface_v1.image_description      a description that is not ready
//   wp_color_management_surface_v1.render_intent          an intent other than perceptual
//   wp_color_management_surface_feedback_v1.inert         get_preferred* after the surface went
//   wp_color_management_surface_feedback_v1.unsupported_feature
//                                                         get_preferred_parametric without
//                                                         the parametric feature
//   wp_image_description_creator_params_v1.incomplete_set create without tf or primaries
//   wp_image_description_creator_params_v1.already_set    any property set twice
//   wp_image_description_creator_params_v1.invalid_tf     a name not offered, an exponent
//                                                         outside [1, 10]
//   wp_image_description_creator_params_v1.invalid_primaries_named
//   wp_image_description_creator_params_v1.invalid_luminance
//                                                         max or ref ≤ min; mastering max ≤
//                                                         min; max_fall > max_cll; version 1:
//                                                         max_cll / max_fall outside the
//                                                         mastering range
//   wp_image_description_v1.not_ready                     get_information before ready
//   wp_image_description_v1.no_information                get_information on a client's own
// Degenerate primaries (no matrix) or luminances are not an error: the description fails
// with cause `unsupported`.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_COLOR_MANAGEMENT_V1)
#include "color-management-v1-server-protocol.h"
#define LUMEN_HAS_COLOR_MANAGEMENT 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.gfx;
import lx.trace;
import :surface;
import :output;
import :wire;

#if defined(LUMEN_HAS_COLOR_MANAGEMENT)

namespace lx::compositor::p0 {

namespace {

constexpr lx::trace::category log_color{"compositor.color"};

/// Identities: 1 is the outputs' description, every other ready description takes the next.
constexpr std::uint64_t k_output_identity = 1;
std::uint64_t g_next_identity = 2;

/// The description every output has (and every surface's feedback prefers).
constexpr lx::gfx::color_description k_output_description = lx::gfx::k_srgb_description;

bool offered() { return g_p0_ctx && g_p0_ctx->color_management; }

// ── wp_image_description_v1 / _info_v1 ─────────────────────────────────────────

struct description_obj {
    bool ready = false;
    /// get_information is allowed (an output's or a preferred description).
    bool info = false;
    lx::gfx::color_description d{};
};

description_obj* description_from(wl_resource* r) {
    return r ? static_cast<description_obj*>(wl_resource_get_user_data(r)) : nullptr;
}

int coord(float v) { return static_cast<int>(std::lround(static_cast<double>(v) * 1000000.0)); }

void send_primaries(wl_resource* info, const lx::gfx::chromaticities& c, bool target) {
    (target ? wp_image_description_info_v1_send_target_primaries
            : wp_image_description_info_v1_send_primaries)(
        info, coord(c.rx), coord(c.ry), coord(c.gx), coord(c.gy), coord(c.bx), coord(c.by),
        coord(c.wx), coord(c.wy));
}

/// The info events of an output's or a preferred description (`k_output_description`, or any
/// with named sRGB primaries and a named transfer function), then done.
void send_information(wl_resource* info, const lx::gfx::color_description& d) {
    send_primaries(info, d.primaries, false);
    if (d.primaries == lx::gfx::primaries_of(lx::gfx::named_primaries::srgb))
        wp_image_description_info_v1_send_primaries_named(info, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
    switch (d.tf) {
    case lx::gfx::color_tf::power:
        wp_image_description_info_v1_send_tf_power(
            info, static_cast<std::uint32_t>(std::lround(static_cast<double>(d.power) * 10000.0)));
        break;
    case lx::gfx::color_tf::ext_linear:
        wp_image_description_info_v1_send_tf_named(info,
                                                   WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR);
        break;
    case lx::gfx::color_tf::st2084_pq:
        wp_image_description_info_v1_send_tf_named(info,
                                                   WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
        break;
    case lx::gfx::color_tf::srgb:
    default:
        // Version 2 deprecates `srgb`: the piecewise encoding is compound_power_2_4 there.
        wp_image_description_info_v1_send_tf_named(
            info, wl_resource_get_version(info) >= 2
                      ? WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4
                      : WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB);
        break;
    }
    const auto lum = [](float v) { return static_cast<std::uint32_t>(std::lround(v)); };
    const auto min_lum = [](float v) {
        return static_cast<std::uint32_t>(std::lround(static_cast<double>(v) * 10000.0));
    };
    wp_image_description_info_v1_send_luminances(info, min_lum(d.min_lum), lum(d.max_lum),
                                                 lum(d.ref_lum));
    send_primaries(info, d.has_target_primaries ? d.target_primaries : d.primaries, true);
    wp_image_description_info_v1_send_target_luminance(
        info, min_lum(d.target_max_lum > 0.f ? d.target_min_lum : d.min_lum),
        lum(d.target_max_lum > 0.f ? d.target_max_lum : d.max_lum));
    if (d.max_cll > 0.f)
        wp_image_description_info_v1_send_target_max_cll(info, lum(d.max_cll));
    if (d.max_fall > 0.f)
        wp_image_description_info_v1_send_target_max_fall(info, lum(d.max_fall));
    wp_image_description_info_v1_send_done(info);
}

void description_destroy_resource(wl_resource* r) { delete description_from(r); }

const struct wp_image_description_v1_interface description_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_information =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id) {
            const description_obj* o = description_from(r);
            if (!o || !o->ready) {
                wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_V1_ERROR_NOT_READY,
                                       "get_information on an image description not ready");
                return;
            }
            if (!o->info) {
                wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_V1_ERROR_NO_INFORMATION,
                                       "get_information is not allowed on this description");
                return;
            }
            wl_resource* info = wl_resource_create(client, &wp_image_description_info_v1_interface,
                                                   wl_resource_get_version(r), id);
            if (!info) {
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource_set_implementation(info, nullptr, nullptr, nullptr);
            send_information(info, o->d);
            // `done` is a destructor event.
            wl_resource_destroy(info);
        },
};

/// A new wp_image_description_v1: ready with `d` (a fresh identity, or `identity`), or —
/// `fail_cause` ≥ 0 — failed with that cause and `why`.
void make_description(wl_client* client, int version, uint32_t id,
                      const lx::gfx::color_description& d, bool info, int fail_cause = -1,
                      const char* why = "", std::uint64_t identity = 0) {
    auto* o = new (std::nothrow) description_obj{};
    wl_resource* r = o ? wl_resource_create(client, &wp_image_description_v1_interface, version, id)
                       : nullptr;
    if (!r) {
        delete o;
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &description_impl, o, &description_destroy_resource);
    if (fail_cause >= 0) {
        wp_image_description_v1_send_failed(r, static_cast<uint32_t>(fail_cause), why);
        return;
    }
    o->ready = true;
    o->info = info;
    o->d = d;
    const std::uint64_t ident = identity ? identity : g_next_identity++;
    if (version >= 2)
        wp_image_description_v1_send_ready2(r, static_cast<uint32_t>(ident >> 32),
                                            static_cast<uint32_t>(ident & 0xFFFFFFFFu));
    else
        wp_image_description_v1_send_ready(r, static_cast<uint32_t>(ident));
}

// ── wp_image_description_creator_params_v1 ─────────────────────────────────────

struct params_obj {
    lx::gfx::color_description d{};
    bool tf = false;
    bool primaries = false;
    bool luminances = false;
    bool mastering_primaries = false;
    bool mastering_luminance = false;
    bool max_cll = false;
    bool max_fall = false;
};

params_obj* params_from(wl_resource* r) {
    return r ? static_cast<params_obj*>(wl_resource_get_user_data(r)) : nullptr;
}

/// Raises already_set when `flag` is set; sets it otherwise.
bool claim(wl_resource* r, bool& flag, const char* what) {
    if (flag) {
        wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET,
                               "%s already set", what);
        return false;
    }
    flag = true;
    return true;
}

lx::gfx::chromaticities chroma(int32_t rx, int32_t ry, int32_t gx, int32_t gy, int32_t bx,
                               int32_t by, int32_t wx, int32_t wy) {
    const auto f = [](int32_t v) { return static_cast<float>(static_cast<double>(v) / 1000000.0); };
    return {f(rx), f(ry), f(gx), f(gy), f(bx), f(by), f(wx), f(wy)};
}

void params_destroy_resource(wl_resource* r) { delete params_from(r); }

const struct wp_image_description_creator_params_v1_interface params_impl = {
    .create =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id) {
            params_obj* p = params_from(r);
            if (!p)
                return;
            if (!p->tf || !p->primaries) {
                wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INCOMPLETE_SET,
                                       "create needs a transfer function and primaries");
                return;
            }
            lx::gfx::color_description& d = p->d;
            if (!p->luminances)
                lx::gfx::set_default_luminances(d);
            else if (d.tf == lx::gfx::color_tf::st2084_pq)
                d.max_lum = d.min_lum + 10000.f; // "max_lum is taken as min_lum + 10000"
            if (p->max_cll && p->max_fall && d.max_fall > d.max_cll) {
                wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
                                       "max_fall exceeds max_cll");
                return;
            }
            const int version = wl_resource_get_version(r);
            if (version == 1 && (p->max_cll || p->max_fall)) {
                // Version 1 only: both inside the mastering luminance range.
                const float lo = p->mastering_luminance ? d.target_min_lum : d.min_lum;
                const float hi = p->mastering_luminance ? d.target_max_lum : d.max_lum;
                const bool bad = (p->max_cll && !(d.max_cll > lo && d.max_cll <= hi)) ||
                                 (p->max_fall && !(d.max_fall > lo && d.max_fall <= hi));
                if (bad) {
                    wl_resource_post_error(
                        r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
                        "max_cll or max_fall outside the mastering luminance range");
                    return;
                }
            }
            const lx::gfx::color_description made = d;
            wl_resource_destroy(r); // `create` is a destructor
            lx::gfx::color_transform t{};
            if (!lx::gfx::make_color_transform(made, t)) {
                make_description(client, version, id, made, false,
                                 WP_IMAGE_DESCRIPTION_V1_CAUSE_UNSUPPORTED,
                                 "degenerate primaries or luminances");
                return;
            }
            make_description(client, version, id, made, false);
        },
    .set_tf_named =
        [](struct wl_client*, struct wl_resource* r, uint32_t tf) {
            params_obj* p = params_from(r);
            if (!p)
                return;
            const bool v1 = wl_resource_get_version(r) < 2;
            lx::gfx::color_tf named{};
            switch (tf) {
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22:
                named = lx::gfx::color_tf::power;
                p->d.power = 2.2f;
                break;
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR:
                named = lx::gfx::color_tf::ext_linear;
                break;
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
                named = lx::gfx::color_tf::st2084_pq;
                break;
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB:
            case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4:
                // Each only where it is offered: `srgb` to version 1, its successor after.
                if ((tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB) == v1) {
                    named = lx::gfx::color_tf::srgb;
                    break;
                }
                [[fallthrough]];
            default:
                wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
                                       "transfer function %u is not supported", tf);
                return;
            }
            if (!claim(r, p->tf, "transfer function"))
                return;
            p->d.tf = named;
        },
    .set_tf_power =
        [](struct wl_client*, struct wl_resource* r, uint32_t eexp) {
            params_obj* p = params_from(r);
            if (!p)
                return;
            if (eexp < 10000u || eexp > 100000u) {
                wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
                                       "power exponent %u / 10000 outside [1, 10]", eexp);
                return;
            }
            if (!claim(r, p->tf, "transfer function"))
                return;
            p->d.tf = lx::gfx::color_tf::power;
            p->d.power = static_cast<float>(static_cast<double>(eexp) / 10000.0);
        },
    .set_primaries_named =
        [](struct wl_client*, struct wl_resource* r, uint32_t primaries) {
            params_obj* p = params_from(r);
            if (!p)
                return;
            if (primaries < WP_COLOR_MANAGER_V1_PRIMARIES_SRGB ||
                primaries > WP_COLOR_MANAGER_V1_PRIMARIES_ADOBE_RGB) {
                wl_resource_post_error(
                    r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_PRIMARIES_NAMED,
                    "named primaries %u are not supported", primaries);
                return;
            }
            if (!claim(r, p->primaries, "primaries"))
                return;
            p->d.primaries =
                lx::gfx::primaries_of(static_cast<lx::gfx::named_primaries>(primaries));
        },
    .set_primaries =
        [](struct wl_client*, struct wl_resource* r, int32_t rx, int32_t ry, int32_t gx,
           int32_t gy, int32_t bx, int32_t by, int32_t wx, int32_t wy) {
            params_obj* p = params_from(r);
            if (!p || !claim(r, p->primaries, "primaries"))
                return;
            p->d.primaries = chroma(rx, ry, gx, gy, bx, by, wx, wy);
        },
    .set_luminances =
        [](struct wl_client*, struct wl_resource* r, uint32_t min_lum, uint32_t max_lum,
           uint32_t reference_lum) {
            params_obj* p = params_from(r);
            if (!p)
                return;
            const double min = static_cast<double>(min_lum) / 10000.0;
            if (!(static_cast<double>(max_lum) > min) || !(static_cast<double>(reference_lum) > min)) {
                wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
                                       "max_lum and reference_lum must exceed min_lum");
                return;
            }
            if (!claim(r, p->luminances, "luminances"))
                return;
            p->d.min_lum = static_cast<float>(min);
            p->d.max_lum = static_cast<float>(max_lum);
            p->d.ref_lum = static_cast<float>(reference_lum);
        },
    .set_mastering_display_primaries =
        [](struct wl_client*, struct wl_resource* r, int32_t rx, int32_t ry, int32_t gx,
           int32_t gy, int32_t bx, int32_t by, int32_t wx, int32_t wy) {
            params_obj* p = params_from(r);
            if (!p || !claim(r, p->mastering_primaries, "mastering display primaries"))
                return;
            p->d.has_target_primaries = true;
            p->d.target_primaries = chroma(rx, ry, gx, gy, bx, by, wx, wy);
        },
    .set_mastering_luminance =
        [](struct wl_client*, struct wl_resource* r, uint32_t min_lum, uint32_t max_lum) {
            params_obj* p = params_from(r);
            if (!p)
                return;
            const double min = static_cast<double>(min_lum) / 10000.0;
            if (!(static_cast<double>(max_lum) > min)) {
                wl_resource_post_error(r, WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
                                       "mastering max_lum must exceed min_lum");
                return;
            }
            if (!claim(r, p->mastering_luminance, "mastering luminance"))
                return;
            p->d.target_min_lum = static_cast<float>(min);
            p->d.target_max_lum = static_cast<float>(max_lum);
        },
    .set_max_cll =
        [](struct wl_client*, struct wl_resource* r, uint32_t max_cll) {
            params_obj* p = params_from(r);
            if (!p || !claim(r, p->max_cll, "max_cll"))
                return;
            p->d.max_cll = static_cast<float>(max_cll);
        },
    .set_max_fall =
        [](struct wl_client*, struct wl_resource* r, uint32_t max_fall) {
            params_obj* p = params_from(r);
            if (!p || !claim(r, p->max_fall, "max_fall"))
                return;
            p->d.max_fall = static_cast<float>(max_fall);
        },
};

// ── wp_color_management_output_v1 ──────────────────────────────────────────────

/// The output it stands for, by slot and id (a slot is reused by a later output; an id
/// never is). `live` false: inert from the start (a wl_output of no output).
struct output_obj {
    unsigned slot = 0;
    lx::output_id id{};
    bool live = false;
};

output_obj* output_obj_from(wl_resource* r) {
    return r ? static_cast<output_obj*>(wl_resource_get_user_data(r)) : nullptr;
}

void output_obj_destroy_resource(wl_resource* r) { delete output_obj_from(r); }

const struct wp_color_management_output_v1_interface output_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_image_description =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id) {
            const output_obj* o = output_obj_from(r);
            const bool there = o && o->live && g_p0_ctx && g_p0_ctx->outputs &&
                               g_p0_ctx->outputs->slot_live(o->slot) &&
                               g_p0_ctx->outputs->slot_state(o->slot).id == o->id;
            if (!there) {
                make_description(client, wl_resource_get_version(r), id, {}, false,
                                 WP_IMAGE_DESCRIPTION_V1_CAUSE_NO_OUTPUT, "the output is gone");
                return;
            }
            make_description(client, wl_resource_get_version(r), id, k_output_description, true,
                             -1, "", k_output_identity);
        },
};

// ── wp_color_management_surface_v1 ─────────────────────────────────────────────

void surface_unset(surface_obj* s) {
    s->pending.color = {};
    s->pending.color_has = false;
    s->pending.color_set = true;
}

void cm_surface_destroy_resource(wl_resource* r) {
    surface_addon* a = addon_from(r);
    // "Destroy … and do the same as unset_image_description" (double-buffered).
    if (a && a->surface)
        surface_unset(a->surface);
    addon_free(a);
}

const struct wp_color_management_surface_v1_interface cm_surface_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .set_image_description =
        [](struct wl_client*, struct wl_resource* r, struct wl_resource* desc,
           uint32_t render_intent) {
            surface_addon* a = addon_from(r);
            if (!a || !a->surface) {
                wl_resource_post_error(r, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT,
                                       "set_image_description after the wl_surface was destroyed");
                return;
            }
            const description_obj* o = description_from(desc);
            if (!o || !o->ready) {
                wl_resource_post_error(r, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_IMAGE_DESCRIPTION,
                                       "the image description is not ready");
                return;
            }
            if (render_intent != WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL) {
                wl_resource_post_error(r, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_RENDER_INTENT,
                                       "rendering intent %u is not supported", render_intent);
                return;
            }
            surface_obj* s = a->surface;
            // Copy semantics: the description object may be destroyed right after.
            s->pending.color = o->d;
            s->pending.color_has = true;
            s->pending.color_set = true;
        },
    .unset_image_description =
        [](struct wl_client*, struct wl_resource* r) {
            surface_addon* a = addon_from(r);
            if (!a || !a->surface) {
                wl_resource_post_error(r, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT,
                                       "unset_image_description after the wl_surface was "
                                       "destroyed");
                return;
            }
            surface_unset(a->surface);
        },
};

// ── wp_color_management_surface_feedback_v1 ────────────────────────────────────

/// The surface by id (ids are never reused): gone means inert.
struct feedback_obj {
    lx::surface_id surface{};
};

feedback_obj* feedback_from(wl_resource* r) {
    return r ? static_cast<feedback_obj*>(wl_resource_get_user_data(r)) : nullptr;
}

void feedback_destroy_resource(wl_resource* r) { delete feedback_from(r); }

void get_preferred(struct wl_client* client, struct wl_resource* r, uint32_t id, bool parametric) {
    const feedback_obj* f = feedback_from(r);
    if (!f || !surface_by_id(f->surface)) {
        wl_resource_post_error(r, WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_ERROR_INERT,
                               "get_preferred after the wl_surface was destroyed");
        return;
    }
    if (parametric && !offered()) {
        wl_resource_post_error(r, WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_ERROR_UNSUPPORTED_FEATURE,
                               "parametric image descriptions are not supported");
        return;
    }
    make_description(client, wl_resource_get_version(r), id, k_output_description, true, -1, "",
                     k_output_identity);
}

const struct wp_color_management_surface_feedback_v1_interface feedback_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_preferred = [](struct wl_client* client, struct wl_resource* r,
                        uint32_t id) { get_preferred(client, r, id, false); },
    .get_preferred_parametric = [](struct wl_client* client, struct wl_resource* r,
                                   uint32_t id) { get_preferred(client, r, id, true); },
};

// ── wp_color_manager_v1 ────────────────────────────────────────────────────────

void unsupported(wl_resource* r, const char* what) {
    wl_resource_post_error(r, WP_COLOR_MANAGER_V1_ERROR_UNSUPPORTED_FEATURE, "%s is not supported",
                           what);
}

const struct wp_color_manager_v1_interface manager_impl = {
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
    .get_output =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id,
           struct wl_resource* output) {
            auto* o = new (std::nothrow) output_obj{};
            wl_resource* res =
                o ? wl_resource_create(client, &wp_color_management_output_v1_interface,
                                       wl_resource_get_version(r), id)
                  : nullptr;
            if (!res) {
                delete o;
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource_set_implementation(res, &output_impl, o, &output_obj_destroy_resource);
            const int slot = output_resource_slot(output);
            if (slot >= 0 && g_p0_ctx && g_p0_ctx->outputs &&
                g_p0_ctx->outputs->slot_live(static_cast<unsigned>(slot))) {
                o->slot = static_cast<unsigned>(slot);
                o->id = g_p0_ctx->outputs->slot_state(o->slot).id;
                o->live = true;
            }
        },
    .get_surface =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id,
           struct wl_resource* surface_res) {
            surface_obj* s = surface_from(surface_res);
            if (!s)
                return;
            if (s->color_mgmt) {
                wl_resource_post_error(r, WP_COLOR_MANAGER_V1_ERROR_SURFACE_EXISTS,
                                       "the surface already has a wp_color_management_surface_v1");
                return;
            }
            (void)addon_create(client, &wp_color_management_surface_v1_interface,
                               wl_resource_get_version(r), id, s, &s->color_mgmt, &cm_surface_impl,
                               cm_surface_destroy_resource);
        },
    .get_surface_feedback =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id,
           struct wl_resource* surface_res) {
            auto* f = new (std::nothrow) feedback_obj{};
            wl_resource* res =
                f ? wl_resource_create(client, &wp_color_management_surface_feedback_v1_interface,
                                       wl_resource_get_version(r), id)
                  : nullptr;
            if (!res) {
                delete f;
                wl_client_post_no_memory(client);
                return;
            }
            if (const surface_obj* s = surface_from(surface_res))
                f->surface = s->id;
            wl_resource_set_implementation(res, &feedback_impl, f, &feedback_destroy_resource);
        },
    .create_icc_creator =
        [](struct wl_client*, struct wl_resource* r, uint32_t) {
            unsupported(r, "create_icc_creator");
        },
    .create_parametric_creator =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id) {
            if (!offered()) {
                unsupported(r, "create_parametric_creator");
                return;
            }
            auto* p = new (std::nothrow) params_obj{};
            wl_resource* res =
                p ? wl_resource_create(client, &wp_image_description_creator_params_v1_interface,
                                       wl_resource_get_version(r), id)
                  : nullptr;
            if (!res) {
                delete p;
                wl_client_post_no_memory(client);
                return;
            }
            wl_resource_set_implementation(res, &params_impl, p, &params_destroy_resource);
        },
    .create_windows_scrgb =
        [](struct wl_client*, struct wl_resource* r, uint32_t) {
            unsupported(r, "create_windows_scrgb");
        },
    .get_image_description =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id, struct wl_resource*) {
            // References come from other protocols, none of which Lumen serves.
            make_description(client, wl_resource_get_version(r), id, {}, false,
                             WP_IMAGE_DESCRIPTION_V1_CAUSE_UNSUPPORTED,
                             "no image description references exist");
        },
    .create_windows_bt2100 =
        [](struct wl_client* client, struct wl_resource* r, uint32_t id) {
            if (!offered()) {
                unsupported(r, "create_windows_bt2100");
                return;
            }
            // BT.2020 primaries, PQ, its default luminances (reference white 203 cd/m²).
            lx::gfx::color_description d{};
            d.tf = lx::gfx::color_tf::st2084_pq;
            d.primaries = lx::gfx::primaries_of(lx::gfx::named_primaries::bt2020);
            lx::gfx::set_default_luminances(d);
            make_description(client, wl_resource_get_version(r), id, d, false);
        },
};

} // namespace

void bind_color_manager(wayland::client_connection& client, unsigned id, int version) {
    if (!g_p0_ctx)
        return;
    auto res = wayland::resource::create(client, &wp_color_manager_v1_interface, version, id);
    if (!res)
        return;
    res.set_implementation(&manager_impl, g_p0_ctx, nullptr);
    auto* r = static_cast<wl_resource*>(res.native());
    wp_color_manager_v1_send_supported_intent(r, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    if (offered()) {
        static constexpr uint32_t k_features[] = {
            WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC,
            WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES,
            WP_COLOR_MANAGER_V1_FEATURE_SET_TF_POWER,
            WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES,
            WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES,
        };
        for (const uint32_t f : k_features)
            wp_color_manager_v1_send_supported_feature(r, f);
        if (version >= 3)
            wp_color_manager_v1_send_supported_feature(r, WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_BT2100);
        wp_color_manager_v1_send_supported_tf_named(r, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
        wp_color_manager_v1_send_supported_tf_named(r,
                                                    WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR);
        wp_color_manager_v1_send_supported_tf_named(r,
                                                    WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
        wp_color_manager_v1_send_supported_tf_named(
            r, version >= 2 ? WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4
                            : WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB);
        for (uint32_t p = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
             p <= WP_COLOR_MANAGER_V1_PRIMARIES_ADOBE_RGB; ++p)
            wp_color_manager_v1_send_supported_primaries_named(r, p);
    }
    wp_color_manager_v1_send_done(r);
    if (log_color.enabled(lx::trace::level::debug)) {
        log_color.debug("color manager bound version={} parametric={}", version,
                        offered() ? 1 : 0);
    }
}

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_COLOR_MANAGEMENT
