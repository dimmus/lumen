// Protocol probe for the app smoke: a deterministic xdg_popup and wlr-layer-shell client.
//
// Talks libwayland-client directly. It maps one plain toplevel; a left press inside it opens
// popup 0 with the positioner given on the command line, and a left press inside popup k
// opens popup k+1 (up to --chain), anchored at popup k's right edge. Every event of interest
// is one stdout line, so a smoke test can grep the sequence:
//
//   toplevel configure serial=<n> w=<w> h=<h>
//   pointer enter surface=<toplevel|popup<k>> serial=<n>
//   press surface=<toplevel|popup<k>> serial=<n>          release surface=<…>
//   popup open idx=<k> grab=<serial|->
//   popup configure idx=<k> x=<x> y=<y> w=<w> h=<h> serial=<n>
//   repositioned idx=0 token=<t>
//   popup_done idx=<k>
//   keyboard enter surface=<toplevel|popup<k>>             key code=<evdev> state=<0|1>
//   keyboard keys=[<evdev>,…] (after every enter)          keyboard leave
//   keyboard modifiers depressed=<m> latched=<m> locked=<m> group=<g>
//
//   lumen-probe popup --size WxH --popup WxH --anchor-rect x,y,w,h --anchor N --gravity N
//                     --adjust BITS --offset x,y [--reactive] [--chain N] [--grab]
//                     [--grab-stale] [--reposition] [--exit-after S]
//
// --grab takes an explicit grab with the serial of the press that opened the popup;
// --grab-stale uses the serial of the `enter` before that press instead (a compositor that
// validates serials must deny it with popup_done). --reposition moves popup 0's anchor rect
// by (+20,+20) through xdg_popup.reposition 500 ms after it is configured.
//
// Popups are destroyed by the client as soon as popup_done reaches them, topmost first.
//
// Layer mode maps one layer surface instead of the toplevel (a left press on it opens a
// popup of it when --popup is given; that popup grabs with --grab):
//
//   lumen-probe layer --layer background|bottom|top|overlay --anchor top,bottom,left,right
//                     --size WxH --zone Z --margin t,r,b,l --kbd none|exclusive|on_demand
//                     [--edge top|bottom|left|right] [--ns NAME] [--popup WxH] [--grab]
//                     [--unmap-after S] [--exit-after S]
//
//   layer configure serial=<n> w=<w> h=<h>     layer closed     layer unmapped
//   keyboard enter surface=layer                key code=<evdev> state=<0|1>
//   protocol-error interface=<i> code=<c>      (then exit 3)
//
// Selection mode maps the toplevel and owns or reads a selection (clipboard or primary):
//
//   lumen-probe selection [--kind clipboard|primary] [--mimes a,b] [--data STR]
//                         [--set-on press|key|focus|never] [--stale-serial] [--reuse-source]
//                         [--receive MIME] [--exit-after S]
//
// A left press (or a key press, or the keyboard enter) sets the selection to a source
// offering --mimes, quoting that event's serial — or serial 1 with --stale-serial, which a
// compositor that checks serials must refuse; --reuse-source then sets the same source a
// second time (wl_data_device.error.used_source). The source answers `send` with --data.
// With --receive, every offer naming MIME is read and printed.
//
//   selection set serial=<n>              selection cancelled      selection send mime=<m>
//   selection offer mimes=[a,b]           selection offer none
//   selection received mime=<m> data="<d>"
//
// Drag-and-drop source mode: a left press on the toplevel starts a drag (wl_data_device
// .start_drag) of a source offering --mimes with --actions, quoting that press's serial — or
// the pointer enter's serial with --stale-serial, which a compositor must refuse. --icon
// gives it a WxH icon attached at (-W/2, -H/2) after start_drag; --null-source drags with no
// source (a drag inside this client); --second-window maps a second toplevel (app_id
// lumen-probe-2) that is a drop target like the target mode's; --exit-during-drag S exits
// abruptly S seconds after start_drag (client death mid-drag).
//
//   lumen-probe dnd-source [--mimes a,b] [--data STR] [--actions copy,move,ask] [--icon WxH]
//                          [--null-source] [--stale-serial] [--second-window]
//                          [--exit-during-drag S] [--exit-after S]
//
//   dnd source start serial=<n>           dnd source target mime=<m|nil>
//   dnd source action=<a>                 dnd source send mime=<m>
//   dnd source drop_performed             dnd source finished      dnd source cancelled
//
// Drag-and-drop target mode: maps the toplevel; every drag entering it is answered with
// accept(--accept, or NULL for none) and set_actions(--actions, --preferred); a drop is read
// (the accepted type) and finished, unless --no-finish.
//
//   lumen-probe dnd-target [--accept MIME|none] [--actions copy,move,ask] [--preferred A]
//                          [--no-finish] [--exit-after S]
//
//   dnd enter surface=<toplevel|second> x=<x> y=<y> offer=<id|nil> mimes=[a,b] source_actions=<a>
//   dnd motion x=<x> y=<y>                dnd action=<a>           dnd leave       dnd drop
//   dnd received data="<d>"               dnd finish
//
// Outputs mode: popup mode (a left press opens popup 0), plus the outputs. It binds wl_output
// (v4) and zxdg_output_manager_v1 (v3) — at once, or with --late S that many seconds after
// the toplevel's first configure — and reports what they say and every wl_surface.enter /
// leave of its toplevel and popups. --unmap-after S unmaps the toplevel (null attach).
//
//   lumen-probe outputs [--late S] [--unmap-after S] [popup options]
//
//   wl_output bind version=<v>
//   wl_output name=<n> mode=<w>x<h> refresh=<mHz> scale=<s> done=<k>
//   xdg_output position=<x>,<y> size=<w>x<h> name=<n> description=<d>
//   surface enter surface=<toplevel|popup<k>> output=<n>   surface leave …   toplevel unmapped
//   wl_output added global=<g>          wl_output removed name=<n>
//
// Every wl_output is bound (one line group each, `done` counted per output), and outputs
// advertised later are bound as they come; enter/leave name the output they refer to.
// --output NAME makes layer, power and lock act on that output (and --fullscreen ask for it:
// `toplevel fullscreen=<0|1> w=<w> h=<h>` per configure; --unset-fullscreen-after S leaves
// it); lock --all-outputs makes one lock surface per output.
//
// Liveness, in any mode with a toplevel: --ignore-ping-for S leaves xdg_wm_base.ping
// unanswered for S seconds after the first configure, then answers the newest one held
// (a client that catches up); --freeze S stops dispatching for S seconds after the first
// frame; --ssd asks for a server-side frame (xdg-decoration) and --title T sets the title.
//
//   ping serial=<n> ignored               pong serial=<n> [late]
//   freeze s=<S>                          thaw
//
// Activation issuer mode: maps the toplevel; a left press on it requests an xdg-activation
// token quoting that press's serial (none with --no-serial) and writes it to --token-file
// (atomically: written beside it, then renamed).
//
//   lumen-probe activation-issuer --token-file F [--no-serial] [--exit-after S]
//
//   activation request serial=<n|none>   activation token=<first 8 chars>
//
// Activation target mode: maps the toplevel and presents a token for it — --token STR, or
// the contents of --token-file F once that file exists (polled). --early presents it before
// the initial commit (the window is not mapped yet; F must exist at start). Every configure
// also prints whether it carries the activated state.
//
//   lumen-probe activation-target (--token STR | --token-file F) [--early] [--exit-after S]
//
//   activation activate token=<first 8 chars> mapped=<0|1>
//   toplevel activated=<0|1>              keyboard enter surface=toplevel
//
// Scale mode maps the toplevel through wp_viewporter and wp_fractional_scale_v1, the way
// foot and GTK 4 do: it renders a one-pixel black/white checker at round(size × scale/120)
// and sets the viewport destination to the logical size, so every pixel on screen is a
// texel (any resampling turns the checker grey). It redraws when the preferred scale
// changes. --integer renders at wl_surface.preferred_buffer_scale with set_buffer_scale
// instead (a client without fractional-scale). Viewport tests: --buffer WxH draws a buffer
// of four coloured quadrants (red, green / blue, yellow, each with a 1 px white frame) at
// --buffer-scale N and --transform T, cropped to --crop x,y,w,h (surface units, fractions
// allowed) and scaled to --dst WxH (default: the configured size).
//
//   lumen-probe scale [--integer] [--buffer WxH] [--buffer-scale N] [--transform T]
//                     [--crop x,y,w,h] [--dst WxH] [popup and toplevel options]
//
//   preferred-scale <120ths>              preferred-buffer-scale <n>
//   buffer <w>x<h> dst <w>x<h> scale120=<n> buffer-scale=<n>
//   pointer position x=<sx> y=<sy>        (surface-local, on enter and motion)
//
// Presentation mode maps the toplevel and animates it, paced by frame callbacks: every frame
// is a new buffer (a different colour), committed with a wp_presentation_feedback. --frames N
// stops after N frames (0: until --exit-after), waits up to 2 s for the outstanding feedback
// and prints a summary. --burst K commits K updates per frame (all but the last are
// superseded before they can be shown: discarded). --maximize asks to be maximized (an
// opaque window covering the others: theirs stops being presented). It binds wl_output for
// sync_output.
//
//   lumen-probe presentation [--frames N] [--burst K] [--maximize] [toplevel options]
//
//   clock_id <id>
//   presented frame=<n> seq=<msc> t=<s.ns> dt=<ms since the previous> refresh=<ns>
//             flags=0x<kind> sync_output=<k> latency=<ms commit → light>
//   discarded frame=<n> at=<s, CLOCK_MONOTONIC when it arrived>
//   presentation summary frames=<n> presented=<n> discarded=<n> pending=<n>
//             seq_monotonic=<0|1> seq_first=<n> seq_last=<n> refresh=<ns> flags=0x<or>
//             dt_mean=<ms> dt_max=<ms> latency_mean=<ms> sync_output=<n> clock_id=<id>
//             t_monotonic=<0|1>
//
// Idle mode maps nothing: it creates one ext_idle_notification_v1 (--timeout MS, default
// 3000; --input-only uses get_input_idle_notification, v2, which ignores idle inhibitors)
// and prints every event with the seconds since the notification was created and the
// CLOCK_MONOTONIC time (to line up with injected input). --count N exits after N `idled`.
//
//   lumen-probe idle [--timeout MS] [--input-only] [--count N] [--exit-after S]
//
//   idle notification timeout=<ms> input_only=<0|1> version=<v>
//   idled t=<s> at=<s>                    resumed t=<s> at=<s>
//
// Inhibit mode maps the toplevel and puts a zwp_idle_inhibitor_v1 on it (before its first
// commit); --unmap-after S unmaps it (null attach), --destroy-after S destroys the inhibitor.
//
//   lumen-probe inhibit [--unmap-after S] [--destroy-after S] [toplevel options]
//
//   inhibitor created                     inhibitor destroyed
//
// Power mode maps nothing: it binds wl_output and zwlr_output_power_manager_v1 and makes one
// zwlr_output_power_v1. --set off|on is requested once the first `mode` arrived (--set-after
// S later); --then off|on --then-after S a second request. --invalid sends mode 2 (the
// object's invalid_mode protocol error).
//
//   lumen-probe power [--set off|on] [--set-after S] [--then off|on] [--then-after S]
//                     [--invalid] [--exit-after S]
//
//   power mode=<on|off> at=<s>            power failed            power set mode=<on|off>
//
// Lock mode is a minimal screen locker (ext-session-lock-v1): it locks, makes one lock
// surface for the output (bound like power mode's), acks every configure and fills it with
// a solid colour (--colour 0xAARRGGBB, default dark blue). --unlock-after S unlocks S seconds
// after `locked` (unlock_and_destroy, then a roundtrip as the spec asks); --password STR
// unlocks when STR (lower-case letters and digits, US keymap) followed by Enter is typed into
// the lock surface — a test password, never a real one; --crash-after S exits without
// unlocking S seconds after `locked` (the session must stay locked); --no-surfaces locks
// without a lock surface (the compositor must lock after its 1 s wait); --surface-after S
// creates the lock surface S seconds after the lock request; --wrong-size commits a buffer 10
// pixels narrower than configured (dimensions_mismatch). `finished` after `locked` (logind
// Unlock) is answered with unlock_and_destroy, before it with destroy; the probe then exits.
//
//   lumen-probe lock [--unlock-after S] [--password STR] [--crash-after S] [--no-surfaces]
//                    [--surface-after S] [--wrong-size] [--colour 0xAARRGGBB] [--exit-after S]
//
//   lock requested at=<s>                 lock configure WxH serial=<n> at=<s>
//   lock locked at=<s> after=<s since the request>
//   lock finished at=<s> locked=<0|1>     lock unlocked at=<s>       lock crash
//   keyboard enter surface=lock           key code=<evdev> state=<0|1>
//   pointer enter surface=lock …          lock password wrong        lock password accepted
//
// Text-input mode maps the toplevel with a zwp_text_input_v3 (v2 when offered), the way GTK
// and foot use it: on `enter` it enables, sends its text model as the surrounding text, a
// content type and the cursor rectangle --cursor x,y,w,h (surface-local, default 20,40,2,18),
// commits, then commits the wl_surface (v2 applies the rectangle with it). Every `done` applies
// preedit / deletion / commit string to the model in the spec's order and, when the text
// changed and the serial matches, sends the new surrounding text. --disable-after S disables
// (and --enable-after S enables again, counted from the disable); --no-enable never enables.
//
//   lumen-probe text-input [--cursor x,y,w,h] [--disable-after S] [--enable-after S]
//                          [--no-enable] [toplevel options]
//
//   ti enter surface=toplevel             ti leave             ti enable commits=<n>
//   ti preedit "<s>" <begin> <end>        ti commit "<s>"      ti delete <before> <after>
//   ti done serial=<n> commits=<n> text="<model>" preedit="<s>"      ti disable commits=<n>
//
// Input-method mode is a minimal input method (input-method-v2 + virtual-keyboard-v1, the way
// fcitx5 and ibus work): it binds the seat's input method and a virtual keyboard, grabs the
// keyboard on activate (released on deactivate, as fcitx5 does), forwards the grab's keymap
// to its virtual keyboard verbatim, and composes: `a` adds "α" to the preedit, Backspace
// removes the last preedit character, space commits the preedit; every other key (and every
// modifier change) is forwarded through the virtual keyboard. A popup surface of --popup WxH
// (default 120x40) shows while active. Commits quote the number of `done` events received.
//
//   lumen-probe im [--popup WxH] [--no-popup] [--exit-after S]
//
//   im bound                              im unavailable
//   im activate                           im deactivate
//   im done n=<dones> active=<0|1> surrounding="<s>" cursor=<n> purpose=<p> hint=<h>
//   im grab keymap size=<n>               im key code=<evdev> state=<0|1>
//   im preedit "<s>"                      im commit "<s>"       im forwarded code=<c> state=<s>
//   im popup rect x=<x> y=<y> w=<w> h=<h>  (zwp_input_popup_surface_v2.text_input_rectangle)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#if defined(LUMEN_PROTOCOL_XDG_OUTPUT_UNSTABLE_V1)
#include "xdg-output-unstable-v1-client-protocol.h"
#define LUMEN_PROBE_HAS_XDG_OUTPUT 1
#endif
#if defined(LUMEN_PROTOCOL_XDG_DECORATION_UNSTABLE_V1)
#include "xdg-decoration-unstable-v1-client-protocol.h"
#define LUMEN_PROBE_HAS_DECORATION 1
#endif
#if defined(LUMEN_PROTOCOL_PRIMARY_SELECTION_UNSTABLE_V1)
#include "primary-selection-unstable-v1-client-protocol.h"
#define LUMEN_PROBE_HAS_PRIMARY_SELECTION 1
#endif
#if defined(LUMEN_PROTOCOL_XDG_ACTIVATION_V1)
#include "xdg-activation-v1-client-protocol.h"
#define LUMEN_PROBE_HAS_ACTIVATION 1
#endif
#if defined(LUMEN_PROTOCOL_VIEWPORTER) && defined(LUMEN_PROTOCOL_FRACTIONAL_SCALE_V1)
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#define LUMEN_PROBE_HAS_SCALE 1
#endif
#if defined(LUMEN_PROTOCOL_PRESENTATION_TIME)
#include "presentation-time-client-protocol.h"
#define LUMEN_PROBE_HAS_PRESENTATION 1
#endif
#if defined(LUMEN_PROTOCOL_EXT_IDLE_NOTIFY_V1)
#include "ext-idle-notify-v1-client-protocol.h"
#define LUMEN_PROBE_HAS_IDLE_NOTIFY 1
#endif
#if defined(LUMEN_PROTOCOL_IDLE_INHIBIT_UNSTABLE_V1)
#include "idle-inhibit-unstable-v1-client-protocol.h"
#define LUMEN_PROBE_HAS_IDLE_INHIBIT 1
#endif
#if defined(LUMEN_PROTOCOL_WLR_OUTPUT_POWER_MANAGEMENT_UNSTABLE_V1)
#include "wlr-output-power-management-unstable-v1-client-protocol.h"
#define LUMEN_PROBE_HAS_OUTPUT_POWER 1
#endif
#if defined(LUMEN_PROTOCOL_EXT_SESSION_LOCK_V1)
#include "ext-session-lock-v1-client-protocol.h"
#define LUMEN_PROBE_HAS_SESSION_LOCK 1
#endif
#if defined(LUMEN_PROTOCOL_TEXT_INPUT_UNSTABLE_V3) && \
    defined(LUMEN_PROTOCOL_INPUT_METHOD_UNSTABLE_V2) && \
    defined(LUMEN_PROTOCOL_VIRTUAL_KEYBOARD_UNSTABLE_V1)
#include "text-input-unstable-v3-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#define LUMEN_PROBE_HAS_IME 1
#endif
#if defined(LUMEN_PROBE_HAS_LAYER_SHELL)
// wayland-scanner names an argument `namespace`, a C++ keyword (Wayfire renames it the same
// way).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define namespace name_space
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#pragma clang diagnostic pop
#endif

namespace {

constexpr int k_max_popups = 8;

struct options {
    int width = 400, height = 300;
    int popup_w = 200, popup_h = 100;
    int ar_x = 10, ar_y = 10, ar_w = 50, ar_h = 20;
    unsigned anchor = 6, gravity = 8, adjust = 0;
    int off_x = 0, off_y = 0;
    bool reactive = false;
    int chain = 1;
    bool grab = false;
    bool grab_stale = false;
    bool reposition = false;
    double exit_after = 30.0;
    // layer mode
    bool layer_mode = false;
    unsigned layer = 2;
    unsigned layer_anchor = 0;
    int zone = 0;
    int margin[4]{}; // t, r, b, l
    unsigned kbd = 0;
    unsigned edge = 0;
    const char* ns = "lumen-probe";
    bool layer_popup = false;
    double unmap_after = 0.0;
    // selection mode
    bool selection_mode = false;
    bool primary = false;
    const char* mimes = "text/plain;charset=utf-8,text/plain,UTF8_STRING";
    const char* data = "lumen-probe";
    enum class set_on { press, key, focus, never } trigger = set_on::press;
    bool stale_serial = false;
    bool reuse_source = false;
    const char* receive = nullptr;
    // drag-and-drop modes
    bool dnd_source_mode = false;
    bool dnd_target_mode = false;
    unsigned dnd_actions = 3;     // source actions (dnd-source), offer actions (dnd-target)
    unsigned dnd_preferred = 2;
    const char* dnd_accept = "text/plain";
    bool no_finish = false;
    int icon_w = 0, icon_h = 0;
    bool null_source = false;
    bool second_window = false;
    double exit_during_drag = -1.0;
    // outputs mode
    bool outputs_mode = false;
    double late_output = 0.0;
    // liveness
    double ignore_ping_for = 0.0;
    double freeze = 0.0;
    bool ssd = false;
    const char* title = "lumen-probe";
    // activation modes
    bool activation_issuer = false;
    bool activation_target = false;
    const char* token_file = nullptr;
    const char* token = nullptr;
    bool no_serial = false;
    bool early = false;
    // scale mode
    bool scale_mode = false;
    bool integer_scale = false;
    int vp_buffer_w = 0, vp_buffer_h = 0;
    int buffer_scale = 1;
    int transform = 0;
    bool crop = false;
    double crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
    int dst_w = 0, dst_h = 0;
    // presentation mode
    bool presentation_mode = false;
    int frames = 300;
    int burst = 1;
    bool maximize = false;
    // Multi-output: --output NAME picks the wl_output for layer / power / lock / fullscreen;
    // --all-outputs gives lock mode one surface per output; --fullscreen asks for it
    // (on --output when given) and --unset-fullscreen-after S leaves it again.
    const char* output_name = nullptr;
    bool all_outputs = false;
    bool fullscreen = false;
    double unfullscreen_after = 0.0;
    // idle / inhibit / power modes
    bool idle_mode = false;
    unsigned idle_timeout_ms = 3000;
    bool input_only = false;
    int idle_count = 0;
    bool inhibit_mode = false;
    double destroy_after = 0.0;
    bool power_mode = false;
    int power_set = -1; // -1 none, 0 off, 1 on
    double power_set_after = 0.0;
    int power_then = -1;
    double power_then_after = 0.0;
    bool power_invalid = false;
    // lock mode
    bool lock_mode = false;
    double unlock_after = 0.0;
    double crash_after = 0.0;
    double surface_after = 0.0;
    bool no_surfaces = false;
    bool wrong_size = false;
    const char* password = nullptr;
    uint32_t lock_colour = 0xff1c2a44;
    // text-input / input-method modes
    bool text_input_mode = false;
    bool im_mode = false;
    int cursor_x = 20, cursor_y = 40, cursor_w = 2, cursor_h = 18;
    double disable_after = 0.0;
    double enable_after = 0.0;
    bool no_enable = false;
    bool no_popup = false;
};

struct surface_state {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;
    xdg_popup* popup = nullptr;
    wl_buffer* buffer = nullptr;
    int buffer_w = 0, buffer_h = 0;
    int pending_w = 0, pending_h = 0;
    int pending_x_y[2]{};
    uint32_t colour = 0xff808080;
    int index = -1; // -1: the toplevel
    bool done = false;
    bool configured = false;
};

/// One wl_output global and what it last said.
inline constexpr unsigned k_max_outputs = 8;
struct probe_output {
    uint32_t global = 0;
    uint32_t version = 0;
    wl_output* out = nullptr;
#if defined(LUMEN_PROBE_HAS_XDG_OUTPUT)
    zxdg_output_v1* xo = nullptr;
#endif
    bool gone = false;
    char name[64]{};
    int w = 0, h = 0, refresh = 0, scale = 0;
    unsigned dones = 0;
    int xo_x = 0, xo_y = 0, xo_w = 0, xo_h = 0;
    char xo_name[64]{};
    char xo_description[128]{};
    bool xo_seen = false;
    /// Lock mode with --all-outputs: this output's lock surface.
    surface_state lock_surf{};
#if defined(LUMEN_PROBE_HAS_SESSION_LOCK)
    ext_session_lock_surface_v1* lock_surface = nullptr;
#endif
};

struct app {
    options opt{};
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    surface_state top{};
    surface_state popups[k_max_popups]{};
    int popup_count = 0;
    surface_state* pointer_on = nullptr;
    uint32_t enter_serial = 0;
    bool closed = false;
    double reposition_at = 0.0;
#if defined(LUMEN_PROBE_HAS_LAYER_SHELL)
    zwlr_layer_shell_v1* layer_shell = nullptr;
    zwlr_layer_surface_v1* layer = nullptr;
#endif
    double unmap_at = 0.0;
    bool unmapped = false;
    // selection mode
    wl_data_device_manager* ddm = nullptr;
    wl_data_device* data_device = nullptr;
    wl_data_offer* offer = nullptr;
#if defined(LUMEN_PROBE_HAS_PRIMARY_SELECTION)
    zwp_primary_selection_device_manager_v1* psm = nullptr;
    zwp_primary_selection_device_v1* primary_device = nullptr;
    zwp_primary_selection_offer_v1* primary_offer = nullptr;
#endif
    bool source_set = false;
    // drag-and-drop
    surface_state second{};
    wl_data_source* dnd_source = nullptr;
    bool drag_started = false;
    wl_surface* icon = nullptr;
    wl_buffer* icon_buffer = nullptr;
    wl_data_offer* dnd_offer = nullptr;
    unsigned dnd_source_actions = 0;
    bool dnd_dropped = false;
    bool dnd_accepted = false;
    char dnd_accepted_mime[256]{};
    double exit_at = 0.0;
    const char* recv_label = "selection";
    char offer_mimes[1024]{};
    bool offer_has_receive = false;
    int recv_fd = -1;
    char recv_buf[1024]{};
    size_t recv_len = 0;
    // outputs mode
    wl_registry* registry = nullptr;
    uint32_t output_global = 0;
    uint32_t output_version = 0;
    /// The output the probe acts on (layer, power, lock, fullscreen): --output NAME, else
    /// the first one advertised.
    wl_output* output = nullptr;
    double bind_output_at = 0.0;
    double unfullscreen_at = 0.0;
    /// Every wl_output global advertised, in order (outputs come and go: hotplug).
    probe_output outs[k_max_outputs]{};
    unsigned out_count = 0;
#if defined(LUMEN_PROBE_HAS_XDG_OUTPUT)
    zxdg_output_manager_v1* xom = nullptr;
#endif
    // liveness
    double ignore_until = 0.0;
    uint32_t deferred_ping = 0;
    double freeze_at = 0.0;
#if defined(LUMEN_PROBE_HAS_DECORATION)
    zxdg_decoration_manager_v1* deco_manager = nullptr;
    zxdg_toplevel_decoration_v1* deco = nullptr;
#endif
    // activation
#if defined(LUMEN_PROBE_HAS_ACTIVATION)
    xdg_activation_v1* activation = nullptr;
#endif
    bool activated = false;     // the target presented its token
    bool token_requested = false;
    // scale mode
#if defined(LUMEN_PROBE_HAS_SCALE)
    wp_viewporter* viewporter = nullptr;
    wp_fractional_scale_manager_v1* fractional_manager = nullptr;
    wp_viewport* viewport = nullptr;
    wp_fractional_scale_v1* fractional = nullptr;
#endif
    uint32_t scale120 = 0;
    int preferred_buffer_scale = 1;
    int last_w = 0, last_h = 0;
    // presentation mode
#if defined(LUMEN_PROBE_HAS_PRESENTATION)
    wp_presentation* presentation = nullptr;
#endif
    // idle / inhibit / power modes
#if defined(LUMEN_PROBE_HAS_IDLE_NOTIFY)
    ext_idle_notifier_v1* idle_notifier = nullptr;
    ext_idle_notification_v1* idle_notification = nullptr;
#endif
#if defined(LUMEN_PROBE_HAS_IDLE_INHIBIT)
    zwp_idle_inhibit_manager_v1* inhibit_manager = nullptr;
    zwp_idle_inhibitor_v1* inhibitor = nullptr;
#endif
#if defined(LUMEN_PROBE_HAS_OUTPUT_POWER)
    zwlr_output_power_manager_v1* power_manager = nullptr;
    zwlr_output_power_v1* power = nullptr;
#endif
    double idle_created_at = 0.0;
    int idled_count = 0;
    double destroy_inhibitor_at = 0.0;
    bool power_seen_mode = false;
    double power_set_at = 0.0;
    double power_then_at = 0.0;
    // lock mode
#if defined(LUMEN_PROBE_HAS_SESSION_LOCK)
    ext_session_lock_manager_v1* lock_manager = nullptr;
    ext_session_lock_v1* lock = nullptr;
    ext_session_lock_surface_v1* lock_surface = nullptr;
#endif
    surface_state lock_surf{};
    double lock_requested_at = 0.0;
    bool lock_locked = false;
    double lock_surface_at = 0.0;
    double unlock_at = 0.0;
    double crash_at = 0.0;
    bool lock_done = false;
    char typed[64]{};
    unsigned typed_len = 0;
};

app g{};
volatile sig_atomic_t g_stop = 0;
/// The input-method mode's popup surface, and the wl_surface the pointer is on (for naming a
/// surface that has no surface_state: "im-popup").
wl_surface* g_im_popup_surface = nullptr;
wl_surface* g_pointer_wl = nullptr;

double now_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

const char* name_of(const surface_state* s, char* buf, size_t cap) {
    if (!s)
        return "none";
    if (s->index == -2)
        return "second";
    if (s->index == -3)
        return "lock";
    if (s->index < 0)
        return g.opt.layer_mode ? "layer" : "toplevel";
    std::snprintf(buf, cap, "popup%d", s->index);
    return buf;
}

surface_state* find(wl_surface* surface) {
    if (g.top.surface == surface)
        return &g.top;
    if (g.second.surface && g.second.surface == surface)
        return &g.second;
    if (g.lock_surf.surface && g.lock_surf.surface == surface)
        return &g.lock_surf;
    for (unsigned i = 0; i < g.out_count; ++i)
        if (g.outs[i].lock_surf.surface && g.outs[i].lock_surf.surface == surface)
            return &g.outs[i].lock_surf;
    for (int i = 0; i < g.popup_count; ++i)
        if (g.popups[i].surface == surface)
            return &g.popups[i];
    return nullptr;
}

/// A solid-colour XRGB buffer of w x h, in its own memfd pool.
wl_buffer* make_buffer(int w, int h, uint32_t colour) {
    const int stride = w * 4;
    const size_t size = static_cast<size_t>(stride) * static_cast<size_t>(h);
    const int fd = memfd_create("lumen-probe", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0)
        return nullptr;
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data != MAP_FAILED) {
        auto* px = static_cast<uint32_t*>(data);
        for (size_t i = 0; i < size / 4; ++i)
            px[i] = colour;
        munmap(data, size);
    }
    wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, static_cast<int>(size));
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

void draw_scaled(int w, int h);
#if defined(LUMEN_PROBE_HAS_PRESENTATION)
void present_configure(int w, int h);
#endif

void draw(surface_state& s, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    if (g.opt.scale_mode && &s == &g.top) {
        draw_scaled(w, h);
        return;
    }
#if defined(LUMEN_PROBE_HAS_PRESENTATION)
    if (g.opt.presentation_mode && &s == &g.top) {
        present_configure(w, h);
        return;
    }
#endif
    if (!s.buffer || s.buffer_w != w || s.buffer_h != h) {
        if (s.buffer)
            wl_buffer_destroy(s.buffer);
        s.buffer = make_buffer(w, h, s.colour);
        s.buffer_w = w;
        s.buffer_h = h;
    }
    wl_surface_attach(s.surface, s.buffer, 0, 0);
    wl_surface_damage_buffer(s.surface, 0, 0, w, h);
    wl_surface_commit(s.surface);
}

// ── scale mode: wp_viewporter + wp_fractional_scale_v1 ──

/// The surface's content in one buffer of `bw`x`bh`, filled by `px(x, y)`.
template <typename Fill>
wl_buffer* make_pattern(int bw, int bh, Fill px) {
    const int stride = bw * 4;
    const size_t size = static_cast<size_t>(stride) * static_cast<size_t>(bh);
    const int fd = memfd_create("lumen-probe-scale", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0)
        return nullptr;
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data != MAP_FAILED) {
        auto* p = static_cast<uint32_t*>(data);
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x)
                p[static_cast<size_t>(y) * bw + x] = px(x, y);
        munmap(data, size);
    }
    wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, static_cast<int>(size));
    wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, bw, bh, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

void on_buffer_release(void*, wl_buffer* b) { wl_buffer_destroy(b); }
const wl_buffer_listener scale_buffer_listener = {on_buffer_release};

void draw_scaled(int w, int h) {
    g.last_w = w;
    g.last_h = h;
    const options& o = g.opt;
    int bw = 0, bh = 0, bs = 1;
    wl_buffer* b = nullptr;
    if (o.vp_buffer_w > 0) {
        // Viewport test: quadrants with 1 px white frames.
        bw = o.vp_buffer_w;
        bh = o.vp_buffer_h;
        bs = o.buffer_scale;
        b = make_pattern(bw, bh, [&](int x, int y) -> uint32_t {
            const int qx = x < bw / 2 ? 0 : 1;
            const int qy = y < bh / 2 ? 0 : 1;
            const int x0 = qx ? bw / 2 : 0, y0 = qy ? bh / 2 : 0;
            const int x1 = qx ? bw - 1 : bw / 2 - 1, y1 = qy ? bh - 1 : bh / 2 - 1;
            if (x == x0 || x == x1 || y == y0 || y == y1)
                return 0xffffffffu;
            static const uint32_t k_quad[4] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu,
                                               0xffffff00u};
            return k_quad[qy * 2 + qx];
        });
    } else {
        // The client's own pixels at the output's scale: round(size·scale), as foot and
        // GTK compute it; --integer renders at the integer preferred buffer scale instead.
        const uint32_t s120 = g.scale120 ? g.scale120 : 120;
        if (o.integer_scale) {
            bs = g.preferred_buffer_scale > 0 ? g.preferred_buffer_scale : 1;
            bw = w * bs;
            bh = h * bs;
        } else {
            bw = static_cast<int>((static_cast<long>(w) * s120 + 60) / 120);
            bh = static_cast<int>((static_cast<long>(h) * s120 + 60) / 120);
        }
        b = make_pattern(bw, bh, [](int x, int y) -> uint32_t {
            return ((x ^ y) & 1) ? 0xffffffffu : 0xff000000u;
        });
    }
    if (!b)
        return;
    wl_buffer_add_listener(b, &scale_buffer_listener, nullptr);
    wl_surface_set_buffer_scale(g.top.surface, bs);
    wl_surface_set_buffer_transform(g.top.surface, o.transform);
    int dw = 0, dh = 0;
#if defined(LUMEN_PROBE_HAS_SCALE)
    if (g.viewport) {
        if (o.crop)
            wp_viewport_set_source(g.viewport, wl_fixed_from_double(o.crop_x),
                                   wl_fixed_from_double(o.crop_y), wl_fixed_from_double(o.crop_w),
                                   wl_fixed_from_double(o.crop_h));
        dw = o.dst_w > 0 ? o.dst_w : w;
        dh = o.dst_h > 0 ? o.dst_h : h;
        if (o.integer_scale && o.vp_buffer_w <= 0)
            wp_viewport_set_destination(g.viewport, -1, -1);
        else
            wp_viewport_set_destination(g.viewport, dw, dh);
    }
#endif
    wl_surface_attach(g.top.surface, b, 0, 0);
    wl_surface_damage_buffer(g.top.surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(g.top.surface);
    std::printf("buffer %dx%d dst %dx%d scale120=%u buffer-scale=%d\n", bw, bh, dw, dh,
                g.scale120, bs);
}

#if defined(LUMEN_PROBE_HAS_SCALE)
void on_preferred_scale(void*, wp_fractional_scale_v1*, uint32_t scale) {
    std::printf("preferred-scale %u\n", scale);
    const bool changed = scale != g.scale120;
    g.scale120 = scale;
    // Redraw only once there is a configured size; the first value arrives before it.
    if (changed && g.last_w > 0 && !g.opt.integer_scale && g.opt.vp_buffer_w <= 0)
        draw_scaled(g.last_w, g.last_h);
}
const wp_fractional_scale_v1_listener fractional_listener = {on_preferred_scale};
#endif

// ── presentation mode: wp_presentation feedback per commit ──

#if defined(LUMEN_PROBE_HAS_PRESENTATION)

/// One shm buffer of the animation, reused once released.
struct anim_buffer {
    wl_buffer* buffer = nullptr;
    uint32_t* pixels = nullptr;
    size_t bytes = 0;
    int w = 0, h = 0;
    bool busy = false;
};

/// One wp_presentation_feedback in flight: which frame it belongs to and when it was
/// committed (CLOCK_MONOTONIC, the presentation clock this probe checks it is told).
struct feedback_rec {
    struct wp_presentation_feedback* fb = nullptr;
    int frame = 0;
    unsigned long long commit_ns = 0;
    unsigned sync_outputs = 0;
    bool used = false;
};

struct presentation_state {
    anim_buffer buffers[8]{};
    feedback_rec recs[64]{};
    int w = 0, h = 0;
    int frame = 0;       // frames committed
    bool started = false;
    bool finished = false; // --frames reached: no more commits
    bool waiting_buffer = false; // a frame is due but every buffer is held
    bool summarized = false;
    double finish_deadline = 0.0;
    uint32_t clock_id = ~0u;
    unsigned presented = 0, discarded = 0, sync_outputs = 0;
    unsigned long long first_seq = 0, last_seq = 0;
    bool seq_monotonic = true;
    bool t_monotonic = true;
    unsigned long long last_t = 0;
    double dt_sum = 0.0, dt_max = 0.0;
    unsigned dt_n = 0;
    double latency_sum = 0.0;
    unsigned refresh = 0;
    unsigned flags_or = 0;
};
presentation_state g_pres{};

unsigned long long mono_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long long>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<unsigned long long>(ts.tv_nsec);
}

void present_frame();

void on_anim_release(void* data, wl_buffer*) {
    static_cast<anim_buffer*>(data)->busy = false;
    if (g_pres.waiting_buffer)
        present_frame();
}
const wl_buffer_listener anim_buffer_listener = {on_anim_release};

anim_buffer* anim_take(int w, int h) {
    for (auto& b : g_pres.buffers) {
        if (b.buffer && !b.busy && b.w == w && b.h == h)
            return &b;
    }
    for (auto& b : g_pres.buffers) {
        if (b.buffer && b.busy)
            continue;
        if (b.buffer) {
            wl_buffer_destroy(b.buffer);
            munmap(b.pixels, b.bytes);
            b = {};
        }
        const int stride = w * 4;
        b.bytes = static_cast<size_t>(stride) * static_cast<size_t>(h);
        const int fd = memfd_create("lumen-probe-anim", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, static_cast<off_t>(b.bytes)) != 0)
            return nullptr;
        void* data = mmap(nullptr, b.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            close(fd);
            return nullptr;
        }
        wl_shm_pool* pool = wl_shm_create_pool(g.shm, fd, static_cast<int>(b.bytes));
        b.buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        wl_buffer_add_listener(b.buffer, &anim_buffer_listener, &b);
        b.pixels = static_cast<uint32_t*>(data);
        b.w = w;
        b.h = h;
        return &b;
    }
    return nullptr; // every buffer is still held by the compositor
}

feedback_rec* rec_of(struct wp_presentation_feedback* fb) {
    for (auto& r : g_pres.recs)
        if (r.used && r.fb == fb)
            return &r;
    return nullptr;
}

void on_fb_sync_output(void* data, struct wp_presentation_feedback*, wl_output*) {
    ++static_cast<feedback_rec*>(data)->sync_outputs;
}

void on_fb_presented(void* data, struct wp_presentation_feedback* fb, uint32_t sec_hi,
                     uint32_t sec_lo, uint32_t nsec, uint32_t refresh, uint32_t seq_hi,
                     uint32_t seq_lo, uint32_t flags) {
    auto* r = static_cast<feedback_rec*>(data);
    const unsigned long long t =
        ((static_cast<unsigned long long>(sec_hi) << 32 | sec_lo) * 1'000'000'000ull) + nsec;
    const unsigned long long seq = static_cast<unsigned long long>(seq_hi) << 32 | seq_lo;
    double dt = 0.0;
    if (g_pres.last_t != 0) {
        dt = static_cast<double>(t - g_pres.last_t) / 1e6;
        g_pres.dt_sum += dt;
        g_pres.dt_n += 1;
        if (dt > g_pres.dt_max)
            g_pres.dt_max = dt;
        if (seq <= g_pres.last_seq)
            g_pres.seq_monotonic = false;
        if (t <= g_pres.last_t)
            g_pres.t_monotonic = false;
    } else {
        g_pres.first_seq = seq;
    }
    const double latency = static_cast<double>(t - r->commit_ns) / 1e6;
    g_pres.latency_sum += latency;
    g_pres.last_t = t;
    g_pres.last_seq = seq;
    g_pres.refresh = refresh;
    g_pres.flags_or |= flags;
    g_pres.sync_outputs += r->sync_outputs;
    ++g_pres.presented;
    std::printf("presented frame=%d seq=%llu t=%llu.%09llu dt=%.3f refresh=%u flags=0x%x "
                "sync_output=%u latency=%.3f\n",
                r->frame, seq, t / 1'000'000'000ull, t % 1'000'000'000ull, dt, refresh, flags,
                r->sync_outputs, latency);
    wp_presentation_feedback_destroy(fb);
    *r = {};
}

void on_fb_discarded(void* data, struct wp_presentation_feedback* fb) {
    auto* r = static_cast<feedback_rec*>(data);
    ++g_pres.discarded;
    std::printf("discarded frame=%d at=%.6f\n", r->frame, now_s());
    wp_presentation_feedback_destroy(fb);
    *r = {};
}

const wp_presentation_feedback_listener fb_listener = {on_fb_sync_output, on_fb_presented,
                                                       on_fb_discarded};

void on_clock_id(void*, wp_presentation*, uint32_t clk) {
    g_pres.clock_id = clk;
    std::printf("clock_id %u\n", clk);
}
const wp_presentation_listener presentation_listener = {on_clock_id};

unsigned feedback_pending() {
    unsigned n = 0;
    for (const auto& r : g_pres.recs)
        n += r.used ? 1u : 0u;
    return n;
}

void present_summary() {
    std::printf("presentation summary frames=%d presented=%u discarded=%u pending=%u "
                "seq_monotonic=%d seq_first=%llu seq_last=%llu refresh=%u flags=0x%x "
                "dt_mean=%.3f dt_max=%.3f latency_mean=%.3f sync_output=%u clock_id=%u "
                "t_monotonic=%d\n",
                g_pres.frame, g_pres.presented, g_pres.discarded, feedback_pending(),
                g_pres.seq_monotonic ? 1 : 0, g_pres.first_seq, g_pres.last_seq, g_pres.refresh,
                g_pres.flags_or, g_pres.dt_n ? g_pres.dt_sum / g_pres.dt_n : 0.0, g_pres.dt_max,
                g_pres.presented ? g_pres.latency_sum / g_pres.presented : 0.0,
                g_pres.sync_outputs, g_pres.clock_id, g_pres.t_monotonic ? 1 : 0);
}

void present_frame();

void on_anim_frame_done(void*, wl_callback* cb, uint32_t) {
    wl_callback_destroy(cb);
    present_frame();
}
const wl_callback_listener anim_frame_listener = {on_anim_frame_done};

/// One frame: `--burst` commits, each a new colour with its own feedback; the first also
/// asks for the frame callback that paces the next (a superseded update's callback is still
/// answered). With every buffer still held, the frame waits for a release instead.
void present_frame() {
    g_pres.waiting_buffer = false;
    if (g_pres.finished || g.unmapped)
        return;
    if (g.opt.frames > 0 && g_pres.frame >= g.opt.frames) {
        g_pres.finished = true;
        g_pres.finish_deadline = now_s() + 2.0;
        return;
    }
    const int w = g_pres.w, h = g_pres.h;
    for (int k = 0; k < g.opt.burst; ++k) {
        if (g.opt.frames > 0 && g_pres.frame >= g.opt.frames)
            break;
        anim_buffer* b = anim_take(w, h);
        if (!b) {
            // All held: the compositor releases one once it has read it.
            g_pres.waiting_buffer = k == 0;
            break;
        }
        ++g_pres.frame;
        // A hue that moves every frame: every commit is new content.
        const uint32_t v = static_cast<uint32_t>(g_pres.frame * 7 % 256);
        const uint32_t colour = 0xff000000u | (v << 16) | ((255u - v) << 8) | 0x60u;
        for (size_t i = 0; i < b->bytes / 4; ++i)
            b->pixels[i] = colour;
        feedback_rec* r = nullptr;
        for (auto& x : g_pres.recs)
            if (!x.used) {
                r = &x;
                break;
            }
        if (r) {
            r->used = true;
            r->frame = g_pres.frame;
            r->sync_outputs = 0;
            r->fb = wp_presentation_feedback(g.presentation, g.top.surface);
            wp_presentation_feedback_add_listener(r->fb, &fb_listener, r);
        }
        if (k == 0) {
            wl_callback* cb = wl_surface_frame(g.top.surface);
            wl_callback_add_listener(cb, &anim_frame_listener, nullptr);
        }
        wl_surface_attach(g.top.surface, b->buffer, 0, 0);
        wl_surface_damage_buffer(g.top.surface, 0, 0, w, h);
        b->busy = true;
        if (r)
            r->commit_ns = mono_ns();
        wl_surface_commit(g.top.surface);
    }
}

void present_configure(int w, int h) {
    g_pres.w = w;
    g_pres.h = h;
    if (g_pres.started)
        return; // the animation picks the new size up with its next frame
    g_pres.started = true;
    present_frame();
}

#endif

// ── idle mode: ext_idle_notification_v1 ──
#if defined(LUMEN_PROBE_HAS_IDLE_NOTIFY)
void on_idled(void*, ext_idle_notification_v1*) {
    const double now = now_s();
    std::printf("idled t=%.3f at=%.3f\n", now - g.idle_created_at, now);
    if (g.opt.idle_count > 0 && ++g.idled_count >= g.opt.idle_count)
        g.closed = true;
}
void on_resumed(void*, ext_idle_notification_v1*) {
    const double now = now_s();
    std::printf("resumed t=%.3f at=%.3f\n", now - g.idle_created_at, now);
}
const ext_idle_notification_v1_listener idle_listener = {on_idled, on_resumed};
#endif

// ── power mode: zwlr_output_power_v1 ──
#if defined(LUMEN_PROBE_HAS_OUTPUT_POWER)
void power_request(int mode) {
    if (!g.power || mode < 0)
        return;
    std::printf("power set mode=%s\n", mode ? "on" : "off");
    zwlr_output_power_v1_set_mode(g.power, static_cast<uint32_t>(mode));
}
void on_power_mode(void*, zwlr_output_power_v1*, uint32_t mode) {
    std::printf("power mode=%s at=%.3f\n", mode == ZWLR_OUTPUT_POWER_V1_MODE_ON ? "on" : "off",
                now_s());
    if (g.power_seen_mode)
        return;
    g.power_seen_mode = true;
    if (g.opt.power_invalid) {
        std::printf("power set mode=2 (invalid)\n");
        zwlr_output_power_v1_set_mode(g.power, 2);
        return;
    }
    if (g.opt.power_set >= 0)
        g.power_set_at = now_s() + g.opt.power_set_after;
}
void on_power_failed(void*, zwlr_output_power_v1*) { std::printf("power failed\n"); }
const zwlr_output_power_v1_listener power_listener = {on_power_mode, on_power_failed};
#endif

// ── lock mode: ext-session-lock-v1 ──
#if defined(LUMEN_PROBE_HAS_SESSION_LOCK)
void lock_unlock(const char* why) {
    if (!g.lock || g.lock_done)
        return;
    g.lock_done = true;
    ext_session_lock_v1_unlock_and_destroy(g.lock);
    g.lock = nullptr;
    // "A correct client that wishes to exit directly after unlocking the session must use
    // the wl_display.sync request to ensure the server receives and processes the
    // unlock_and_destroy request."
    wl_display_roundtrip(g.display);
    std::printf("lock unlocked at=%.3f (%s)\n", now_s(), why);
    g.closed = true;
}
void on_lock_locked(void*, ext_session_lock_v1*) {
    const double now = now_s();
    g.lock_locked = true;
    std::printf("lock locked at=%.3f after=%.3f\n", now, now - g.lock_requested_at);
    if (g.opt.unlock_after > 0.0)
        g.unlock_at = now + g.opt.unlock_after;
    if (g.opt.crash_after > 0.0)
        g.crash_at = now + g.opt.crash_after;
}
void on_lock_finished(void*, ext_session_lock_v1*) {
    std::printf("lock finished at=%.3f locked=%d\n", now_s(), g.lock_locked ? 1 : 0);
    if (!g.lock || g.lock_done)
        return;
    g.lock_done = true;
    if (g.lock_locked)
        ext_session_lock_v1_unlock_and_destroy(g.lock);
    else
        ext_session_lock_v1_destroy(g.lock);
    g.lock = nullptr;
    wl_display_roundtrip(g.display);
    g.closed = true;
}
const ext_session_lock_v1_listener lock_listener = {on_lock_locked, on_lock_finished};

void on_lock_configure(void* data, ext_session_lock_surface_v1* ls, uint32_t serial, uint32_t w,
                       uint32_t h) {
    auto* s = data ? static_cast<surface_state*>(data) : &g.lock_surf;
    std::printf("lock configure %ux%u serial=%u at=%.3f\n", w, h, serial, now_s());
    ext_session_lock_surface_v1_ack_configure(ls, serial);
    const int bw = static_cast<int>(w) - (g.opt.wrong_size ? 10 : 0);
    draw(*s, bw, static_cast<int>(h));
}
const ext_session_lock_surface_v1_listener lock_surface_listener = {on_lock_configure};

/// --all-outputs: a lock surface for output `o` (one plugged in while locked too, as swaylock
/// does: "clients should create lock surfaces for all outputs").
void lock_add_output_surface(probe_output& o) {
    if (!g.lock || !o.out || o.out == g.output || o.lock_surface || o.gone)
        return;
    o.lock_surf.index = -3;
    o.lock_surf.colour = g.opt.lock_colour;
    o.lock_surf.surface = wl_compositor_create_surface(g.compositor);
    o.lock_surface = ext_session_lock_v1_get_lock_surface(g.lock, o.lock_surf.surface, o.out);
    ext_session_lock_surface_v1_add_listener(o.lock_surface, &lock_surface_listener, &o.lock_surf);
}

void lock_make_surface() {
    if (!g.lock || g.lock_surface || !g.output)
        return;
    g.lock_surf.index = -3;
    g.lock_surf.colour = g.opt.lock_colour;
    g.lock_surf.surface = wl_compositor_create_surface(g.compositor);
    g.lock_surface = ext_session_lock_v1_get_lock_surface(g.lock, g.lock_surf.surface, g.output);
    ext_session_lock_surface_v1_add_listener(g.lock_surface, &lock_surface_listener, nullptr);
    if (!g.opt.all_outputs)
        return;
    // "Clients should immediately create lock surfaces for all outputs": one per other output.
    for (unsigned i = 0; i < g.out_count; ++i)
        lock_add_output_surface(g.outs[i]);
}

/// US-layout evdev codes of a-z and 0-9 (enough for a test password).
char lock_char(uint32_t code) {
    static const char row1[] = "qwertyuiop"; // 16..25
    static const char row2[] = "asdfghjkl";  // 30..38
    static const char row3[] = "zxcvbnm";    // 44..50
    if (code >= 16 && code <= 25)
        return row1[code - 16];
    if (code >= 30 && code <= 38)
        return row2[code - 30];
    if (code >= 44 && code <= 50)
        return row3[code - 44];
    if (code >= 2 && code <= 10)
        return static_cast<char>('1' + (code - 2));
    if (code == 11)
        return '0';
    return 0;
}

void lock_key(uint32_t code) {
    if (!g.opt.password)
        return;
    if (code == 28 /* Enter */) {
        g.typed[g.typed_len] = '\0';
        if (std::strcmp(g.typed, g.opt.password) == 0) {
            std::printf("lock password accepted\n");
            lock_unlock("password");
        } else {
            std::printf("lock password wrong\n");
        }
        g.typed_len = 0;
        return;
    }
    if (code == 14 /* Backspace */) {
        if (g.typed_len)
            --g.typed_len;
        return;
    }
    if (const char c = lock_char(code); c && g.typed_len + 1 < sizeof(g.typed))
        g.typed[g.typed_len++] = c;
}
#endif

// ── outputs mode: wl_output, xdg-output, wl_surface.enter/leave ──

const char* output_name_of(const wl_output* out) {
    for (unsigned i = 0; i < g.out_count; ++i)
        if (g.outs[i].out && g.outs[i].out == out)
            return g.outs[i].name[0] ? g.outs[i].name : "?";
    return "?";
}

void on_surface_enter(void*, wl_surface* surface, wl_output* out) {
    char buf[16];
    std::printf("surface enter surface=%s output=%s\n", name_of(find(surface), buf, sizeof(buf)),
                output_name_of(out));
}
void on_surface_leave(void*, wl_surface* surface, wl_output* out) {
    char buf[16];
    std::printf("surface leave surface=%s output=%s\n", name_of(find(surface), buf, sizeof(buf)),
                output_name_of(out));
}
void on_surface_scale(void*, wl_surface*, int32_t s) {
    if (g.opt.scale_mode) {
        std::printf("preferred-buffer-scale %d\n", s);
        const bool changed = s != g.preferred_buffer_scale;
        g.preferred_buffer_scale = s;
        if (changed && g.opt.integer_scale && g.last_w > 0)
            draw_scaled(g.last_w, g.last_h);
        return;
    }
    std::printf("surface preferred_scale=%d\n", s);
}
void on_surface_transform(void*, wl_surface*, uint32_t t) {
    std::printf("surface preferred_transform=%u\n", t);
}
const wl_surface_listener surface_listener = {on_surface_enter, on_surface_leave,
                                              on_surface_scale, on_surface_transform};

void on_output_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t,
                        const char*, const char*, int32_t) {}
void on_output_mode(void* data, wl_output*, uint32_t flags, int32_t w, int32_t h,
                    int32_t refresh) {
    auto* o = static_cast<probe_output*>(data);
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        o->w = w;
        o->h = h;
        o->refresh = refresh;
    }
}
void on_output_done(void* data, wl_output*) {
    auto* o = static_cast<probe_output*>(data);
    ++o->dones;
    std::printf("wl_output name=%s mode=%dx%d refresh=%d scale=%d done=%u\n", o->name, o->w,
                o->h, o->refresh, o->scale, o->dones);
    if (o->xo_seen)
        std::printf("xdg_output position=%d,%d size=%dx%d name=%s description=%s\n", o->xo_x,
                    o->xo_y, o->xo_w, o->xo_h, o->xo_name, o->xo_description);
}
void on_output_scale(void* data, wl_output*, int32_t s) { static_cast<probe_output*>(data)->scale = s; }
void on_output_name(void* data, wl_output*, const char* n) {
    auto* o = static_cast<probe_output*>(data);
    std::snprintf(o->name, sizeof(o->name), "%s", n);
}
void on_output_description(void*, wl_output*, const char*) {}
const wl_output_listener output_listener = {on_output_geometry, on_output_mode,
                                            on_output_done,     on_output_scale,
                                            on_output_name,     on_output_description};

#if defined(LUMEN_PROBE_HAS_XDG_OUTPUT)
void on_xo_position(void* data, zxdg_output_v1*, int32_t x, int32_t y) {
    auto* o = static_cast<probe_output*>(data);
    o->xo_x = x;
    o->xo_y = y;
    o->xo_seen = true;
}
void on_xo_size(void* data, zxdg_output_v1*, int32_t w, int32_t h) {
    auto* o = static_cast<probe_output*>(data);
    o->xo_w = w;
    o->xo_h = h;
}
void on_xo_done(void*, zxdg_output_v1*) {} // v3: wl_output.done closes the group
void on_xo_name(void* data, zxdg_output_v1*, const char* n) {
    auto* o = static_cast<probe_output*>(data);
    std::snprintf(o->xo_name, sizeof(o->xo_name), "%s", n);
}
void on_xo_description(void* data, zxdg_output_v1*, const char* d) {
    auto* o = static_cast<probe_output*>(data);
    std::snprintf(o->xo_description, sizeof(o->xo_description), "%s", d);
}
const zxdg_output_v1_listener xdg_output_listener = {on_xo_position, on_xo_size, on_xo_done,
                                                     on_xo_name, on_xo_description};
#endif

void bind_one_output(probe_output& o) {
    if (o.out || o.gone)
        return;
    const uint32_t v = o.version < 4 ? o.version : 4;
    std::printf("wl_output bind version=%u\n", v);
    o.out = static_cast<wl_output*>(wl_registry_bind(g.registry, o.global, &wl_output_interface, v));
    wl_output_add_listener(o.out, &output_listener, &o);
#if defined(LUMEN_PROBE_HAS_XDG_OUTPUT)
    if (g.xom) {
        o.xo = zxdg_output_manager_v1_get_xdg_output(g.xom, o.out);
        zxdg_output_v1_add_listener(o.xo, &xdg_output_listener, &o);
    }
#endif
}

/// Binds every advertised wl_output and picks the one the probe acts on: --output NAME
/// (a roundtrip learns the names), else the first.
void bind_outputs() {
    if (g.output || g.out_count == 0)
        return;
    for (unsigned i = 0; i < g.out_count; ++i)
        bind_one_output(g.outs[i]);
    if (g.opt.output_name) {
        wl_display_roundtrip(g.display);
        for (unsigned i = 0; i < g.out_count && !g.output; ++i)
            if (g.outs[i].out && std::strcmp(g.outs[i].name, g.opt.output_name) == 0)
                g.output = g.outs[i].out;
        if (!g.output)
            std::fprintf(stderr, "lumen-probe: no wl_output named %s\n", g.opt.output_name);
    } else {
        for (unsigned i = 0; i < g.out_count && !g.output; ++i)
            g.output = g.outs[i].out;
    }
}

#if defined(LUMEN_PROBE_HAS_DECORATION)
void on_deco_configure(void*, zxdg_toplevel_decoration_v1*, uint32_t mode) {
    std::printf("decoration configure mode=%u\n", mode);
}
const zxdg_toplevel_decoration_v1_listener deco_listener = {on_deco_configure};
#endif

// ── xdg_wm_base / xdg_surface / xdg_toplevel ──

void on_ping(void*, xdg_wm_base* base, uint32_t serial) {
    if (g.opt.ignore_ping_for > 0.0) {
        // Not answered until the window has passed (counted from the first configure); the
        // newest ping is answered then, as a client that catches up would.
        if (g.ignore_until == 0.0 || now_s() < g.ignore_until) {
            g.deferred_ping = serial;
            std::printf("ping serial=%u ignored\n", serial);
            return;
        }
        std::printf("pong serial=%u\n", serial);
    }
    xdg_wm_base_pong(base, serial);
}
const xdg_wm_base_listener wm_base_listener = {on_ping};

void on_xdg_configure(void* data, xdg_surface* xs, uint32_t serial) {
    auto* s = static_cast<surface_state*>(data);
    xdg_surface_ack_configure(xs, serial);
    if (s->index < 0) {
        const int w = s->pending_w > 0 ? s->pending_w : g.opt.width;
        const int h = s->pending_h > 0 ? s->pending_h : g.opt.height;
        std::printf("toplevel configure serial=%u w=%d h=%d\n", serial, w, h);
        if (s == &g.top && !s->configured) {
            const double now = now_s();
            if (g.opt.ignore_ping_for > 0.0)
                g.ignore_until = now + g.opt.ignore_ping_for;
            if (g.opt.freeze > 0.0)
                g.freeze_at = now + 0.2; // after the first frame is on screen
            if (g.opt.outputs_mode && g.opt.late_output > 0.0)
                g.bind_output_at = now + g.opt.late_output;
            if (!g.opt.layer_mode && g.opt.unmap_after > 0.0)
                g.unmap_at = now + g.opt.unmap_after;
        }
        if (g.unmapped)
            return;
        draw(*s, w, h);
    } else {
        std::printf("popup configure idx=%d x=%d y=%d w=%d h=%d serial=%u\n", s->index,
                    s->pending_x_y[0], s->pending_x_y[1], s->pending_w, s->pending_h, serial);
        if (!s->configured && s->index == 0 && g.opt.reposition)
            g.reposition_at = now_s() + 0.5;
        draw(*s, s->pending_w, s->pending_h);
    }
    s->configured = true;
}
const xdg_surface_listener xdg_listener = {on_xdg_configure};

void on_toplevel_configure(void* data, xdg_toplevel*, int32_t w, int32_t h, wl_array* states) {
    auto* s = static_cast<surface_state*>(data);
    s->pending_w = w;
    s->pending_h = h;
    if (g.opt.fullscreen) {
        bool fs = false;
        const auto* st = static_cast<const uint32_t*>(states->data);
        for (size_t i = 0; i < states->size / sizeof(uint32_t); ++i)
            fs = fs || st[i] == XDG_TOPLEVEL_STATE_FULLSCREEN;
        std::printf("toplevel fullscreen=%d w=%d h=%d\n", fs ? 1 : 0, w, h);
    }
    if (g.opt.activation_issuer || g.opt.activation_target) {
        bool active = false;
        const auto* st = static_cast<const uint32_t*>(states->data);
        for (size_t i = 0; i < states->size / sizeof(uint32_t); ++i)
            active = active || st[i] == XDG_TOPLEVEL_STATE_ACTIVATED;
        std::printf("toplevel activated=%d\n", active ? 1 : 0);
    }
}
void on_toplevel_close(void*, xdg_toplevel*) { g.closed = true; }
void on_toplevel_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void on_toplevel_caps(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener toplevel_listener = {on_toplevel_configure, on_toplevel_close,
                                                 on_toplevel_bounds, on_toplevel_caps};

// ── xdg_popup ──

void destroy_finished_popups() {
    // Topmost first: the only order the protocol allows.
    while (g.popup_count > 0 && g.popups[g.popup_count - 1].done) {
        surface_state& p = g.popups[g.popup_count - 1];
        xdg_popup_destroy(p.popup);
        xdg_surface_destroy(p.xdg);
        wl_surface_destroy(p.surface);
        if (p.buffer)
            wl_buffer_destroy(p.buffer);
        if (g.pointer_on == &p)
            g.pointer_on = nullptr;
        p = {};
        --g.popup_count;
    }
}

void on_popup_configure(void* data, xdg_popup*, int32_t x, int32_t y, int32_t w, int32_t h) {
    auto* s = static_cast<surface_state*>(data);
    s->pending_x_y[0] = x;
    s->pending_x_y[1] = y;
    s->pending_w = w;
    s->pending_h = h;
}
void on_popup_done(void* data, xdg_popup*) {
    auto* s = static_cast<surface_state*>(data);
    std::printf("popup_done idx=%d\n", s->index);
    s->done = true;
    destroy_finished_popups();
}
void on_repositioned(void* data, xdg_popup*, uint32_t token) {
    std::printf("repositioned idx=%d token=%u\n", static_cast<surface_state*>(data)->index, token);
}
const xdg_popup_listener popup_listener = {on_popup_configure, on_popup_done, on_repositioned};

xdg_positioner* positioner_for(int index, int dx, int dy) {
    xdg_positioner* p = xdg_wm_base_create_positioner(g.wm_base);
    xdg_positioner_set_size(p, g.opt.popup_w, g.opt.popup_h);
    if (index == 0) {
        xdg_positioner_set_anchor_rect(p, g.opt.ar_x + dx, g.opt.ar_y + dy, g.opt.ar_w, g.opt.ar_h);
        xdg_positioner_set_anchor(p, g.opt.anchor);
        xdg_positioner_set_gravity(p, g.opt.gravity);
        xdg_positioner_set_constraint_adjustment(p, g.opt.adjust);
        xdg_positioner_set_offset(p, g.opt.off_x, g.opt.off_y);
        if (g.opt.reactive && xdg_wm_base_get_version(g.wm_base) >= 3)
            xdg_positioner_set_reactive(p);
    } else {
        // Nested: at the parent popup's right edge, top-aligned (a submenu).
        const surface_state& parent = g.popups[index - 1];
        const int pw = parent.pending_w > 0 ? parent.pending_w : g.opt.popup_w;
        xdg_positioner_set_anchor_rect(p, 0, 0, pw, 20);
        xdg_positioner_set_anchor(p, XDG_POSITIONER_ANCHOR_TOP_RIGHT);
        xdg_positioner_set_gravity(p, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        xdg_positioner_set_constraint_adjustment(
            p, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
                   XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    }
    return p;
}

void open_popup(xdg_surface* parent, uint32_t press_serial) {
    if (g.popup_count >= k_max_popups)
        return;
    const int index = g.popup_count++;
    surface_state& s = g.popups[index];
    s = {};
    s.index = index;
    s.colour = index % 2 ? 0xffd98a3d : 0xff3dd98a;
    s.surface = wl_compositor_create_surface(g.compositor);
    if (g.opt.outputs_mode)
        wl_surface_add_listener(s.surface, &surface_listener, nullptr);
    s.xdg = xdg_wm_base_get_xdg_surface(g.wm_base, s.surface);
    xdg_surface_add_listener(s.xdg, &xdg_listener, &s);
    xdg_positioner* pos = positioner_for(index, 0, 0);
    s.popup = xdg_surface_get_popup(s.xdg, parent, pos);
    xdg_positioner_destroy(pos);
    xdg_popup_add_listener(s.popup, &popup_listener, &s);
#if defined(LUMEN_PROBE_HAS_LAYER_SHELL)
    // A layer surface's popup: created without a parent, adopted before its initial commit.
    if (index == 0 && g.layer)
        zwlr_layer_surface_v1_get_popup(g.layer, s.popup);
#endif
    uint32_t grab_serial = 0;
    if (g.opt.grab || g.opt.grab_stale) {
        grab_serial = g.opt.grab_stale ? g.enter_serial : press_serial;
        xdg_popup_grab(s.popup, g.seat, grab_serial);
        std::printf("popup open idx=%d grab=%u\n", index, grab_serial);
    } else {
        std::printf("popup open idx=%d grab=-\n", index);
    }
    wl_surface_commit(s.surface);
}

// ── selection mode ──

void sel_send(const char* mime, int32_t fd) {
    std::printf("selection send mime=%s\n", mime);
    const size_t n = std::strlen(g.opt.data);
    (void)!write(fd, g.opt.data, n);
    close(fd);
}

void on_source_target(void*, wl_data_source*, const char*) {}
void on_source_send(void*, wl_data_source*, const char* mime, int32_t fd) { sel_send(mime, fd); }
void on_source_cancelled(void*, wl_data_source* src) {
    std::printf("selection cancelled\n");
    wl_data_source_destroy(src);
}
void on_source_drop(void*, wl_data_source*) {}
void on_source_finished(void*, wl_data_source*) {}
void on_source_action(void*, wl_data_source*, uint32_t) {}
const wl_data_source_listener source_listener = {on_source_target, on_source_send,
                                                 on_source_cancelled, on_source_drop,
                                                 on_source_finished, on_source_action};

#if defined(LUMEN_PROBE_HAS_PRIMARY_SELECTION)
void on_psource_send(void*, zwp_primary_selection_source_v1*, const char* mime, int32_t fd) {
    sel_send(mime, fd);
}
void on_psource_cancelled(void*, zwp_primary_selection_source_v1* src) {
    std::printf("selection cancelled\n");
    zwp_primary_selection_source_v1_destroy(src);
}
const zwp_primary_selection_source_v1_listener psource_listener = {on_psource_send,
                                                                   on_psource_cancelled};
#endif

/// Sets the selection quoting `serial` (once; --reuse-source sets the same source twice).
void set_selection(uint32_t serial) {
    if (g.source_set || g.opt.trigger == options::set_on::never)
        return;
    g.source_set = true;
    if (g.opt.stale_serial)
        serial = 1;
    char list[512];
    std::snprintf(list, sizeof(list), "%s", g.opt.mimes);
#if defined(LUMEN_PROBE_HAS_PRIMARY_SELECTION)
    if (g.opt.primary) {
        if (!g.primary_device)
            return;
        auto* src = zwp_primary_selection_device_manager_v1_create_source(g.psm);
        zwp_primary_selection_source_v1_add_listener(src, &psource_listener, nullptr);
        for (char* t = std::strtok(list, ","); t; t = std::strtok(nullptr, ","))
            zwp_primary_selection_source_v1_offer(src, t);
        zwp_primary_selection_device_v1_set_selection(g.primary_device, src, serial);
        if (g.opt.reuse_source)
            zwp_primary_selection_device_v1_set_selection(g.primary_device, src, serial);
        std::printf("selection set serial=%u\n", serial);
        return;
    }
#endif
    if (!g.data_device)
        return;
    wl_data_source* src = wl_data_device_manager_create_data_source(g.ddm);
    wl_data_source_add_listener(src, &source_listener, nullptr);
    for (char* t = std::strtok(list, ","); t; t = std::strtok(nullptr, ","))
        wl_data_source_offer(src, t);
    wl_data_device_set_selection(g.data_device, src, serial);
    if (g.opt.reuse_source)
        wl_data_device_set_selection(g.data_device, src, serial);
    std::printf("selection set serial=%u\n", serial);
}

/// Mimes of the offer being announced (data_offer, then one `offer` each).
void note_offer_mime(const char* mime) {
    const size_t used = std::strlen(g.offer_mimes);
    std::snprintf(g.offer_mimes + used, sizeof(g.offer_mimes) - used, "%s%s", used ? "," : "",
                  mime);
    if (g.opt.receive && std::strcmp(mime, g.opt.receive) == 0)
        g.offer_has_receive = true;
}

/// A selection arrived (null: none). Prints it and, with --receive, starts the read.
template <typename Receive>
void on_selection_offer(bool present, Receive receive) {
    if (!present) {
        std::printf("selection offer none\n");
        return;
    }
    std::printf("selection offer mimes=[%s]\n", g.offer_mimes);
    if (!g.opt.receive || !g.offer_has_receive || g.recv_fd >= 0)
        return;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
        return;
    receive(fds[1]);
    close(fds[1]);
    g.recv_fd = fds[0];
    g.recv_len = 0;
}

void on_offer_offer(void*, wl_data_offer*, const char* mime) { note_offer_mime(mime); }
void on_offer_source_actions(void*, wl_data_offer*, uint32_t a) { g.dnd_source_actions = a; }
void on_offer_action(void*, wl_data_offer* o, uint32_t a) {
    if (o == g.dnd_offer)
        std::printf("dnd action=%u\n", a);
}
const wl_data_offer_listener offer_listener = {on_offer_offer, on_offer_source_actions,
                                               on_offer_action};

void on_dd_data_offer(void*, wl_data_device*, wl_data_offer* o) {
    g.offer_mimes[0] = '\0';
    g.offer_has_receive = false;
    g.dnd_source_actions = 0;
    wl_data_offer_add_listener(o, &offer_listener, nullptr);
}

// ── drag-and-drop: target side ──

/// The offer's type list contains `mime` exactly.
bool offer_lists(const char* mime) {
    char list[1024];
    std::snprintf(list, sizeof(list), "%s", g.offer_mimes);
    for (char* t = std::strtok(list, ","); t; t = std::strtok(nullptr, ","))
        if (std::strcmp(t, mime) == 0)
            return true;
    return false;
}

void dnd_forget_offer() {
    if (g.dnd_offer)
        wl_data_offer_destroy(g.dnd_offer);
    g.dnd_offer = nullptr;
    g.dnd_dropped = false;
    g.dnd_accepted = false;
}

void on_dd_enter(void*, wl_data_device*, uint32_t serial, wl_surface* surface, wl_fixed_t x,
                 wl_fixed_t y, wl_data_offer* o) {
    surface_state* on = find(surface);
    char buf[16];
    std::printf("dnd enter surface=%s x=%.0f y=%.0f offer=%s mimes=[%s] source_actions=%u\n",
                name_of(on, buf, sizeof(buf)), wl_fixed_to_double(x), wl_fixed_to_double(y),
                o ? "obj" : "nil", o ? g.offer_mimes : "", o ? g.dnd_source_actions : 0u);
    if (g.dnd_offer && g.dnd_offer != o && !g.dnd_dropped)
        dnd_forget_offer();
    g.dnd_offer = o;
    g.dnd_dropped = false;
    if (!o)
        return;
    // The source probe's own toplevel is not a target; its second window and the target
    // probe are.
    const bool target = g.opt.dnd_target_mode || (on && on == &g.second);
    const char* mime = target && g.opt.dnd_accept && offer_lists(g.opt.dnd_accept)
                           ? g.opt.dnd_accept
                           : nullptr;
    g.dnd_accepted = mime != nullptr;
    std::snprintf(g.dnd_accepted_mime, sizeof(g.dnd_accepted_mime), "%s", mime ? mime : "");
    wl_data_offer_accept(o, serial, mime);
    if (wl_data_offer_get_version(o) >= 3)
        wl_data_offer_set_actions(o, target ? g.opt.dnd_actions : 0u,
                                  target ? g.opt.dnd_preferred : 0u);
}
void on_dd_leave(void*, wl_data_device*) {
    std::printf("dnd leave\n");
    // After a drop the offer lives on until the data is read and finished.
    if (!g.dnd_dropped)
        dnd_forget_offer();
}
void on_dd_motion(void*, wl_data_device*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    std::printf("dnd motion x=%.0f y=%.0f\n", wl_fixed_to_double(x), wl_fixed_to_double(y));
}
void on_dd_drop(void*, wl_data_device*) {
    std::printf("dnd drop\n");
    g.dnd_dropped = true;
    if (!g.dnd_offer || !g.dnd_accepted || g.recv_fd >= 0)
        return;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
        return;
    wl_data_offer_receive(g.dnd_offer, g.dnd_accepted_mime, fds[1]);
    close(fds[1]);
    g.recv_fd = fds[0];
    g.recv_len = 0;
    g.recv_label = "dnd";
}
void on_dd_selection(void*, wl_data_device*, wl_data_offer* o) {
    if (g.offer && g.offer != o)
        wl_data_offer_destroy(g.offer);
    g.offer = o;
    if (g.opt.primary || !g.opt.selection_mode)
        return;
    on_selection_offer(o != nullptr, [o](int fd) { wl_data_offer_receive(o, g.opt.receive, fd); });
}
const wl_data_device_listener data_device_listener = {on_dd_data_offer, on_dd_enter,
                                                      on_dd_leave, on_dd_motion, on_dd_drop,
                                                      on_dd_selection};

#if defined(LUMEN_PROBE_HAS_PRIMARY_SELECTION)
void on_poffer_offer(void*, zwp_primary_selection_offer_v1*, const char* mime) {
    note_offer_mime(mime);
}
const zwp_primary_selection_offer_v1_listener poffer_listener = {on_poffer_offer};
void on_pd_data_offer(void*, zwp_primary_selection_device_v1*,
                      zwp_primary_selection_offer_v1* o) {
    g.offer_mimes[0] = '\0';
    g.offer_has_receive = false;
    zwp_primary_selection_offer_v1_add_listener(o, &poffer_listener, nullptr);
}
void on_pd_selection(void*, zwp_primary_selection_device_v1*,
                     zwp_primary_selection_offer_v1* o) {
    if (g.primary_offer && g.primary_offer != o)
        zwp_primary_selection_offer_v1_destroy(g.primary_offer);
    g.primary_offer = o;
    if (!g.opt.primary)
        return;
    on_selection_offer(o != nullptr, [o](int fd) {
        zwp_primary_selection_offer_v1_receive(o, g.opt.receive, fd);
    });
}
const zwp_primary_selection_device_v1_listener primary_device_listener = {on_pd_data_offer,
                                                                          on_pd_selection};
#endif

/// Drains the pipe of a --receive; prints the data at EOF.
void pump_receive() {
    if (g.recv_fd < 0)
        return;
    for (;;) {
        const ssize_t n = read(g.recv_fd, g.recv_buf + g.recv_len,
                               sizeof(g.recv_buf) - 1 - g.recv_len);
        if (n > 0) {
            g.recv_len += static_cast<size_t>(n);
            if (g.recv_len < sizeof(g.recv_buf) - 1)
                continue;
        } else if (n < 0 && errno == EAGAIN) {
            return;
        }
        g.recv_buf[g.recv_len] = '\0';
        close(g.recv_fd);
        g.recv_fd = -1;
        if (std::strcmp(g.recv_label, "dnd") == 0) {
            std::printf("dnd received data=\"%s\"\n", g.recv_buf);
            g.recv_label = "selection";
            if (g.dnd_offer && !g.opt.no_finish) {
                if (wl_data_offer_get_version(g.dnd_offer) >= 3) {
                    wl_data_offer_finish(g.dnd_offer);
                    std::printf("dnd finish\n");
                }
                dnd_forget_offer();
            }
            return;
        }
        std::printf("selection received mime=%s data=\"%s\"\n", g.opt.receive, g.recv_buf);
        return;
    }
}

void create_selection_devices() {
    if (!(g.opt.selection_mode || g.opt.dnd_source_mode || g.opt.dnd_target_mode) || !g.seat)
        return;
    if (g.ddm && !g.data_device) {
        g.data_device = wl_data_device_manager_get_data_device(g.ddm, g.seat);
        wl_data_device_add_listener(g.data_device, &data_device_listener, nullptr);
    }
#if defined(LUMEN_PROBE_HAS_PRIMARY_SELECTION)
    if (g.psm && !g.primary_device) {
        g.primary_device = zwp_primary_selection_device_manager_v1_get_device(g.psm, g.seat);
        zwp_primary_selection_device_v1_add_listener(g.primary_device,
                                                     &primary_device_listener, nullptr);
    }
#endif
}

// ── drag-and-drop: source side ──

void on_dsrc_target(void*, wl_data_source*, const char* mime) {
    std::printf("dnd source target mime=%s\n", mime ? mime : "nil");
}
void on_dsrc_send(void*, wl_data_source*, const char* mime, int32_t fd) {
    std::printf("dnd source send mime=%s\n", mime);
    (void)!write(fd, g.opt.data, std::strlen(g.opt.data));
    close(fd);
}
void on_dsrc_cancelled(void*, wl_data_source* src) {
    std::printf("dnd source cancelled\n");
    wl_data_source_destroy(src);
    if (g.dnd_source == src)
        g.dnd_source = nullptr;
}
void on_dsrc_drop(void*, wl_data_source*) { std::printf("dnd source drop_performed\n"); }
void on_dsrc_finished(void*, wl_data_source* src) {
    std::printf("dnd source finished\n");
    wl_data_source_destroy(src);
    if (g.dnd_source == src)
        g.dnd_source = nullptr;
}
void on_dsrc_action(void*, wl_data_source*, uint32_t a) {
    std::printf("dnd source action=%u\n", a);
}
const wl_data_source_listener dnd_source_listener = {on_dsrc_target,    on_dsrc_send,
                                                     on_dsrc_cancelled, on_dsrc_drop,
                                                     on_dsrc_finished,  on_dsrc_action};

/// start_drag from the toplevel, quoting the press serial (or the enter's: --stale-serial).
void start_drag(uint32_t press_serial) {
    if (g.drag_started || !g.data_device)
        return;
    g.drag_started = true;
    const uint32_t serial = g.opt.stale_serial ? g.enter_serial : press_serial;
    if (!g.opt.null_source) {
        g.dnd_source = wl_data_device_manager_create_data_source(g.ddm);
        wl_data_source_add_listener(g.dnd_source, &dnd_source_listener, nullptr);
        char list[512];
        std::snprintf(list, sizeof(list), "%s", g.opt.mimes);
        for (char* t = std::strtok(list, ","); t; t = std::strtok(nullptr, ","))
            wl_data_source_offer(g.dnd_source, t);
        if (wl_data_source_get_version(g.dnd_source) >= 3)
            wl_data_source_set_actions(g.dnd_source, g.opt.dnd_actions);
    }
    if (g.opt.icon_w > 0 && g.opt.icon_h > 0)
        g.icon = wl_compositor_create_surface(g.compositor);
    wl_data_device_start_drag(g.data_device, g.dnd_source, g.top.surface, g.icon, serial);
    if (g.icon) {
        // After start_drag, as GTK does: the role is set, the content follows. The attach
        // offset puts the hotspot at the icon's centre.
        g.icon_buffer = make_buffer(g.opt.icon_w, g.opt.icon_h, 0xffc04040);
        wl_surface_attach(g.icon, g.icon_buffer, -g.opt.icon_w / 2, -g.opt.icon_h / 2);
        wl_surface_damage_buffer(g.icon, 0, 0, g.opt.icon_w, g.opt.icon_h);
        wl_surface_commit(g.icon);
        std::printf("dnd icon w=%d h=%d dx=%d dy=%d\n", g.opt.icon_w, g.opt.icon_h,
                    -g.opt.icon_w / 2, -g.opt.icon_h / 2);
    }
    std::printf("dnd source start serial=%u\n", serial);
    if (g.opt.exit_during_drag >= 0.0)
        g.exit_at = now_s() + g.opt.exit_during_drag;
}

unsigned action_bits(const char* list) {
    unsigned bits = 0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s", list);
    for (char* t = std::strtok(buf, ","); t; t = std::strtok(nullptr, ",")) {
        if (!std::strcmp(t, "copy")) bits |= 1;
        else if (!std::strcmp(t, "move")) bits |= 2;
        else if (!std::strcmp(t, "ask")) bits |= 4;
        else if (!std::strcmp(t, "none")) bits |= 0;
        else bits |= static_cast<unsigned>(std::atoi(t));
    }
    return bits;
}

// ── xdg-activation ──

#if defined(LUMEN_PROBE_HAS_ACTIVATION)
void on_token_done(void*, xdg_activation_token_v1* t, const char* token) {
    xdg_activation_token_v1_destroy(t);
    if (g.opt.token_file) {
        char tmp[512];
        std::snprintf(tmp, sizeof(tmp), "%s.tmp", g.opt.token_file);
        if (FILE* f = std::fopen(tmp, "w")) {
            std::fputs(token, f);
            std::fclose(f);
            std::rename(tmp, g.opt.token_file);
        }
    }
    std::printf("activation token=%.8s\n", token);
}
const xdg_activation_token_v1_listener token_listener = {on_token_done};
#endif

/// The issuer: a token for the press with `serial`.
void request_token(uint32_t serial) {
#if defined(LUMEN_PROBE_HAS_ACTIVATION)
    if (!g.activation)
        return;
    xdg_activation_token_v1* t = xdg_activation_v1_get_activation_token(g.activation);
    xdg_activation_token_v1_add_listener(t, &token_listener, nullptr);
    if (!g.opt.no_serial)
        xdg_activation_token_v1_set_serial(t, serial, g.seat);
    xdg_activation_token_v1_set_surface(t, g.top.surface);
    xdg_activation_token_v1_set_app_id(t, "lumen-probe");
    xdg_activation_token_v1_commit(t);
    if (g.opt.no_serial)
        std::printf("activation request serial=none\n");
    else
        std::printf("activation request serial=%u\n", serial);
#else
    (void)serial;
#endif
}

/// The target: presents its token once it has one (--token, or the file once it exists).
void try_activate(bool mapped) {
#if defined(LUMEN_PROBE_HAS_ACTIVATION)
    if (g.activated || !g.activation || !g.opt.activation_target)
        return;
    char buf[256]{};
    const char* token = g.opt.token;
    if (!token && g.opt.token_file) {
        FILE* f = std::fopen(g.opt.token_file, "r");
        if (!f)
            return;
        const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        buf[n] = '\0';
        size_t len = std::strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        if (!buf[0])
            return;
        token = buf;
    }
    if (!token)
        return;
    g.activated = true;
    xdg_activation_v1_activate(g.activation, token, g.top.surface);
    std::printf("activation activate token=%.8s mapped=%d\n", token, mapped ? 1 : 0);
#else
    (void)mapped;
#endif
}

// ── seat ──

void on_pointer_enter(void*, wl_pointer*, uint32_t serial, wl_surface* surface, wl_fixed_t sx,
                      wl_fixed_t sy) {
    g.pointer_on = find(surface);
    g_pointer_wl = surface;
    g.enter_serial = serial;
    char buf[16];
    std::printf("pointer enter surface=%s serial=%u\n",
                surface && surface == g_im_popup_surface ? "im-popup"
                                                         : name_of(g.pointer_on, buf, sizeof(buf)),
                serial);
    // Scale mode: surface-local (logical) coordinates, for hit-testing at the edges.
    if (g.opt.scale_mode)
        std::printf("pointer position x=%.2f y=%.2f\n", wl_fixed_to_double(sx),
                    wl_fixed_to_double(sy));
}
void on_pointer_leave(void*, wl_pointer*, uint32_t, wl_surface*) {
    g.pointer_on = nullptr;
    g_pointer_wl = nullptr;
    std::printf("pointer leave\n");
}
void on_pointer_motion(void*, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    if (g.opt.scale_mode)
        std::printf("pointer position x=%.2f y=%.2f\n", wl_fixed_to_double(sx),
                    wl_fixed_to_double(sy));
}
void on_pointer_button(void*, wl_pointer*, uint32_t serial, uint32_t, uint32_t button,
                       uint32_t state) {
    char buf[16];
    const char* where = g_pointer_wl && g_pointer_wl == g_im_popup_surface
                            ? "im-popup"
                            : name_of(g.pointer_on, buf, sizeof(buf));
    if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
        std::printf("release surface=%s button=%u\n", where, button);
        return;
    }
    std::printf("press surface=%s button=%u serial=%u\n", where, button, serial);
    if (button != 0x110 || !g.pointer_on)
        return;
    if (g.opt.selection_mode) {
        if (g.opt.trigger == options::set_on::press)
            set_selection(serial);
        return;
    }
    if (g.opt.dnd_source_mode) {
        if (g.pointer_on == &g.top)
            start_drag(serial);
        return;
    }
    if (g.opt.dnd_target_mode)
        return;
    if (g.opt.activation_issuer || g.opt.activation_target) {
        if (g.opt.activation_issuer && g.pointer_on == &g.top)
            request_token(serial);
        return;
    }
    if (g.pointer_on == &g.top && g.popup_count == 0 &&
        (!g.opt.layer_mode || g.opt.layer_popup)) {
        open_popup(g.top.xdg, serial);
    } else if (g.pointer_on->index >= 0 && g.pointer_on->index == g.popup_count - 1 &&
               g.popup_count < g.opt.chain && !g.pointer_on->done) {
        open_popup(g.pointer_on->xdg, serial);
    }
}
void on_pointer_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
void on_pointer_frame(void*, wl_pointer*) { std::fflush(stdout); }
void on_axis_source(void*, wl_pointer*, uint32_t) {}
void on_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
void on_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}
const wl_pointer_listener pointer_listener = {on_pointer_enter, on_pointer_leave,
                                              on_pointer_motion, on_pointer_button,
                                              on_pointer_axis, on_pointer_frame,
                                              on_axis_source, on_axis_stop, on_axis_discrete};

void on_keymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); }
void on_key_enter(void*, wl_keyboard*, uint32_t serial, wl_surface* surface, wl_array* keys) {
    char buf[16];
    std::printf("keyboard enter surface=%s\n", name_of(find(surface), buf, sizeof(buf)));
    // The keys the compositor says are already down (its own line, so parsers of the enter
    // line are unaffected): after a VT switch back this must be empty.
    char list[128];
    unsigned n = 0;
    list[0] = '\0';
    const auto* k = static_cast<const uint32_t*>(keys->data);
    for (size_t i = 0; i < keys->size / sizeof(uint32_t) && n + 12 < sizeof(list); ++i)
        n += static_cast<unsigned>(
            std::snprintf(list + n, sizeof(list) - n, "%s%u", i ? "," : "", k[i]));
    std::printf("keyboard keys=[%s]\n", list);
    if (g.opt.selection_mode && g.opt.trigger == options::set_on::focus)
        set_selection(serial);
}
void on_key_leave(void*, wl_keyboard*, uint32_t, wl_surface*) { std::printf("keyboard leave\n"); }
void on_key(void*, wl_keyboard*, uint32_t serial, uint32_t, uint32_t key, uint32_t state) {
    std::printf("key code=%u state=%u\n", key, state);
#if defined(LUMEN_PROBE_HAS_SESSION_LOCK)
    if (g.opt.lock_mode && state == WL_KEYBOARD_KEY_STATE_PRESSED)
        lock_key(key);
#endif
    if (g.opt.selection_mode && g.opt.trigger == options::set_on::key &&
        state == WL_KEYBOARD_KEY_STATE_PRESSED)
        set_selection(serial);
}
void on_mods(void*, wl_keyboard*, uint32_t, uint32_t depressed, uint32_t latched,
             uint32_t locked, uint32_t group) {
    std::printf("keyboard modifiers depressed=%u latched=%u locked=%u group=%u\n", depressed,
                latched, locked, group);
}
void on_repeat(void*, wl_keyboard*, int32_t, int32_t) {}
const wl_keyboard_listener keyboard_listener = {on_keymap, on_key_enter, on_key_leave,
                                                on_key, on_mods, on_repeat};

// ── text-input and input-method modes ──────────────────────────────────────

#if defined(LUMEN_PROBE_HAS_IME)

constexpr unsigned k_text = 4000;

struct ime_state {
    // text-input mode
    zwp_text_input_manager_v3* ti_manager = nullptr;
    uint32_t ti_version = 0;
    zwp_text_input_v3* ti = nullptr;
    wl_surface* ti_focus = nullptr;
    unsigned commits = 0;
    bool enabled = false;
    char text[k_text + 1]{};
    unsigned len = 0;
    unsigned cursor = 0;
    char preedit[k_text + 1]{};
    bool p_preedit = false;
    char p_preedit_text[k_text + 1]{};
    bool p_commit = false;
    char p_commit_text[k_text + 1]{};
    unsigned p_before = 0, p_after = 0;
    double disable_at = 0.0;
    double enable_at = 0.0;
    // input-method mode
    zwp_input_method_manager_v2* im_manager = nullptr;
    zwp_virtual_keyboard_manager_v1* vk_manager = nullptr;
    zwp_input_method_v2* im = nullptr;
    zwp_input_method_keyboard_grab_v2* grab = nullptr;
    zwp_virtual_keyboard_v1* vk = nullptr;
    bool vk_ready = false;
    bool active = false;
    bool pending_activate = false;
    bool pending_deactivate = false;
    unsigned dones = 0;
    char composing[k_text + 1]{};
    unsigned composing_len = 0;
    char surrounding[k_text + 1]{};
    unsigned s_cursor = 0;
    uint32_t purpose = 0, hint = 0;
    wl_surface* popup_surface = nullptr;
    zwp_input_popup_surface_v2* popup = nullptr;
    bool forwarded[256]{};
};
ime_state gi{};

void copy_text(char* dst, const char* src) {
    const size_t n = src ? std::strlen(src) : 0;
    const size_t m = n > k_text ? k_text : n;
    if (m)
        std::memcpy(dst, src, m);
    dst[m] = '\0';
}

void ti_send_state(const char* why) {
    zwp_text_input_v3_set_surrounding_text(gi.ti, gi.text, static_cast<int32_t>(gi.cursor),
                                           static_cast<int32_t>(gi.cursor));
    zwp_text_input_v3_set_content_type(gi.ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
                                       ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    zwp_text_input_v3_set_cursor_rectangle(gi.ti, g.opt.cursor_x, g.opt.cursor_y, g.opt.cursor_w,
                                           g.opt.cursor_h);
    zwp_text_input_v3_commit(gi.ti);
    ++gi.commits;
    // Version 2 applies the rectangle with the surface's next commit (the frame that shows it).
    if (g.top.surface)
        wl_surface_commit(g.top.surface);
    (void)why;
}

void ti_enable_now() {
    if (!gi.ti || !gi.ti_focus)
        return;
    zwp_text_input_v3_enable(gi.ti);
    gi.enabled = true;
    ti_send_state("enable");
    std::printf("ti enable commits=%u\n", gi.commits);
    if (g.opt.disable_after > 0.0)
        gi.disable_at = now_s() + g.opt.disable_after;
}

void ti_disable_now() {
    if (!gi.ti || !gi.ti_focus)
        return;
    zwp_text_input_v3_disable(gi.ti);
    zwp_text_input_v3_commit(gi.ti);
    ++gi.commits;
    gi.enabled = false;
    std::printf("ti disable commits=%u\n", gi.commits);
    if (g.opt.enable_after > 0.0)
        gi.enable_at = now_s() + g.opt.enable_after;
}

void ti_on_enter(void*, zwp_text_input_v3*, wl_surface* s) {
    char buf[16];
    gi.ti_focus = s;
    std::printf("ti enter surface=%s\n", name_of(find(s), buf, sizeof(buf)));
    if (!g.opt.no_enable)
        ti_enable_now();
}
void ti_on_leave(void*, zwp_text_input_v3*, wl_surface*) {
    gi.ti_focus = nullptr;
    gi.enabled = false;
    gi.preedit[0] = '\0';
    std::printf("ti leave\n");
}
void ti_on_preedit(void*, zwp_text_input_v3*, const char* text, int32_t b, int32_t e) {
    gi.p_preedit = true;
    copy_text(gi.p_preedit_text, text);
    std::printf("ti preedit \"%s\" %d %d\n", text ? text : "", b, e);
}
void ti_on_commit(void*, zwp_text_input_v3*, const char* text) {
    gi.p_commit = true;
    copy_text(gi.p_commit_text, text);
    std::printf("ti commit \"%s\"\n", text ? text : "");
}
void ti_on_delete(void*, zwp_text_input_v3*, uint32_t before, uint32_t after) {
    gi.p_before = before;
    gi.p_after = after;
    std::printf("ti delete %u %u\n", before, after);
}
void ti_on_done(void*, zwp_text_input_v3*, uint32_t serial) {
    // The spec's order: preedit replaced by the cursor, deletion, the commit string, preedit.
    bool changed = false;
    if (gi.p_before || gi.p_after) {
        const unsigned from = gi.p_before > gi.cursor ? 0 : gi.cursor - gi.p_before;
        const unsigned to = gi.cursor + gi.p_after > gi.len ? gi.len : gi.cursor + gi.p_after;
        std::memmove(gi.text + from, gi.text + to, gi.len - to + 1);
        gi.len -= to - from;
        gi.cursor = from;
        changed = true;
    }
    if (gi.p_commit) {
        const unsigned n = static_cast<unsigned>(std::strlen(gi.p_commit_text));
        if (n && gi.len + n <= k_text) {
            std::memmove(gi.text + gi.cursor + n, gi.text + gi.cursor, gi.len - gi.cursor + 1);
            std::memcpy(gi.text + gi.cursor, gi.p_commit_text, n);
            gi.len += n;
            gi.cursor += n;
            changed = true;
        }
    }
    copy_text(gi.preedit, gi.p_preedit ? gi.p_preedit_text : "");
    gi.p_preedit = gi.p_commit = false;
    gi.p_before = gi.p_after = 0;
    std::printf("ti done serial=%u commits=%u text=\"%s\" preedit=\"%s\"\n", serial, gi.commits,
                gi.text, gi.preedit);
    // Up to date with the compositor: tell it what the text is now (GTK and foot do).
    if (changed && serial == gi.commits && gi.enabled)
        ti_send_state("done");
}
void ti_on_action(void*, zwp_text_input_v3*, uint32_t, uint32_t) {}
void ti_on_language(void*, zwp_text_input_v3*, const char*) {}
void ti_on_preedit_hint(void*, zwp_text_input_v3*, uint32_t, uint32_t, uint32_t) {}
const zwp_text_input_v3_listener ti_listener = {
    ti_on_enter, ti_on_leave, ti_on_preedit, ti_on_commit, ti_on_delete,
    ti_on_done, ti_on_action, ti_on_language, ti_on_preedit_hint};

// input method

void im_commit_state() { zwp_input_method_v2_commit(gi.im, gi.dones); }

void grab_on_keymap(void*, zwp_input_method_keyboard_grab_v2*, uint32_t format, int32_t fd,
                    uint32_t size) {
    std::printf("im grab keymap size=%u\n", size);
    // Forwarded verbatim, as fcitx5 and ibus do: the compositor sees the seat's own keymap.
    if (gi.vk) {
        zwp_virtual_keyboard_v1_keymap(gi.vk, format, fd, size);
        gi.vk_ready = true;
    }
    close(fd);
}
void grab_on_key(void*, zwp_input_method_keyboard_grab_v2*, uint32_t, uint32_t time, uint32_t key,
                 uint32_t state) {
    std::printf("im key code=%u state=%u\n", key, state);
    const bool press = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    const bool mine = key == 30 || key == 57 || (key == 14 && gi.composing_len > 0);
    if (mine && press) {
        if (key == 30) { // a → α
            if (gi.composing_len + 2 <= k_text) {
                std::memcpy(gi.composing + gi.composing_len, "\xce\xb1", 2);
                gi.composing_len += 2;
                gi.composing[gi.composing_len] = '\0';
            }
            zwp_input_method_v2_set_preedit_string(gi.im, gi.composing,
                                                   static_cast<int32_t>(gi.composing_len),
                                                   static_cast<int32_t>(gi.composing_len));
            im_commit_state();
            std::printf("im preedit \"%s\"\n", gi.composing);
        } else if (key == 14) { // Backspace inside the preedit
            unsigned n = gi.composing_len;
            while (n > 0 && (static_cast<unsigned char>(gi.composing[n - 1]) & 0xC0) == 0x80)
                --n;
            gi.composing_len = n > 0 ? n - 1 : 0;
            gi.composing[gi.composing_len] = '\0';
            zwp_input_method_v2_set_preedit_string(gi.im, gi.composing,
                                                   static_cast<int32_t>(gi.composing_len),
                                                   static_cast<int32_t>(gi.composing_len));
            im_commit_state();
            std::printf("im preedit \"%s\"\n", gi.composing);
        } else if (gi.composing_len > 0) { // space commits the preedit
            zwp_input_method_v2_commit_string(gi.im, gi.composing);
            im_commit_state();
            std::printf("im commit \"%s\"\n", gi.composing);
            gi.composing_len = 0;
            gi.composing[0] = '\0';
        } else {
            zwp_input_method_v2_commit_string(gi.im, " ");
            im_commit_state();
            std::printf("im commit \" \"\n");
        }
        return;
    }
    if (key < 256 && !press && !gi.forwarded[key])
        return; // its press was composed, not forwarded
    if (!gi.vk_ready)
        return;
    if (key < 256)
        gi.forwarded[key] = press;
    zwp_virtual_keyboard_v1_key(gi.vk, time, key, state);
    std::printf("im forwarded code=%u state=%u\n", key, state);
}
void grab_on_modifiers(void*, zwp_input_method_keyboard_grab_v2*, uint32_t, uint32_t d,
                       uint32_t l, uint32_t k, uint32_t group) {
    if (gi.vk_ready)
        zwp_virtual_keyboard_v1_modifiers(gi.vk, d, l, k, group);
}
void grab_on_repeat(void*, zwp_input_method_keyboard_grab_v2*, int32_t, int32_t) {}
const zwp_input_method_keyboard_grab_v2_listener grab_listener = {
    grab_on_keymap, grab_on_key, grab_on_modifiers, grab_on_repeat};

void im_on_activate(void*, zwp_input_method_v2*) {
    gi.pending_activate = true;
    std::printf("im activate\n");
}
void im_on_deactivate(void*, zwp_input_method_v2*) {
    gi.pending_deactivate = true;
    std::printf("im deactivate\n");
}
void im_on_surrounding(void*, zwp_input_method_v2*, const char* text, uint32_t cursor, uint32_t) {
    copy_text(gi.surrounding, text);
    gi.s_cursor = cursor;
}
void im_on_cause(void*, zwp_input_method_v2*, uint32_t) {}
void im_on_content_type(void*, zwp_input_method_v2*, uint32_t hint, uint32_t purpose) {
    gi.hint = hint;
    gi.purpose = purpose;
}
void im_on_done(void*, zwp_input_method_v2*) {
    ++gi.dones;
    if (gi.pending_deactivate) {
        gi.pending_deactivate = false;
        gi.active = false;
        // fcitx5: the grab goes with the context; whatever was composing is dropped.
        if (gi.grab) {
            zwp_input_method_keyboard_grab_v2_release(gi.grab);
            gi.grab = nullptr;
        }
        for (unsigned k = 0; k < 256; ++k)
            if (gi.forwarded[k] && gi.vk_ready) {
                zwp_virtual_keyboard_v1_key(gi.vk, 0, k, WL_KEYBOARD_KEY_STATE_RELEASED);
                gi.forwarded[k] = false;
            }
        gi.composing_len = 0;
        gi.composing[0] = '\0';
    }
    if (gi.pending_activate) {
        gi.pending_activate = false;
        gi.active = true;
        if (gi.grab)
            zwp_input_method_keyboard_grab_v2_release(gi.grab);
        gi.grab = zwp_input_method_v2_grab_keyboard(gi.im);
        zwp_input_method_keyboard_grab_v2_add_listener(gi.grab, &grab_listener, nullptr);
    }
    std::printf("im done n=%u active=%d surrounding=\"%s\" cursor=%u purpose=%u hint=%u\n",
                gi.dones, gi.active ? 1 : 0, gi.surrounding, gi.s_cursor, gi.purpose, gi.hint);
}
void im_on_unavailable(void*, zwp_input_method_v2*) {
    std::printf("im unavailable\n");
    g.closed = true;
}
const zwp_input_method_v2_listener im_listener = {im_on_activate, im_on_deactivate,
                                                  im_on_surrounding, im_on_cause,
                                                  im_on_content_type, im_on_done,
                                                  im_on_unavailable};

void popup_on_rect(void*, zwp_input_popup_surface_v2*, int32_t x, int32_t y, int32_t w, int32_t h) {
    std::printf("im popup rect x=%d y=%d w=%d h=%d\n", x, y, w, h);
}
const zwp_input_popup_surface_v2_listener popup_rect_listener = {popup_on_rect};

/// Binds the input method, its virtual keyboard and its popup.
bool im_start() {
    if (!gi.im_manager || !gi.vk_manager) {
        std::fprintf(stderr, "lumen-probe: no zwp_input_method_manager_v2 / "
                             "zwp_virtual_keyboard_manager_v1\n");
        return false;
    }
    gi.vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(gi.vk_manager, g.seat);
    gi.im = zwp_input_method_manager_v2_get_input_method(gi.im_manager, g.seat);
    zwp_input_method_v2_add_listener(gi.im, &im_listener, nullptr);
    std::printf("im bound\n");
    if (!g.opt.no_popup) {
        const int w = g.opt.layer_popup ? g.opt.popup_w : 120;
        const int h = g.opt.layer_popup ? g.opt.popup_h : 40;
        gi.popup_surface = wl_compositor_create_surface(g.compositor);
        g_im_popup_surface = gi.popup_surface;
        gi.popup = zwp_input_method_v2_get_input_popup_surface(gi.im, gi.popup_surface);
        zwp_input_popup_surface_v2_add_listener(gi.popup, &popup_rect_listener, nullptr);
        wl_surface_attach(gi.popup_surface, make_buffer(w, h, 0xffe0c040), 0, 0);
        wl_surface_damage(gi.popup_surface, 0, 0, w, h);
        wl_surface_commit(gi.popup_surface);
    }
    return true;
}

#endif // LUMEN_PROBE_HAS_IME

void on_caps(void*, wl_seat* seat, uint32_t caps) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g.pointer) {
        g.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g.pointer, &pointer_listener, nullptr);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g.keyboard) {
        g.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g.keyboard, &keyboard_listener, nullptr);
    }
}
void on_seat_name(void*, wl_seat*, const char*) {}
const wl_seat_listener seat_listener = {on_caps, on_seat_name};

#if defined(LUMEN_PROBE_HAS_LAYER_SHELL)
void bind_layer_shell(wl_registry* r, uint32_t name, const char* iface, uint32_t version);
#endif

void on_global(void*, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        // v4 by default (dnd-source attaches with an offset, an error from v5 on); scale
        // and outputs modes take v6 for wl_surface.preferred_buffer_scale (per output).
        const uint32_t want = g.opt.scale_mode || g.opt.outputs_mode ? 6 : 4;
        g.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(r, name, &wl_compositor_interface, version < want ? version : want));
    }
#if defined(LUMEN_PROBE_HAS_SCALE)
    else if (std::strcmp(iface, wp_viewporter_interface.name) == 0)
        g.viewporter = static_cast<wp_viewporter*>(
            wl_registry_bind(r, name, &wp_viewporter_interface, 1));
    else if (std::strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0)
        g.fractional_manager = static_cast<wp_fractional_scale_manager_v1*>(
            wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1));
#endif
#if defined(LUMEN_PROBE_HAS_PRESENTATION)
    else if (std::strcmp(iface, wp_presentation_interface.name) == 0) {
        g.presentation = static_cast<wp_presentation*>(
            wl_registry_bind(r, name, &wp_presentation_interface, version < 2 ? version : 2));
        wp_presentation_add_listener(g.presentation, &presentation_listener, nullptr);
    }
#endif
#if defined(LUMEN_PROBE_HAS_IDLE_NOTIFY)
    else if (std::strcmp(iface, ext_idle_notifier_v1_interface.name) == 0)
        g.idle_notifier = static_cast<ext_idle_notifier_v1*>(
            wl_registry_bind(r, name, &ext_idle_notifier_v1_interface, version < 2 ? version : 2));
#endif
#if defined(LUMEN_PROBE_HAS_IDLE_INHIBIT)
    else if (std::strcmp(iface, zwp_idle_inhibit_manager_v1_interface.name) == 0)
        g.inhibit_manager = static_cast<zwp_idle_inhibit_manager_v1*>(
            wl_registry_bind(r, name, &zwp_idle_inhibit_manager_v1_interface, 1));
#endif
#if defined(LUMEN_PROBE_HAS_OUTPUT_POWER)
    else if (std::strcmp(iface, zwlr_output_power_manager_v1_interface.name) == 0)
        g.power_manager = static_cast<zwlr_output_power_manager_v1*>(
            wl_registry_bind(r, name, &zwlr_output_power_manager_v1_interface, 1));
#endif
#if defined(LUMEN_PROBE_HAS_SESSION_LOCK)
    else if (std::strcmp(iface, ext_session_lock_manager_v1_interface.name) == 0)
        g.lock_manager = static_cast<ext_session_lock_manager_v1*>(
            wl_registry_bind(r, name, &ext_session_lock_manager_v1_interface, 1));
#endif
#if defined(LUMEN_PROBE_HAS_IME)
    else if (std::strcmp(iface, zwp_text_input_manager_v3_interface.name) == 0) {
        gi.ti_version = version < 2 ? version : 2;
        gi.ti_manager = static_cast<zwp_text_input_manager_v3*>(
            wl_registry_bind(r, name, &zwp_text_input_manager_v3_interface, gi.ti_version));
    } else if (std::strcmp(iface, zwp_input_method_manager_v2_interface.name) == 0)
        gi.im_manager = static_cast<zwp_input_method_manager_v2*>(
            wl_registry_bind(r, name, &zwp_input_method_manager_v2_interface, 1));
    else if (std::strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0)
        gi.vk_manager = static_cast<zwp_virtual_keyboard_manager_v1*>(
            wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
#endif
    else if (std::strcmp(iface, wl_shm_interface.name) == 0)
        g.shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
    else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0)
        g.wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(r, name, &xdg_wm_base_interface, version < 3 ? version : 3));
    else if (std::strcmp(iface, wl_seat_interface.name) == 0)
        g.seat = static_cast<wl_seat*>(
            wl_registry_bind(r, name, &wl_seat_interface, version < 5 ? version : 5));
    else if (std::strcmp(iface, wl_data_device_manager_interface.name) == 0)
        g.ddm = static_cast<wl_data_device_manager*>(
            wl_registry_bind(r, name, &wl_data_device_manager_interface, version < 3 ? version : 3));
#if defined(LUMEN_PROBE_HAS_PRIMARY_SELECTION)
    else if (std::strcmp(iface, zwp_primary_selection_device_manager_v1_interface.name) == 0)
        g.psm = static_cast<zwp_primary_selection_device_manager_v1*>(
            wl_registry_bind(r, name, &zwp_primary_selection_device_manager_v1_interface, 1));
#endif
    else if (std::strcmp(iface, wl_output_interface.name) == 0) {
        if (!g.output_global) {
            g.output_global = name;
            g.output_version = version;
        }
        if (g.out_count < k_max_outputs) {
            probe_output& o = g.outs[g.out_count++];
            o.global = name;
            o.version = version;
            // An output plugged in after the probe bound its outputs: watched as well; a
            // locker with --all-outputs covers it with a lock surface of its own.
            if ((g.opt.outputs_mode || (g.opt.lock_mode && g.opt.all_outputs)) && g.output) {
                std::printf("wl_output added global=%u\n", name);
                bind_one_output(o);
#if defined(LUMEN_PROBE_HAS_SESSION_LOCK)
                if (g.opt.lock_mode && g.lock_surface)
                    lock_add_output_surface(o);
#endif
            }
        }
    }
#if defined(LUMEN_PROBE_HAS_XDG_OUTPUT)
    else if (std::strcmp(iface, zxdg_output_manager_v1_interface.name) == 0)
        g.xom = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(r, name, &zxdg_output_manager_v1_interface, version < 3 ? version : 3));
#endif
#if defined(LUMEN_PROBE_HAS_ACTIVATION)
    else if (std::strcmp(iface, xdg_activation_v1_interface.name) == 0)
        g.activation = static_cast<xdg_activation_v1*>(
            wl_registry_bind(r, name, &xdg_activation_v1_interface, 1));
#endif
#if defined(LUMEN_PROBE_HAS_DECORATION)
    else if (std::strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0)
        g.deco_manager = static_cast<zxdg_decoration_manager_v1*>(
            wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1));
#endif
#if defined(LUMEN_PROBE_HAS_LAYER_SHELL)
    else
        bind_layer_shell(r, name, iface, version);
#endif
}
#if defined(LUMEN_PROBE_HAS_LAYER_SHELL)
void bind_layer_shell(wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    if (std::strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0)
        g.layer_shell = static_cast<zwlr_layer_shell_v1*>(
            wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, version < 5 ? version : 5));
}

void on_layer_configure(void*, zwlr_layer_surface_v1* l, uint32_t serial, uint32_t w,
                        uint32_t h) {
    zwlr_layer_surface_v1_ack_configure(l, serial);
    std::printf("layer configure serial=%u w=%u h=%u\n", serial, w, h);
    if (g.unmapped)
        return;
    const int dw = w ? static_cast<int>(w) : g.opt.width;
    const int dh = h ? static_cast<int>(h) : g.opt.height;
    if (!g.top.configured && g.opt.unmap_after > 0.0)
        g.unmap_at = now_s() + g.opt.unmap_after;
    g.top.configured = true;
    draw(g.top, dw, dh);
}
void on_layer_closed(void*, zwlr_layer_surface_v1*) {
    std::printf("layer closed\n");
    g.closed = true;
}
const zwlr_layer_surface_v1_listener layer_listener = {on_layer_configure, on_layer_closed};

unsigned anchor_bits(const char* list) {
    unsigned bits = 0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s", list);
    for (char* tok = std::strtok(buf, ","); tok; tok = std::strtok(nullptr, ",")) {
        if (!std::strcmp(tok, "top")) bits |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
        else if (!std::strcmp(tok, "bottom")) bits |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
        else if (!std::strcmp(tok, "left")) bits |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
        else if (!std::strcmp(tok, "right")) bits |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        else bits |= static_cast<unsigned>(std::atoi(tok));
    }
    return bits;
}

unsigned layer_value(const char* v) {
    if (!std::strcmp(v, "background")) return 0;
    if (!std::strcmp(v, "bottom")) return 1;
    if (!std::strcmp(v, "top")) return 2;
    if (!std::strcmp(v, "overlay")) return 3;
    return static_cast<unsigned>(std::atoi(v));
}

unsigned kbd_value(const char* v) {
    if (!std::strcmp(v, "none")) return 0;
    if (!std::strcmp(v, "exclusive")) return 1;
    if (!std::strcmp(v, "on_demand")) return 2;
    return static_cast<unsigned>(std::atoi(v));
}
#endif

void on_global_remove(void*, wl_registry*, uint32_t name) {
    for (unsigned i = 0; i < g.out_count; ++i) {
        probe_output& o = g.outs[i];
        if (o.global != name || o.gone)
            continue;
        o.gone = true;
        std::printf("wl_output removed name=%s\n", o.name[0] ? o.name : "?");
#if defined(LUMEN_PROBE_HAS_SESSION_LOCK)
        // Its lock surface goes with it (the compositor already dropped it).
        if (o.lock_surface) {
            ext_session_lock_surface_v1_destroy(o.lock_surface);
            o.lock_surface = nullptr;
            if (o.lock_surf.surface)
                wl_surface_destroy(o.lock_surf.surface);
            o.lock_surf.surface = nullptr;
        }
#endif
#if defined(LUMEN_PROBE_HAS_XDG_OUTPUT)
        if (o.xo)
            zxdg_output_v1_destroy(o.xo);
        o.xo = nullptr;
#endif
        // The object stays until released; the compositor keeps the global inert until then.
        if (o.out && wl_output_get_version(o.out) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
            wl_output_release(o.out);
        else if (o.out)
            wl_output_destroy(o.out);
        if (g.output == o.out)
            g.output = nullptr;
        o.out = nullptr;
    }
}
const wl_registry_listener registry_listener = {on_global, on_global_remove};

bool parse_pair(const char* s, int& a, int& b, char sep) {
    char* end = nullptr;
    a = static_cast<int>(std::strtol(s, &end, 10));
    if (!end || *end != sep)
        return false;
    b = static_cast<int>(std::strtol(end + 1, &end, 10));
    return end && *end == '\0';
}

int usage() {
    std::fprintf(stderr,
                 "usage: lumen-probe popup [--size WxH] [--popup WxH] [--anchor-rect x,y,w,h]\n"
                 "         [--anchor N] [--gravity N] [--adjust BITS] [--offset x,y]\n"
                 "         [--reactive] [--chain N] [--grab] [--grab-stale] [--reposition]\n"
                 "         [--exit-after S]\n"
                 "       lumen-probe layer [--layer L] [--anchor top,bottom,left,right] [--size WxH]\n"
                 "         [--zone Z] [--margin t,r,b,l] [--kbd none|exclusive|on_demand]\n"
                 "         [--edge E] [--ns NAME] [--popup WxH] [--grab] [--unmap-after S]\n"
                 "         [--exit-after S]\n"
                 "       lumen-probe selection [--kind clipboard|primary] [--mimes a,b]\n"
                 "         [--data STR] [--set-on press|key|focus|never] [--stale-serial]\n"
                 "         [--reuse-source] [--receive MIME] [--exit-after S]\n"
                 "       lumen-probe dnd-source [--mimes a,b] [--data STR] [--actions LIST]\n"
                 "         [--icon WxH] [--null-source] [--stale-serial] [--second-window]\n"
                 "         [--exit-during-drag S] [--exit-after S]\n"
                 "       lumen-probe dnd-target [--accept MIME|none] [--actions LIST]\n"
                 "         [--preferred A] [--no-finish] [--exit-after S]\n"
                 "       lumen-probe outputs [--late S] [--unmap-after S] [popup options]\n"
                 "       lumen-probe activation-issuer --token-file F [--no-serial]\n"
                 "       lumen-probe activation-target (--token STR | --token-file F) [--early]\n"
                 "       lumen-probe scale [--integer] [--buffer WxH] [--buffer-scale N]\n"
                 "         [--transform T] [--crop x,y,w,h] [--dst WxH]\n"
                 "       lumen-probe presentation [--frames N] [--burst K] [--maximize]\n"
                 "       lumen-probe idle [--timeout MS] [--input-only] [--count N]\n"
                 "       lumen-probe inhibit [--unmap-after S] [--destroy-after S]\n"
                 "       lumen-probe power [--set off|on] [--set-after S] [--then off|on]\n"
                 "         [--then-after S] [--invalid]\n"
                 "       lumen-probe lock [--unlock-after S] [--password STR] [--crash-after S]\n"
                 "         [--no-surfaces] [--surface-after S] [--wrong-size] [--colour C]\n"
                 "       lumen-probe text-input [--cursor x,y,w,h] [--disable-after S]\n"
                 "         [--enable-after S] [--no-enable]\n"
                 "       lumen-probe im [--popup WxH] [--no-popup] [--exit-after S]\n"
                 "       any toplevel mode: [--ignore-ping-for S] [--freeze S] [--ssd]\n"
                 "         [--title T] [--fullscreen] [--unset-fullscreen-after S]\n"
                 "       multi-output: [--output NAME] (layer, power, lock, fullscreen)\n"
                 "         [--all-outputs] (lock: a surface on every output)\n"
                 "       lumen-probe output-mgmt [--watch S] [--apply SPEC | --test SPEC]\n"
                 "         [--stale-serial] [--error KIND]  (SPEC: NAME:mode=WxH[@mHz],custom=WxH,\n"
                 "         pos=X,Y,scale=S,transform=T,vrr=0|1;NAME:off)\n"
                 "       lumen-probe timing [--frames N] [--timed N] [--step MS] [--size WxH]\n"
                 "       lumen-probe solid [--rgba R,G,B,A] [--alpha F] [--content-type T]\n"
                 "         [--size WxH] [--exit-after S]\n"
                 "       lumen-probe tearing [--exit-after S]\n"
                 "       lumen-probe dialog [--no-modal] | icon [--name N] [--sizes 16,32] |\n"
                 "         bell [--count N] [--null] | foreign-export [--v1] |\n"
                 "         foreign-import --handle H [--v1] [--modal] | shortcuts-inhibit |\n"
                 "         data-control watch|set TEXT|get|errors [--primary] [--wlr] |\n"
                 "         security-context [--app-id ID]   (each with [--exit-after S])\n"
                 "       lumen-probe constraints [--confine] [--oneshot] [--region X,Y,W,H]\n"
                 "         [--hint X,Y] [--size WxH] [--destroy-after S] | gestures\n"
                 "         (each with [--exit-after S])\n"
                 "       lumen-probe toplevels [--watch S]\n"
                 "       lumen-probe capture (--output NAME | --toplevel ID | --app-id A)\n"
                 "         [--dmabuf] [--frames N] [--cursor] [--expect R,G,B@X,Y]...\n"
                 "         [--save FILE.ppm] [--timeout S] [--buffers K]\n"
                 "       lumen-probe cursor-capture --output NAME [--frames N] [--timeout S]\n"
                 "       lumen-probe animate [--size WxH] [--box N] [--interval MS]\n"
                 "         [--colour R,G,B] [--exit-after S]\n");
    return 2;
}

} // namespace

int output_mgmt_main(int argc, char** argv);
int compat_main(int argc, char** argv);
int window_main(int argc, char** argv);
int pointer_main(int argc, char** argv);
int capture_main(int argc, char** argv);

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    // The commit-path protocols (fifo, commit-timing, single-pixel, alpha, content type,
    // tearing) have their own window and connection (lumen-probe-compat.cpp).
    if (argc >= 2 && (std::strcmp(argv[1], "timing") == 0 || std::strcmp(argv[1], "solid") == 0 ||
                      std::strcmp(argv[1], "tearing") == 0))
        return compat_main(argc, argv);
    // The window and session protocols (dialog, icon, bell, xdg-foreign, shortcuts inhibit,
    // data control, security context): lumen-probe-window.cpp.
    if (argc >= 2) {
        static const char* const k_window_modes[] = {"dialog", "icon", "bell", "foreign-export",
                                                     "foreign-import", "shortcuts-inhibit",
                                                     "data-control", "security-context"};
        for (const char* m : k_window_modes)
            if (std::strcmp(argv[1], m) == 0)
                return window_main(argc, argv);
    }
    // The pointer protocols (constraints, relative pointer, gestures):
    // lumen-probe-pointer.cpp.
    if (argc >= 2 && (std::strcmp(argv[1], "constraints") == 0 || std::strcmp(argv[1], "gestures") == 0))
        return pointer_main(argc, argv);
    // Screen capture (toplevel list, copy capture, cursor sessions, an animated window):
    // lumen-probe-capture.cpp.
    if (argc >= 2 && (std::strcmp(argv[1], "toplevels") == 0 || std::strcmp(argv[1], "capture") == 0 ||
                      std::strcmp(argv[1], "cursor-capture") == 0 ||
                      std::strcmp(argv[1], "animate") == 0))
        return capture_main(argc, argv);
    // wlr-output-management has its own connection and no surfaces
    // (lumen-probe-output-mgmt.cpp).
    if (argc >= 2 && std::strcmp(argv[1], "output-mgmt") == 0)
        return output_mgmt_main(argc, argv);
    if (argc < 2 || (std::strcmp(argv[1], "popup") != 0 && std::strcmp(argv[1], "layer") != 0 &&
                     std::strcmp(argv[1], "selection") != 0 &&
                     std::strcmp(argv[1], "dnd-source") != 0 &&
                     std::strcmp(argv[1], "dnd-target") != 0 &&
                     std::strcmp(argv[1], "outputs") != 0 &&
                     std::strcmp(argv[1], "activation-issuer") != 0 &&
                     std::strcmp(argv[1], "activation-target") != 0 &&
                     std::strcmp(argv[1], "scale") != 0 &&
                     std::strcmp(argv[1], "presentation") != 0 &&
                     std::strcmp(argv[1], "idle") != 0 && std::strcmp(argv[1], "inhibit") != 0 &&
                     std::strcmp(argv[1], "power") != 0 && std::strcmp(argv[1], "lock") != 0 &&
                     std::strcmp(argv[1], "text-input") != 0 && std::strcmp(argv[1], "im") != 0))
        return usage();
    options& o = g.opt;
    o.layer_mode = std::strcmp(argv[1], "layer") == 0;
    o.selection_mode = std::strcmp(argv[1], "selection") == 0;
    o.dnd_source_mode = std::strcmp(argv[1], "dnd-source") == 0;
    o.dnd_target_mode = std::strcmp(argv[1], "dnd-target") == 0;
    o.outputs_mode = std::strcmp(argv[1], "outputs") == 0;
    o.activation_issuer = std::strcmp(argv[1], "activation-issuer") == 0;
    o.activation_target = std::strcmp(argv[1], "activation-target") == 0;
    o.scale_mode = std::strcmp(argv[1], "scale") == 0;
    o.presentation_mode = std::strcmp(argv[1], "presentation") == 0;
    o.idle_mode = std::strcmp(argv[1], "idle") == 0;
    o.inhibit_mode = std::strcmp(argv[1], "inhibit") == 0;
    o.power_mode = std::strcmp(argv[1], "power") == 0;
    o.lock_mode = std::strcmp(argv[1], "lock") == 0;
    o.text_input_mode = std::strcmp(argv[1], "text-input") == 0;
    o.im_mode = std::strcmp(argv[1], "im") == 0;
    if (o.dnd_source_mode || o.dnd_target_mode)
        o.mimes = "text/plain;charset=utf-8,text/plain";
#if !defined(LUMEN_PROBE_HAS_LAYER_SHELL)
    if (o.layer_mode) {
        std::fprintf(stderr, "lumen-probe: built without wlr-layer-shell\n");
        return 2;
    }
#endif
    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!std::strcmp(a, "--size") && v && parse_pair(v, o.width, o.height, 'x')) ++i;
        else if (!std::strcmp(a, "--popup") && v && parse_pair(v, o.popup_w, o.popup_h, 'x')) {
            o.layer_popup = true;
            ++i;
        }
#if defined(LUMEN_PROBE_HAS_LAYER_SHELL)
        else if (!std::strcmp(a, "--layer") && v) { o.layer = layer_value(v); ++i; }
        else if (o.layer_mode && !std::strcmp(a, "--anchor") && v) { o.layer_anchor = anchor_bits(v); ++i; }
        else if (!std::strcmp(a, "--zone") && v) { o.zone = std::atoi(v); ++i; }
        else if (!std::strcmp(a, "--margin") && v &&
                 std::sscanf(v, "%d,%d,%d,%d", &o.margin[0], &o.margin[1], &o.margin[2],
                             &o.margin[3]) == 4) ++i;
        else if (!std::strcmp(a, "--kbd") && v) { o.kbd = kbd_value(v); ++i; }
        else if (!std::strcmp(a, "--edge") && v) { o.edge = anchor_bits(v); ++i; }
        else if (!std::strcmp(a, "--ns") && v) { o.ns = v; ++i; }
        else if (!std::strcmp(a, "--unmap-after") && v) { o.unmap_after = std::atof(v); ++i; }
#endif
        else if (!std::strcmp(a, "--anchor-rect") && v &&
                 std::sscanf(v, "%d,%d,%d,%d", &o.ar_x, &o.ar_y, &o.ar_w, &o.ar_h) == 4) ++i;
        else if (!std::strcmp(a, "--anchor") && v) { o.anchor = static_cast<unsigned>(std::atoi(v)); ++i; }
        else if (!std::strcmp(a, "--gravity") && v) { o.gravity = static_cast<unsigned>(std::atoi(v)); ++i; }
        else if (!std::strcmp(a, "--adjust") && v) { o.adjust = static_cast<unsigned>(std::atoi(v)); ++i; }
        else if (!std::strcmp(a, "--offset") && v && parse_pair(v, o.off_x, o.off_y, ',')) ++i;
        else if (!std::strcmp(a, "--reactive")) o.reactive = true;
        else if (!std::strcmp(a, "--chain") && v) { o.chain = std::atoi(v); ++i; }
        else if (!std::strcmp(a, "--grab")) o.grab = true;
        else if (!std::strcmp(a, "--grab-stale")) o.grab_stale = true;
        else if (!std::strcmp(a, "--reposition")) o.reposition = true;
        else if (!std::strcmp(a, "--exit-after") && v) { o.exit_after = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--kind") && v) { o.primary = !std::strcmp(v, "primary"); ++i; }
        else if (!std::strcmp(a, "--mimes") && v) { o.mimes = v; ++i; }
        else if (!std::strcmp(a, "--data") && v) { o.data = v; ++i; }
        else if (!std::strcmp(a, "--set-on") && v) {
            o.trigger = !std::strcmp(v, "key")     ? options::set_on::key
                        : !std::strcmp(v, "focus") ? options::set_on::focus
                        : !std::strcmp(v, "never") ? options::set_on::never
                                                   : options::set_on::press;
            ++i;
        }
        else if (!std::strcmp(a, "--stale-serial")) o.stale_serial = true;
        else if (!std::strcmp(a, "--reuse-source")) o.reuse_source = true;
        else if (!std::strcmp(a, "--receive") && v) { o.receive = v; ++i; }
        else if (!std::strcmp(a, "--actions") && v) { o.dnd_actions = action_bits(v); ++i; }
        else if (!std::strcmp(a, "--preferred") && v) { o.dnd_preferred = action_bits(v); ++i; }
        else if (!std::strcmp(a, "--accept") && v) {
            o.dnd_accept = !std::strcmp(v, "none") ? nullptr : v;
            ++i;
        }
        else if (!std::strcmp(a, "--no-finish")) o.no_finish = true;
        else if (!std::strcmp(a, "--icon") && v && parse_pair(v, o.icon_w, o.icon_h, 'x')) ++i;
        else if (!std::strcmp(a, "--null-source")) o.null_source = true;
        else if (!std::strcmp(a, "--second-window")) o.second_window = true;
        else if (!std::strcmp(a, "--exit-during-drag") && v) { o.exit_during_drag = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--late") && v) { o.late_output = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--ignore-ping-for") && v) { o.ignore_ping_for = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--freeze") && v) { o.freeze = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--ssd")) o.ssd = true;
        else if (!std::strcmp(a, "--title") && v) { o.title = v; ++i; }
        else if (!std::strcmp(a, "--unmap-after") && v) { o.unmap_after = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--token-file") && v) { o.token_file = v; ++i; }
        else if (!std::strcmp(a, "--token") && v) { o.token = v; ++i; }
        else if (!std::strcmp(a, "--no-serial")) o.no_serial = true;
        else if (!std::strcmp(a, "--early")) o.early = true;
        else if (!std::strcmp(a, "--integer")) o.integer_scale = true;
        else if (!std::strcmp(a, "--buffer") && v && parse_pair(v, o.vp_buffer_w, o.vp_buffer_h, 'x')) ++i;
        else if (!std::strcmp(a, "--buffer-scale") && v) { o.buffer_scale = std::atoi(v); ++i; }
        else if (!std::strcmp(a, "--transform") && v) { o.transform = std::atoi(v); ++i; }
        else if (!std::strcmp(a, "--crop") && v &&
                 std::sscanf(v, "%lf,%lf,%lf,%lf", &o.crop_x, &o.crop_y, &o.crop_w, &o.crop_h) == 4) {
            o.crop = true;
            ++i;
        }
        else if (!std::strcmp(a, "--dst") && v && parse_pair(v, o.dst_w, o.dst_h, 'x')) ++i;
        else if (!std::strcmp(a, "--frames") && v) { o.frames = std::atoi(v); ++i; }
        else if (!std::strcmp(a, "--burst") && v) { o.burst = std::atoi(v) > 0 ? std::atoi(v) : 1; ++i; }
        else if (!std::strcmp(a, "--maximize")) o.maximize = true;
        else if (!std::strcmp(a, "--output") && v) { o.output_name = v; ++i; }
        else if (!std::strcmp(a, "--all-outputs")) o.all_outputs = true;
        else if (!std::strcmp(a, "--fullscreen")) o.fullscreen = true;
        else if (!std::strcmp(a, "--unset-fullscreen-after") && v) {
            o.unfullscreen_after = std::atof(v);
            ++i;
        }
        else if (!std::strcmp(a, "--timeout") && v) { o.idle_timeout_ms = static_cast<unsigned>(std::atol(v)); ++i; }
        else if (!std::strcmp(a, "--input-only")) o.input_only = true;
        else if (!std::strcmp(a, "--count") && v) { o.idle_count = std::atoi(v); ++i; }
        else if (!std::strcmp(a, "--destroy-after") && v) { o.destroy_after = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--set") && v) { o.power_set = !std::strcmp(v, "on") ? 1 : 0; ++i; }
        else if (!std::strcmp(a, "--set-after") && v) { o.power_set_after = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--then") && v) { o.power_then = !std::strcmp(v, "on") ? 1 : 0; ++i; }
        else if (!std::strcmp(a, "--then-after") && v) { o.power_then_after = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--invalid")) o.power_invalid = true;
        else if (!std::strcmp(a, "--unlock-after") && v) { o.unlock_after = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--crash-after") && v) { o.crash_after = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--surface-after") && v) { o.surface_after = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--no-surfaces")) o.no_surfaces = true;
        else if (!std::strcmp(a, "--wrong-size")) o.wrong_size = true;
        else if (!std::strcmp(a, "--password") && v) { o.password = v; ++i; }
        else if (!std::strcmp(a, "--cursor") && v &&
                 std::sscanf(v, "%d,%d,%d,%d", &o.cursor_x, &o.cursor_y, &o.cursor_w,
                             &o.cursor_h) == 4) ++i;
        else if (!std::strcmp(a, "--disable-after") && v) { o.disable_after = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--enable-after") && v) { o.enable_after = std::atof(v); ++i; }
        else if (!std::strcmp(a, "--no-enable")) o.no_enable = true;
        else if (!std::strcmp(a, "--no-popup")) o.no_popup = true;
        else if (!std::strcmp(a, "--colour") && v) {
            o.lock_colour = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
            ++i;
        }
        else return usage();
    }
    if (o.chain < 1)
        o.chain = 1;
    if (o.chain > k_max_popups)
        o.chain = k_max_popups;

    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::signal(SIGINT, [](int) { g_stop = 1; });

    g.display = wl_display_connect(nullptr);
    if (!g.display) {
        std::fprintf(stderr, "lumen-probe: cannot connect to the Wayland display\n");
        return 1;
    }
    wl_registry* registry = wl_display_get_registry(g.display);
    g.registry = registry;
    wl_registry_add_listener(registry, &registry_listener, nullptr);
    wl_display_roundtrip(g.display);
    if (!g.compositor || !g.shm || !g.wm_base || !g.seat) {
        std::fprintf(stderr, "lumen-probe: missing a required global\n");
        return 1;
    }
    xdg_wm_base_add_listener(g.wm_base, &wm_base_listener, nullptr);
    wl_seat_add_listener(g.seat, &seat_listener, nullptr);
    // Before the window exists: the devices must be there when the keyboard enter comes.
    create_selection_devices();

    g.top.colour = 0xff5060a0;
#if defined(LUMEN_PROBE_HAS_LAYER_SHELL)
    if (o.layer_mode) {
        if (!g.layer_shell) {
            std::fprintf(stderr, "lumen-probe: no zwlr_layer_shell_v1\n");
            return 1;
        }
        g.top.colour = 0xff3a7a50;
        g.top.surface = wl_compositor_create_surface(g.compositor);
        // --output NAME: on that output; otherwise the compositor picks (null).
        if (o.output_name) {
            bind_outputs();
            if (!g.output)
                return 1;
        }
        g.layer = zwlr_layer_shell_v1_get_layer_surface(g.layer_shell, g.top.surface,
                                                        o.output_name ? g.output : nullptr,
                                                        o.layer, o.ns);
        zwlr_layer_surface_v1_add_listener(g.layer, &layer_listener, nullptr);
        zwlr_layer_surface_v1_set_size(g.layer, static_cast<uint32_t>(o.width),
                                       static_cast<uint32_t>(o.height));
        zwlr_layer_surface_v1_set_anchor(g.layer, o.layer_anchor);
        zwlr_layer_surface_v1_set_exclusive_zone(g.layer, o.zone);
        zwlr_layer_surface_v1_set_margin(g.layer, o.margin[0], o.margin[1], o.margin[2],
                                         o.margin[3]);
        zwlr_layer_surface_v1_set_keyboard_interactivity(g.layer, o.kbd);
        if (o.edge && zwlr_layer_surface_v1_get_version(g.layer) >= 5)
            zwlr_layer_surface_v1_set_exclusive_edge(g.layer, o.edge);
        wl_surface_commit(g.top.surface);
    }
#endif
    if (o.outputs_mode && o.late_output <= 0.0)
        bind_outputs();
#if defined(LUMEN_PROBE_HAS_PRESENTATION)
    if (o.presentation_mode) {
        if (!g.presentation) {
            std::fprintf(stderr, "lumen-probe: no wp_presentation\n");
            return 1;
        }
        bind_outputs(); // sync_output names the wl_output it was presented on
    }
#else
    if (o.presentation_mode) {
        std::fprintf(stderr, "lumen-probe: built without presentation-time\n");
        return 2;
    }
#endif
#if defined(LUMEN_PROBE_HAS_IDLE_NOTIFY)
    if (o.idle_mode) {
        if (!g.idle_notifier) {
            std::fprintf(stderr, "lumen-probe: no ext_idle_notifier_v1\n");
            return 1;
        }
        const uint32_t v = ext_idle_notifier_v1_get_version(g.idle_notifier);
        if (o.input_only && v < 2) {
            std::fprintf(stderr, "lumen-probe: ext_idle_notifier_v1 v%u has no input-only\n", v);
            return 1;
        }
        g.idle_notification =
            o.input_only ? ext_idle_notifier_v1_get_input_idle_notification(
                               g.idle_notifier, o.idle_timeout_ms, g.seat)
                         : ext_idle_notifier_v1_get_idle_notification(g.idle_notifier,
                                                                      o.idle_timeout_ms, g.seat);
        ext_idle_notification_v1_add_listener(g.idle_notification, &idle_listener, nullptr);
        g.idle_created_at = now_s();
        std::printf("idle notification timeout=%u input_only=%d version=%u\n", o.idle_timeout_ms,
                    o.input_only ? 1 : 0, v);
    }
#else
    if (o.idle_mode) {
        std::fprintf(stderr, "lumen-probe: built without ext-idle-notify\n");
        return 2;
    }
#endif
#if defined(LUMEN_PROBE_HAS_OUTPUT_POWER)
    if (o.power_mode) {
        bind_outputs();
        if (!g.power_manager || !g.output) {
            std::fprintf(stderr, "lumen-probe: no zwlr_output_power_manager_v1 or wl_output\n");
            return 1;
        }
        g.power = zwlr_output_power_manager_v1_get_output_power(g.power_manager, g.output);
        zwlr_output_power_v1_add_listener(g.power, &power_listener, nullptr);
    }
#else
    if (o.power_mode) {
        std::fprintf(stderr, "lumen-probe: built without wlr-output-power-management\n");
        return 2;
    }
#endif
#if defined(LUMEN_PROBE_HAS_SESSION_LOCK)
    if (o.lock_mode) {
        bind_outputs();
        if (!g.lock_manager || !g.output) {
            std::fprintf(stderr, "lumen-probe: no ext_session_lock_manager_v1 or wl_output\n");
            return 1;
        }
        g.lock = ext_session_lock_manager_v1_lock(g.lock_manager);
        ext_session_lock_v1_add_listener(g.lock, &lock_listener, nullptr);
        g.lock_requested_at = now_s();
        std::printf("lock requested at=%.3f\n", g.lock_requested_at);
        // "Clients should immediately create lock surfaces for all outputs."
        if (!o.no_surfaces) {
            if (o.surface_after > 0.0)
                g.lock_surface_at = now_s() + o.surface_after;
            else
                lock_make_surface();
        }
    }
#else
    if (o.lock_mode) {
        std::fprintf(stderr, "lumen-probe: built without ext-session-lock\n");
        return 2;
    }
#endif
#if defined(LUMEN_PROBE_HAS_IME)
    if (o.im_mode && !im_start())
        return 1;
#else
    if (o.im_mode || o.text_input_mode) {
        std::fprintf(stderr, "lumen-probe: built without text-input / input-method\n");
        return 2;
    }
#endif
    if (!o.layer_mode && !o.idle_mode && !o.power_mode && !o.lock_mode && !o.im_mode) {
        g.top.surface = wl_compositor_create_surface(g.compositor);
#if defined(LUMEN_PROBE_HAS_IME)
        if (o.text_input_mode) {
            if (!gi.ti_manager) {
                std::fprintf(stderr, "lumen-probe: no zwp_text_input_manager_v3\n");
                return 1;
            }
            gi.ti = zwp_text_input_manager_v3_get_text_input(gi.ti_manager, g.seat);
            zwp_text_input_v3_add_listener(gi.ti, &ti_listener, nullptr);
            std::printf("ti created version=%u\n", gi.ti_version);
        }
#endif
#if defined(LUMEN_PROBE_HAS_IDLE_INHIBIT)
        if (o.inhibit_mode) {
            if (!g.inhibit_manager) {
                std::fprintf(stderr, "lumen-probe: no zwp_idle_inhibit_manager_v1\n");
                return 1;
            }
            g.inhibitor =
                zwp_idle_inhibit_manager_v1_create_inhibitor(g.inhibit_manager, g.top.surface);
            std::printf("inhibitor created\n");
            if (o.destroy_after > 0.0)
                g.destroy_inhibitor_at = now_s() + o.destroy_after;
        }
#else
        if (o.inhibit_mode) {
            std::fprintf(stderr, "lumen-probe: built without idle-inhibit\n");
            return 2;
        }
#endif
        if (o.outputs_mode || o.scale_mode)
            wl_surface_add_listener(g.top.surface, &surface_listener, nullptr);
#if defined(LUMEN_PROBE_HAS_SCALE)
        if (o.scale_mode) {
            if (!g.viewporter || !g.fractional_manager) {
                std::fprintf(stderr, "lumen-probe: no wp_viewporter / wp_fractional_scale\n");
                return 1;
            }
            g.viewport = wp_viewporter_get_viewport(g.viewporter, g.top.surface);
            if (!o.integer_scale) {
                g.fractional =
                    wp_fractional_scale_manager_v1_get_fractional_scale(g.fractional_manager,
                                                                        g.top.surface);
                wp_fractional_scale_v1_add_listener(g.fractional, &fractional_listener, nullptr);
            }
        }
#endif
        g.top.xdg = xdg_wm_base_get_xdg_surface(g.wm_base, g.top.surface);
        xdg_surface_add_listener(g.top.xdg, &xdg_listener, &g.top);
        g.top.toplevel = xdg_surface_get_toplevel(g.top.xdg);
        xdg_toplevel_add_listener(g.top.toplevel, &toplevel_listener, &g.top);
        xdg_toplevel_set_title(g.top.toplevel, o.title);
        xdg_toplevel_set_app_id(g.top.toplevel, "lumen-probe");
        if (o.maximize)
            xdg_toplevel_set_maximized(g.top.toplevel);
        if (o.fullscreen) {
            if (o.output_name) {
                bind_outputs();
                if (!g.output)
                    return 1;
            }
            xdg_toplevel_set_fullscreen(g.top.toplevel, o.output_name ? g.output : nullptr);
            if (o.unfullscreen_after > 0.0)
                g.unfullscreen_at = now_s() + o.unfullscreen_after;
        }
#if defined(LUMEN_PROBE_HAS_DECORATION)
        if (o.ssd && g.deco_manager) {
            // Before the initial commit: the first configure then carries the mode.
            g.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(g.deco_manager,
                                                                        g.top.toplevel);
            zxdg_toplevel_decoration_v1_add_listener(g.deco, &deco_listener, nullptr);
            zxdg_toplevel_decoration_v1_set_mode(g.deco,
                                                 ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
#endif
        if (o.activation_target && o.early)
            try_activate(false); // before the initial commit: not mapped yet
        wl_surface_commit(g.top.surface);
        if (o.second_window) {
            // A second toplevel of this client: a drop target inside the source's own client.
            g.second.index = -2;
            g.second.colour = 0xff50a060;
            g.second.surface = wl_compositor_create_surface(g.compositor);
            g.second.xdg = xdg_wm_base_get_xdg_surface(g.wm_base, g.second.surface);
            xdg_surface_add_listener(g.second.xdg, &xdg_listener, &g.second);
            g.second.toplevel = xdg_surface_get_toplevel(g.second.xdg);
            xdg_toplevel_add_listener(g.second.toplevel, &toplevel_listener, &g.second);
            xdg_toplevel_set_title(g.second.toplevel, "lumen-probe-2");
            xdg_toplevel_set_app_id(g.second.toplevel, "lumen-probe-2");
            wl_surface_commit(g.second.surface);
        }
    }

    const double deadline = now_s() + o.exit_after;
    int rc = 0;
    while (!g_stop && !g.closed && now_s() < deadline) {
        if (g.exit_at > 0.0 && now_s() >= g.exit_at) {
            // Client death mid-drag: no cleanup, no goodbye — the socket just closes.
            std::printf("dnd source exiting\n");
            std::fflush(stdout);
            _exit(0);
        }
        if (g.unmap_at > 0.0 && now_s() >= g.unmap_at && !g.unmapped) {
            // Null buffer: the surface unmaps and goes back to before its initial commit;
            // this probe stays unmapped until it exits.
            g.unmapped = true;
            wl_surface_attach(g.top.surface, nullptr, 0, 0);
            wl_surface_commit(g.top.surface);
            std::printf("%s unmapped\n", o.layer_mode ? "layer" : "toplevel");
        }
        if (o.activation_target && !o.early && g.top.configured)
            try_activate(true);
#if defined(LUMEN_PROBE_HAS_IME)
        if (gi.disable_at > 0.0 && now_s() >= gi.disable_at) {
            gi.disable_at = 0.0;
            ti_disable_now();
        }
        if (gi.enable_at > 0.0 && now_s() >= gi.enable_at) {
            gi.enable_at = 0.0;
            ti_enable_now();
        }
#endif
#if defined(LUMEN_PROBE_HAS_IDLE_INHIBIT)
        if (g.destroy_inhibitor_at > 0.0 && now_s() >= g.destroy_inhibitor_at && g.inhibitor) {
            g.destroy_inhibitor_at = 0.0;
            zwp_idle_inhibitor_v1_destroy(g.inhibitor);
            g.inhibitor = nullptr;
            std::printf("inhibitor destroyed\n");
        }
#endif
#if defined(LUMEN_PROBE_HAS_SESSION_LOCK)
        if (g.lock_surface_at > 0.0 && now_s() >= g.lock_surface_at) {
            g.lock_surface_at = 0.0;
            lock_make_surface();
        }
        if (g.unlock_at > 0.0 && now_s() >= g.unlock_at) {
            g.unlock_at = 0.0;
            lock_unlock("--unlock-after");
            break;
        }
        if (g.crash_at > 0.0 && now_s() >= g.crash_at) {
            // A locker that dies: no unlock, no goodbye — the socket just closes.
            std::printf("lock crash\n");
            std::fflush(stdout);
            _exit(0);
        }
#endif
#if defined(LUMEN_PROBE_HAS_OUTPUT_POWER)
        if (g.power_set_at > 0.0 && now_s() >= g.power_set_at) {
            g.power_set_at = 0.0;
            power_request(o.power_set);
            if (o.power_then >= 0)
                g.power_then_at = now_s() + o.power_then_after;
        }
        if (g.power_then_at > 0.0 && now_s() >= g.power_then_at) {
            g.power_then_at = 0.0;
            power_request(o.power_then);
        }
#endif
#if defined(LUMEN_PROBE_HAS_PRESENTATION)
        if (o.presentation_mode && g_pres.finished &&
            (feedback_pending() == 0 || now_s() >= g_pres.finish_deadline)) {
            present_summary();
            g_pres.summarized = true;
            break;
        }
#endif
        if (g.bind_output_at > 0.0 && now_s() >= g.bind_output_at) {
            g.bind_output_at = 0.0;
            bind_outputs();
        }
        if (g.unfullscreen_at > 0.0 && now_s() >= g.unfullscreen_at) {
            g.unfullscreen_at = 0.0;
            std::printf("toplevel unset_fullscreen\n");
            xdg_toplevel_unset_fullscreen(g.top.toplevel);
        }
        if (g.deferred_ping && g.ignore_until > 0.0 && now_s() >= g.ignore_until) {
            std::printf("pong serial=%u late\n", g.deferred_ping);
            xdg_wm_base_pong(g.wm_base, g.deferred_ping);
            g.deferred_ping = 0;
        }
        if (g.freeze_at > 0.0 && now_s() >= g.freeze_at) {
            // A hung client: nothing is read or dispatched; the compositor's events queue up
            // in the socket (and its connection buffer) meanwhile.
            g.freeze_at = 0.0;
            wl_display_flush(g.display);
            std::printf("freeze s=%g\n", o.freeze);
            std::fflush(stdout);
            const double until = now_s() + o.freeze;
            while (!g_stop && now_s() < until)
                usleep(20000);
            std::printf("thaw\n");
        }
        if (g.reposition_at > 0.0 && now_s() >= g.reposition_at && g.popup_count > 0 &&
            !g.popups[0].done) {
            g.reposition_at = 0.0;
            if (xdg_wm_base_get_version(g.wm_base) >= 3) {
                xdg_positioner* pos = positioner_for(0, 20, 20);
                xdg_popup_reposition(g.popups[0].popup, pos, 42);
                xdg_positioner_destroy(pos);
                std::printf("reposition idx=0 token=42\n");
            }
        }
        while (wl_display_prepare_read(g.display) != 0)
            wl_display_dispatch_pending(g.display);
        if (wl_display_flush(g.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(g.display);
            rc = 1;
            break;
        }
        pollfd pfd{wl_display_get_fd(g.display), POLLIN, 0};
        const int ready = poll(&pfd, 1, g.recv_fd >= 0 ? 10 : 50);
        pump_receive();
        if (ready > 0) {
            if (wl_display_read_events(g.display) < 0) {
                rc = 1;
                break;
            }
        } else {
            wl_display_cancel_read(g.display);
        }
        if (wl_display_dispatch_pending(g.display) < 0) {
            rc = 1;
            break;
        }
    }
#if defined(LUMEN_PROBE_HAS_PRESENTATION)
    if (o.presentation_mode && !g_pres.summarized)
        present_summary(); // stopped early (signal, --exit-after, closed)
#endif
    if (wl_display_get_error(g.display) == EPROTO) {
        const wl_interface* iface = nullptr;
        uint32_t id = 0;
        const uint32_t code = wl_display_get_protocol_error(g.display, &iface, &id);
        std::printf("protocol-error interface=%s code=%u\n", iface ? iface->name : "?", code);
        rc = 3;
    }
    wl_display_disconnect(g.display);
    return rc;
}
