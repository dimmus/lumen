// lumen-portal-probe: a deterministic client of xdg-desktop-portal for the portal checks of
// scripts/app-smoke.sh (group Z) and live verification (design-app-compat-capture F.3).
// It talks to the frontend (org.freedesktop.portal.Desktop) like an application does — or,
// with --backend, straight to xdg-desktop-portal-lumen, bypassing the frontend's permission
// store — and consumes a ScreenCast's PipeWire stream itself.
//
//   lumen-portal-probe screenshot [--interactive] [--target 1|2|4] [--backend] [--timeout S]
//       prints `response N` and, on success, `uri URI` and `file PATH size=WxH` (from the
//       PNG header).
//   lumen-portal-probe pick-color [--backend] [--timeout S]
//       prints `response N` and `color R,G,B` (0–255, rounded) / `color_raw r,g,b` (0–1).
//   lumen-portal-probe screencast [--types 1|2|3] [--cursor-mode 1|2|4] [--seconds S]
//       [--expect R,G,B@X,Y] [--dmabuf] [--timeout S]
//       CreateSession → SelectSources → Start (the source chooser runs here) →
//       OpenPipeWireRemote, then a PipeWire video stream of the node for S seconds (default
//       3): `started node=N size=WxH source_type=T`, `format WxH FMT`, `frame N bytes=B`
//       for the first three, `expect … OK|FAIL got=…` on the first frame after one second,
//       and `screencast frames=N seconds=S fps=F buffers=memfd|dmabuf`. With --dmabuf the
//       stream offers LINEAR or implicit dma-buf first (what a browser or OBS does), shared
//       memory as the fallback; a dma-buf frame is read for --expect through a mapping of the
//       buffer when the driver allows one. The session is closed at the end.
//   lumen-portal-probe version
//       the Screenshot and ScreenCast interface versions the frontend reports.
//
// Exit status: 0 when the request succeeded (and the cast delivered frames), 1 otherwise.

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-buf.h>

#include <systemd/sd-bus.h>

#if defined(LUMEN_PROBE_HAS_PIPEWIRE)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc99-extensions"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wgnu-statement-expression-from-macro-expansion"
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/pod/builder.h>
#pragma clang diagnostic pop
#endif

namespace {

constexpr const char* k_desktop = "org.freedesktop.portal.Desktop";
constexpr const char* k_path = "/org/freedesktop/portal/desktop";
constexpr const char* k_backend = "org.freedesktop.impl.portal.desktop.lumen";

double now_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

struct response {
    bool done = false;
    unsigned code = 2;
    std::string uri;
    std::string session_handle;
    double r = -1, g = -1, b = -1;
    struct stream {
        unsigned node = 0;
        int w = 0, h = 0;
        unsigned source_type = 0;
    };
    std::vector<stream> streams;
};

/// Reads an a{sv} results dict of a Response (only the keys the probe needs).
int read_results(sd_bus_message* m, response& out) {
    int r = sd_bus_message_enter_container(m, 'a', "{sv}");
    if (r < 0)
        return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char* key = nullptr;
        if ((r = sd_bus_message_read(m, "s", &key)) < 0)
            return r;
        if (std::strcmp(key, "uri") == 0 || std::strcmp(key, "session_handle") == 0) {
            const char* v = nullptr;
            const char* type = std::strcmp(key, "uri") == 0 ? "s" : "s";
            if (sd_bus_message_verify_type(m, 'v', "o") > 0)
                type = "o";
            if ((r = sd_bus_message_read(m, "v", type, &v)) < 0)
                return r;
            (std::strcmp(key, "uri") == 0 ? out.uri : out.session_handle) = v ? v : "";
        } else if (std::strcmp(key, "color") == 0) {
            if ((r = sd_bus_message_read(m, "v", "(ddd)", &out.r, &out.g, &out.b)) < 0)
                return r;
        } else if (std::strcmp(key, "streams") == 0) {
            if ((r = sd_bus_message_enter_container(m, 'v', "a(ua{sv})")) < 0 ||
                (r = sd_bus_message_enter_container(m, 'a', "(ua{sv})")) < 0)
                return r;
            while ((r = sd_bus_message_enter_container(m, 'r', "ua{sv}")) > 0) {
                response::stream s;
                if ((r = sd_bus_message_read(m, "u", &s.node)) < 0 ||
                    (r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0)
                    return r;
                while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
                    const char* k = nullptr;
                    if ((r = sd_bus_message_read(m, "s", &k)) < 0)
                        return r;
                    if (std::strcmp(k, "size") == 0) {
                        r = sd_bus_message_read(m, "v", "(ii)", &s.w, &s.h);
                    } else if (std::strcmp(k, "source_type") == 0) {
                        r = sd_bus_message_read(m, "v", "u", &s.source_type);
                    } else {
                        r = sd_bus_message_skip(m, "v");
                    }
                    if (r < 0 || (r = sd_bus_message_exit_container(m)) < 0)
                        return r;
                }
                sd_bus_message_exit_container(m); // a{sv}
                sd_bus_message_exit_container(m); // (ua{sv})
                out.streams.push_back(s);
            }
            sd_bus_message_exit_container(m); // a
            sd_bus_message_exit_container(m); // v
        } else if ((r = sd_bus_message_skip(m, "v")) < 0) {
            return r;
        }
        if ((r = sd_bus_message_exit_container(m)) < 0)
            return r;
    }
    return sd_bus_message_exit_container(m);
}

int on_response(sd_bus_message* m, void* user, sd_bus_error*) {
    auto& out = *static_cast<response*>(user);
    if (sd_bus_message_read(m, "u", &out.code) < 0)
        out.code = 2;
    else
        (void)read_results(m, out);
    out.done = true;
    return 0;
}

std::string token(const char* what) {
    static unsigned n = 0;
    char buf[64];
    std::snprintf(buf, sizeof buf, "lumen_probe_%s_%d_%u", what, static_cast<int>(getpid()), ++n);
    return buf;
}

/// The frontend's request path for `tok`: /org/freedesktop/portal/desktop/request/SENDER/TOKEN.
std::string request_path(sd_bus* bus, const std::string& tok) {
    const char* unique = nullptr;
    sd_bus_get_unique_name(bus, &unique);
    std::string sender = unique ? unique + 1 : "";
    for (char& c : sender)
        if (c == '.')
            c = '_';
    return std::string(k_path) + "/request/" + sender + "/" + tok;
}

bool wait_response(sd_bus* bus, response& out, double timeout_s) {
    const double deadline = now_s() + timeout_s;
    while (!out.done) {
        const int r = sd_bus_process(bus, nullptr);
        if (r < 0)
            return false;
        if (r > 0)
            continue;
        const double left = deadline - now_s();
        if (left <= 0)
            return false;
        sd_bus_wait(bus, static_cast<uint64_t>(left * 1e6));
    }
    return true;
}

/// Calls a portal method whose reply is a request handle (frontend) and waits for its
/// Response. `append` adds the arguments after the method's own leading ones.
template <typename Append>
bool request(sd_bus* bus, const char* iface, const char* method, const std::string& tok, Append append,
             response& out, double timeout_s) {
    const std::string path = request_path(bus, tok);
    sd_bus_slot* slot = nullptr;
    if (sd_bus_match_signal(bus, &slot, k_desktop, path.c_str(), "org.freedesktop.portal.Request", "Response",
                            on_response, &out) < 0) {
        std::printf("error cannot watch %s\n", path.c_str());
        return false;
    }
    sd_bus_message* call = nullptr;
    sd_bus_message_new_method_call(bus, &call, k_desktop, k_path, iface, method);
    append(call);
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    const int r = sd_bus_call(bus, call, 0, &err, &reply);
    sd_bus_message_unref(call);
    if (r < 0) {
        std::printf("error %s.%s: %s\n", iface, method, err.message ? err.message : std::strerror(-r));
        sd_bus_error_free(&err);
        sd_bus_slot_unref(slot);
        return false;
    }
    sd_bus_message_unref(reply);
    const bool ok = wait_response(bus, out, timeout_s);
    sd_bus_slot_unref(slot);
    if (!ok)
        std::printf("error no Response within %.0f s\n", timeout_s);
    return ok;
}

bool png_size(const std::string& path, int& w, int& h) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    unsigned char hdr[24];
    const bool ok = read(fd, hdr, sizeof hdr) == static_cast<ssize_t>(sizeof hdr) && hdr[1] == 'P' && hdr[2] == 'N' &&
                    hdr[3] == 'G';
    close(fd);
    if (!ok)
        return false;
    w = (hdr[16] << 24) | (hdr[17] << 16) | (hdr[18] << 8) | hdr[19];
    h = (hdr[20] << 24) | (hdr[21] << 16) | (hdr[22] << 8) | hdr[23];
    return true;
}

std::string uri_path(const std::string& uri) {
    if (uri.rfind("file://", 0) != 0)
        return {};
    std::string out;
    for (std::size_t i = 7; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            out.push_back(static_cast<char>(std::strtol(uri.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(uri[i]);
        }
    }
    return out;
}

void append_dict_entry_b(sd_bus_message* m, const char* k, bool v) {
    sd_bus_message_append(m, "{sv}", k, "b", v ? 1 : 0);
}

int screenshot_main(sd_bus* bus, bool interactive, unsigned target, bool backend, double timeout, bool pick) {
    response out;
    if (backend) {
        // Straight to the backend: its impl interface, a made-up handle.
        const std::string handle = std::string(k_path) + "/request/lumen_probe/" + token("impl");
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* call = nullptr;
        sd_bus_message_new_method_call(bus, &call, k_backend, k_path, "org.freedesktop.impl.portal.Screenshot",
                                       pick ? "PickColor" : "Screenshot");
        sd_bus_message_append(call, "oss", handle.c_str(), "lumen-portal-probe", "");
        sd_bus_message_open_container(call, 'a', "{sv}");
        if (!pick) {
            append_dict_entry_b(call, "interactive", interactive);
            if (target)
                sd_bus_message_append(call, "{sv}", "target", "u", target);
        }
        sd_bus_message_close_container(call);
        sd_bus_message* reply = nullptr;
        const int r = sd_bus_call(bus, call, static_cast<uint64_t>(timeout * 1e6), &err, &reply);
        sd_bus_message_unref(call);
        if (r < 0) {
            std::printf("error backend call: %s\n", err.message ? err.message : std::strerror(-r));
            sd_bus_error_free(&err);
            return 1;
        }
        on_response(reply, &out, nullptr);
        sd_bus_message_unref(reply);
    } else {
        const std::string tok = token(pick ? "pick" : "shot");
        const bool ok = request(
            bus, "org.freedesktop.portal.Screenshot", pick ? "PickColor" : "Screenshot", tok,
            [&](sd_bus_message* m) {
                sd_bus_message_append(m, "s", "");
                sd_bus_message_open_container(m, 'a', "{sv}");
                sd_bus_message_append(m, "{sv}", "handle_token", "s", tok.c_str());
                if (!pick) {
                    append_dict_entry_b(m, "interactive", interactive);
                    if (target)
                        sd_bus_message_append(m, "{sv}", "target", "u", target);
                }
                sd_bus_message_close_container(m);
            },
            out, timeout);
        if (!ok)
            return 1;
    }
    std::printf("response %u\n", out.code);
    if (out.code != 0)
        return 1;
    if (pick) {
        std::printf("color_raw %.6f,%.6f,%.6f\n", out.r, out.g, out.b);
        std::printf("color %ld,%ld,%ld\n", std::lround(out.r * 255.0), std::lround(out.g * 255.0),
                    std::lround(out.b * 255.0));
        return out.r >= 0 ? 0 : 1;
    }
    std::printf("uri %s\n", out.uri.c_str());
    const std::string path = uri_path(out.uri);
    int w = 0, h = 0;
    if (path.empty() || !png_size(path, w, h)) {
        std::printf("error the uri is not a readable PNG\n");
        return 1;
    }
    std::printf("file %s size=%dx%d\n", path.c_str(), w, h);
    return 0;
}

#if defined(LUMEN_PROBE_HAS_PIPEWIRE)

struct cast_state {
    pw_main_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    spa_video_info_raw format{};
    bool have_format = false;
    unsigned frames = 0;
    double first_frame = 0;
    double started = 0;
    bool expect = false;
    int ex = 0, ey = 0, er = 0, eg = 0, eb = 0;
    bool expect_done = false;
    bool expect_ok = false;
    bool want_dmabuf = false;
    bool dmabuf = false;   // the negotiated format carries a modifier: dma-buf buffers
    unsigned dmabuf_frames = 0;
};

/// The pixel at (x, y) of a frame (4 bytes per pixel), mapping a dma-buf for the read when
/// it is one; false when it cannot be read.
bool read_pixel(const spa_data& d, int stride, int x, int y, uint8_t out[4]) {
    const std::size_t at = d.chunk->offset + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u;
    if (d.type != SPA_DATA_DmaBuf) {
        if (!d.data)
            return false;
        std::memcpy(out, static_cast<const uint8_t*>(d.data) + at, 4);
        return true;
    }
    const int fd = static_cast<int>(d.fd);
    const std::size_t len = d.mapoffset + d.maxsize;
    void* m = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED)
        return false;
    dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    std::memcpy(out, static_cast<const uint8_t*>(m) + d.mapoffset + at, 4);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    munmap(m, len);
    return true;
}

void on_param_changed(void* data, uint32_t id, const spa_pod* param) {
    auto& s = *static_cast<cast_state*>(data);
    if (!param || id != SPA_PARAM_Format)
        return;
    uint32_t media_type = 0, media_subtype = 0;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0 || media_type != SPA_MEDIA_TYPE_video ||
        media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_video_raw_parse(param, &s.format) < 0)
        return;
    s.have_format = true;
    s.dmabuf = (s.format.flags & SPA_VIDEO_FLAG_MODIFIER) != 0;
    std::printf("format %ux%u %s%s\n", s.format.size.width, s.format.size.height,
                s.format.format == SPA_VIDEO_FORMAT_BGRx   ? "BGRx"
                : s.format.format == SPA_VIDEO_FORMAT_BGRA ? "BGRA"
                : s.format.format == SPA_VIDEO_FORMAT_RGBx ? "RGBx"
                : s.format.format == SPA_VIDEO_FORMAT_RGBA ? "RGBA"
                                                           : "other",
                s.dmabuf ? (s.format.modifier == 0                       ? " dmabuf modifier=LINEAR"
                            : s.format.modifier == 0x00ffffffffffffffull ? " dmabuf modifier=implicit"
                                                                          : " dmabuf modifier=other")
                         : "");
    std::fflush(stdout);
    // Shared memory, or dma-buf when the format was negotiated with a modifier.
    uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const spa_pod* params[1];
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
        SPA_POD_CHOICE_RANGE_Int(4, 2, 16), SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int(s.dmabuf ? (1 << SPA_DATA_DmaBuf)
                                          : ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)))));
    pw_stream_update_params(s.stream, params, 1);
}

void on_process(void* data) {
    auto& s = *static_cast<cast_state*>(data);
    pw_buffer* b = pw_stream_dequeue_buffer(s.stream);
    if (!b)
        return;
    spa_buffer* sb = b->buffer;
    const bool corrupted = sb->datas[0].chunk && (sb->datas[0].chunk->flags & SPA_CHUNK_FLAG_CORRUPTED);
    const uint32_t size = sb->datas[0].chunk ? sb->datas[0].chunk->size : 0;
    const bool is_dmabuf = sb->datas[0].type == SPA_DATA_DmaBuf;
    // A dma-buf frame's chunk size may be 0 (the data lives in the buffer); count it.
    if (!corrupted && (size > 0 || is_dmabuf)) {
        ++s.frames;
        if (is_dmabuf)
            ++s.dmabuf_frames;
        if (s.frames == 1)
            s.first_frame = now_s();
        if (s.frames <= 3) {
            std::printf("frame %u bytes=%u%s\n", s.frames, size, is_dmabuf ? " dmabuf" : "");
            std::fflush(stdout);
        }
        if (s.expect && !s.expect_done && now_s() - s.started > 1.0 && s.have_format) {
            const int stride = sb->datas[0].chunk->stride > 0 ? sb->datas[0].chunk->stride
                                                              : static_cast<int>(s.format.size.width) * 4;
            uint8_t p[4]{};
            if (s.ex >= 0 && s.ey >= 0 && s.ex < static_cast<int>(s.format.size.width) &&
                s.ey < static_cast<int>(s.format.size.height) && read_pixel(sb->datas[0], stride, s.ex, s.ey, p)) {
                const bool bgr = s.format.format == SPA_VIDEO_FORMAT_BGRx || s.format.format == SPA_VIDEO_FORMAT_BGRA;
                const int r = bgr ? p[2] : p[0], g = p[1], bl = bgr ? p[0] : p[2];
                s.expect_ok = std::abs(r - s.er) <= 2 && std::abs(g - s.eg) <= 2 && std::abs(bl - s.eb) <= 2;
                std::printf("expect %d,%d,%d@%d,%d %s got=%d,%d,%d\n", s.er, s.eg, s.eb, s.ex, s.ey,
                            s.expect_ok ? "OK" : "FAIL", r, g, bl);
                std::fflush(stdout);
            }
            s.expect_done = true;
        }
    }
    pw_stream_queue_buffer(s.stream, b);
}

void on_state_changed(void*, pw_stream_state, pw_stream_state state, const char* error) {
    std::printf("stream state %s%s%s\n", pw_stream_state_as_string(state), error ? " " : "", error ? error : "");
    std::fflush(stdout);
}

int run_stream(int fd, unsigned node, double seconds, cast_state& s) {
    pw_init(nullptr, nullptr);
    s.loop = pw_main_loop_new(nullptr);
    pw_context* ctx = pw_context_new(pw_main_loop_get_loop(s.loop), nullptr, 0);
    pw_core* core = pw_context_connect_fd(ctx, fd, nullptr, 0);
    if (!core) {
        std::printf("error cannot connect to the PipeWire remote\n");
        return 1;
    }
    s.stream = pw_stream_new(core, "lumen-portal-probe",
                             pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                               PW_KEY_MEDIA_ROLE, "Screen", nullptr));
    static const pw_stream_events events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = on_state_changed,
        .param_changed = on_param_changed,
        .process = on_process,
    };
    pw_stream_add_listener(s.stream, &s.listener, &events, &s);
    uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    spa_rectangle def_size = {1920, 1080}, min_size = {1, 1}, max_size = {8192, 8192};
    spa_fraction def_rate = {60, 1}, min_rate = {0, 1}, max_rate = {360, 1};
    const spa_pod* params[2];
    uint32_t n_params = 0;
    if (s.want_dmabuf) {
        // LINEAR or implicit dma-buf (OBS and browsers offer their EGL's modifiers plus the
        // implicit one): the modifier is mandatory and left for the producer to fix.
        spa_pod_frame f{}, c{};
        spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_add(&b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
                            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
                            0);
        spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_builder_push_choice(&b, &c, SPA_CHOICE_Enum, 0);
        spa_pod_builder_long(&b, 0); // default
        spa_pod_builder_long(&b, 0); // DRM_FORMAT_MOD_LINEAR
        spa_pod_builder_long(&b, static_cast<int64_t>(0x00ffffffffffffffull)); // DRM_FORMAT_MOD_INVALID
        spa_pod_builder_pop(&b, &c);
        spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&def_size, &min_size, &max_size),
                            SPA_FORMAT_VIDEO_framerate,
                            SPA_POD_CHOICE_RANGE_Fraction(&def_rate, &min_rate, &max_rate), 0);
        params[n_params++] = static_cast<const spa_pod*>(spa_pod_builder_pop(&b, &f));
    }
    params[n_params++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                               SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&def_size, &min_size, &max_size),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&def_rate, &min_rate, &max_rate)));
    if (pw_stream_connect(s.stream, PW_DIRECTION_INPUT, node,
                          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                          params, n_params) < 0) {
        std::printf("error pw_stream_connect failed\n");
        return 1;
    }
    s.started = now_s();
    pw_loop* loop = pw_main_loop_get_loop(s.loop);
    spa_source* timer = pw_loop_add_timer(
        loop, [](void* data, uint64_t) { pw_main_loop_quit(static_cast<pw_main_loop*>(data)); }, s.loop);
    timespec value{static_cast<time_t>(seconds), static_cast<long>((seconds - std::floor(seconds)) * 1e9)};
    timespec interval{0, 0};
    pw_loop_update_timer(loop, timer, &value, &interval, false);
    pw_main_loop_run(s.loop);
    const double span = s.frames > 1 ? now_s() - s.first_frame : 0.0;
    std::printf("screencast frames=%u seconds=%.2f fps=%.1f buffers=%s\n", s.frames, seconds,
                span > 0 ? (s.frames - 1) / span : 0.0,
                s.dmabuf_frames > 0 ? "dmabuf" : (s.frames > 0 ? "memfd" : "none"));
    pw_stream_destroy(s.stream);
    pw_core_disconnect(core);
    pw_context_destroy(ctx);
    pw_main_loop_destroy(s.loop);
    pw_deinit();
    return 0;
}

#endif

int screencast_main(sd_bus* bus, unsigned types, unsigned cursor_mode, double seconds, double timeout,
                    const char* expect, bool dmabuf) {
    response created;
    const std::string t1 = token("create"), st = token("session");
    if (!request(
            bus, "org.freedesktop.portal.ScreenCast", "CreateSession", t1,
            [&](sd_bus_message* m) {
                sd_bus_message_open_container(m, 'a', "{sv}");
                sd_bus_message_append(m, "{sv}", "handle_token", "s", t1.c_str());
                sd_bus_message_append(m, "{sv}", "session_handle_token", "s", st.c_str());
                sd_bus_message_close_container(m);
            },
            created, timeout))
        return 1;
    std::printf("create response %u session %s\n", created.code, created.session_handle.c_str());
    if (created.code != 0 || created.session_handle.empty())
        return 1;
    const std::string session = created.session_handle;
    const auto close_session = [&] {
        sd_bus_call_method(bus, k_desktop, session.c_str(), "org.freedesktop.portal.Session", "Close", nullptr,
                           nullptr, "");
    };
    response selected;
    const std::string t2 = token("select");
    if (!request(
            bus, "org.freedesktop.portal.ScreenCast", "SelectSources", t2,
            [&](sd_bus_message* m) {
                sd_bus_message_append(m, "o", session.c_str());
                sd_bus_message_open_container(m, 'a', "{sv}");
                sd_bus_message_append(m, "{sv}", "handle_token", "s", t2.c_str());
                sd_bus_message_append(m, "{sv}", "types", "u", types);
                sd_bus_message_append(m, "{sv}", "multiple", "b", 0);
                sd_bus_message_append(m, "{sv}", "cursor_mode", "u", cursor_mode);
                sd_bus_message_close_container(m);
            },
            selected, timeout)) {
        close_session();
        return 1;
    }
    std::printf("select response %u\n", selected.code);
    if (selected.code != 0) {
        close_session();
        return 1;
    }
    response started;
    const std::string t3 = token("start");
    if (!request(
            bus, "org.freedesktop.portal.ScreenCast", "Start", t3,
            [&](sd_bus_message* m) {
                sd_bus_message_append(m, "os", session.c_str(), "");
                sd_bus_message_open_container(m, 'a', "{sv}");
                sd_bus_message_append(m, "{sv}", "handle_token", "s", t3.c_str());
                sd_bus_message_close_container(m);
            },
            started, timeout)) {
        close_session();
        return 1;
    }
    std::printf("start response %u streams=%zu\n", started.code, started.streams.size());
    if (started.code != 0 || started.streams.empty()) {
        close_session();
        return 1;
    }
    const response::stream st0 = started.streams[0];
    std::printf("started node=%u size=%dx%d source_type=%u\n", st0.node, st0.w, st0.h, st0.source_type);
    std::fflush(stdout);
    sd_bus_message* reply = nullptr;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(bus, k_desktop, k_path, "org.freedesktop.portal.ScreenCast", "OpenPipeWireRemote",
                               &err, &reply, "oa{sv}", session.c_str(), 0);
    if (r < 0) {
        std::printf("error OpenPipeWireRemote: %s\n", err.message ? err.message : std::strerror(-r));
        sd_bus_error_free(&err);
        close_session();
        return 1;
    }
    int fd = -1;
    r = sd_bus_message_read(reply, "h", &fd);
    const int own = r >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 3) : -1;
    sd_bus_message_unref(reply);
    if (own < 0) {
        std::printf("error no PipeWire fd\n");
        close_session();
        return 1;
    }
#if defined(LUMEN_PROBE_HAS_PIPEWIRE)
    cast_state s;
    s.want_dmabuf = dmabuf;
    if (expect && std::sscanf(expect, "%d,%d,%d@%d,%d", &s.er, &s.eg, &s.eb, &s.ex, &s.ey) == 5)
        s.expect = true;
    const int status = run_stream(own, st0.node, seconds, s);
    close_session();
    if (status != 0 || s.frames == 0)
        return 1;
    return s.expect && !s.expect_ok ? 1 : 0;
#else
    (void)seconds;
    (void)expect;
    (void)dmabuf;
    close(own);
    close_session();
    std::printf("error built without libpipewire: no stream consumed\n");
    return 1;
#endif
}

unsigned read_version(sd_bus* bus, const char* iface) {
    uint32_t v = 0;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    if (sd_bus_get_property_trivial(bus, k_desktop, k_path, iface, "version", &err, 'u', &v) < 0)
        v = 0;
    sd_bus_error_free(&err);
    return v;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: lumen-portal-probe screenshot|pick-color|screencast|version [options]\n");
        return 2;
    }
    const std::string mode = argv[1];
    bool interactive = false, backend = false, dmabuf = false;
    unsigned target = 0, types = 1, cursor_mode = 1;
    double timeout = 30, seconds = 3;
    const char* expect = nullptr;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--interactive")
            interactive = true;
        else if (a == "--backend")
            backend = true;
        else if (a == "--dmabuf")
            dmabuf = true;
        else if (a == "--target")
            target = static_cast<unsigned>(std::atoi(next()));
        else if (a == "--types")
            types = static_cast<unsigned>(std::atoi(next()));
        else if (a == "--cursor-mode")
            cursor_mode = static_cast<unsigned>(std::atoi(next()));
        else if (a == "--timeout")
            timeout = std::atof(next());
        else if (a == "--seconds")
            seconds = std::atof(next());
        else if (a == "--expect")
            expect = next();
        else {
            std::fprintf(stderr, "lumen-portal-probe: unknown option %s\n", a.c_str());
            return 2;
        }
    }
    sd_bus* bus = nullptr;
    if (sd_bus_open_user(&bus) < 0) {
        std::printf("error no session bus\n");
        return 1;
    }
    int status = 2;
    if (mode == "screenshot")
        status = screenshot_main(bus, interactive, target, backend, timeout, false);
    else if (mode == "pick-color")
        status = screenshot_main(bus, false, 0, backend, timeout, true);
    else if (mode == "screencast")
        status = screencast_main(bus, types, cursor_mode, seconds, timeout, expect, dmabuf);
    else if (mode == "version") {
        std::printf("screenshot version=%u screencast version=%u\n",
                    read_version(bus, "org.freedesktop.portal.Screenshot"),
                    read_version(bus, "org.freedesktop.portal.ScreenCast"));
        status = 0;
    } else
        std::fprintf(stderr, "lumen-portal-probe: unknown mode %s\n", mode.c_str());
    std::fflush(stdout);
    sd_bus_flush_close_unref(bus);
    return status;
}
