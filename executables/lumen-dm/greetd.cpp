#include <errno.h>
#include <json-c/json.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

import std;
import lx.foundation;
import lx.runtime;

#include "greetd.hpp"

namespace lumen::dm {
namespace {

constexpr std::string_view k_session_executable = "lumen-session";
constexpr lx::runtime::clock_time k_transport_timeout = 5'000'000'000;
constexpr lx::runtime::clock_time k_split_start_timeout = 12'000'000'000;
constexpr lx::runtime::clock_time k_auth_timeout = 120'000'000'000;

// A single valid scalar; allow combining marks, layout-dependent symbols and
// spaces, but not control characters masquerading as text input.
unsigned scalar_bytes(std::string_view text) {
    const auto byte = [&](unsigned i) { return static_cast<unsigned char>(text[i]); };
    const unsigned lead = byte(0);
    if (lead < 0x80)
        return lead >= 0x20 && lead != 0x7f ? 1 : 0;
    const unsigned n = lead < 0xc2 ? 0 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : lead < 0xf5 ? 4 : 0;
    if (!n || n > text.size())
        return 0;
    unsigned cp = lead & (0x7fu >> n);
    for (unsigned i = 1; i < n; ++i) {
        const unsigned b = byte(i);
        if ((b & 0xc0) != 0x80)
            return 0;
        cp = (cp << 6) | (b & 0x3f);
    }
    constexpr unsigned minimum[] = {0, 0, 0xa0, 0x800, 0x10000};
    return cp >= minimum[n] && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff) && cp != 0x2028 &&
                   cp != 0x2029
               ? n
               : 0;
}

[[nodiscard]] bool continuation_byte(char byte) {
    return (static_cast<unsigned char>(byte) & 0xc0u) == 0x80u;
}

// Serialize directly into writable, zeroizable storage. The only inputs to
// string() are validated printable UTF-8, so quote/backslash are the only escapes.
struct writer {
    std::span<char> bytes;
    std::size_t size = 0;
    bool ok = true;

    void add(std::string_view data) {
        if (!ok || data.size() > bytes.size() - size) {
            ok = false;
            return;
        }
        std::ranges::copy(data, bytes.begin() + static_cast<std::ptrdiff_t>(size));
        size += data.size();
    }

    void string(std::string_view data) {
        add("\"");
        for (const char& c : data) {
            if (c == '"' || c == '\\')
                add("\\");
            add({&c, 1});
        }
        add("\"");
    }
};

std::optional<std::string_view> member(json_object* object, const char* name) {
    json_object* value = nullptr;
    if (!json_object_object_get_ex(object, name, &value) ||
        !json_object_is_type(value, json_type_string))
        return std::nullopt;
    return std::string_view{json_object_get_string(value),
                            static_cast<std::size_t>(json_object_get_string_len(value))};
}

} // namespace

std::optional<std::string> session_word(std::string_view path) {
    if (path.empty() || path.front() != '/' || path.size() > 1024 ||
        path.substr(path.find_last_of('/') + 1) != k_session_executable)
        return std::nullopt;
    for (std::size_t pos = 1; pos < path.size();) {
        const auto end = path.find('/', pos);
        const auto component = path.substr(pos, end == path.npos ? end : end - pos);
        if (component.empty() || component == "." || component == ".." ||
            !std::ranges::all_of(component, [](unsigned char c) { return c >= 0x20 && c <= 0x7e; }))
            return std::nullopt;
        if (end == path.npos)
            break;
        pos = end + 1;
    }

    std::string word{"'"};
    for (const char c : path) {
        if (c == '\'')
            word += "'\\''";
        else
            word += c;
    }
    word += '\'';
    return word;
}

conversation::conversation(lx::runtime::event_loop& loop, std::string socket,
                           std::string session_command, lx::runtime::callback changed,
                           bool split_vt)
    : loop_(loop),
      socket_(std::move(socket)),
      session_command_(std::move(session_command)),
      changed_(std::move(changed)),
      split_vt_(split_vt) {}

conversation::~conversation() {
    close();
}

void conversation::reconnect() {
    if (phase_ != phase::offline)
        return;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_.empty() || socket_.front() != '/' || socket_.size() >= sizeof(address.sun_path)) {
        fatal(notice::bad_peer);
        return;
    }
    std::ranges::copy(socket_, address.sun_path);
    lx::unique_fd fd{::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (fd.get() < 0) {
        lost();
        return;
    }
    const auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + socket_.size() + 1);
    const int outcome = ::connect(fd.get(), reinterpret_cast<const sockaddr*>(&address), len);
    if (outcome < 0 && errno != EINPROGRESS) {
        lost();
        return;
    }
    attach(std::move(fd), outcome < 0);
    if (outcome == 0)
        connected();
}

void conversation::attach(lx::unique_fd fd, bool connecting) {
    fd_ = std::move(fd);
    loop_.add_source(this);
    if (connecting) {
        phase_ = phase::connecting;
        deadline_ = loop_.now() + k_transport_timeout;
        loop_.set_write_interest(this, true);
        changed();
    }
}

void conversation::connected() {
    ucred peer{};
    socklen_t size = sizeof(peer);
    if (::getsockopt(fd_.get(), SOL_SOCKET, SO_PEERCRED, &peer, &size) < 0 ||
        size != sizeof(peer) || peer.uid != 0 || peer.pid <= 0) {
        fatal(notice::bad_peer);
        return;
    }
    ready();
}

void conversation::report_presented() {
    if (!split_vt_ || phase_ != phase::username || may_own_session_)
        return;
    phase_ = phase::reporting;
    queue(request::presented);
}

void conversation::ready() {
    loop_.set_write_interest(this, false);
    if (may_own_session_) {
        cancel_session(notice::none);
        return;
    }
    deadline_ = 0;
    phase_ = phase::username;
    notice_ = notice::none;
    changed();
}

void conversation::close() {
    if (fd_.get() >= 0) {
        loop_.remove_source(this);
        fd_.reset();
    }
    deadline_ = 0;
    readable_ = writable_ = cancel_requested_ = confirm_cancel_ = false;
    rx_size_ = rx_payload_ = prompt_size_ = 0;
    wipe_input();
    wipe_tx();
}

void conversation::lost() {
    if (phase_ == phase::starting) {
        // greetd might already have scheduled the session. Never send StartSession again.
        fatal(notice::uncertain_start);
        return;
    }
    close();
    phase_ = phase::offline;
    notice_ = notice::disconnected;
    changed();
}

void conversation::fatal(notice reason) {
    close();
    phase_ = phase::fatal;
    notice_ = reason;
    changed();
}

bool conversation::prepare(lx::runtime::clock_time& timeout) {
    const auto now = loop_.now();
    timeout = deadline_ ? std::max<lx::runtime::clock_time>(0, deadline_ - now) : -1;
    return timeout == 0;
}

bool conversation::check() {
    return readable_ || writable_ || (deadline_ && loop_.now() >= deadline_);
}

bool conversation::dispatch() {
    const bool read = std::exchange(readable_, false);
    const bool write = std::exchange(writable_, false);
    if (phase_ == phase::connecting && (read || write)) {
        int result = 0;
        socklen_t size = sizeof(result);
        if (::getsockopt(fd_.get(), SOL_SOCKET, SO_ERROR, &result, &size) < 0 || result)
            lost();
        else
            connected();
        return true;
    }
    if (read)
        receive();
    if (fd_.get() >= 0 && write && tx_size_)
        send();
    if (deadline_ && loop_.now() >= deadline_)
        lost();
    return true;
}

void conversation::receive() {
    for (;;) {
        const std::size_t total = k_header_bytes + rx_payload_;
        const auto n = ::recv(fd_.get(), rx_.data() + rx_size_, total - rx_size_, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n <= 0) {
            lost();
            return;
        }
        if (tx_size_ || (phase_ != phase::authenticating && phase_ != phase::cancelling &&
                         phase_ != phase::starting && phase_ != phase::reporting)) {
            fatal(notice::protocol);
            return;
        }
        rx_size_ += static_cast<std::size_t>(n);
        if (rx_size_ != total)
            continue;
        if (rx_payload_ == 0) {
            std::memcpy(&rx_payload_, rx_.data(), k_header_bytes);
            if (rx_payload_ == 0 || rx_payload_ > k_rx_bytes) {
                fatal(notice::protocol);
                return;
            }
        } else {
            const std::string_view json{rx_.data() + k_header_bytes, rx_payload_};
            rx_size_ = rx_payload_ = 0;
            deadline_ = 0;
            response(json);
            return;
        }
    }
}

void conversation::send() {
    while (tx_sent_ < tx_size_) {
        const auto n = ::send(fd_.get(), tx_.data() + tx_sent_, tx_size_ - tx_sent_, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            loop_.set_write_interest(this, true);
            return;
        }
        if (n <= 0) {
            lost();
            return;
        }
        ::explicit_bzero(tx_.data() + tx_sent_, static_cast<std::size_t>(n));
        tx_sent_ += static_cast<std::size_t>(n);
    }
    wipe_tx();
    loop_.set_write_interest(this, false);
    if (phase_ == phase::authenticating)
        deadline_ = loop_.now() + k_auth_timeout;
}

void conversation::response(std::string_view json) {
    const std::unique_ptr<json_tokener, decltype(&json_tokener_free)> tokener{
        json_tokener_new_ex(8), &json_tokener_free};
    if (!tokener) {
        fatal(notice::service_error);
        return;
    }
    json_tokener_set_flags(tokener.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    const std::unique_ptr<json_object, decltype(&json_object_put)> object{
        json_tokener_parse_ex(tokener.get(), json.data(), static_cast<int>(json.size())),
        &json_object_put};
    if (json_tokener_get_error(tokener.get()) != json_tokener_success ||
        json_tokener_get_parse_end(tokener.get()) != json.size() ||
        !json_object_is_type(object.get(), json_type_object)) {
        fatal(notice::protocol);
        return;
    }
    const auto type = member(object.get(), "type");
    if (phase_ == phase::reporting) {
        // This report uses a single-request connection. An error on initial boot
        // simply means there is no desktop waiting for the greeter.
        close();
        phase_ = phase::offline;
        reconnect();
        return;
    }
    if (type == "error") {
        const auto kind = member(object.get(), "error_type");
        if ((kind != "auth_error" && kind != "error") || !member(object.get(), "description")) {
            fatal(notice::protocol);
            return;
        }
        if (phase_ == phase::cancelling) {
            // greetd 0.10 takes the configuring session before notifying the
            // PAM worker, so cancellation can report a broken worker even
            // though the session was removed. Confirm with one more cancel.
            if (confirm_cancel_) {
                lost();
            } else {
                confirm_cancel_ = true;
                queue(request::cancel);
            }
            return;
        }
        cancel_session(cancel_requested_      ? notice::none
                       : kind == "auth_error" ? notice::denied
                                              : notice::service_error);
        return;
    }
    if (type == "success") {
        switch (phase_) {
        case phase::authenticating:
            if (cancel_requested_)
                cancel_session(notice::none);
            else {
                phase_ = phase::starting;
                queue(request::start);
            }
            return;
        case phase::cancelling:
            may_own_session_ = false;
            cancel_requested_ = confirm_cancel_ = false;
            phase_ = phase::username;
            changed();
            return;
        case phase::starting:
            may_own_session_ = false;
            close();
            phase_ = phase::started;
            changed();
            return;
        default:
            break;
        }
    }
    if (type == "auth_message" && phase_ == phase::authenticating) {
        const auto kind = member(object.get(), "auth_message_type");
        const auto text = member(object.get(), "auth_message");
        const phase next = kind == "visible"  ? phase::visible
                           : kind == "secret" ? phase::secret
                           : kind == "info"   ? phase::info
                           : kind == "error"  ? phase::error
                                              : phase::fatal;
        if (!text || next == phase::fatal || text->size() > prompt_.size() ||
            text->find('\0') != text->npos) {
            fatal(notice::protocol);
            return;
        }
        if (cancel_requested_) {
            cancel_session(notice::none);
            return;
        }
        phase_ = next;
        std::ranges::copy(*text, prompt_.begin());
        prompt_size_ = text->size();
        changed();
        return;
    }
    fatal(notice::protocol);
}

void conversation::queue(request kind) {
    writer payload{std::span{tx_}.subspan(k_header_bytes)};
    const std::string_view input{input_.data(), input_size_};
    switch (kind) {
    case request::create:
        payload.add(R"({"type":"create_session","username":)");
        payload.string(input);
        payload.add("}");
        break;
    case request::answer:
        payload.add(R"({"type":"post_auth_message_response","response":)");
        payload.string(input);
        payload.add("}");
        break;
    case request::acknowledge:
        payload.add(R"({"type":"post_auth_message_response","response":null})");
        break;
    case request::cancel:
        payload.add(R"({"type":"cancel_session"})");
        break;
    case request::presented:
        payload.add(R"({"type":"greeter_presented","version":1})");
        break;
    case request::start:
        if (split_vt_) {
            // A distinct request cannot be mistaken for a same-VT start by stock greetd.
            payload.add(R"({"type":"start_session_split_vt","version":1})");
        } else {
            payload.add(R"({"type":"start_session","cmd":[)");
            payload.string(session_command_);
            payload.add(R"(],"env":["XDG_SESSION_TYPE=wayland"]})");
        }
        break;
    }
    wipe_input();
    prompt_size_ = 0;
    if (!payload.ok) {
        fatal(notice::protocol);
        return;
    }
    const auto size = static_cast<std::uint32_t>(payload.size);
    std::memcpy(tx_.data(), &size, k_header_bytes);
    tx_size_ = k_header_bytes + payload.size;
    deadline_ = loop_.now() +
                (kind == request::start && split_vt_ ? k_split_start_timeout : k_transport_timeout);
    send();
    if (fd_.get() >= 0)
        changed();
}

void conversation::submit() {
    switch (phase_) {
    case phase::username:
        if (!input_size_)
            return;
        may_own_session_ = true;
        notice_ = notice::none;
        phase_ = phase::authenticating;
        queue(request::create);
        break;
    case phase::visible:
    case phase::secret:
        phase_ = phase::authenticating;
        queue(request::answer);
        break;
    case phase::info:
    case phase::error:
        phase_ = phase::authenticating;
        queue(request::acknowledge);
        break;
    case phase::offline:
        reconnect();
        break;
    default:
        break;
    }
}

void conversation::cancel_session(notice after) {
    phase_ = phase::cancelling;
    notice_ = after;
    cancel_requested_ = false;
    queue(request::cancel);
}

void conversation::cancel() {
    switch (phase_) {
    case phase::username:
        clear_input();
        break;
    case phase::visible:
    case phase::secret:
    case phase::info:
    case phase::error:
        cancel_session(notice::none);
        break;
    case phase::authenticating:
        if (tx_size_) {
            // We cannot interleave CancelSession with a partial frame. Drop it and
            // explicitly clean up on a freshly authenticated stream.
            lost();
            reconnect();
        } else if (!cancel_requested_) {
            cancel_requested_ = true;
            deadline_ = loop_.now() + k_transport_timeout;
            changed();
        }
        break;
    default:
        break;
    }
}

void conversation::wipe_input() {
    ::explicit_bzero(input_.data(), input_.size());
    input_size_ = input_chars_ = caret_byte_ = caret_index_ = 0;
}

void conversation::wipe_tx() {
    ::explicit_bzero(tx_.data(), tx_.size());
    tx_size_ = tx_sent_ = 0;
}

void conversation::clear_input() {
    if (input_size_) {
        wipe_input();
        changed();
    }
}

bool conversation::editable() const {
    return phase_ == phase::username || phase_ == phase::visible || phase_ == phase::secret;
}

std::size_t conversation::previous_scalar(std::size_t offset) const {
    // Input is already validated; an interior byte can only be a continuation.
    while (continuation_byte(input_[--offset])) {
    }
    return offset;
}

std::size_t conversation::next_scalar(std::size_t offset) const {
    while (++offset < input_size_ && continuation_byte(input_[offset])) {
    }
    return offset;
}

void conversation::place_caret(std::size_t index) {
    if (!editable())
        return;
    index = std::min(index, input_chars_);
    if (index == caret_index_)
        return;
    if (index == 0) {
        caret_byte_ = 0;
    } else if (index == input_chars_) {
        caret_byte_ = input_size_;
    } else {
        for (auto current = caret_index_; current > index; --current)
            caret_byte_ = previous_scalar(caret_byte_);
        for (auto current = caret_index_; current < index; ++current)
            caret_byte_ = next_scalar(caret_byte_);
    }
    caret_index_ = index;
    changed();
}

void conversation::move_caret(int delta) {
    // All indices are bounded by the byte capacity; clamp before adding delta.
    const auto index = static_cast<int>(caret_index_);
    const auto remaining = static_cast<int>(input_chars_ - caret_index_);
    place_caret(static_cast<std::size_t>(index + std::clamp(delta, -index, remaining)));
}

void conversation::append(std::string_view utf8) {
    if (!editable() || utf8.empty() || utf8.size() > input_.size() - input_size_)
        return;
    std::size_t added = 0;
    for (auto chars = utf8; !chars.empty(); ++added) {
        const unsigned n = scalar_bytes(chars);
        if (!n)
            return;
        chars.remove_prefix(n);
    }

    auto* const first = input_.data();
    auto* const at = first + caret_byte_;
    auto* const end = first + input_size_;
    const std::less<const char*> less;
    if (at != end && !less(utf8.data(), first) && less(utf8.data(), end)) {
        // A caller may be inserting part of its own visible_value. Copy it before shifting.
        std::memcpy(end, utf8.data(), utf8.size());
        std::rotate(at, end, end + utf8.size());
    } else {
        if (at != end)
            std::memmove(at + utf8.size(), at, input_size_ - caret_byte_);
        std::memcpy(at, utf8.data(), utf8.size());
    }
    input_size_ += utf8.size();
    input_chars_ += added;
    caret_byte_ += utf8.size();
    caret_index_ += added;
    changed();
}

void conversation::erase() {
    if (!editable() || !caret_index_)
        return;
    const auto last = caret_byte_;
    caret_byte_ = previous_scalar(last);
    --caret_index_;
    remove_scalar(caret_byte_, last);
}

void conversation::erase_forward() {
    if (!editable() || caret_index_ == input_chars_)
        return;
    remove_scalar(caret_byte_, next_scalar(caret_byte_));
}

void conversation::remove_scalar(std::size_t first, std::size_t last) {
    auto* const data = input_.data();
    if (last != input_size_)
        std::memmove(data + first, data + last, input_size_ - last);
    const auto removed = last - first;
    input_size_ -= removed;
    ::explicit_bzero(data + input_size_, removed);
    --input_chars_;
    changed();
}

screen conversation::view() const {
    const auto visible = phase_ == phase::username || phase_ == phase::visible
                             ? std::string_view{input_.data(), input_size_}
                             : std::string_view{};
    return {phase_,
            notice_,
            {prompt_.data(), prompt_size_},
            visible,
            phase_ == phase::secret ? input_chars_ : 0,
            editable() ? caret_index_ : 0};
}

void conversation::changed() {
    if (changed_)
        changed_();
}

} // namespace lumen::dm
