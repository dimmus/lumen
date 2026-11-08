module;

// Bodies of lx.app:clipboard — the selections read through pipes on the loop, this client's
// sources written to readers without blocking, drags started and dropped.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

module lx.app;

import std;

import lx.foundation;
import lx.runtime;
import lx.trace;
import lx.ui;
import lx.wayland.client;

namespace lx::app {

namespace {

constexpr lx::trace::category log_clip{"app.clipboard"};

/// What this client offers its text as: the freedesktop name, then X11's (Xwayland clients
/// ask for those). Every one carries the same UTF-8 bytes.
constexpr const char* k_offered[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain",
                                     "TEXT", "STRING"};

constexpr const char* k_uri_list = "text/uri-list";

/// What a selection or a drop is read as, in order of preference.
constexpr const char* k_wanted[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT",
                                    "STRING"};

[[nodiscard]] bool offered_mime(const char* mime) {
    for (const char* m : k_offered)
        if (std::strcmp(m, mime) == 0)
            return true;
    return false;
}

[[nodiscard]] const char* wanted_mime(const lx::wayland::data_offer& offer) {
    for (const char* m : k_wanted)
        if (offer.has_mime(m))
            return m;
    return nullptr;
}

/// `in` as valid UTF-8: an invalid or overlong sequence, a surrogate or a NUL becomes
/// U+FFFD (a NUL would end the text early; the field takes valid UTF-8 only).
[[nodiscard]] std::string sanitize_utf8(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    const auto* s = reinterpret_cast<const unsigned char*>(in.data());
    const std::size_t n = in.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned c = s[i];
        unsigned len = 0;
        std::uint32_t cp = 0;
        std::uint32_t min = 0;
        if (c >= 0x01 && c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        if ((c & 0xE0u) == 0xC0u) {
            len = 2;
            cp = c & 0x1Fu;
            min = 0x80;
        } else if ((c & 0xF0u) == 0xE0u) {
            len = 3;
            cp = c & 0x0Fu;
            min = 0x800;
        } else if ((c & 0xF8u) == 0xF0u) {
            len = 4;
            cp = c & 0x07u;
            min = 0x10000;
        }
        bool ok = len != 0 && i + len <= n;
        for (unsigned k = 1; ok && k < len; ++k) {
            if ((s[i + k] & 0xC0u) != 0x80u)
                ok = false;
            else
                cp = (cp << 6u) | (s[i + k] & 0x3Fu);
        }
        ok = ok && cp >= min && cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
        if (ok) {
            out.append(in.data() + i, len);
            i += len;
        } else {
            out.append("\xEF\xBF\xBD"); // U+FFFD
            ++i;
        }
    }
    return out;
}

} // namespace

// ── a pipe's read end on the loop: a selection, or a drop ──

struct clipboard::reader final : public lx::runtime::event_source {
    reader(clipboard& c, int read_fd, channel* ch) : owner{c}, fd{read_fd}, target{ch} {}
    ~reader() override {
        if (fd >= 0)
            ::close(fd);
    }
    bool prepare(lx::runtime::clock_time& timeout) override {
        timeout = -1;
        return ready;
    }
    bool check() override { return ready; }
    bool dispatch() override {
        ready = false;
        char chunk[16384];
        for (;;) {
            const ssize_t got = ::read(fd, chunk, sizeof(chunk));
            if (got > 0) {
                if (data.size() + static_cast<std::size_t>(got) > k_max_bytes) {
                    owner.finish_read(*this, false);
                    return true;
                }
                data.append(chunk, static_cast<std::size_t>(got));
                continue;
            }
            if (got == 0) {
                owner.finish_read(*this, true);
                return true;
            }
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN)
                owner.finish_read(*this, false);
            return true;
        }
    }
    [[nodiscard]] int poll_fd() const override { return fd; }
    void on_readable() override { ready = true; }

    clipboard& owner;
    int fd = -1;
    /// The selection it reads; null: a drop.
    channel* target = nullptr;
    bool ready = false;
    std::string data{};
};

// ── a send's writer: a pipe's write end, waiting on the loop for room ──

struct clipboard::writer final : public lx::runtime::event_source {
    writer(clipboard& c, int write_fd, std::string text)
        : owner{c},
          fd{write_fd},
          data{std::move(text)} {}
    ~writer() override {
        if (fd >= 0)
            ::close(fd);
    }
    bool prepare(lx::runtime::clock_time& timeout) override {
        timeout = -1;
        return ready;
    }
    bool check() override { return ready; }
    bool dispatch() override {
        ready = false;
        if (pump())
            owner.finish_write(*this);
        return true;
    }
    [[nodiscard]] int poll_fd() const override { return fd; }
    // Room (EPOLLOUT), or the reader hung up (EPOLLERR): the next write tells which.
    void on_writable() override { ready = true; }
    void on_readable() override { ready = true; }

    /// Writes what fits. True when it is done: all written, or the reader is gone.
    [[nodiscard]] bool pump() {
        while (offset < data.size()) {
            const ssize_t put = ::write(fd, data.data() + offset, data.size() - offset);
            if (put > 0) {
                offset += static_cast<std::size_t>(put);
                continue;
            }
            if (put < 0 && errno == EINTR)
                continue;
            return put < 0 && errno != EAGAIN; // EPIPE and the like: the reader is gone
        }
        return true;
    }

    clipboard& owner;
    int fd = -1;
    std::string data{};
    std::size_t offset = 0;
    bool ready = false;
};

// ── the service ──

clipboard::clipboard() {
    (void)std::format_to_n(own_mime_, sizeof(own_mime_) - 1, "application/x-lumen-owner-{}",
                           static_cast<long>(::getpid()));
    channels_[1].kind = lx::ui::clipboard_kind::primary;
}

clipboard::~clipboard() {
    stop();
}

lx::result<void> clipboard::start(lx::wayland::display& d, lx::runtime::event_loop& loop,
                                  transfer_host* host) {
    if (display_)
        return {};
    lx::wayland::seat* s = d.default_seat();
    if (!s) {
        log_clip.info("no seat: selections stay in this process, nothing drags");
        return {};
    }
    display_ = &d;
    loop_ = &loop;
    host_ = host;
    const struct {
        lx::wayland::global_kind global;
        lx::wayland::selection_kind kind;
    } devices[2] = {
        {lx::wayland::global_kind::data_device_manager, lx::wayland::selection_kind::clipboard},
        {lx::wayland::global_kind::primary_selection, lx::wayland::selection_kind::primary}};
    for (unsigned i = 0; i < 2; ++i) {
        if (!d.has(devices[i].global)) {
            log_clip.info("no {}: that selection stays in this process",
                          d.global(devices[i].global).interface);
            continue;
        }
        if (auto made = channels_[i].device.create(d, *s, devices[i].kind); !made)
            log_clip.error(made.get_error());
        else
            channels_[i].device.set_handler(this);
    }
    if (!channels_[0].device.valid() && !channels_[1].device.valid()) {
        display_ = nullptr;
        loop_ = nullptr;
        host_ = nullptr;
        return {};
    }
    lx::ui::clipboard::global().set_sink(&clipboard::sink, this);
    if (channels_[0].device.valid() && host_) {
        lx::ui::drag_service::global().set_starter(&clipboard::start_drag, this);
        // The chooser is part of the same registration — it used to sit under the `if`
        // without braces, so it ran unconditionally (a clipboard without a drag device
        // still claimed the service) and `stop()` never withdrew it.
        lx::ui::drag_service::global().set_chooser(
            [](void* user, std::uint32_t action) {
                static_cast<clipboard*>(user)->choose_dnd_action(action);
            },
            this);
        chooser_registered_ = true;
    }
    // A paste target may close its end before the text is written: EPIPE, not a signal
    // that ends the process (as GIO does, only when nobody chose a disposition).
    struct sigaction now{};
    if (::sigaction(SIGPIPE, nullptr, &now) == 0 && now.sa_handler == SIG_DFL)
        (void)::signal(SIGPIPE, SIG_IGN);
    return {};
}

void clipboard::stop() {
    if (display_) {
        lx::ui::clipboard::global().set_sink(nullptr, nullptr);
        lx::ui::drag_service::global().set_starter(nullptr, nullptr);
        if (chooser_registered_) {
            lx::ui::drag_service::global().set_chooser(nullptr, nullptr);
            chooser_registered_ = false;
        }
    }
    // A drag still running: its field hears it ended, unmoved.
    if (drag_source_.valid())
        end_drag(false);
    reset_drag_over();
    stop_reader(drop_reader_);
    for (channel& c : channels_)
        stop_reader(c.pending);
    for (writer*& w : writers_) {
        if (w && loop_)
            loop_->remove_source(w);
        delete w;
        w = nullptr;
    }
    drag_source_.destroy();
    for (channel& c : channels_) {
        c.source.destroy();
        c.device.destroy();
    }
    display_ = nullptr;
    loop_ = nullptr;
    host_ = nullptr;
}

void clipboard::set_text(const char* text) {
    channel& c = channel_of(lx::ui::clipboard_kind::clipboard);
    c.own = text ? text : "";
    lx::ui::clipboard::global().set_text(c.own.c_str()); // → sink → offer, when active
}

const char* clipboard::text() const {
    return lx::ui::clipboard::global().text();
}
const char* clipboard::primary_text() const {
    return lx::ui::clipboard::global().primary_selection();
}

clipboard::channel* clipboard::channel_of(const lx::wayland::data_device& d) {
    for (channel& c : channels_)
        if (&c.device == &d)
            return &c;
    return nullptr;
}

clipboard::channel* clipboard::channel_of(const lx::wayland::data_source& s) {
    for (channel& c : channels_)
        if (&c.source == &s)
            return &c;
    return nullptr;
}

void clipboard::sink(void* user, lx::ui::clipboard_kind kind, const char* utf8) {
    auto* self = static_cast<clipboard*>(user);
    self->offer(self->channel_of(kind), utf8);
}

void clipboard::offer(channel& c, const char* text) {
    if (text != c.own.c_str())
        c.own = text ? text : "";
    if (!c.device.valid() || !display_)
        return;
    lx::wayland::seat* s = display_->default_seat();
    // A new source each time (a source is used once); the old one's pending sends keep
    // their own copy of its text.
    c.source.destroy();
    const auto kind = c.kind == lx::ui::clipboard_kind::primary
                          ? lx::wayland::selection_kind::primary
                          : lx::wayland::selection_kind::clipboard;
    if (auto made = c.source.create(*display_, kind); !made) {
        log_clip.error(made.get_error());
        return;
    }
    c.source.set_handler(this);
    for (const char* m : k_offered)
        c.source.offer(m);
    c.source.offer(own_mime_);
    c.device.set_selection(&c.source, s ? s->last_input_serial() : 0);
    ++c.stats.copies;
}

// ── sources: send, cancelled, and our drag's events ──

void clipboard::on_send(lx::wayland::data_source& s, const char* mime, int fd) {
    if (fd < 0)
        return;
    if (&s == &drag_source_) {
        // The drag source offers `text/uri-list` besides the text types (a file manager's
        // drop reads only that), and that offer has to be answerable. The text-only guard
        // that used to stand above this branch closed the fd first, so a drag of files
        // looked like a source refusing every format it had just advertised.
        if (std::strcmp(mime, k_uri_list) != 0 && !offered_mime(mime)) {
            ::close(fd);
            return;
        }
        ++drag_stats_.sends;
        if (std::strcmp(mime, k_uri_list) == 0)
            (void)write(fd, drag_uri_list());
        else
            (void)write(fd, drag_text_);
        return;
    }
    if (!offered_mime(mime)) {
        ::close(fd);
        return;
    }
    channel* c = channel_of(s);
    if (!c) {
        ::close(fd);
        return;
    }
    ++c->stats.sends;
    if (!write(fd, c->own))
        ++c->stats.dropped;
}

bool clipboard::write(int fd, const std::string& text) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0)
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    auto* w = new writer(*this, fd, text);
    if (w->pump()) {
        delete w; // written at once (the common case: it fits the pipe)
        return true;
    }
    for (writer*& slot : writers_) {
        if (!slot) {
            slot = w;
            loop_->add_source(slot);
            loop_->set_write_interest(slot, true);
            return true;
        }
    }
    delete w;
    log_clip.warn("clipboard: {} sends already waiting for their readers; this one dropped",
                  k_writers);
    return false;
}

void clipboard::finish_write(writer& w) {
    for (writer*& slot : writers_) {
        if (slot == &w) {
            loop_->remove_source(slot);
            delete slot;
            slot = nullptr;
            return;
        }
    }
}

void clipboard::on_cancelled(lx::wayland::data_source& s) {
    if (&s == &drag_source_) {
        // Dropped nowhere that took it, or the compositor refused the drag.
        ++drag_stats_.cancelled;
        end_drag(false);
        return;
    }
    if (channel* c = channel_of(s))
        ++c->stats.cancelled;
    s.destroy();
}

void clipboard::on_action(lx::wayland::data_source& s, std::uint32_t action) {
    if (&s == &drag_source_)
        drag_action_ = action;
}

void clipboard::on_dnd_finished(lx::wayland::data_source& s) {
    if (&s != &drag_source_)
        return;
    const bool moved = drag_action_ == lx::wayland::k_dnd_move;
    ++(moved ? drag_stats_.moved : drag_stats_.copied);
    end_drag(moved);
}

void clipboard::set_drag_icon(const std::uint32_t* argb, unsigned w, unsigned h) {
    if (!display_ || !argb || w == 0 || h == 0 || w > 512 || h > 512)
        return;
    void* compositor = display_->native_global(lx::wayland::global_kind::compositor);
    void* shm = display_->native_global(lx::wayland::global_kind::shm);
    if (!compositor || !shm)
        return;
    const std::size_t bytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
    // A wl_shm pool is mmap'd by the compositor, so it needs a memory-backed fd (memfd),
    // never a pipe: the pixels are copied in once, the fd handed over, our map dropped.
    const int fd = ::memfd_create("lumen-drag-icon", MFD_CLOEXEC);
    if (fd < 0)
        return;
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        ::close(fd);
        return;
    }
    void* map = ::mmap(nullptr, bytes, PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ::close(fd);
        return;
    }
    std::memcpy(map, argb, bytes);
    ::munmap(map, bytes);
    auto* pool = wl_shm_create_pool(static_cast<wl_shm*>(shm), fd,
                                    static_cast<std::int32_t>(bytes));
    ::close(fd);
    if (!pool)
        return;
    auto* buffer = wl_shm_pool_create_buffer(
        pool, 0, static_cast<std::int32_t>(w), static_cast<std::int32_t>(h),
        static_cast<std::int32_t>(w * 4), WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (!buffer)
        return;
    if (!icon_surface_) {
        auto* surface =
            wl_compositor_create_surface(static_cast<wl_compositor*>(compositor));
        icon_surface_ = surface;
    }
    wl_surface_attach(static_cast<wl_surface*>(icon_surface_), buffer, 0, 0);
    wl_surface_damage_buffer(static_cast<wl_surface*>(icon_surface_), 0, 0, INT32_MAX,
                             INT32_MAX);
    wl_surface_commit(static_cast<wl_surface*>(icon_surface_));
    wl_buffer_destroy(buffer);
    icon_w_ = w;
    icon_h_ = h;
}

void clipboard::end_drag(bool moved) {
    drag_source_.destroy();
    drag_action_ = lx::wayland::k_dnd_none;
    if (icon_surface_ && display_) {
        // The drag is over: the icon surface detaches its buffer (the compositor drops
        // the pointer-following role with the drag) and is kept for the next drag.
        wl_surface_attach(static_cast<wl_surface*>(icon_surface_), nullptr, 0, 0);
        wl_surface_commit(static_cast<wl_surface*>(icon_surface_));
    }
    lx::ui::drag_service::global().end(moved);
}

void clipboard::choose_dnd_action(std::uint32_t action) {
    if (drag_offer_ && drop_ask_) {
        // The target's half of the `ask` handshake: the chosen action as the preference
        // (wl_data_offer.set_actions), then the drop is finished as taken.
        drag_offer_->set_actions(action, action);
        channels_[0].device.finish_drop(true);
        drop_ask_ = false;
        drag_offer_ = nullptr;
    }
}

bool clipboard::start_drag(void* user, const lx::ui::drag_payload& payload) {
    auto* self = static_cast<clipboard*>(user);
    channel& c = self->channel_of(lx::ui::clipboard_kind::clipboard);
    lx::wayland::surface* origin = self->host_ ? self->host_->host_drag_origin() : nullptr;
    lx::ui::ui_root* root = origin ? self->host_->host_root_of(origin) : nullptr;
    lx::wayland::seat* s = self->display_ ? self->display_->default_seat() : nullptr;
    if (!c.device.valid() || !origin || !root || !s)
        return false;
    self->drag_source_.destroy();
    if (auto made = self->drag_source_.create(*self->display_); !made) {
        log_clip.error(made.get_error());
        return false;
    }
    self->drag_source_.set_handler(self);
    for (const char* m : k_offered)
        self->drag_source_.offer(m);
    if (!payload.uris.empty()) {
        self->drag_source_.offer(k_uri_list);
        self->drag_uris_.assign(payload.uris.begin(), payload.uris.end());
    } else {
        self->drag_uris_.clear();
    }
    self->drag_source_.offer(self->own_mime_);
    self->drag_source_.set_actions(
        lx::wayland::k_dnd_copy | lx::wayland::k_dnd_move |
        (payload.ask ? lx::wayland::k_dnd_ask : lx::wayland::k_dnd_none));
    self->drag_text_ = payload.text ? payload.text : "";
    self->drag_action_ = lx::wayland::k_dnd_none;
    // The press that started it: the button is still held (the compositor's rule).
    // The icon is a bare `wl_surface*` created here, never an `lx::wayland::surface` (which
    // owns a proxy and a listener table): reinterpreting the pointer as one and calling
    // `native()` on it read a member of an object that does not exist.
    c.device.start_drag(self->drag_source_, *origin, s->last_input_serial(),
                        static_cast<wl_surface*>(self->icon_surface_));
    ++self->drag_stats_.started;
    // The compositor holds the pointer now: the press here ends without a click.
    root->pointer_cancel();
    return true;
}

// ── devices: selections ──

void clipboard::on_selection(lx::wayland::data_device& d, lx::wayland::data_offer* offer) {
    channel* c = channel_of(d);
    if (!c)
        return;
    stop_reader(c->pending);
    lx::ui::clipboard& ui = lx::ui::clipboard::global();
    if (!offer) {
        // No selection anywhere: a paste has nothing to take.
        ui.set_system_text("", c->kind);
        c->system.clear();
        return;
    }
    if (offer->has_mime(own_mime_)) {
        // Ours, coming back: the text is in hand.
        ++c->stats.own_offers;
        return;
    }
    const char* mime = wanted_mime(*offer);
    if (!mime) {
        ++c->stats.non_text;
        ui.set_system_text("", c->kind);
        c->system.clear();
        return;
    }
    c->pending = read(*offer, mime, c);
    if (!c->pending)
        ++c->stats.dropped;
}

clipboard::reader* clipboard::read(lx::wayland::data_offer& offer, const char* mime, channel* c) {
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        log_clip.warn("clipboard: pipe2 failed (errno {}): not read", errno);
        return nullptr;
    }
    offer.receive(mime, fds[1]);
    ::close(fds[1]); // marshalling duplicated it; the source's copy is the only writer now
    auto* r = new reader(*this, fds[0], c);
    loop_->add_source(r);
    return r;
}

void clipboard::stop_reader(reader*& r) {
    if (!r)
        return;
    if (loop_)
        loop_->remove_source(r);
    delete r;
    r = nullptr;
}

void clipboard::finish_read(reader& r, bool ok) {
    if (channel* c = r.target) {
        if (ok) {
            std::string text = sanitize_utf8(r.data);
            c->system.swap(text);
            lx::ui::clipboard::global().set_system_text(c->system.c_str(), c->kind);
            ++c->stats.reads;
        } else {
            ++c->stats.dropped;
            log_clip.warn("clipboard: a selection could not be read ({} bytes; cap {})",
                          r.data.size(), k_max_bytes);
        }
        stop_reader(c->pending);
        return;
    }
    // A drop: delivered where it was dropped (files or text), then the offer is done.
    bool taken = false;
    if (ok && drop_root_) {
        if (drop_uris_) {
            const auto uris = lx::ui::parse_uri_list(sanitize_utf8(r.data));
            std::vector<std::string_view> views;
            views.reserve(uris.size());
            for (const std::string& u : uris)
                views.emplace_back(u);
            if (drop_ask_)
                // The actions the *source* allowed, remembered by `on_drop`: it had already
                // cleared `drag_offer_` through `reset_drag_over()`, so reading the action
                // off the offer here dereferenced null on every ask drop.
                taken = drop_root_->drop_ask(drop_at_, views, drop_actions_);
            else
                taken = drop_root_->drop_files(drop_at_, views);
        } else {
            taken = drop_root_->drop_text(drop_at_, sanitize_utf8(r.data));
        }
    }
    ++(taken ? drag_stats_.drops : drag_stats_.refused);
    // An ask drop stays open only while the target may still answer through
    // `choose_dnd_action`; one it refused is finished here, or the source waits forever for
    // an answer that cannot come.
    const bool open_ask = drop_ask_ && taken;
    if (!open_ask)
        channels_[0].device.finish_drop(taken);
    drop_ask_ = open_ask;
    drop_root_ = nullptr;
    stop_reader(drop_reader_);
}

// ── devices: a drag over our windows ──

void clipboard::on_drag_enter(lx::wayland::data_device& d, lx::wayland::data_offer* offer,
                              lx::wayland::surface* target, double x, double y,
                              std::uint32_t serial) {
    if (&d != &channels_[0].device)
        return;
    reset_drag_over();
    ++drag_stats_.entered;
    drag_offer_ = offer;
    drag_root_ = host_ ? host_->host_root_of(target) : nullptr;
    drag_serial_ = serial;
    // A drag of files offers *only* `text/uri-list`, so the text types alone would leave
    // `drag_mime_` null and the drop refused before it was read.
    drag_mime_ = offer ? wanted_mime(*offer) : nullptr;
    drop_uris_ = offer && offer->has_mime(k_uri_list);
    if (!drag_mime_ && drop_uris_)
        drag_mime_ = k_uri_list;
    drop_ask_ = offer && offer->action() == lx::wayland::k_dnd_ask;
    drag_at_ = {static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y))};
    update_drop();
}

void clipboard::on_drag_motion(lx::wayland::data_device& d, double x, double y) {
    if (&d != &channels_[0].device)
        return;
    drag_at_ = {static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y))};
    update_drop();
}

void clipboard::on_drag_leave(lx::wayland::data_device& d) {
    if (&d == &channels_[0].device)
        reset_drag_over();
}

void clipboard::reset_drag_over() {
    if (drag_root_)
        drag_root_->drop_leave();
    drag_offer_ = nullptr;
    drag_root_ = nullptr;
    drag_mime_ = nullptr;
    drag_accepting_ = -1;
}

void clipboard::update_drop() {
    const bool takes = drag_offer_ && drag_mime_ && drag_root_ && drag_root_->drop_motion(drag_at_);
    if (!drag_offer_ || drag_accepting_ == (takes ? 1 : 0))
        return;
    drag_accepting_ = takes ? 1 : 0;
    drag_offer_->accept(drag_serial_, takes ? drag_mime_ : nullptr);
    // Our own drag moves by default (within a field, or field to field); another client's
    // copies. The user's modifiers decide over both (the compositor's Shift / Ctrl).
    const bool ours = drag_offer_->has_mime(own_mime_);
    drag_offer_->set_actions(takes ? lx::wayland::k_dnd_copy | lx::wayland::k_dnd_move
                                   : lx::wayland::k_dnd_none,
                             takes ? (ours ? lx::wayland::k_dnd_move : lx::wayland::k_dnd_copy)
                                   : lx::wayland::k_dnd_none);
}

void clipboard::on_drop(lx::wayland::data_device& d, lx::wayland::data_offer* offer) {
    if (&d != &channels_[0].device)
        return;
    const bool takes = offer && drag_accepting_ == 1 && drag_mime_ && drag_root_;
    lx::ui::ui_root* root = drag_root_;
    const lx::point2i at = drag_at_;
    const char* mime = drag_mime_;
    // Read before `reset_drag_over()` drops the offer: the ask handshake needs the actions
    // the source allowed, and every pointer to the offer is gone after the reset.
    drop_actions_ = offer ? offer->source_actions() : lx::wayland::k_dnd_none;
    reset_drag_over();
    stop_reader(drop_reader_);
    if (!takes) {
        ++drag_stats_.refused;
        d.finish_drop(false);
        return;
    }
    drop_root_ = root;
    drop_at_ = at;
    drop_reader_ = read(*offer, mime, nullptr);
    if (!drop_reader_) {
        ++drag_stats_.refused;
        drop_root_ = nullptr;
        d.finish_drop(false);
    }
}

} // namespace lx::app
