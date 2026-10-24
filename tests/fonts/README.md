# Test fonts

Two small subsets the text tests register next to the bundled fonts (`assets/fonts`), so the
text golden images (tests/golden/ui/text_*.png) and `test_text_raster` do not depend on what
the host has installed: the bundled IBM Plex Sans and Lilex have no Arabic and no colour
glyphs, and a fontconfig fallback would pick whatever the host offers. Tests ask for these
families by name; nothing outside `tests/` registers this directory.

Both are subsets made with fontTools 4 (`pyftsubset`, `--no-hinting --desubroutinize`) and
renamed (name IDs 1, 3, 4, 6; 21 and 22 dropped), which both licenses require of a modified
font: the OFL reserves the name "Plex", and CC BY 4.0 asks that changes be indicated.

## Lumen Test Arabic — `LumenTestArabic-Regular.ttf`

A subset of **IBM Plex Sans Arabic** Regular 1.1.0 (© 2017 IBM Corp., SIL Open Font License
1.1, Reserved Font Name "Plex"; license: `OFL-IBM-Plex-Sans-Arabic.txt`, the release's
`LICENSE.txt` byte for byte).

Source: <https://github.com/IBM/plex/releases/tag/%40ibm%2Fplex-sans-arabic%401.1.0> (asset
`ibm-plex-sans-arabic.zip`, SHA-256
`f03915581aea37d82792c188b08064023a73494d679b8e19f85f5971db714013`), file
`fonts/complete/ttf/IBMPlexSansArabic-Regular.ttf` (SHA-256
`8e0f1046c736bf939d4939ee3ae0116acf61cbcd6592deae7656761627080981`).

Kept: U+0020–U+007E and the characters of `العربية: مرحبا بالعالم 42 Lumen`, every layout
feature (`--layout-features='*'`), so joining forms, ligatures and marks shape as in the
full font.

## Lumen Test Emoji — `LumenTestEmoji-Regular.ttf`

A COLRv0 / CPAL subset of **Twemoji Mozilla** 0.7.0 (code © 2016–2018 Mozilla Foundation,
Apache License 2.0; emoji art from Twemoji by Twitter, CC BY 4.0; both in
`LICENSE-Twemoji-Mozilla.md`, the repository's `LICENSE.md` at tag v0.7.0).

Source: <https://github.com/mozilla/twemoji-colr/releases/tag/v0.7.0> (asset
`Twemoji.Mozilla.ttf`, SHA-256
`6d90152ee0d29e82fe2a87793af5aa4b7ad13e6538360889e141e81ed299ee8e`).

Kept: U+1F600 😀, U+2764 ❤, U+1F44D 👍, U+1F680 🚀, U+2B50 ⭐ (19 glyphs with their colour
layers), no layout features. Changes: subset and renamed, as above.

| File | SHA-256 |
|------|---------|
| LumenTestArabic-Regular.ttf | `3b92bf6b651a89d1b0d60ad49f5d96186fe402f5077d392f43acca2afc1b20c1` |
| LumenTestEmoji-Regular.ttf | `22f17171cf1b36bd7660f6be39f49b92f79750be410fde031ae9c9e24fa1bd51` |
| OFL-IBM-Plex-Sans-Arabic.txt | `7e6b2818edbd8f6a01ae80641cc8f16a51080d08fb4e532be3a0b6f74adb07da` |
| LICENSE-Twemoji-Mozilla.md | `64419edc28e9163204c3be73f835a8dfc34cd6c9b8f7d067bb685f119f839a99` |
