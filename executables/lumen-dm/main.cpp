#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon-keysyms.h>

import std;
import lx.foundation;
import lx.gfx;
import lx.input;
import lx.layout;
import lx.runtime;
import lx.scene;
import lx.session;
import lx.text;
import lx.trace;
import lx.ui;

#include "greetd.hpp"
#include "kms_host.hpp"

namespace {

namespace dm = lumen::dm;
constexpr lx::trace::category log_dm{"lumen.dm"};
constexpr lx::runtime::clock_time k_repeat_delay_ns = 400'000'000;
constexpr lx::runtime::clock_time k_repeat_step_ns = 25'000'000;

[[nodiscard]] lx::color background() {
    return lx::ui::active_theme().color(lx::ui::tok::surface_background);
}

[[nodiscard]] std::uint32_t background_xrgb() {
    const auto color = background();
    const auto channel = [](float value) {
        return static_cast<std::uint32_t>(std::clamp(value, 0.f, 1.f) * 255.f + 0.5f);
    };
    return (channel(color.r) << 16) | (channel(color.g) << 8) | channel(color.b);
}

[[nodiscard]] bool trusted_session(const char* path) {
    struct stat st{};
    return ::stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 &&
           !(st.st_mode & (S_IWGRP | S_IWOTH)) && (st.st_mode & S_IXOTH);
}

[[nodiscard]] const char* notice_label(dm::notice value) {
    switch (value) {
    case dm::notice::none:
        return nullptr;
    case dm::notice::denied:
        return "Sign-in failed. Check your credentials and try again.";
    case dm::notice::service_error:
        return "Sign-in could not be completed. Try again.";
    case dm::notice::disconnected:
        return "The sign-in service is unavailable.";
    case dm::notice::bad_peer:
    case dm::notice::protocol:
    case dm::notice::uncertain_start:
        return nullptr; // Terminal states are logged and exit for supervision.
    }
    return nullptr;
}

[[nodiscard]] const char* phase_label(dm::phase value) {
    switch (value) {
    case dm::phase::username:
        return "Username";
    case dm::phase::offline:
    case dm::phase::fatal:
        return nullptr;
    case dm::phase::connecting:
        return "Connecting to the sign-in service…";
    case dm::phase::authenticating:
        return "Checking credentials…";
    case dm::phase::cancelling:
        return "Cancelling sign-in…";
    case dm::phase::starting:
    case dm::phase::started:
        return "Starting your session…";
    case dm::phase::visible:
    case dm::phase::secret:
        return "Enter your response";
    case dm::phase::info:
    case dm::phase::error:
        return nullptr;
    }
    return nullptr;
}

class greeter;
// The root's describe_fn is a plain function pointer: one binding on this UI thread.
thread_local greeter* active = nullptr;

class greeter final : public lx::runtime::event_source {
public:
    greeter(lx::runtime::event_loop& loop, lx::session::logind_session& session, std::string socket,
            std::string command)
        : loop_(loop),
          session_(session),
          kms_(loop, session,
               {.configure =
                    [](void* user, lx::rect2i rect) {
                        return static_cast<greeter*>(user)->configure(rect);
                    },
                .paint =
                    [](void* user, const lx::gfx::pixel_surface& target) {
                        return static_cast<greeter*>(user)->paint(target);
                    },
                .user = this,
                .background_xrgb = background_xrgb(),
                .presented =
                    [](void* user, std::uint64_t revision) {
                        static_cast<greeter*>(user)->presented_revision_ = revision;
                    }}),
          conversation_(loop, std::move(socket), std::move(command), [this] { changed(); }) {
        repeat_.set_enabled(false);
    }

    ~greeter() override {
        stopping_ = true;
        loop_.set_after_dispatch({});
        loop_.remove_source(&repeat_);
        loop_.remove_source(this);
        if (input_source_) {
            loop_.remove_source(&*input_source_);
            input_.set_sink({});
            input_.suspend();
        }
        reset_compose();
        conversation_.clear_input();
        kms_.close();
        lx::ui::set_ui_resources({});
        active = nullptr;
    }

    [[nodiscard]] lx::result<void> open() {
        if (!arena_.valid())
            return lx::make_error(lx::error_domain::io, 0, "cannot allocate greeter form arena");
        lx::ui::ui_resources resources{};
        resources.text = {{}, &glyphs_, nullptr};
        lx::ui::set_ui_resources(resources);
        active = this;
        root_.set_describer(&describe);
        make_compose();

        if (auto result = session_.set_listener({
                .device_paused =
                    [](unsigned maj, unsigned min, lx::session::device_pause kind, void* user) {
                        static_cast<greeter*>(user)->record_pause(maj, min, kind);
                    },
                .device_resumed =
                    [](unsigned maj, unsigned min, int, void* user) {
                        auto& self = *static_cast<greeter*>(user);
                        self.resume_owed_ |= self.kms_.owns_device(maj, min);
                    },
                .active_changed =
                    [](bool foreground, void* user) {
                        auto& self = *static_cast<greeter*>(user);
                        self.pause_owed_ |= !foreground;
                        self.resume_owed_ |= foreground;
                    },
                .vt_switch_failed =
                    [](const char*, void* user) {
                        static_cast<greeter*>(user)->switch_failed_ = true;
                    },
                .user = this,
            });
            !result)
            return result;
        if (session_.bus_fd() < 0)
            return lx::make_error(lx::io_err::broken_pipe, "logind bus is not available");
        // At simultaneous readiness, handle a pending session loss before input or KMS.
        loop_.add_source(this);
        loop_.add_source(&repeat_);
        kms_.request_frame(form_revision_);
        if (auto result = kms_.open(); !result)
            return result;
        if (session_.is_active()) {
            if (auto result = resume_input(); !result)
                return result;
            engaged_ = true;
        } else {
            kms_.suspend();
        }
        loop_.set_after_dispatch([this] { update_form(); });
        if (engaged_)
            start_conversation();
        update_form();
        return {};
    }

    void stop() {
        stopping_ = true;
        loop_.quit(conversation_.view().exit_code().value_or(1));
    }

private:
    [[nodiscard]] lx::result<void> configure(lx::rect2i rect) {
        const int scale = kms_.scale();
        if (scale <= 0 || rect.width < scale || rect.height < scale)
            return lx::invalid_argument("invalid greeter form extent");
        // A release following an output change must not click at the old target.
        presented_revision_ = 0;
        cancel_pointer();
        form_ = rect;
        root_.set_scale(static_cast<float>(scale));
        root_.set_bounds({0, 0, rect.width / scale, rect.height / scale});
        return {};
    }

    [[nodiscard]] lx::result<void> paint(const lx::gfx::pixel_surface& target) {
        (void)root_.tick(draws_);
        const lx::gfx::ui_stream stream{
            .refs = draws_.refs(),
            .batches = draws_.batches(),
            .quads = draws_.quad_instances(),
            .shadows = draws_.shadow_instances(),
            .sprites = draws_.sprite_instances(),
            .underlines = draws_.underline_instances(),
        };
        lx::gfx::damage_region full{};
        full.set_full();
        const auto result = renderer_.draw(target, stream, &atlas_, full, background());
        if (!result)
            return result.get_error();
        return {};
    }

    [[nodiscard]] bool seat_active() const { return engaged_ && !stopping_ && !vt_switch_pending_; }

    [[nodiscard]] bool accepts_input() const {
        return seat_active() && !cancel_pointer_owed_ && kms_.content_visible() &&
               presented_revision_ == form_revision_;
    }

    void update_form() {
        if (stopping_ || !engaged_)
            return;
        if (std::exchange(cancel_pointer_owed_, false))
            cancel_pointer();
        const auto display = kms_.display_rect();
        if (display.width <= 0 || display.height <= 0) {
            if (display_present_) {
                display_present_ = false;
                clear_focus();
            }
            return;
        }
        display_present_ = true;
        if (!interactive_logged_ && keyboard_ready_ && accepts_input() &&
            conversation_.view().state == dm::phase::username) {
            interactive_logged_ = true;
            log_dm.info("greeter_interactive timestamp_ns={}", loop_.now());
        }
        if (root_.wants_frame())
            kms_.request_frame(form_revision_);
    }

    [[nodiscard]] static int open_input(const char* path, int flags, void* user) {
        auto& session = *static_cast<lx::session::logind_session*>(user);
        bool inactive = false;
        auto device = session.take_device_path(path, &inactive);
        if (!device)
            return -EACCES;
        const int fd = device->get();
        if (inactive) {
            session.release_device_fd(fd);
            return -EACCES;
        }
        const int current = ::fcntl(fd, F_GETFL);
        if (current < 0 ||
            ::fcntl(fd, F_SETFL, (current & ~O_NONBLOCK) | (flags & O_NONBLOCK)) < 0) {
            const int error = errno;
            session.release_device_fd(fd);
            return -error;
        }
        return device->release();
    }

    [[nodiscard]] lx::result<void> resume_input() {
        if (!input_source_) {
            auto opened = lx::input::input_manager::open(
                {
                    .open_device = open_input,
                    .close_device =
                        [](int fd, void* user) {
                            static_cast<lx::session::logind_session*>(user)->release_device_fd(fd);
                            ::close(fd);
                        },
                    .user = &session_,
                },
                "seat0");
            if (!opened)
                return opened.get_error();
            input_ = std::move(*opened);
            if (input_.fd() < 0)
                return lx::make_error(lx::io_err::broken_pipe, "input event fd is not available");
            input_.set_sink({
                .pointer_motion =
                    [](const lx::input::pointer_motion_event& event, void* user) {
                        static_cast<greeter*>(user)->pointer_motion(event);
                    },
                .pointer_button =
                    [](const lx::input::pointer_button_event& event, void* user) {
                        static_cast<greeter*>(user)->pointer_button(event);
                    },
                .key = [](const lx::input::key_event& event,
                          void* user) { static_cast<greeter*>(user)->key(event); },
                .touch = [](const lx::input::touch_event& event,
                            void* user) { static_cast<greeter*>(user)->touch(event); },
                .device_added =
                    [](const char*, bool keyboard, bool, bool, bool, void* user) {
                        static_cast<greeter*>(user)->keyboard_ready_ |= keyboard;
                    },
                .user = this,
            });
            input_source_.emplace(input_.fd(), [this] {
                if (engaged_ && !stopping_)
                    (void)input_.dispatch();
            });
            loop_.add_source(&*input_source_);
        } else if (auto result = input_.resume(); !result) {
            return result;
        }
        // Initial hotplug events and LED initialization need not wait for another wake.
        (void)input_.dispatch();
        input_.update_leds();
        return {};
    }

    bool prepare(lx::runtime::clock_time& timeout) override {
        timeout = session_.dispatch_timeout_ns();
        return bus_ready_ || timeout == 0;
    }

    bool check() override { return bus_ready_ || session_.dispatch_timeout_ns() == 0; }
    [[nodiscard]] int poll_fd() const override { return session_.bus_fd(); }
    void on_readable() override { bus_ready_ = true; }

    bool dispatch() override {
        bus_ready_ = false;
        session_.dispatch();
        if (session_.bus_fd() < 0 || event_overflow_) {
            abort(lx::make_error(lx::io_err::broken_pipe, "greeter logind event stream failed"));
            return true;
        }
        apply_session_events();
        return true;
    }

    struct device_event {
        unsigned major;
        unsigned minor;
        lx::session::device_pause kind;
    };

    void record_pause(unsigned major, unsigned minor, lx::session::device_pause kind) {
        if (!session_.holds_device(major, minor))
            return;
        if (kind != lx::session::device_pause::gone)
            pause_owed_ = true;
        if (kind != lx::session::device_pause::force &&
            !device_events_.try_push_back({major, minor, kind}))
            event_overflow_ = true;
    }

    void apply_session_events() {
        const bool pause = std::exchange(pause_owed_, false);
        const bool resume = std::exchange(resume_owed_, false);
        if (pause || !session_.is_active()) {
            vt_switch_pending_ = false;
            pause_session();
        }
        for (const auto& event : device_events_) {
            if (event.kind == lx::session::device_pause::gone) {
                if (kms_.owns_device(event.major, event.minor))
                    kms_.device_gone(event.major, event.minor);
            } else if (session_.holds_device(event.major, event.minor)) {
                session_.complete_pause(event.major, event.minor);
            }
        }
        device_events_.clear();
        if (std::exchange(switch_failed_, false)) {
            vt_switch_pending_ = false;
            log_dm.warn("greeter VT switch was refused by logind");
        }
        if (!stopping_ && resume && session_.is_active()) {
            // Active can precede DRM master restoration. Retry on the matching ResumeDevice
            // even when the input seat was already resumed on the Active edge.
            if (auto result = kms_.resume(); !result) {
                abort(result.get_error());
                return;
            }
            if (!engaged_) {
                if (auto result = resume_input(); !result) {
                    abort(result.get_error());
                    return;
                }
                engaged_ = true;
                start_conversation();
            }
        }
    }

    void start_conversation() {
        if (!greetd_started_) {
            greetd_started_ = true;
            conversation_.reconnect();
        }
    }

    void abort(lx::error error) {
        log_dm.error(error);
        stopping_ = true;
        pause_session();
        loop_.quit(conversation_.view().exit_code().value_or(1));
    }

    void pause_session() {
        if (!std::exchange(engaged_, false))
            return;
        clear_focus();
        presented_revision_ = 0;
        kms_.suspend();
        input_.suspend();
        auto& seat = input_.default_seat();
        seat.clear_pressed();
        (void)seat.keymap().reset_depressed();
    }

    void clear_focus() {
        cancel_pointer();
        stop_repeat();
        reset_compose();
        const auto screen = conversation_.view();
        if (screen.state == dm::phase::secret && screen.mask_length != 0) {
            invalidate_form_revision();
            conversation_.clear_input();
        }
    }

    void cancel_pointer() {
        touch_slot_.reset();
        root_.pointer_cancel();
        root_.pointer_leave();
    }

    [[nodiscard]] lx::point2i position(double x, double y) const {
        const auto display = kms_.display_rect();
        return {display.x + static_cast<int>(std::clamp(x, 0.0, double(display.width - 1))),
                display.y + static_cast<int>(std::clamp(y, 0.0, double(display.height - 1)))};
    }

    bool route_position(lx::point2i point) {
        const lx::point2i local{point.x - form_.x, point.y - form_.y};
        if (local.x < 0 || local.y < 0 || local.x >= form_.width || local.y >= form_.height) {
            root_.pointer_leave();
            return false;
        }
        const int scale = kms_.scale();
        root_.pointer_move({local.x / scale, local.y / scale});
        return true;
    }

    void pointer_motion(const lx::input::pointer_motion_event& event) {
        if (!seat_active() || touch_slot_)
            return;
        const auto display = kms_.display_rect();
        if (display.width <= 0 || display.height <= 0)
            return;
        if (!pointer_initialized_) {
            pointer_x_ = display.width / 2.0;
            pointer_y_ = display.height / 2.0;
            pointer_initialized_ = true;
        }
        pointer_x_ = std::clamp(event.absolute ? event.x * display.width : pointer_x_ + event.dx,
                                0.0, double(display.width - 1));
        pointer_y_ = std::clamp(event.absolute ? event.y * display.height : pointer_y_ + event.dy,
                                0.0, double(display.height - 1));
        const auto point = position(pointer_x_, pointer_y_);
        kms_.move_pointer(point.x, point.y);
        if (accepts_input())
            (void)route_position(point);
    }

    void pointer_button(const lx::input::pointer_button_event& event) {
        if (!accepts_input() || touch_slot_ || event.which != lx::input::button::left)
            return;
        if (event.device_removed) {
            cancel_pointer();
        } else if (pointer_initialized_ && route_position(position(pointer_x_, pointer_y_))) {
            root_.pointer_button(lx::ui::mouse_button::left, event.pressed);
        } else if (!event.pressed) {
            root_.pointer_cancel();
        }
    }

    void touch(const lx::input::touch_event& event) {
        using phase = lx::input::touch_event::phase;
        if (!accepts_input())
            return;
        if (event.kind == phase::cancel) {
            cancel_pointer();
            return;
        }
        if (event.kind == phase::down) {
            if (touch_slot_)
                return;
            root_.pointer_cancel();
            touch_slot_ = event.slot;
        } else if (touch_slot_ != event.slot) {
            return;
        }
        if (event.kind == phase::up) {
            touch_slot_.reset();
            root_.pointer_button(lx::ui::mouse_button::left, false);
            root_.pointer_leave();
            return;
        }
        const auto display = kms_.display_rect();
        if (!route_position(position(event.x * display.width, event.y * display.height))) {
            root_.pointer_cancel();
        } else if (event.kind == phase::down) {
            root_.pointer_button(lx::ui::mouse_button::left, true);
        }
    }

    void key(const lx::input::key_event& event) {
        if (!seat_active())
            return;
        if (!event.pressed) {
            if (repeat_key_ == event.evdev_keycode)
                stop_repeat();
            if (event.device_removed)
                reset_compose();
            return;
        }
        auto& keymap = input_.default_seat().keymap();
        const unsigned sym = keymap.keysym(event.evdev_keycode);
        const auto mods = keymap.named();
        unsigned vt = 0;
        if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12)
            vt = sym - XKB_KEY_XF86Switch_VT_1 + 1;
        else if (mods.ctrl && mods.alt && event.evdev_keycode >= KEY_F1 &&
                 event.evdev_keycode <= KEY_F10)
            vt = event.evdev_keycode - KEY_F1 + 1;
        else if (mods.ctrl && mods.alt &&
                 (event.evdev_keycode == KEY_F11 || event.evdev_keycode == KEY_F12))
            vt = event.evdev_keycode - KEY_F11 + 11;
        if (vt != 0) {
            if (vt != session_.vt_number()) {
                if (auto result = session_.switch_vt(vt); !result) {
                    log_dm.warn("greeter could not request VT switch: {}",
                                lx::format_error(result.get_error()));
                } else {
                    vt_switch_pending_ = true;
                    clear_focus();
                }
            }
            return;
        }
        input_.update_leds();
        if (!accepts_input())
            return;
        const auto old_revision = form_revision_;
        const bool repeatable = handle_key(event.evdev_keycode, false);
        if (repeatable && keymap.repeats(event.evdev_keycode) && form_revision_ == old_revision) {
            repeat_key_ = event.evdev_keycode;
            repeat_.schedule_at(loop_.now() + k_repeat_delay_ns);
        }
    }

    bool handle_key(unsigned code, bool repeated) {
        auto& keymap = input_.default_seat().keymap();
        const unsigned sym = keymap.keysym(code);
        if (sym == XKB_KEY_BackSpace) {
            reset_compose();
            conversation_.erase();
            return conversation_.editable();
        }
        if (!repeated && sym == XKB_KEY_Escape) {
            reset_compose();
            conversation_.cancel();
        } else if (!repeated && (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)) {
            reset_compose();
            conversation_.submit();
        } else if (conversation_.editable()) {
            const auto mods = keymap.named();
            if (!mods.ctrl && !mods.super)
                return type_key(code, sym);
        }
        return false;
    }

    void stop_repeat() {
        repeat_key_.reset();
        repeat_.set_enabled(false);
    }

    void repeat_key() {
        if (!repeat_key_ || !accepts_input() || !handle_key(*repeat_key_, true))
            stop_repeat();
    }

    void make_compose() {
        const char* locale = nullptr;
        for (const char* name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
            locale = ::getenv(name);
            if (locale && locale[0])
                break;
        }
        const std::unique_ptr<xkb_context, decltype(&xkb_context_unref)> xkb{
            xkb_context_new(XKB_CONTEXT_NO_FLAGS), &xkb_context_unref};
        if (!xkb)
            return;
        const std::unique_ptr<xkb_compose_table, decltype(&xkb_compose_table_unref)> table{
            xkb_compose_table_new_from_locale(xkb.get(), locale && locale[0] ? locale : "C",
                                              XKB_COMPOSE_COMPILE_NO_FLAGS),
            &xkb_compose_table_unref};
        if (table)
            compose_.reset(xkb_compose_state_new(table.get(), XKB_COMPOSE_STATE_NO_FLAGS));
    }

    void reset_compose() {
        if (compose_)
            xkb_compose_state_reset(compose_.get());
    }

    // Typed UTF-8 goes directly from xkbcommon into the conversation, never through lx.ui.
    bool type_key(unsigned code, unsigned keysym) {
        if (compose_ &&
            xkb_compose_state_feed(compose_.get(), keysym) == XKB_COMPOSE_FEED_ACCEPTED) {
            switch (xkb_compose_state_get_status(compose_.get())) {
            case XKB_COMPOSE_COMPOSING:
                return false;
            case XKB_COMPOSE_COMPOSED: {
                char utf8[64]{};
                const int n = xkb_compose_state_get_utf8(compose_.get(), utf8, sizeof(utf8));
                reset_compose();
                const bool got_text = n > 0 && static_cast<std::size_t>(n) < sizeof(utf8);
                if (got_text)
                    conversation_.append({utf8, static_cast<std::size_t>(n)});
                ::explicit_bzero(utf8, sizeof(utf8));
                return got_text;
            }
            case XKB_COMPOSE_CANCELLED:
                reset_compose();
                return false;
            case XKB_COMPOSE_NOTHING:
                break;
            }
        }
        char utf8[8]{};
        const auto n = input_.default_seat().keymap().utf8(code, utf8, sizeof(utf8));
        if (n > 0 && n < sizeof(utf8))
            conversation_.append({utf8, n});
        ::explicit_bzero(utf8, sizeof(utf8));
        return n > 0 && n < sizeof(utf8);
    }

    // Advance for prompt and action changes, not for ordinary typing or hover. The host
    // may paint later in this same dispatch pass, so set the revision before it can do so.
    void invalidate_form_revision() {
        ++form_revision_;
        kms_.request_frame(form_revision_);
    }

    void changed() {
        const auto screen = conversation_.view();
        if (screen.state != last_phase_ || screen.status != last_notice_) {
            invalidate_form_revision();
            // A button can call changed() during pointer dispatch: do not reenter the root.
            cancel_pointer_owed_ = true;
        }
        if (screen.state != last_phase_) {
            reset_compose();
            stop_repeat();
            last_phase_ = screen.state;
        }
        if (screen.status != last_notice_) {
            switch (screen.status) {
            case dm::notice::disconnected:
                log_dm.warn("sign-in service disconnected");
                break;
            case dm::notice::service_error:
                log_dm.warn("sign-in service rejected a request");
                break;
            case dm::notice::bad_peer:
                log_dm.error("greetd socket or peer verification failed");
                break;
            case dm::notice::protocol:
                log_dm.error("invalid greetd protocol response");
                break;
            case dm::notice::uncertain_start:
                log_dm.error("greetd session start not confirmed");
                break;
            default:
                break;
            }
            last_notice_ = screen.status;
        }
        if (const auto exit = screen.exit_code()) {
            stopping_ = true;
            loop_.quit(*exit);
        } else if (!stopping_) {
            root_.set_describer(&describe);
        }
    }

    static lx::ui::child describe(lx::ui::build_context& build) {
        using namespace lx::ui;
        using lx::layout::px;
        const auto screen = active->conversation_.view();
        const auto copy = [&](std::string_view text) -> const char* {
            if (text.empty())
                return "";
            auto* out = build.alloc_chars(static_cast<unsigned>(text.size() + 1));
            if (!out)
                return "";
            std::ranges::copy(text, out);
            out[text.size()] = '\0';
            return out;
        };
        const auto submit = [](int) { active->conversation_.submit(); };
        const auto cancel = [](int) { active->conversation_.cancel(); };
        const auto primary = [&](const char* title) -> child {
            return button(title).tint(tint_color::accent).size(button_size::large).on_press(submit);
        };
        const auto secondary = button("Cancel")
                                   .style(button_style::outlined_ghost)
                                   .size(button_size::large)
                                   .on_press(cancel);
        const bool editable = active->conversation_.editable();
        const bool message = screen.state == dm::phase::info || screen.state == dm::phase::error;
        std::array<child, 6> parts{};
        std::size_t n = 0;
        parts[n++] = label("LUMEN").color(tok::text_accent).size_px(13).bold();
        parts[n++] = label("Sign in").size_px(27).bold();
        if (const char* status = notice_label(screen.status))
            parts[n++] = label(status).color(tok::text_muted);
        if (!screen.prompt.empty())
            parts[n++] = label(copy(screen.prompt))
                             .color(screen.state == dm::phase::error ? tok::error : tok::text);
        else if (const char* hint = phase_label(screen.state))
            parts[n++] = label(hint);

        if (editable) {
            // No credential text reaches lx.ui for a secret prompt, even temporarily.
            // Cap mask width so a long passphrase never pushes the form off-screen.
            char bullets[97]{};
            const std::size_t count = std::min<std::size_t>(32, screen.mask_length);
            for (std::size_t i = 0; i < count; ++i)
                std::memcpy(bullets + 3 * i, "\xe2\x80\xa2", 3);
            const char* value = screen.state == dm::phase::secret ? copy({bullets, 3 * count})
                                                                  : copy(screen.visible_value);
            const child field = label(value).size_px(17).truncate();
            parts[n++] = div(style{}
                                 .w_full()
                                 .h(px(42))
                                 .px(px(12))
                                 .flex_col()
                                 .justify_center()
                                 .bg(tok::background)
                                 .border_1(tok::border_variant)
                                 .rounded(radius::md)
                                 .overflow_hidden(),
                             {&field, 1});
        }
        std::array<child, 2> actions{};
        std::size_t a = 0;
        if (screen.state != dm::phase::username &&
            (editable || message || screen.state == dm::phase::authenticating))
            actions[a++] = secondary;
        if (editable || message || screen.state == dm::phase::offline)
            actions[a++] = primary(screen.state == dm::phase::offline ? "Retry" : "Continue");
        if (a)
            parts[n++] =
                div(style{}.w_full().flex_row().justify_end().gap(px(10)), {actions.data(), a});

        const child form =
            div(style{}.w_full().max_w(px(420)).flex_col().gap(px(12)), {parts.data(), n});
        return div(style{}
                       .size_full()
                       .p(px(24))
                       .bg(tok::surface_background)
                       .flex_col()
                       .items_center()
                       .justify_center(),
                   {&form, 1});
    }

    lx::runtime::event_loop& loop_;
    lx::session::logind_session& session_;
    lx::runtime::memory_arena arena_{64 * 1024};
    lx::gfx::atlas atlas_{};
    lx::text::glyph_cache glyphs_{atlas_};
    lx::scene::draw_list draws_{};
    lx::gfx::ui_cpu_renderer renderer_{};
    lx::ui::ui_root root_{arena_};
    dm::kms_host kms_;
    lx::input::input_manager input_{};
    std::optional<lx::runtime::fd_source> input_source_{};
    lx::runtime::timer_source repeat_{k_repeat_step_ns, [this] { repeat_key(); }};
    dm::conversation conversation_;
    lx::rect2i form_{};
    std::uint64_t form_revision_ = 1;
    std::uint64_t presented_revision_ = 0;
    double pointer_x_ = 0.0;
    double pointer_y_ = 0.0;
    std::optional<unsigned> repeat_key_{};
    std::optional<int> touch_slot_{};
    // logind holds at most 64 devices; overflow fails closed rather than dropping a pause.
    std::inplace_vector<device_event, 64> device_events_{};
    bool engaged_ = false;
    bool greetd_started_ = false;
    bool stopping_ = false;
    bool bus_ready_ = false;
    bool pause_owed_ = false;
    bool resume_owed_ = false;
    bool switch_failed_ = false;
    bool event_overflow_ = false;
    bool cancel_pointer_owed_ = false;
    bool vt_switch_pending_ = false;
    bool display_present_ = false;
    bool keyboard_ready_ = false;
    bool interactive_logged_ = false;
    bool pointer_initialized_ = false;
    dm::phase last_phase_ = dm::phase::offline;
    dm::notice last_notice_ = dm::notice::none;
    std::unique_ptr<xkb_compose_state, decltype(&xkb_compose_state_unref)> compose_{
        nullptr, &xkb_compose_state_unref};
};

int fail(lx::error error) {
    log_dm.error(error);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (::getuid() == 0 || ::geteuid() == 0 || ::getuid() != ::geteuid())
        return fail(lx::make_error(lx::io_err::permission_denied, "greeter must run unprivileged"));
    auto command = argc == 2 ? dm::session_word(argv[1]) : std::nullopt;
    if (!command || !trusted_session(argv[1]))
        return fail(
            lx::invalid_argument("expected one absolute path to the installed lumen-session"));
    // Do not change inherited rlimits: the target session is a separate greetd child.
    // Make this process non-dumpable before any credentials can enter it.
    if (::prctl(PR_SET_DUMPABLE, 0L, 0L, 0L, 0L) < 0)
        return fail(
            lx::make_error(lx::io_err::permission_denied, "cannot disable greeter core dumps"));
    const char* socket = ::getenv("GREETD_SOCK");
    if (!socket || socket[0] != '/')
        return fail(lx::invalid_argument("expected an absolute GREETD_SOCK path"));

    // Finish resource teardown on service stop, including when stopped during startup.
    sigset_t signals{};
    ::sigemptyset(&signals);
    ::sigaddset(&signals, SIGTERM);
    ::sigaddset(&signals, SIGINT);
    if (::sigprocmask(SIG_BLOCK, &signals, nullptr) < 0)
        return fail(lx::make_error(lx::error_domain::io, 0, "cannot block greeter stop signals"));
    const lx::unique_fd signal_fd{::signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC)};
    if (signal_fd.get() < 0)
        return fail(lx::make_error(lx::error_domain::io, 0, "cannot create greeter signal fd"));

    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::ui::set_active_builtin_theme(lx::ui::appearance::dark);
    lx::runtime::event_loop loop{};
    auto session = lx::session::logind_session::open_greeter();
    if (!session)
        return fail(session.get_error());
    greeter view{loop, *session, socket, std::move(*command)};
    lx::runtime::fd_source stop{signal_fd.get(), [&] {
                                    signalfd_siginfo signal{};
                                    if (::read(signal_fd.get(), &signal, sizeof(signal)) ==
                                        static_cast<ssize_t>(sizeof(signal)))
                                        view.stop();
                                }};
    loop.add_source(&stop);
    const auto result = view.open();
    const int status = result ? loop.run() : fail(result.get_error());
    loop.remove_source(&stop);
    return status;
}
