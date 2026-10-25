#!/usr/bin/env python3
"""Transcribe Zed's theme defaults into lx.ui's theme compiler (ADR-010 §4.8).

Zed refines every theme it loads off a base theme of the same appearance: a key the theme
file leaves out (or sets to null) takes a value derived from another key where Zed has such a
rule (crates/theme_settings/src/schema.rs), and otherwise the base value —
`ThemeColors::dark()` / `light()` (crates/theme/src/default_colors.rs), `StatusColors` and
`PlayerColors` (crates/theme/src/styles/). Those bases are written in terms of Radix colour
scales. This script evaluates them to hex for every colour key of Zed's theme schema
(crates/settings_content/src/theme.rs; the compiler looks up the keys it keeps) and prints
the C++ partition modules/lx.ui/theme_defaults.cppm.

Licence: those four files belong to crates licensed GPL-3.0-or-later (only
assets/themes/one/one.json, vendored beside, is MIT). The script reads them and emits facts —
colour values and the schema's key names — not their code; assets/themes/README.md records
the project's position on that.

Usage (a checkout or the raw files of github.com/zed-industries/zed):
    scripts/zed-theme-defaults.py <zed-source-dir> ["<origin note>"] \
        > modules/lx.ui/theme_defaults.cppm

<zed-source-dir> holds the four files under their repository paths, or flat:
default_colors.rs, styles/status.rs (status.rs), styles/players.rs (players.rs),
settings_content/src/theme.rs (sc_theme.rs).
"""

import colorsys
import os
import re
import sys


def find(root, *names):
    for n in names:
        p = os.path.join(root, n)
        if os.path.exists(p):
            return open(p, encoding="utf-8").read()
    sys.exit(f"missing {names[0]} under {root}")


def parse_hex(s):
    s = s.lstrip("#")
    if len(s) == 6:
        s += "ff"
    return [int(s[i:i + 2], 16) / 255.0 for i in (0, 2, 4, 6)]


def hsla_to_rgba(h, s, l, a):
    r, g, b = colorsys.hls_to_rgb(h, l, s)
    return [r, g, b, a]


def to_hex(c):
    return "#" + "".join(f"{max(0, min(255, int(round(v * 255)))):02x}" for v in c)


def scales(src):
    out = {}
    for m in re.finditer(r'pub\(crate\) fn (\w+)\(\) -> ColorScaleSet \{\s*StaticColorScaleSet \{(.*?)\n    \}', src, re.S):
        name, body = m.group(1), m.group(2)
        sets = {}
        for k in ("light_alpha", "light", "dark_alpha", "dark"):
            mm = re.search(r'\b' + k + r': \[(.*?)\]', body, re.S)
            sets[k] = [parse_hex(x) for x in re.findall(r'"(#[0-9a-fA-F]+)"', mm.group(1))]
        out[name] = sets
    out["neutral"] = out["sand"]
    return out


def consts(src):
    out = {}
    for m in re.finditer(r'const (\w+): Hsla = Hsla \{\s*h: ([\d.]+) / 360\.,\s*s: ([\d.]+),\s*l: ([\d.]+),\s*a: ([\d.]+),', src):
        out[m.group(1)] = hsla_to_rgba(float(m.group(2)) / 360, float(m.group(3)), float(m.group(4)), float(m.group(5)))
    return out


def evaluate(expr, sc, cs):
    expr = expr.strip().rstrip(",")
    if expr in ("system.transparent", "gpui::transparent_black()"):
        return [0.0, 0.0, 0.0, 0.0]
    if expr == "None":
        return None
    m = re.match(r'(\w+)\(\)\.(light_alpha|dark_alpha|light|dark)\(\)\.step_(\d+)\(\)(.*)$', expr)
    if m:
        c = list(sc[m.group(1)][m.group(2)][int(m.group(3)) - 1])
        rest = m.group(4)
    else:
        m = re.match(r'([A-Z_]+)(.*)$', expr)
        if not m or m.group(1) not in cs:
            raise ValueError(expr)
        c = list(cs[m.group(1)])
        rest = m.group(2)
    for op, v in re.findall(r'\.(alpha|opacity)\(([\d.]+)\)', rest):
        c[3] = float(v) if op == "alpha" else c[3] * float(v)
    return c


def body(src, start):
    i = src.index(start)
    j = src.index("Self", i)
    depth, k = 0, src.index("(", j) if src[j:j + 6] == "Self(v" else src.index("{", j)
    open_c = src[k]
    close_c = ")" if open_c == "(" else "}"
    for n in range(k, len(src)):
        if src[n] == open_c:
            depth += 1
        elif src[n] == close_c:
            depth -= 1
            if depth == 0:
                return src[k + 1:n]
    raise ValueError(start)


def fields(text, sc, cs):
    out = {}
    for m in re.finditer(r'^\s*(\w+): (.+?),\s*$', text, re.M):
        try:
            v = evaluate(m.group(2), sc, cs)
        except (ValueError, KeyError):
            continue
        out[m.group(1)] = v
    return out


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    colors_src = find(root, "crates/theme/src/default_colors.rs", "default_colors.rs")
    status_src = find(root, "crates/theme/src/styles/status.rs", "status.rs", "styles_status.rs")
    players_src = find(root, "crates/theme/src/styles/players.rs", "players.rs", "styles_players.rs")
    schema_src = find(root, "crates/settings_content/src/theme.rs", "sc_theme.rs")

    sc = scales(colors_src)
    cs = consts(colors_src)
    base = {"dark": {}, "light": {}}
    for app in ("dark", "light"):
        b = body(colors_src, f"pub fn {app}() -> Self")
        base[app].update(fields(b, sc, cs))
        b = body(status_src, f"pub fn {app}() -> Self")
        base[app].update(fields(b, sc, cs))
    players = {}
    for app in ("dark", "light"):
        b = players_src[players_src.index(f"pub fn {app}() -> Self"):]
        b = b[:b.index("])")]
        ps = []
        for m in re.finditer(r'cursor: (.+?),\s*background: (.+?),\s*selection: (.+?),', b, re.S):
            ps.append([evaluate(x, sc, cs) for x in m.groups()])
        players[app] = ps

    # JSON key of each field, from the schema's serde renames.
    key_of = {}
    for m in re.finditer(r'#\[serde\(rename = "([^"]+)"\)\]\s*(?:///[^\n]*\n\s*)*pub (\w+):', schema_src):
        key_of[m.group(2)] = m.group(1)

    print("// Generated by scripts/zed-theme-defaults.py from Zed's crates/theme and")
    print("// crates/settings_content (GPL-3.0-or-later, github.com/zed-industries/zed): colour values")
    print("// evaluated to hex and the theme schema's key names, no code (assets/themes/README.md")
    print("// records what is taken and why). Do not edit by hand.")
    if len(sys.argv) > 2:
        print("// Source: " + sys.argv[2])
    print("//")
    print("// The base values Zed refines a theme off (`ThemeColors`, `StatusColors` and")
    print("// `PlayerColors` `::dark()` / `::light()`): a token a theme file leaves out, with no")
    print("// derived rule of its own (theme_compile.cppm), takes the value of its appearance here.")
    print()
    print("export module lx.ui:theme_defaults;")
    print()
    print("export namespace lx::ui::zed_defaults {")
    print()
    print("struct base_entry {")
    print("    const char* key;")
    print("    const char* dark;")
    print("    const char* light;")
    print("};")
    print()
    print("inline constexpr base_entry k_colors[] = {")
    for field in base["dark"]:
        key = key_of.get(field)
        d, l = base["dark"][field], base["light"].get(field)
        if not key or d is None or l is None:
            continue
        print(f'    {{"{key}", "{to_hex(d)}", "{to_hex(l)}"}},')
    print("};")
    print()
    print("/// cursor, background, selection per player.")
    print("inline constexpr const char* k_players[2][8][3] = {")
    for app in ("dark", "light"):
        print("    {")
        for p in players[app]:
            print("        {" + ", ".join(f'"{to_hex(c)}"' for c in p) + "},")
        print("    },")
    print("};")
    print()
    print("} // namespace lx::ui::zed_defaults")


if __name__ == "__main__":
    main()
