module;

// lx.app:clipboard — data transfer with other clients behind lx.ui's `clipboard::global()`
// and `drag_service::global()`: the clipboard and the primary selection (a data device each
// on the default seat) and drag and drop of text (on the clipboard's device).
//
// Selections. A copy (or a field's settled selection, for the primary) becomes the seat's
// selection, set with the serial of the input event that asked for it, and offered as UTF-8
// text under the usual names. Another client's selection is read as soon as it is offered —
// text only, through a non-blocking pipe on the loop — into what the next paste takes, so a
// paste stays synchronous (the field inserts at once). Until that read ends, a paste takes
// the previous text. Our own selection, coming back to us, is recognised by a per-process
// mime type and not read. Sends are written without blocking, the rest waiting on the loop
// for room.
//
// Drag and drop. A field dragging text out starts a drag of a source offering copy and move
// from the window the pointer is in, at the press's serial, with no icon (the pointer shows
// the drag); the field hears the end — a move deletes the text there. Over our windows a
// drag's text is offered to the drop target under the pointer (`ui_root::drop_motion`),
// accepted only while one takes it, preferring a move for our own drag and a copy for
// another client's; dropped, it is read through a pipe and inserted (`ui_root::drop_text`),
// then the offer is finished. Bodies in clipboard.cpp.

export module lx.app:clipboard;

import std;

import lx.foundation;
import lx.runtime;
import lx.ui;
import lx.wayland.client;

export namespace lx::app {

struct clipboard_stats {
    std::uint64_t copies = 0;     ///< selections set (a source each)
    std::uint64_t cancelled = 0;  ///< sources the compositor cancelled (replaced or refused)
    std::uint64_t sends = 0;      ///< send requests answered with the text
    std::uint64_t reads = 0;      ///< other clients' selections read to the end
    std::uint64_t own_offers = 0; ///< selections recognised as this client's own (not read)
    std::uint64_t non_text = 0;   ///< selections with no text type (nothing to paste)
    std::uint64_t dropped = 0;    ///< reads over the cap or failed, sends with no writer free
};

struct drag_stats {
    std::uint64_t started = 0;   ///< drags started from this client
    std::uint64_t moved = 0;     ///< ended as a move (the text deleted here)
    std::uint64_t copied = 0;    ///< ended as a copy
    std::uint64_t cancelled = 0; ///< dropped nowhere that took it, or refused
    std::uint64_t sends = 0;     ///< send requests of our drags answered
    std::uint64_t entered = 0;   ///< drags that entered one of our windows
    std::uint64_t drops = 0;     ///< dropped on us and inserted
    std::uint64_t refused = 0;   ///< dropped on us where nothing took it, or unreadable
};

/// What the data transfer needs of the application's windows.
class transfer_host {
public:
    /// The UI of the window whose surface is `s` (null: not one of ours).
    [[nodiscard]] virtual lx::ui::ui_root* host_root_of(const lx::wayland::surface* s) = 0;
    /// The surface a drag starts from: the window the pointer is in (null: none).
    [[nodiscard]] virtual lx::wayland::surface* host_drag_origin() = 0;

protected:
    ~transfer_host() = default;
};

class clipboard final : public lx::wayland::data_device_handler,
                        public lx::wayland::data_source_handler {
public:
    /// A selection or drop read past this many bytes is dropped.
    static constexpr std::size_t k_max_bytes = std::size_t{16} << 20;
    /// Sends in flight at once (a slow reader each).
    static constexpr unsigned k_writers = 4;

    clipboard();
    ~clipboard() override;
    clipboard(const clipboard&) = delete;
    clipboard& operator=(const clipboard&) = delete;

    /// Takes the default seat's data devices — the clipboard's (with drag and drop) and the
    /// primary selection's, each where the compositor has its manager — and becomes lx.ui's
    /// clipboard sink and drag starter. Without them a selection stays the process's own and
    /// nothing drags (not an error). UI affinity, after the display connected.
    [[nodiscard]] lx::result<void> start(lx::wayland::display& d, lx::runtime::event_loop& loop,
                                         transfer_host* host);
    /// Before the display disconnects.
    void stop();
    [[nodiscard]] bool
    active(lx::ui::clipboard_kind kind = lx::ui::clipboard_kind::clipboard) const {
        return channel_of(kind).device.valid();
    }

    /// A copy: what lx.ui's clipboard then holds, offered to other clients.
    void set_text(const char* text);
    /// What a paste takes (lx.ui's clipboard text), and a middle click (its primary).
    [[nodiscard]] const char* text() const;
    [[nodiscard]] const char* primary_text() const;
    /// Another client's selection of `kind` is being read.
    [[nodiscard]] bool
    reading(lx::ui::clipboard_kind kind = lx::ui::clipboard_kind::clipboard) const {
        return channel_of(kind).pending != nullptr;
    }
    [[nodiscard]] const clipboard_stats&
    stats(lx::ui::clipboard_kind kind = lx::ui::clipboard_kind::clipboard) const {
        return channel_of(kind).stats;
    }
    [[nodiscard]] const drag_stats& drags() const { return drag_stats_; }
    /// A drop is being read.
    [[nodiscard]] bool reading_drop() const { return drop_reader_ != nullptr; }
    /// The mime type that marks this process's own offers.
    [[nodiscard]] const char* own_mime() const { return own_mime_; }

    // data_device_handler, data_source_handler
    void on_selection(lx::wayland::data_device& d, lx::wayland::data_offer* offer) override;
    void on_drag_enter(lx::wayland::data_device& d, lx::wayland::data_offer* offer,
                       lx::wayland::surface* target, double x, double y,
                       std::uint32_t serial) override;
    void on_drag_motion(lx::wayland::data_device& d, double x, double y) override;
    void on_drag_leave(lx::wayland::data_device& d) override;
    void on_drop(lx::wayland::data_device& d, lx::wayland::data_offer* offer) override;
    void on_send(lx::wayland::data_source& s, const char* mime, int fd) override;
    void on_cancelled(lx::wayland::data_source& s) override;
    void on_action(lx::wayland::data_source& s, std::uint32_t action) override;
    void on_dnd_finished(lx::wayland::data_source& s) override;

private:
    struct reader;
    struct writer;

    /// One selection: its device, this client's source and text, the text last read.
    struct channel {
        lx::ui::clipboard_kind kind = lx::ui::clipboard_kind::clipboard;
        lx::wayland::data_device device{};
        lx::wayland::data_source source{};
        reader* pending = nullptr; ///< owned: the selection being read
        std::string own{};
        std::string system{};
        clipboard_stats stats{};
    };

    static void sink(void* user, lx::ui::clipboard_kind kind, const char* utf8);
    static bool start_drag(void* user, const char* utf8);
    [[nodiscard]] channel& channel_of(lx::ui::clipboard_kind kind) {
        return channels_[kind == lx::ui::clipboard_kind::primary ? 1 : 0];
    }
    [[nodiscard]] const channel& channel_of(lx::ui::clipboard_kind kind) const {
        return channels_[kind == lx::ui::clipboard_kind::primary ? 1 : 0];
    }
    [[nodiscard]] channel* channel_of(const lx::wayland::data_device& d);
    [[nodiscard]] channel* channel_of(const lx::wayland::data_source& s);
    void offer(channel& c, const char* text);
    /// A pipe's read end on the loop, reading `offer` as `mime` for `c` (null: a drop).
    [[nodiscard]] reader* read(lx::wayland::data_offer& offer, const char* mime, channel* c);
    void stop_reader(reader*& r);
    void finish_read(reader& r, bool ok);
    void finish_write(writer& w);
    /// Writes `text` to `fd` (taken), the rest waiting on the loop. False: no writer free.
    bool write(int fd, const std::string& text);
    /// The drop target under the drag: accept or refuse the offer as that changes.
    void update_drop();
    void reset_drag_over();
    void end_drag(bool moved);

    lx::wayland::display* display_ = nullptr;
    lx::runtime::event_loop* loop_ = nullptr;
    transfer_host* host_ = nullptr;
    channel channels_[2]{};
    writer* writers_[k_writers]{}; ///< owned
    // Our drag: its source, text, and the action the compositor chose.
    lx::wayland::data_source drag_source_{};
    std::string drag_text_{};
    std::uint32_t drag_action_ = lx::wayland::k_dnd_none;
    // A drag over our windows: its offer, the UI under it, where, the type it would be read as.
    lx::wayland::data_offer* drag_offer_ = nullptr;
    lx::ui::ui_root* drag_root_ = nullptr;
    lx::point2i drag_at_{};
    const char* drag_mime_ = nullptr;
    std::uint32_t drag_serial_ = 0;
    int drag_accepting_ = -1; ///< what the offer was last told: -1 nothing yet, 0 no, 1 yes
    // A drop being read: where it goes.
    reader* drop_reader_ = nullptr; ///< owned
    lx::ui::ui_root* drop_root_ = nullptr;
    lx::point2i drop_at_{};
    char own_mime_[48]{};
    drag_stats drag_stats_{};
};

} // namespace lx::app
