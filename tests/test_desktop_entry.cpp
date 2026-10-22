// lumen-shell's desktop-entry parser and Exec expansion (Desktop Entry Specification 1.5).
#include "lumen_test.hpp"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/stat.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.shell;

namespace {

using lx::shell::desktop_entry;
using lx::shell::entry_skip;

lx::shell::string_arena g_arena;

struct env {
    lx::shell::locale_keys locale = lx::shell::locale_keys::from(nullptr, nullptr, "de_DE.UTF-8@euro");
    lx::shell::desktop_list current = lx::shell::desktop_list::from("GNOME:Lumen");
    std::string path = "/usr/bin:/bin";
};

desktop_entry parse(std::string_view text, const env& e = {}, bool try_exec = true) {
    lx::shell::parse_context ctx{&e.locale, &e.current, e.path, try_exec};
    desktop_entry out{};
    lx::shell::parse_desktop_entry(text, "test.desktop", "/x/applications/test.desktop", ctx,
                                   g_arena, out);
    return out;
}

std::string argv_of(const desktop_entry& e) {
    lx::shell::exec_argv a;
    if (!lx::shell::expand_exec(e, a))
        return "<invalid>";
    std::string s;
    for (unsigned i = 0; i < a.argc; ++i) {
        if (i)
            s += '|';
        s += a.argv[i];
    }
    return s;
}

desktop_entry with_exec(std::string_view exec, std::string_view icon = {}) {
    desktop_entry e{};
    e.exec = exec;
    e.icon = icon;
    e.name = "My App";
    e.file = "/x/my.desktop";
    return e;
}

} // namespace

LUMEN_TEST(minimal_entry_is_listed_and_keeps_its_keys) {
    const auto e = parse("[Desktop Entry]\nType=Application\nName=Foot\nExec=foot\n"
                         "GenericName=Terminal\nCategories=System;TerminalEmulator;\n");
    LUMEN_CHECK(e.listed());
    LUMEN_CHECK(e.name == "Foot" && e.generic_name == "Terminal" && e.exec == "foot");
    LUMEN_CHECK(e.name_fold == "foot" && e.exec_name == "foot");
    LUMEN_CHECK(lx::shell::list_contains(e.categories, "TerminalEmulator"));
    LUMEN_CHECK(!lx::shell::list_contains(e.categories, "Terminal"));
    LUMEN_CHECK(e.id == "test.desktop");
}

LUMEN_TEST(comments_blank_lines_crlf_and_later_groups) {
    const auto e = parse("# leading comment\r\n\r\n[Desktop Entry]\r\n# inside\r\n"
                         "Type = Application\r\nName= Spaced \r\nExec=app\r\n"
                         "[Desktop Action New]\r\nName=Not this\r\nExec=other\r\n");
    LUMEN_CHECK(e.listed());
    LUMEN_CHECK(e.name == "Spaced"); // whitespace around '=' trimmed; the action ignored
    LUMEN_CHECK(e.exec == "app");
}

LUMEN_TEST(invalid_files_are_skipped) {
    LUMEN_CHECK(parse("Name=x\n[Desktop Entry]\nType=Application\nName=y\nExec=y\n").skip ==
                entry_skip::invalid_file); // key before a group
    LUMEN_CHECK(parse("[Other]\nA=b\n[Desktop Entry]\nType=Application\nName=y\nExec=y\n").skip ==
                entry_skip::invalid_file); // not the first group
    LUMEN_CHECK(parse("[Desktop Entry]\nType=Application\nExec=y\n").skip ==
                entry_skip::invalid_file); // no Name
    LUMEN_CHECK(parse("[Desktop Entry]\nType=Application\nName=\xff\xfe\nExec=y\n").skip ==
                entry_skip::invalid_file); // not UTF-8
    LUMEN_CHECK(parse("[Desktop Entry]\nType=Application\nName=\xc0\xaf\nExec=y\n").skip ==
                entry_skip::invalid_file); // overlong
    LUMEN_CHECK(parse("[Desktop Entry]\nType=Link\nName=y\nURL=x\n").skip ==
                entry_skip::not_application);
    LUMEN_CHECK(parse("[Desktop Entry]\nType=Application\nName=y\n").skip == entry_skip::no_exec);
    LUMEN_CHECK(parse("[Desktop Entry]\nType=Application\nName=y\nDBusActivatable=true\n").skip ==
                entry_skip::no_exec);
}

LUMEN_TEST(duplicate_keys_keep_the_first) {
    const auto e = parse("[Desktop Entry]\nType=Application\nName=First\nName=Second\nExec=a\n");
    LUMEN_CHECK(e.name == "First");
}

LUMEN_TEST(locale_ranking_prefers_the_most_specific_match) {
    const auto k = lx::shell::locale_keys::from(nullptr, nullptr, "de_DE.UTF-8@euro");
    LUMEN_CHECK(k.count == 4);
    LUMEN_CHECK(std::strcmp(k.key[0], "de_DE@euro") == 0 && std::strcmp(k.key[1], "de_DE") == 0 &&
                std::strcmp(k.key[2], "de@euro") == 0 && std::strcmp(k.key[3], "de") == 0);
    // LC_ALL wins over LC_MESSAGES over LANG; C means no localisation.
    LUMEN_CHECK(lx::shell::locale_keys::from("fr_FR", "de", "en").rank("fr_FR") == 0);
    LUMEN_CHECK(lx::shell::locale_keys::from("", "de", "en").rank("de") == 0);
    LUMEN_CHECK(lx::shell::locale_keys::from(nullptr, nullptr, "C.UTF-8").count == 0);
    const char* all = "[Desktop Entry]\nType=Application\nExec=a\nName=Default\nName[de]=De\n"
                      "Name[de@euro]=DeEuro\nName[de_DE]=DeDE\nName[fr]=Fr\n";
    // The order in the file does not matter: the best rank wins.
    LUMEN_CHECK(parse(all).name == "DeDE");
    LUMEN_CHECK(parse("[Desktop Entry]\nType=Application\nExec=a\nName[de_DE@euro]=Best\n"
                      "Name[de_DE]=DeDE\nName=Default\n")
                    .name == "Best");
    LUMEN_CHECK(parse("[Desktop Entry]\nType=Application\nExec=a\nName[de]=De\nName=Default\n"
                      "Name[de@euro]=DeEuro\n")
                    .name == "DeEuro");
    LUMEN_CHECK(parse("[Desktop Entry]\nType=Application\nExec=a\nName[fr]=Fr\nName=Default\n")
                    .name == "Default");
    // Exec is not a localestring: a localised Exec is ignored.
    LUMEN_CHECK(parse("[Desktop Entry]\nType=Application\nName=n\nExec=a\nExec[de]=b\n").exec ==
                "a");
}

LUMEN_TEST(escapes_in_strings_and_lists) {
    const auto e = parse("[Desktop Entry]\nType=Application\nExec=a\nName=a\\sb\\tc\\\\d\\qe\n"
                         "Comment=line\\nbreak\nKeywords=one\\;two;three;\n");
    LUMEN_CHECK(e.name == "a b\tc\\d\\qe"); // unknown \q keeps both characters
    LUMEN_CHECK(e.comment == "line\nbreak");
    LUMEN_CHECK(lx::shell::list_contains(e.keywords, "one;two"));
    LUMEN_CHECK(lx::shell::list_contains(e.keywords, "three"));
    LUMEN_CHECK(!lx::shell::list_contains(e.keywords, "one"));
}

LUMEN_TEST(booleans_hidden_and_nodisplay) {
    auto e = parse("[Desktop Entry]\nType=Application\nName=n\nExec=a\nTerminal=true\n");
    LUMEN_CHECK(e.listed() && e.terminal);
    e = parse("[Desktop Entry]\nType=Application\nName=n\nExec=a\nTerminal=yes\n");
    LUMEN_CHECK(!e.terminal); // only "true" is true
    e = parse("[Desktop Entry]\nType=Application\nName=n\nExec=a\nHidden=true\n");
    LUMEN_CHECK(e.skip == entry_skip::hidden && e.hidden);
    e = parse("[Desktop Entry]\nType=Application\nName=n\nExec=a\nNoDisplay=true\n");
    LUMEN_CHECK(e.skip == entry_skip::no_display && e.no_display);
    e = parse("[Desktop Entry]\nType=Application\nName=n\nExec=a\nNoDisplay=false\n");
    LUMEN_CHECK(e.listed());
}

LUMEN_TEST(only_show_in_and_not_show_in_against_several_desktops) {
    const env two; // GNOME:Lumen
    auto e = parse("[Desktop Entry]\nType=Application\nName=n\nExec=a\nOnlyShowIn=KDE;Lumen;\n", two);
    LUMEN_CHECK(e.listed());
    e = parse("[Desktop Entry]\nType=Application\nName=n\nExec=a\nOnlyShowIn=KDE;XFCE;\n", two);
    LUMEN_CHECK(e.skip == entry_skip::not_shown_here);
    e = parse("[Desktop Entry]\nType=Application\nName=n\nExec=a\nNotShowIn=GNOME;\n", two);
    LUMEN_CHECK(e.skip == entry_skip::not_shown_here);
    e = parse("[Desktop Entry]\nType=Application\nName=n\nExec=a\nNotShowIn=KDE;\n", two);
    LUMEN_CHECK(e.listed());
    // Case-sensitive; unset means Lumen.
    env lower;
    lower.current = lx::shell::desktop_list::from("lumen");
    e = parse("[Desktop Entry]\nType=Application\nName=n\nExec=a\nOnlyShowIn=Lumen;\n", lower);
    LUMEN_CHECK(e.skip == entry_skip::not_shown_here);
    env unset;
    unset.current = lx::shell::desktop_list::from(nullptr);
    e = parse("[Desktop Entry]\nType=Application\nName=n\nExec=a\nOnlyShowIn=Lumen;\n", unset);
    LUMEN_CHECK(e.listed());
}

LUMEN_TEST(try_exec_absent_or_present) {
    char dir[] = "/tmp/lumen-tryexec-XXXXXX";
    LUMEN_CHECK(mkdtemp(dir) != nullptr);
    const std::string prog = std::string(dir) + "/lumen-present";
    FILE* f = std::fopen(prog.c_str(), "w");
    LUMEN_CHECK(f != nullptr);
    std::fputs("#!/bin/sh\n", f);
    std::fclose(f);
    chmod(prog.c_str(), 0755);
    env e;
    e.path = std::string("relative:") + dir; // the relative entry is ignored
    auto d = parse("[Desktop Entry]\nType=Application\nName=n\nExec=a\nTryExec=lumen-present\n", e);
    LUMEN_CHECK(d.listed());
    d = parse("[Desktop Entry]\nType=Application\nName=n\nExec=a\nTryExec=lumen-absent\n", e);
    LUMEN_CHECK(d.skip == entry_skip::try_exec_missing);
    d = parse(("[Desktop Entry]\nType=Application\nName=n\nExec=a\nTryExec=" + prog + "\n").c_str(),
              e);
    LUMEN_CHECK(d.listed());
    // Not executable: missing.
    chmod(prog.c_str(), 0644);
    d = parse("[Desktop Entry]\nType=Application\nName=n\nExec=a\nTryExec=lumen-present\n", e);
    LUMEN_CHECK(d.skip == entry_skip::try_exec_missing);
    unlink(prog.c_str());
    rmdir(dir);
}

LUMEN_TEST(exec_quoting) {
    LUMEN_CHECK(argv_of(with_exec("app  one\ttwo")) == "app|one|two");
    LUMEN_CHECK(argv_of(with_exec("app \"a \\\"b\\\"\"")) == "app|a \"b\"");
    LUMEN_CHECK(argv_of(with_exec("app \"\\` \\$ \\\\\"")) == "app|` $ \\");
    LUMEN_CHECK(argv_of(with_exec("app \"\\x\"")) == "app|\\x"); // other backslashes literal
    LUMEN_CHECK(argv_of(with_exec("sh -c 'echo $HOME \"x\"'")) == "sh|-c|echo $HOME \"x\"");
    LUMEN_CHECK(argv_of(with_exec("app pre\"mid dle\"post")) == "app|premid dlepost");
    LUMEN_CHECK(argv_of(with_exec("app \"\"")) == "app|"); // an explicit empty argument
    LUMEN_CHECK(argv_of(with_exec("app \"open")) == "<invalid>");
    LUMEN_CHECK(argv_of(with_exec("app 'open")) == "<invalid>");
    LUMEN_CHECK(argv_of(with_exec("")) == "<invalid>");
    LUMEN_CHECK(argv_of(with_exec("bin/app")) == "<invalid>"); // relative path with '/'
    LUMEN_CHECK(argv_of(with_exec("/usr/bin/app")) == "/usr/bin/app");
}

LUMEN_TEST(exec_double_unescaping_from_the_file) {
    // In the file, "\\\\" is a string-level "\\", which the quoting level reads as "\".
    const auto e = parse("[Desktop Entry]\nType=Application\nName=n\n"
                         "Exec=app \"a\\\\\\\\b\" \"\\\\$x\"\n");
    LUMEN_CHECK(e.listed());
    LUMEN_CHECK(argv_of(e) == "app|a\\b|$x");
}

LUMEN_TEST(exec_field_codes) {
    LUMEN_CHECK(argv_of(with_exec("app %f")) == "app");
    LUMEN_CHECK(argv_of(with_exec("app %F %u %U")) == "app");
    LUMEN_CHECK(argv_of(with_exec("app --url=%u --keep")) == "app|--keep");
    LUMEN_CHECK(argv_of(with_exec("app %i", "icon-name")) == "app|--icon|icon-name");
    LUMEN_CHECK(argv_of(with_exec("app %i")) == "app");
    LUMEN_CHECK(argv_of(with_exec("app %c")) == "app|My App");
    LUMEN_CHECK(argv_of(with_exec("app --name=%c")) == "app|--name=My App");
    LUMEN_CHECK(argv_of(with_exec("app %k")) == "app|/x/my.desktop");
    LUMEN_CHECK(argv_of(with_exec("app 100%%")) == "app|100%");
    LUMEN_CHECK(argv_of(with_exec("app %d %D %n %N %v %m x")) == "app|x");
    LUMEN_CHECK(argv_of(with_exec("app %z")) == "app"); // unknown: removed
    LUMEN_CHECK(argv_of(with_exec("app \"%f\" \"%%\"")) == "app|%f|%"); // quoted: only %%
    // An entry whose Exec does not expand is not listed.
    LUMEN_CHECK(parse("[Desktop Entry]\nType=Application\nName=n\nExec=app \"x\n").skip ==
                entry_skip::invalid_exec);
}

LUMEN_TEST(resolve_program_searches_path_and_takes_absolute_paths) {
    char out[PATH_MAX];
    LUMEN_CHECK(lx::shell::resolve_program("sh", "/nonexistent::/usr/bin:/bin", out));
    LUMEN_CHECK(out[0] == '/' && std::string_view{out}.ends_with("/sh"));
    LUMEN_CHECK(lx::shell::resolve_program("/bin/sh", "", out));
    LUMEN_CHECK(!lx::shell::resolve_program("lumen-surely-not-installed", "/usr/bin:/bin", out));
    LUMEN_CHECK(!lx::shell::resolve_program("bin/sh", "/", out));
    LUMEN_CHECK(!lx::shell::resolve_program("/usr/bin", "", out)); // a directory
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
