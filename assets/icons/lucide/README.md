# Bundled icons: Lucide

The icons of ADR-010 §5.5: a subset of [Lucide](https://lucide.dev) (ISC; the icons Lucide
derives from Feather are also MIT, as `LICENSE` states), the 53 the first component set
(§5.6) and the shell panel use. Every file is copied byte for byte from the release tag
[`1.61.0`](https://github.com/lucide-icons/lucide/tree/1.61.0/icons) (`icons/<name>.svg`);
`LICENSE` is the repository's `LICENSE` at that tag. 17566 bytes of SVG.

Lucide icons are 24-unit, stroke-only (`stroke="currentColor"`, width 2), monochrome.
`lx.ui:icon` rasterises each (icon, device size) once with librsvg into the atlas's R8
pages and tints it at draw time with an `icon.*` theme token. The directory is found as
`LUMEN_ICON_DIR`, this source tree, or the install (`${CMAKE_INSTALL_DATADIR}/lumen/icons/lucide`).

`trash.svg` is the icon Zed and earlier Lucide releases called `trash-2` (Lucide 1.x renamed it;
`trash-2` remains an alias in its metadata).

To add one: fetch `icons/<name>.svg` from the same tag, add it to `lx::ui::icon` and
`k_icon_names` (modules/lx.ui/icon.cppm) in the same order, and add its row below.

LICENSE: `b495047bd93a9b06913511076f504daba17d5bbeb3e0650f3bb53a4220329c57`

| File | SHA-256 |
|------|---------|
| arrow-left.svg | `c5f4b88076108eb1a0d95336e1aa7e7bb1419a3a92af96f6247d88cd3456f933` |
| arrow-right.svg | `288397fdb73bb4f783cf9eabb726ff47357df1d4e3f1201dd748090579f108b9` |
| battery-charging.svg | `6a95bf102235d7070121bb8eb21a4836fa7ea9f0dbe8327eadbdf94253acdcbd` |
| battery-low.svg | `a158265a07d3ea60134de7ca88c023ebbe35d9af9c724c410ed251f248987a52` |
| battery.svg | `bd0e96da226beca6d150ee25f04bc5ca8466c1ed91e806c2179723e173af2282` |
| bell-off.svg | `14ad2775c3a594f09a79c195531778ae51302f847a9c558f720340d1e450bd6a` |
| bell.svg | `394889e9631fe3a9a63be9a479a95b451b708e432d895978d21d9130b5ee5dbe` |
| bluetooth.svg | `f2bf3d88cf608834dc389f01b1ee41fdb7bf3656dabef69632c60d5498f3bf29` |
| calendar.svg | `312ac8e715ac2b71bcc942dec2e8de9c1929085474f08885af35b563f6aa7a0b` |
| check.svg | `7f33acc9a77a61659531044525fc008edebe215bf4dcf1c789c8674ad3277db0` |
| chevron-down.svg | `66ea878e72ed3488bb3b464c39dfdccee8d1f78e560dccea40e5e12da0e87e87` |
| chevron-left.svg | `83b0681aa38bf55e9d52a1e4b4cced624abe1fe7678ecafda133a574f1161d93` |
| chevron-right.svg | `2758143d7b2434e4aa7307dfd34405c87909ff4052f21b5f3f40d45224b4f19b` |
| chevron-up.svg | `d09f13fcbef3c493ead7e4663b5ee91804fec38d5edb5e7cc3acfbd547641cf4` |
| circle-alert.svg | `ce3e98b7a03b78c0071131df42466e4e1ff5187fccad30e739ec9ce951cf685a` |
| circle-check.svg | `9711e045a5999f8a3b0782eb88e09b04e0559bc7b379dcc0a4ae645223e10345` |
| circle-dot.svg | `c263f6a08a089c06ad40f408ad5da9100359ab52d379e6c571cd7c5bd74eaa87` |
| circle-x.svg | `bcd8788901e6f29e1b231a81ba5e707d083d06cb4848a28f29407fab4f8e0b64` |
| circle.svg | `3a991bd47beaf9874fba6fdf87bbba442970a89b5e6aa391558bc8b0a00a0513` |
| clock.svg | `e9d3e3acf4d1c280fcf8092293439dc0a4756a908ceb859de144b12451cd1cb9` |
| copy.svg | `ea80e566c7a12628a447cb53179b19aea4f60b9143e2becd0d0f20bf260e5718` |
| ellipsis-vertical.svg | `655ee2104d9540cc9377c83f3b9fea628d78d9f4e1cdca7c1b36e952599944d7` |
| ellipsis.svg | `4f495cc72013ffdfec677f03b33a150f7b4dd741979283fd6853a09024bca112` |
| external-link.svg | `3891241b8c3bfc3a8a930078f26ffc5a7e9763da6d0162ace2050b28d4c1d1fe` |
| file.svg | `499039795d7412d67664ff79d2eb6b7412e746b24c18c8fd8aba383c24ddada0` |
| folder.svg | `f0d92b94b797a8ab7d4c4ae33c3236c47f64068351b4e14bdb5014ee42898a39` |
| house.svg | `28216c479005731ef5e382e7d8627eda2b129b019eb1f3d9de9f5915576d448e` |
| info.svg | `bc977a64eb96f3e9c1041ffd09a3fceb70e3e65c02b571f76064062ed31f3cb9` |
| keyboard.svg | `f509925ca81964ed06251ad4702e989bb81a566ffc74d15c2226b4f07ded67be` |
| lock.svg | `d2cfe60cac8deaef36442b5c8218c1e61abe18222784b2a370ea090d13fca840` |
| log-out.svg | `f24b5b45c05a3acc346af8095408f6ba197ff58ea1641a4f2cb25a9702d2c0ac` |
| menu.svg | `af6491988ac1ba30dc5414f485bc48713e957e1edb0fbef4a0dd8691c3c21598` |
| minus.svg | `a0c743ab6dbf545d8a6e19ef3874f48ede686ce68d25e231bd81f540d97b1f19` |
| monitor.svg | `d1a443233345724859de8e4fb48968ba91e885e12ddbd710701548bd91b39428` |
| moon.svg | `205b959e940d5841f0ce1d09c933153da9d9c94130a54d5cc0c68c2af1d403da` |
| pencil.svg | `7e1ca7a6f5c1eb949671df762f2baadd32f5bd841d43153c3a15279af7d78d0c` |
| plus.svg | `7f6af73bf1ff6c4bca3f18351c8d1bdec6749c0c2530c4de5da85d520c21df17` |
| power.svg | `1e6b84a659aa43f826cdd8df0a63d349720004e2ccb10c33e415a990a311aa05` |
| refresh-cw.svg | `2e10dd403c85a24f163d59fc6151aa21147fe9402e1305dfc8979208caee8944` |
| search.svg | `283d371c2e433817bb9c0c8310caa6c77fa4177c0f4f1168d9c83b97af7389dc` |
| settings.svg | `0ae27fd0f81999229e3127ac96c5b32edfea448e291d509e76212b917551d66b` |
| sun.svg | `a3955c8b0425fcc5c9ba12d2bdd77fe53b2cd5dd05e7742a56f342e996fb2ef8` |
| terminal.svg | `4f9b6488b237757d5cb615ccd07502fa99774826839e2a99017784bdb2596f1a` |
| trash.svg | `27299f69ad7c6272be64b1b8e2d48cbd6dcf0ef0d4f92827a1affa945c91700e` |
| triangle-alert.svg | `4866f38b8560d410f21e3226413e0b77997b6dfbb6931fadfe0a0d5aef9ffeb4` |
| user.svg | `eae3b3b2400031ddd5ec869327d127a96014e5f63aa6e06658dd5d4e90661aa8` |
| volume-1.svg | `e8520e1c57f8bf84579ecacf5463cd3ac55ab1597e62b2844ecd95d753233bf2` |
| volume-2.svg | `fb404f9c128a0579de67b399177631e5edb3502fdc247ceb30e8b15754b46071` |
| volume-x.svg | `f382e421b7aae01172989f2b749ee87e44c0c82c16970b13a9ad5a323b613be0` |
| volume.svg | `d2062d9512891894f25ad9f7be9ec8546ab45125814aebacdc6122f6f56ae65c` |
| wifi-off.svg | `318b290106e55c3505933f434d3663524206c7a44165a89508774fba4fdbaadd` |
| wifi.svg | `5c4c795d014fa95d1e1038362df44a0f0f32ded9cb08c833ae72419ebadc9a91` |
| x.svg | `4a9cdab38fbb96162e7dace28e33f4ca0e49d8963a6162abc3d4691b7d675117` |
