module;

// lx.app:clipboard — the Wayland clipboard (wl_data_device on the default seat) behind
// lx.ui's `clipboard::global()`. A copy in the UI becomes this client's selection, set with
// the serial of the input event that asked for it, and offered as UTF-8 text under the
// usual names. Another client's selection is read as soon as it is offered — text only,
// through a non-blocking pipe on the loop — into what the next paste takes, so a paste stays
// synchronous (the field inserts at once). Until that read ends, a paste takes the previous
// text. Our own selection, coming back to us, is recognised by a per-process mime type and
// not read. Sends are written without blocking, the rest waiting on the loop for room.
// Bodies in clipboard.cpp.

export module lx.app:clipboard;

import std;

import lx.foundation;
import lx.runtime;
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

class clipboard final : public lx::wayland::data_device_handler,
                        public lx::wayland::data_source_handler {
public:
    /// A selection read past this many bytes is dropped.
    static constexpr std::size_t k_max_bytes = std::size_t{16} << 20;
    /// Sends in flight at once (a slow reader each).
    static constexpr unsigned k_writers = 4;

    clipboard();
    ~clipboard() override;
    clipboard(const clipboard&) = delete;
    clipboard& operator=(const clipboard&) = delete;

    /// Takes the default seat's data device and becomes lx.ui's clipboard sink. Without
    /// wl_data_device_manager or a seat it does nothing and the clipboard stays the process's
    /// own (not an error). UI affinity, after the display connected.
    [[nodiscard]] lx::result<void> start(lx::wayland::display& d, lx::runtime::event_loop& loop);
    /// Before the display disconnects.
    void stop();
    [[nodiscard]] bool active() const { return device_.valid(); }

    /// A copy: what lx.ui's clipboard then holds, offered to other clients.
    void set_text(const char* text);
    /// What a paste takes (lx.ui's clipboard text).
    [[nodiscard]] const char* text() const;
    /// Another client's selection is being read.
    [[nodiscard]] bool reading() const { return reader_ != nullptr; }
    [[nodiscard]] const clipboard_stats& stats() const { return stats_; }
    /// The mime type that marks this process's own offers.
    [[nodiscard]] const char* own_mime() const { return own_mime_; }

    // data_device_handler, data_source_handler
    void on_selection(lx::wayland::data_device& d, lx::wayland::data_offer* offer) override;
    void on_send(lx::wayland::data_source& s, const char* mime, int fd) override;
    void on_cancelled(lx::wayland::data_source& s) override;

private:
    struct reader;
    struct writer;

    static void sink(void* user, const char* utf8);
    void offer(const char* text);
    void stop_reader();
    void finish_read(reader& r, bool ok);
    void finish_write(writer& w);

    lx::wayland::display* display_ = nullptr;
    lx::runtime::event_loop* loop_ = nullptr;
    lx::wayland::data_device device_{};
    lx::wayland::data_source source_{};
    reader* reader_ = nullptr;     ///< owned
    writer* writers_[k_writers]{}; ///< owned
    /// The text this client offers, and the last one it read from another client: what
    /// lx.ui's clipboard points at.
    std::string own_{};
    std::string system_{};
    char own_mime_[48]{};
    clipboard_stats stats_{};
};

} // namespace lx::app
