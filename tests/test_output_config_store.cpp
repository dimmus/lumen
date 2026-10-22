// The persistent output configuration (ADR 008 §C.5, `lx.compositor:output_config_store` and
// `:toml_lite`): the TOML subset (values, arrays over lines, comments, escapes, every
// unsupported form refused with its line), the model's parse and write round trip,
// capacities and least-recently-used eviction, setup matching in any order, and the file:
// a debounced atomic save, LUMEN_OUTPUT_CONFIG, and a broken file renamed to .bad by the next
// save and never overwritten before.
#include <stdio.h>
#include <stdlib.h>

#include <sys/stat.h>
#include <unistd.h>

import std;
import lx.compositor;

namespace {

int g_failed = 0;
int g_ran = 0;

void check(bool ok, const char* what) {
    ++g_ran;
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

namespace toml = lx::compositor::toml;
using lx::compositor::output_config;

struct collected {
    std::string log;
};

bool on_table(void* u, const char* name, bool array, toml::parse_error&) {
    auto& c = *static_cast<collected*>(u);
    c.log += array ? "[[" : "[";
    c.log += name;
    c.log += array ? "]]" : "]";
    c.log += ";";
    return true;
}

bool on_kv(void* u, const char* key, const toml::value& v, toml::parse_error&) {
    auto& c = *static_cast<collected*>(u);
    c.log += key;
    c.log += "=";
    const auto put = [&](const toml::value& x) {
        char buf[64];
        switch (x.type) {
        case toml::value::kind::string: c.log += "'" + std::string(x.str) + "'"; break;
        case toml::value::kind::integer:
            std::snprintf(buf, sizeof(buf), "%lld", x.integer);
            c.log += buf;
            break;
        case toml::value::kind::floating:
            std::snprintf(buf, sizeof(buf), "%g", x.floating);
            c.log += buf;
            break;
        case toml::value::kind::boolean: c.log += x.boolean ? "true" : "false"; break;
        case toml::value::kind::array: break;
        }
    };
    if (v.type == toml::value::kind::array) {
        c.log += "(";
        for (unsigned i = 0; i < v.count; ++i) {
            if (i)
                c.log += ",";
            put(v.items[i]);
        }
        c.log += ")";
    } else {
        put(v);
    }
    c.log += ";";
    return true;
}

bool read(const char* text, collected& c, toml::parse_error& err) {
    toml::handler h{};
    h.user = &c;
    h.table = &on_table;
    h.key_value = &on_kv;
    return toml::parse(text, std::strlen(text), h, err);
}

bool fails_at(const char* text, unsigned line) {
    collected c{};
    toml::parse_error err{};
    const bool ok = read(text, c, err);
    if (ok || err.line != line)
        std::printf("    (got ok=%d line=%u: %s)\n", ok, err.line, err.message);
    return !ok && err.line == line;
}

std::string slurp(const char* path) {
    std::string s;
    if (FILE* f = std::fopen(path, "rb")) {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            s.append(buf, n);
        std::fclose(f);
    }
    return s;
}

void spit(const char* path, const char* text) {
    if (FILE* f = std::fopen(path, "wb")) {
        std::fputs(text, f);
        std::fclose(f);
    }
}

} // namespace

int main() {
    std::printf("test_output_config_store\n");

    // ── the TOML subset ──
    {
        collected c{};
        toml::parse_error err{};
        const char* text = "# comment\n"
                           "version = 1 # trailing\n"
                           "name = \"a \\\"b\\\" \\u00e9\\t\"\r\n"
                           "lit = 'C:\\path'\n"
                           "neg = -42\nf = 1.5\nexp = 2e3\nbig = 1_000\nhex = 0x1f\nyes = true\n"
                           "\n"
                           "[[setup]]\n"
                           "outputs = [\n  \"x\", # first\n  \"y\",\n]\n"
                           "[ setup . \"output\" ]\n"
                           "pos = [0, -10]\n";
        const bool ok = read(text, c, err);
        check(ok, "a file of every supported form reads");
        check(c.log == "version=1;name='a \"b\" \xc3\xa9\t';lit='C:\\path';neg=-42;f=1.5;exp=2000;"
                       "big=1000;hex=31;yes=true;[[setup]];outputs=('x','y');[setup.output];"
                       "pos=(0,-10);",
              "values, escapes (\\u to UTF-8), CRLF, arrays over lines, dotted headers");
        if (!ok || c.log.empty())
            std::printf("    %s (%u: %s)\n", c.log.c_str(), err.line, err.message);
    }
    check(fails_at("a = 1\nb = {x = 1}\n", 2), "inline tables are refused with their line");
    check(fails_at("a.b = 1\n", 1), "dotted keys are refused");
    check(fails_at("a = \"\"\"x\"\"\"\n", 1), "multi-line strings are refused");
    check(fails_at("a = 1\n\nb = \"open\n", 3), "an unterminated string");
    check(fails_at("a = [1, [2]]\n", 1), "nested arrays");
    check(fails_at("a = 1 b\n", 1), "text after a value");
    check(fails_at("a = 01\n", 1), "leading zeros");
    check(fails_at("a = 1__0\n", 1), "a doubled underscore");
    check(fails_at("[x\n", 1), "an unclosed header");
    check(fails_at("a = \"\\q\"\n", 1), "an unknown escape");
    check(fails_at("a =\n", 1), "a missing value");
    check(fails_at("a = [1,\n2,\n", 3), "an unterminated array (line of the end)");
    {
        toml::writer w{};
        w.key("s", "q\"\\\n\x01");
        w.key("f", 1.25);
        w.key("g", 2.0);
        w.key("h", 156.0 / 120.0);
        collected c{};
        toml::parse_error err{};
        const bool ok = read(w.text().c_str(), c, err);
        check(ok && c.log.find("s='q\"\\\n\x01';") == 0, "the writer escapes what it must");
        check(w.text().find("g = 2.0\n") != std::string::npos && w.text().find("f = 1.25\n") != std::string::npos,
              "floats keep a '.' (TOML) and their shortest form");
        toml::handler h{};
        double back = 0.0;
        h.user = &back;
        h.key_value = [](void* u, const char* k, const toml::value& v, toml::parse_error&) {
            if (!std::strcmp(k, "h"))
                *static_cast<double*>(u) = v.floating;
            return true;
        };
        (void)toml::parse(w.text().c_str(), w.text().size(), h, err);
        check(back == 156.0 / 120.0, "a scale of n/120 reads back exactly");
    }

    // ── the model ──
    auto* cfg = new output_config{};
    {
        const char* text = "version = 1\n"
                           "[[output]]\n"
                           "identifier = \"Dell Inc. DELL U2720Q 7X8S1234\"\n"
                           "mode = \"3840x2160@59997\"\nscale = 1.5\ntransform = \"90\"\n"
                           "adaptive_sync = false\n"
                           "[[output]]\nidentifier = \"connector:eDP-1\"\nmode = \"preferred\"\n"
                           "[[setup]]\n"
                           "outputs = [\"connector:eDP-1\", \"Dell Inc. DELL U2720Q 7X8S1234\"]\n"
                           "  [[setup.output]]\n  identifier = \"Dell Inc. DELL U2720Q 7X8S1234\"\n"
                           "  enabled = true\n  position = [0, 0]\n"
                           "  [[setup.output]]\n  identifier = \"connector:eDP-1\"\n  enabled = false\n"
                           "[unknown]\nwhatever = 3\n";
        toml::parse_error err{};
        const bool ok = lx::compositor::parse_output_config(text, std::strlen(text), *cfg, err);
        check(ok && cfg->monitor_count == 2 && cfg->setup_count == 1,
              "the design's example parses: two monitors, one setup (unknown tables skipped)");
        const auto* dell = lx::compositor::find_monitor(*cfg, "Dell Inc. DELL U2720Q 7X8S1234");
        check(dell && dell->has_mode && !dell->mode_preferred && dell->width == 3840 &&
                  dell->refresh_mhz == 59997 && dell->scale == 1.5 && dell->transform == 1 &&
                  dell->has_adaptive_sync && !dell->adaptive_sync,
              "per monitor: mode, scale, transform, adaptive sync");
        const auto* edp = lx::compositor::find_monitor(*cfg, "connector:eDP-1");
        check(edp && edp->has_mode && edp->mode_preferred && !edp->has_scale,
              "mode = \"preferred\" follows the preferred mode; unset keys stay unset");
        const char* ids[2] = {"Dell Inc. DELL U2720Q 7X8S1234", "connector:eDP-1"};
        const auto* s = lx::compositor::find_setup(*cfg, ids, 2);
        const char* ids_rev[2] = {"connector:eDP-1", "Dell Inc. DELL U2720Q 7X8S1234"};
        check(s && s == lx::compositor::find_setup(*cfg, ids_rev, 2), "setups match in any order");
        check(!lx::compositor::find_setup(*cfg, ids, 1), "a subset is another setup");
        bool edp_off = false, dell_at0 = false;
        for (unsigned i = 0; s && i < s->count; ++i) {
            if (!std::strcmp(s->outputs[i].identifier, "connector:eDP-1"))
                edp_off = !s->outputs[i].enabled && !s->outputs[i].has_position;
            else
                dell_at0 = s->outputs[i].enabled && s->outputs[i].has_position && s->outputs[i].x == 0;
        }
        check(edp_off && dell_at0, "per setup: enabled and position (omitted = automatic)");

        // Round trip.
        const std::string written = lx::compositor::serialize_output_config(*cfg);
        auto* back = new output_config{};
        const bool ok2 = lx::compositor::parse_output_config(written.data(), written.size(), *back, err);
        check(ok2 && lx::compositor::serialize_output_config(*back) == written,
              "write → read → write is stable");
        delete back;
    }
    {
        toml::parse_error err{};
        auto* bad = new output_config{};
        check(!lx::compositor::parse_output_config("[[output]]\nscale = 0\n", 20, *bad, err) &&
                  err.line == 2,
              "scale 0 is refused with its line");
        const char* t1 = "[[output]]\nidentifier = \"a\"\ntransform = \"sideways\"\n";
        check(!lx::compositor::parse_output_config(t1, std::strlen(t1), *bad, err) && err.line == 3,
              "an unknown transform is refused");
        const char* t2 = "[[output]]\nidentifier = \"a\"\nmode = \"big\"\n";
        check(!lx::compositor::parse_output_config(t2, std::strlen(t2), *bad, err) && err.line == 3,
              "a malformed mode is refused");
        const char* t3 = "[[setup]]\noutputs = [\"a\", \"b\"]\n[[setup.output]]\nidentifier = \"a\"\n";
        check(!lx::compositor::parse_output_config(t3, std::strlen(t3), *bad, err),
              "a setup whose `outputs` and entries disagree is refused");
        const char* t4 = "version = 2\n";
        check(!lx::compositor::parse_output_config(t4, std::strlen(t4), *bad, err) && err.line == 1,
              "a newer version is refused (not misread)");
        const char* t5 = "[[setup.output]]\nidentifier = \"a\"\n";
        check(!lx::compositor::parse_output_config(t5, std::strlen(t5), *bad, err),
              "[[setup.output]] outside a [[setup]]");
        delete bad;
    }
    // Recording: most recently used last, capacity, change detection.
    {
        auto* c = new output_config{};
        lx::compositor::monitor_config m{};
        std::snprintf(m.identifier, sizeof(m.identifier), "A");
        m.has_scale = true;
        m.scale = 2.0;
        check(lx::compositor::record_monitor(*c, m) && c->monitor_count == 1, "a new monitor is recorded");
        check(!lx::compositor::record_monitor(*c, m), "recording the same entry again changes nothing");
        for (unsigned i = 0; i < output_config::k_max_monitors + 3; ++i) {
            lx::compositor::monitor_config n{};
            std::snprintf(n.identifier, sizeof(n.identifier), "M%u", i);
            (void)lx::compositor::record_monitor(*c, n);
        }
        check(c->monitor_count == output_config::k_max_monitors && !lx::compositor::find_monitor(*c, "A") &&
                  lx::compositor::find_monitor(*c, "M66"),
              "past 64 monitors the least recently used goes");
        lx::compositor::setup_config s{};
        s.count = 2;
        std::snprintf(s.outputs[0].identifier, sizeof(s.outputs[0].identifier), "B");
        std::snprintf(s.outputs[1].identifier, sizeof(s.outputs[1].identifier), "A");
        s.outputs[1].has_position = true;
        s.outputs[1].x = 1920;
        check(lx::compositor::record_setup(*c, s) && c->setup_count == 1, "a setup is recorded");
        lx::compositor::setup_config s2 = s;
        std::swap(s2.outputs[0], s2.outputs[1]);
        check(!lx::compositor::record_setup(*c, s2) && c->setup_count == 1,
              "the same setup in another order is the same setup");
        s2.outputs[0].x = 0;
        check(lx::compositor::record_setup(*c, s2) && c->setup_count == 1 &&
                  c->setups[0].outputs[0].x == 0,
              "a moved monitor replaces the setup");
        delete c;
    }
    check(lx::compositor::transform_from_name("flipped-270") == 7 &&
              !std::strcmp(lx::compositor::transform_name(5), "flipped-90") &&
              lx::compositor::transform_from_name("rotate") == -1,
          "transform names (sway's and kanshi's spellings)");

    // ── the file ──
    {
        char dir[] = "/tmp/lumen-outcfg-XXXXXX";
        if (!mkdtemp(dir)) {
            check(false, "temporary directory");
        } else {
            const std::string path = std::string(dir) + "/sub/outputs.toml";
            ::setenv("LUMEN_OUTPUT_CONFIG", "0", 1);
            {
                lx::compositor::output_config_file f{};
                check(!f.load() && !f.enabled(), "LUMEN_OUTPUT_CONFIG=0: nothing read or written");
            }
            ::setenv("LUMEN_OUTPUT_CONFIG", path.c_str(), 1);
            {
                lx::compositor::output_config_file f{};
                check(f.load() && f.enabled() && f.config().monitor_count == 0,
                      "a missing file is an empty configuration");
                lx::compositor::monitor_config m{};
                std::snprintf(m.identifier, sizeof(m.identifier), "X Y Z");
                m.has_mode = true;
                m.mode_preferred = true;
                (void)lx::compositor::record_monitor(f.config(), m);
                f.schedule_save(1'000);
                check(!f.run_due_save(1'000 + 1'000'000'000ull, nullptr), "not before 2 s");
                f.schedule_save(500'000'000ull); // a burst moves it
                check(!f.run_due_save(2'000'000'000ull, nullptr), "a later request moves the save");
                check(f.run_due_save(2'600'000'000ull, nullptr) && f.saves() == 1,
                      "then it is written (the directory made on demand)");
                check(slurp(path.c_str()).find("identifier = \"X Y Z\"\nmode = \"preferred\"") !=
                          std::string::npos,
                      "the file holds the monitor");
                struct stat st {};
                check(::stat((path + ".tmp").c_str(), &st) != 0, "no temporary file is left");
            }
            spit(path.c_str(), "[[output]]\nscale = \"big\"\n");
            {
                lx::compositor::output_config_file f{};
                check(f.load() && f.bad_file() && f.config().monitor_count == 0,
                      "a broken file is ignored (logged with its line)");
                check(slurp(path.c_str()) == "[[output]]\nscale = \"big\"\n",
                      "and not overwritten by loading");
                lx::compositor::monitor_config m{};
                std::snprintf(m.identifier, sizeof(m.identifier), "new");
                (void)lx::compositor::record_monitor(f.config(), m);
                f.schedule_save(0);
                (void)f.run_due_save(3'000'000'000ull, nullptr);
                check(slurp((path + ".bad").c_str()) == "[[output]]\nscale = \"big\"\n" &&
                          slurp(path.c_str()).find("\"new\"") != std::string::npos,
                      "the next save renames it to outputs.toml.bad first");
            }
            {
                lx::compositor::output_config_file f{};
                (void)f.load();
                lx::compositor::monitor_config m{};
                std::snprintf(m.identifier, sizeof(m.identifier), "flushed");
                (void)lx::compositor::record_monitor(f.config(), m);
                f.schedule_save(0);
            } // destroyed with a save pending: flushed
            check(slurp(path.c_str()).find("\"flushed\"") != std::string::npos,
                  "a pending save is written at exit");
            std::string cmd = std::string("rm -rf ") + dir;
            (void)std::system(cmd.c_str());
            ::unsetenv("LUMEN_OUTPUT_CONFIG");
        }
    }
    delete cfg;

    std::printf("%d cases, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}
