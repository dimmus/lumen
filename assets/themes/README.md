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
