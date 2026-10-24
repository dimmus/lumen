# Bundled fonts

The UI and mono families of ADR-010 §5.4. Both are under the SIL Open Font License 1.1; each
family's license text is next to its files (`OFL.txt`, copied byte for byte from the
release). `lx.text:faces` registers this directory with `FcConfigAppFontAddDir` before the
first family is resolved: the build tree's copy, the installed copy
(`${CMAKE_INSTALL_DATADIR}/lumen/fonts`), or the directory named by `LUMEN_FONT_DIR`, which
replaces both.

All files are the static (non-variable), hinted TrueType builds the releases ship (each has
`fpgm` and `prep`); `lx.text` renders unhinted, so the hints only cost bytes. 1 055 830 bytes
in total.

## IBM Plex Sans 3.005 — `ibm-plex-sans/`

Source: <https://github.com/IBM/plex/releases/tag/%40ibm%2Fplex-sans%401.1.0>
(release `@ibm/plex-sans@1.1.0`, asset `ibm-plex-sans.zip`, SHA-256
`fb365d910566e6d199cc2c15579a7dd9a267128e18431a394ed81f1970c69200`), files taken from
`ibm-plex-sans/fonts/complete/ttf/`; `OFL.txt` is the package's `LICENSE.txt` (CRLF as shipped).

| File | SHA-256 |
|------|---------|
| IBMPlexSans-Regular.ttf | `975dcda37d80f038dcd143c22e33ca2d97a0cc5a929aace1c749153b0fe1afa5` |
| IBMPlexSans-Italic.ttf | `a9c6ef9942c49e49d11e11a6dacc0b3a087978757e9b22a06b8ac22a6400fb15` |
| IBMPlexSans-Bold.ttf | `9e6c74a889a700d707613d24548fe4ffa6bc59559a0689d2cf9e133bdcdafb2f` |
| OFL.txt | `7e6b2818edbd8f6a01ae80641cc8f16a51080d08fb4e532be3a0b6f74adb07da` |

## Lilex 2.630 — `lilex/`

Source: <https://github.com/mishamyrt/Lilex/releases/tag/2.630> (asset `Lilex.zip`, SHA-256
`c946bf022f22895799eb69bcc5f2318ed4089e4e34b90054dfd78ed5316728e0`), files taken from
`ttf/`; `OFL.txt` from the repository at tag `2.630`
(<https://raw.githubusercontent.com/mishamyrt/Lilex/2.630/OFL.txt>), since the archive carries
no license file.

| File | SHA-256 |
|------|---------|
| Lilex-Regular.ttf | `511e5093b256b1cd1a02b8b6175d93a899e0ac3d3ca7af957ae798894097cb5c` |
| Lilex-Bold.ttf | `c67d3ea85f9777c3fdc0bd891c242c35da98c075023bfc2534b91e4a8cd01d99` |
| OFL.txt | `cc3bd94cf51f40137e353fc9ca0859d54ec07e59c2092270b64a0723f4b34011` |

Updating a font: download the release into a scratch directory, check the archive's hash,
copy the files, and update this table in the same change.
