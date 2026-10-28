module;

// Bodies of lx.app:clipboard — the data device's selection read through a pipe on the loop,
// this client's source written to readers without blocking.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

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

/// What a selection is read as, in order of preference.
constexpr const char* k_wanted[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT",
                                    "STRING"};

[[nodiscard]] bool offered_mime(const char* mime) {
    for (const char* m : k_offered)
        if (std::strcmp(m, mime) == 0)
            return true;
    return false;
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

// ── the selection's reader: a pipe's read end on the loop ──

struct clipboard::reader final : public lx::runtime::event_source {
    reader(clipboard& c, int read_fd) : owner{c}, fd{read_fd} {}
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

// ── the clipboard ──

clipboard::clipboard() {
    (void)std::format_to_n(own_mime_, sizeof(own_mime_) - 1, "application/x-lumen-owner-{}",
                           static_cast<long>(::getpid()));
}

clipboard::~clipboard() {
    stop();
}

lx::result<void> clipboard::start(lx::wayland::display& d, lx::runtime::event_loop& loop) {
    if (device_.valid())
        return {};
    lx::wayland::seat* s = d.default_seat();
    if (!d.has(lx::wayland::global_kind::data_device_manager) || !s) {
        log_clip.info("no wl_data_device_manager or seat: the clipboard stays in this process");
        return {};
    }
    if (auto made = device_.create(d, *s); !made) {
        log_clip.error(made.get_error());
        return made;
    }
    display_ = &d;
    loop_ = &loop;
    device_.set_handler(this);
    lx::ui::clipboard::global().set_sink(&clipboard::sink, this);
    // A paste target may close its end before the text is written: EPIPE, not a signal
    // that ends the process (as GIO does, only when nobody chose a disposition).
    struct sigaction now{};
    if (::sigaction(SIGPIPE, nullptr, &now) == 0 && now.sa_handler == SIG_DFL)
        (void)::signal(SIGPIPE, SIG_IGN);
    return {};
}

void clipboard::stop() {
    if (display_)
        lx::ui::clipboard::global().set_sink(nullptr, nullptr);
    stop_reader();
    for (writer*& w : writers_) {
        if (w && loop_)
            loop_->remove_source(w);
        delete w;
        w = nullptr;
    }
    source_.destroy();
    device_.destroy();
    display_ = nullptr;
    loop_ = nullptr;
}

void clipboard::set_text(const char* text) {
    own_ = text ? text : "";
    lx::ui::clipboard::global().set_text(own_.c_str()); // → sink → offer, when active
}

const char* clipboard::text() const {
    return lx::ui::clipboard::global().text();
}

void clipboard::sink(void* user, const char* utf8) {
    static_cast<clipboard*>(user)->offer(utf8);
}

void clipboard::offer(const char* text) {
    if (text != own_.c_str())
        own_ = text ? text : "";
    if (!device_.valid() || !display_)
        return;
    lx::wayland::seat* s = display_->default_seat();
    // A new source each time (a source is used once); the old one's pending sends keep
    // their own copy of its text.
    source_.destroy();
    if (auto made = source_.create(*display_); !made) {
        log_clip.error(made.get_error());
        return;
    }
    source_.set_handler(this);
    for (const char* m : k_offered)
        source_.offer(m);
    source_.offer(own_mime_);
    device_.set_selection(&source_, s ? s->last_input_serial() : 0);
    ++stats_.copies;
}

void clipboard::on_cancelled(lx::wayland::data_source& s) {
    ++stats_.cancelled;
    s.destroy();
}

void clipboard::on_send(lx::wayland::data_source&, const char* mime, int fd) {
    if (fd < 0)
        return;
    if (!offered_mime(mime)) {
        ::close(fd);
        return;
    }
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0)
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    auto w = std::make_unique<writer>(*this, fd, own_);
    ++stats_.sends;
    if (w->pump())
        return; // written at once (the common case: it fits the pipe), closed with `w`
    for (writer*& slot : writers_) {
        if (!slot) {
            slot = w.release();
            loop_->add_source(slot);
            loop_->set_write_interest(slot, true);
            return;
        }
    }
    ++stats_.dropped;
    log_clip.warn("clipboard: {} sends already waiting for their readers; this one dropped",
                  k_writers);
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

void clipboard::on_selection(lx::wayland::data_device&, lx::wayland::data_offer* offer) {
    stop_reader();
    if (!offer) {
        // No selection anywhere: a paste has nothing to take.
        lx::ui::clipboard::global().set_system_text("");
        system_.clear();
        return;
    }
    if (offer->has_mime(own_mime_)) {
        // Ours, coming back: the text is in hand.
        ++stats_.own_offers;
        return;
    }
    const char* mime = nullptr;
    for (const char* m : k_wanted)
        if (offer->has_mime(m)) {
            mime = m;
            break;
        }
    if (!mime) {
        ++stats_.non_text;
        lx::ui::clipboard::global().set_system_text("");
        system_.clear();
        return;
    }
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        ++stats_.dropped;
        log_clip.warn("clipboard: pipe2 failed (errno {}): the selection is not read", errno);
        return;
    }
    offer->receive(mime, fds[1]);
    ::close(fds[1]); // marshalling duplicated it; the source's copy is the only writer now
    reader_ = new reader(*this, fds[0]);
    loop_->add_source(reader_);
}

void clipboard::stop_reader() {
    if (!reader_)
        return;
    if (loop_)
        loop_->remove_source(reader_);
    delete reader_;
    reader_ = nullptr;
}

void clipboard::finish_read(reader& r, bool ok) {
    if (ok) {
        std::string text = sanitize_utf8(r.data);
        system_.swap(text);
        lx::ui::clipboard::global().set_system_text(system_.c_str());
        ++stats_.reads;
    } else {
        ++stats_.dropped;
        log_clip.warn("clipboard: the selection could not be read ({} bytes; cap {})",
                      r.data.size(), k_max_bytes);
    }
    stop_reader();
}

} // namespace lx::app
