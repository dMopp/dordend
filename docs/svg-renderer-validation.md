# SVG renderer acceptance

On 2026-11-10 the author approved ending the pixel-parity effort, returning stroke
expansion to FreeType, and judging a fixed set of icons at native size alongside
geometric correctness and performance tests. This supersedes the original stage-3
requirements for 98% corpus agreement and pixel passes for every filtered icon.

The captured rasters and numeric comparator remain unchanged. Corpus percentages
are diagnostic measurements, not release gates. Changes to this policy must be
recorded explicitly; improving a percentage alone does not justify more custom
stroke or raster approximation code.

## Required checks

- All SVG semantic, geometric, API, bounds and committed original-fixture tests
  pass. Keep exact pixels where geometry supports them. Keep the existing fixture
  comparator; do not recapture or replace references to accommodate changes.
- Full release tests, ASan/UBSan tests, and the Nix package build pass, with the
  existing documented unrelated test accommodations. A final-source sanitizer
  fuzz run lasts at least 1800 seconds with no new finding, timeout or OOM.
- Total parse/render time stays within 1.5 times each preserved corpus baseline
  at 16, 48 and 128 px. Measure after competing builds and fuzzing stop.
- Review every entry in `tests/data/svg-review.tsv` at native 16, 48 and 128 px on
  light and dark backgrounds. The fixed selection includes all 17 filtered
  Adwaita icons, all three filtered hicolor icons, prior problem silhouettes,
  thin strokes, patterns, embedded images and a known sizing-contract difference.
  Do not replace entries because they look worse or have low pixel scores.

## Visual acceptance

An icon must retain its identity, overall silhouette, intended content and usable
contrast at its display size. Missing components, unexpected holes, clipping,
broken masks, misplaced images, missing patterns, unreadable outlines and broad
unexplained colour loss block acceptance. Also record rendering errors encountered
outside the fixed selection; this set is not permission to ignore known defects.

Differences in antialiasing, subpixel edge placement and fine stroke weight can be
accepted after inspection when identity, content and usability are retained.
Known unsupported text remains out of scope. Public aspect-ratio sizing continues
to round; the old CLI's ceiling-sized reference does not override that contract.
Enlargements help explain a difference but do not substitute for native-size review.

Record the renderer hash, selection, review result, named deviations and any open
defects. A contact sheet alone is not evidence that the review passed. If the
simpler backend has unacceptable native-size defects, report them and revisit the
renderer constraints; do not restart a custom pixel-matching loop.

## Reproduction

Inside `nix develop`, after building `rummage-svg-render`:

```sh
python3 scripts/svg_review.py --output build/svg-review
python3 scripts/svg_corpus.py --report build/svg-review/corpus.json
```

The review command verifies source hashes and requires all selected captures. It
creates a self-contained native-size comparison and a JSON rendering inventory.
Missing or changed sources make the review incomplete; they are never silently
replaced. Neither command invokes resvg. The full corpus still records every alias,
numeric failure and timing. Keep third-party sources and generated reviews local
under ignored build directories; only selection identifiers are committed.

## Hardening checks

Hardening applies to every renderer stage. The renderer consumes untrusted bytes
in-process on the UI thread; these are deterministic resource ceilings, not a
wall-clock deadline or a whole-process memory limit.

| Resource | Enforced limit |
| --- | --- |
| Input and XML | 8 MiB, 100,000 elements, depth 256 |
| Parsed geometry | 1,000,000 segments across the document |
| References | Cycle detection, chain depth 256, 10,000 expanded `use` nodes |
| Stylesheets | 10,000 rules, 100,000 expanded declarations, 10 million style work units |
| Resource parsing | 64 MiB of cumulative paint, inherited resource-attribute and embedded-image text |
| Raster geometry | 32,767 outline points, 262,144 coverage edges |
| Dash expansion | 1 million work units, including zero-entry scans and curve subdivision |
| Raster and compositing work | 64 × 1024² units, accumulated through layers |
| Offscreen storage | 16 live layers, 64 MiB combined layer/texture accounting |
| Dimensions | Output side 1024, padded layer side 2048, pattern tile side 1024 |
| Embedded images | PNG/JPEG base64 only, at most 1024² decoded pixels |
| Blur | Device-space sigma clamped to 32, total padding at most 384 |

Style work includes attribute/declaration bytes and selector comparisons, including
class-list scans. Dash work also contributes to canvas work, including all-zero
arrays that fall back to solid strokes and the bytes scanned in inherited arrays.
Resource parsing charges inherited values again when they are consumed; charging
only their original XML attribute would allow small shapes to repeat large parses.
Resource inheritance copies attributes and child indices, not unused element text.
Texture accounting is conservative: bytes
remain charged after temporary textures are released. The output canvas, parsed
document, path storage and decoder/raster scratch allocations are outside the
offscreen-storage total and have their own bounds.

Numeric input must be finite. Generated path coordinates are bounded to 10¹²;
FreeType input coordinates must fit the renderer's ±32,767 coordinate limit
before fixed-point conversion. Arc angles are checked after radius/centre
calculations, and repeated texture coordinates are reduced to the tile range
before integer conversion. Finite inputs alone do not guarantee finite
intermediate arithmetic.

The public `render` entry point catches exceptions, including allocation failures
and exceptions from warning callbacks, and returns an empty image. FreeType
decomposition callbacks catch failures before returning to C. External references
are ignored; the renderer has no file or network fetching path, and Wuffs receives
only decoded data-URI bytes. DTD declarations never expand custom entities.

Keep the adversarial cases in `tests/svg*_test.cpp` alongside the semantic tests:
input/depth/node/segment limits, reference cycles and expansion, CSS work, dash
work, raster expansion, layer count and aggregate padded-layer memory, oversized
embedded images, blur clamping, numeric extremes and callback exceptions. Both
accepted small inputs and rejected oversized expansions matter. Preserve the
original regression seeds under `tests/fuzz/svg/`.

For numeric hardening, also enable `-fsanitize=float-cast-overflow` in the fuzz
build and set `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. A zero process
exit alone is insufficient when UBSan recovery is enabled: inspect diagnostics.
The final-source fuzz requirement remains at least 1800 seconds with no finding,
timeout or OOM. Keep its binary immutable, record source/binary hashes, and verify
the process has exited before measuring final corpus performance.
