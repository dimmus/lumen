#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.runtime;

#include "greetd.hpp"

namespace lumen::dm {

// Real nonblocking AF_UNIX descriptors and lx.runtime's epoll loop; bypass only
// UID verification for conversation cases so the tests do not need greetd or root.
struct conversation_test {
    static void connect(conversation& c, lx::unique_fd fd, bool verify_peer = false) {
        c.attach(std::move(fd), false);
        if (verify_peer)
            c.connected();
        else
            c.ready();
    }

    static bool input_tail_wiped(const conversation& c) {
        return std::ranges::all_of(std::span{c.input_}.subspan(c.input_size_),
                                   [](char byte) { return byte == 0; });
    }

    static bool input_wiped(const conversation& c) {
        return c.input_size_ == 0 && c.input_chars_ == 0 && c.caret_byte_ == 0 &&
               c.caret_index_ == 0 && input_tail_wiped(c);
    }

    static bool sent_wiped(const conversation& c) {
        return c.tx_size_ == 0 && std::ranges::all_of(c.tx_, [](char byte) { return byte == 0; });
    }

    static int socket(const conversation& c) { return c.fd_.get(); }
    static void expire(conversation& c) { c.deadline_ = c.loop_.now(); }
};

} // namespace lumen::dm

namespace {

using namespace lumen::dm;

constexpr std::string_view success = R"({"type":"success"})";
constexpr std::string_view secret =
    R"({"type":"auth_message","auth_message_type":"secret","auth_message":"Password:"})";
constexpr std::string_view cancelled = R"({"type":"cancel_session"})";
constexpr std::string_view create = R"({"type":"create_session","username":"sally"})";
constexpr std::string_view ack = R"({"type":"post_auth_message_response","response":null})";
constexpr std::string_view start =
    R"({"type":"start_session","cmd":["'/usr/bin/lumen-session'"],"env":["XDG_SESSION_TYPE=wayland"]})";

unsigned checked = 0;

void expect(bool value, const char* check) {
    if (!value) {
        std::println(stderr, "FAIL: {}", check);
        std::exit(1);
    }
    ++checked;
}

std::string framed(std::string_view message) {
    const auto size = static_cast<std::uint32_t>(message.size());
    std::string result(sizeof(size), '\0');
    std::memcpy(result.data(), &size, sizeof(size));
    result.append(message);
    return result;
}

struct fixture {
    lx::runtime::event_loop loop{};
    unsigned changes = 0;
    conversation login;
    lx::unique_fd server{};

    explicit fixture(bool verify_peer = false, bool split_vt = false)
        : login{loop, "/dev/null/lumen-dm-test", "'/usr/bin/lumen-session'", [this] { ++changes; },
                split_vt} {
        connect(verify_peer);
    }

    void connect(bool verify_peer = false) {
        int fd[2]{};
        expect(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fd) == 0,
               "socketpair created");
        server.reset(fd[0]);
        conversation_test::connect(login, lx::unique_fd{fd[1]}, verify_peer);
    }

    void run() {
        lx::runtime::timer_source stop{0, [&] { loop.quit(); }, false};
        stop.schedule_at(loop.now() + 1'000'000);
        loop.add_source(&stop);
        (void)loop.run();
        loop.remove_source(&stop);
    }

    void write(std::string_view bytes) {
        const auto n = ::send(server.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
        expect(n >= 0 && static_cast<std::size_t>(n) == bytes.size(), "server wrote fixture bytes");
        run();
    }

    void reply(std::string_view message) { write(framed(message)); }

    void read(std::span<char> out) {
        while (!out.empty()) {
            const auto n = ::recv(server.get(), out.data(), out.size(), 0);
            if (n < 0 && errno == EINTR)
                continue;
            expect(n > 0, "server received request bytes");
            out = out.subspan(static_cast<std::size_t>(n));
        }
    }

    void request(std::string_view expected) {
        std::uint32_t size = 0;
        read({reinterpret_cast<char*>(&size), sizeof(size)});
        expect(size == expected.size(), "request has expected header and size");
        std::string bytes(size, '\0');
        read(bytes);
        expect(bytes == expected, "request matches expected wire JSON");
    }

    void waiting() {
        char byte{};
        const auto n = ::recv(server.get(), &byte, 1, 0);
        expect(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK), "no unexpected request");
    }

    void begin() {
        expect(login.view().state == phase::username, "begin at username");
        login.append("sally");
        login.submit();
        request(create);
    }
};

void split_start() {
    fixture f{false, true};
    f.login.report_presented();
    f.request(R"({"type":"greeter_presented","version":1})");
    expect(f.login.view().state == phase::reporting && !f.login.editable(),
           "credential editing is disabled while reporting the first greeter frame");
    f.login.append("sally");
    f.login.submit();
    f.waiting();
    f.reply(R"({"type":"error","error_type":"error","description":"no pending return"})");
    expect(f.login.view().state == phase::offline && conversation_test::socket(f.login) < 0,
           "initial boot report closes its one-shot stream before auth reconnects");
    f.connect();
    f.begin();
    f.write(framed(success));
    f.run();
    f.request(R"({"type":"start_session_split_vt","version":1})");
    expect(f.login.view().state == phase::starting && !f.login.view().exit_code(),
           "split greeter waits for the broker to finish the handoff");
    f.write(framed(success));
    f.run();
    expect(f.login.view().state == phase::started && f.login.view().exit_code() == 0,
           "split greeter exits only after the broker acknowledges the handoff");
}

void visible_caret() {
    fixture f;
    f.login.append("A\xc3\xa9\xf0\x9f\x94\x91Z");
    expect(f.login.view().caret_index == 4 && f.login.view().mask_length == 0,
           "visible caret counts scalars, not bytes");
    f.login.place_caret(1);
    f.login.append("\xcc\x81-");
    expect(f.login.view().visible_value == "A\xcc\x81-\xc3\xa9\xf0\x9f\x94\x91Z" &&
               f.login.view().caret_index == 3,
           "middle insertion advances by scalars, including a combining mark");
    f.login.erase();
    f.login.erase_forward();
    expect(f.login.view().visible_value == "A\xcc\x81\xf0\x9f\x94\x91Z" &&
               f.login.view().caret_index == 2 && conversation_test::input_tail_wiped(f.login),
           "backspace and forward delete shift the suffix and wipe its old tail");
    f.login.move_caret(1);
    f.login.erase();
    expect(f.login.view().visible_value == "A\xcc\x81Z" && f.login.view().caret_index == 2 &&
               conversation_test::input_tail_wiped(f.login),
           "movement and deletion do not split a four-byte scalar");

    f.login.move_caret(std::numeric_limits<int>::min());
    expect(f.login.view().caret_index == 0, "large negative movement clamps at the beginning");
    const auto changes = f.changes;
    f.login.erase();
    f.login.move_caret(-1);
    f.login.append("Q\xf0\x9f");
    expect(f.changes == changes && f.login.view().visible_value == "A\xcc\x81Z",
           "boundary no-ops and malformed insertion neither edit nor notify");
    f.login.place_caret(std::numeric_limits<std::size_t>::max());
    f.login.erase_forward();
    expect(f.changes == changes + 1 && f.login.view().caret_index == 3,
           "large placement clamps at the end and notifies once");
    f.login.place_caret(1);
    f.login.append(f.login.view().visible_value);
    expect(f.login.view().visible_value == "AA\xcc\x81Z\xcc\x81Z" &&
               f.login.view().caret_index == 4,
           "insertion can use the current visible view without corrupting its bytes");
    f.login.clear_input();
    expect(f.login.view().caret_index == 0 && conversation_test::input_wiped(f.login),
           "clearing wipes the buffer and resets both caret positions");
}

void multi_prompt() {
    fixture f;
    f.login.submit();
    f.waiting();
    f.begin();
    const auto greeting = framed(
        R"({"type":"auth_message","auth_message_type":"info","auth_message":"First\nnotice"})");
    f.write(std::string_view{greeting}.substr(0, 2));
    expect(f.login.view().state == phase::authenticating,
           "partial header cannot advance the state");
    f.write(std::string_view{greeting}.substr(2, 12));
    expect(f.login.view().state == phase::authenticating, "partial JSON cannot advance the state");
    f.write(std::string_view{greeting}.substr(14));
    expect(f.login.view().state == phase::info && f.login.view().prompt == "First\nnotice",
           "fragmented info prompt is decoded");
    f.waiting();
    f.login.submit();
    f.request(ack);
    f.reply(
        R"({"auth_message":"Token name?","auth_message_type":"visible","type":"auth_message"})");
    expect(f.login.view().state == phase::visible, "visible PAM prompt accepts text");
    f.login.append("caf\xc3\xa9");
    f.login.append("\xf0\x9f\x94\x91");
    f.login.erase();
    f.login.append("\n");
    f.login.append("\xc0\x80");
    expect(f.login.view().visible_value == "caf\xc3\xa9",
           "UTF-8 editing accepts scalars and rejects controls");
    f.login.submit();
    f.request("{\"type\":\"post_auth_message_response\",\"response\":\"caf\xc3\xa9\"}");
    f.reply(secret);
    expect(f.login.view().state == phase::secret && f.login.view().prompt == "Password:",
           "secret PAM prompt");
    // Synthetic punctuation and Unicode test vector; no account credential.
    f.login.append("[\xc3\xb6\xf0\x9f\xa7\xaa]");
    f.login.place_caret(2);
    expect(f.login.view().visible_value.data() == nullptr && f.login.view().mask_length == 4 &&
               f.login.view().caret_index == 2,
           "masked view exposes scalar count and position, with no pointer to the input");
    f.login.erase_forward();
    f.login.erase();
    expect(f.login.view().mask_length == 2 && f.login.view().caret_index == 1 &&
               conversation_test::input_tail_wiped(f.login),
           "middle deletions of masked multibyte text wipe the trailing bytes");
    f.login.append("\"\\\xe2\x98\x83");
    f.login.move_caret(-2);
    expect(f.login.view().visible_value.data() == nullptr && f.login.view().mask_length == 5 &&
               f.login.view().caret_index == 2,
           "editing and moving within masked text still exposes no input bytes");
    f.login.submit();
    expect(conversation_test::input_wiped(f.login) && conversation_test::sent_wiped(f.login) &&
               f.login.view().caret_index == 0,
           "submitting resets the caret and wipes the secret field and sent frame");
    f.request("{\"type\":\"post_auth_message_response\",\"response\":\"[\\\"\\\\\xe2\x98\x83]\"}");
    f.reply(
        R"({"type":"auth_message","auth_message_type":"error","auth_message":"Additional step required"})");
    expect(f.login.view().state == phase::error, "PAM error message is not a terminal denial");
    f.login.submit();
    f.request(ack);
    f.reply(secret);
    expect(f.login.view().mask_length == 0 && f.login.view().caret_index == 0,
           "new prompt has no prior input or caret position");
    f.login.submit();
    f.request(R"({"type":"post_auth_message_response","response":""})");
    f.reply(success);
    expect(f.login.view().state == phase::starting && !f.login.view().exit_code(),
           "authentication success does not exit before a session-start acknowledgement");
    f.request(start);
    f.reply(success);
    expect(f.login.view().state == phase::started && f.login.view().exit_code() == 0,
           "only StartSession acknowledgement returns a successful greeter exit");
}

void cancel_and_retry() {
    fixture f;
    f.begin();
    f.reply(secret);
    f.login.append("not sent");
    f.login.cancel();
    f.request(cancelled);
    expect(conversation_test::input_wiped(f.login) && f.login.view().state == phase::cancelling,
           "cancelling secret input wipes the field and sends a cancellation only");
    f.reply(success);
    f.begin();
    f.login.cancel();
    f.waiting();
    f.reply(success);
    f.request(cancelled);
    f.reply(success);
    f.begin();
    f.reply(R"({"type":"error","error_type":"auth_error","description":"sensitive diagnostic"})");
    f.request(cancelled);
    f.reply(R"({"type":"error","error_type":"error","description":"PAM worker gone"})");
    f.request(cancelled);
    f.reply(success);
    expect(f.login.view().state == phase::username && f.login.view().status == notice::denied &&
               f.login.view().prompt.empty() && f.login.view().visible_value.empty(),
           "auth denial returns a generic retry view without the daemon description");
    f.begin();
    f.reply(secret);
    f.login.append("wipe on EOF");
    f.server.reset();
    f.run();
    expect(f.login.view().state == phase::offline && conversation_test::input_wiped(f.login),
           "EOF wipes an in-progress secret and offers retry");
    f.login.reconnect();
    expect(f.login.view().state == phase::offline, "failed reconnect remains retryable");
    f.connect();
    f.request(cancelled);
    f.reply(success);
    f.begin();
    f.reply(success);
    f.request(start);
    f.server.reset();
    f.run();
    expect(f.login.view().state == phase::fatal &&
               f.login.view().status == notice::uncertain_start && f.login.view().exit_code() == 1,
           "lost start acknowledgement exits for supervision, without another start");
    f.login.reconnect();
    expect(f.login.view().state == phase::fatal, "cannot reconnect after an ambiguous start");
}

void silent_start() {
    fixture f;
    f.begin();
    f.reply(success);
    f.request(start);
    conversation_test::expire(f.login);
    f.run();
    expect(f.login.view().status == notice::uncertain_start && f.login.view().exit_code() == 1,
           "silent start broker times out and exits for supervision without a second start");
    char byte{};
    expect(::recv(f.server.get(), &byte, 1, 0) == 0, "start timeout closed the stream");
}

void blocked_write(bool cancel) {
    fixture f;
    f.begin();
    f.reply(secret);
    const int fd = conversation_test::socket(f.login);
    const int buffer = 4096;
    expect(::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)) == 0,
           "use small send buffer for deterministic backpressure");
    std::array<char, 1024> block{};
    std::size_t filled = 0;
    for (;;) {
        const auto sent = ::send(fd, block.data(), block.size(), MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        expect(sent > 0, "fill the client socket before submitting");
        filled += static_cast<std::size_t>(sent);
    }
    f.login.append(std::string(512, 'a'));
    f.login.place_caret(256);
    f.login.append("\xc3\xa9");
    expect(f.login.view().mask_length == 512 && f.login.view().caret_index == 256,
           "an over-capacity middle insertion leaves the bounded input and caret unchanged");
    f.login.submit();
    expect(conversation_test::input_wiped(f.login) && !conversation_test::sent_wiped(f.login),
           "would-block retains only the pending wire buffer, not the secret field");
    if (cancel) {
        f.login.cancel();
        expect(f.login.view().state == phase::offline && conversation_test::sent_wiped(f.login),
               "cancelling a blocked response drops and wipes its frame");
        f.connect();
        f.request(cancelled);
        f.reply(success);
    } else {
        std::vector<char> drain(filled);
        f.read(drain);
        f.run();
        f.request(std::string{R"({"type":"post_auth_message_response","response":")"} +
                  std::string(512, 'a') + "\"}");
        expect(conversation_test::sent_wiped(f.login),
               "epoll writable flushes and wipes the response");
        conversation_test::expire(f.login);
        f.run();
        expect(f.login.view().state == phase::offline && conversation_test::sent_wiped(f.login),
               "unanswered authentication request times out without retaining a frame");
    }
}

void invalid_frames() {
    for (const auto json :
         {"", "[]", "{", "{} {}", "{\"type\":\"success\"}junk",
          R"({"type":"auth_message","auth_message_type":"wrong","auth_message":"x"})",
          R"({"type":"auth_message","auth_message_type":"secret","auth_message":"\u0000"})"}) {
        fixture f;
        f.begin();
        f.reply(json);
        expect(f.login.view().state == phase::fatal && f.login.view().status == notice::protocol,
               "malformed framing or JSON is fatal");
    }
    fixture f;
    f.begin();
    constexpr std::uint32_t oversized = 1U << 31;
    f.write({reinterpret_cast<const char*>(&oversized), sizeof(oversized)});
    expect(f.login.view().state == phase::fatal,
           "reject oversized frame without waiting for its payload");
    fixture unsolicited;
    unsolicited.reply(success);
    expect(unsolicited.login.view().state == phase::fatal,
           "unsolicited response cannot start a session");
}

void trust_boundaries() {
    expect(!session_word("lumen-session") && !session_word("/usr/bin/not-lumen") &&
               !session_word("/usr/../bin/lumen-session") &&
               !session_word("/usr/bin//lumen-session") &&
               !session_word("/usr/\nbin/lumen-session"),
           "launch argument rejects relative, wrong, noncanonical and control-containing paths");
    expect(session_word("/opt/O'Brien $HOME/lumen-session") ==
               "'/opt/O'\\''Brien $HOME/lumen-session'",
           "launch is one shell-quoted word, including apostrophes and metacharacters");
    fixture f{true};
    expect(::geteuid() == 0
               ? f.login.view().state == phase::username
               : f.login.view().state == phase::fatal && f.login.view().status == notice::bad_peer,
           "production SO_PEERCRED verification enforces root even on a live socket");
}

} // namespace

int main() {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    split_start();
    visible_caret();
    multi_prompt();
    cancel_and_retry();
    silent_start();
    blocked_write(false);
    blocked_write(true);
    invalid_frames();
    trust_boundaries();
    std::println("lumen-dm: {} checks passed", checked);
}
