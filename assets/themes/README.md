# Bundled themes

Zed's **One** theme family (One Dark, One Light), the source of ADR-010 §5.1's colour
tokens, vendored unmodified from Zed:

| File | Source | SHA-256 |
|------|--------|---------|
| `one/one.json` | [`assets/themes/one/one.json`](https://github.com/zed-industries/zed/blob/a3f6ef252b6de19d22a1223952fc335253163642/assets/themes/one/one.json) at commit `a3f6ef252b6de19d22a1223952fc335253163642` (2026-07-17, the file's last change on `main` when fetched on 2026-10-25) | `ed6cf88015f2f9b39691315840cf0615f35cdc1be4a61f9f0dba8183fa427695` |
| `one/LICENSE` | the same directory's `LICENSE` (MIT, GitHub Inc. — the One themes descend from Atom's) | `35da5a4cdd100e20c792bf0c30ccfb6668e90b67aa3511c5f2828b019e2b342f` |

The file is a theme family in Zed's schema (`https://zed.dev/schema/themes/v0.2.0.json`).
`tools/lumen-theme` compiles each theme of it into the `.lxtheme` format `lx.ui:theme` loads:
the build writes `themes/one-dark.lxtheme` and `themes/one-light.lxtheme` into the build tree
and installs them to `${CMAKE_INSTALL_DATADIR}/lumen/themes` (`LUMEN_THEME_DIR` overrides
where they are found). `modules/lx.ui/theme_builtin.cpp` holds the same two compiled themes,
so an application without the files still renders One Dark; regenerate it after changing
this file or the format:

    ./build/release/lumen-theme --emit-cpp modules/lx.ui/theme_builtin.cpp assets/themes/one/one.json

Any other Zed theme family compiles the same way (`lumen-theme <family.json> <out.lxtheme>
--theme "<name>"`); keys the compiler does not keep are counted, missing ones fall back by
Zed's own rules (`--verbose` lists both).

## Zed's base colours (`modules/lx.ui/theme_defaults.cppm`)

A key a theme file leaves out falls back as Zed falls back: through Zed's derived rules, then
to the value of Zed's base theme of the same appearance. `scripts/zed-theme-defaults.py`
evaluates those base values — `ThemeColors::dark()` / `light()`
(`crates/theme/src/default_colors.rs`), `StatusColors` and `PlayerColors`
(`crates/theme/src/styles/`), over the Radix colour scales they are written in — to hex
colours, keyed by the JSON key names of Zed's theme schema (`crates/settings_content/src/theme.rs`),
and writes them as a table. Unlike `one/one.json` (MIT), those crates are licensed
**GPL-3.0-or-later**.

What the table takes from them is data, not code: for each schema key, a dark and a light
colour value, plus the key names a Zed theme file uses (needed to read such files at all). No
Rust source, expression or structure of Zed's is copied; the evaluation is the script's own.
The working position (recorded for the maintainer to confirm; it is not legal advice) is that
these colour values and key names are factual interoperability data rather than a copy of
Zed's code, so `theme_defaults.cppm` is distributed under Lumen's licence. Only 11 (One Dark) and 13 (One Light) of the compiled
tokens come from the table — the rest are `one.json`'s own or derived from it — so, should
that position ever change, the table can be replaced by values chosen from `one.json` and
first-party ones without touching the compiler or the format.
