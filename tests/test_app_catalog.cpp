// lumen-shell's application catalog: XDG directory precedence and desktop-ID shadowing,
// inotify staleness, search ranking and terminal selection — against temporary directories.
#include "lumen_test.hpp"

#include <stdio.h>
#include <stdlib.h>

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.shell;

namespace {

std::string g_root;

void write_file(const std::string& path, std::string_view text, mode_t mode = 0644) {
    FILE* f = std::fopen(path.c_str(), "w");
    LUMEN_CHECK(f != nullptr);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    chmod(path.c_str(), mode);
}

void mkdirs(const std::string& p) {
    std::string cur;
    for (std::size_t i = 1; i <= p.size(); ++i)
        if (i == p.size() || p[i] == '/')
            (void)mkdir(p.substr(0, i).c_str(), 0755);
}

std::string app(std::string_view name, std::string_view exec, std::string_view extra = {}) {
    std::string s = "[Desktop Entry]\nType=Application\nName=";
    s.append(name).append("\nExec=").append(exec).append("\n").append(extra);
    return s;
}

/// home/applications, sys1/applications, sys2/applications; a bin dir with programs.
lx::shell::catalog_config make_tree() {
    char tmpl[] = "/tmp/lumen-catalog-XXXXXX";
    LUMEN_CHECK(mkdtemp(tmpl) != nullptr);
    g_root = tmpl;
    for (const char* d : {"/home/applications", "/sys1/applications/kde", "/sys2/applications",
                          "/bin"})
        mkdirs(g_root + d);
    for (const char* p : {"foot", "alacritty", "firefox", "gimp", "editor", "term-x"})
        write_file(g_root + "/bin/" + p, "#!/bin/sh\n", 0755);
    lx::shell::catalog_config c{};
    c.data_home = g_root + "/home";
    c.data_dirs = {g_root + "/sys1", g_root + "/sys2"};
    c.path_env = g_root + "/bin";
    c.locale = lx::shell::locale_keys::from(nullptr, nullptr, "C");
    c.current = lx::shell::desktop_list::from("Lumen");
    return c;
}

void remove_tree() {
    if (g_root.size() > 5) {
        const std::string cmd = "rm -rf '" + g_root + "'";
        (void)std::system(cmd.c_str());
    }
}

std::vector<std::string> names(const lx::shell::app_catalog& c, std::string_view q) {
    unsigned idx[64];
    const unsigned n = c.rank(q, idx, 64);
    std::vector<std::string> out;
    for (unsigned i = 0; i < n; ++i)
        out.emplace_back(c.entry(idx[i])->name);
    return out;
}

bool drain_until_stale(lx::shell::app_catalog& c) {
    for (int i = 0; i < 50; ++i) {
        pollfd p{c.watch_fd(), POLLIN, 0};
        if (poll(&p, 1, 20) > 0)
            (void)c.on_watch_readable();
        if (c.stale())
            return true;
    }
    return false;
}

} // namespace

LUMEN_TEST(precedence_shadowing_and_subdirectory_ids) {
    auto cfg = make_tree();
    write_file(g_root + "/sys1/applications/firefox.desktop", app("Firefox", "firefox"));
    write_file(g_root + "/sys2/applications/firefox.desktop", app("Firefox Old", "firefox"));
    // The user directory deletes gimp from the system with Hidden=true.
    write_file(g_root + "/sys1/applications/gimp.desktop", app("GIMP", "gimp"));
    write_file(g_root + "/home/applications/gimp.desktop", app("GIMP", "gimp", "Hidden=true\n"));
    write_file(g_root + "/sys1/applications/kde/editor.desktop", app("Kate-like Editor", "editor"));
    write_file(g_root + "/sys1/applications/notes.txt", "not a desktop file");
    write_file(g_root + "/sys2/applications/nodisplay.desktop",
               app("Hidden Helper", "editor", "NoDisplay=true\n"));
    lx::shell::app_catalog c;
    c.configure(cfg);
    c.scan();
    LUMEN_CHECK(c.generation() == 1 && !c.stale());
    const auto* ff = c.find("firefox.desktop");
    LUMEN_CHECK(ff && ff->name == "Firefox"); // sys1 before sys2
    const auto* gimp = c.find("gimp.desktop");
    LUMEN_CHECK(gimp && gimp->skip == lx::shell::entry_skip::hidden);
    LUMEN_CHECK(c.find("kde-editor.desktop") != nullptr);
    const auto* nd = c.find("nodisplay.desktop");
    LUMEN_CHECK(nd && nd->skip == lx::shell::entry_skip::no_display); // kept, not listed
    LUMEN_CHECK(c.listed().size() == 2);
    LUMEN_CHECK((names(c, "") == std::vector<std::string>{"Firefox", "Kate-like Editor"}));
    LUMEN_CHECK(c.stats().files == 4); // shadowed files are not even read
    remove_tree();
}

LUMEN_TEST(inotify_marks_stale_and_a_rescan_picks_changes_up) {
    auto cfg = make_tree();
    (void)rmdir((g_root + "/home/applications").c_str()); // created later
    write_file(g_root + "/sys1/applications/foot.desktop", app("Foot", "foot"));
    lx::shell::app_catalog c;
    c.configure(cfg);
    c.scan();
    LUMEN_CHECK(c.watch_fd() >= 0 && !c.stale());
    // A new file.
    write_file(g_root + "/sys1/applications/firefox.desktop", app("Firefox", "firefox"));
    LUMEN_CHECK(drain_until_stale(c));
    c.scan();
    LUMEN_CHECK(c.listed().size() == 2 && !c.stale());
    // A rename.
    LUMEN_CHECK(rename((g_root + "/sys1/applications/firefox.desktop").c_str(),
                       (g_root + "/sys1/applications/ff.desktop").c_str()) == 0);
    LUMEN_CHECK(drain_until_stale(c));
    c.scan();
    LUMEN_CHECK(c.find("ff.desktop") && !c.find("firefox.desktop"));
    // A deletion.
    unlink((g_root + "/sys1/applications/ff.desktop").c_str());
    LUMEN_CHECK(drain_until_stale(c));
    c.scan();
    LUMEN_CHECK(c.listed().size() == 1);
    // ~/.local/share/applications appearing later.
    mkdirs(g_root + "/home/applications");
    LUMEN_CHECK(drain_until_stale(c));
    c.scan();
    write_file(g_root + "/home/applications/mine.desktop", app("Mine", "editor"));
    LUMEN_CHECK(drain_until_stale(c));
    c.scan();
    LUMEN_CHECK(c.find("mine.desktop") && c.listed().size() == 2);
    // Something unrelated in a data directory does not.
    mkdirs(g_root + "/sys2/icons");
    for (int i = 0; i < 5; ++i) {
        pollfd p{c.watch_fd(), POLLIN, 0};
        if (poll(&p, 1, 20) > 0)
            (void)c.on_watch_readable();
    }
    LUMEN_CHECK(!c.stale());
    remove_tree();
}

LUMEN_TEST(search_ranks_name_prefix_word_substring_keywords_exec) {
    auto cfg = make_tree();
    const char* dir = "/sys1/applications/";
    write_file(g_root + dir + "a.desktop", app("Terminal Tools", "editor"));       // prefix
    write_file(g_root + dir + "b.desktop", app("Super Terminal", "editor"));       // word
    write_file(g_root + dir + "c.desktop", app("XTerminator", "editor"));          // substring
    write_file(g_root + dir + "d.desktop", app("Foot", "foot", "GenericName=Terminal\n"));
    write_file(g_root + dir + "e.desktop", app("Console", "term-x"));              // exec name
    write_file(g_root + dir + "f.desktop", app("Kitchen", "editor", "Keywords=shell;term;\n"));
    write_file(g_root + dir + "g.desktop", app("Another Terminal", "editor"));     // word too
    lx::shell::app_catalog c;
    c.configure(cfg);
    c.scan();
    LUMEN_CHECK((names(c, "TERM") ==
                 std::vector<std::string>{"Terminal Tools", "Another Terminal", "Super Terminal",
                                          "XTerminator", "Foot", "Kitchen", "Console"}));
    LUMEN_CHECK(names(c, "zzz").empty());
    LUMEN_CHECK(names(c, "").size() == 7);
    unsigned idx[2];
    LUMEN_CHECK(c.rank("term", idx, 2) == 2); // capped
    remove_tree();
}

LUMEN_TEST(terminal_selection) {
    auto cfg = make_tree();
    const char* dir = "/sys1/applications/";
    // foot-server and footclient never qualify; the preference order picks foot over
    // Alacritty; NoDisplay terminals count.
    write_file(g_root + dir + "foot-server.desktop",
               app("Foot Server", "foot --server", "Categories=System;TerminalEmulator;\n"));
    write_file(g_root + dir + "footclient.desktop",
               app("Foot Client", "foot", "Categories=System;TerminalEmulator;\n"));
    write_file(g_root + dir + "Alacritty.desktop",
               app("Alacritty", "alacritty", "Categories=System;TerminalEmulator;\n"));
    lx::shell::app_catalog c;
    c.configure(cfg);
    c.scan();
    LUMEN_CHECK(c.terminal().found && std::strcmp(c.terminal().source, "Alacritty.desktop") == 0);
    LUMEN_CHECK(std::strcmp(c.terminal().exec_flag, "-e") == 0);
    write_file(g_root + dir + "foot.desktop",
               app("Foot", "foot", "NoDisplay=true\nCategories=System;TerminalEmulator;\n"));
    c.scan();
    LUMEN_CHECK(std::strcmp(c.terminal().source, "foot.desktop") == 0);
    // X-TerminalArgExec.
    unlink((g_root + dir + "foot.desktop").c_str());
    unlink((g_root + dir + "Alacritty.desktop").c_str());
    write_file(g_root + dir + "zz-term.desktop",
               app("ZZ Term", "term-x", "Categories=TerminalEmulator;\nX-TerminalArgExec=--\n"));
    c.scan();
    LUMEN_CHECK(std::strcmp(c.terminal().source, "zz-term.desktop") == 0);
    LUMEN_CHECK(std::strcmp(c.terminal().exec_flag, "--") == 0);
    // $TERMINAL wins when it resolves.
    cfg.terminal_env = "term-x";
    c.configure(cfg);
    c.scan();
    LUMEN_CHECK(std::strcmp(c.terminal().source, "$TERMINAL") == 0 && c.terminal().entry == -1);
    LUMEN_CHECK(std::string_view{c.terminal().program}.ends_with("/bin/term-x"));
    cfg.terminal_env = "not-installed-terminal";
    c.configure(cfg);
    c.scan();
    LUMEN_CHECK(std::strcmp(c.terminal().source, "zz-term.desktop") == 0);
    remove_tree();
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
