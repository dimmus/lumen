// lx.session: which logind session the compositor takes control of (design-session-and-wm
// S.5) — $XDG_SESSION_ID, then this process's session, then the user's display session,
// then the user's one active seat session; each only when it is this UID's, on a seat, of
// class user.
#include "lumen_test.hpp"

import std;
import lx.session;

namespace {

struct fake_logind {
    const char* pid_session = nullptr;
    const char* display = nullptr;
    std::vector<std::string> sessions;
    struct entry {
        std::string id;
        lx::session::session_facts facts;
    };
    std::vector<entry> known;

    void add(const char* id, unsigned uid, bool seat, bool user_class, bool active,
             bool manager = false) {
        lx::session::session_facts f{};
        f.exists = true;
        f.uid = uid;
        f.has_seat = seat;
        f.user_class = user_class;
        f.manager_class = manager;
        f.active = active;
        known.push_back({id, f});
    }
};

lx::session::session_discovery discovery(fake_logind& f, unsigned uid, const char* env) {
    lx::session::session_discovery d{};
    d.uid = uid;
    d.env_session_id = env;
    d.user = &f;
    d.pid_session = [](void* u, char* out, std::size_t size) {
        const auto* f = static_cast<fake_logind*>(u);
        if (!f->pid_session)
            return false;
        std::snprintf(out, size, "%s", f->pid_session);
        return true;
    };
    d.uid_display = [](void* u, unsigned, char* out, std::size_t size) {
        const auto* f = static_cast<fake_logind*>(u);
        if (!f->display)
            return false;
        std::snprintf(out, size, "%s", f->display);
        return true;
    };
    d.uid_sessions = [](void* u, unsigned, char (*out)[64], unsigned cap) {
        const auto* f = static_cast<fake_logind*>(u);
        unsigned n = 0;
        for (const auto& s : f->sessions)
            if (n < cap)
                std::snprintf(out[n++], 64, "%s", s.c_str());
        return n;
    };
    d.facts = [](void* u, const char* id) {
        const auto* f = static_cast<fake_logind*>(u);
        for (const auto& e : f->known)
            if (e.id == id)
                return e.facts;
        return lx::session::session_facts{};
    };
    return d;
}

std::string_view id_of(const lx::session::session_choice& c) { return c.id; }

} // namespace

LUMEN_TEST(xdg_session_id_wins_when_it_is_a_seat_session_of_this_user) {
    fake_logind f;
    f.add("1", 1000, true, true, true);
    f.add("7", 1000, true, true, false);
    f.pid_session = "7";
    const auto c = lx::session::discover_session(discovery(f, 1000, "1"));
    LUMEN_CHECK(c.found_by == lx::session::session_found_by::environment);
    LUMEN_CHECK(id_of(c) == "1");
}

LUMEN_TEST(a_process_in_a_seatless_session_never_takes_the_console_session) {
    // A compositor (or a test) started over SSH: its own session has no seat, logind's
    // display session is the tty login. It must not take that over.
    fake_logind f;
    f.add("3", 1000, false, true, true); // the SSH session
    f.add("1", 1000, true, true, true);  // the console login
    f.pid_session = "3";
    f.display = "1";
    f.sessions = {"1", "3"};
    const auto c = lx::session::discover_session(discovery(f, 1000, "3"));
    LUMEN_CHECK(!c.found());
}

LUMEN_TEST(a_foreign_or_seatless_xdg_session_id_falls_through_to_the_process) {
    fake_logind f;
    f.add("1", 1001, true, true, true);  // another user's
    f.add("9", 1000, false, true, true); // an SSH login: no seat
    f.add("3", 1000, true, true, true);
    f.pid_session = "3";
    auto c = lx::session::discover_session(discovery(f, 1000, "1"));
    LUMEN_CHECK(c.found_by == lx::session::session_found_by::process && id_of(c) == "3");
    c = lx::session::discover_session(discovery(f, 1000, "9"));
    LUMEN_CHECK(c.found_by == lx::session::session_found_by::process && id_of(c) == "3");
}

LUMEN_TEST(a_user_unit_finds_the_display_session) {
    // In user@.service: no XDG_SESSION_ID, the process is in the manager session ("2",
    // class manager), logind's display session is the tty login.
    fake_logind f;
    f.add("2", 1000, false, false, false, true); // class manager
    f.add("1", 1000, true, true, true);
    f.pid_session = "2";
    f.display = "1";
    const auto c = lx::session::discover_session(discovery(f, 1000, nullptr));
    LUMEN_CHECK(c.found_by == lx::session::session_found_by::display && id_of(c) == "1");
}

LUMEN_TEST(without_a_display_session_the_one_active_seat_session_is_taken) {
    fake_logind f; // a user unit: the process is in no session
    f.add("2", 1000, false, false, false, true);
    f.add("1", 1000, true, true, true);
    f.add("10", 1000, false, true, true); // SSH
    f.sessions = {"2", "1", "10"};
    const auto c = lx::session::discover_session(discovery(f, 1000, ""));
    LUMEN_CHECK(c.found_by == lx::session::session_found_by::active_seat && id_of(c) == "1");
}

LUMEN_TEST(two_active_seat_sessions_are_ambiguous) {
    fake_logind f;
    f.add("1", 1000, true, true, true);
    f.add("5", 1000, true, true, true); // a second seat
    f.sessions = {"1", "5"};
    const auto c = lx::session::discover_session(discovery(f, 1000, nullptr));
    LUMEN_CHECK(!c.found());
}

LUMEN_TEST(greeter_sessions_and_unknown_ids_are_never_taken) {
    fake_logind f;
    f.add("c1", 1000, true, false, true); // greeter class
    f.pid_session = "c1";
    f.display = "nope";
    f.sessions = {"c1"};
    const auto c = lx::session::discover_session(discovery(f, 1000, "missing"));
    LUMEN_CHECK(!c.found());
    LUMEN_CHECK(std::string_view{lx::session::session_found_by_name(c.found_by)} == "none");
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
