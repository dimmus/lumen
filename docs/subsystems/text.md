# Text

`lx.text` turns UTF-8 into glyphs on screen: font faces, itemization, shaping, line layout
and glyph rasterisation into the shared atlas. It is the text half of the GPU UI engine
([ADR-010](../adr/010-gpu-ui-engine.md) §4.6, Phase 2); the drawing half is the `mono_sprite`
/ `poly_sprite` primitives of `lx.gfx` (§4.2) and their three renderers.

Read with: [ui-architecture.md](ui-architecture.md) §6 (how nodes emit text),
[rendering-performance.md](rendering-performance.md) §6 (UI numbers),
[memory-management.md](memory-management.md) §3 (caps and `overflow_action`).

---

## 1. Partitions

Layer: Graphics. `lx.text` depends on `lx.foundation`, `lx.runtime`, `lx.trace` and
`lx.gfx` — the last for the atlas only (tile ids, `atlas::insert` / `lookup` / `use`).

| Partition | Does | Library | Kept |
|-----------|------|---------|------|
| `:faces` ([faces.cppm](../../modules/lx.text/faces.cppm)) | Family resolution, face loading, metrics, fallback search | fontconfig, FreeType, HarfBuzz (`hb_font_t`) | LRU of 32 (family, weight, style) faces, each with its `FT_Face` and `hb_font_t`; sized per use (`FT_Set_Char_Size` only when the size changes) |
| `:itemize` ([itemize.cppm](../../modules/lx.text/itemize.cppm)) | One paragraph → runs of one (bidi level, script, face, attribute span); grapheme and line-break flags | fribidi (UAX #9), HarfBuzz (scripts), libunibreak (UAX #14, #29) | Fallback face per (primary face, script, 128-code-point block), 256 entries, negative answers included |
| `:shape` ([shape.cppm](../../modules/lx.text/shape.cppm)) | One run → glyph ids, advances, offsets, clusters | HarfBuzz | `shape_cache`: 4 096 entries keyed by (face, size 26.6, features + language, direction, script, FNV-1a 64 and length of the UTF-8), LRU, glyphs in one arena of 131 072 |
| `:lines` ([lines.cppm](../../modules/lx.text/lines.cppm)) | The retained `text_layout`: paragraphs → lines → visually ordered, positioned runs; wrapping, ellipsis, alignment, hit testing | — | Owned by the node holding it, until an input changes |
| `:raster` ([raster.cppm](../../modules/lx.text/raster.cppm)) | Glyph → atlas tile | FreeType | `glyph_cache`: 16 384 ways (8 per set), keyed by (face, size × scale, subpixel variant, glyph, hinting) |
| facade ([lx.text.cppm](../../modules/lx.text/lx.text.cppm)) | The single-line and wrapped-paragraph API over the shared caches: `shape_line` / `fit_line` / `render_line`, `wrap_text` / `render_wrapped` | — | — |

Consumers:

- `lx.ui:text_node` — `text_block_node`, a retained `text_layout` emitted as sprites
  (ui-architecture.md §6).
- `lx.compositor:decoration` — server-side title bars: `chrome_text` shapes and fits a title
  with `shape_line` / `fit_line` and rasterises its glyphs through a `glyph_cache` over the
  compositor's own atlas (`chrome_atlas`; §6 below).
- `lx.shell` (`shm_canvas::text`), `lx.portal` and the `lx.ui` widgets still rasterise into
  ARGB with `render_line` / `render_wrapped` until Phase 5.

---

## 2. Caps

Every structure is sized at construction; growth past a cap applies its `overflow_action`
(memory-management.md §3), never an unbounded allocation.

| Structure | Cap | Past it |
|-----------|-----|---------|
| Face cache | 32 faces | least recently used face dropped (ids are never reused, so nothing keyed by an old id can hit) |
| Itemizer | 65 536 code points per call, 4 096 runs | the rest of a long paragraph is a further chunk; extra runs merge into the last (`runs_overflowed`) or the call fails (`reject`) |
| Fallback table (per itemizer) | 256 (primary, script, block) entries, direct-mapped; a 4 096-entry (face, code point) coverage memo | a colliding key replaces the entry |
| Shape cache | 4 096 entries, 131 072 glyphs, 4 096 glyphs per run, 4 × the glyphs in UTF-8 bytes (compared on every hit) | `drop_oldest` (LRU, the arenas compacted 1/16 at a time); a longer run is cut at a cluster (`truncated`) |
| `text_layout` | 4 MiB of UTF-8, 1 M glyphs, 65 536 runs, 65 536 lines, 4 096 attribute spans, 16 faces | `truncated()`, or the pass fails with `reject` |
| Glyph cache | 16 384 ways (the compositor's: 4 096), bitmaps up to 256 px (128) | the set's way drawn longest ago (by the atlas's clock) replaced; its tile released only if already stale, else left to the atlas's 120-frame aging |
| Atlas (`lx.gfx:atlas`) | 4 R8 + 2 RGBA8 pages, 1024² growing once to 2048²; 16 384 tiles; 256 dirty rects per page | stale tiles (unused 120 frames) freed in place; a page above 85 % with stale tiles rebuilt at the next `begin_frame`; more dirty rects make the page whole-dirty |

Per glyph a layout keeps about 20 B, per cluster 28 B, per line and per run 64 B, plus a
byte per text byte; S4's 139 KB, 99 771-glyph document holds 7.9 MB.

---

## 3. Threading contract

| Object | Owner | Lock |
|--------|-------|------|
| `face_cache` | any thread, under its mutex | `lock()` for anything that uses a face beyond one call (shaping, glyph loading); `contended()` lets a long holder (a worker's shape pass) yield between paragraphs |
| `shape_cache`, `itemizer` | one owner each (`set_owner_affinity` asserts); `shared()` ones only under `face_cache::shared().lock()` | a miss shapes under the face lock; a hit takes none |
| `text_layout` | the node that holds it (UI), or a worker while it shapes a staging copy | its queries lock the engine's face cache lazily |
| `glyph_cache` | the atlas's owner (UI) | a miss rasterises under the face lock; `find` / `find_placed` take no lock |
| `gfx::atlas` | UI (`allocate`, `write`, `release`, `begin_frame` assert it) | none of its own; the compositor's `chrome_atlas` adds a mutex because render reads its pages (§6) |

Large text is shaped off the UI thread: a `text_block_node` with a `text_async_shaping`
lays out text of 4 KiB or more (`lx::ui::k_text_async_min_bytes`) on the worker affinity —
its own shape cache and itemizer over the block's face cache — into a staging layout handed
back with `executor.post(ui, …)`; the previous layout stays on screen until it lands. Glyph
rasterisation stays on the UI thread, where the atlas lives.

---

## 4. Fonts and fallback

- **Bundled**: IBM Plex Sans (`k_ui_family`, regular, bold, italic) and Lilex
  (`k_mono_family`) under `assets/fonts`, registered with `FcConfigAppFontAddDir` on the
  cache's own fontconfig configuration (the process default is never modified), from
  `LUMEN_FONT_DIR`, else the build tree, else the install. The UI family falls back to
  fontconfig's `sans-serif` match, the mono family to JetBrains Mono, then `monospace`.
- **Per-character fallback**: a grapheme cluster whose code points the run's face lacks
  takes a face fontconfig finds by charset for the first missing one; the answer (or "none")
  is kept per (primary face, script, 128-code-point block). Common / Inherited clusters
  (spaces, digits, punctuation) stay in the face of the run before them, so a run is not
  split at every space.
- The single-line API (`shape_line`) resolves one face and does not fall back: titles in a
  script the title font lacks show its `.notdef`, as the CPU titles did.

---

## 5. Rasterisation

- **Subpixel positioning**: a pen x is rounded to a quarter pixel (`glyph_pen`): its whole
  pixels place the sprite, the quarter picks one of 4 variants, rasterised with the outline
  moved right by variant / 4 px (GPUI's `SUBPIXEL_VARIANTS`). The baseline snaps to a whole
  pixel. A glyph sprite therefore always lands 1:1 on whole pixels and samples its tile
  exactly (nearest, on every backend).
- **Hinting**: unhinted grayscale by default; `glyph_hinting::light` (FreeType's light
  autohinter, vertical only) for scales below 1.5 when the caller asks — the compositor's
  titles do, as the CPU titles did.
- **Colour glyphs** (`FT_LOAD_COLOR`: COLRv0, CBDT / sbix) go to an RGBA8 page as
  premultiplied pixels, drawn as `poly_sprite`; rasterised once, at variant 0.
- **Eviction**: a tile the atlas evicted makes the next lookup miss and the glyph is
  rasterised again on the frame that needs it (`counters().rerasterized`).
- **Linear light**: a target that blends in linear light (the Vulkan compositor's F16 pass;
  later an scRGB client swapchain) draws `colour × coverage^contrast`, the theme's
  `text.contrast` (`gfx::k_text_contrast_dark` 1.67 for light text on dark,
  `k_text_contrast_light` 0.57 for dark on light), which keeps the mean stem weight within
  3 % of the encoded-space blend (`test_ui_golden` `text_contrast_keeps_stem_coverage`;
  measured +1.5 % on the compositor's titles, +14.7 % without the exponent,
  `test_deco_titles`).

---

## 6. Compositor titles

`decoration_manager` owns a `chrome_text` over the compositor's `chrome_atlas` (ADR-010
§4.11). A title is shaped at the title font × scale ("Sans" bold 10 pt by default), fitted to
the room left of the close button with a "…" at a cluster boundary (a not-responding window
keeps " (Not Responding)" whole and fits the title to what it leaves), and every glyph is
placed and rasterised once — when the title, its ellipsis cut or the window's output scales
change. The manager keeps one run per output scale the frame is on (up to 4), so the title
is drawn 1:1 on every output; a view of another scale resamples the largest run. The close
glyph is not a font glyph: its analytic "×" (`build_close_coverage`) is rasterised per
scale into the same atlas.

The atlas is the one structure here two threads touch: the UI thread writes tiles (under
`chrome_atlas::mutex`, only on a miss) and looks them up while frames emit; the render
thread's UI renderer reads the pages under the mutex (the CPU renderer samples them in place,
GL and Vulkan upload their dirty rects). A tile a published frame names is not reused before
it ages out (120 frames); a rebuild moves tiles and fully damages the next frame. A frame
emits each glyph with one `atlas::use` — no shaping, no rasterising, no allocation.

---

## 7. Lines and bidi

- Paragraphs split at LF, CR LF, CR and U+2029. Each is itemized once; fribidi is skipped
  for a paragraph with nothing right-to-left.
- Every run is shaped whole, once. A line cuts a run's glyphs at a cluster boundary instead
  of reshaping the halves, so a re-wrap at any width shapes nothing; HarfBuzz's
  unsafe-to-break is not consulted (breaks fall after spaces or between ideographs).
- Greedy wrapping at UAX #14 opportunities (inside a word only when one cluster is wider
  than the line); trailing whitespace hangs past the edge (UAX #9 L1); each line's runs are
  reordered visually (L2); start / end alignment follow the paragraph's direction.
- `max_lines` puts "…" at the logical end of the last line, within its wrapped extent (CSS
  `line-clamp`); an unwrapped line too wide is cut at any grapheme.
- Hit testing maps a point to a logical position with caret affinity (`upstream` at a
  direction boundary and a soft break); inside a ligature carets are spaced evenly.
- Correct for Arabic and Hebrew test strings (`test_text_lines`).

---

## 8. The shaping boundary (measured)

Host F8 (VMware, 2 vCPUs), load 0.4–0.7; cold = shape cache empty, warm = a new layout over
a warm cache (ADR-010 §7 Phase 2, as built):

| Invalidation | Cold | Warm |
|--------------|------|------|
| 14-byte label | 3.3 µs | — |
| one 76-byte line | 15–36 µs (0.7–0.8 ms the first time a script needs a fontconfig fallback search) | — |
| 669-byte paragraph | 0.13 ms | 53 µs |
| 6.9 KB page | 1.4–1.5 ms | 0.7–1.1 ms |
| 139 KB document (S4) | 31–38 ms | 20–22 ms; a re-wrap 0.8 ms |

About 0.21 µs a byte cold: below 4 KiB an inline reshape stays under ~1 ms and is done on the
UI thread; at 4 KiB and above it moves to the worker. While a worker shaped a 150 KB document
the UI thread waited at most 1.3 ms for the face lock. A steady S4 frame (2 000 lines,
scrolling) shapes nothing and emits ~2 100 sprites in one sprite batch.
