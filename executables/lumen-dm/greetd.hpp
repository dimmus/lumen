#pragma once

namespace lumen::dm {

// greetd 0.10 joins cmd elements into a /bin/sh command. Return a single
// shell-quoted word for the trusted absolute executable path.
[[nodiscard]] std::optional<std::string> session_word(std::string_view path);

enum class phase {
    offline,
    connecting,
    reporting,
    username,
    authenticating,
    visible,
    secret,
    info,
    error,
    cancelling,
    starting,
    started,
    fatal,
};

enum class notice {
    none,
    denied,
    service_error,
    disconnected,
    bad_peer,
    protocol,
    uncertain_start
};

struct screen {
    phase state;
    notice status;
    std::string_view prompt;
    std::string_view visible_value;
    std::size_t mask_length;
    // Insertion position in Unicode scalars, including for the masked view.
    std::size_t caret_index;

    // If greetd accepted an ambiguous start, exiting releases the user session;
    // otherwise the watched greeter's exit lets the supervisor recover.
    [[nodiscard]] std::optional<int> exit_code() const {
        if (state == phase::started)
            return 0;
        if (state == phase::fatal)
            return 1;
        return std::nullopt;
    }
};

// One source on the greeter's UI loop. Exactly one request is outstanding;
// closing a stream that may own a PAM session requires cancellation on reconnect.
class conversation final : public lx::runtime::event_source {
public:
    conversation(lx::runtime::event_loop& loop, std::string socket, std::string session_command,
                 lx::runtime::callback changed, bool split_vt = false);
    ~conversation() override;
    conversation(const conversation&) = delete;
    conversation& operator=(const conversation&) = delete;

    void reconnect();
    void report_presented();
    void submit();
    void cancel();
    void clear_input();
    void place_caret(std::size_t index);
    void move_caret(int delta);
    void erase();
    void erase_forward();
    void append(std::string_view utf8);
    [[nodiscard]] screen view() const;
    [[nodiscard]] bool editable() const;

private:
    friend struct conversation_test;
    enum class request { create, answer, acknowledge, cancel, start, presented };

    bool prepare(lx::runtime::clock_time& timeout) override;
    bool check() override;
    bool dispatch() override;
    [[nodiscard]] int poll_fd() const override { return fd_.get(); }
    void on_readable() override { readable_ = true; }
    void on_writable() override { writable_ = true; }

    void attach(lx::unique_fd fd, bool connecting);
    void connected();
    void ready();
    void close();
    void receive();
    void send();
    void response(std::string_view json);
    void queue(request kind);
    void cancel_session(notice after);
    void lost();
    void fatal(notice reason);
    [[nodiscard]] std::size_t previous_scalar(std::size_t offset) const;
    [[nodiscard]] std::size_t next_scalar(std::size_t offset) const;
    void remove_scalar(std::size_t first, std::size_t last);
    void wipe_input();
    void wipe_tx();
    void changed();

    static constexpr std::size_t k_input_bytes = 512;
    static constexpr std::size_t k_rx_bytes = 8192;
    static constexpr std::size_t k_tx_bytes = 8192;
    static constexpr std::size_t k_prompt_bytes = 2048;
    static constexpr std::size_t k_header_bytes = sizeof(std::uint32_t);

    lx::runtime::event_loop& loop_;
    const std::string socket_;
    const std::string session_command_;
    lx::runtime::callback changed_;
    const bool split_vt_;
    lx::unique_fd fd_{};
    lx::runtime::clock_time deadline_ = 0;
    phase phase_ = phase::offline;
    notice notice_ = notice::none;
    bool may_own_session_ = false;
    bool cancel_requested_ = false;
    bool confirm_cancel_ = false;
    bool readable_ = false;
    bool writable_ = false;

    std::array<char, k_input_bytes> input_{};
    std::size_t input_size_ = 0;
    std::size_t input_chars_ = 0;
    std::size_t caret_byte_ = 0;
    std::size_t caret_index_ = 0;
    std::array<char, k_prompt_bytes> prompt_{};
    std::size_t prompt_size_ = 0;
    std::array<char, k_header_bytes + k_rx_bytes> rx_{};
    std::size_t rx_size_ = 0;
    std::uint32_t rx_payload_ = 0;
    std::array<char, k_header_bytes + k_tx_bytes> tx_{};
    std::size_t tx_size_ = 0;
    std::size_t tx_sent_ = 0;
};

} // namespace lumen::dm
