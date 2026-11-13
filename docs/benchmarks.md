> Historical notes from Nordend development, retained with their original names and measurements.
> Upstream: https://github.com/pankaspe/nordend-fm. Current application: rummage.

# Benchmarks

Nordend's performance targets (the design's performance goals, summed up in "DESIGN targets"
below) are measured here, not assumed. Every number below comes from a tool in this repository, so it can be run
again. Newest results first in each section; the [devlog](devlog.md) tells the story around them.

## Machine

| | |
|---|---|
| CPU | AMD Ryzen 7 5700G (8 cores, 16 threads) |
| RAM | 16 GB |
| System disk | Samsung SSD 980 500 GB (NVMe), btrfs |
| Data disk | SPCC Solid State Disk 1 TB (SATA), ext4 |
| OS | Void Linux (glibc), kernel 6.18 |
| Build | GCC 14.2, CMake Release (`-O3`) |

## DESIGN targets (ROADMAP 7.6)

### 2026-11-01

Every target of the DESIGN, measured again with today's code. On screen: Umbriel, ASUS ProArt
at 2560x1440, 75 Hz (a frame is 13.3 ms).

| Target | Measured | Verdict |
|--------|----------|---------|
| Cold start to the first frame: < 150 ms | **128 ms** median after `drop_caches` (126.7-131.7, 5 runs); **64.5 ms** warm | met |
| 10k-file folder: first frame < 50 ms, full list in the background | first entries ready in **0.68 ms**, whole folder in 16.6 ms (warm); started inside it: first frame 69 ms, whole folder on screen 86 ms after exec | met |
| Quick Look of an ORF: < 100 ms | sharp image on screen **82-96 ms** after Space (offscreen, 5 files x 3 runs; median 86); **92 ms** on screen (1 file) | met, narrowly |
| Scrolling at the monitor's full rate, no dropped frame | worst frame **3.7 ms** at 2560x1440 in every view (median 0.3-0.8 ms) | met |

Details in "Start-up", "Directory loading", "Quick Look" and "Column view scrolling" below.

## Start-up (ROADMAP 7.6)

**Target:** cold start to the first frame in < 150 ms (DESIGN).

**Tool:** `nordend-bench-start` (`src/tools/bench_start.cpp`). It forks and execs `nordend`
with `NORDEND_STARTUP_NS` (the steady clock just before the exec) and `NORDEND_STARTUP_QUIT`;
Nordend prints `startup: STEP MS` at each step and closes once its folder is shown. Steps:
`main` (dynamic linking and static initialisation done), `display` (Wayland connected), `egl`
(EGL and the GPU driver loaded), `fonts` (fontconfig), `portal` (the desktop's look read),
`window` (the window created and configured), `first-draw` (the first frame starts drawing),
`first-frame-shown` (the compositor's frame callback for it), `folder-shown` (the same for the
first frame drawn with the folder loaded), `exit` (the process is gone, seen from the tool).

```fish
set -x XDG_CACHE_HOME /var/tmp/nordend-bench-cache   # not ~/.cache; on disk, not tmpfs
set -x XDG_STATE_HOME /tmp/nordend-state-test        # a copy of state.toml: the usual size
build/nordend-bench-start -n 3 -- build/nordend ~ >/dev/null   # fills Mesa's shader cache
build/nordend-bench-start -n 10 -- build/nordend ~
for i in (seq 5); sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
    build/nordend-bench-start -n 1 -- build/nordend ~ | string match 'run*'; end
```

### 2026-11-01: on screen, Umbriel

Milliseconds since exec; the window at its remembered size, the home folder.

| Step | warm, median of 10 | warm, min-max | cold, median of 5 | cold, min-max |
|------|------:|------:|------:|------:|
| main | 4.8 | 4.6-5.5 | 26.6 | 26.2-27.0 |
| display | 5.1 | 4.8-5.8 | 27.7 | 27.5-28.1 |
| egl | 32.3 | 31.0-36.8 | 74.2 | 73.6-75.3 |
| fonts | 37.1 | 35.7-41.5 | 85.3 | 82.7-86.3 |
| portal | 38.3 | 36.9-42.7 | 88.0 | 85.3-88.9 |
| window | 44.7 | 43.3-49.1 | 95.9 | 92.6-96.3 |
| first-draw | 49.2 | 47.6-53.3 | 107.9 | 104.8-108.4 |
| **first-frame-shown** | **64.5** | 56.8-71.1 | **128.3** | 126.7-131.7 |
| folder-shown | 64.6 | 56.8-71.1 | 144.5 | 143.2-147.2 |
| exit | 83.5 | 66.0-89.2 | 184.8 | 183.1-186.6 |

Started inside the generated 10 000-entry folder (`build/bench-10k`), warm, median of 10: first
frame shown at **69.1 ms** (61.7-71.3), the folder fully listed on screen at **85.9 ms**
(83.8-94.8).

### Reading the numbers

- Loading EGL and the GPU driver (radeonsi) is the largest step: 27 ms warm, 46 ms cold, about
  40 % of the start. Nordend's own work before the window (fonts, theme, portal, state) is
  about 12 ms warm.
- A cold start costs about 64 ms more than a warm one, spread over reading the binary and its
  21 libraries (`main` at 27 ms), Mesa (`egl`) and the fonts.
- From the first draw to the compositor showing it: 15 ms warm, 20 ms cold: up to a 75 Hz frame
  of waiting for the next vblank, plus the GPU's first frame (shader cache read).
- In a 10k folder the first frame is not slower (the first chunk is ready in under 1 ms); the
  whole list follows 17 ms later, as the directory benchmark predicts.

### 2026-11-01: headless weston (llvmpipe), for comparison

`weston --backend=headless --renderer=pixman`: Mesa draws in software there, so these are not
the real figures. Warm, median of 10: `egl` 48.7, `window` 68.5, `first-frame-shown` 102.0 ms.
Once in about 30 runs weston stopped sending frame callbacks to every client until restarted;
the tool now stops a run after 10 s of silence.

## Markdown and diffs (ROADMAP 3b-2b, fourth group)

### 2026-11-09

The same harness (`g++ -O3 -DNDEBUG`, 300 runs, median, `nice 19`):

| File | Lines | Spans | Median |
|---|---|---|---|
| `docs/devlog.md` (308 kB of Markdown) | 4730 | 3863 | 1302 µs |
| `README.md` (14 kB) | 233 | 144 | 37 µs |
| `git format-patch` of the third group's commit (54 kB) | 1137 | 955 | 23 µs |
| `browser.cpp` (C++, for comparison) | 3449 | 5470 | 710 µs |

Markdown costs about 4 ns a byte, against about 1 for JSON: its inline pass looks at every
character for emphasis, code, links and URLs. A diff is coloured a line at a time from its first
character.

## Data and configuration files (ROADMAP 3b-2b, third group)

### 2026-11-09

`colour` alone on lines already read, a harness compiled with `g++ -O3 -DNDEBUG` from
`syntax.cpp` and `languages.cpp`, 300 runs, median, at `nice 19`:

| File | Lines | Spans | Median |
|---|---|---|---|
| `build/compile_commands.json` (311 kB, long lines) | 1292 | 1720 | 258 µs |
| Rust's `target-spec-json-schema.json` (26 kB) | 1390 | 1094 | 81 µs |
| a CI workflow repeated 100 times (YAML: block scalars, anchors, flows) | 3300 | 5000 | 194 µs |
| `docs/config.example.toml` repeated 140 times | 3220 | 3500 | 130 µs |
| `browser.cpp` (C++, for comparison: 662 µs on 2026-11-08) | 3449 | 5470 | 685 µs |

The C++ figure moves within the runs' noise: the new checks (keys, INI lines, TOML headers) run
only for the languages that have them.

## The general lexer (ROADMAP 3b-2b, first group)

### 2026-11-08

`colour` on `browser.cpp`'s lines (3449 lines, 5470 spans), 300 runs, median: 3b-2a's lexer
780 µs and the general one 1073 µs, both compiled with `g++ -O3` from their sources side by side;
then 973 µs with a table of quotes, 622 µs with words tried first. Quick Look on the same file
through the helper: 1.44 ms.

## Syntax colours (ROADMAP 3b-2a)

### 2026-11-08

As for 3b-1 below (throwaway program, 300 runs, median, warm cache), `browser.cpp` as C++ and
`README.md` as plain text, at 3b-2a's end:

| File | Limits | Process | Helper |
|---|---|---|---|
| `src/views/src/browser.cpp` (3449 lines, 5470 spans) | Quick Look (256 KiB) | 1145 µs | 1622 µs |
| same | info pane (40 lines) | 13 µs | 51 µs |
| `README.md` (no language) | Quick Look | 39 µs | 93 µs |
| same | info pane | 10 µs | 44 µs |

Colouring alone (`colour` on the lines already read, the same file): 1781 µs with three binary
searches per word and every character tried against every rule; 762 µs with a 256-entry table
of the characters that may open a comment, a string, a number or a word, and one hash table of
the language's words. Inside the helper (timed there for a moment): reading 186 µs, colouring
398 µs, the payload and its memfd 91 µs. On Nordend's side, decoding a payload with 5470 spans
287 µs (122 µs without); `Helpers::load_text` copied its result through a conditional
expression until 3b-2a: plain text 704 → 587 µs, coloured 2228 → 1691 µs once moved.

## Text through the base engine (ROADMAP 3b-1)

### 2026-11-08

A throwaway program linked against the build's libraries, `steady_clock` around each call, 300
runs, median, warm cache. "Before" is `preview::read_text` in Nordend at `eb1c288`; "process" is
the text family in the same process; "helper" is `isolate::Helpers::load_text` through a real
`nordend-preview` (socket, the request, the read and decoding in the cage, a sealed memfd back,
checked and split).

| File | Limits | Before | Process | Helper |
|---|---|---|---|---|
| `src/views/src/browser.cpp` (131 KiB, 3449 lines) | Quick Look (256 KiB) | 517 µs | 545 µs | 810 µs |
| same | info pane (8 KiB, 40 lines) | 26 µs | 9 µs | 45 µs |
| `README.md` (14 KiB, 233 lines) | Quick Look | 39 µs | 41 µs | 102 µs |
| same | info pane | 23 µs | 10 µs | 46 µs |

Before the ASCII fast path in the UTF-8 check (8 bytes at a time) and `memchr` for NUL bytes, the
process column read 678 µs for `browser.cpp`. The info pane got quicker in the process since
`max_lines` stops at 40 lines instead of splitting 8 KiB.

## Engines' manifests (ROADMAP 3a)

### 2026-11-08

`engines::Registry::load` on the build tree's `engines/` (one manifest, the image engine's 30
types; its program looked up next to the binary), timed with `steady_clock` around each call in a
throwaway program linked against the build's libraries, 200 runs, warm cache: median 54 µs, min
53 µs, max 170-220 µs (the first runs). Nordend does it once at start-up.

## The preview helper without exiv2 (ROADMAP 2e-5)

### 2026-11-08

`nordend-preview` started with a socket on fd 3 (a small Python script: spawn, wait 1 s, read
`/proc/PID/smaps_rollup`; and spawn to `Seccomp: 2` in `/proc/PID/status`, 40 times), before and
after exiv2 left the build:

| | with exiv2 | without |
|---|---|---|
| Direct libraries (`readelf -d`, NEEDED) | 9 | 8 |
| Libraries loaded (`ldd`) | 21 | 17 |
| RSS after the warm-up | 10.3 MB | 7.8 MB |
| PSS | 5.8 MB | 3.4 MB |
| Anonymous | 1.2 MB | 0.65 MB |
| Spawn to cage closed (median) | 4.4 ms | 3.3 ms |
| Binary (stripped, MiB) | 0.87 MB | 0.87 MB |

Fuzzing at the end of 2e (an hour, five targets in parallel, 3 processes each, nice 19 while a
game ran): camera 1.6 million inputs, picture 3.2 million, tiff 37.2 million, dng 31.9 million,
ljpeg 41.9 million; no crash, timeout or out-of-memory.

## Photo information: Nordend's reader against exiv2 (ROADMAP 2e-3)

### 2026-11-08

`nordend-bench-previews info-exiv2|info-ours 30 0 FILE` (best of 30: open, read the info pane's
fields and the size, close; no sidecar; page cache warm, machine idle):

| File | exiv2 -> ours |
|---|---|
| OM-1 ORF (author's) | 3.05 -> 0.03 ms |
| Nikon Z 7II NEF | 1.25 -> 0.05 ms |
| Canon 5DS CR2 | 0.55 -> 0.03 ms |
| Fujifilm RAF | 2.90 -> 0.05 ms |
| Panasonic GX7 RW2 | 1.09 -> 0.05 ms |
| DJI DNG | 0.33 -> 0.04 ms |
| shot.jpg (fixture) | 0.06 -> 0.01 ms |
| blocks.png (fixture) | 0.03 -> 0.00 ms |

System calls for an ORF (`strace -c -f`, two files minus one): 30 -> 28 (36 before the reader
learned to skip the previews it does not need).

## Raw previews: Nordend's reader against exiv2 (ROADMAP 2e-2)

### 2026-11-08

`nordend-bench-previews exiv2|ours 30 SIDE FILE` (best of 30: open, find the preview for a long
side of SIDE, read its bytes, close; page cache warm, machine idle after a game, load 1.4):

| File | 160 px: exiv2 -> ours | 4000 px: exiv2 -> ours |
|---|---|---|
| OM-1 ORF (author's) | 3.45 -> 0.03 ms | 4.16 -> 0.66 ms |
| Nikon Z 7II NEF | 2.46 -> 0.14 ms | 14.78 -> 4.25 ms |
| Canon 5DS CR2 | 11.81 -> 3.47 ms | 11.77 -> 3.47 ms |
| Sony ARW (_SNY1264) | 3.31 -> 0.21 ms | 8.08 -> 1.94 ms |
| Fujifilm RAF | 3.56 -> 0.38 ms | 3.48 -> 0.38 ms |
| Panasonic GX7 RW2 | 2.21 -> 0.04 ms | 2.32 -> 0.44 ms |
| Samsung EX2F SRW | 1.81 -> 0.53 ms | 1.78 -> 0.53 ms |
| DJI DNG | 1.87 -> 0.03 ms | 3.67 -> 0.61 ms |
| Samsung S22 DNG | 31.31 -> 5.44 ms | 31.09 -> 5.53 ms |
| Hasselblad 3FR | 1.55 -> 0.20 ms | 1.56 -> 0.20 ms |

Most of what is left is reading the preview's bytes (the S22's is 4 MB; the 5DS takes its 5.7
MB JPEG even for a thumbnail, as before: its 160x120 one is 4:3 with black bars). System calls
for an ORF's thumbnail (`strace -c -f`, two files minus one): 35 -> 22. An earlier run that read
Olympus's maker note whole (1.8 MB) took 2 ms for the ORF: the note is now read through a window.

## DNG opcodes: gain maps and lens warp (ROADMAP 2d-4)

### 2026-11-08

`nordend-decode --runs 9 --raw` (median of 8 runs after the first, one process, sensor read
included), with the file's opcodes and without (`DevelopOptions::opcodes`):

| File | Opcodes | 2560x1440 box | Half size |
|---|---|---|---|
| DJI FC3170 | GainMap 32x32x3 + WarpRectilinear (list 3) | 108.6 -> 135.8-139.8 ms | 243.8 -> 384.9 ms |
| Samsung S22 | 4 GainMaps of ones (list 2): dropped | 295 ms, unchanged | |
| Canon, M240, M8, GX20 | none | unchanged | |

Of the DJI's 27-31 ms in the box: the gain map about 8 ms (interpolated once a row along the
grid, then a multiply a colour), the warp about 18 ms (a 1920x1440 RGBA pass: where each pixel
comes from worked out every 16 pixels and on a line between, bilinear in 16-bit lanes with
SSE2). How the warp got there (a standalone copy of the function, 1920x1440): per-pixel
polynomial in doubles and `lround` 100 ms; knots every 16 pixels 45 ms; whole RGBA words mixed
as two 16-bit pairs 30 ms (no better: the multiplies, not the memory: nearest pixel only 8.6 ms,
the 11 MB new buffer included); SSE2 18 ms.

Quick Look (offscreen snapshot, helpers caged, 2560x1440, three runs): DJI 207-227 ms (2d-3:
169-187).

Fuzzing (`build-fuzz`, an hour, 4 processes, seeds with every opcode shape):
`nordend-fuzz-dng` 70.5 million inputs (about 19 500 a second); no crash, timeout or
out-of-memory.

Checked by eye against the DJI's own embedded rendering (960x720): the gain map lifts the
corners by up to 2.57 times (1.4 stops) and matches it; the warp moves the corners by under 4 %
(about 3 px of 800 halfway to a corner), which the preview cannot confirm (it is framed about 9
px differently all over); the warp's formula is checked by tests (twice the size from the
centre).

## DNG development made quicker (ROADMAP 2d-3)

### 2026-11-07

Same methods as 2d-2 below. Quick Look (offscreen, helpers caged, 2560x1440, three runs each):

| File | 2d-2 | 2d-3 |
|---|---|---|
| DJI FC3170 | 249 ms | 169-187 ms |
| Canon 350D | 110 ms | 110 ms |
| Leica M240 (thumbnail cached) | 296 ms (thumbnail made at once) | 256-262 ms |

`load(fd, 2560, 1440)` in one process, best of 3: DJI 195 -> 126 ms, M240 216 -> 184 ms, Canon
-> 75 ms, M8 -> 62 ms. What did it:
- Developing straight to the size shown: the half-size blocks averaged by area (as
  `scale_to_fit` does, but on linear light) into the box, the colour then worked out for the
  pixels shown only: no 48 MB picture in between, no `scale_to_fit` (27-43 ms).
- Huffman tables of 12 kB (an 11-bit lookup and the canonical codes for the long ones) instead
  of 140 kB: the Canon's 150 tiles each built two (callgrind: 128 million instructions of
  memset and fill).
- No branches on the data in the colour loop: `maxss`/`minss` for the largest and smallest
  colour, the tone curve as one line through them (no sorting), table indexes limited as
  integers.

Fuzzing (`build-fuzz`, clang, libFuzzer + ASan + UBSan, an hour, 2 processes each):
`nordend-fuzz-dng` 30.9 million inputs (about 8 500 a second), `nordend-fuzz-ljpeg` 60.7 million
(about 16 800 a second); no crash, timeout or out-of-memory.

Left: the M240's lossless JPEG (one strip, no restart markers: one stream, one core) is about
100 ms of its 184; from `load()` to the screen another 70 ms (helper, transfer, upload), as for
every format.

## DNGs developed (ROADMAP 2d-2)

### 2026-11-07

Quick Look, `nordend-snapshot --columns DIR --reveal FILE --keys "Ctrl+2,space" ... 2560 1440 1`
with `NORDEND_LOG=debug` (Release, helpers caged; the "on screen ... after it was asked for"
line); and `load(fd, 2560, 1440)` in one process (a throwaway program, median of 3, file in the
page cache):

| File | Developed to | Quick Look | `load()` | of which `develop()` | `scale_to_fit` |
|---|---|---|---|---|---|
| DJI FC3170 (48 MP, uncompressed) | 4000x3000 -> 1920x1440 | 249 ms | 195 ms | 153 ms | 41-43 ms |
| Leica M240 (24 MP, lossless JPEG) | 2988x1996 -> 2156x1440 | 296 ms | 216 ms | 170 ms | 27 ms |
| Canon 350D (8 MP, lossless JPEG tiles) | 1728x1152 | 110 ms | | 56-61 ms | |
| Leica M8 (10 MP, 8 bit) | 1958x1317 | | | 41 ms | |

`develop()` includes reading the sensor (DJI 11 ms of bands, M240 117 ms, Canon 33 ms). The M240
was measured while its thumbnail (developed too: its preview is 160 px) was being made. Steps of
the DJI's development: 220 -> 203 ms with a 32-bit index into the curve table (callgrind: the
unsigned 64-bit conversion took 13 instructions a colour), -> 153 ms with `minss`/`maxss`.

Against LibRaw's development (`dcraw_process`, half size, camera white balance, sRGB, no
automatic brightness), ours without the look, 16x12 grid of mean colours: DJI 1.2 levels of 255
on average (worst cell 11.3), M240 0.4 (1.5), M8 0.1 (1.4), GX20 1.7 (14.0), Samsung S22 0.7
(4.7), Canon 0.9 (3.9).

## DNG sensor data, our own reader (ROADMAP 2d-1)

### 2026-11-07

`nordend-decode --runs 5 --raw /dev/null FILE`, Release build, median of 4 runs after the first
(file in the page cache): every value of the sensor read and linearized into one buffer
(`read_all`). LibRaw 0.21.4: best of 3 `unpack()` in a throwaway program (`g++ -O2`).

| File | Sensor | Storage | Ours | LibRaw |
|---|---|---|---|---|
| Leica M240 | 5984x4000 CFA | lossless JPEG, 16 bit, one strip | 134 ms | 521 ms |
| Samsung S22 Ultra | 4000x3000 linear raw | lossless JPEG, 3 x 16 bit, one strip | 179 ms | 649 ms |
| Canon 350D (Adobe) | 3516x2328 CFA | lossless JPEG, 12 bit, 256x240 tiles | 46 ms | 168 ms |
| DJI FC3170 | 8000x6000 CFA | uncompressed 16 bit, one strip | 74 ms | 186 ms |
| Leica M8 | 3920x2638 CFA | uncompressed 8 bit + linearization | 18 ms | 83 ms |
| Samsung GX20 | 4864x3136 CFA | uncompressed 12 bit, packed | 38 ms | 137 ms |

The lossless JPEG decoder alone (a throwaway program on the M240's strip, 27 MB, in memory): 191
ms with a 16-bit lookup table, 161 ms with an 11-bit first level, 99 ms when that level also
holds the decoded difference (most codes and their extra bits fit in 11 bits).

## TIFF, our own reader (ROADMAP 2c)

### 2026-11-07

`nordend-thumb --size normal --make`, Release build, 5 runs, median (best), peak RSS of the
tool; three 6000x4000 (24 MP) files made with ImageMagick from one plasma picture, in
`/var/tmp/nordend-tiff-bench` (a game was running, about 1.5 cores busy):

| File | Size | Thumbnail | Peak RSS |
|---|---|---|---|
| 8-bit RGB, LZW + predictor | 16 MB | 467 (465) ms | 169 MB |
| 16-bit RGB, Deflate + predictor | 100 MB | 888 (878) ms | 169 MB |
| 8-bit RGB, uncompressed | 69 MB | 302 (297) ms | 168 MB |

The author's 12 TIFFs (`fotografia/tiff_test_pictures`), decode only (`decode_file`, one run):
2 ms (512x512) to 480 ms (5600x4200, LZW, alpha); in Quick Look on screen (the author's
run, helpers on): 21 ms (707x397) to 553 ms (5600x4200).

Fuzzing (`build-fuzz`, clang 21, libFuzzer + ASan + UBSan): `nordend-fuzz-tiff` 33.5 million
inputs in an hour on 4 processes (about 9 300 a second), `nordend-fuzz-picture` 1.24 million in
30 minutes on 2 (exiv2 and the decoders make it slower).

## Parsers out of Nordend (ROADMAP 2b-3)

### 2026-11-07: v0.3.1 against now, Release builds

| | v0.3.1 | now |
|---|---|---|
| `readelf -d nordend`, NEEDED | 21 | 17 |
| `ldd nordend`, shared objects | 40 | 29 |
| `nordend` stripped | 3 374 008 B | 3 357 336 B |
| `nordend --version`, 50 runs, median (min) | 5.78-5.88 (5.41) ms | 3.37-3.40 (3.11) ms |

`--version` loads every library and leaves: the difference is the dynamic loading of exiv2 and
the image libraries with their dependencies. Cold start to the first frame on screen: *to be
measured by the author* (`nordend-bench-start`, ROADMAP 7.6 method).

## The helper's cage (ROADMAP 2b-2)

### 2026-11-07

Same throwaway programs as for 2b-1, three rounds each: a 48x32 PNG through a helper that has to
start 5.25-5.59 ms (3.6-4.1 ms before the cage), through a running one 0.07-0.16 ms; a 1920x1440
ORF through a running helper 64.0-65.3 ms; 16 ORF thumbnails on 8 threads and 8 helpers
185.2-187.3 ms once running (204-206 ms with the helpers starting). A helper run with no socket,
start to exit (warm-up and cage included): 5.77 ms median of 20.

## Pictures in a helper (ROADMAP 2b-1)

### 2026-11-07: throwaway programs linked against the build's libraries

All Release build, warm page cache, the OM-1 ORFs of the demo home.

| What | Result |
|---|---|
| Starting a process: `nordend-decode --help` (22 shared objects) / `nordend --version` (40) | 3.71 / 5.68 ms, median of 30 |
| `_7230017.ORF` at 1920x1440, in process, 5 runs | 90, 69, 55, 54, 54 ms |
| the same through one helper, 6 runs (first starts it) | 103, 79, 64, 64, 64, 65 ms |
| of which, in a running helper: decode / memfd write / read into Nordend | 54 / 5.3 / 2.5 ms |
| a 48x32 PNG through a helper: starting / running | 3.6-4.1 / 0.12-0.19 ms |
| 16 ORF thumbnails (256 px), 8 threads, in process | 185-215 ms |
| the same, 4 helpers (starting / running) | 260-265 / 219-235 ms |
| the same, 8 helpers (starting / running) | 199-207 / 184-191 ms |
| one helper's memory after 16 ORFs at 2560x1440 (smaps_rollup) | Anonymous 54 508 kB; 1 372 kB once trimmed |
| trimming after every picture: 1920x1440 ORF | 85 ms (instead of 64-65) |

Quick Look of `_7230017.ORF` in `nordend-snapshot` (offscreen 2560x1440, scale 1, a fresh
process each run, so the helper starts cold): 119.5-136.4 ms with 4 helpers, 120.6-122.0 ms with
8 (in process: 103-107 ms).

## One path for every picture (ROADMAP 2a)

### 2026-11-07: before and after `image::load`

`nordend-thumb --size normal --make FILE` (128 px), fresh cache each round, 7 rounds, median and
minimum, Release build; the same runner (a throwaway Python script) on the binary of `0dc49bc`
and on the new one:

| File | Before | After |
|---|---|---|
| 2400x1600 PNG (plasma noise) | 177.3 ms | 175.5 ms |
| the same as JPEG q85 | 34.3 ms | 34.3 ms |
| the same as WebP q85 | 61.7 ms | 61.8 ms |
| `shot.jpg` (fixture, OM-1 EXIF) | 0.6 ms | 0.6 ms |
| OM-1 ORF | 7.6 ms | 7.8 ms |
| Nikon NEF (8280 px) | 8.0 ms | 8.2 ms |
| Samsung GX20 DNG | 133.4 ms | 134.8 ms |
| Hasselblad CFV PPM (4096x4096) | 217.6 ms | 216.4 ms |

Quick Look of `_7230017.ORF` in `nordend-snapshot` (offscreen, 2560x1440 at scale 1,
`NORDEND_LOG=debug`, "on screen" time), 5 runs: 105.1-107.2 ms before (`0dc49bc`, built in a
worktree), 103.1-104.9 ms after. Not comparable with the on-screen 82-96 ms of 7.6 (another
size and no compositor).

## Raw previews (ROADMAP 1, 0.3.1)

### 2026-11-07: `nordend-thumb`, the new sample folder

`nordend-thumb --size xx-large --make FILE` (1024 px, warm cache, Release build), one run each, on
`fotografia/raw_test_photo_new`: NEF 79-85 ms, RAF 83-100 ms, LX7 RW2 82 ms, ARW 68-86 ms, SRW
117 ms; the small TIFF previews 3-10 ms (Epson ERF 160 px, Hasselblad 3FR 290-320 px, Leica M8
DNG 320 px). Samsung GX20 DNG, whose only usable preview is the full-size JPEG inside a TIFF
(4672x3104): 177 ms. Hasselblad CFV (a 4096x4096 8-bit PPM, 50 MB): read in 43 ms, decoded in
121 ms (`nordend-decode`), 221 ms to a 256 px thumbnail.

Checking that a chosen JPEG preview is whole (`whole_jpeg`, which follows its segments and scan
markers to the EOI): 0.39 ms on the OM-1 ORF's 3200x2400 preview (969 kB), mean of 100 runs, a
throwaway program built `-O2` against `jpeg_segments.cpp`.

## Command palette (ROADMAP 8a)

### 2026-11-06: filtering

`tests/command_palette_test.cpp` ("filtering 500 items…"): 500 items of about 40 characters,
"photos" typed a letter at a time, every item matching; each key re-scores all 500 with fzy and
sorts them. **0.10-0.15 ms a key** in the Release build (target < 1 ms), 2.3 ms under ASan,
3.8 ms under TSan. The palette itself offers about 50 items plus the places and tabs.

## Finding files (ROADMAP 8b)

### 2026-11-06: `nordend-bench-find`, warm caches

`nordend-bench-find [--hidden] [--runs N] DIR QUERY...`: FileFinder on the worker pool (8
workers), the query set as the listing starts (typed as the card opens); then the same query
again over the whole list.

| Folder | Entries | First results | Whole listing | A new query over all | Paths |
|---|---:|---:|---:|---:|---:|
| `DATA/dev` (source trees, node_modules) | 273 187 | **8.2-8.7 ms** | 435-446 ms | 6-14 ms | 23.1 MB |
| `/` (its own file system only), hidden included | 106 674 | 21 ms | 448 ms | 3 ms | 6.2 MB |

Target: first results < 50 ms on ~100 000 entries. Anonymous memory: 1.6 MB at start, 30.3 MB
with the 273 000 paths, **2.9 MB** once the finder is gone and `malloc_trim` has run. Before
shrinking each batch to its contents the paths took 32.7 MB (doubling growth). Cold caches:
*to be measured by the author* (`drop_caches` before a `--runs 1`).

## Binary

### 2026-11-01: start-up and Quick Look timings (7.6)

3 668 120 bytes; 3 062 648 after `strip`; 21 direct libraries.

### 2026-10-31: settings panel removed, `--about` added

3 663 616 bytes; 3 058 552 after `strip`; 21 direct libraries. The panel and its font list are
gone; the icon font is back to 47 outline icons (13 432 bytes).

### 2026-10-31: desktop colours (7.4)

3 579 800 bytes; 2 988 792 after `strip`; 21 direct libraries, none new (the portal goes through
the sdbus-c++ already linked for udisks2).

### 2026-10-30: after 7.3

3 457 608 bytes; 2 882 136 after `strip`; 20 direct libraries: libsdbus-c++ and libbasu (its
sd-bus) for udisks2 are new.

### 2026-10-30: after 6.5

3 046 656 bytes; 2 558 296 after `strip`; 18 direct libraries (the clipboard is libwayland's
core protocol: nothing new).

### 2026-10-30: after 6.4d

2 966 072 bytes; 2 496 792 after `strip`; 18 direct libraries.

### 2026-10-29: after 6.3

2 475 360 bytes; 2 095 192 after `strip`; 18 direct libraries.

### 2026-10-29: after 6.2

2 344 128 bytes; 1 988 632 after `strip`; 18 direct libraries.

### 2026-10-29

After ROADMAP 6.1 (default applications): 2 254 728 bytes; 1 918 968 bytes after `strip`
(+275 KB, mostly `std::format` and `std::filesystem` code at `-O3`); 18 direct libraries, the
same as before.

### 2026-10-28

`build/nordend`, CMake Release (`-O3`), GCC 14.2, x86-64, after ROADMAP 5.11: 1 979 504 bytes;
1 677 048 bytes after `strip`. `readelf -d` lists 18 direct libraries (libwebp, libwebpdemux,
libjxl, libexiv2, libz, libwayland-egl, libwayland-client, libEGL, libGLESv2, libxkbcommon,
libharfbuzz, libfontconfig, libfreetype, libtomlplusplus, libstdc++, libm, libgcc_s, libc);
`ldd` resolves 37 in all.

## Memory

### 2026-10-28

The app running on screen (Umbriel, RX 6650 XT, radeonsi), started on a source folder in the
columns, 69 s after start (`/proc/PID/status` read by the author, `smaps_rollup` and `smaps`
summed per file):

| | |
|---|---:|
| Resident (VmRSS) | 117 068 kB |
| Private, anonymous (heap, glyph atlases, listings) | 32 796 kB |
| File-backed (libraries, fonts) | 84 264 kB |
| Proportional (PSS: shared pages divided among their users) | 52 438 kB |

The largest file-backed pieces: libLLVM 51.4 MB and libgallium 15.3 MB (Mesa's driver, the same
pages for every OpenGL process), libexiv2 3.5 MB, libstdc++ 2.5 MB, the `nordend` binary 1.5 MB.
A folder of photos in the grid holds more (thumbnail textures live on the GPU, *to be
measured*).

## Directory loading (ROADMAP 3.1)

**Target:** a 10k-file folder draws its first frame in < 50 ms; the full list loads in the
background.

**Tool:** `nordend-bench-dir` (`src/tools/bench_dir.cpp`).

```sh
./build/nordend-bench-dir --make 10000 DIR   # camera-like folder: ORF, ORF.xmp, JPG, subfolders
./build/nordend-bench-dir --runs 20 DIR      # warm: median of runs 2..20
sync; echo 3 | sudo tee /proc/sys/vm/drop_caches
./build/nordend-bench-dir --runs 5 DIR       # cold: the "run 1" row
```

Columns, in milliseconds:

- **1st chunk:** from the request to the first chunk (128 entries) sorted into the list on the UI
  thread, through the worker pool. This is what bounds the first frame.
- **complete:** the same request until the whole directory is in the list, sorted.
- **readdir:** reading the names alone (baseline).
- **+stat:** names plus type, size, date and link target for each entry, on one thread.
- **sort:** sorting all entries by name (natural order, directories first).

### 2026-11-01: 10k files on the SATA SSD, warm cache (7.6)

Same tool, a freshly generated folder (`build/bench-10k`), 20 runs, 8 worker threads; the MIME
column (types from names) was added in 3.2.

| ms | readdir | +stat | sort | mime | 1st chunk | complete |
|----|--------:|------:|-----:|-----:|----------:|---------:|
| min | 2.93 | 15.13 | 2.88 | 1.66 | 0.54 | 16.28 |
| **median** | **3.00** | **15.33** | **2.95** | **1.70** | **0.68** | **16.59** |
| max | 3.08 | 16.54 | 3.28 | 1.74 | 0.73 | 17.20 |

### 2026-10-26: 10k files on the SATA SSD, warm cache

20 runs, 8 worker threads.

| ms | readdir | +stat | sort | 1st chunk | complete |
|----|--------:|------:|-----:|----------:|---------:|
| min | 3.65 | 17.44 | 3.44 | 0.66 | 18.77 |
| **median** | **3.69** | **18.01** | **3.47** | **0.80** | **19.51** |
| max | 4.08 | 19.69 | 3.69 | 0.92 | 21.79 |

### 2026-10-26: 10k files on the SATA SSD, cold cache

After `drop_caches`, run 1 (the async path runs first, so its columns are the cold ones; the
others are warm by then).

| ms | 1st chunk | complete |
|----|----------:|---------:|
| cold | **1.72** | **81.27** |
| warm (median, same session) | 0.75 | 19.59 |

An earlier cold run, with the synchronous read first, gave **79.09 ms** for readdir + stat: the
same cost without the worker pool and the sorted merge.

### 2026-10-26: other folders, warm cache

Median of 20 runs.

| Folder | Entries | readdir | +stat | sort | 1st chunk | complete |
|--------|--------:|--------:|------:|-----:|----------:|---------:|
| generated 10k, tmpfs | 10 000 | 2.08 | 15.44 | 3.61 | 0.52 | 16.93 |
| `/usr/lib`, NVMe btrfs | 1 862 | 0.51 | 4.49 | 0.49 | 0.77 | 5.19 |
| `/usr/bin`, NVMe btrfs | 1 493 | 0.40 | 2.89 | 0.31 | 0.79 | 3.26 |
| a photo folder, SATA SSD ext4 | 327 | 0.13 | 0.52 | 0.07 | 0.40 | 0.75 |

### Reading the numbers

- The first chunk is ready in under 1 ms warm: the 50 ms target leaves plenty of room for
  layout and drawing.
- `stat` is most of the cost: about 1.4 µs per entry warm on the SSD, more for symbolic links
  (`/usr/lib`), which need a second `stat` for their target.
- A cold cache multiplies the cost of the whole folder by about 4 (19.6 → 81 ms for 10k), but
  the first chunk still arrives in 1.7 ms: chunked loading keeps disk latency off the first
  frame, and the rest arrives while the view is already on screen.
- Sorting 10k names is about 3.5 ms; merging chunks as they arrive costs little on top
  (complete ≈ +stat + 1.5 ms).

## List scrolling (ROADMAP 4.1)

**Target:** scrolling at the monitor's full rate with no dropped frame (DESIGN), in folders of
any size.

**Tool:** `nordend-snapshot --list DIR --bench FRAMES out.ppm [w h scale]` (replaced by
`--columns` with ROADMAP 4.2, see below): draws the list
offscreen while scrolling it from top to bottom at constant speed over FRAMES frames (about 17
new rows per frame for 10k entries), waiting for the GPU (`glFinish`) after each one.

### 2026-10-26

| List | Buffer | median | p95 | max |
|------|--------|-------:|----:|----:|
| 10 000 entries | 960x640 | 0.20 ms | 0.28 ms | 5.6 ms |
| 10 000 entries | 2880x1620 (1920x1080 at scale 1.5) | 0.25 ms | 0.45 ms | 1.5 ms |
| 123 entries (a photo folder) | 960x640 | 0.17 ms | 0.21 ms | 5.9 ms |

The maximum is the first frame, when glyphs are rasterized for the first time. Only visible
rows are drawn, so the cost does not depend on the folder size: a 60 Hz frame (16.7 ms) leaves
the list about 1.5 % of it.

## Column view scrolling (ROADMAP 4.2)

**Tool:** `nordend-snapshot --columns PARENT --reveal DIR --bench FRAMES out.ppm [w h scale]`:
opens the columns down to DIR, then scrolls the last column (DIR) from top to bottom over FRAMES
frames, drawing the whole view (title, every visible column) and waiting for the GPU after each
frame, as for the list.

### 2026-11-01: every view at 2560x1440 (7.6)

The author's screen at scale 1 (ASUS ProArt, 75 Hz: 13.3 ms a frame), the generated 10 000-entry
folder, 600 frames, two runs each (ranges are the spread between runs). Grid and details are
scrolled the same way (`--keys "Right,Ctrl+2"`, `"Right,Ctrl+3"`; `Ctrl+a` selects everything).

| View | median | p95 | max |
|------|-------:|----:|----:|
| Columns | 0.33 ms | 0.53-0.55 ms | 1.6-1.9 ms |
| Columns, info pane expanded | 0.33-0.43 ms | 0.55-0.56 ms | 1.6-1.9 ms |
| Columns, all selected | 0.53-0.55 ms | 0.71-0.76 ms | 1.7 ms |
| Grid, 96 px | 0.51-0.52 ms | 0.78 ms | 2.1-3.7 ms |
| Grid, all selected | 0.83-0.84 ms | 1.03-1.07 ms | 1.9-2.2 ms |
| Details | 0.50-0.53 ms | 0.66-0.69 ms | 2.3-2.5 ms |

The worst frame of all, 3.7 ms, is 28 % of a 75 Hz frame: no frame is dropped on the GPU side.
This is the offscreen benchmark (the design asks for no dropped frame "in the scrolling
benchmark"); frames dropped on screen by the compositor are not counted by it.

### 2026-10-26

| Last column | Buffer | median | p95 | max |
|-------------|--------|-------:|----:|----:|
| 10 000 entries (2 columns on screen) | 960x640 | 0.19 ms | 0.28 ms | 6.2 ms |
| 10 000 entries (2 columns on screen) | 2880x1620 (1920x1080 at scale 1.5) | 0.23 ms | 0.44 ms | 1.2 ms |

Same cost as the single list: the columns draw only their visible rows, and the maximum is
again the first frame's glyph rasterization.

### 2026-10-26: with a selection (ROADMAP 4.3)

Same benchmark, before and after `--keys "Right,Ctrl+a"` (all 10 000 entries selected: every
visible row gets a filled box). Two runs each, at 2880x1620 (1920x1080 at scale 1.5).

| Last column | median | p95 | max |
|-------------|-------:|----:|----:|
| nothing selected | 0.26 ms | 0.45 ms | 1.5 ms |
| all 10 000 selected | 0.52 ms | 0.62-0.68 ms | 2.0 ms |

Selected rows cost their fill (pixels, not entries): the worst case is 3 % of a 60 Hz frame.
(The unselected median is 0.26 ms here against 0.23 ms in the first run: same code path, run
to run spread.)

## Grid scrolling (ROADMAP 4.4)

**Tool:** `nordend-snapshot --columns PARENT --reveal DIR --keys "Right,Ctrl+2[,...]" --bench
FRAMES out.ppm [w h scale]`: switches to the grid on DIR (with `Ctrl+minus`/`Ctrl+plus` for
other sizes, `Ctrl+a` to select everything), then scrolls it from top to bottom over FRAMES
frames, drawing the whole view and waiting for the GPU after each frame.

### 2026-10-27

The generated 10 000-entry folder, 600 frames at 2880x1620 (1920x1080 at scale 1.5), two runs
each (the spread between runs is in the ranges).

| Icon size (cells on screen) | Selected | median | p95 | max |
|-----------------------------|----------|-------:|----:|----:|
| 96 px, default (12 x 8) | nothing | 0.32-0.33 ms | 0.57-0.60 ms | 1.3-3.1 ms |
| 96 px | all 10 000 | 0.74-0.75 ms | 0.85-0.89 ms | 1.6 ms |
| 48 px (14 x 11) | nothing | 0.48 ms | 0.71-0.72 ms | 1.7-4.0 ms |
| 48 px | all 10 000 | 0.84 ms | 0.96-0.98 ms | 1.6-2.5 ms |
| 256 px (6 x 4) | nothing | 0.25-0.26 ms | 0.36 ms | 2.3 ms |

The cost follows the cells on screen (an icon, a name, a box when selected), not the folder
size. The worst case, the smallest cells all selected, is about 5 % of a 60 Hz frame.

## Image decoding (ROADMAP 5.1)

**Tool:** `nordend-decode [--runs N] [--stored] FILE...`: reads each file, then decodes it N
times from memory and prints the median (the first run is not counted). `--stored` skips the
EXIF turn.

### 2026-10-27: 20 MP photos, warm cache

OM-1 JPEGs from the photo library (5184x3888, 4:2:2, 9-14 MB, EXIF orientation 8), and one of
them converted with ImageMagick to the other formats (after the turn: 3888x5184).

| What | Size | Decode |
|------|-----:|-------:|
| JPEG, wuffs, 20 files, as stored | 9-14 MB | median 149 ms (133-159) |
| JPEG, wuffs, turned upright (orientation 8) | 13.4 MB | 218 ms |
| PNG, wuffs | 27.5 MB | 229 ms |
| WebP lossy (q90), libwebp | 2.8 MB | 210 ms |
| JPEG XL lossy (q90), libjxl, one thread | 2.8 MB | 394 ms |
| GIF, wuffs (2592x1944) | 2.9 MB | 12 ms |
| WebP from the library (2048x1536 and 1536x2048) | 0.2-0.7 MB | 16-33 ms |

Reading a 13 MB JPEG from the SATA SSD with a warm cache: 10-37 ms.

- The EXIF turn of a full 20 MP image costs about 65 ms (80 MB read and written, even in 32x32
  tiles). The views will not pay it at full size: thumbnails turn after scaling down, Quick Look
  can turn with the texture coordinates.
- A rough comparison for the thumbnails of ROADMAP 5.2: ImageMagick, which decodes JPEG with
  libjpeg-turbo, takes 284 ms for this file in full and 97 ms with DCT scaling to 1/8
  (`-define jpeg:size=648x486`). Its 16-bit float pipeline inflates both numbers; what
  libjpeg-turbo scaling gains on its own is *to be verified* with a real benchmark.

### 2026-10-27: JPEG for thumbnails, wuffs against TurboJPEG scaling and embedded previews

**Tool:** `nordend-bench-jpeg [--runs N] FILE...` (built when `libjpeg-turbo-devel` is
installed): the same file from memory with wuffs at full size and with TurboJPEG at 1/1, 1/2,
1/4, 1/8 (DCT scaling), to RGBA, no EXIF turn.

20 OM-1 JPEGs (5184x3888, 4:2:2, 3-14 MB), median of the per-file medians:

| Decoder | Output | Decode |
|---------|--------|-------:|
| wuffs | 5184x3888 | 145 ms |
| TurboJPEG 1/1 | 5184x3888 | 108 ms |
| TurboJPEG 1/2 | 2592x1944 | 96 ms |
| TurboJPEG 1/4 | 1296x972 | 90 ms |
| TurboJPEG 1/8 | 648x486 | 75 ms |

Scaling saves little: in these high-quality files the entropy (Huffman) decoding of every
coefficient dominates, and it is the same at every scale.

What the files already carry (extracted with exiftool, decoded with wuffs):

| Embedded image | Size | Bytes | Decode |
|----------------|------|------:|-------:|
| OM-1 JPEG, Olympus maker-note preview | 1600x1200 | 375 kB | 7.8 ms |
| OM-1 JPEG, EXIF thumbnail (IFD1) | 160x120 | 9 kB | 0.1 ms |
| OM-1 ORF, Olympus maker-note preview | 3200x2400 | 1.0 MB | 30 ms |

## Thumbnails (ROADMAP 5.2)

### 2026-10-27: making one thumbnail

Inside `thumbs::Cache::make`, one file, warm cache, median of 3 (a scratch timing program
around the same calls):

| Source | Size | Find preview | Decode | Scale | Turn | PNG | Total |
|--------|------|-------------:|-------:|------:|-----:|----:|------:|
| OM-1 JPEG, 1600x1200 preview | large (256) | 1.1 ms | 4.6 ms | 3.5 ms | 0 | 2.6 ms | ~12 ms |
| OM-1 JPEG, 1600x1200 preview | xx-large (1024) | 1.2 ms | 5.0 ms | 11.1 ms | 1.6 ms | 36 ms | ~55 ms |
| OM-1 ORF, 3200x2400 preview | large (256) | 1.8 ms | 19.4 ms | 12.8 ms | 0 | 2.0 ms | ~36 ms |
| OM-1 ORF, 3200x2400 preview | xx-large (1024) | 1.9 ms | 19.2 ms | 25.8 ms | 1.7 ms | 29 ms | ~78 ms |

Two changes came from these numbers: zlib level 1 instead of 6 (PNG of an xx-large thumbnail
171 ms → 36 ms, files about 13 % larger) and halving 2x2 before the area filter (3200 → 1024:
42 ms → 26 ms). Finding the preview with exiv2 (open, metadata, preview list) costs 1-2.5 ms.
With ROADMAP 5.3 the area filter became streaming (below): the xx-large scales are now 6.0 ms
(JPEG preview) and 15.6 ms (ORF preview).

### 2026-10-27: a screenful of thumbnails in the grid

**Tool:** `nordend-snapshot --columns DIR --keys "Ctrl+2[,Ctrl+plus...]" --thumbnails CACHE`:
logs "thumbnails ready in N ms", from the first grid frame to the last thumbnail drawn (made on
the worker pool, 8 threads on the Ryzen 7 5700G). 1600x1000 at scale 1.5.

| Folder | Cells (thumbnails) | Empty cache | Warm cache |
|--------|--------------------|------------:|-----------:|
| OM-1 JPEGs | 96 px, large (69) | 253 ms | 21 ms |
| OM-1 JPEGs | 192 px, x-large (23) | 142 ms | 22 ms |
| OM-1 ORF + sidecars + WebP | 96 px, large (35) | 405 ms | 12 ms |
| OM-1 ORF + sidecars + WebP | 192 px, x-large (12) | 189 ms | 13 ms |

Scrolling the grid with thumbnails (`--bench 600`, 90 JPEGs, warm, 2880x1620): 0.62 ms median,
0.76 ms p95, against 0.32 ms with icons only; one draw call per thumbnail.

## Quick Look (ROADMAP 5.3)

### 2026-11-01: from Space to the sharp image (7.6)

**Target:** Quick Look of an ORF in < 100 ms (DESIGN).

With `NORDEND_LOG=debug`, Quick Look logs the time from asking for an entry's content to its
sharp image going to the GPU, the frame that shows it (and the thumbnail stage, when there is a
cached thumbnail; none in these runs). Offscreen:

```sh
NORDEND_LOG=debug build/nordend-snapshot --columns DIR --reveal DIR/_4230010.ORF --keys space \
    out.ppm 2560 1440 1
```

OM-1 ORF files (20 MP, 18 MB) from the author's library, shown 1440x1920 or 1920x1440 (the
window 2560x1440), each run a new process; the two neighbours are made at the same time.

| File | run 1 (first read) | run 2 | run 3 |
|------|-----:|-----:|-----:|
| _4230010.ORF | 89.8 | 92.4 | 92.5 |
| _4230011.ORF | 89.0 | 90.7 | 91.3 |
| _4230017.ORF | 82.8 | 81.9 | 81.9 |
| _4230025.ORF | 95.8 | 82.3 | 82.6 |
| _4230048.ORF | 84.8 | 86.0 | 83.0 |

On screen (Umbriel, `NORDEND_LOG=debug build/nordend DIR`, Space on one ORF): **92.1 ms**.

The cache does not matter (reading 18 MB is not the cost). Fitting the preview alone took
51-55 ms on 2026-10-27 (below); the rest is, *to be verified*, the two neighbours decoded at the
same time on other workers and the scaling to a larger screen than then. Under the target, with
4-18 ms to spare: the first thing to look at if Quick Look ever gets slower.

### 2026-10-27: an image fitted to the screen

`image::load_to_fit(file, 2880, 1620)` (1920x1080 at scale 1.5, minus nothing), best of 4, warm
cache: the preview when it is at least as large as the screen (or the file is raw), else the
whole image; scaled, then turned.

| File | Shown | From | Time |
|------|-------|------|-----:|
| OM-1 ORF (5 files) | 2160x1620 or 1620x2160 | 3200x2400 preview | 51-55 ms |
| OM-1 JPEG, 20 MP | 1620x2160 | whole image (preview 1600 < 2880) | 212 ms |
| Sony A7 II ARW | 1616x1080 | preview | 17 ms |
| Nikon D800 NEF | 2427x1620 | preview (full size) | 248 ms |
| WebP 1536x2048 | 1215x1620 | whole image | 56 ms |

Quick Look shows the cached thumbnail at once and this image when it arrives; the entries
before and after are made in advance.

The area filter had to change for this: 3200x2400 → 2160x1620 took 84 ms, because the
horizontal pass filled an intermediate buffer of input rows x output width (83 MB here). Now it
streams (each input row scaled across once, accumulated into the output row it belongs to), with
an opaque fast path and one reciprocal per pixel: 27 ms. The ORF went from 107-110 ms to 51-55 ms.

## Status bar (ROADMAP 5.11)

### 2026-10-28: scrolling with the status bar

Same folder and command as for the breadcrumb, sidebar shown.

| View | Median | p95 |
|------|-------:|----:|
| Details | 0.42-0.50 ms | 0.58-0.60 ms |
| Grid | 0.30-0.33 ms | 0.51-0.52 ms |
| Columns | 0.26 ms | 0.33-0.47 ms |

The details' median jumps between about 0.36 and 0.50 ms from run to run; five runs of the code
before this step gave the same two values (0.36, 0.50, 0.50, 0.37, 0.50 ms; GPU clock states,
*to be verified*). The p95 is unchanged: the bar costs nothing measurable. The free space is
read with one `statvfs` on a worker when the folder or its listing changes, never per frame.

## Breadcrumb (ROADMAP 5.10)

### 2026-10-27: scrolling with the breadcrumb

The same 10 000-entry folder and command as for the sidebar (sidebar shown), three runs each.
The breadcrumb measures its steps' text every frame (cached layouts).

| View | Median | p95 |
|------|-------:|----:|
| Details | 0.36-0.37 ms | 0.55-0.58 ms |
| Grid | 0.31-0.33 ms | 0.50-0.51 ms |
| Columns | 0.25-0.26 ms | 0.34-0.36 ms |

The same as without it (sidebar section below): nothing measurable.

## Context menu: Open With (ROADMAP 6.4b)

**Tool:** `nordend-open --apps TYPE...` ("<type>\t<registered>\t<recommended>", IDs separated by
`;`), against `gio mime TYPE` (GLib 2.88.3).

### 2026-10-29

Every type in `/usr/share/mime/types` and every alias in `/usr/share/mime/aliases`, 1 232 names:
**0 differences** in both lists. 564 have applications; for 380 the lists with and without
parent types differ.

```sh
(cat /usr/share/mime/types; cut -d' ' -f1 /usr/share/mime/aliases) | sort -u > all.txt
build/nordend-open --apps $(cat all.txt) 2>/dev/null > ours.txt
while read t; do
  out=$(gio mime "$t" 2>/dev/null)
  reg=$(printf '%s\n' "$out" | sed -n '/^Registered applications:/,/^Recommended applications:/p' \
        | grep '^\s' | tr -d '\t ' | sed 's/$/;/' | tr -d '\n')
  rec=$(printf '%s\n' "$out" | sed -n '/^Recommended applications:/,$p' \
        | grep '^\s' | tr -d '\t ' | sed 's/$/;/' | tr -d '\n')
  printf '%s\t%s\t%s\n' "$t" "${reg:--}" "${rec:--}"
done < all.txt > gio.txt
diff ours.txt gio.txt
```

## Trash (ROADMAP 6.4)

**Tool:** `nordend-transfer --trash SOURCE_DIR NAME...` (the engine behind Delete), against
`gio trash NAME...` (GLib 2.88.3, which trashes local files itself, without gvfs).

### 2026-10-29

1 000 empty files and a folder of 200, in a fake `$HOME` on tmpfs, each tool with its own
`$XDG_DATA_HOME` (3 runs):

| What | Time |
|------|-----:|
| Nordend, 1 001 items | 24-25 ms |
| `gio trash`, 1 001 items | 69-71 ms |

Each item is a `.trashinfo` made with `O_EXCL`, then a rename; the folder's size (`du -B1`) goes
to `directorysizes`. Same files, same `.trashinfo` sizes and `Path=` lines as gio's for names
with spaces, `%`, `#`, `?`, accented letters and leading dots; the same numbering for a name
trashed twice (`a b%#?.2.ORF`).

```sh
set -x HOME $S/home   # fish; files in $S/home/n and $S/home/g
cd $S/home/n; env XDG_DATA_HOME=$S/home/dn build/nordend-transfer --trash . (ls)
cd $S/home/g; env XDG_DATA_HOME=$S/home/dg gio trash (ls)
```

## Deleting permanently (ROADMAP 6.4d)

**Tool:** `nordend-transfer --erase SOURCE_DIR NAME...` (the engine behind Shift+Delete and Empty
Trash), against `rm -rf` (GNU coreutils 9.4), bash's `time`.

### 2026-10-30

A tree of 100 folders of 100 small files (10 101 entries, 5.0 MB), made again before each run
(3 runs each, `sync` before):

| Where | Nordend | `rm -rf` |
|-------|--------:|---------:|
| tmpfs | 79-80 ms | 53-55 ms |
| ext4 (SATA SSD, the DATA disk) | 195-198 ms | 170-171 ms |

Nordend walks the tree twice: once to count the entries and bytes (the progress bar's total),
then to delete (an `fstatat` and an `unlinkat` per entry, through folder descriptors, links never
followed). The first walk is the 25 ms difference; the card's own count (`plan_erase`, on a
worker before asking) is the same walk.

```sh
# a tree in $D/tree: 100 folders x 100 files, then
build/nordend-transfer --erase $D tree
time rm -rf $D/tree
```

## Giving decoded photos back (ROADMAP 6.3c)

### 2026-10-29: on screen, the author's five steps, after the change

The same script and steps as for 6.3b, no `GLIBC_TUNABLES`: the app sets the threshold itself.

| Step | RSS (peak so far) | malloc in use / freed but kept | VRAM | Thumbnail textures |
|------|------:|------:|-----:|------:|
| 1 home | 111.2 MB (111.2) | 18.2 / 3.6 MB | 50 MB | 0 |
| 2 `vero`, columns | 119.5 MB (164.2) | 18.6 / 8.9 MB | 54 MB | 1.0 MB |
| 3 grid, all thumbnails | 141.0 MB (189.7) | 21.4 / 25.7 MB | 156 MB | 85.5 MB |
| 4 after Quick Look | 142.6 MB (240.4) | 22.9 / 25.6 MB | 206 MB | 120.1 MB |
| 5 back home | **142.6 MB** | 22.9 / 25.6 MB | 206 MB | 120.1 MB |

Against 6.3b's default run: 143 MB instead of 545 MB after browsing the photos. The VRAM is
the thumbnails' textures (257 of them, under the 256 MB budget), kept for a return to the
folder; it is GPU memory, not in the RSS.

### 2026-10-29: offscreen, with `core::give_back_large_blocks` (mmap threshold 16 MB)

`nordend-snapshot --memory` reports the peak too (`VmHWM`); `--default-malloc` leaves glibc's
threshold alone to compare. Grid's first screenful into an empty thumbnail cache, three runs
each; the concurrency limit was a temporary experiment (a semaphore around
`thumbs::Cache::make`, removed):

| malloc | Thumbnails made at once | Ready | RSS after | Peak |
|--------|------------------------:|------:|----------:|-----:|
| glibc's default | 8 (all workers) | 188-201 ms | 386 MB | 524-525 MB |
| glibc's default | 4 | 230-234 ms | 547 MB | 547 MB |
| glibc's default | 2 | 378-395 ms | 543-545 MB | 543-545 MB |
| threshold 16 MB | 8 | 205-215 ms | 152 MB | 536 MB |
| threshold 16 MB | 4 | 252-255 ms | 152 MB | 348 MB |
| threshold 16 MB | 2 | 440-452 ms | 152 MB | 250 MB |

The threshold costs 7 % on a screenful of new thumbnails and keeps 234 MB less afterwards. A
limit of 4 would lower the peak (the moment all workers decode at once) by 188 MB for 20 % more
time, 2 by 286 MB for twice the time: over the 10 % agreed, so no limit.

Quick Look over 20 ORF (`--keys Down,Down,space --walk 20`): RSS 155 MB (peak 457 MB) instead of
584 MB; the three slots keep 0 bytes of pixels once uploaded (was 10.5 MB).

## Memory with photos (ROADMAP 6.3b)

**Tools:** the memory report (`pkill -USR1 nordend` logs it; `nordend-snapshot --memory
[--trim]` prints it): `smaps_rollup` and `smaps` by mapping, glibc `mallinfo2`, the GPU driver's
fdinfo (`drm-memory-vram`, `-gtt`), and Nordend's own counts. heaptrack 1.5.0 for who allocates.

### 2026-10-29: on screen, the author's five steps

The app on Umbriel, a fresh thumbnail cache each run (`XDG_CACHE_HOME` in `/tmp`), the steps
driven by a script that sends SIGUSR1 after each: (1) the home, (2) `vero` in the columns, the
cursor on an ORF, (3) the grid scrolled to the end, every thumbnail made, (4) Quick Look over
about 20 photos, closed, (5) back home, 10 s later. Default glibc, then with
`GLIBC_TUNABLES=glibc.malloc.mmap_threshold=16777216`.

| Step | RSS, default | malloc in use / freed but kept | RSS, threshold 16 MB | malloc in use / kept |
|------|------:|------:|------:|------:|
| 1 home | 110.9 MB | 18.2 / 3.6 MB | 110.9 MB | 18.0 / 3.6 MB |
| 2 `vero`, columns | 130.1 MB | 18.9 / 19.8 MB | 119.8 MB | 18.6 / 9.6 MB |
| 3 grid, all thumbnails | **526.4 MB** | 20.4 / **412.8 MB** | **138.7 MB** | 20.2 / 25.1 MB |
| 4 after Quick Look | 544.7 MB | 21.3 / 429.7 MB | 139.4 MB | 21.0 / 25.0 MB |
| 5 back home | **544.7 MB** | 21.5 / 429.5 MB | **139.5 MB** | 20.7 / 25.4 MB |

Where it goes, step 5 by default: anonymous memory 455.8 MB, of which malloc's arenas hold
445.9 MB and its own mappings 5.1 MB (99 %); libraries 89 MB (shared with other programs). Of
malloc's 446 MB, 21.5 MB are in use and 429.5 MB were freed and kept. The GPU is separate: VRAM
50-94 MB (the second run 159-166 MB: step 4 was done from the grid, with 238 thumbnails, 89 MB
of textures), GTT 17-34 MB; neither is in the RSS. Thumbnail textures were 1-89 MB (budget
256 MB), Quick Look 0 once closed, glyph atlases 21 MB of VRAM.

### 2026-10-29: who allocates (heaptrack), offscreen

`nordend-snapshot --columns vero --thumbnails EMPTY_DIR --keys Ctrl+2 --walk 60 --memory`,
2880x1620: peak consumers are wuffs' pixel buffer (245.8 MB peak over 51 calls) and work buffer
(122.9 MB over 51 calls), both under `thumbs::Cache::make` → `image::decode_preview` → wuffs
JPEG, on the workers: the OM-1's embedded preview decoded whole, about 30 MB of pixels and 15 MB
of work buffer each, eight workers at a time. They are freed at once; glibc keeps them because
its dynamic mmap threshold has grown to their size (up to 32 MB), so they come from the worker
threads' arenas, which are not given back. The base, in the sources: Mesa's gallium 6.7 MB,
LLVM (shader compiles) 6.3 MB, Nordend itself under 2 MB (renderer 0.5 MB, MIME database
0.4 MB); the snapshot tool's own pixel buffers 18.7 MB (not in the app).

The 3 GB copy (`nordend-transfer vero /tmp/...`): peak RSS of the whole process 11.6 MB: the
kernel copies (`copy_file_range`), nothing passes through Nordend.

### 2026-10-29: what a fix would give, offscreen

The grid's first screenful with an empty thumbnail cache (`--keys Ctrl+2 --memory`, three runs),
and Quick Look over 20 photos (`--keys Down,Down,space --walk 20`), with the threshold set by
`GLIBC_TUNABLES`:

| mmap threshold | Thumbnails ready | RSS after | RSS after Quick Look |
|----------------|-----------------:|----------:|---------------------:|
| glibc's default (dynamic) | 183-189 ms | 386 MB | 567 MB |
| 1 MB | 208-212 ms | 138 MB | 191 MB |
| 4 MB | 202-209 ms | 152 MB | 164 MB |
| 16 MB | 199-201 ms | 152 MB | 181 MB |

Also tried: `glibc.malloc.arena_max=2`: 197 MB (one walk with a 21.6 ms frame: contention);
`malloc_trim(0)` once the work is done: 2-9 ms, 529 → 250 MB in the grid but 558 → 460 MB after
Quick Look (fragmented arenas give back little).

## Copy and move (ROADMAP 6.3)

**Tool:** `nordend-transfer SRC DEST` (the engine Ctrl+V uses; prints files, MB, time, rate),
against `cp -a SRC/. DEST/` (GNU coreutils 9.4) timed with `date +%s%N`.

### 2026-10-29: a photo folder into tmpfs, warm cache

A folder of the author's photos on the SATA SSD (ext4: 342 files, 3 087.8 MB, ORF
files, their sidecars and a folder of exports) into an empty folder in `/tmp` (tmpfs), read
once first so the page cache holds it; the destination emptied between runs.

| Run | Nordend | `cp -a` |
|-----|--------:|--------:|
| 1 | 1.390 s | 1.395 s |
| 2 | 1.393 s | 1.378 s |
| 3 | 1.382 s | 1.400 s |

About 2.2 GB/s for both: the copy is the kernel's (`copy_file_range`), and the extra steps
(a temporary per file, its rename, times and permissions) cost nothing measurable.

## Renaming in place (ROADMAP 6.2)

### 2026-10-29: scrolling with the name check in every row

The same 10 000-entry folder and command as below (sidebar shown), three runs each:

| View | Median | p95 |
|------|-------:|----:|
| Details | 0.37-0.50 ms | 0.56-0.58 ms |
| Details, a name being edited | 0.50 ms | 0.57 ms |
| Grid | 0.30-0.32 ms | 0.50-0.53 ms |
| Columns | 0.25-0.27 ms | 0.34-0.47 ms |

The same as before: nothing measurable.

## Sidebar (ROADMAP 5.9)

### 2026-10-27: scrolling with the sidebar

A 10 000-entry folder (`nordend-bench-dir --make 10000`), 600 frames at 2880x1620
(`nordend-snapshot --columns DIR --keys "Ctrl+3|Ctrl+2|Right[,Ctrl+B]" --bench 600 out.ppm
1440 810 2`), three runs each; the sidebar lists 9 places.

| View | Sidebar | Median | p95 |
|------|---------|-------:|----:|
| Details | shown | 0.35-0.50 ms | 0.54-0.58 ms |
| Details | hidden | 0.29-0.30 ms | 0.51-0.52 ms |
| Grid | shown | 0.30-0.32 ms | 0.50-0.51 ms |
| Grid | hidden | 0.27 ms | 0.47-0.50 ms |
| Columns | shown | 0.24-0.25 ms | 0.33-0.34 ms |
| Columns | hidden | 0.18-0.19 ms | 0.26-0.27 ms |

The sidebar costs about 0.03-0.06 ms per frame: its background, a dozen rows of text and icons.

The mount table, polled for POLLPRI (scratch C program and a `PlacesWatcher` in a user and
mount namespace, `unshare -rm`): idle, poll() times out with no events; a mount wakes it once
(POLLPRI|POLLERR, then quiet again), and the watcher reports the new mount point and, after
`umount`, its removal.

## Info pane preview (ROADMAP 5.8)

### 2026-10-27: walking a folder of ORF files

**Target** (ROADMAP 5.8): walking a folder of ORF files with the arrows, the thumbnail appears
without slowing the walk.

A folder of the author's photos (163 OM-1 ORF, with sidecars and JPEGs), at
2880x1620 (`nordend-snapshot --columns DIR --reveal FIRST.ORF [--keys Ctrl+2|Ctrl+3]
--thumbnails CACHE --walk 120 out.ppm 1440 810 2`): Down once per frame, the event loop run
for about 1 ms between frames so that the pane's summaries and thumbnails arrive, frames
timed alone. "Cold": a new thumbnail cache.

| View | Cache | Median | p95 | Max |
|------|-------|-------:|----:|----:|
| Columns | cold | 0.41 ms | 0.62 ms | 1.6 ms |
| Grid | cold | 0.46 ms | 1.30 ms | 3.3 ms |
| Details | cold | 0.58 ms | 1.33 ms | 2.3 ms |
| Columns | warm | 0.47 ms | 1.19 ms | 3.2 ms |
| Grid | warm | 0.55 ms | 1.39 ms | 3.7 ms |
| Details | warm | 0.61 ms | 1.10 ms | 3.3 ms |

The highest frames upload a 1024 px thumbnail texture. Behind the frames, once the cursor
stops on an entry (five ORFs, best of 5 / one run each, warm file cache):

| Step | Time |
|------|-----:|
| `preview::summarize` (stat, owner, EXIF, sidecar) | 3.7-4.6 ms |
| xx-large thumbnail made (`nordend-thumb --size xx-large --make`) | 107 ms |
| xx-large thumbnail read from the cache | 9.5-10 ms |

## Info pane (ROADMAP 5.7)

### 2026-10-27: scrolling with the pane open

The same 10 030-entry folder as for the details view, 600 frames at 2880x1620
(`nordend-snapshot --columns DIR --keys "Ctrl+3[,Ctrl+I]" --bench 600 out.ppm 1440 810 2`),
three runs each:

| View | Pane | Median | p95 |
|------|------|-------:|----:|
| Details | open | 0.44-0.46 ms | 0.52-0.54 ms |
| Details | closed | 0.33-0.45 ms | 0.50-0.53 ms |
| Grid | open | 0.28 ms | 0.46 ms |
| Grid | closed | 0.30 ms | 0.48 ms |

The median moves between about 0.33 and 0.46 ms from run to run with or without the pane (GPU
clock states, *to be verified*); the p95 stays at about 0.5 ms. The pane is drawn once per frame
and costs nothing measurable.

## Details view (ROADMAP 5.6)

### 2026-10-27: sorting and scrolling 10 000 entries

**Target** (ROADMAP 5.6): a 10k-file folder sorted by size or date scrolls without stutter.

A folder of 10 000 sparse files of random sizes (0-30 MB) and dates, five camera-like
extensions, plus 30 folders, made by a scratch Python script (`truncate` and `utime`).
`nordend-snapshot --columns DIR --keys "Ctrl+3" --sort FIELDS --bench 600 out.ppm 1440 810 SCALE`:
the sort is timed around `DetailsView::sort_by`, then the table is scrolled from top to bottom
over 600 frames, each waiting for the GPU.

| Order | Sort | 1440x810 median / p95 / max | 2880x1620 median / p95 / max |
|-------|-----:|----------------------------:|-----------------------------:|
| Name (as loaded) | — | 0.31 / 0.50 / 1.7 ms | 0.33 / 0.49 / 1.8 ms |
| Size | 1.1 ms | 0.31 / 0.50 / 3.3 ms | 0.33 / 0.51 / 1.6 ms |
| Date, newest first (two clicks) | 1.1 + 2.0 ms | 0.44 / 0.51 / 1.9 ms | 0.45 / 0.53 / 1.6 ms |
| Type | 3.2 ms | 0.31 / 0.52 / 3.6 ms | 0.33 / 0.53 / 1.5 ms |

The maximum is the first frame (glyphs rasterized). Sorting by type compares type names
(string comparisons); by size or date, numbers.

## Text previews (ROADMAP 5.5)

### 2026-10-27: reading and scrolling text

`preview::read_text` and `preview::read_info`, best of 7, warm cache (a scratch program linking
`libnordend_preview.a`):

| File | Read | Result |
|------|-----:|--------|
| Log file, 1.6 MB (first 256 KB) | 0.67 ms | 3 196 lines |
| C++ source, 22 kB | 0.06 ms | 495 lines |
| `/usr/bin/ls` (as text) | 0.02 ms | binary: card |
| Folder of 10 000 files (`read_info`) | 4.3 ms | 10 000 entries |

Scrolling Quick Look's text page from top to bottom over 600 frames
(`nordend-snapshot --columns DIR --reveal FILE --keys space --bench 600 out.ppm 1440 810 SCALE`),
each frame waiting for the GPU:

| Text | Screen | Median | p95 | Max |
|------|--------|-------:|----:|----:|
| Log, 3 196 lines | 1440x810 | 0.45 ms | 0.64 ms | 3.2 ms |
| Log, 3 196 lines | 2880x1620 | 0.82 ms | 1.03 ms | 4.0 ms |
| darktable XMP, 486 lines | 2880x1620 | 0.69 ms | 0.95 ms | 2.6 ms |

The maximum is the first frame, when the monospace glyphs are rasterized. Each benchmark frame
moves about 5 lines, so most frames shape new lines: scrolling by hand costs less.

## Raw previews (ROADMAP 5.4)

### 2026-10-27: other cameras (rawsamples.ch, 22 files)

22 files from rawsamples.ch (not in the repository; the test over them runs when
`NORDEND_RAW_SAMPLES` names their folder), warm cache, Release.

**What the files carry** (`exiv2 -pp`, then the TIFFs extracted with `exiv2 -ep` and looked at):
every one has a 160x120 EXIF thumbnail, and a JPEG of 1536-8688 px except the Hasselblad 3FR
(only an uncompressed 320x240 TIFF). Canon's TIFF previews (CR2, 384x256 to 668x448, 8 or
16 bits) are sensor data without white balance or tone curve: green and dark, not usable. The
EXIF thumbnail is always 4:3: for 3:2, 16:9 and 1:1 photos it has black bars (6-8 rows, or 20
columns; measured with `magick -crop ... -format %[fx:mean]`).

**Quick Look**, `image::load_to_fit(file, 2880, 1620)`, best of 4 (a scratch program linking
`libnordend_image.a`):

| File | Shown | Whole file read first | Head read first |
|------|-------|---------------------:|----------------:|
| OM-1 ORF (preview large enough) | 1620x2160 | 51.9 ms | 51.6 ms |
| Hasselblad 3FR (TIFF preview) | 320x240 | 42.1 ms | 0.4 ms |
| Nikon D800 TIFF | 570x375 | 89.2 ms | 2.3 ms |
| Panasonic GX8 16:9 (cropped) | 1920x1082 | 17.6 ms | 10.8 ms |
| Sony A7 II ARW | 1616x1080 | 16.4 ms | 12.6 ms |
| Nikon D800 NEF (preview large enough) | 2427x1620 | — | 265 ms |
| Canon 5DS CR2 (8688x5792 JPEG) | 2430x1620 | — | 379 ms |

When the preview is smaller than the screen, `decode_file` is tried on the raw file first (it
could be an image format) and fails; it allocated and read the whole file before finding out
(55 MB for the 3FR, 112 MB for the TIFF). Now it reads 16 bytes first and stops on a format it
does not decode. The NEF and the CR2 decode a full-size JPEG preview (wuffs has no reduced-scale
JPEG decoding).

**Thumbnails**, `nordend-thumb --make --size SIZE`, a fresh cache each run, best of 3:

| File | normal (128), before | normal, after | large (256) |
|------|--------------------:|--------------:|------------:|
| Canon 5DS CR2 | 8.9 ms | 372 ms | 376 ms |
| Canon 1D X CR2 | 9.0 ms | 143 ms | 146 ms |
| Sony RX10 II ARW | 7.5 ms | 22 ms | 24 ms |
| Panasonic GX7 1:1 RW2 | 4.3 ms | 33 ms | 37 ms |
| Nikon D800 TIFF | 10.0 ms | 9.8 ms | 11.8 ms |
| Hasselblad 3FR | no thumbnail | 2.9 ms | 5.5 ms |
| Panasonic GX8 16:9 RW2 | 4.2 ms (4:3) | 4.2 ms | 30 ms |

The normal size got slower for 3:2 and 1:1 photos: their EXIF thumbnail has black bars and is
now skipped, so the next preview is decoded (for the Canons, the full-size JPEG: wuffs cannot
decode a JPEG at a reduced scale). The large size, which a 96 px cell uses at scale 2, already
decoded it before. A grid of the 22 files at scale 1 is ready in 401 ms instead of 48 ms the
first time; from the cache, nothing changes.

## MIME detection (ROADMAP 3.2)

**Tools:** `nordend-bench-dir` ("mime" column: types from names, as the loader does) and
`nordend-mime PATH...` (full detection, content included, timings on stderr).

### 2026-10-26: warm cache

| What | Entries | Time |
|------|--------:|-----:|
| Load the shared-mime-info database (1 231 globs, 507 magic sections) | | 1.0 ms |
| Types from names, generated 10k folder (median of 20) | 10 000 | 1.7 ms |
| Types from content, `.xmp` sidecars of a photo folder | 163 | 7 ms |
| Types from content, `/usr/bin` (mostly no extension) | 1 475 | 52 ms |

Names decide most files for free; reading heads costs about 35-45 µs per file even warm, so the
views will ask for content-based types only for visible entries, on workers.

Correctness, against `gio info -a standard::content-type`: 0 differences on about 4 750 files.

## Default applications (ROADMAP 6.1)

**Tool:** `nordend-open` (`--default TYPE...` prints the chosen desktop ID; the time to read the
applications goes to stderr).

### 2026-10-29

| What | Time |
|------|-----:|
| Read every desktop entry, mimeapps.list and mimeinfo.cache (21 entries, 5 runs) | 0.42-0.48 ms |

Correctness, against `gio mime TYPE` (GLib 2.88.3): every type in `/usr/share/mime/types` and
every alias in `/usr/share/mime/aliases`, 1 232 names, 564 of them with an application:
0 differences.

```sh
(cat /usr/share/mime/types; cut -d' ' -f1 /usr/share/mime/aliases) | sort -u > all.txt
build/nordend-open --default $(cat all.txt) 2>/dev/null > ours.txt
while read t; do
  d=$(gio mime "$t" | sed -n 's/^Default application for “[^”]*”: //p')
  printf '%s\t%s\n' "$t" "${d:--}"
done < all.txt > gio.txt
diff ours.txt gio.txt
```

## Desktop portal at start (ROADMAP 7.4)

### 2026-10-31

Reading the desktop's look before the first frame (session bus connection, proxy, signal match,
`ReadAll` of `org.freedesktop.appearance`, synchronous with a 300 ms limit): **about 1 ms**
(1.0 to 1.3 ms over five runs), timed with doctest's `-d` on the read-only test against the real
portal (xdg-desktop-portal with the gtk and Umbriel backends, already running):

```sh
for i in 1 2 3 4 5; do build/tests/nordend-tests -tc="portal: the real*" -d | grep -E '^[0-9.]+ s'; done
```

A portal still starting (D-Bus activation) can take longer: past 300 ms Nordend starts with its
default colours and takes the answer when it arrives (not measured: the portal was running).

---

*Benchmarks designed, run and written up by Claude (Opus), Nordend's copilot; hardware runs and
cold-cache measurements by the author.*
