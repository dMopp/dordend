> Historical notes from Nordend development, retained with their original names and measurements.
> Upstream: https://github.com/pankaspe/nordend-fm. Current application: rummage.

# Devlog

Work log, newest first. Each entry: what was done, decisions, measurements, open points.

---

## 2026-11-12 — Go to a folder or a server (Ctrl+L)

**Done:** `LocationDialog`, opened by Ctrl+L (`go_to`, configurable) or the command palette,
takes a path or a server address. `fs::parse_location` reads `/path`, `~`, `file://`, `smb://`,
`sftp://` and `\\server\share`; `fs::add_remote_bookmark` appends a server once. A server is
added to the GTK bookmarks (a toggle, on by default) and mounted through the same path as a
sidebar click. Texts translated.

**Validation:** four new cases (parsing, adding a server once, the card's keys, the browser
going to a folder and to a server with and without the sidebar). Shortcut and palette tests
cover the new command. Full suite: 525 of 527 pass, the same two environment failures as before.
End to end in an isolated headless Umbriel: Ctrl+L, a local Samba test server's address, Go,
the bookmark appeared, gvfs asked for a login, anonymous mounted it and its files showed.

---

## 2026-11-12 — Network bookmarks through gvfs

**Done:** `smb://` and `sftp://` lines in the GTK bookmarks file are sidebar places now, without a
path until mounted. `platform::Gvfs` talks to gvfsd on the session bus through sdbus-c++
(`MountTracker.MountLocation`, `ListMounts`, `Mount.Unmount`) and exports an
`org.gtk.vfs.MountOperation` object per mount, so `AskPassword` and `AskQuestion` arrive on the
loop thread and are answered from Nordend's own cards (`PasswordDialog`, `ConfirmDialog`). A mounted
share opens through gvfsd-fuse's folder (`$XDG_RUNTIME_DIR/gvfs/smb-share:server=…,share=…`) and
gets the eject icon. `TextField` gained a masked mode for the password.

**Decisions:** gvfs's D-Bus protocol rather than `gio mount` in a child process: no GLib or gio
binary needed, structured errors, and the password dialog stays Nordend's. URIs map to mount specs
as gvfs's own mappers do (smb: server and share case-folded, `DOMAIN;user` split, port 445
dropped; sftp: host, user, port); a mount matches when it has every key asked for, since gvfs adds
the user who logged in. gvfsd is only asked for its mounts when it already runs, so Nordend does
not start it for people without network bookmarks. Without a session bus or gvfs a message says
so; nothing else changes.

**Validation:** 14 new cases (URI parsing, labels, removal by URI, smb/sftp mount specs, matching,
local paths, places with mounts, the password card, the browser flow with a fake gvfs, the masked
field, and a read-only check of the real session bus). End to end in a headless Umbriel with its
own runtime dir and session bus: a guest share on a local Samba server mounted from the sidebar and
showed its files, unmounted from the eject icon; a real server's share asked for a password and
cancelling it said so; on a bus without gvfs the message appeared. Full suite: 521 of 523 cases
pass; the two failures (`column_view_test.cpp:334`, `transfer_test.cpp:249`) fail the same way
before the change in this environment.

**Open:** AskQuestion (sftp host keys) uses the two-button confirm card with the first choice; a
card for more choices could come later. The breadcrumb shows the FUSE path of a mounted share.

---

## 2026-11-11 — SVG icons prepared against fork main

**Integration:** rebased the icon-theme prerequisite and six SVG commits onto the fork's
`main` (`da06cd9`) in a separate branch. The fork did not yet contain icon-theme support.
Retained its merged larger-grid, transparency and Nix smoke-test changes; excluded the older
combined branch's video-thumbnail and packaging-policy changes. Added the SVG comparison
header to the Nix source fileset so the development renderer builds when unit tests are off.

**Validation:** renderer, picture decoder, SVG fixtures/tests and fuzz harness are unchanged
from `841d54f`; its final 1801-second fuzz and corpus evidence still applies to those sources.
The integrated release suite passes 483 cases / 140,752 assertions. ASan/UBSan passes
482 cases / 140,717 assertions, using the documented fixture adaptations in a temporary copy,
Mesa software rendering and fresh processes for both memory tests. The first sanitizer run
hit `memory_test.cpp:30`: ASan released cached anonymous memory between the two measurements.
The memory-report case passes separately (14 assertions); it was not omitted or weakened.
Only the already documented unrelated isolate zero-length memcpy diagnostics remain.
`nix flake check` passes the x86_64-linux package and installed-entry-point smoke checks;
aarch64-linux was not built on this host. Integration evidence is retained locally under
`build-svg-handoff/pr-integration/` in the original checkout.

---

## 2026-11-11 — SVG hardening: bounded repeated work and safe numeric conversions

**Scope:** audited the required hardening section after stage 3. Existing input, XML,
geometry, reference, layer, image and blur bounds remain in place. No rendering feature,
reference capture, dependency or acceptance policy changed. The revised stage-3 visual
policy in `docs/svg-renderer-validation.md` still applies; that document now records the
resource limits, exception boundaries and sanitizer procedure.

**Work bounds:** CSS's existing 10-million-unit budget now charges attribute/declaration
bytes and selector/class-list scans, rather than counting rules alone. Dash expansion
charges zero-entry scans, resets and curve subdivision against its million-unit budget,
then contributes that work and parsed array bytes to the canvas budget. Repeated inherited
paint, resource attributes and embedded-image data share a 64 MiB cumulative parsing-byte
budget. Resource inheritance no longer copies unused element text. These close amplification
paths where many small shapes could repeatedly process a large inherited value while staying
under node and raster limits. They are deterministic operation/byte limits, not a wall-clock
promise or a whole-process memory cap.

**Numbers:** extreme arc radius correction and subnormal centre arithmetic could produce
NaN before converting the subdivision count to an integer, despite finite input. Angles are
now checked before that conversion. Pattern wrapping previously subtracted a rounded huge
quotient; cancellation could leave an out-of-range remainder and an undefined integer cast.
It now uses a bounded remainder before indexing. Original probes demonstrated both failures;
no reference renderer was used. Ordinary geometry and FreeType stroke expansion are unchanged.

**Regression coverage:** eight new cases cover CSS amplification, zero-heavy dashes,
repeated inherited paint/dash text, large pattern translations, both arc-overflow mechanisms,
the exact 8 MiB input boundary, warning-callback exceptions and padded-layer aggregate memory.
Four small original fuzz seeds preserve numeric and dash cases. The large resource probes
are generated in tests, not committed as oversized fixtures. Pre-fix CSS/dash/texture and
inherited-text regressions failed as expected; the arc probes produced float-cast sanitizer
diagnostics. Those probes now fail cleanly or retain the expected solid texture.

**Validation:** 60 focused cases / 13,318 assertions pass. Full release passes 484 cases /
140,505 assertions; ASan/UBSan passes 483 cases / 140,490 assertions (the allocator case has
no assertions under ASan). Validation uses the approved copy-only fixture adaptations,
Mesa software rendering and the Nix package's isolation of the allocator-sensitive test.
Both copied ctest suites and `nix build --no-link` pass. Earlier runs reproduced the known
allocator-state assertion and an external NVIDIA EGL leak; neither was suppressed in SVG
code. Only the previously documented unrelated isolate zero-length memcpy UBSan diagnostics
remain in the final full sanitizer log. Final-source clang/libFuzzer with ASan, UBSan and
float-cast-overflow checking completed 11,528,247 inputs in 1801 seconds, peak RSS
843 MiB, exit 0, with no finding, timeout or OOM. UBSan was configured to halt on error.
Preliminary runs were explicitly stopped during the audit and do not count as final evidence.
The final process exited and source/binary hashes remained unchanged.

**Compatibility and performance:** diagnostic pass counts at 16/48/128 are 4346/4710 (92.27%),
4441/4710 (94.29%) and 4525/4710 (96.07%). The filtered subset remains 18/20, 20/20 and 20/20.
All 14,130 per-image comparison results match completed stage 3, with no stale sources or
missing timings. All 102 fixed native-review PNGs are byte-identical to the accepted stage-3
review, retaining its named deviations and the unchanged 818 diagnostic failures. Final,
uncontended parse/render totals are 0.607327, 0.856290 and 2.088467 s, respectively 0.984,
0.952 and 1.137 times the preserved baseline; all three performance gates pass. Local logs, immutable binaries,
hashes and reproduction scripts are under ignored `build-svg-handoff/hardening/`.

**Disposition:** the hardening pass is complete. No known SVG failure remains from this audit;
this is bounded regression/fuzz evidence, not a proof against all possible hostile inputs.
The original brief and third-party corpus remain untracked/ignored.

---

## 2026-11-11 — SVG stage 3: FreeType strokes and native-size acceptance

**Decision:** the author explicitly approved ending the pixel-parity effort after repeated
iterations failed to clear the original acceptance gates. `docs/svg-renderer-validation.md`
records the replacement policy: semantic/geometric tests, a fixed native-size review,
performance, full builds/tests and sanitizer fuzzing. The 98% corpus and all-filtered pixel
pass requirements are retired; the captured references and numeric comparator are unchanged
and remain diagnostic. This supersedes the acceptance conclusions in the earlier entries.

**Implementation:** FreeType now expands every stroke width. Removed the experimental custom
stroker and separate thin-stroke coverage/union path, reducing `raster.cpp` from 1106 to 699
lines. The existing bounded coverage sampler remains. Anchored FreeType's local coordinates
to whole scaled units so an unused move cannot shift its fixed-point rounding phase; move-only
subpaths do not paint caps. The bounds regression now exercises the backend's output-point
limit instead of the removed rectangle-union work limit. Geometric tests for split/retraced
strokes, gradients, caps, tight curves and cubic endpoint coverage remain and pass. A diagonal
stroke now has the same sampled area and interior footprint as its normal-width rectangle.

**Features preserved:** colour matrices, Gaussian blur, patterns, embedded PNG/JPEG through
Wuffs, offscreen effects and once-per-file unsupported-filter warnings remain intact. No new
dependency, copied third-party implementation, resvg invocation or reference recapture.

**Native review:** `tests/data/svg-review.tsv` fixes 34 entries before rendering, including all
20 filtered sources and 14 named examples of complex strokes, prior problems, embedded images,
patterns and aspect-ratio sizing. `scripts/svg_review.py` verifies source hashes and requires
every selected capture; missing captures fail rather than shrinking the set. All 102 pairs
were inspected at native 16/48/128 on light and dark backgrounds and accepted under the new
policy. No missing components, broken masks, displaced images, missing patterns or unusable
silhouettes were found. Named deviations: Desktop's 16 px wave detail is denser/darker;
Firefox/LibreOffice have fine edge and stroke-weight changes; Virustotal retains public rounding
versus reference ceiling. Source-dark symbolic artwork has low contrast on dark backgrounds in
both versions. Exact reference sampling differences remain unresolved, not reclassified as
pixel passes. Local review, per-entry assessment and renderer/selection hashes are preserved in
`build-svg-handoff/freetype-reset/review/`.

**Validation:** 52 focused cases / 13,286 assertions pass. Full release passes 476 cases /
140,085 assertions (two skipped; 9.42 s); ASan/UBSan passes 475 cases / 140,070 assertions
(three skipped; 14.06 s), using the approved copy-only fixture adaptations. Only the previously
documented unrelated isolate zero-length `memcpy` diagnostics remain. `nix build --no-link`
passes. Final-source clang/libFuzzer ASan/UBSan completes 12,389,269 inputs in 1801 seconds,
peak RSS 644 MiB, exit 0, with no finding, timeout or OOM. The fuzzer has exited and source/binary
hashes remain unchanged. Uncontended parse/render totals are 0.603146, 0.854762 and 2.088420 s
at 16/48/128, respectively 0.977, 0.951 and 1.137 times the preserved baseline; all performance
gates pass. No stale sources or missing timings, and final per-image metrics match the frozen
native review exactly.

**Diagnostic corpus:** 4346/4710 (92.27%), 4441/4710 (94.29%) and 4525/4710 (96.07%) at
16/48/128. The filtered subset has 18/20 numeric passes at 16 and 20/20 at 48/128; Shotwell
(max 33, four pixels over 24) and SimpleScan (max 34, three pixels) fail at 16. All twenty
were accepted in the native-size review. Every one of the 818 numeric failures remains listed
in the ignored `remaining-failures.tsv/json`, including the 1/11/7 dimension-contract differences.
No claim of pixel parity is made. Detailed validation, filtered results, assessments and source/
binary hashes are under `build-svg-handoff/freetype-reset/`.

**Disposition:** stage 3 is complete under the explicitly approved revised acceptance policy.
The fixed native review, semantic tests, performance, full builds/tests and final-source fuzzing
all pass. The original user brief and local third-party material remain outside the commit.

---

## 2026-11-10 — SVG stroke invariants repaired; small-size compatibility still open

**Scope:** implemented the two concrete defects from the compatibility audit without changing
references, thresholds or public sizing. Existing stage-3 work is preserved. No resvg invocation,
third-party implementation or new dependency was used.

**Thin strokes:** each flattened segment contributes rectangular coverage fragments within its
neighbouring pixels. Those fragments form a union before the paint is composited once. This
retains the existing centreline approximation while eliminating split seams and repeated opacity
for overlapping/retraced geometry. The pixel-aligned split-line probe now has alpha 255 instead
of 192; overlapping and crossing half-opacity subpaths have alpha 128 instead of 192. Tests
also cover reverse traversal, partial overlap, vertical strokes and gradient paint. Fragment
storage is capped at 262,144; sorting and union work consume the shared 64M work budget. An
original dense-overlap test verifies clean failure rather than unbounded union work.

**Wide strokes:** the apparent full-path assembly failure was actually a spurious inflection
root near a cubic endpoint with coincident controls. Changing the bounds origin changed floating
point cancellation, introducing a tiny subdivision whose normals collapsed. For a cubic with
an endpoint derivative of zero, the curvature numerator has constant sign on the open interval;
the implementation now uses that fact instead of solving the unstable polynomial. Coefficients
also use differences of consecutive controls. An original regression previously changed alpha
from 255 to 48 merely by adding an unused move; both versions now agree throughout the image.
Musescore's audited missing pixel is restored, and its 128 px reference maximum falls from 255
to 23. All six aliases now pass at that size.

**Compatibility:** final results are 4420/4710 (93.84%) at 16 px, 4515/4710 (95.86%) at
48 px and 4630/4710 (98.30%) at 128 px. Against the preceding validated renderer, gains/losses
are 1/50, 4/0 and 11/0. The 50 regressions represent 17 distinct artworks; manual comparison
confirms changed small-icon outline coverage. These are not silently accepted exceptions.
An isolated thin-only build reproduces all 51 changed 16 px results pixel-for-pixel. At 48 px,
two gains come from each fix; all eleven 128 px gains require the cubic correction.
All 17 filtered Adwaita icons still pass; hicolor Shotwell retains its 16 px failure (max 33,
four pixels over 24). The other filtered comparisons pass. Desktop's broad 16 px mismatch
remains unresolved (max 109, 55/256 pixels over 24). The hairline model remains an approximation:
an independent analytic circle-area diagnostic does not show uniformly improved error. Fixing
the demonstrated compositing invariants does not establish exact curved-stroke coverage.
An exact polygon/pixel intersection probe isolates a further pre-existing limitation: a width-1
45-degree butt stroke with length `11*sqrt(2)` should cover 15.5563 pixel areas, but both the old
and new hairline models cover 11.0039 (29.26% short). Its major-axis integration omits the
normal-width geometry. This is a concrete next correctness target, not evidence that it explains
all current corpus failures or that correcting it will improve reference agreement.

**Validation:** 51 focused cases / 13,280 assertions pass. Full release ctest passes
(475 cases / 140,079 assertions, two skipped; 9.44 s), as does ASan/UBSan ctest
(474 cases / 140,064 assertions, three skipped; 14.20 s), using the approved copy-only fixture
adaptations. Only the previously documented unrelated isolate zero-length `memcpy` diagnostics
appear. `nix build --no-link` passes. The final-source clang/libFuzzer ASan/UBSan run completes
9,891,075 inputs in 1801 seconds, peak RSS 684 MiB, exit 0, with no finding, timeout or OOM.
The fuzzer has exited and the recorded source/binary hashes still match. Final corpus timing
with builds and fuzzing stopped is 0.554203, 0.805818 and 2.065165 s at 16/48/128, respectively
0.898, 0.896 and 1.124 times the preserved baseline; all performance gates pass. No stale
sources or missing timings, and every final per-image metric matches the candidate report.

**Disposition:** stage 3 remains incomplete and uncommitted. Reaching the unchanged 98% gate
requires 196 more passes at 16 px and 101 at 48 px, plus the filtered Shotwell comparison.
Ignored evidence lives in `build-svg-handoff/stroke-union/`, including source/binary hashes,
geometric probes, before/after reviews, all remaining failures and explicit filtered results.
The next unresolved problem is the small-size stroke/coverage model, not the two repaired
invariants. No claim is made that the remaining reference target is impossible.

---

## 2026-11-10 — SVG compatibility audit: distinguish correctness from raster agreement

**Scope:** bounded capture/invariant audit after the author asked whether the reference process
makes the target impossible. No production renderer, numeric threshold or reference changed;
resvg was not invoked. Diagnostic scripts and local derivative images remain ignored under
`build-svg-handoff/compatibility-audit/`, with the full conclusion in `report.txt`.

**Capture:** all 14,130 PNG chunk CRCs validate, all 4,710 SVG source hashes match, all recorded
capture statuses are zero, and identical source hashes have byte-identical PNGs at each size.
There are 1,911 distinct artworks. The log has 57 missing-font warnings across 15 corpus sources;
text is out of scope, and these warnings do not establish the cause of non-text differences.
Every dimension failure was verified directly from source dimensions as public rounding versus
reference ceiling: 1/11/7 failures at 16/48/128. These are unavoidable under the current contract,
but too few by themselves to prevent 98%.

**Demonstrated renderer defects:** original pixel-aligned width-1 line probes show that adding
a redundant split at a half-pixel lowers alpha from 255 to 192; retracing a translucent stroke
within the same path raises alpha from 128 to 192. Thin segments are individually composited
instead of forming one painted stroke union. A separate Musescore probe reproduces alpha 0
at (95,83) for the full stroke, versus 255 for the relevant isolated cubic. All 256 geometric
sample points within that pixel lie within the cubic's stroke radius, independently supporting
coverage. The exact full-path assembly mechanism remains to diagnose. These are not grounds
for relaxing a reference tolerance. The latter artwork has six failing aliases at 128 px.

**Reference sensitivity:** area-averaging existing 48 px captures down to 16 passes the same
pixel gate against the existing 16 px captures for 3299/4708 dimension-compatible images
(70.07%); using 128 px captures gives 1664/4696 (35.43%). This is an auxiliary diagnostic,
not an acceptance score: rasterization, filtering and compositing need not commute with
reduction. It explains why increasing geometric precision is not a reliable compatibility fix.
Desktop's reference 128-to-16 reduction differs from its captured 16 px image by maximum 104
and 86/256 pixels over 24. Its broad low-resolution mismatch remains unresolved.

**Disposition:** `classified-failures.json` retains all 531 failing size/source pairs: 19 proven
dimension conflicts, six aliases with demonstrated wide-stroke coverage loss, and 506 unresolved
differences. No unresolved row is declared harmless or blamed on the oracle. At 48 px, eight
distinct failing artworks account for 122 failed source paths; aliases remain in the gate.
`semantic-review.html` and `scale-review.html` show the probes and representative comparisons.

**Decision:** impossibility is not established, nor is an exact pixel oracle a correctness proof.
The next bounded task is to fix stroke union/coverage semantics and full-path coverage loss with
geometric invariants, then rerun the unchanged corpus. Avoid further tolerance sweeps first.
Any later acceptance change should be explicit and tied to named, reviewed exceptions;
there is not yet evidence for a responsibly calibrated replacement numeric threshold.
Stage 3 remains incomplete. The prior production-source validation is unchanged.

---

## 2026-11-10 — SVG stage 3: stroke corrections and validation, acceptance still open

**Implemented:** integrated the preserved original stroke-expansion prototype with independent
corrections. Cubics with coincident endpoint controls use the first nonzero tangent rather than
the chord. Thin strokes retain caps when zero-length segments precede or follow a real segment;
mixed degenerate subpaths retain round/square dots, and move-only contours do not paint. Folded
inner offsets use bounded swept-segment unions: a radius-2 circle with a width-12 stroke no
longer has a transparent centre. Three original regression cases cover these behaviours.
FreeType remains responsible for outline decomposition; its unused stroker allocation is gone.
Existing stage-3 filter, pattern, embedded-image and warning work is preserved.

**Independent assessment:** the preserved prototype baseline reproduced exactly. Correcting the
endpoint tangent reduces Firefox's 16 px mismatch from 19 pixels over 24 to four. Tighter offset
fitting and more precise round caps did not consistently improve agreement with the captured
approximations, so those experiments were not retained. Thin-path coverage accumulation and
paint/endpoint quantization did not resolve the desktop icon's broad translucent-stroke colour
mismatch; its exact mechanism remains unresolved. The inner-offset defect was found through
an original geometric test, not reference tuning. No thresholds, corpus entries or sizing
contracts changed, and resvg was not run.

**Final corpus:** 4469/4710 (94.88%) at 16 px, 4511/4710 (95.77%) at 48 px, and
4619/4710 (98.07%) at 128 px. Parse+render totals are 0.528890, 0.760394 and 1.912007 s,
respectively 0.857, 0.846 and 1.041 times the captured baseline. No stale sources or missing
timings. Against the retained repository rasterizer, gains/losses are 35/1, 38/6 and 135/43.
The candidate therefore still has regressions, including small gaps at tight bends. All 17
filtered Adwaita icons pass at every size. Hicolor Shotwell still fails at 16 px (maximum 33,
four pixels over 24); all 20 filtered icons pass at 48 and 128 px.

**Validation:** focused SVG tests pass (46 cases / 13,240 assertions). Full release ctest passes
(472 cases / 140,056 assertions, two skipped; 9.94 s), as does ASan/UBSan ctest (471 cases /
140,041 assertions, three skipped; 14.23 s), using the approved copy-only package fixture
adaptations and Mesa software driver. The only sanitizer diagnostics in the full suite are the
previously documented unrelated null zero-length memcpy calls in isolate helper/protocol code.
`nix build --no-link` passes. The final-source clang/libFuzzer ASan/UBSan run completes
10,833,628 inputs in 1801 seconds, peak RSS 570 MiB, exit 0, with no finding, timeout or OOM.
The fuzzer has exited. Final timing used an immutable executable after builds and fuzzing ended.

**Still open:** stage 3 is not complete and is not committed. Reaching 98% needs 147 additional
passes at 16 px and 105 at 48 px, plus Shotwell's filtered-subset failure. Local ignored evidence
is in `build-svg-handoff/agent-evidence/`: `final-corpus.json`, complete per-source
`remaining-failures.tsv`, `filtered-icons.json`, `regressions.json`, `review.html`, source/binary
hashes and validation logs. Failure lists distinguish sizing mismatches (1/11/7 by size) from
unresolved rendering mechanisms; they do not claim every exact cause has been established.
The review includes improvements and regressions at native size and enlarged. Original
handoff evidence and before-source snapshots are preserved. No third-party icons or renders
are staged or committed.

---

## 2026-11-10 — SVG renderer stage 2: groups, clipping and masks

**Done:** group/use opacity composites an offscreen layer once; opaque groups render directly.
Clip paths support both coordinate systems, clip-rule, child and clip-path transforms, nested
clips and `use`. Masks support their region and coordinate systems, nested masks, and luminance
or alpha interpretation. Luminance uses straight colour and applies alpha once. Shape bounds
include group descendants and referenced viewports. Layers and reference expansion remain
bounded. Skipping transparent source pixels avoids unnecessary full-canvas blend arithmetic.
Four captured layer fixtures and exact-pixel cases pass, including a referenced SVG's bounding
box (its viewport must only be applied once).

**Stroke hardening:** fuzzing found a tiny circle whose distinct fixed-point controls are still
below FreeType's internal curve tolerance. Manual `EndSubPath` then dereferenced an empty
border. Stroke subpaths now go through `FT_Stroker_ParseOutline`, which checks whether any
edge was actually generated before ending the subpath. The minimized original SVG is a unit
test and seed. The crash replay is clean, as are all existing stroke references. No FreeType
code was copied or patched.

**Validation:** `nix build --no-link` passes. The complete direct release run passes all 453
applicable cases / 140,326 assertions (`--order-by=name`, launched from the repository).
ASan/UBSan ctest passes in 27.06 s. The previously recorded unrelated isolate memcpy UBSan
messages remain. An initial draft-copy build used stale object files because preserved source
timestamps predated the objects; the validation copier now updates changed contents with fresh
timestamps, and affected objects were rebuilt.

**Approved gate exception:** direct release ctest repeatedly fails the existing allocator-state
assertion at `tests/memory_test.cpp:52` (about 20 MiB resident after a freed allocation). The
case passes alone; the full binary passes from the repository directory, while changing only
test order under ctest does not reliably fix it. A single-thread Mesa experiment also triggered
an unrelated drag/scroll timeout and was not adopted. The author approved accepting the Nix
package suite plus the complete direct test run for this stage and documenting the limitation.
No unrelated source or assertions were changed. Existing package fixture adaptations remain
confined to the validation copy.

**Local corpus:** no stale sources or missing timings. At 16 px, 2606/4710 pass (55.33%),
0.588958 s versus 0.617100 s (0.954×); at 48 px, 3372/4710 (71.59%), 1.014378 s versus
0.899150 s (1.128×); at 128 px, 4165/4710 (88.43%), 2.941364 s versus 1.837350 s (1.601×).
All times are parse+render totals. Final acceptance is not met.

**Fuzzing:** an initial clean run was interrupted before 30 minutes. A restart found the tiny
curve crash above. After the fix, clang 21 libFuzzer with ASan/UBSan completed 6,103,254 inputs
in 1801 seconds, with no crash, sanitizer finding, timeout or out-of-memory failure; reported
RSS was 625 MiB under the 1 GiB cap. This final run completed as a background job and exited.

**Next:** stage 3 is a separate draft under `/tmp`, with colour matrices, blur, pattern tiles and
embedded images. Its initial feature tests pass, but the corpus remains far below 98%, mostly
because of small antialiased edge differences. Unsupported-filter logging, further hardening,
final corpus analysis and the stage 3 gates are still outstanding. No tolerance was relaxed.

---

## 2026-11-10 — SVG renderer stage 1: shapes, paint and structure

**Done:** all path commands (including arcs and compact numbers), the basic SVG shapes,
FreeType strokes and bounded dash splitting, transforms, viewports and symbols, CSS cascade
and inheritance, SVG named colours and currentColor, linear/radial gradients and reference
inheritance, and bounded `use` expansion. Geometry, stylesheet matching, paint expansion,
coordinate conversion and pixel work have explicit limits. Namespace-local attributes and
numeric entities work; duplicate XML attributes, non-finite numbers and reference cycles fail
cleanly. No external resources are opened. The existing icon-theme SVG test is unchanged.

**Raster decision:** use FreeType direct AA spans at four times the output resolution, then
average coverage before painting. Native-resolution analytic coverage undercounted a
self-intersecting path (alpha 38 versus reference 160); supersampling makes all 16 stage 1
reference fixtures pass using the approved premultiplied comparison. This is still an open
quality/performance tradeoff for small icons, not a claim that final acceptance is met.

**Validation:** `nix build --no-link`, release ctest (20.10 s) and address/undefined sanitizer
ctest (25.17 s) pass. Direct suites use the approved validation copy and package fixture
adaptations. Both use the package's Mesa software driver. The sanitizer suite nevertheless
reports pre-existing zero-length memcpy/null-pointer UBSan findings in
`src/isolate/src/helper_main.cpp:140` and `src/isolate/src/protocol.cpp:190`; these are outside
SVG scope and remain unchanged. SVG-only sanitizer fuzzing has no findings.

**Local corpus:** no stale sources or missing timings. At 16 px, 2583/4710 pass (54.84%),
0.583552 s versus 0.617100 s (0.946×); at 48 px, 3350/4710 (71.13%), 1.003512 s versus
0.899150 s (1.116×); at 128 px, 4097/4710 (86.99%), 2.901506 s versus 1.837350 s (1.579×).
These are parse+render totals. Group effects, filters, patterns and images are still pending;
small-size edge differences also account for many failures. The corpus tool now reports
failed render timings as missing, rather than assigning an artificial 30 seconds.

**Fuzzing:** the first run found a FreeType stroker crash when a final dash's distinct floating
endpoints collapsed to the same 26.6 coordinate. Degeneracy checks now use converted points,
and zero-width strokes skip the stroker. The minimized input is an original regression seed
and unit test. After the fix, a fresh clang 21 ASan/UBSan libFuzzer run completed 12,601,039
inputs in 1801 seconds, with no crash, timeout, out-of-memory or sanitizer finding. The replay
of the original crash is clean.

**Next:** stage 2 has a separately tested draft for group opacity, clip paths and masks. Stages
2 and 3 have not passed their gates. The final 98% quality and 1.5× speed requirements are not
yet met; thresholds are unchanged.

---

## 2026-11-10 — SVG renderer stage 0: resvg removed

**Done:** `nordend::svg` now owns SVG icon rendering. The initial implementation tokenizes XML
with explicit input, node and depth limits and renders solid rectangles through FreeType's
clipped direct antialiasing spans. It uses premultiplied pixels internally and returns straight
RGBA. Output is limited to 1024 px; converted coordinates and pixel work are bounded. Failures,
including allocation errors, return an empty image. No external resources are opened.
This is deliberately the first piece of stage 1: other geometry and paints are still missing.
`IconTheme::render` delegates to it; its existing SVG test is unchanged and passes. resvg is
removed from CMake, the UI library, the Nix package and the README. There is no Rust replacement.

**Tools/tests:** `nordend-svg-render`, `scripts/svg-corpus.sh`, a reference-comparison helper,
SVG unit tests and `nordend-fuzz-svg` (the original SVG fixtures supply its seeds). The corpus
script reports missing capture without failing and excludes stale source hashes. Third-party
icons and renders remain ignored and uncommitted.

**Approved comparison correction:** the author agreed to compare premultiplied RGBA while
retaining the maximum-channel difference of 64 and the 1% limit for pixels differing by more
than 24. Straight RGB made nearly transparent antialiased edges fail by 255 for a visible
contribution of only 1. For example, the stage 1 shapes draft differs by at most 29 in
premultiplied channels (6/4096 pixels over 24), versus 255 in straight channels. Public output
and exact-pixel tests remain straight-alpha. The comparison has a regression test for this.

**Validation:** `nix build --no-link` passes without resvg. With the author's approval, direct
ctest runs in `/tmp/nordend-svg-validation`, a source copy using exactly the existing package's
MIME, sandbox-probe and sparse-file fixture adjustments. Release ctest passes (20.15 s), as
does the address/undefined sanitizer suite (14.43 s). The Nix package's `NORDEND_WERROR=OFF`
setting is needed for an existing ignored-write-return warning in `src/isolate/src/sandbox.cpp`.
The sanitizer suite also uses the package's software-rendering setup: the first host-driver
run passed all assertions but reported a 183-byte NVIDIA driver leak; no suppression was added.

**Local corpus, the intentionally incomplete renderer:** 0/4710 pass at each size, no stale
sources. Parse+render totals: 16 px 0.148186 s (reference 0.617100 s), 48 px 0.303426 s
(reference 0.899150 s), 128 px 0.162753 s (reference 1.837350 s). These times are not a speed
claim: most features are not rendered yet. The local JSON lists every mismatch and the script
prints the worst 20 for each size.

**Fuzzing:** clang 21, libFuzzer with AddressSanitizer and UBSan, 1801 seconds (30 minutes),
22,872,347 inputs, no crash, timeout, out-of-memory or sanitizer finding. The run's binary
stayed fixed while the stage 1 tests and code were prepared separately under `/tmp`.

**Next:** integrate and complete stage 1. All 16 stage 1 reference fixtures pass in the draft
with the approved comparison; coverage supersampling handles self-intersections. Its first
corpus run still shows mismatches to investigate. Stages 1–3 are not complete.

---

## 2026-11-10 — SVG renderer: reference capture, stage 0 in progress

Work is on `feat/svg-renderer` from `86aa3d3`, with the author's agreement: local `main`
does not yet contain the icon-theme renderer or Nix packaging that the brief assumes.

**Captured once:** `nix shell nixpkgs#resvg --command bash scripts/svg-capture-references.sh`,
resvg 0.49.0, 4,710 installed SVG paths at 16, 48 and 128 px (14,130 PNGs), plus 24 original
hand-written fixtures covering shapes, paths, strokes, CSS, gradients, uses, transforms,
compositing, clipping, masks, filters and patterns. No render failed. The first discovery
attempt stopped before invoking resvg on a broken Candy symlink; discovery now records
unreadable targets separately. The missing target is Candy's `places/16/folder-library.svg`.
The corpus includes WhiteSur cursor SVGs as well as Candy, Adwaita and both hicolor roots.
Third-party renders and their manifest are local and ignored; only original fixtures and
their PNGs belong in Git. The capture refuses to overwrite an existing corpus.

**Baseline:** total CLI wall time (process startup, reading, parsing, rendering and PNG writing)
is 4.648366 s at 16 px, 5.663249 s at 48 px, and 9.608956 s at 128 px. The retained `--perf`
log also gives XML parsing + SVG parsing + rendering totals of 0.617100 s, 0.899150 s and
1.837350 s respectively (each resvg phase is rounded to 0.01 ms). Both measures are in the
manifest, as `seconds` and `render_seconds`, to keep later comparisons on matching work.
Source SHA-256 hashes are recorded. The perf column was extracted from the same capture log;
resvg was not run again. Non-square fixture PNGs retain their aspect ratio.

**Blocked before integration:** the existing build's `nix develop --command ctest --test-dir
build --output-on-failure` fails before any renderer changes are compiled: 422/425 cases pass,
three fail, two skipped, 19.21 s. `column_view_test.cpp:334` cannot resolve the `.md` fixture's
MIME type; `details_view_test.cpp:151` sorts that ambiguous type differently; the transfer
fixture at `transfer_test.cpp:249` throws when creating a 1 PiB sparse file. `nix/package.nix`
already adapts these fixtures in its build copy. They are outside SVG scope. The brief requires
direct ctest to pass at every stage and says to stop on conflicting requirements; the proposal
is to apply the existing Nix fixture adaptations in a separate validation copy, keeping the
application and unrelated source tests unchanged.

**Not complete:** resvg is still linked. Initial API/XML and test scaffolding is unintegrated;
there is no replacement renderer yet. No corpus comparison, sanitizer validation or SVG fuzz
run has been claimed, and no stage has passed its gates.

---

## 2026-11-10 — The Noctalia template ships with Nordend

A user on Noctalia saw no Noctalia colours: `colors.toml` is written by a Noctalia user template
that only existed in the author's own configuration, so on any other Noctalia Nordend fell back
to the portal (where it gets nothing under Noctalia, *to be verified*) or its default palette,
while the README promised the Noctalia theme. The template is now in `data/noctalia/nordend`
(the author's, unchanged), installed to `share/nordend/noctalia/`, and the README's "On
Noctalia" says how to enable it (two steps); README and features no longer promise it without.

**Open:** propose `nordend` as a Noctalia community template (one switch for users, with
`requires_path = "~/.config/nordend"`), with the author's OK. The theme watcher watches
`~/.config/nordend`: when that folder is missing at start, a `colors.toml` written later is seen
only at the next start (hence the `mkdir` in the README).

---

## 2026-11-09 — Nordend 0.5.0

Step 3 (text previews and the engines) closed and released as 0.5.0: 3a (the image helper became
the first engine, with its manifest), 3b-1 (text read in the base engine), 3b-2 (syntax colours
for every language planned, plus ignore files and `.gitattributes`), 3b-3 (selecting and copying),
3b-4 (sideways, page keys, the caption, minified files, logs from their end), all tested by the
author; the long fuzz run of all six targets clean (an hour), a short text run after 3b-4 clean.
CMake version, README (badge, note, install note, Versions, What's next, At a glance: nordend
3.95 MB / 3.28 stripped, the helper 1.13 / 1.01, `readelf -d` NEEDED 17 and 8 as before,
51 300 + 17 400 lines, 420 tests; memory left at 25 MB: the author's log shows 23.5-24 MB
anonymous today, as before) and features say so. The engine protocol is 4: Nordend 0.5.0's
helper and manifest go together, installed by the same package.

Released: tag `v0.5.0` on `0bc58c8` and the GitHub release by the author. Void template on it
(here and in void-noctalia): sha256 `ba1c06d2…bdaffd7` of the tag's archive (fetched twice, the
same); `nordend-0.5.0_1` builds with no error or warning, the same run dependencies as 0.4.0, the
engine's manifest at `/usr/share/nordend/engines/base.toml`.

---

## 2026-11-09 — Text pages: sideways, keys, caption, minified files, logs (ROADMAP 3b-4)

**Tested by the author** (2026-11-09, installed in `~/.local`, the test plan: a 6000-line log,
a minified JavaScript, a Windows-1252 file with CRLF, `browser.cpp`, a photo's Home/End):
"approvato, funziona". Step 3 is done.

**Done**: the last part of step 3, as proposed and agreed (Home/End/PgUp/PgDn and logs' ends
confirmed by the author). **Sideways**: lines are drawn whole (no ellipsis) at the text's left
less a second `ui::Scroller` (`text_sideways_`), clipped to the text's area, the line numbers in
their own clip, so only the text moves; the extent is the widest line in characters (counted once
per text) and one more; Shift+wheel and a touchpad's or tilting wheel's x move it; a thin thumb
under the text; selecting past the text's left or right scrolls that way as past its top or
bottom; the next file starts at the left again. **Keys** (`QuickLook::text_key`, before the view
gets them): Home and End glide to the top and the bottom, Page Up and Page Down by the lines shown
less one; the arrows still walk the folder; on a picture nothing changes. F1 lists them under Quick
Look. **Caption**: `name   C++   UTF-8   CRLF   3449 lines   120 kB` (the language when known,
"(minified)" after it, CRLF only when present, "first N lines" or "last N lines" when cut).
**Minified**: `nordend::text::looks_minified` (lines over `kMinifiedLineBytes` = 500 on average);
`highlight` then names the language and leaves every line plain. **Logs**: `text::Limits::tail`
asks for the file's end; `formats::text::read` then `fstat`s the file (the cage allows `fstat` on
a descriptor) and, when it is larger than `max_bytes`, `read_tail` reads its last `max_bytes`
from the first line starting in them (one long line: from a character's start), `truncated` and
`text::Text::tail` set; the text provider asks so for `text/x-log`; Quick Look opens such a text
at its bottom and draws no line numbers (they would count from where the reading started: the real
ones need the whole file read). The head read and the tail read share `read_at`. The protocol
carries `tail` both ways (the request's new last byte, the response's former `reserved` one),
checked to be 0 or 1: **protocol 4** (`engines::kProtocol`, `base.toml`), 3 having never been
released.

**Tests**: a log's end from a line's start, a file within the limit read whole, one long line cut
at a character; `looks_minified`; a minified JavaScript keeps its language without spans; `tail`
through the request and refused when 2; the helper reads tails the same as this process (inside
its cage, `fstat` included); offscreen, a 4000-line log over 256 KiB opens at its bottom with its
last line, Home, Page Down and End move the page (End within a pixel: a glide stops there), the
keys leave the folder's cursor alone, Shift+wheel scrolls sideways, the next file starts at the
left. 420 tests.

**Checked**: offscreen snapshots in the author's palette of a 915 kB log (newest lines at the
bottom, the sideways thumb, "last 1714 lines"; the first take had line numbers 1672-1714, wrong
for a file of 6000 lines: removed), a minified JavaScript ("JavaScript (minified)", plain), a
Windows-1252 file with CRLF ("Latin-1   CRLF").

**Fuzzing** (short run): `nordend-fuzz-text` with a fourth name, `fuzz.log`, read with
`Limits::tail` (its small limits reach the tail path), 10 minutes, 3 processes at the lowest
priority, the long run's corpus plus two log seeds: 0.30 million inputs, coverage 2599 → 2625
edges, no crash, timeout or out-of-memory.

**Open**: a minified file's single line is cut at 1024 bytes on reading (`max_line_bytes`), so
sideways scrolling ends there.

---

## 2026-11-09 — Selecting and copying text in Quick Look (ROADMAP 3b-3)

**Tested by the author** (2026-11-09, installed in `~/.local`, the test plan on `browser.cpp`:
dragging, scrolling past the page's bottom, double and triple click, Shift+click, Ctrl+A,
Ctrl+C pasted elsewhere, arrows, a click beside the page, Ctrl+C with nothing selected, F1):
"tutti i test passati, funziona".

**Done**: `views::TextSelection` (`text_selection.{hpp,cpp}`, no drawing, no pointer: points as
line and byte) keeps a selection by characters, words or lines: a press (one click a place, two
the word, three the line with its break), a drag reaching out from what the press took by the
same unit (a drag after a double click goes by words), Shift+click extending (from the last
press when nothing is selected), select all, the text joined by `\n`. Words are letters, digits,
`_` and any byte beyond ASCII (so `caffè_1` is one); a run of spaces is one; punctuation goes one
character at a time. Columns and bytes are converted counting UTF-8 characters
(`byte_at_column`, `column_at_byte`): the page is monospace and tabs are expanded on reading.
`QuickLook` keeps the page's layout from the last draw (page, lines area, where text starts, a
character's width) and turns pointer positions into points (half way through a character is
after it; above the lines the first line's start, below the last line's end); a left press on
the page selects and is taken (`pointer` now says so), a press elsewhere still closes Quick Look;
while the button is down above or below the lines the page scrolls, 12 px/s for each pixel
beyond (at most 4000 px/s), as an animation (so it goes on with the pointer still), the selection
following. The selection is drawn behind the text in `secondary_container`, the colour text
fields select with, a character's room for each selected line break. Walking to another file,
closing or reopening drops it. `Browser`: on a text page Ctrl+A selects its text and Ctrl+C puts
it on the clipboard (`text/plain;charset=utf-8`, `UTF8_STRING`, `text/plain`) with a message
("Copied 12 lines", "Copied 7 characters"); Nordend's own file clipboard is emptied then, so
Ctrl+V has nothing to paste; with nothing selected Ctrl+C copies the file as before. F1 lists both
under Quick Look. The snapshot tool now waits after each key for what it loaded, so
`--keys "space,Ctrl+a"` shows the selection.

**Tests**: `tests/text_selection_test.cpp` (columns and bytes with a two-byte `è`; drags over
lines and back past the press; words, spaces, punctuation, lines, dragging by words and lines, the
last line without a break; Shift+click, select all, clear, a point inside a character, no text);
in `quick_look_test.cpp` Ctrl+A/Ctrl+C through the browser with a fake clipboard (the file first
when nothing is selected, then the text; walking drops the selection), and an offscreen-drawn
page: a double click in its middle takes one word and keeps Quick Look open, a drag three lines
down selects across them, a click beside the page closes it.

**Checked**: offscreen snapshot of a C++ file with Ctrl+A, in the default palette (a saturated
blue) and in the author's Noctalia colours (a muted grey-green, the text readable on it): kept,
as text fields use the same colour.

**Fuzzing, the long run for step 3's parsers** (2026-11-09, started once the last lexer was in:
3b-3 and 3b-4 add none; an hour, the six targets side by side, 3 processes each, at the lowest
priority while the author was out): `nordend-fuzz-camera` 1.3 million inputs (coverage 1745 →
1806), `-picture` 2.2 million (4299 → 4310), `-tiff` 31.7 million (1382 → 1383), `-dng` 27.4
million (2891 → 2894), `-ljpeg` 36.0 million (368), `-text` 0.9 million (2539 → 2611); no crash,
timeout or out-of-memory. 416 tests, green in `build`, `build-asan`, `build-tsan` after it.

**Open**: wide characters (CJK, emoji) take two columns on screen but count one, so on such lines
a selection made with the pointer may be one character off (Ctrl+A and Ctrl+C stay exact).
clang-tidy's `bugprone-forward-declaration-namespace` on `isolate/helpers.hpp` (`Request`), seen
through `snapshot.cpp`, predates this step.

---

## 2026-11-09 — Markdown and diffs (ROADMAP 3b-2b, fourth group)

**Tested by the author** (2026-11-09, installed in `~/.local`, the test plan: this `README.md`,
`docs/devlog.md`, a `git format-patch` of the last commit): "ok funziona". 3b-2 is done.

**Done**: the last two languages of 3b-2, each a line lexer of its own on `Dedicated`.
**Markdown**, highlighted and not rendered: headings (`#` to `######` and a space) as keywords,
`===` and `---` as keywords when they underline a paragraph's line (the lexer's state keeps
`after_text`), else `---`/`***`/`___` rules as comments; `>` quotes and `<!-- -->` comments (over
lines) as comments; list markers (`-`, `*`, `+`, `1.`, `2)`) and task boxes (`[ ]`, `[x]`) as
numbers, at any indentation; emphasis `*a*`, `**a**`, `***a***`, `_a_` (not inside words:
`snake_case`), `~~a~~` as types; code spans of any number of backticks as strings; links'
addresses (`[text](url)`, `[label]: url`, `<https://…>`, `<a@b>`, bare `http(s)://` stopping at
quotes and brackets, trailing punctuation left out as GitHub does) as functions; `\` escapes.
Each kind of opener that found no closer is not looked for again past where it failed, so a line
of a thousand `*` stays linear. **Fenced code** (```` ``` ```` or `~~~`, three or more, closed by
a fence as long; at any indentation, so in lists too) is coloured in the language its fence names
(`cpp`, `rust`, `py`, `sh`, `json`, `yaml`, `diff`… a `fence_names` list per language, any case,
`{.python}` too), the language's own state carried across the block's lines in an inner `State`;
an unknown or missing name leaves the block a string. Markdown has no fence names, so fences
never nest (the recursion is one level). **Front matter**: `---` (YAML) or `+++` (TOML) on the
first line opens a block in that language up to the same line (`...` too for YAML). **Diffs**
(`text/x-patch`, `text/x-diff`, `text/x-reject`; `.diff .patch .rej`): `+` lines `added`, `-`
lines `removed`, `@@ -a,b +c,d @@` as a function with its counts kept so that inside a hunk
`--- x` is a removed line and after it a header again; headers (`diff`, `index`, `---`, `+++`,
modes, renames, copies, `Binary files`, `Only in`) as keywords; `\ No newline at end of file` as
a comment; combined diffs' `@@@` coloured, not counted. Git's patch mails: `From <hash>` starts a
mail (each of a series), `From:`/`Subject:`/`Date:`/`To:`/`Cc:` as keys, the message's `-` and
`+` lines left alone up to the first header or hunk, `---` before the files' list a keyword, the
`-- ` signature plain. Without a mail, `+`/`-` lines outside hunks are changes (fragments,
Markdown's diff fences).

**Tests**: eight new cases (detection; Markdown blocks, underlines, lists and tasks, link
definitions; inline emphasis, look-alikes, code spans, links and URLs, comments over lines;
fences in C++, an unknown language, `~~~ {.python}`, in a list, a language's state across lines,
a fence never closed; front matter in YAML and TOML, only on the first line; diffs with headers
and hunks, `---` inside and after a hunk, fragments without headers; patch mails, a series).
410 tests.

**Checked**: offscreen snapshots of this `README.md` (HTML attributes' URLs stopped at the
quote, a slip found there and fixed), a `git format-patch` of the last commit, and a Markdown
page with front matter, `fish`/`toml`/`diff` fences (the `diff` one first showed no colours: the
mail rule applied to every file; now only to mails). Tests green in `build`, `build-asan`,
`build-tsan`; clang-tidy clean (the one-level recursion marked as such).

**Fuzzing** (short run): `nordend-fuzz-text` with sixteen types (`text/markdown` and
`text/x-patch` added; the ignore files' seeds moved to their new first byte), 10 minutes, 3
processes at the lowest priority, the previous corpus plus six seeds (this README, the Markdown
page above, the patch, Markdown and diff edge cases): 0.37 million inputs, coverage 2385 → 2547
edges, no crash, timeout or out-of-memory. The fuzzer was built before clang-tidy's last
rewrites (De Morgan, parentheses, a nested conditional), which change no behaviour.

**Measured** (`docs/benchmarks.md`): `docs/devlog.md` (308 kB, 4730 lines) 1302 µs, `README.md`
37 µs, a 1137-line patch 23 µs.

**Checked with the author**: the `added` green, pale in the snapshots' default palette (its
lightness is the accent's, chosen in 3b-2a), reads well in his Noctalia palette: kept.

---

## 2026-11-09 — JSON, TOML, YAML, INI, ignore files (ROADMAP 3b-2b, third group)

**Tested by the author** (2026-11-09, installed in `~/.local`, the two test plans: this
repository's YAML, TOML, desktop entry and `.gitignore`, `/usr/share/applications`,
`.gitconfig`, a JSON with comments, void-noctalia's `.gitignore`): "funziona tutto perfetto",
then "ok perfetto" for the ignore files.

**Done**: four languages for data and configuration files, the same colours in all of them:
keys as keywords, section headers as types, strings, numbers and constants, comments; in YAML
anchors and aliases as functions, tags as types, `---`/`...` as keywords. In `Syntax`:
`Keys::json` (a string, or JSON5's bare name, before `:`, where a key may start: line start,
after `{`, a comma or a block comment), `Keys::toml` (bare, quoted and dotted keys before `=`,
inline tables too; `[table]` and `[[array]]` alone on a line, `[1, 2]` in a value is none),
`Keys::ini` (`key=` and `key:` at a line's start, `Name[it]=`, keys with spaces; `[section]`;
`;` and `#` comments only on their own line, so `Exec=x # y` keeps its `#`), `literal_quotes`
(TOML's `'...'` and `'''...'''` without escapes), `dates` (`1979-05-27T07:32:00-08:00` one
number). JSON reads JSONC and JSON5 too (comments, single quotes, names, `NaN`/`Infinity`):
plain JSON has none of them, so one syntax serves all three. YAML has its own line lexer
(`Dedicated::yaml`): keys before `: ` (also after `- `, quoted, inside flows), `#` a comment only
at a line's start or after a space (`http://x/#top` is a value), plain values coloured only when
a number, a date or a constant (YAML 1.1's `yes`/`off` included, `~`), `'it''s'` and `"\""`
quoted strings going on over lines, block scalars `|`/`>` with their indicators: the lines after
are a string while indented more than their key (under a list item, the key's column, not the
dash's), blank lines included; `%YAML` directives. Detection: `application/json` (and `json5`,
`geo+json`, `ld+json`, `schema+json`, `x-ipynb+json`, `manifest+json`), `application/toml`,
`application/yaml`, `application/x-desktop`, `text/x-dbus-service`; `.jsonc .json5 .jsonl
.geojson .webmanifest .ipynb .toml .yaml .yml .ini .desktop .directory .flatpakref
.flatpakrepo` and systemd units (`.service .socket .timer .mount .automount .path .target
.slice .network .netdev`); names `flake.lock`, `deno.lock`, `Cargo.lock`, `Pipfile`,
`poetry.lock`, `uv.lock`, `.clang-format`, `.clang-tidy`, `.clangd`, `.yamllint`,
`.editorconfig`, `.gitconfig`, `.gitmodules`, `mimeapps.list`. Not `.conf` nor `.cfg`: too many
languages behind them (agreed with the author).

Then, asked by the author once the four worked: ignore files, by name (`.gitignore`,
`.dockerignore`, `.ignore`, `.rgignore`, `.fdignore`, `.npmignore`, `.prettierignore`,
`.eslintignore`, `.stignore`; all `text/plain` here), and `.gitattributes`, each a line lexer of
its own (`Dedicated::ignore`, `Dedicated::attributes`, no state between lines): `#` comments
at a line's start only (`\#` is a name), a leading `!` as a keyword, wildcards `*`, `**`, `?`,
`[a-z]` (with `[!a]` and `[]x]`) as functions, a leading `/` (from the root) and a trailing one
(folders only) as types, `\` escaping, trailing spaces not counted; in `.gitattributes` the
pattern (quoted when it holds spaces) then attributes as keys (`text`, `-diff`, `!merge`) with
values after `=` as strings. `.git/info/exclude` is left out: its name alone says nothing.

**Tests**: six new cases in `tests/syntax_test.cpp` (detection, `.conf`/`.cfg` left plain;
JSON keys with escaped quotes, JSONC/JSON5; TOML headers, dotted and quoted keys, inline
tables, dates, literal and triple-quoted strings; YAML keys, URLs, quoted keys, anchors, tags,
flows, lists; block scalars, their end, a `|` inside a value, quoted strings over lines; INI
desktop entries, `.gitconfig`, systemd lines), and one for ignore files and `.gitattributes`.
Failing checks now print the marked lines. 403 tests (the README said 395 after the second
group: it was 396). The fuzz target picks a file name too (`fuzz`, `.gitignore`,
`.gitattributes`) from its first byte, for the languages found by name only.

**Checked**: offscreen snapshots of a CI workflow (YAML), `docs/config.example.toml`,
`data/nordend.desktop`, Rust's `target-spec-json-schema.json`, the `.gitignore` of this
repository and of void-noctalia, a `.gitattributes` with LFS lines. Tests green in `build`,
`build-asan`, `build-tsan`; clang-tidy clean.

**Fuzzing** (short run): `nordend-fuzz-text` with fourteen types (the four new ones added), 10
minutes, 3 processes at the lowest priority, the previous corpus plus seven seeds (the YAML
workflow, `config.example.toml`, the JSON schema, a desktop entry, a D-Bus service, JSON5, TOML
and YAML edge cases): 0.64 million inputs, coverage 1864 → 2003 edges, no crash, timeout or
out-of-memory. A second run once ignore files came, the same way, with the file name picked too
and three more seeds (this `.gitignore`, ignore edge cases, a `.gitattributes`): 0.60 million
inputs, coverage 2138 → 2160 edges, no crash, timeout or out-of-memory.

**Measured** (`docs/benchmarks.md`): `compile_commands.json` (311 kB) 258 µs, 3300 lines of YAML
194 µs, 3220 of TOML 130 µs; `browser.cpp` 685 µs (662 before, within the noise).

---

## 2026-11-08 — Shell, fish, CMake, Makefiles (ROADMAP 3b-2b, second group)

**Tested by the author** (2026-11-08, installed in `~/.local`: a Makefile, a script found by its
`#!`, the Void `template`, his `config.fish`, this `CMakeLists.txt`): "funziona tutto".

**Done**: four languages and what they need in `Syntax`: `#` comments only where a word may
start (`a#b` and `$#` are no comments in shell); `$` variables written three ways
(`Variables::shell`: `$name`, `${…}`, `$1`, `$@ $# $?…`, `$(…)` lexed as code; `make`:
`$(NAME)`/`${NAME}`, `$@ $< $^…`, `$(shell …)` naming a function; `cmake`: `${…}` nested,
`$ENV{…}`, `$CACHE{…}`, `$<generator:expressions>`); heredocs (`<<EOF`, `<<-EOF` with tabs
before the closing word, `<<'EOF'`, `<< "EOF"`; `<<<` and `1 << 2` are not), carried in the
lexer's state from the line after the operator to the closing word alone on its line; a list of
builtins coloured as functions (`echo`, `printf`, `set`…); shell assignments `NAME=value` where
a command starts (line start, after `; | & ( {`, after `export`/`local`/`readonly`/`declare`/
`typeset`: `echo a=b` stays an argument; a second assignment on one command is not told apart);
Makefile lines (`target:` a function, `NAME =`/`:=`/`?=`/`+=`/`!=`/`::=` a variable, recipes
lexed as code); Lua's long brackets generalised (`long_comment` "--" or "#", `long_strings`) so
CMake gets `#[[ ]]` comments and `[[ ]]` arguments. Detection: `application/x-shellscript`,
`application/x-fishscript`, `text/x-cmake`, `text/x-makefile` (all `text/plain` subtypes here);
`.sh .bash .zsh .ksh .fish .cmake .mk .mak`; names `.bashrc`, `.profile`, `.zshrc`, `.envrc`,
`PKGBUILD`, `APKBUILD`, xbps-src's `template`, `CMakeLists.txt`, `Makefile`, `GNUmakefile`;
interpreters `sh bash dash zsh ksh mksh ash busybox fish make`.

**Tests**: `tests/syntax_test.cpp` grew to 23 cases (detection; shell words, variables,
comments in words, strings over lines, assignments; heredocs and their look-alikes; fish; CMake
commands, variables, generator expressions, bracket comments and arguments; Makefile targets,
variables, functions, directives). 395 tests.

**Checked**: offscreen snapshots of void-noctalia's `build-package.sh` and this repository's
`CMakeLists.txt`; dumps of the Void `template` and the author's `config.fish`. A `then` that
looked half coloured in the shrunken snapshot was whole at full size.
Tests green in `build`, `build-asan`, `build-tsan`.

**Fuzzing** (short run): `nordend-fuzz-text` with ten types (the four new ones added), 10
minutes, 3 processes at the lowest priority, the previous corpus plus six seeds (a build script,
heredocs and expansions, `config.fish`, this `CMakeLists.txt`, CMake brackets and generator
expressions, a Makefile): 0.8 million inputs, coverage 1211 → 1550 edges, no crash, timeout or
out-of-memory.

**Measured**: colouring `browser.cpp` 662 µs (622 µs before this group: the checks for long
comments, variables and heredocs in every step).

---

## 2026-11-08 — Rust, Python, JavaScript/TypeScript, Lua/Luau (ROADMAP 3b-2b, first group)

**Tested by the author** (2026-11-08, installed in `~/.local`, on his own files: a Rust crate,
a Python script, a Svelte project's `vite.config.ts` and `eslint.config.js`, swayimg's
`init.lua`): "tutto perfetto".

**Done**: the languages' tables moved out of the lexer into `formats/text/src/languages.{hpp,cpp}`
(word lists kept out of clang-format); the lexer grew what these languages need, each a field of
`Syntax`: nested block comments (Rust), Lua's long brackets (`--[==[ ]==]` comments, `[[ ]]`
strings, any number of `=`), strings that go on over lines (Rust's, JavaScript's template
literals; a backslash at a line's end in any string with escapes), Python's triple quotes,
string prefixes as a list (C++ `u8 u U L`, Rust `b c`, Python's `r b f u t` and their
combinations in any case, those with an `r` raw), Rust raw strings `r#"…"#` with any number of
`#`, decorators (`@name.attr`, Python and TypeScript), Rust attributes `#[…]`/`#![…]` and
`name!` macros (not `!=`), Rust lifetimes (`'a`, `'static`) told from characters (`'x'`, `'é'`,
`'\''`), ranges (`0..10`) ending a number, capitals as constants (Python, Rust, JS, Lua) or
macros (C). Language detection gained extensions, tried after the MIME type and file names:
`.ts` is Qt Linguist and `.tsx` Tiled for shared-mime-info, `.jsx` and `.luau` plain text; a
`.ts` whose first line starts with `<` stays XML. MIME types as this system names them:
`text/rust`, `text/x-python3`/`text/x-python`, `text/javascript`, `text/x-lua`. Interpreters:
`python`, `pypy`, `node`, `deno`, `bun`, `lua`, `luajit`, `luau`, with a version after the name
(`python3.12`, `lua5.4`).

**Tests**: `tests/syntax_test.cpp` grew to 17 cases: detection by extension and interpreter
(the Qt Linguist and Tiled cases, `env -S`, versions, a hidden `.rs`), and one case per language
(Rust: macros, attributes, lifetimes against characters, raw/byte strings, strings over lines,
nested comments, ranges; Python: decorators, every prefix, triple quotes, an unclosed string
ending with its line; JS/TS: templates over lines, types, decorators; Lua/Luau: long comments
and strings with levels, `nil`, Luau types). 389 tests.

**Checked**: offscreen snapshots of a Rust demo and of `tests/fuzz/make_dng_seeds.py` (a real
Python file: its docstring, constants in capitals, calls, the shebang as a comment).
Tests green in `build`, `build-asan`, `build-tsan`.

**Fuzzing** (short run): `nordend-fuzz-text`, the first byte now choosing among plain text, C++,
Rust, Python, JavaScript and Lua; 10 minutes, 3 processes at the lowest priority, 3b-2a's corpus
plus six seeds for the new languages: 1.15 million inputs, coverage 963 → 1211 edges, no crash,
timeout or out-of-memory.

**Measured**: the general lexer first coloured `browser.cpp` in 1073 µs against 780 µs for
3b-2a's (both rebuilt the same way, measured side by side); a 256-entry table of quotes instead
of `quotes.find()` after every word (−90 µs) and trying words first in each step (nothing that
starts a comment starts with a letter) took it to 622 µs, quicker than before. Quick Look on
`browser.cpp` through the helper: 1.44 ms (1.62 at 3b-2a).

---

## 2026-11-08 — Syntax colours, C and C++ first (ROADMAP 3b-2a)

**Tested by the author** (2026-11-08, installed in `~/.local`): C and C++ files coloured in
Quick Look and the info pane with Noctalia's colours, plain text as before: "va tutto bene".

**Decided with the author** (2026-11-08): eight styles (keyword, type, string, number/constant,
comment, function/macro, and a diff's added and removed lines), their colours made from the
palette, readable on the page (WCAG AA); colours in the info pane's lines too; no colours in
`config.toml` yet. 3b-2 split in two: 3b-2a the whole path with C and C++ only, 3b-2b the other
languages.

**Done**: `text::Style`, `text::Span` (start, length, style), `Text::language` and
`Text::spans` (one list per line, none without a language). In the text family, `syntax.{hpp,cpp}`:
`language_for(name, mime, first_line)` (MIME, then file names, then the interpreter of a `#!`
line, `env` and its options skipped, a version after the name allowed), `highlight`, `colour`;
one lexer driven by a table per language (line and block comments, quotes and escapes, C++ raw
strings `R"delim(…)delim"` with their prefixes, `u8""`/`L''`, digit separators, hexadecimal
floats, the preprocessor's directives with `<header>` as a string, keywords/types/constants,
`_t` names and CamelCase as types, capitals as macros, a name before `(` as a function), with
a state carried between lines for block comments and raw strings. C (`text/x-csrc`,
`text/x-chdr`) and C++ (`text/x-c++src`, `text/x-c++hdr`) share it. Protocol 3: the text answer
carries the language's name and the spans after the lines in the memfd (three 32-bit numbers
each); Nordend checks every span (inside its line, after the previous one, a known style)
before taking them. `ui::Renderer::draw_text_runs`: one shaping per line, each glyph coloured
by the run holding its byte (HarfBuzz's cluster). `theme::syntax_colors(palette)`: keyword
primary, type tertiary, string secondary, comment on_surface_variant, removed error, number
between primary and error, function between secondary and tertiary, added a green at the
primary's lightness (OKLCH), each moved in lightness until it reads on `surface_container`.
Quick Look and the info pane draw with them. The fuzz target's first byte picks the language
too.

**Tests**: `tests/syntax_test.cpp` (8 cases: languages, C++ words, numbers, strings, raw strings
over lines, comments over lines, the preprocessor, odd lines); `syntax_colors_test.cpp` (4: one
colour per style, Noctalia's colours kept as they are and apart, readable on every desktop
palette dark and light, a palette whose colours do not read mended);
`renderer_runs_test.cpp` (pixels: "MMMM" white, "éééé" red, by bytes); `helpers_test.cpp`: the
helper colours a C++ file as this process does, a payload travels whole and spans out of place
are refused (past the line, overlapping, out of order, unknown style, empty). 384 tests.

**Checked**: offscreen snapshots of a C++ demo file in Quick Look (desktop palette dark and
light) and the info pane's lines: every style as meant; the lexer's own source in Quick Look.
Tests green in `build`, `build-asan`, `build-tsan`.

**Fuzzing** (short run after the new lexer): `nordend-fuzz-text`, 10 minutes, 3 processes at the
lowest priority, the 3b-1 corpus plus three C/C++ seeds (the lexer's source, its tests, raw
strings and directives): 2.4 million inputs, coverage 278 → 963 edges, no crash, timeout or
out-of-memory.

**Measured** (`docs/benchmarks.md`): `browser.cpp` (131 KiB, 3449 lines) in Quick Look through
the helper 1.62 ms (0.81 ms without colours; colouring alone 0.76 ms in the process). Two fixes
on the way: a 256-entry table of the characters that may open something and one hash table of
words per language took colouring from 1.78 to 0.76 ms; `Helpers::load_text` returned its
result through a conditional expression, which copied every line: now moved (−120 µs without
colours, −540 µs with).

---

## 2026-11-08 — Text read by the base engine (ROADMAP 3b-1)

**Tested by the author** (2026-11-08, installed in `~/.local`, the old `image.toml` removed):
only `base.toml`, `--about` without warnings; Windows-1252 and UTF-16 files read as written, a
binary file gets the card, `browser.cpp` as before; photos, thumbnails and EXIF as before.

**Decided with the author** (2026-11-08): the image engine becomes the **base** engine
(`data/engines/base.toml`, the same `nordend-preview`), with a second family, `text`
(`shows = "text"`, `text/plain` and its subtypes). Bytes that are not UTF-8 are read as
Windows-1252, the superset of Latin-1 that browsers read "Latin-1" as (the author asked whether
the name clashed with Nordend's philosophy: no, it is a 32-entry table of our own, and Nordend
calls it "Latin-1").

**Done**: Nordend no longer reads text files. A new module `text/` (the `Text` type, `Limits`,
`Loaded` with `none`/`binary`/`unreadable`/`unavailable`, the loader, `encoding_name`), like
`picture/` for images; the text family in `formats/text/` (helper only): `decode` finds the
encoding from the bytes (UTF-8 BOM dropped; UTF-16 LE/BE after their BOM, surrogate pairs joined,
a lone one U+FFFD; valid UTF-8 kept, strict: no overlong forms, surrogates or code points past
U+10FFFF, a character cut at the end U+FFFD or, when the file goes on, left for later; anything
else Windows-1252 as the WHATWG Encoding Standard reads it), NUL means binary, then lines as
before (tabs, long lines cut on a character boundary, the last incomplete line left out) plus
`max_lines` and whether a line ended in CRLF. Protocol 2 (`engines::kProtocol`): a text request
(limits, the file's name and MIME type for 3b-2's languages, never its path) answered with the
lines joined by `\n` in a sealed memfd; Nordend checks the count, every line's length and the
seals before taking them; the helper refuses limits of zero or past 16 MiB / 64 KiB / tab 16.
`preview::read_text` is gone: Quick Look and the info pane (`summarize`, now with the MIME type)
ask `text::load_text`; `isolate::use_engines` sets the text loader to the first engine with a
text family (one set of helpers per engine). Without a text engine, text files get the
information card. A fuzz target, `nordend-fuzz-text`.

**Tests**: `tests/text_test.cpp` (8 cases: lines and tabs, CRLF, UTF-8 with its cuts,
Windows-1252 byte for byte from 0x80 to 0xFF against the WHATWG table, what UTF-8 never holds,
UTF-16 both ways with surrogates and cuts, binary, limits, files, no loader);
`helpers_test.cpp`: the helper reads the same texts as this process (eight files, three sets of
limits), a text request travels whole and too much is refused, a helper lying about a text (a
line short) is let go of; the engines' test now checks the base manifest's two families (text:
`text/x-csrc`, Python, JSON, TOML, Markdown, SVG, shell scripts). 370 tests.

**Checked**: offscreen snapshots: a Windows-1252 file with CRLF ("Il caffè costa 3€", “quotes”)
in Quick Look, a UTF-16 file's lines in the info pane (before: the card), a source file as
before. Tests green in `build`, `build-asan`, `build-tsan`.

**Fuzzing** (short run after a new parser): `nordend-fuzz-text`, 10 minutes, 3 processes at the
lowest priority, 8 seeds (UTF-8, Windows-1252, UTF-16 both ways, a BOM, cuts, binary, a source
file): 11 million inputs, coverage flat at 278 edges after the first minutes, no crash, timeout
or out-of-memory.

**Measured** (`docs/benchmarks.md`): reading `browser.cpp` (131 KiB, 3449 lines) in this
process 545 µs (517 before, in Nordend), through the helper 810 µs; the info pane's 40 lines 45
µs through the helper. An ASCII fast path (8 bytes at a time) in the UTF-8 check and `memchr`
for NUL took the in-process time from 678 to 545 µs.

---

## 2026-11-08 — Engines and their manifests (ROADMAP 3a)

**Tested by the author** (2026-11-08, installed in `~/.local`): the manifest installed and found
by `--about`; protocol 2 refused with its line and "none in …"; restored and found again;
thumbnails, Quick Look on raw files and the info pane's EXIF as before; text as before.

**Decided with the author** (2026-11-08): step 3 starts by making the image helper the first
*engine*, described by a manifest on disk rather than a list compiled into Nordend, so that an
engine packaged apart (step 8, Void subpackages) adds itself by installing a manifest. MIME types
match exactly unless a family asks for its subtypes (`subclasses = true`, for text). No cage field
yet: today the engine builds its own cage and a manifest could neither widen nor check it; it
comes with the Luau plugins (8a), where Nordend applies the cage. Text stays where it is until
3b-1.

**Done**: a new module `engines/` (no file parser): `parse_manifest` (strict: an unknown key, a
value of the wrong type, another protocol, a program given as a path, a type listed twice: the
manifest is refused, with its file and line), `Registry` (`load` reads every `*.toml` of a
directory in name order, skipping with a warning a broken manifest, a missing program, or an
engine showing a type an earlier one shows; `find(mime, type)`; `engine_of`), `default_directory`
(`engines/` next to the program in the build tree, else `../share/nordend/engines/`),
`locate_program` (next to the program, else `../libexec/nordend/`; a file name only) and the
registry in use (`set_installed`/`installed`). `data/engines/image.toml` lists the 30 types of
the old `picture::may_show`, which is gone: Quick Look and the info pane (`preview::provider_for`)
and the grid's thumbnails ask the registry (`thumbnails = true`). `isolate::use_engines` replaces
`default_program`: the helpers start the program the manifest names, and the photo information
goes to an engine with `info = true`. `nordend --about` lists the engines and their programs
("Engines: image (/usr/libexec/nordend/nordend-preview)", or "none in DIR"). CMake copies the
manifests next to `nordend` in the build tree and installs them. The tests load the shipped
manifest from the source tree (`tests/main.cpp`).

**Tests**: `tests/engines_test.cpp`, 7 cases: the shipped manifest shows exactly the 30 types of
before (and not SVG, text, PDF), a full manifest read, 18 ways a manifest is refused (with the
line), exact types against subtypes (an exact type wins over a family of subtypes), first engine
wins, a directory with good, broken and uninstalled engines, `locate_program` refusing paths.
361 tests.

**Checked**: offscreen snapshots before and after (`nordend-snapshot`, a folder with PNG, JPEG,
WebP, JXL, TIFF, a DNG and a RAF): the grid's thumbnails, Quick Look on the DNG and the info
pane's EXIF identical (one glyph of the free space in the status bar differs, `/tmp` changing).
Without any manifest, and with one speaking protocol 2: Quick Look shows the information card,
`--about` says why, Nordend carries on. Installed in a scratch prefix: the manifest in
`share/nordend/engines/`, found by `--about`; the Void template needs no change.

**Measured**: reading the manifests (`Registry::load` on the build tree's, one engine, warm
cache, 200 runs): median 54 µs.

**Open**: when two engines make pictures, `picture::load_picture` names no type to choose by, so
the first is used (logged); to settle when a second picture engine exists (8c).

---

## 2026-11-08 — Nordend 0.4.0

Step 2 (image previews) closed and released as 0.4.0, the author's choice ("tanto ci lavoriamo
molto bene fino alla versione 1"): CR3 and PEF previews and more DNGs (Pentax, an editor's
export) come in a 0.4.x when samples arrive. CMake version, README (badge, note, Versions, What's
next, At a glance: nordend 3.85 MB / 3.20 stripped, the helper 0.97 / 0.87, 46 600 + 15 700
lines, 355 tests) and features say so. The README's helper size had been written in decimal MB
at 2e-5 (1.02 / 0.91): now in MiB like the rest.

The site is taken down until its SvelteKit version (the author, the same day: "lasciamo solo
readme e basta"): `site/`, the Pages workflow and `tests/site_test.cpp` removed, GitHub Pages
switched off, the repository's homepage cleared; the README points to the repository's own
documents (features, benchmarks, the example config, F1 for the shortcuts); the Void template's
homepage is the GitHub repository.

Released: tag `v0.4.0` on `450edb1` and the GitHub release by the author. Void template on it
(here and in void-noctalia, which also dropped `exiv2-devel` and the site as homepage):
`nordend-0.4.0_1` builds clean, its run dependencies without libexiv2.

---

## 2026-11-08 — exiv2 out of Nordend (ROADMAP 2e-5)

**Tested by the author** (2026-11-08, installed build, fresh cache): "funziona tutto".

**Done**: exiv2 no longer a dependency: out of the top CMakeLists (the tests find it quietly,
for the reference comparison and `nordend-bench-previews` only), out of `formats/image`'s link,
`FdIo` (exiv2's input adapter) removed from `fd_file`; the Void template and the build
instructions (README, site) no longer ask for `exiv2-devel`.

**Measured** (`docs/benchmarks.md`): the preview helper `nordend-preview` 9 -> 8 direct
libraries, 21 -> 17 loaded (`libexiv2` 4.1 MB, `libexpat`, `libinih`, `libINIReader` gone;
brotli stays, libjxl's); after its warm-up RSS 10.3 -> 7.8 MB, PSS 5.8 -> 3.4 MB, anonymous 1.2
-> 0.65 MB (each helper; up to 8 run); from spawn to its cage closed 4.4 -> 3.3 ms (median of
40). Binary size unchanged (exiv2 was a shared library). Tests green in `build`, `build-asan`,
`build-tsan` (356).

**Fuzzing, the long run closing 2e** (2026-11-08, an hour, the five targets side by side, 3
processes each, at the lowest priority while the author played): `nordend-fuzz-camera` 1.6
million inputs, `-picture` 3.2 million, `-tiff` 37.2 million, `-dng` 31.9 million, `-ljpeg` 41.9
million; no crash, timeout or out-of-memory.

**Open**: the cage could now refuse mapping files (only exiv2 asked for it): to be proposed.

---

## 2026-11-08 — No more sidecar ratings (ROADMAP 2e-4)

**Tested by the author** (2026-11-08): EXIF as before, no stars row; an ORF renamed and the
rename undone, its sidecar following.

**Decision** (the author, 2026-11-08, in place of reading `xmp:Rating` ourselves): Nordend
shows its own ratings only (ROADMAP 5, in extended attributes); editors' sidecars (darktable,
Lightroom, Capture One...) become a plugin (8c). So the stars from `FILE.xmp` leave the info
pane until step 5 (the author's sidecars in "piramide vincent" all said 0). Moving, renaming and
trashing a photo's darktable sidecars along with it stays: that is file handling, not reading.

**Done**: `read_photo_info(fd)` and `formats::photo_info(fd)` without a sidecar; Nordend no
longer opens `FILE.xmp`, the helper gets one descriptor for a photo's information; the warm-up
no longer reads a sidecar (`shot.jpg.xmp` out of it); `PhotoInfo::rating` kept, empty, for step
5. exiv2 is no longer called by Nordend's code: only linked, and its input adapter (`FdIo`)
unused, until 2e-5.

---

## 2026-11-08 — TIFFs whose data cannot fill them refused at once (2c follow-up)

**Found** by the 10-minute fuzz after 2e-3 (`nordend-fuzz-picture`, 2 timeouts, both real in the
release build): 3 kB TIFFs of one uncompressed strip with no byte count, claiming 172 MP and
151 MP; the decoder filled the missing data with zeros: 2.9 s and 0.9 s of work, 170 MB
allocated. The same kind of work amplification closed in the DNG reader at 2d-3.

**Done**: before allocating, the strips' or tiles' bytes present in the file must be able to
fill the picture: uncompressed at least half (a file cut short still shows its top, as before),
compressed at most the scheme's largest expansion (PackBits 64, Deflate 1100, LZW 2800, JPEG
8192). Both files kept as `tests/data/image/tiff/broken-huge-none{,-2}.tif` (refused, under 200
ms). The 13 real TIFFs of the author's folder decode as before.

---

## 2026-11-08 — The info pane's EXIF read by Nordend (ROADMAP 2e-3)

**Done**
- `src/formats/image/src/camera/info.{hpp,cpp}`: the info pane's fields without exiv2, each
  written as exiv2 0.28 printed it (its printing rules read in its source, the code ours):
  ExposureTime ("1/250 s" when it reduces to a fraction of 1, else the seconds as C++ prints a
  float: "2.5 s"; "(a/b)" for zero or a signed type), FNumber ("F" and two significant digits),
  ISO (first value), FocalLength (one decimal, "mm"), ExposureBias (reduced, signed, "0 EV"),
  the program, metering, flash and white balance tables ("(n)" for a value not in them), date,
  make and model (`camera_of` as before), software and lens as text (no lens database).
  Where they are looked for, as exiv2 merged them: the raw's IFD0 and EXIF IFD, then its
  JPEG's (RW2's IFD0 holds Panasonic's own tags: the camera's name only in the JPEG of tag 0x2E;
  RAF all in its JPEG). The size: per format as for the previews; PNG, GIF and WebP (VP8, VP8L,
  VP8X) from their headers; JPEG XL stays unknown, as with exiv2.
- `camera::read(file, false)`: type, size and orientation only, no previews walked.
- `photo.cpp`: the fields from the new reader; only the sidecar's rating still through exiv2
  (2e-4).
- The exiv2 comparison gained TIFFs made in the test with every way a value is written (long
  exposures, odd and zero fractions, signed types, values not in the tables, trailing spaces).

**Tested by the author** (2026-11-08): "funziona tutto" (info pane on his ORFs and the samples).

**Measured** (`docs/benchmarks.md`): the same information as exiv2 on every fixture, sample and
the author's 30 ORFs, and on 7 made-up TIFFs; reading it 7 to 100 times quicker (ORF 3.05 ->
0.03 ms), 30 -> 28 system calls per ORF.

---

## 2026-11-08 — Raw previews found by Nordend's own reader (ROADMAP 2e-2)

**Tested by the author** (2026-11-08, installed build, fresh cache): "funziona tutto" (the sample
folder in the grid and Quick Look, his ORFs, the info pane).

**Done**
- `src/formats/image/src/camera/previews.{hpp,cpp}`: the previews of a camera file without
  exiv2. TIFF-like files (TIFF, NEF, CR2, ARW, ORF, RW2, SRW, DNG, 3FR, ERF): every IFD of the
  chain, its SubIFDs and theirs; a JPEG by `JPEGInterchangeFormat` or as an old-style JPEG strip
  (CR2's IFD0), a reduced 8-bit RGB/YCbCr image as a TIFF preview (not in CR2: sensor-like);
  sensor data never (CFA, LinearRaw, CR2's slices). Maker notes: Olympus / OM System (thumbnail,
  CameraSettings preview), Nikon (PreviewIFD), Samsung (PreviewIFD), each read through a window
  of the file as long as the note (no longer read whole: an ORF's is 1.8 MB, holding its
  preview). RAF: the JPEG in its header and that JPEG's EXIF thumbnail; RW2: the JPEG of tag
  0x2E and its thumbnail; a JPEG: its EXIF thumbnail and Olympus maker-note preview. The main
  image's size as exiv2 gave it per format (TIFF: the first full-resolution IFD of IFD0 and its
  SubIFDs not holding a JPEG; CR2: PixelX/YDimension; RW2: SensorWidth/Height; ORF: IFD0; RAF:
  record 0x100 of its raw header) and the EXIF orientation.
- `tiff::extract`: one IFD's image copied out as a TIFF of its own (the decoder and the
  JPEG-in-TIFF check read TIFFs); `tiff::Source` over a window of the file,
  `Container::headless` for a structure without its header (Olympus's note).
- `preview.cpp`: the same choice as before (shapes, order, broken ones passed over, the
  Panasonic crop), its candidates from the reader; exiv2 no longer used for previews. CRW and
  MRW get the plain message (decided with the author).
- Caught by the comparison with the exiv2 reference before the author saw anything: CR2's
  sensor IFD (a lossless JPEG, compression 6) taken for a preview; RAF's JPEG size beyond the
  first 64 kB read (now up to 256 kB when needed); and, after the note window, a Nikon Coolscan
  note's preview IFD pointing past the note (the window is now as long as the note, as exiv2
  reads it).
- `nordend-fuzz-camera` (seeds: the first 96 kB of the sample raws, kept locally);
  `nordend-bench-previews ours|exiv2 RUNS SIDE FILE...` (built with the exiv2 reference).

**Measured** (`docs/benchmarks.md`): the same previews as exiv2 on the fixtures, the 33 readable
samples and 30 of the author's ORFs (3 379 checks); finding the preview 3 to 100 times quicker
(ORF 3.5 -> 0.03 ms for the thumbnail, 4.2 -> 0.7 ms for the 3200x2400 one with its bytes); 35 ->
22 system calls per ORF. Fuzzing: an hour, 5.7 million inputs, clean (before the note window), then 20 minutes, 1.3
million, clean. Tests green in `build`, `build-asan`, `build-tsan` (355).

**Open**: Pentax maker-note previews (no PEF sample yet), CR3 (no sample yet), old-style
Olympus maker notes ("OLYMP\0", E-1 era: none in the samples).

---

## 2026-11-08 — A scanner's TIFF shows its scan, not its thumbnail (2c follow-up)

**Found** on a sample the author added (Nikon Coolscan IV ED): IFD0 is a 320x218 reduced image
(NewSubfileType 1) and the 869x593 scan sits in IFD0's SubIFD; the decoder looked for a
full-resolution image in the main chain only, so it showed the thumbnail. The 2c comparison
with libtiff could not see it (it compared first images too).

**Done**: the first full-resolution image of the chain, else of the SubIFDs, else the first
image; only IFDs with a width, and never sensor data (CFA, LinearRaw). A test builds such a
TIFF (fails without the change); `nordend-fuzz-tiff` 10 minutes on 4 processes, 6.1 million
inputs, clean. **Tested by the author** (2026-11-08): the scan shown, in its thumbnail and in
Quick Look.

---

## 2026-11-08 — exiv2 as a reference only (ROADMAP 2e-1)

**Decisions** (with the author): the lens only from what the file writes as text (no lens
database: `Exif.Photo.LensModel`, as today); CRW and MRW get the plain message; one commit a
sub-step (2e-1 reference, 2e-2 raw previews, 2e-3 EXIF, 2e-4 XMP rating, 2e-5 exiv2 out). The
author fetches CR2, CR3, PEF and a recent NEF from raw.pixls.us.

**Done**
- `tests/exiv2_reference.{hpp,cpp}`: Nordend's exiv2 code as it was at `b27367a`
  (`preview.cpp`, `photo.cpp`, exiv2's input over a descriptor), moved into the tests under
  `nordend::formats::image::reference`, unchanged.
- `tests/exiv2_reference_test.cpp`, built only when pkg-config finds exiv2: for every fixture,
  every file of `NORDEND_RAW_SAMPLES` and of the folders in `NORDEND_EXTRA_SAMPLES`
  (colon-separated: the author's photos, never in the repository), the embedded preview at 160,
  1600 and 4000 px (bytes, size, crop, orientation, full size) and the photo information equal
  to the reference's.

**Measured**: green on the 15 fixtures, the 28 samples and 30 of the author's ORFs (with their
darktable sidecars): 2 949 checks in 1.4 s.

**Tested by the author** (2026-11-08): 3 213 checks green with the samples he added the same
day (Nikon Z 6II, Z 7II, Z 8; Canon EOS 5DS, 1D X, 600D, 70D, M3, 40D, 5D Mark III; two
"CR2" that are CHDK sensor dumps without metadata or preview, and a PowerShot CRW: not read, as
decided). Committed as `38647f8`.

---

## 2026-11-08 — DNG opcodes: gain maps and lens warp (ROADMAP 2d-4)

**Tested by the author** (2026-11-08, installed build, fresh cache): test plan passed (DJI's
corners as bright as the rest, no lines, straight shores; the other DNGs as before);
`nordend-decode`: DJI in a 2560x1440 box 109.1 ms without opcodes, 141.0 ms with them.

**Done**
- `src/formats/image/src/dng/opcodes.{hpp,cpp}`: OpcodeList1/2/3 read big-endian (whatever the
  file's order), sizes and values checked before allocating (grids up to 2^20 points, gains
  0-16, finite numbers). `GainMap` in lists 2 and 3 (maps of ones dropped), `WarpRectilinear` in
  list 3 (green's coefficients when there are three planes: the planes differ by under half a
  pixel at the sizes developed). FixBadPixelsConstant/List skipped (a site is averaged with
  three others at least); opcodes flagged optional or skippable for previews skipped; anything
  else the file requires (or a gain map after the warp, or on a pitch other than 1 or 2) stops
  the development and `load()` shows the previews, as the DNG specification asks.
- `develop()`: gain maps applied to the averaged values per output pixel (the maps are smooth
  and the averaging linear): list 3 per colour; list 2 per site of the 2x2 block (pitch 2 picks
  one site: red, blue, or one green whose gain is averaged with the other's). The warp is a last
  pass over the developed picture (bilinear, SSE2 with a portable fallback).
  `DevelopOptions::opcodes` (default on) lets the LibRaw comparison leave them out (LibRaw
  applies none of list 3).
- `nordend-decode --raw` times the development at half size and in a 2560x1440 box, with and
  without opcodes. Fuzz seeds with every opcode shape (`cfa16-opcodes`, `linear-opcodes`).
- **Fixed, a slip of mine in 2d-3** (`719a942`, seen in this step's Quick Look snapshot): thin
  lines across a developed DNG reduced to fit (the DJI in Quick Look: pairs of lines a few
  hundred pixels apart, both ways). `shares()` worked out the output pixel a block starts in as
  `i / ratio`, which in doubles can come out just under an output's edge (block 1025 of 4000 into
  1920: just under 492); that block then went whole into the pixel before, one pixel too bright
  and the next too dark. Now moved on to the right pixel; checked for every size up to 600
  blocks (every output gets exactly its share), and a test (18 blocks into 14).
- Fixed too (the author's OK): a DNG with DefaultCropSize but no DefaultCropOrigin (or the other
  way round) lost its crop; the missing one now takes the specification's default (origin 0, 0;
  size the rest of the area). None of the samples is affected (all have both).

**Measured** (`docs/benchmarks.md`): DJI in a 2560x1440 box 108.6 -> about 137 ms (gain map
about 8, warp about 18); the other samples unchanged (the S22's four maps are all ones).
Fuzzing an hour on 4 processes with the new seeds: `nordend-fuzz-dng` 70.5 million inputs, no
crash, timeout or out-of-memory. Tests green in `build`, `build-asan`, `build-tsan`.

**Decisions** (with the author): unknown required opcodes fall back to the preview; bad pixel
fixes are skipped.

**Open**: the warp's geometry checked by tests and by the author's eye only (the DJI's preview
is framed differently).

---

## 2026-11-07 — DNG fuzzing and timings (ROADMAP 2d-3)

**Tested by the author** (2026-11-07, installed build, fresh cache): "funziona tutto". In his
Quick Look (about 1200 px) the M240 took 183 ms (it was 274-323), the Canon 105 ms; the other raws
as before.

**Decisions** (with the author): the "sharp at 100 %" of 2d's "done when" moved to step 6, with
the zoom (a full-resolution demosaic there).

**Done**
- A work limit found before fuzzing, by reading: the lossless JPEG decoder filled data that
  ended early with zeros, so a 1 kB DNG could claim a 200 MP strip (200 million values decoded
  from nothing, and up to 200 MB allocated by `develop()` before reading a byte). Now every value
  costs at least a bit, as in any lossless JPEG: `find_raw` refuses a compressed strip or tile
  under a byte per 8 values, an uncompressed one shorter than its rows, and any not in the file,
  before anything is allocated; `decode_lossless` refuses a stream too short for its frame.
- Fuzz targets `nordend-fuzz-dng` (container, levels, colour, sensor data, development to a
  64x64 box) and `nordend-fuzz-ljpeg`; seeds from `tests/fuzz/make_dng_seeds.py` (six small
  DNGs: 16-bit and packed 12-bit strips, 8-bit with a linearization table, lossless JPEG strip
  and tiles, linear raw with a tone curve) and the 12 lossless JPEG fixtures.
- Quicker (`docs/benchmarks.md`): developed straight to the size shown (half-size blocks
  averaged by area on linear light into the box: no picture in between, no `scale_to_fit`; a
  straight path when nothing is reduced), Huffman tables of 12 kB (canonical decoding for codes
  over 11 bits) instead of 140 kB, the colour loop without branches on the data (tone curve as
  one line through the largest and smallest colour, `maxss`/`minss`, indexes limited as
  integers).

**Measured**: fuzzing an hour on 2 processes each: `nordend-fuzz-dng` 30.9 million inputs,
`nordend-fuzz-ljpeg` 60.7 million (plus a 5-minute run of each before, 1.9 million each): no
crash, timeout or out-of-memory. Quick Look (offscreen, 2560x1440): DJI 249 -> 169-187 ms, Canon
110 ms, M240 296 -> 256-262 ms with its thumbnail cached (aimed at: 250; its lossless JPEG,
one stream, is 100 ms of it).

**Open**: the M240 a few ms over 250 in Quick Look; the 70 ms from `load()` to the screen are
the same for every format (the memfd-per-helper proposal would cut ~10 of them).

---

## 2026-11-07 — DNGs developed to look at (ROADMAP 2d-2)

**Tested by the author** (2026-11-07, installed build, fresh cache): "tutto perfetto". His
Quick Look asked about 1200 px, so the DJI (960) and the Canon (1024) kept their previews by the
4/3 rule; the M240 developed in 274-323 ms (its thumbnail 172 ms the first time), the M8 in 90
ms; the Samsung S22 and the GX20 on their full previews; the other raws as before.

**Decisions** (with the author)
- Developed only when the previews are too small: when the largest would be stretched more than
  4/3 to fill the box asked (either way round). Quick Look at 2560x1440 develops the DJI, the
  Canon, the M240 and the M8; the Samsung S22 and the GX20 keep their full-size previews; the DJI
  (960) and the Canon (1024) keep theirs for thumbnails, the M240 (160) is developed for them.
- The look: the file's ProfileToneCurve when it has one, else a light curve of ours; no exposure
  of our own beyond BaselineExposure ("stiamo leggeri": the author, looking at the DJI next to its
  preview and darktable, which adds about +0.7 EV by default).
- Tone curves applied as Adobe's DNG SDK does (RefBaselineRGBTone: the largest and smallest
  colour through the curve, the middle one kept between them), not colour by colour, which made
  the DJI's sky an inky blue. In linear sRGB (Adobe works in ProPhoto): a difference noted.

**Done**
- `src/formats/image/src/dng/colour.{hpp,cpp}`: the DNG specification's colour (chapter 6): the
  neutral (AsShotNeutral, else AsShotWhiteXY, else D65), its temperature found by iterating
  (McCamy's CCT), ColorMatrix/CameraCalibration/ForwardMatrix of the two illuminants mixed by
  inverse temperature (the DNG SDK's temperatures for the EXIF light sources), AnalogBalance;
  forward matrix when there is one, else the inverse colour matrix and a Bradford adaptation to
  D50; then XYZ (D50) to linear sRGB. BaselineExposure + BaselineExposureOffset (clamped to +-4
  EV); ProfileToneCurve read and checked.
- `src/formats/image/src/dng/develop.{hpp,cpp}`: `develop()`: Bayer 2x2 blocks (or 2x2 linear
  raw pixels) averaged into one pixel, m x m blocks into one when the box asked is small (only
  the thumbnails so far: at 2560x1440 m stays 1); black with its repeat and its per-row and
  per-column deltas, scaled to the white level as the specification says; white balance with
  every colour clipped at 1 (a sensor full in every colour stays white); one matrix with the
  neutral and the exposure in it; the tone curve; sRGB through a table; default crop; IFD0's
  orientation. Rows two at a time from the bands of 2d-1. A usual-case path for Bayer files
  whose black repeats in 2x2 (all six samples), the general one otherwise; `minss`/`maxss`
  through SSE intrinsics (GCC makes std::min/max of floats into a dozen instructions).
- `load()`: a DNG whose previews are not enough is developed; when that fails for any reason
  the previews are used as before. `nordend-decode --raw` also times the development.
- `tiff::Source::read_into` (a kept buffer: no zeroing, no allocation per band); 16-bit
  samples in the machine's byte order copied with memcpy.
- Tests: colour (a camera seeing linear sRGB gives the identity through both the colour and
  the forward matrix; a neutral is white; two illuminants pick the nearer; exposure; no matrix),
  development of DNGs built in the test (18 % grey gives 118 at every pixel; the look keeps it
  grey; a full sensor stays white; black per row; active area and default crop; m x m blocks;
  what is refused), `load()` developing a DNG whose 16x16 preview is too small for 64 and
  keeping it for 16, and the six real DNGs developed without the look next to LibRaw's
  `dcraw_process` (half size, camera white balance, sRGB, no automatic brightness, clipped): a
  16x12 grid of mean colours, mean difference 0.1-1.7 levels, worst cell 14.

**Measured** (`docs/benchmarks.md`): Quick Look (offscreen, helpers caged, 2560x1440) from
Space to the sharp picture: DJI 249 ms, Canon 110, M240 296 (its thumbnail was being developed
at the same time). In one process: `load()` M240 216 ms, DJI 195, of which the final
`scale_to_fit` 27-43 ms.

**Slips** (mine): two tests wrong at first, not the code (the per-row black scaled against a
fixed range, the specification wants white minus the whole black; the test's signed fractions
over 10^7 overflowed past 214); `std::clamp` and an unsigned 64-bit conversion cost three
times what they should in the colour loop; the per-colour tone curve (above).

**Open**: M240 in Quick Look above the 250 ms aimed at: the final scaling (27-43 ms) could be
folded into the development, and the colour stage vectorised (to be proposed). The DJI's
corners are dark: its GainMap is 2d-4. The light curve itself was looked at on four files next
to darktable and kept as it is.

---

## 2026-11-07 — DNG sensor data, our own reader (ROADMAP 2d-1)

**Tested by the author** (2026-11-07): the three DNGs moved into `raw_test_photo_new`; the
`dng:*` tests (LibRaw comparison included) and the whole suite green; `nordend-decode --raw` on
every DNG as expected.

**Decisions** (with the author)
- 2d as proposed, updated after the author's three new DNGs (Samsung S22 Ultra, DJI Mavic Air 2,
  Canon 350D through Adobe's converter): the Samsung carries a full-size JPEG preview and will
  never need developing; the DJI (960x720 preview of 48 MP) and the Canon (1024x683) will, like
  the M240 (160 px) and the M8 (320 px). Added to the plan: the DJI's OpcodeList3 (GainMap +
  WarpRectilinear) as 2d-4 right after 2d-3; ProfileToneCurve when the file has one;
  CameraCalibration in the colour chain; linear raw (3 samples) read too.
- LibRaw (`libraw-devel`, installed by the author) is the reference for the tests only: found
  by pkg-config (`libraw_r`), linked into `nordend-tests` when present, never into Nordend or
  the helper.
- The author's DNGs go in one folder with the other raws (`NORDEND_RAW_SAMPLES`); the LibRaw
  test reads every `.dng` there, whatever its name.

**Done**
- `src/formats/image/src/dng/ljpeg.{hpp,cpp}`: lossless JPEG (SOF3) of our own: 2-16 bits, 1-4
  components 1x1, one interleaved scan, predictors 1-7, point transform, restart intervals of
  whole lines (others refused), byte stuffing; tables and lengths checked; data that ends early
  decodes as zeros; lines given one at a time to a callback. Huffman: an 11-bit table that
  gives the decoded difference directly when code and extra bits fit, an 11-bit table of codes,
  a 16-bit one for the long codes, on the heap.
- `src/formats/image/src/dng/raw.{hpp,cpp}`: `find_raw` (the main IFD: NewSubfileType 0, CFA
  or LinearRaw, the largest, in IFD0's chain and its SubIFDs), `Raw` (size, samples, bits,
  compression, CFA pattern through CFAPlaneColor, linearization table, black level with its
  repeat and deltas, white level, ActiveArea, DefaultCrop), `read_rows` (bands: 64 rows of a
  taller strip, a strip, a row of tiles; uncompressed strips read by pread a band at a time,
  lossless JPEG strips decoded straight into bands, so a sensor is never whole in memory),
  `read_all` for tests and tools. Uncompressed 8 and 16 bits in the file's byte order, other
  depths packed highest bit first, rows on whole bytes. Not read (reason given, the preview
  stays): Deflate, JPEG XL, lossy JPEG, floats, more than 16 bits, planes, non-rectangular CFAs.
- `tiff::Ifd::reals` (integers, RATIONAL, SRATIONAL, FLOAT, DOUBLE) and the DNG tags.
- `nordend-decode`: a camera raw now shows what `load()` shows (its largest preview), not the
  first image of its TIFF structure (it showed the DJI's 160x120 thumbnail); `--raw OUT.pnm`
  reads a DNG's sensor data, prints the time and writes the stored values as 16-bit PNM.
- Tests (`tests/dng_test.cpp`): 12 lossless JPEGs made by libjpeg-turbo 3 (`tests/data/image/
  dng/make.sh`: grey 8/12/16 bits, predictors 1-7, point transform, restarts, RGB 14 bits) equal
  to djpeg's decoding; cut and byte-flipped streams; DNGs built in the test (8/10/12/14/16-bit
  strips, linearization and levels, lossless JPEG tiles cut at the edges, linear raw in two
  strips, a reader that stops, what is refused, a file cut anywhere). `tests/dng_libraw_test.cpp`:
  the six DNGs (M240, M8, GX20, DJI, Canon, Samsung) equal LibRaw's values at every site.

**Measured** (`docs/benchmarks.md`): sensor data read 2-4 times faster than LibRaw's `unpack`;
M240 134 ms, Samsung 179, Canon 46, DJI 74, M8 18, GX20 38. The decoder alone on the M240's
strip went from 191 to 99 ms with the tables above.

**Slips** (mine, fixed before the tests ran): the first Huffman tables lived inside the headers
struct, four of 140 kB on the stack; a negative difference divided by 256 rounded the wrong way
in the throwaway benchmark (the checksum showed it at once).

**Open**: nothing is shown differently yet: development comes in 2d-2.

---

## 2026-11-07 — A home page, the documentation under /docs, a new message

**Decisions** (with the author)
- Nordend's message: "A native file manager for Wayland: fast, light and safe. No GTK, no Qt,
  no GLib: C++20 straight on Wayland and OpenGL ES." ("without dependencies", the author's first
  wording, would not be true: 17 libraries; "no toolkit" is.) "Made for photographers" is gone
  from the README and the site; photos stay as one feature among others.
- The README says once, plainly, that TIFF, PGM and PPM are read by Nordend's own code (checked
  against libtiff, fuzzed) and that files are read in a caged helper.

**Done**
- `site/index.html` (+ `home.css`, `home.js`): a home page: the message, Install / Read the
  docs / GitHub, the screenshot, three cards Fast / Light / Safe with their figures, "Its own
  readers" with the formats as tags, six features, "At home on any desktop, right at home on
  Noctalia", a footer. The alpine hero (contours, the ridge at dusk) as in the documentation.
- `site/docs/` (moved with git mv): the documentation as it was, with `../images/` and
  `../nordend.svg`, "Docs" beside the brand (which leads home), its opening text with the new
  message. `site/tokens.css`: the colours and type shared by both.
- Old one-page links (`…/nordend-fm/#install/source`, in the README and maybe elsewhere) are
  sent on to `docs/` by a few lines in the home page (checked with bun on seven addresses); the
  README links straight to `docs/`. The Pages workflow copies the folder (`cp -r`); the shortcut
  test reads `site/docs/index.html`.
- Looked at with headless Firefox: home dark and light at 1440 px, home at 390 px, the guide in
  `docs/` with its pictures.

---

## 2026-11-07 — Our own TIFF reader (ROADMAP 2c)

**Open** (the author checks it as he uses Nordend): Quick Look neighbours made in advance that
took 4.5-4.7 s while the fuzzers and a game held the CPU; to look at again on an idle machine
(Space and the arrows in a folder of ORFs, the "made in advance" lines with NORDEND_LOG=debug).

**Tested by the author** (2026-11-07): the 12 TIFFs of `fotografia/tiff_test_pictures` in the
grid and Quick Look (21 ms for 707x397 to 553 ms for 5600x4200; the JPEG one too), the caged
helpers on; ORFs as before (75-98 ms asked directly). Two neighbours made in advance arrived
after 4.5-4.7 s while the fuzzers held 6 cores: *to be checked again on an idle machine*.

**Decisions** (with the author)
- Our own reader rather than libtiff (a table of pros and cons, discussed): libtiff would add
  about 1.7 MB of libraries to every helper (libtiff 594 KB, zstd 817 KB, libjpeg 666 KB, lzma
  199 KB, jbig 54 KB) and a dozen codecs we do not need, with a long CVE history; and the DNG
  reader (2d) and exiv2's replacement (2e) need a TIFF container of ours anyway, so with libtiff
  we would have had two TIFF codebases. The risk of new code is met with exact comparisons
  against libtiff, fuzzing from day one, and the cage.
- Scope: classic TIFF and BigTIFF, both byte orders; strips and tiles; chunky and planar; no
  compression, LZW, Deflate (zlib), PackBits; predictors 2 and 3; 1/2/4/8/16-bit integers and
  16/32/64-bit floats; grey (either way), RGB, palette, CMYK (naive), associated and unassociated
  alpha; the first full-resolution image; Orientation. Then, the same day, JPEG inside TIFF
  (one of the author's 12 sample TIFFs used it): each strip or tile is spliced with the shared
  JPEGTables and decoded by wuffs (RGB, YCbCr 4:2:0/4:4:4, grey; strips and tiles), no new
  library. Not yet: the old "style 6" JPEG, CCITT G3/G4, ICC profiles (sRGB assumed).

**Done**
- `src/formats/image/src/tiff/`: `container` (Source over a file with pread or bytes in
  memory; IFDs with every offset and count checked; next-IFD pointer optional, as small EXIF
  blocks end right after their entries; loops and long chains stopped), `codecs` (PackBits and
  TIFF LZW ours, early change, the pre-1991 LZW refused; Deflate through zlib; every decompressor
  writes at most the expected size, short data padded as libtiff does), `decode` (layout checks
  before any allocation: sides up to 2^20, 8 samples a pixel, strips/tiles up to 2^22, 1 GiB a
  chunk, the picture limit; then chunk by chunk, read with pread, never the whole file).
- The old `TiffReader` and `decode_tiff_rgb8` are gone: EXIF orientation, the Panasonic crop
  (RW2's own magic: `any_magic`), the DNG JPEG-in-TIFF previews and the raws' small TIFF
  previews all go through the new container and decoder.
- Routing: a file with TIFF's magic is a camera raw when an IFD or SubIFD holds a CFA or
  LinearRaw image, a 10/12/14-bit one (Samsung SRW leaves out the CFA tag: found when every raw
  sample was compared, before the author saw it), a DNGVersion tag, or is a CR2; raws keep their
  embedded previews. Anything else is a plain TIFF, decoded by us without exiv2; one that does
  not decode still tries the stored previews (a raw we failed to recognise keeps its preview),
  and keeps the TIFF's reason when there is none.
- Fixtures: `tests/data/image/tiff/make.sh` (ImageMagick 7 with libtiff, and libtiff's tiffcp
  for LZW and Deflate without a predictor, which ImageMagick adds on its own) writes 30 TIFFs of
  37x29 (odd, so strips and tiles end short) with ImageMagick's own decoding of each as PNG; 280
  KB in all.
- Fuzzing: `NORDEND_FUZZ=ON` (clang only, `build-fuzz`): `nordend-fuzz-tiff` (the reader from
  memory, 4 MP cap) and `nordend-fuzz-picture` (the whole family through the registry, from a
  memfd, as the helper runs it).

**Measured**
- The author's 12 TIFFs (`fotografia/tiff_test_pictures`, found online: 512x512 to 5600x4200,
  uncompressed, Deflate, LZW, JPEG YCbCr, associated and unassociated alpha, ICC profiles, a
  "CMYK" sample that is RGB inside): **12 of 12 identical to libtiff** (ImageMagick 7 with
  libtiff 4.7.1), difference 0 on every channel; 2 ms (512x512) to 480 ms (5600x4200).
- 36 of the 36 valid fixtures (29 + 7 JPEG ones, generated with ImageMagick and tiffcp) decode
  identical to libtiff's decoding, pixel for pixel
  (difference 0 on every channel), BigTIFF, floats, CMYK, palette, planar and tiled included;
  the palette needed libtiff's rounding (v*255/65535) instead of a shift.
- Every byte of an LZW TIFF set to 0xFF in turn (a few thousand files): a picture or a plain
  reason each time, no crash (test).
- Fuzzing, first run (`nordend-fuzz-tiff`, 4 processes, 41 minutes, 11.7 million inputs, a
  corpus of 583): **no crash, 8 timeouts**. All the same story: a TileWidth tag slipped into a
  TIFF of strips (6 KB, 37x29) made the reader expect tiles of up to 1 GB each, allocated and
  zero-filled: 1.2 to 4.6 s per file in the release build. Fixed with two general limits: tiles
  never larger than the image rounded up to 16 a side, and all chunks together decompressing
  to at most four times the picture's own size (64 MB at least). The 16 slow and timed-out
  inputs are now refused in 0 ms; one is kept as `tests/data/image/tiff/broken-tile-tag.tif`
  (test: damaged in under 200 ms). The author's 12 TIFFs stay identical to libtiff.
- Fuzzing the whole image family (`nordend-fuzz-picture`, 2 processes): **a crash after 7
  minutes**, not in our reader: a file with TIFF's magic that our reader refuses goes on to
  exiv2 for stored previews, and exiv2's safe arithmetic throws `std::overflow_error`
  ("Overflow in addition"), which our `catch (const Exiv2::Error&)` did not catch: terminate.
  In Nordend it would have cost one helper ("Preview unavailable"); still ours to fix. Every
  call into exiv2 now catches `std::exception`. The file is kept as
  `tests/data/image/tiff/broken-exiv2-overflow.tif` (test: a plain reason, no exception).
- After both fixes: `nordend-fuzz-tiff` one hour, 4 processes, timeout 2 s: **33.5 million
  inputs, 0 crash, 0 timeout, 0 out of memory** (corpus 732, coverage 1295 edges);
  `nordend-fuzz-picture` 30 minutes, 2 processes: **1.24 million inputs, 0/0/0** (corpus 839).
  In all, about 1 h 40 min and 45 million inputs on the TIFF reader, 37 minutes on the whole
  family.
- All raw samples, both thumbnail sizes: same source, size and pixels (md5) as before the
  reader; the 60 TIFF pictures through the caged helper in report mode: identical, no system
  call reported.

---

## 2026-11-07 — Parsers out of Nordend, a modular layout for previews (ROADMAP 2b-3)

**Tested by the author** (2026-11-07): the info pane shows camera, lens, exposure and the
darktable stars as before; `/proc/PID/maps` of the running Nordend: 0 lines of exiv2, libjxl,
libwebp. `nordend-bench-start -n 10` in `fotografia`: main 3.4 ms (4.8 in 7.6), first frame
shown 68.7 ms median (64.5 in 7.6, but another folder and possibly another Mesa: a same-day
comparison with v0.3.1 is still to do). The helper check raced again with the 30 s idle close
(no Space pressed before it): my test plan's fault.

**Decisions** (the author: "creiamo una struttura modulare, intelligente": images now, then
text, video and the rest)
- `src/picture/` (`nordend::picture`): what Nordend does with a picture whatever its source: the
  `Image` type, `apply_orientation`, `scale_to_fit`, the thumbnail cache's PNG (`encode_png`,
  `png_text`, and `decode_png`, PNG only, through wuffs), wuffs itself (`decode_wuffs` returns
  the raw EXIF block; the orientation is read by whoever knows EXIF), and how a picture is asked
  for: `LoadRequest`, `Loaded`, `Failure`, `set_loader`/`load_picture`,
  `set_info_loader`/`load_photo_info`, `may_show`, the `PhotoInfo` struct. Linked by Nordend.
- `src/formats/<family>/`: the parsers, one module per family, linked into the helper (and
  tests and development tools), **never into Nordend: the linker enforces it**. Today
  `formats/image` (`nordend::formats::image`: decoders, raw previews, EXIF/XMP through exiv2,
  `recognises(head)`); text, video and the others will be siblings.
- `src/formats/` registry (`nordend::formats`): `make_picture(fd, w, h)` and
  `photo_info(fd, sidecar_fd)` ask each family in order whether the first 64 bytes are its own
  (never the name); unclaimed files go to the image family (camera raws by MIME). A new family:
  its module, one line in the registry.
- `src/isolate/`: transport and cage only, knows no format. Protocol: `Request::kind`
  (`picture`, `photo_info`), up to two descriptors (the file, its `.xmp` sidecar), the
  information packet (`kInfoMagic`, numbers, 13 text fields of at most 255 bytes;
  `encode_info`/`decode_info`), packets up to 8 KiB. The client's exchange is one generic
  `exchange` + `run` with an `accept` callback; `Helpers::photo_info(path)` opens the file and
  `path.xmp` itself (regular files only) and sends both.

**Done**
- Moved with `git mv` (history kept): `src/image` -> `src/formats/image`; `scale`, `png` and
  the wuffs files -> `src/picture`. Every user updated; the info pane reads EXIF through
  `picture::load_photo_info`, i.e. the helpers (main and the snapshot set both loaders; tests
  and `nordend-thumb` set the image family in-process).
- `formats::image::read_photo_info(fd, sidecar_fd)`: EXIF and the sidecar's rating through
  `FdIo`. The warm-up now also reads one photo's information with its sidecar (shot.jpg +
  shot.jpg.xmp added to the generated warm-up files).
- One slip of mine: extracting `struct Image` and `struct PhotoInfo` with a search for `};`
  cut them at `width{0};`; rebuilt from git before compiling.

**Measured**
- `nordend` against v0.3.1 (built in a worktree): NEEDED 21 -> 17 (libexiv2, libjxl, libwebp,
  libwebpdemux gone), shared objects loaded (`ldd`) 40 -> 29 (also libhwy, liblcms2,
  libjxl_cms, libinih, libbrotlienc gone; expat and brotlidec stay: fontconfig and freetype use
  them); stripped binary 3 374 008 -> 3 357 336 bytes. `nordend --version` (loading every
  library, then exit), 50 runs: median 5.8 -> 3.4 ms.
- The 55 sample and fixture files' information through the caged helper (report mode on):
  identical to the in-process reading, no unexpected call; 165 pictures unchanged (2b-2).

---

## 2026-11-07 — The helper's cage (ROADMAP 2b-2)

**Tested by the author** (2026-11-07): the raw sample folder in Quick Look; every helper logged
"seccomp on, landlock ABI 7, limits set"; `/proc/PID/status` of a live helper: NoNewPrivs 1,
Seccomp 2, Seccomp_filters 1; `/proc/PID/limits`: 32 open files, 4294967296 bytes of address
space. (A first try failed on my test plan: an expected value in parentheses after a fish
command, and a helper closed by the reaper between pgrep and grep; `pgrep -n` since.)

**Done** (proposal approved by the author: 4 GiB, EACCES for open, probe for tests only,
`NORDEND_SANDBOX_REPORT`)
- `src/isolate/src/sandbox.{hpp,cpp}`, our own code, no library. In order, once the helper has
  decoded its warm-up pictures:
  - resource limits: no core dumps, 32 descriptors, 4 GiB of address space (not under the
    sanitizers, whose shadow memory is terabytes);
  - `PR_SET_NO_NEW_PRIVS`, then landlock with no rule: no file system (every access right of the
    running ABI), no TCP bind/connect (ABI 4+), no abstract unix sockets or signals outward (ABI
    6+); the attribute's size follows the kernel's ABI. This machine: ABI 7 (we use up to 6);
  - seccomp, a BPF filter written here: other architectures' calls and x32 killed; allowed:
    read, pread64, write, recvmsg, sendmsg, ppoll/poll, lseek, close, fcntl, fstat, mmap, munmap,
    mremap, mprotect, brk, madvise, memfd_create, ftruncate, futex, getrandom, clock_gettime,
    rt_sigreturn, rt_sigprocmask, exit, exit_group; newfstatat only with AT_EMPTY_PATH (glibc's
    fstat, never a path); open/openat/openat2 fail with EACCES; anything else kills the process
    (SIGSYS). The syscall list came from strace on JPEG, PNG, WebP, JPEG XL, ORF and DNG: no
    decoder starts a thread (libjxl without a runner, libwebp with use_threads = 0).
- Warm-up: nine tiny fixture pictures (PNG, GIF, two JPEGs with EXIF, WebP still and animated,
  JPEG XL, the TIFF preview; 24 KB) generated into the helper at configure time by pure CMake
  (`file(READ … HEX)`), decoded from memfds before the cage closes.
- A helper that cannot build its cage exits with status 3 (`kSandboxFailedExit`); Nordend reads
  it when it reaps the helper: "the preview helper could not set up its sandbox". Landlock missing
  is not fatal (seccomp alone already forbids opening files); with `NORDEND_LOG=debug` each helper
  says "seccomp on, landlock ABI N, limits set".
- `NORDEND_SANDBOX_REPORT=1`: the filter traps instead of killing; the handler writes the call's
  number to stderr and makes it fail with ENOSYS (checked: "system call not allowed: 41" for a
  socket).
- ASan builds: LeakSanitizer checks at exit with a tracer (clone + ptrace), which the cage
  forbids; `__lsan_is_turned_off()` returns 1 in the caged processes only. A few extra calls are
  allowed in sanitizer builds only (sigaltstack, gettid, …, and pipe2 and ioctl: ASan checks
  whether memory is readable through a pipe; an ASan snapshot showed "Preview unavailable"
  everywhere until `NORDEND_SANDBOX_REPORT=1` named them, 16 and 293). The helpers' timeout is
  five times longer in sanitizer builds (a TSan snapshot timed out on the ORF).
- One slip of mine caught before running: the debug line asked `getpid()` after the cage closed
  (not allowed, the helper would have died); the pid is now read before.
- Tests: `tests/sandbox_test.cpp` with `tests/sandbox_probe.cpp` (`nordend-sandbox-probe`, not
  installed): what a picture needs works; open and stat of a path give EACCES; socket, exec and
  fork die with SIGSYS; 5 GiB are refused; landlock alone refuses a file and a TCP connection.
  `helpers: a helper without its cage never works` (fake helper "no-sandbox").

**Measured**
- All 165 pictures (the raw sample folder, the demo ORFs and the fixtures, at 128, 1024 and
  2880 px) through the caged helper with `NORDEND_SANDBOX_REPORT=1`: identical to the in-process
  pictures, no call reported.
- Starting a helper: 5.3-5.6 ms to the first tiny picture (3.6-4.1 before the warm-up and the
  cage); a 1920x1440 ORF through a running helper 64-65 ms (unchanged); 16 ORF thumbnails on 8
  helpers 185-187 ms (unchanged).

---

## 2026-11-07 — Pictures made in a helper process (ROADMAP 2b-1)

**Done**
- `nordend-preview` (`src/isolate/src/helper_main.cpp`), installed in `libexec/nordend/`, built
  next to `nordend` in the build tree: it reads files only through descriptors Nordend sends,
  runs `image::load(fd, w, h)`, and answers with the pixels in a sealed memfd (shrink, grow, write
  and seal sealed). No sandbox yet (2b-2).
- Protocol (`src/isolate/src/protocol.{hpp,cpp}`): SOCK_SEQPACKET socketpair, the helper's end
  on descriptor 3; request = 12-byte header + the file's descriptor (SCM_RIGHTS); response =
  header (failure, from_preview, sizes, detail length) + detail (≤ 1 KiB) + the memfd; a goodbye
  packet when a helper leaves after its idle time.
- `isolate::Helpers` (`nordend/isolate/helpers.hpp`), the client: helpers spawned with
  posix_spawn when a picture is first asked for, at most `max_helpers`, reused, waited for with a
  condition variable; each request has a timeout (3 s) after which the helper is killed; a helper
  that crashes or talks nonsense is killed and waited for; a goodbye or a dead socket before the
  request was taken gets one retry in a new helper. Nordend opens the file itself (O_NONBLOCK, a
  FIFO named like a photo cannot hang a worker; only regular files go); the helper never gets a
  path. `default_program()` finds the helper next to the running program or in
  `../libexec/nordend/`.
- `image::load(int fd, …)`, `embedded_preview(int fd, …)`: everything reads through a
  descriptor (`detail::FdFile`, pread); exiv2 gets `detail::FdIo`, an `Exiv2::BasicIo` over it
  (with a custom input, exiv2 answers an unknown type with a null image instead of an exception:
  the 7z sample crashed the first version, found by the raw test).
- `image::set_loader` / `image::load_picture`: thumbnails and Quick Look ask for pictures through
  the loader; Nordend and `nordend-snapshot` set it to the helpers at start (a warning when
  `nordend-preview` is missing: then no pictures, never parsed in Nordend). Tests and
  `nordend-thumb` keep the in-process `image::load`. New failure `unavailable`, "Preview
  unavailable".
- The helper gives its memory back (`malloc_trim`) after one second without work. Not
  `PR_SET_PDEATHSIG`: it follows the *thread* that spawned the helper; with `std::async` threads in
  the test, helpers died under the client. It leaves when Nordend closes the socket.
- Idle helpers are closed by Nordend: a reaper thread in `Helpers` (asleep on a condition
  variable until the oldest idle helper's deadline, so no CPU while idle) closes the socket,
  kills and **waits for** a helper idle for 30 s. The first version let the helper leave by itself
  after 30 s (goodbye first): the author's test showed the four helpers left as `<defunct>`
  zombies until the next request swept them. The helper's own idle exit stays as a fallback, at
  twice Nordend's time plus 1 s.
- 8 helpers at most (one per worker), decided by the author after the measurements below
  ("la sicurezza viene prima delle iper performance").
- Tests `tests/helpers_test.cpp`: the helper's pictures equal the in-process ones byte for byte
  (10 fixtures); 12 concurrent requests on 2 helpers all answered, never more than 2; a helper
  leaving after 50 ms idle is replaced unseen; a fake helper (`tests/fake_preview_helper.cpp`) that
  crashes, hangs, answers garbage or lies about the size gives "unavailable" within 2 s and is
  reaped; a missing program gives "unavailable" and nothing parsed in-process; a FIFO, a folder and
  a missing file are refused before any helper starts; `load_picture` follows the loader.

**Measured** (benchmarks.md "Pictures in a helper")
- Starting the helper: 3.7 ms (a binary with the image libraries, 22 shared objects, against
  5.7 ms and 40 for `nordend`).
- A 1920x1440 ORF picture: 54 ms in process, 64-65 ms through a running helper; the 10 ms are
  the pixels: 5.3 ms writing the memfd in the helper, 2.5 ms reading it into Nordend (page faults
  of 11 MB each side). A populated shared mapping in the helper made it worse (70 ms).
- 16 ORF thumbnails (256 px) on 8 threads: 185-215 ms in process; 4 helpers 219-235 ms (+15-20 %),
  8 helpers 184-191 ms.
- An idle helper after 16 Quick Look sized ORFs: 54 MB anonymous kept by malloc; trimming after
  each picture brings it to 1.4 MB but costs 20 ms a picture (64 -> 85 ms); trimming after one
  quiet second keeps both (1.4 MB idle, 65 ms).
- Quick Look of an ORF in `nordend-snapshot` (offscreen, a fresh process, so a cold helper):
  105 ms in process, 120-136 ms with 4 helpers, 121 ms with 8 (the variance was the main picture
  waiting for a helper busy with its neighbours).

**Tested by the author** (2026-11-07): thumbnails, Quick Look, the raw sample folder; four
helpers while browsing, none after 30 s (zombies in the first version, fixed and checked: 8
helpers after work, 0 processes left after the idle time).

**Open**
- The 10 ms of the pixels' trip: a memfd kept per helper (no new pages per picture) and an upload
  to the GPU straight from the mapping would remove most of it; not done.

---

## 2026-11-07 — One path for every picture (ROADMAP 2a)

**Tested by the author** (2026-11-07): thumbnails and Quick Look on the raw sample folder and the
author's ORF and JPEG folders, the plain reasons, a PNG named `.jpg`.

**Done**
- `image::load(LoadRequest) -> Loaded` (`nordend/image/load.hpp`) is the only way a file becomes a
  picture: thumbnails (`thumbs::Cache::make`) and Quick Look (the image provider) both call it.
  `image::load_to_fit` / `Fitted` and the copy of the same steps in `Cache::make` are gone
  (`fit.cpp` deleted). The request and the result are plain data, ready to cross a process
  boundary in 2b.
- The format comes from the first bytes (64 KiB read once), not the name: PNG, GIF, WebP, JPEG
  XL, PGM/PPM are decoded whole and **never reach exiv2**; a JPEG asks exiv2 for a stored preview
  only when it has EXIF (a camera's); TIFF and the raw formats built like it (ORF, RW2, old
  Panasonic RAW), RAF, CR3, CRW and MRW use stored previews only, no pointless whole-file decode;
  Sigma X3F is refused at once (exiv2 does not read it); anything else that a raw MIME type led
  here still goes to exiv2.
- Failures are a type, `image::Failure` (unknown_format, no_readable_preview, too_large, damaged,
  unreadable), each with one sentence (`failure_message`) shown the same everywhere; the
  technical reason stays in `Loaded::detail` and goes to the log (`NORDEND_LOG=debug`,
  "preview: …"). Decoders now report `Decoded::problem`, and the four spellings of "too large"
  became one (`detail::kTooLarge`).
- `thumbs::can_make` became `image::may_show` (the list lives with the image code).
- Tests: `tests/load_test.cpp` (every fixture through `load` gives the same pixels as its
  thumbnail; a PNG named .jpg, a JPEG named .png, a WebP named .orf all show; empty, noise, huge,
  cut short, missing and X3F files give their own failure; exiv2 is not called for PNG, GIF, WebP,
  JPEG XL, PGM/PPM or a JPEG without EXIF, counted by `embedded_preview_reads()`).

**Measured** (benchmarks.md "One path for every picture")
- Thumbnails at 128 px, median of 7: unchanged within noise (PNG 2400x1600 177.3 → 175.5 ms, the
  exiv2 open saved; ORF 7.6 → 7.8 ms, the 64 KiB head read). Quick Look of an ORF in the
  offscreen snapshot (2560x1440, scale 1): 105-107 ms before, 103-105 ms after.
- The gain of this step is fewer parser runs on outside files (safety) and one place for the
  logic, not speed.

**Refactors** (asked by the author after the test)
- The preview choice in `preview.cpp` is one list in order of preference
  (`order_by_preference`: JPEGs large enough smallest first, TIFFs large enough smallest first,
  then the largest, a JPEG before a TIFF of the same size; stable, so ties keep the file's order),
  tried until one is usable. The two-pass `choose()` and its retry loop are gone (17 lines in, 42
  out). Checked: `nordend-thumb` at the four sizes on the 55 sample and fixture files picks the
  same preview at the same size as before; only the failure sentences differ (2a).
- One reading of the file per picture: **measured, not done**. `strace` on one ORF thumbnail: 7
  opens, about 130 system calls on the file, of which ours are 2 opens (the 64 KiB head and the
  Olympus preview); the rest is exiv2 (lseek, reads, an mmap of the whole file). Sharing one
  buffer among our own steps saves well under 0.2 ms of 7.8; feeding exiv2 from memory would mean
  reading or mapping the whole raw (a mapped file cut short while read kills the process with
  SIGBUS). Moved into 2e: our own reader opens the file once, with pread.

---

## 2026-11-07 — Nordend 0.3.1

**Done**
- Release 0.3.1 (fixes only): the raw previews of the step below. CMake version, README (badge,
  note, install note, Versions, What's next), site (badge, notice, install note, roadmap),
  features.md Next.
- Tagged `v0.3.1` on `183a2b0` by the author, GitHub release published by the author; the
  template (void-noctalia and `packaging/void`) on the tag's tarball (sha256 `6f5a7e45…094ac1`),
  built with xbps-src: `nordend-0.3.1_1` (no `=> ERROR` in the log; /usr/bin/nordend, the
  desktop entry, the icon, the licence). The site shows v0.3.1 (Pages run green in 23 s).

**Decisions** (with the author, the same evening)
- Nordend's philosophy, stated by the author: a native C++ file manager made for Noctalia Shell,
  no GTK or Qt; very light, safe and fast. Everything else is a feature on top. README and site
  "What's next" say so.
- The roadmap reordered: previews first, one family at a time. Images (a universal path for
  every format, the isolated helper, our own TIFF decoder, then DNG on top of it; raw files keep
  the embedded JPEG, one without a readable preview is not read), then text (syntax highlighting,
  selecting, copying), then other formats one at a time (heavy ones like video and PDF discussed
  when we get there). Then labels and ratings, Quick Look for photographers, the dual pane.
- Measured for that choice: a plain `.tif` file is not decoded today ("not an image format
  Nordend decodes"): only the small TIFF previews inside raw files are read.

---

## 2026-11-07 — Raw previews of the new sample folder (ROADMAP 1, 0.3.1)

**Tested by the author** (2026-11-07): thumbnails and Quick Look on the new sample folder and on
the author's ORFs, all as expected.

**Done**
- DNG previews stored as a JPEG inside a TIFF (compression 7, or 6 when the one strip is a whole
  JPEG) are taken out and read as JPEGs (`detail::jpeg_in_tiff`): Leica M240 (160x120, its only
  preview) and Samsung GX20 (4672x3104, full size) failed before, both show now.
- A chosen JPEG preview must be whole (`detail::whole_jpeg` follows the segments and the scan's
  markers to the EOI; bytes after it are fine, Panasonic pads its previews), else the next
  candidate is chosen: Samsung EX2F's 640x360 preview stops before its end, so small thumbnails
  of that SRW failed; they now come from the 4000x2248 one.
- Binary PGM and PPM (P5, P6, 8 and 16 bits) decoded by our own `decode_pnm` (`Format::pnm`;
  `image/x-portable-graymap`, `image/x-portable-pixmap` get thumbnails and Quick Look): the
  Hasselblad CFV sample is a PPM.
- Raw formats exiv2 does not read (Sigma X3F, Panasonic `.RAW` of the DMC-FZ8, LX1, Leica D-LUX 3)
  say "No preview Nordend can show in this file" in Quick Look instead of the decoder's "not an
  image format Nordend decodes".
- The raw-sample test rewritten for `fotografia/raw_test_photo_new`: every sample with a preview at
  four sizes, plus the special cases above; the GX8 crop checks moved to their own test, skipped
  until the folder has rawsamples.ch's GX8 files. New test "image: PGM and PPM".

**Measured** (see benchmarks.md "Raw previews")
- Previews at 1024 px: 68-117 ms for NEF, RAF, RW2, ARW, SRW; GX20 DNG 177 ms (it decodes the
  full-size JPEG even for small thumbnails: the 160x120 TIFF thumbnail is 4:3, the photo 3:2).
- What newer bodies carry: Nikon (8280 px wide) and the 7168 px Sony a full-size JPEG; Fuji X-T2,
  X-A2, X-Pro1 1920x1280 only; older Sony 1616x1080; the OM-1 3200x2400.
- `whole_jpeg` costs 0.39 ms on a 969 kB ORF preview.

**Open**
- Thumbnails of the GX20's DNG decode its 4672x3104 JPEG (177 ms); a scaled JPEG decode would
  help, *not planned*.
- GX8 samples for the crop test (rawsamples.ch `RAW_PANASONIC_GX8_{11,169,43}.RW2`).

---

## 2026-11-07 — A new roadmap after 0.3.0

**Decisions** (with the author)
- Nordend depends on no raw editor: darktable, Lightroom, Capture One, ART and the new editors are
  equal guests. Stars and colour labels are Nordend's own, in the file's extended attributes
  (`user.baloo.rating` 0-10 as Dolphin/KDE writes it, `user.nordend.label`), on files and
  folders alike; XMP sidecars of any editor are a bridge (read when the file has no attributes,
  written only when asked in `config.toml`). Colour labels as on macOS: right click, the row takes
  the colour.
- The roadmap starts again: 0.3.1 raw fixes, 0.4.0 labels and ratings, 0.5.0 Quick Look for
  photographers (100 % zoom, EXIF overlay, histogram, rating from Quick Look, compare), then
  isolated previews (a pre-started helper under seccomp + landlock, no network; LibRaw only in
  there), then dual pane, operations queue and batch rename. DESIGN's post-MVP list follows.
- Later the same day: **no raw decoding beyond the embedded JPEG** (no LibRaw, no camera
  database); a raw's 100 % zoom is the preview's 100 %, said in Quick Look. **DNG is decoded by
  our own C++ decoder** (bounded reader, checked arithmetic, fuzzing), born inside the isolated
  helper. Nordend is a file manager for creators: Quick Look gains syntax highlighting and text
  selection, a video/audio player (libmpv render API the candidate, loaded with `dlopen` on the
  first video), then HEIC/AVIF, SVG, PDF, fonts, PSD one at a time. New order: 0.3.1 raw fixes,
  labels and ratings, Quick Look for photographers, isolated previews, DNG decoder, Quick Look for
  creators, dual pane, then translations, portal, Luau, helix.
- Measured for that choice: the OM-1 ORF embeds a 3200×2400 JPEG of a 5220 px wide photo.
- Prompted by Strata (Rust + GTK4, Miller columns, Ctrl+K, previews parsed out of process in
  Bubblewrap, no colour labels or ratings).

**Measured**
- New raw folder `fotografia/raw_test_photo_new` with `nordend-thumb --size xx-large`: NEF ×2,
  RAF ×3, LX7 RW2, SRW, ARW ×3 give 1024 px previews in 68-117 ms; Epson ERF (160×120),
  Hasselblad 3FR ×2 (320, 290) and Leica M8 DNG (320×240) only carry tiny previews; **Leica M240
  and Samsung GX20 DNG fail** (exiv2 lists a 160×120 big-endian TIFF and a "4672×3104 TIFF"),
  Panasonic `.RAW` ×3 and Sigma X3F are unknown to exiv2, the Hasselblad CFV is a plain PPM (not
  decoded). The old raw test expects the rawsamples.ch names of the previous folder: to rewrite.
- `user.*` extended attributes work on the DATA disk (ext4); our transfers do not copy them yet.

---

## 2026-11-06 — The site, as documentation

**Done** (asked by the author: "più carino", full-width with a sidebar, Nordend's colours, easier
to read, like browsing documentation)
- Layout: a top bar (menu on narrow screens, brand, version, GitHub, theme), a full-height
  sidebar with the pages in four groups (Get started, Use it, Under the hood, Project) and, under
  the page shown, its sections; the page in a 52rem column; "On this page" at the right from
  1360 px, following the reading (IntersectionObserver); previous and next at the bottom; a "#"
  link beside every section title. Below 960 px the sidebar is a drawer behind "Menu".
- Search at the top of the sidebar: every word typed must start a word of the page or section
  (`raw` finds "raw files", not "draw"); `/` focuses it, Enter goes to the first hit, Escape
  clears; `?q=…` in the address opens the site with that search (a search to share).
- Colours: the "Nordend" palette the author made for Noctalia (void-noctalia
  `config/noctalia/palettes/Nordend.json`): dark page `#1a1918`, sidebar `#232322`, ice
  `#7fc0ea`; light page `#eeeceb`, sidebar `#e3e1df`, `#256292`. The alpenglow
  (`mTertiary`) only in the hero's kicker and glow.
- Pictures: the author's six new shots (WebP, 12-102 kB, metadata stripped; no people's names,
  no bar): the overview's grid (dark) and a tour gallery (columns dark, details light, Quick Look
  of a raw, F1), the palette in the guide, F1 on the shortcuts page. The old
  `screenshot-columns.webp` and `screenshot-quick-look-text.webp` removed. The 16 guide pictures
  made again with the Nordend palette: `noctalia theme --theme-json Nordend.json --default-mode
  dark|light -r ~/.config/noctalia/templates/nordend:OUT` renders Nordend's colors.toml for each
  mode (the light one is byte for byte the author's live file), then the usual snapshot recipe.
  NB the demo's state now has the info pane visible: `Ctrl+2` gives the grid with the pane,
  `Ctrl+2,Ctrl+I` without (names swapped accordingly).
- Looked at in headless Firefox: overview (dark), guide (light, the gallery loaded), shortcuts
  (dark), 420 px wide, three searches. The shortcut data block is unchanged (site test green).
  `firefox --screenshot` with a `#page/section` address renders from the document's top while the
  sticky bars follow the scroll: not a site bug.

---

## 2026-11-06 — 0.3.0

**Done**
- Version 0.3.0: the command palette (8a), finding files (8b), the icon in the accent (9.2). The
  README's Versions table, badge and notice, the site's badge, notice, install note and roadmap,
  and features.md say so. The tag goes on this commit; the Void template moves to it in the next
  one, as for 0.1.0 and 0.2.0.
- Tagged `v0.3.0` on `653d4db` by the author, GitHub release published by the author; the
  template (void-noctalia and `packaging/void`) on the tag's tarball (sha256 `5a08c204…74c1b8`),
  built with xbps-src: `nordend-0.3.0_1` (no `=> ERROR` in the log; /usr/bin/nordend, the
  desktop entry, the icon, the licence).

---

## 2026-11-06 — 8b: finding files (Ctrl+F)

**Done**
- `fs::walk`: breadth first with `openat`/`fdopendir`/`readdir` and `d_type` (one `fstatat` per
  folder, for its device), relative paths in `PathBatch` (one buffer and an end offset per path,
  shrunk to fit before it is handed over), links listed but never followed, other file systems
  not entered, dotfiles left out unless asked, cancellable between folders. Limits: 300 000
  entries (truncated, said in the status), depth 64.
- `views::FileFinder`: the walk streamed from a worker (`submit_streaming`), batches kept as
  `shared_ptr<const PathBatch>`; a query is matched on all the workers (the batches dealt out in
  shares), each share keeping its best 200 in a heap, merged on the loop; new batches are matched
  as they come; a new query cancels the old jobs (generation). Nothing typed: the first 200
  entries, the nearest. `status()`: "Searching… N items", "N items", "Stopped at N items".
- `FuzzyQuery` (prepared once per job): spaces in a query are dropped (so Space can stay Quick
  Look, the ROADMAP's choice, and "monte rosa" still finds Monte Rosa); `path_score` ranks a match
  in the name above any that needs the folders; `path_positions` highlights the name when it
  matches there.
- `CommandPalette::open_external` / `set_results` / status line / close handler; path items are
  shortened in the middle; Space runs `quick_look`, Shift+Enter `open`; the highlight follows the
  item it was moved to while results keep coming.
- `Browser`: Ctrl+F (`Command::find_files`, also "Find Files…" in the palette), Ctrl+F again
  closes, Ctrl+K switches and back. `show_found`: in the columns `reveal` from the first column
  when the file is inside it (the way stays), else from the folder searched; in the grid and the
  details the file's folder opens with the cursor on it. Quick Look and Open wait for the cursor
  to land (`settle_found`, run by `draw()`), and a key or a click meanwhile forgets them. The
  finder goes when the card closes, then `core::give_back_free_memory()` (`malloc_trim`). Not in
  the trash. `loading()` counts the finder; the memory report has a line for it.
- New tool `nordend-bench-find`; 9 new tests (316). ASan and TSan clean (the finder tests five
  times under TSan, the benchmark and a snapshot under both).

**Measured** (`benchmarks.md`)
- `DATA/dev`, 273 187 entries: first results 8 ms, whole listing 0.44 s, a new query over all of
  them 6-14 ms; 23 MB of paths; anonymous memory back to 2.9 MB after closing (30.3 with them).
- Snapshots looked at, dark (nothing typed: folders first, the nearest) and light ("vinc17").

**Tested by the author** (2026-11-06): everything works.

**Open**
- Cold-cache numbers: the author's run.
- Results do not follow changes on disk while the card is open (listed once).

---

## 2026-11-06 — 8a: the command palette (Ctrl+K)

**Decisions** (with the author, the same day)
- ROADMAP 8 split: 8a the command palette (Ctrl+K), 8b finding files under the folder (Ctrl+F);
  together they make 0.3.0. Ctrl+K holds commands, places and tabs; the recently used (ten) are
  remembered in `state.toml`.
- fzy 1.1 vendored in `third_party/fzy` (the author's OK): the matcher only, unmodified, compiled
  as C without our warnings; listed in `THIRD_PARTY.md` with sha256. Noctalia's launcher uses the
  same release.

**Done**
- `views::fuzzy_score` / `fuzzy_positions` over fzy: case-insensitive (the query lowercased, as
  Noctalia does, since fzy widens only lowercase), bytes as they are for UTF-8, infinities made
  finite so they sort.
- `views::CommandPalette`: a card at the top centre (600 px, up to ten rows, fewer in a short
  window), a `TextField`, the list re-scored on every edit and stable-sorted (equal scores keep
  the items' order); with nothing typed, the recent first. Matched letters in `primary`; the
  highlighted row in `hover`/`on_hover` like the context menu. Keys: arrows, Page Up/Down, Enter,
  Escape; Tab swallowed; Ctrl+V pastes (first line, no control characters). Pointer: the row
  under it highlighted, a click runs, the wheel scrolls, a click outside closes. The pointer
  shape: text over the field, a hand over the rows.
- `Browser::palette_items()` (new `browser_palette.cpp`): commands through `run(Command)`, the
  keys the views handle themselves (zoom, Select All, parent) as the same key sent to the view,
  the menus' actions (Sort by…, New Empty File, Restore, Empty Trash, Open in New Tab, Eject or
  Unmount), places as "Go to …", other tabs as "Tab N: …" (not remembered), Quit (main gives a
  handler). Only what can be used now: no Paste without something to paste, no Rename… for two
  entries, no "Show Grid" in the grid, labels that say what happens ("Hide Hidden Files").
  "Open With…" fills the palette again with the applications, found as the menu finds them.
- Ctrl+K in the shortcut table (General), on the site's shortcut list; the palette takes every
  key and click while open, after the conflict and confirm cards, before the context menu (which
  it closes). `State::palette_recent` → `[palette] recent = [...]`.
- 9 new tests (307): fzy ranking, the widget's keys and filter, the browser (Ctrl+K, every
  command offered somewhere, items doing what their keys do, recent through state.toml). Tests
  pressing every key close the palette in their reset. ASan and TSan clean, snapshot too.

**Measured**
- Filtering 500 items: 0.10-0.15 ms a key (Release), target < 1 ms (`benchmarks.md`).
- Snapshots looked at, dark and light, empty and with "new" and "godo" typed.

**Tested by the author** (2026-11-06): everything works ("molto utile").

**Slip**
- I overwrote `tests/palette_test.cpp` (the colour palette's tests) with the new tests; restored
  from git at once (no difference left), the new tests are in `command_palette_test.cpp`.

**Open**
- "New File" from a template is not in the palette (the templates are looked for on a worker;
  the context menu has them).
- No icon before the items (the Tabler subset has no generic command icons; adding some means
  rebuilding the font subset).

---

## 2026-11-06 — 9.2: the icon follows the accent

**Done**
- `theme::app_icon_tones(accent)`: the three tones in OKLCH. Each keeps the ice icon's lightness
  (front 0.767, tab 0.617, mountain 0.508) and its small steps of chroma and hue from the front;
  hue and chroma come from the accent, chroma held to 0.16 (GNOME's orange would glow). The ice
  front as accent gives back `#6dbdf0`, `#3f8cc4`, `#2a6a9e` exactly; the snow stays white.
- `app_icon_svg(nullopt)` is `data/nordend.svg` byte for byte (tested against the file); with an
  accent the comment says "written by Nordend", which is how Nordend tells its own file.
- `sync_app_icon(place, accent)` on a worker (`AppIconKeeper`: one job at a time, the latest
  accent wins): writes `$XDG_DATA_HOME/icons/hicolor/scalable/apps/nordend.svg` atomically, only
  when its text changes; without an accent takes Nordend's copy away; never touches an icon that is
  not Nordend's. When the running binary's prefix holds its icon at that same path (the author's
  `cmake --install --prefix ~/.local`), the ice icon is written back instead of removed, and that
  file may be overwritten.
- `Theme::accent()`: colors.toml's primary, else the portal's accent; none with the default
  palette or a desktop without an accent (the ice icon then). Followed at start, on every theme
  change and on a config.toml reload.
- `config.toml`: `[icon] follow_accent = true` (new `read_bool`); example, site and features
  updated. OKLCH conversions moved from `desktop.cpp` to a private `oklch.hpp/.cpp`.
- 9 new tests (298); ASan and TSan clean; clang-tidy and clang-format clean.

**Measured**
- Noctalia (source at `f90f230`): `IconThemePollSource` watches the icon folders with inotify
  (`IN_CLOSE_WRITE`/`IN_MOVED_TO` of an `.svg`), invalidates the resolver after 100 ms and
  rebuilds the dock's items, which reload the image: the dock should change without a restart.
  *To be verified live.* GNOME Shell and Plasma: *to be verified in the VM* (Plasma's icon cache
  may need a restart).
- Ten accents rendered and looked at: every one keeps the icon's contrast; Noctalia's default
  yellow becomes olive (`#c0b65d`), as yellow cannot stay bright at the front's lightness.

**Tested by the author** (2026-11-06): everything works; Noctalia's dock changes the icon live
when the colour scheme changes, `follow_accent = false` puts the ice icon back and `true` brings
the accent back. GNOME and Plasma still to be tried in the VM.

The README now opens with a single screenshot of the author's (`readme-screenshot.webp`: the
columns in a folder of darktable exports, the context menu, the info pane), three folder names
with people's names blurred; the two earlier README images removed.

**Open**
- An icon left in `~/.local/share/icons` after the Void package is removed (until
  `follow_accent = false` with Nordend running, or by hand).

---

## 2026-11-06 — 0.2.0

**Done**
- Version 0.2.0: Sort By in the context menu (8.0), tabs (9.1a-d), zoom in the lists and a zoom
  per tab (8.1, 8.1b), with the new screenshots. The README's Versions table and badge, the
  site's badge, notice, install note and roadmap say so. The tag goes on this commit; the Void
  template moves to it in the next one, as for 0.1.0.
- Tagged `v0.2.0` on `ed8f10a` by the author; the template (void-noctalia and `packaging/void`)
  on the tag's tarball, built with xbps-src: `nordend-0.2.0_1`.

---

## 2026-11-06 — New screenshots for the README and the site

**Done**
- From the author's five new desktop shots (Noctalia's Nord, 2560x1440, with tabs): the README
  opens with the grid on raw photos (light) and the columns deep in a source tree (dark); the
  site's overview shows the columns on a folder of photos and Quick Look on a source file. WebP:
  66-142 kB each. The details on `node_modules` left out.
- Blurred before anything left the machine: the media title and the Wi-Fi name in the bar (all
  five), a folder named after a person in the columns shot.
- The old `screenshot-columns.png`, `screenshot-quick-look.png` and `readme-desktop-light.webp`
  removed; the Pages workflow copies only `docs/images/*.webp` now (with no PNG left, `cp *.png`
  would have failed the deploy).

---

## 2026-11-06 — 8.1b: a zoom per tab

**Done**
- The zoom is the tab's (the author's choice, after 8.1): a view's zoom goes to its own pane only
  (the two lists together, the grid apart). A new tab starts with the zoom of the tab it comes
  from (`add_pane` copies from the tab shown).
- `State::Tab` has `list_size` and `grid_icon`, both optional: written for every tab; a tab read
  without them (written before) takes the window's `[view]` values, which stay for a start with
  one tab. `restore_tabs` gives each tab its own; `Browser::grid_index(pixels)`.
- `set_list_size` / `set_grid_size` now set the tab shown's.
- Tests: a zoom in one tab leaving the other as it was, the grid apart, each tab's sizes in the
  state and back in a new browser; a tab without sizes taking the window's. 289 tests, ASan and
  TSan clean.

**Tested by the author** (2026-11-06): a zoom per tab, a new tab starting with the zoom of the
one it comes from, Ctrl+0 in one tab only, two grids of different sizes, each tab's zoom back
after a restart.

---

## 2026-11-06 — 8.1: zoom in the lists

**Done**
- `views::ListSize`, five sizes (`kListSizes`: row 24-52 px, icon 16-36, name 13-17, the other
  cells 12-15; index 1 the usual, as before). `Column` and `DetailsView` take their row, icon and
  text from it instead of constants (`kRowHeight` stays as the usual size, for the benchmarks).
- Ctrl+plus, Ctrl+minus, Ctrl+wheel and Ctrl+0 in the columns and the details, as in the grid;
  the shortcuts' "Grid" group is now "Zoom", listed for every view. A zoom keeps the cursor's
  row at the same height on screen, the whole row in sight (else the first rows scale).
- The details' fixed columns (size, modified, type) grow with the text: widths are kept at the
  usual size and shown times the text's scale; a dragged edge is converted back, so a zoom
  scales it too. At the largest size dates no longer end in "…".
- One size for the lists and one for the grid, the window's: the views report a zoom
  (`set_zoom_handler`), `Browser::set_list_size` / `set_grid_size` give it to every tab, a new
  tab takes it. Both remembered in `state.toml` (`view.list_size` "small" … "largest",
  `view.grid_icon` 48-256); the grid's size was not remembered before, nor shared between tabs.
- Tests: keys and Ctrl+wheel in the details and the columns, the size shared between tabs and
  the two views, Ctrl+0, the smallest as a floor, the cursor's row kept on screen, the grid apart;
  state read and written, bad values reported. Snapshots at the smallest, the largest (details)
  and a larger size (columns). 288 tests, ASan and TSan clean.

**Tested by the author** (2026-11-06): keys and Ctrl+wheel in both lists, dates whole at the
largest size, Ctrl+0 and the smallest floor, the grid apart, a new tab with the same sizes, a
dragged column scaling, the sizes back after a restart.

---

## 2026-11-06 — 9.1d: tabs remembered

**Done**
- `State::tabs` (with two tabs or more; one tab writes none, so Super+E still opens Home):
  `[[tab]]` tables with `folder`, `mode`, `root` (the columns' first folder) and `cursor` (the
  name under the cursor), `shown = true` on the tab that was shown. At most 64.
- Written as TOML basic strings (quotes, backslashes, control characters escaped); TOML holds
  only UTF-8, so a tab on a name that is not well-formed UTF-8 is not written (`valid_utf8`:
  no stray, overlong or surrogate sequences).
- Read back with each problem said where it is and the tab left out: a folder or root not
  absolute, an unknown mode, a cursor with a slash, a tab that is not a table.
- `Browser::state()` lists the tabs; `restore_tabs(tabs, shown)` opens them as they were: the
  columns unfold from their root down to the cursor (the focus where it was), the grid and the
  details put the cursor back; folders gone meanwhile (an unplugged disk) are left out; false when
  none is left. `main` calls it only at a start without a folder, after `apply()`; with a folder
  (xdg-open, "Show in folder") one tab on it, as decided with the author.
- `StateKeeper` moves the state it is given (clang-tidy, now that it holds a list).
- Tests: written and read back with odd names (quotes, backslash, a tab, "città", a cursor with
  quotes); a name not UTF-8 left out; four bad tabs reported at their lines; the browser's tabs
  through the file into a new browser, the columns unfolded to the same focus, a folder gone left
  out, nothing left giving false. 286 tests, ASan and TSan clean.

**Tested by the author** (2026-11-06): three tabs (columns two levels down, grid with the cursor
on a photo, details), the middle one shown, back after a restart with Super+E as they were; a
folder given opening alone; one tab starting at Home.

---

## 2026-11-06 — 9.1c: folders into tabs, behind the one shown

**Done**
- A middle click on a folder in any view opens it in a new tab right after the one shown, which
  stays shown, with a message; on a file, or in the trash, nothing (the views take no middle
  clicks). On a folder of the selection it opens every selected folder, as the menu does (the
  author found it opening only the one clicked, 2026-11-06); files in the selection are left out.
- The context menu of folders: "Open in New Tab", "Open in New Tabs" for several, opened behind
  in their order (`open_tabs`), with "Opened N folders in new tabs".
- `open_tab_behind` apart from `open_tab(…, true)`: opening behind never moves the pointer to
  another view, so the pointer's path does not loop back through `enter_pane` (clang-tidy's
  misc-no-recursion).
- Tests: the menu item on a file (none), a folder, two folders; a middle click on a file, on a
  folder, and on a folder of a selection with a file in the details drawn offscreen. 283 tests,
  ASan and TSan clean.

**Tested by the author** (2026-11-06): middle clicks on folders and files in the three views,
Open in New Tab(s) from the menu; after the fix, a middle click on one of three selected folders
opening all three.

---

## 2026-11-06 — 9.1b: the tab bar and its keys

**Done**
- `views::TabBar`: above the path while there are two tabs or more; the tabs share the width
  (90-240 px), named after their folder (Home, Trash, / for the root), the one shown in the view's
  colour and open onto the path below. A click shows a tab, its × (on the tab shown and the one
  under the pointer) or a middle click closes it, the + after the last opens one (two bars: the
  icon font has no plus).
- `Browser`: `new_tab` (Ctrl+T: the folder shown, the same view, shown at once), `open_tab(dir,
  show)` (right after the tab shown; behind it, with a message, for 9.1c), `close_tab` (Ctrl+W:
  nothing on the last tab; the one after takes the place of the tab shown, or the one before),
  `select_tab` (Ctrl+Tab, Ctrl+Shift+Tab round, Alt+1…9), `tab_titles`, `folder_of(pane)`.
  Before another pane shows, `leave_pane` closes Quick Look and a menu, confirms a name being
  edited (as a click elsewhere), drops a press or a band and the view's hover; `enter_pane` gives
  the new view the pointer. The bar takes the pointer before the path (`Target::tabs`) and shows
  a hand on what a click acts on.
- Shortcuts: a "Tabs" group (overlay: the second of two columns, the third of three; the site's
  table from the same data). Alt+1…9 needs the digit: `take_key` turns it into the index.
- `~ColumnView` unhooks its folders' cursor handler: a folder can outlive its columns now that a
  tab closes while the app runs (held by a rename on its way). A test moves the cursor of a folder
  kept past its tab: without the unhook ASan reports a heap-use-after-free in
  `ColumnView::on_cursor_changed`.
- Tests (`tabs_test.cpp`, 5): the keys and the last tab staying, a view and a history per tab with
  the window's order and hidden files, tabs opened behind, closing the tab shown or another, a tab
  closed during a rename, the bar drawn offscreen (clicks, ×, middle click, +, the hand, the path
  below it). Snapshot of three tabs, Nord light and dark. 282 tests, ASan and TSan clean.

**Decisions**
- Ctrl+W while a name is edited is the field's, as every key then; the tab's × closes it, keeping
  the name typed.
- No reordering of tabs by dragging yet; not in the step.

**Tested by the author** (2026-11-06): Ctrl+T, the titles following the folder, Ctrl+Tab,
Ctrl+Shift+Tab, Alt+1/2, a view and a history per tab, +, ×, middle click, Ctrl+W (not the
last), Sort By and Ctrl+H in every tab, Quick Look and the menu let go on a switch, a rename kept
when its tab's × closes it.

---

## 2026-11-06 — 9.1a: a pane, ready for tabs

**Done**
- `views::Pane` (`pane.hpp`): the column view, the grid and the details of one place, which is
  shown (`ViewMode`) and its history (`HistoryStep`, `here`). The browser keeps a list of panes and
  shows one (`pane()`, `active_`); everything else stays the window's: the sidebar, the info pane,
  the status bar, Quick Look, the dialogs, the transfers, undo. `Browser::Mode` and
  `Browser::Step` are now names for `ViewMode` and `HistoryStep`, so callers and tests are
  unchanged.
- `Browser::add_pane()` makes a pane and wires its views to the window (open, edit slot, menu,
  drop and cut marks), with the window's order, hidden files, info pane and column widths.
  `set_sort`, `set_show_hidden`, `set_info_visible` and `apply(State)` go over every pane; a click
  on a title of the details sorts every pane. `panes_` sits where the views were, so the order in
  which things are destroyed is the same (the transfers' thread stops first).
- `update_band` and `scroll_band` keep a reference to the band once checked (clang-tidy could not
  see it stay set across the calls to `pane()`).

**Measured**: nothing to measure; the same 277 tests pass (also under ASan and TSan), and
snapshots of the columns, the grid, the details and Quick Look match this morning's to the pixel
but for the free space in the status bar.

**Tested by the author** (2026-11-06): everything as before (views, history, Sort By, hidden
files, info pane, rubber band, copy, paste, undo, state at the next start).

---

## 2026-11-06 — 8.0: Sort By in the context menu

**Done**
- The context menu, on an entry and on empty space, ends with a **Sort By** submenu: Name, Size,
  Modified, Type, then Ascending, Descending; a dot on the order in use. Choosing a field keeps
  the direction.
- One order for the window (`Browser::set_sort`, `sort_order()`): the details view owned it and
  applied it only to the folder it showed; now `ColumnView` and `GridView` keep it too and give it
  to every folder they open or are handed, so a folder opened in the grid or the columns is sorted
  the same. A click on a title of the details tells the browser (`DetailsView::set_sort_handler`),
  which sorts the other views. `state.toml` as before (`sort`, `descending`).
- `fs::SortOrder` has `==`; `Folder::set_order` does nothing (no re-sort, no jump) when the order
  is the same.
- `MenuItem::radio` / `checked`: a panel with radio items keeps room before the labels and draws a
  dot on the checked ones.
- Tests: the submenu's items and dots, Size then Descending by keyboard and by action, the grid's
  folder re-sorted, a folder opened later in the columns sorted so, a title click of the details
  sorting the grid. Snapshot of the submenu in Nord light and dark. 277 tests.
- `memory_test`: under the sanitizers the 64 MB block is checked to show by half at least. ASan's
  allocator gives memory back on its own between the two reads; after the whole suite (some
  500 MB held) it passed by 90 kB, and this change was enough to tip it.

**Tested by the author** (2026-11-06): Sort By from empty space and from a photo in the grid,
Modified descending, another folder in the columns sorted so, a title click in the details
carried to the grid, the keyboard through the submenu, the order back after a restart.

---

## 2026-11-06 — 7.8c: 0.1.0

**Done**
- Version 0.1.0 (`nordend --version`). The README's and the site's pre-release notices now say
  0.1.0 is the first release, young software used every day by its author; the README badge
  and the site's header badge link to the release. "Now" in the README and the site's roadmap:
  tabs. The site's Umbriel bind uses the current syntax (`spawn nordend`; `spawn:nordend` is
  dropped silently by today's Umbriel) and says the install puts the icon in place too.

**Decisions**
- The tag goes on this commit; a public copy of the Void template, pinned to the tag's tarball,
  comes in the next one (it cannot be part of the commit it checksums).
- The README opens with the author's desktop screenshot (Noctalia's Nord light, the grid on raw
  files), the media title and the Wi-Fi name in the bar blurred:
  `docs/images/readme-desktop-light.webp`. A dark/light split was tried and set aside; a video
  (GIF) may come later. The guide
  badge takes the site's colours.
- After the tag (`v0.1.0` on `a21057a`): `packaging/void/nordend/template`, the same as
  void-noctalia's, on the tag's tarball; the site's install page shows how to build it with
  void-packages. Built from the tag with xbps-src: `nordend-0.1.0_1`.

---

## 2026-11-06 — Pictures in the guide

**Done**
- Eight pictures on the site's Guide page, each in Noctalia's Nord light and dark: the shortcuts
  overlay, the three views side by side, the sidebar, the info pane, Quick Look, a copy in
  progress. The site shows the ones for its own theme (`.themed`, `.for-dark` / `.for-light`);
  a click opens one full size. WebP, 1600 px wide, 15-106 kB each (1.1 MB for all sixteen);
  `loading="lazy"` but the first, so the other theme's are never fetched.
- Drawn with `nordend-snapshot` (1600x1000 at scale 1.5) on a demo home with neutral names and
  sixteen OM-1 raw files from the Piramide Vincent (Monte Rosa), checked first: mountains only, no
  GPS coordinates, and the info pane shows no serial numbers. The demo lives on DATA so the status
  bar shows a real disk. The Pages workflow copies `docs/images/*.webp` too.

---

## 2026-11-06 — 7.8b: a Void package

**Done**
- An xbps-src template `nordend` in void-noctalia (`packages/srcpkgs/nordend`, next to noctalia
  and umbriel): `build_style=cmake`, tests off, `/usr/bin/nordend`, the desktop entry, the icon
  and the licence; depends on `shared-mime-info` and `hicolor-icon-theme`, the libraries found by
  xbps-src. 1.1 MB package, 2.6 MB installed.
- `NORDEND_WERROR` (CMake option, ON by default): the package turns it off, so that a newer
  compiler's new warning cannot break it.
- The first package build failed: xbps-src builds with `-D_FORTIFY_SOURCE=2`, which marks
  `fchmodat` `warn_unused_result`, and `Eraser::empty_folder` ignored its result. Now kept in a
  `[[maybe_unused]]` local: a failure still shows as the open's error just after. The whole tree
  built with xbps-src's flags and `-Werror`: no other case. 276 tests.

**Decisions**
- The short way (the author, 2026-11-06): a Void package only; elsewhere, build from source. A
  portable Zed-style tarball, an AUR package: later. Flatpak, Snap, AppImage: no (a sandbox
  works against a file manager).

**Tested by the author** (2026-11-06): installed with `xbps-install`, the `~/.local` copy moved
aside: Super+E, the launcher with the icon, a folder opened from another app, raw thumbnails,
Quick Look, a copy, a deletion to the trash.

---

## 2026-11-06 — The site's own look

**Done**
- `site/style.css`: alpine colours taken from the icon instead of Noctalia's palette: night at
  altitude (`#0e1519`, granite surfaces) and snow (`#f7fafb`), ice as the accent (`#6dbdf0` /
  `#2a6a9e`), a warm alpenglow (`#f2906b` / `#c2562e`) only on the badge, the kicker and the
  glow behind the summit. The active page in the navigation is a bar of ice, not a filled block.
- The overview opens with faint contour lines, a kicker "Nordend · 4609 m" and, under the
  buttons, a ridge in two planes with a snowy summit (inline SVG, colours from the theme). The
  numbers "At a glance" read like an altimeter: big tabular monospace digits.
- Words: Nordend is its own project, at home on any desktop and right at home on Noctalia; "part
  of the Noctalia family" is gone from the site, the README and features. The footer says where
  the name comes from.

- Sharper corners (asked by the author): 4 px for boxes, cards, tables and screenshots, 3 px for
  buttons, navigation, code, keys and the badge (were 6-12 px and a pill).
- Accessibility: every text pair at 4.5:1 or more (the light theme's alpenglow was 4.29, now
  `#b04a24` at 5.2); button and key borders at 3:1 or more (`--outline-strong` was 2.3 dark,
  1.9 light); a focus ring on everything that takes focus; a "Skip to content" link (focuses
  `main` without touching the hash, which chooses the page); choosing a page focuses its title;
  the header buttons at least 36 px high; the badge and stat labels a little larger; the active
  page outlined in forced-colours mode; `prefers-reduced-motion` honoured.

**Decisions**
- The author asked for a site with a soul of its own, away from Noctalia's colours. Alpine (from
  the icon and the name) chosen over a darkroom look (18 % grey, safelight red). Still only system
  fonts, no CDN.

**Tested by the author** (2026-11-06): both themes from the screenshots, before committing.

---

## 2026-11-06 — 7.8a: Nordend's own icon

**Done**
- `data/nordend.svg`: a folder with the Nordend's summit on its front, in one hue ("ice":
  `#6dbdf0` front, `#3f8cc4` tab, `#2a6a9e` mountain, white snow), no background tile.
  Installed as `share/icons/hicolor/scalable/apps/nordend.svg`; the desktop entry says
  `Icon=nordend` (was `folder`). `desktop-file-validate` clean.
- The site: favicon and the brand mark in the header are the icon (the two squares are gone);
  the Pages workflow copies it from `data/`. The README shows it above the title.

**Decisions**
- Three concepts drawn (folder with summit, ridge under a moon, two folders as the site's mark);
  the author chose the folder with the summit. On a Noctalia-blue tile first; the author asked
  for none: Nordend works well with Noctalia but is its own project. Four hues tried on dark and
  light backgrounds at 200/48/24 px; a graphite mountain vanished on dark backgrounds, so the
  mountain is the hue's dark tone. The author chose "ice".
- An icon that follows the desktop's accent is not possible with an installed icon (docks read
  the file themselves; only `-symbolic` icons are recoloured, in one colour): ROADMAP 9.2, after
  0.1.0, Nordend writing a recoloured copy into `~/.local/share/icons` (whether docks reload it
  *to be verified*).

**Tested by the author** (2026-11-06): after reinstalling, the icon shows in Noctalia's launcher.

---

## 2026-11-06 — 7.7: a week of daily use, closed

**Done**
- The author used Nordend as the file manager from 2026-11-01 to 2026-11-06 (Super+E, default
  for folders), with 7.7a (terminal in a folder) and 7.7b (rubber band) added along the way.
  No bugs found.

**Measured** (`~/nordend-rss.log`, `smaps_rollup` of each `nordend` every 10 minutes, 112 lines,
about 15 sessions, some 20 hours open)
- Anonymous memory 23.7-26.6 MB per session, flat: the longest session (2026-11-04, 5 hours)
  stayed at 26 512 kB to the byte; short sessions grow 0.1-1 MB in the first minutes (thumbnails,
  glyphs), then stop. Two windows at once behave like two single sessions.
- One step: 2026-11-05 11:40-11:50, 24.7 → 48.9 MB anonymous (RSS 108.8 → 135.3 MB), then flat
  to the end (49.8 MB). The author was scrolling Quick Look through a folder of Olympus ORFs, then
  opened one and closed it. `QuickLook::close()` drops every slot; what stays is memory malloc
  keeps in the workers' arenas after the decode buffers under the 16 MB mmap threshold are freed:
  the same residue as 6.3c (111 → 143 MB after Quick Look in the author's run). A step, not a
  slope: not a leak. `malloc_trim(0)` was already measured to give little back after Quick Look
  (6.3b): not added.

**Decisions**
- The memory logger (`~/.local/bin/nordend-rss-log`, user service `~/.config/service/nordend-rss`)
  stays on through development, so every installed version is watched (the author's choice).

**Tested by the author** (2026-11-06): the whole week; the log read together.

---

## 2026-11-04 — 7.7b: rubber-band selection with the mouse

**Done**
- A press on empty space moved past 6 px (the drag threshold) draws a band; what it touches is
  selected, with Ctrl added to what was selected. Empty space: below the entries, a row away
  from its icon and name (8 px past the name still count as the name), in the grid a cell away
  from its icon and name, the margins. A press on the icon or name drags as before; a click
  (no move) anywhere on a row still selects it.
- Views: `ViewHit::on_item` (each view notes at draw where each visible row's name ends, from
  `draw_text`'s width; the grid notes each cell's name width), `band_lane()` (the entries' area
  and scroll), `band_entries()` (the rows a band touches: `rows_touched()` for the lists, the
  cells' highlight boxes for the grid), `band_scroll()`. `Folder::select_band(base, rows)`
  leaves the cursor where it is.
- `Browser`: the band is kept in content coordinates, so it stays anchored while the list
  scrolls; beyond the top or bottom of its list a 16 ms timer scrolls it (2-40 px a tick, a pixel
  per 4 px beyond) and selects as it goes; the timer exists only then (idle stays 0 CPU). In
  the column view the band stays in the column it started in. The band is drawn in the text's
  colour (12 % fill, 85 % border): over the selection it makes, the accent would vanish.
- Ends on release, on leaving the window, on any press, and when an overlay takes the pointer.
- `nordend-snapshot --band X1,Y1,X2,Y2`.
- Tests (drawn with `Offscreen`): in all three views, from empty space, from beside a name,
  Ctrl adding, a click beside a name; scrolling a list of 80 by holding below it. The drag
  tests now press on the name. 276 tests, ASan and TSan clean.

**Decisions**
- A click beside a name keeps selecting the row (as before): only a drag from there draws a
  band, so nothing the author is used to changes.
- No cursor move during the band: in the column view a cursor on a folder would open columns
  while dragging.

**Tested by the author** (2026-11-04): all three views, from empty space and beside a name,
Ctrl, scrolling past the bottom, a drag from a name and a click beside one as before.

**Open**
- After a band from empty space there is no cursor: arrows start from the first entry.

---

## 2026-11-04 — 7.7a: open a folder in a terminal, Open With on folders

The week of daily use so far: the memory log has 9 lines (Nordend was rarely open for 10
minutes in a row); two windows for half an hour on 11-02 stayed at 24.7 MB anonymous (+40 kB):
no sign of a leak, too little to call it. The author asked for three comforts: 7.7a (this), 7.7b
(rubber-band selection), and tabs, agreed as 9.1, the first step after 0.1.0 (the DESIGN keeps
them out of the MVP).

**Done**
- `F4` (views and Quick Look) opens a terminal in the cursor's folder when the cursor is on one,
  else in the folder shown. Context menu: "Open in Terminal" on a folder and on empty space.
- `platform::terminal_alone()`: the terminal `in_terminal()` would use (`$TERMINAL` with its own
  options, `xdg-terminal-exec`, the known ones), with nothing to run; the shell starts in the
  folder through the launcher's working directory. `Opener::open_terminal()` works it out on a
  worker and reports "Opening sub with foot…" or "No terminal found: set $TERMINAL".
- Open With on folders too: the selected entries of the cursor's kind (folders with a folder,
  files with a file); `Opener::open` keeps folders when an application is chosen.
- Tests: `terminal_alone` against fake programs; the opener starts a fake foot that writes `pwd`;
  F4, the menu items and Open With on a folder in `context_menu_test`. 274 tests.

**Decisions**
- The cursor's folder wins over the folder shown, as agreed: in the column view the cursor on
  `Documents` already shows its contents next to it.
- No new key for the terminal program: `$TERMINAL` stays the way to choose it.

**Tested by the author** (2026-11-04): F4 on a folder and on a file, both menu items, Open With
on a folder, the message without a terminal.

**Open**
- Terminals other than foot not tried here (kitty, ghostty, gnome-terminal...): they should
  start in the working directory they are given, *to be verified*.
- Open With on a folder lists Nordend itself first (the default for `inode/directory`): a second
  window on that folder.

---

## 2026-11-01 — The site, and a shorter README

**Done**
- `site/`: Nordend's site for GitHub Pages, `https://pankaspe.github.io/nordend-fm/`. One HTML
  page with nine tabs (Overview, Install, Guide, Shortcuts, Configuration, Performance,
  Compatibility, Architecture, Roadmap), chosen by the address (`#guide`, `#guide/quick-look`:
  every section has a link to share); plain HTML, CSS and about 140 lines of JavaScript, no
  framework, no web fonts, nothing loaded from other servers. Dark and light themes from
  Noctalia's default palette, the system's choice unless the visitor picks one. Without
  JavaScript every page shows, one after the other. Narrow screens get the tabs as a row.
- The shortcuts page is drawn from JSON data in the page; `site_test.cpp` checks that the data is
  exactly `views::shortcuts()` (and prints what it should be when it is not).
- `.github/workflows/pages.yml`: on a push touching `site/` or `docs/images/`, assembles the
  site with the two screenshots and deploys it (actions/checkout v7, configure-pages v6,
  upload-pages-artifact v5, deploy-pages v5: the latest releases today).
- README: a "Read the guide" button under the pre-release notice; the benchmarks down to the
  four targets; the long feature list and the shortcut table moved to the site (one place to
  keep them); building in a few lines with `cmake --install`; 224 lines instead of 384.
  Numbers brought up to date: 0.7 ms (not 0.8) for the first entries, Quick Look "under 100 ms"
  (the "about 50 ms" was the decoding alone), 272 tests.

**Measured**
- A clean Release build: 54 s for everything, 33 s for `nordend` alone (Ryzen 7 5700G, 16
  threads): "about a minute on an 8-core CPU".
- The site looked at in headless Firefox (own profile, no window): overview, shortcuts and
  performance at 1440 px, the guide and overview at 420 px, dark and light.

**Decisions**
- No Alpine (the author's choice): a few lines of JavaScript do the tabs.
- The site carries what users need; the markdown in `docs/` stays the detailed record, linked
  from the site.

**Open**
- The site states facts checked today (keys, files, numbers); the shortcuts are tested, the rest
  is kept by hand.

---

## 2026-11-01 — Installed for daily use (ROADMAP 7.7)

**Done**
- `data/nordend.desktop` (app id `nordend`, `Exec=nordend %f`, `MimeType=inode/directory`,
  category FileManager), installed by `cmake --install` into `share/applications`. Valid for
  `desktop-file-validate`. Icon `folder` (Adwaita) until Nordend has its own (7.8).
- On the author's machine: `cmake --install build --prefix ~/.local --strip`; Super+E starts
  Nordend in Umbriel (Super+Shift+E stays nnn), and folders open in Nordend
  (`inode/directory=nordend.desktop`: `xdg-mime`, `gio mime` and `nordend-open --default` agree).
- Memory log for the week: a turnstile user service runs a fish loop writing `Rss` and
  `Anonymous` of each `nordend` every 10 minutes into `~/nordend-rss.log`.

---

## 2026-11-01 — Every DESIGN target measured (ROADMAP 7.6)

**Done**
- `nordend-bench-start [-n RUNS] [--keep-open] -- COMMAND...` (development tool, not
  installed): runs `nordend` RUNS times with `NORDEND_STARTUP_NS` (steady clock before the exec)
  and `NORDEND_STARTUP_QUIT`; prints each run's steps and their median, min and max. A run
  silent for 10 s is stopped. Nordend's info lines are not echoed, its warnings are.
- `nordend`: with `NORDEND_STARTUP_NS` set, prints `startup: STEP MS` after `main`, `display`,
  `egl`, `fonts`, `portal`, `window`, at the first draw, when the compositor shows the first
  frame and when it shows the first frame drawn with the folder loaded; with
  `NORDEND_STARTUP_QUIT` it then closes. Without the variable nothing changes.
- `ui::Window::set_frame_shown_handler`: called on each frame callback.
- Quick Look logs at debug level how long an entry's thumbnail and sharp image took from the
  request to the frame that uploads them ("made in advance" for a neighbour).
- `nordend-snapshot` reads `NORDEND_LOG` (it did not), so those timings show offscreen too.

**Measured** (details in `benchmarks.md`, "DESIGN targets")
- Cold start to the first frame on screen: 128.3 ms median of 5 after `drop_caches` (target
  < 150); warm 64.5 ms. EGL and radeonsi are the largest step (27 ms warm, 46 ms cold).
- Started inside a 10k folder: first frame 69 ms, the whole list on screen 86 ms after exec.
  Directory benchmark again: first chunk 0.68 ms, complete 16.6 ms (19.5 on 2026-10-26).
- Quick Look of an OM-1 ORF, Space to the sharp image: 82-96 ms offscreen (median 86, 15 runs),
  92 ms on screen. Target < 100: met, with the smallest margin of the four.
- Scrolling at 2560x1440 in every view, all selected included: worst frame 3.7 ms of a 13.3 ms
  frame (75 Hz).
- Binary 3 668 120 bytes (3 062 648 stripped), 21 libraries; 271 tests.

**Decisions**
- "Cold" means `sync; echo 3 > /proc/sys/vm/drop_caches` before each run; the cache directory
  is on disk (`/var/tmp`), so Mesa's shader cache is dropped too, as after a reboot.
- The scrolling target is judged on the offscreen benchmark, as DESIGN words it; dropped frames
  on screen are not counted.
- Headless weston (llvmpipe) numbers are kept only for comparison: software rendering.

**Open**
- Quick Look's 30 ms over the 2026-10-27 `load_to_fit` time: the neighbours decoded at the same
  time are the suspect, *to be verified* (e.g. `nearby` jobs started after the current one).
- Once, headless weston stopped sending frame callbacks to every client until restarted; not
  seen again in about 30 runs.

**Tested by the author** on Umbriel (2026-11-01): `nordend-bench-start` warm (home and the 10k
folder), five cold runs, and Quick Look with `NORDEND_LOG=debug`.

---

## 2026-10-31 — No settings panel; `nordend --about`

**Done**
- The author decided Nordend should be ready as it starts, with the system's fonts and the
  desktop's colours, and the few settings in `~/.config/nordend/config.toml`: the settings
  panel (7.5c), its font list (7.5c-bis), the gear in the status bar and Ctrl+, are gone. Not a
  `git revert` (later commits touched the same files): every file the panel touched and nothing
  after it is back to its state before `dd60141`; `Browser` is byte-identical to it.
- Removed with it: `core::set_config_value`/`config_problem`/`toml_string` (config_edit.cpp),
  `ui::installed_font_families`, `Theme::from_file()`/`desktop()`, `Window::gl_renderer()`,
  `Browser::copy_text()`, the "settings" icon (the outline subset is 47 icons again, sha256 as
  before the panel).
- Kept: `config.toml` followed while Nordend runs (the double click at once, the fonts at the
  next start), `--check-config`, `platform::system_about()` with its tests, `NORDEND_COMMIT`.
- `nordend --about` prints "Label: value" lines, aligned: Nordend's version and commit,
  distribution, kernel, desktop, Noctalia and Umbriel when they run, and the GPU, read from a
  surfaceless OpenGL ES context (`ui::Offscreen`, 1x1) since there is no window.

**Measured**
- Binary 3 663 616 bytes (3 058 552 stripped); the README said 3.64/3.04 MB; 21 libraries.
- 271 tests (14 fewer: the panel's, config_edit's and one pointer-shape test), passing in
  `build`, `build-asan` and `build-tsan`.

**Decisions**
- The file keeps its name, `config.toml` (DESIGN, and the author's existing file).
- DESIGN's MVP line now says "no settings panel" (consequence of the author's decision).

**Open**
- How to explain config.toml to users (what each key does, where it lives): later, as agreed.
- `--about` on GNOME and Plasma: at the next VM session.

**Tested by the author** on Umbriel (2026-10-31): no gear, Ctrl+, does nothing, no "Settings"
line in the shortcuts overlay, `--about` and `--check-config` print as expected, an edited
config.toml is reloaded.

---

## 2026-10-31 — "About", in the settings (ROADMAP 7.5d)

**Done**
- The settings panel has two pages, "Settings" and "About": Left and Right or a click on the
  titles turn them; it always opens on "Settings".
- "About": Nordend's version and commit, distribution, kernel, desktop, Noctalia's and
  Umbriel's versions when they run, and the GPU; lines whose value is not known are left out.
  Enter or "Copy" puts "Label: value" lines on the clipboard (the text types Nordend offers),
  "Copied: paste it into a bug report" beside the button. MIT license and the repository's
  address at the bottom.
- `platform::system_about()` with `distribution_name` (os-release's PRETTY_NAME, else NAME),
  `version_from` ("noctalia v5.4.0" -> "5.4.0"), `running(name)` (a process of this user by
  /proc/PID/comm, its /proc/PID/exe) and `version_of(program)` (posix_spawn, stdout through a
  pipe, nothing else inherited, 1 s at most, killed past it). Called on a worker the first time
  the panel opens, like the font families.
- Noctalia and Umbriel are asked through /proc/PID/exe of the process running: never another
  copy, which with `--version` might do something else. Their sources were read first (5.4.0
  and the pinned Umbriel commit): `--version` prints and exits in both.
- The commit comes from git when CMake configures (`NORDEND_COMMIT`), configured again after
  every commit (`.git/logs/HEAD` is a configure dependency); none outside a checkout.
- `Window::gl_renderer()`; `Browser::copy_text()` empties Nordend's own file clipboard, since the
  clipboard holds text now.
- Tests (285): os-release, version lines, the test's own process found by name, `version_of`
  with a good, a failing, a missing and a slow program (killed in time); the pages, the lines
  read on a worker, the copy offered as every text type; the drawn test looks for a value's hand
  right of the page titles.

**Measured**
- `noctalia --version` and `umbriel --version` together: 31 ms (on a worker).
- Binary 3.64 MB, 3.04 MB stripped (+50 KB).

**Decisions**
- Two pages rather than one long card: everything on one would not fit a 640 px window.
- No "+" for uncommitted changes after the commit: it would only be as fresh as the last
  configure.

**Tested by the author** (2026-10-31, Umbriel): the lines right (version and commit, Void Linux,
the kernel, umbriel, Noctalia 5.4.0, Umbriel 0.1.0, the GPU), the pages turned by keys and by
clicks, "Copy" pasted elsewhere, Ctrl+V in a view then finds nothing of Nordend's to paste.

**Open**
- "About" on GNOME and Plasma (no Noctalia or Umbriel lines) to look at in the VM.
- `core::read_text_file` reads by the size the file states: /proc's files read empty with it
  (comm is read with a stream instead).

---

## 2026-10-31 — Choosing a font from the installed ones (ROADMAP 7.5c-bis)

**Done**
- `ui::installed_font_families()`: fontconfig's list (family, spacing), one entry per name (case
  aside), fixed-width when one of its fonts is (`FC_MONO`, `FC_CHARCELL`), sorted by name.
- `views::font_suggestions()`: the families whose name holds what is typed before any ':', case
  aside; "sans-serif", "serif", "monospace" first (tagged "default"), then for the monospace font
  the fixed-width ones (tagged "mono"), names starting with the text before those holding it.
  `views::font_known()`: installed, generic, or a pattern with properties.
- The panel: while a font is changed, up to 8 suggestions under the field (every family until
  something is typed), a scroll bar when there are more, the wheel scrolls them; Down and Up pick
  (above the first: back to the text), Enter or a click keeps the one picked. Enter on a name
  not installed is refused ("not installed: choose one from the list"); "No installed font
  matches" when nothing does. Numbers get no list.
- `Browser` reads the families on a worker the first time the panel opens (`Settings::families`,
  guarded by `alive_`).
- Tests (280): ordering, filtering, patterns, `font_known`, the panel's list, picking, refusing,
  a pattern kept, no list for numbers.

**Measured**
- `fc-list : family spacing` (the same fontconfig call), 317 families, warm cache: 6-7 ms.
- Binary 3.58 MB, 2.99 MB stripped (+60 KB).

**Decisions**
- Suggestions while typing rather than a plain drop-down: 317 families are too many to scroll,
  and a fontconfig pattern can still be written.
- Until the families are read (a few milliseconds after the panel opens) any name is accepted:
  there is nothing to check it against. Seen in a snapshot, whose keys arrive all at once.

**Tested by the author** (2026-10-31, Umbriel, on a copy of the config): the list, filtering,
picking with the arrows and with a click, the fixed-width families first for the monospace font,
a name not installed refused, "No installed font matches", the font chosen in use after a
restart.

**Open**
- Families are listed by name only, not drawn in their own font (the renderer has one family).

---

## 2026-10-31 — The settings panel (ROADMAP 7.5c)

**Done**
- `views::SettingsPanel`: a card over the window (Ctrl+, or the new gear in the status bar, right
  of "?") with the fonts (interface, monospace), the double click (time, distance) and where the
  colours come from (read-only). Up, Down and Tab choose, Enter, F2 or a click change a value in
  a `TextField` (Ctrl+V pastes its first line), Enter keeps and Escape drops; Escape, Ctrl+, or a
  click outside the card close it. A refused value stays in a red field with the reason under
  it; a font that differs from the one in use says "Fonts change at the next start".
- What it reads and writes goes through `views::Settings`, a struct of functions wired by main
  and faked in tests, like `Devices`.
- `core::set_config_value`: config.toml edited as text, only the value on its line (indentation,
  spacing, a comment after it kept; literal, multi-line and escaped strings, quoted and dotted
  keys; a missing key at the end of its section, a missing section at the end).
  `core::toml_string`; `core::config_problem` gives the parser's own message for a key ("must be
  between 100 and 2000, got 5").
- main: a change is applied to the file's text, parsed, refused with that message (a file that
  no longer parses is never overwritten), else written atomically on the loop thread (a small
  file, one write per Enter, in order) and applied. config.toml is watched (`core::FileWatcher`)
  and reloaded when edited by hand: the double click changes at once (`Seat::set_multi_click`),
  the fonts stay until the next start.
- `Theme::from_file()`/`desktop()` for the colours' line; the icon `settings` added to the Tabler
  subset (`icons.txt`, `scripts/make-icon-font.sh`, sha256 in THIRD_PARTY.md, 14 KB now);
  `Command::settings` in the one key table, README's table too.
- Snapshot: the panel over a config.toml in memory (keys "Ctrl+comma,...").
- Tests (277): editing kept comments, spacing and other keys, missing keys and sections, dotted,
  quoted, escaped and multi-line values, `toml_string`, `config_problem`; the panel's keys
  (opening, closing, choosing around the ends, changing, a refused value, a non-number, Escape),
  paste; drawn: the gear opens it, hands on its values, the text cursor in the one changed. The
  every-key test's reset closes the panel too.

**Measured**
- Binary 3.51 MB, 2.93 MB stripped (+50 KB).

**Decisions**
- Fonts at the next start: changing them live would rebuild the font system, the glyph atlas,
  the icon set and every view's text; the panel says so.
- Writing on the loop thread, not a worker: one small file per Enter, in order, and its error
  reaches the panel at once.

**Tested by the author** (2026-10-31, Umbriel, on a copy of the config with `--config`): the gear
and Ctrl+, open the panel; 5 refused with its reason, 800 kept and the double click slower;
"DejaVu Serif" kept with the note, and in use after a restart; the comments still in the file; a
hand edit reloaded at once; a broken file kept the settings in use, with its line in the log.
The author asked for help choosing fonts: 7.5c-bis.

---

## 2026-10-31 — Pointer shapes: a hand and a text cursor (ROADMAP 7.5b)

**Done**
- `Browser` decides the pointer's shape in one place: `cursor_at(x, y)` asks what is under the
  pointer, in the order `take_pointer()` hands events out (the conflict and confirm cards first,
  the menu, Quick Look and the shortcuts give an arrow, the name field, then the sidebar, the
  path, the status bar, the info pane). A hand on what a click acts on: the status bar's view
  buttons, "?" and a transfer's ×, the path's crumbs except the folder shown, a disk's eject icon,
  "More details", the cards' buttons and the conflict's "for the rest" checkbox. The text cursor
  over a name being edited (rename, new folder). The context menu's rows stay arrows, as menus
  are.
- The views' and the sidebar's resize arrows come through `Browser` now and win over the shape
  under the pointer; a shape is sent to the seat only when it changes, and sent again after the
  pointer left (the title bar, another window may have set one meanwhile).
- `clickable_at(x, y)` on `StatusBar`, `Breadcrumb`, `Sidebar` (with `eject_at`, shared with its
  hover), `InfoPane`, `ConflictDialog`, `ConfirmDialog`; `InfoPane::forget_layout()` at each
  frame so a hidden pane has no "More details" left; `ui::Rect::contains`.
- Tests (267), drawn offscreen: hands and arrows found zone by zone (status bar, path, sidebar
  with a mounted external disk, "More details" once read), the text cursor over the field, a
  card taking the pointer, the menu's arrows, shapes sent only on a change, the resize arrow
  first, sent again after leaving.

**Measured**
- Binary 3.46 MB (+9 KB).

**Decisions**
- No hand on the title bar's buttons (Nordend's own, client-side): title bars keep the arrow, as
  GTK's and KWin's do; no hand on the sidebar's places and the views' entries, which are lists.

**Tested by the author** (2026-10-31, Umbriel, with cursor-shape-v1 and with
`NORDEND_FORCE_XCURSOR=1`): the hand, the text cursor and the arrows where expected, the same in
both.

---

## 2026-10-31 — Remembering how Nordend was left (ROADMAP 7.5a)

**Done**
- ROADMAP: 7.5 split into four steps (7.5a state, 7.5b pointer shapes, 7.5c settings panel on
  Ctrl+, and a gear button in the status bar, 7.5d About), approved by the author; DESIGN's
  "portal" out of the MVP now says "portal FileChooser".
- `views::State`: the view, hidden files, the sidebar (shown, width), the info pane (shown,
  width, "More details"), the details' order and the widths of size, modified and type, the
  window's size and whether it was maximized. `parse_state`/`format_state`/`load_state` in
  `$XDG_STATE_HOME/nordend/state.toml` (`~/.local/state`): a value of the wrong type or out of
  range keeps its default with a problem line, unknown keys are fine (a newer Nordend), a broken
  file is the defaults.
- `views::StateKeeper`: writes a second after the last change (a dragged edge writes once), on a
  worker, one write at a time; `flush()` at exit. Writes carry a generation and go through a
  mutex, so a worker's write still running at exit cannot land after the final one.
- `Browser::state()`/`apply()` (before `open()` the view comes up in the remembered mode);
  `DetailsView::set_sort`/`widths`/`set_widths` (each column keeps its minimum);
  `WindowOptions::maximized` (asked before the first commit).
- main: the state is read before the window opens and noted after every frame drawn (every change
  shows in a frame, and nothing is drawn at rest, so idle stays at 0 CPU). The window's size is
  taken only when it is neither maximized, fullscreen nor tiled, so a maximized window comes back
  to its own size.
- Tests (263): written and read back, defaults, bad values with their lines, a broken file,
  unknown keys, the path under `XDG_STATE_HOME`, the browser giving back what it was given (and
  the columns' minimum), the keeper writing once after two changes and flushing at exit.
  `tests/main.cpp` points `XDG_STATE_HOME` into the test root too.

**Measured**
- Binary 3.45 MB, 2.88 MB stripped (+30 KB).

**Decisions**
- Not remembered: the last folder (Home or the folder given, as Nautilus and Dolphin) and the
  back/forward history (decided with the author).
- State, not configuration: its own file under `XDG_STATE_HOME`, never mixed into `config.toml`.

**Tested by the author** (2026-10-31, Umbriel, `XDG_STATE_HOME` in /tmp): details sorted by
date, hidden files, the sidebar's and the info pane's widths and "More details" come back after a
restart; a broken file gives the defaults with one warning line. The window is tiled on Umbriel,
so its size is the compositor's and is not taken, as meant. Maximizing there does not reach any
client as the xdg maximized state (not Nordend, Firefox nor foot), so "maximized" could not be
tried on Umbriel (*to be verified* in Umbriel's code; a buglog candidate, upstream only with the
author's OK).

**Open**
- "Maximized" untried: on a floating window in the VM (GNOME, Plasma) with 7.5b or 7.5c.
- Two Nordend windows: the last one to write wins (one window per process today).

---

## 2026-10-31 — The desktop's colours without Noctalia (ROADMAP 7.4, last item)

**Done**
- `theme::desktop_palette(Desktop)`: a palette from light/dark and an accent colour, in OKLCH.
  Neutral surfaces (a trace of the accent's hue, chroma 0.005) around GNOME's grey in dark mode
  and near white in light mode; the accent for `primary` (selection, icons, links, the context
  menu's row), its lightness kept in a band and pushed further until it reads on
  `surface_container_high` (4.5:1); `on_*` colours chosen light or dark by contrast; error roles
  a fixed red. No accent: GNOME's blue `#3584e4`.
- `theme::Theme::set_desktop()`: `colors.toml` wins; without it the desktop's palette, otherwise
  the default one. Removing the file falls back to the latest desktop look.
- `platform::DesktopPortal`: the freedesktop portal's Settings on the session bus, namespace
  `org.freedesktop.appearance` (`color-scheme`, `accent-color`), read before the window opens
  (synchronous, 300 ms at most; later, the answer comes through the loop) and followed through
  `SettingChanged`. `appearance_from()` checks the values: color-scheme 0 (no preference) reads as
  light, as GTK and Qt apps read it; an accent outside 0..1 is the portal's "unset".
- `platform::BusLoop`: the D-Bus connection served by the event loop, moved out of `Udisks` so
  the portal uses it too.
- `ui::Color` gains `operator==`. Snapshot: `--desktop dark|light[,#rrggbb]`.
- Tests (256): contrast of every text/fill pair for 15 accents (GNOME's nine, Plasma's blue,
  white, black, grey, pure yellow, pure blue) in both modes; surfaces step the right way; the
  accent's hue; the portal's values; Theme's rules (file wins, back to the desktop, nothing to
  redraw on the same look); a read-only test against the real portal (here: dark, no accent).

**Measured**
- Reading the portal at start: about 1 ms (`docs/benchmarks.md`).
- Binary 3.41 MB, 2.85 MB stripped (+70 KB); no new library.
- Snapshots looked at: dark and light with GNOME's blue, dark orange, light green, light purple
  and red, with the title bar, the context menu and the rename field.

**Decisions**
- Selection, icons and the context menu's row take the accent, as GNOME and Plasma show them.
- The first test showed brownish dark surfaces with an orange accent: the tint went from 0.008 to
  0.005 chroma.

**Tested by the author** (2026-10-31, the VM): on GNOME 48.7 light/dark and the accent change
Nordend at once (screenshot: light, orange accent); on Plasma 6.7.5 too, Breeze and Breeze Dark and
the accent (screenshot: dark, the wallpaper's salmon accent, the same KWin shows), so
xdg-desktop-portal-kde gives `accent-color`. On Umbriel, with `colors.toml`: Noctalia's colours
as before. In the VM, GNOME Settings stays dark in light mode because of the Noctalia
`~/.config/gtk-4.0/gtk.css` there, not Nordend's business.

**Open**
- Plasma's colour schemes (Breeze's own greys and their many roles) are not read: only light/dark
  and the accent, through the portal.

---

## 2026-10-31 — Nordend on GNOME, Plasma, niri and Umbriel (ROADMAP 7.4, the author's tests)

**Done**
- The checklist of `docs/compatibility.md` on four desktops, results in its table.
- VM: the Void VM `nordend-dev` instead of an Arch one (decided with the author): GNOME 48.7 and
  Plasma 6.7.5 installed in it, Nordend built and tested there over SSH (copied with
  `git archive`). Snapshot `before-7.4` taken before the install.
- niri 26.04 nested in Umbriel on the host (in the VM niri needs virgl, which BUG-017 of
  void-noctalia rules out), with a minimal config without `prefer-no-csd`; removed afterwards.

**Measured**
- 249 tests pass in the VM (1 skipped: udisks2), Release build with GCC 14.2.

**Decisions**
- New item in 7.4, asked by the author before going public: without `colors.toml`, follow the
  desktop's light/dark and accent colour through the freedesktop portal.

**Tested by the author** (2026-10-31): every point of the checklist on GNOME (Nordend's title
bar; fractional scale after enabling `scale-monitor-framebuffer`), Plasma (KWin's bar), niri
(Nordend's bar; scale not tried, nested) and Umbriel (Umbriel's bar). Files opened in the apps of
each `mimeapps.list`, focused.

**Open**
- The definition of `nordend-dev` had virgl on again (`accel3d='yes'`, SPICE GL), although
  BUG-017 records it as removed; every older snapshot carries it. It booted with virgl for a few
  seconds before it was noticed and forced off; no amdgpu fault in the host log (*to be verified*
  as root). The definition and `before-7.4` are now without 3D; reverting an older snapshot
  brings virgl back (to note in void-noctalia, with the author's OK).
- KWin shows the generic Wayland icon: no desktop entry for the app id `nordend` (with the 0.1.0
  package, 7.8).
- Colours follow only Noctalia's palette (the new 7.4 item).

---

## 2026-10-30 — A title bar of its own where the compositor draws none (ROADMAP 7.4, third part)

**Done**
- `ui::Window`: client-side decorations when the compositor has no xdg-decoration, when it
  configures client-side mode, or with `NORDEND_FORCE_CSD=1` (then no decoration object is made,
  which tells the compositor the client decorates). `client_decorations()`, `state()` (maximized,
  fullscreen, activated, tiled per edge from configure; minimize, maximize and window menu from
  `wm_capabilities`, all assumed without it), and `move`, `resize(Edge)`, `minimize`,
  `toggle_maximized`, `show_window_menu`, `close`.
- `views::TitleBar`: the bar (36 px, `surface_container_low` like the sidebar, a hairline under
  it), the title centered on the window and never under the buttons, minimize / maximize
  (restore, `squares`, when maximized) / close at the right, close red under the pointer;
  buttons act on release over them; a press on the title moves the window, a double click
  maximizes, a right click opens the compositor's window menu; a 5 px band inside the window's
  edges (16 px at the corners) resizes it, not when maximized or fullscreen nor on a tiled edge.
  The actions go through `WindowControls`, a struct of functions wired by main (faked in tests).
  Fullscreen: no bar.
- `CursorShape` gains the eight edge and corner arrows (cursor-shape-v1 and xcursor names).
- main: the bar is drawn above the Browser, which gets the rest; the bar sees the pointer first,
  except while a press that started in the content is held; the Browser hears a leave when the
  bar takes the pointer.
- Snapshot: `--csd STATE` (`normal`, `maximized`, `inactive`, `no-minimize`, `hover-close`,
  `hover-maximize`, `press-close`).
- `docs/compatibility.md`: what depends on the compositor, what each desktop should give, the
  checklist for the author's sessions, and a results table.
- Tests (250): what is where (title, buttons, every edge and corner, content), maximized, tiled,
  fullscreen and no minimize; buttons act on release over them only, holding the pointer while
  pressed; drag, double click, right click, resize; the edges' cursors and back to the arrow.

**Measured**
- Headless weston 15 (`--renderer=pixman --debug`), screenshot by `weston-screenshooter`: the
  window with Nordend's bar, the title dimmed (weston activates no window without a seat), the
  three buttons, the Browser below unchanged. Snapshots in every state at scale 1 and 1.5 sharp.
- Binary 3.34 MB, 2.78 MB stripped.

**Decisions**
- The resize band is inside the window: GTK puts it outside, in an invisible margin, which needs
  extra surfaces (or a transparent margin around the buffer) and an offset window geometry.
  Inside, 5 px of the window's edge resize instead of reaching what is there.
- The move starts on the press (as weston's own clients do), not after a few pixels of drag.

**Tested by the author** (2026-10-30, Umbriel, `NORDEND_FORCE_CSD=1`, also with
`NORDEND_FORCE_XCURSOR=1`): move, double click, the buttons, the window menu, the edges and
their arrows, the clicks under the bar, and nothing changed without the variables. But Umbriel
drew its own title bar too (two bars): `NORDEND_FORCE_CSD` then made no decoration object, and
Umbriel decorates such windows anyway, although the protocol says that without a negotiation
"clients continue to self-decorate as they see fit" (a candidate for `buglog.md`, *to be
verified* in Umbriel's code, upstream only with the author's OK). Fixed on our side the same
day: the variable now asks for client-side mode through xdg-decoration and the compositor's
answer decides, so there can never be two bars. Tried again by the author: Umbriel answers
client-side and only Nordend's bar shows; without the variable, only Umbriel's. Umbriel's
behaviour is NB-003 in `buglog.md`.

**Open**
- No rounded corners, no shadow.
- The title is always "Nordend" (the folder shown could go in it).
- Umbriel decorates windows that make no decoration object (NB-003, suspected).

---

## 2026-10-30 — A cursor of its own without cursor-shape-v1 (ROADMAP 7.4, second part)

**Done**
- `ui::XcursorFallback` (`ui/xcursor.hpp`): when the compositor has no cursor-shape-v1 (GNOME,
  weston), the pointer's image comes from the xcursor theme through `libwayland-cursor`:
  `XCURSOR_THEME` (its "default" without it), `XCURSOR_SIZE` (24 unless a number from 1 to 256),
  loaded at the window's scale rounded up and reloaded when it changes, on a surface of
  Nordend's own set with `wl_pointer.set_cursor` on every enter and every shape change.
- Size: with viewporter the surface gets the image's logical size whatever size the theme has
  (Adwaita has no 72 px cursor for 24 px at scale 3); without it the buffer scale is used only
  when it divides the image, else the image shows at scale 1 (too large, but no protocol error).
- Names: each shape tries the cursor spec's CSS name first, then the X11 names older themes use
  (`pointer`, `hand2`, `hand1`...). Animated cursors (`wait`, `progress`) show their first frame.
- `Display` binds `wl_shm`; `Seat` picks cursor-shape-v1 or the fallback when the pointer
  appears, and takes the window's scale from main's draw handler (`set_cursor_scale`).
  `NORDEND_FORCE_XCURSOR=1` uses the fallback even with cursor-shape-v1, to try it on Umbriel.
- Links `wayland-cursor` (part of `wayland-devel`, same package as `wayland-client`).
- Tests (245): every shape has a CSS name then an X11 one, `XCURSOR_SIZE` parsing, every
  shape's first name present in the installed Adwaita theme.

**Measured**
- Binary 3.32 MB, 2.76 MB stripped (+15 KB); 21 direct libraries (+ `libwayland-cursor`).
- Headless weston (`--backend=headless`) offers no `wl_seat`: no pointer, so the fallback cannot
  be exercised there. Verified by the build, the tests and, for the author, on Umbriel with
  `NORDEND_FORCE_XCURSOR=1`.

- Under headless weston the ASan build reports a 128-byte leak in an unloaded module (no
  symbols; most likely Mesa's software driver, as EGL finds no device there). The commit before
  this step, built the same way, leaks the same 128 bytes: not ours, not on Umbriel.

**Tested by the author** (2026-10-30, Umbriel with `NORDEND_FORCE_XCURSOR=1`): Adwaita at 24 px
and scale 1 in the log, the arrow, the double arrow on the column dividers, 48 px with
`XCURSOR_SIZE=48`, and the normal path unchanged without the variable. The test plan also asked
for a hand over buttons and a text cursor while renaming: a slip of the copilot's, Nordend never
asks for those shapes (only `standard` and `ew_resize`, with cursor-shape-v1 too).

**Open**
- The hand and text shapes exist in `CursorShape` but no view uses them (to propose).
- The busy cursors are not animated (first frame only).
- On integer scale without viewporter a theme without the exact size shows a larger cursor.

---

## 2026-10-30 — Nordend's own icon font (ROADMAP 7.4, first part)

The author approved vendoring a subset of the official Tabler Icons font and Arch as the VM
distro for the compatibility tests.

**Done**
- `third_party/tabler-icons/`: the official webfont (npm `@tabler/icons-webfont` 3.50.0, the
  same files as the GitHub release `v3.50.0`, which ships no font asset) cut down by
  `scripts/make-icon-font.sh` with `pyftsubset` to the glyphs in `icons.txt` (47 outline) and
  `icons-filled.txt` (1 filled: `trash`), glyph names kept, layout features dropped; the
  upstream `LICENSE` beside them. The script checks the tarball's sha256 and gives the same
  bytes on every run (checked twice).
- `cmake/embed.cmake` turns each font into a C++ array at build time (`nordend::embedded`), so
  the fonts are inside the program: no install path, no lookup, and dev runs, tests and the
  snapshot tool all use the same font.
- `FontSystem::add_font_data` loads a font from memory (`FT_New_Memory_Face`, a read-only
  harfbuzz blob); `add_font_file` goes through it.
- `IconSet` uses the two embedded fonts; a name ending in `-filled` is looked up in the filled
  font without the suffix (upstream calls it `trash`, Noctalia's merged font `trash-filled`). The
  XDG search and Noctalia's `noctalia-tabler.ttf` are gone: the embedded font is always there,
  so a fallback would only hide a missing name. An unknown name still draws `file` and is now
  logged once.
- Also kept for the title bar to come: `minus`, `square`, `squares`, `x`.
- Tests (242): the icon tests no longer skip without Noctalia; every name of the MIME table, the
  places and the views' own (sidebar, breadcrumb, status bar, info pane, title bar) is in the
  font, `trash-filled` too; `folder-filled` and `-filled` are not.

**Measured**
- Font: 13 432 bytes (outline) + 1 072 (filled), against Noctalia's 2 413 312.
- Binary: 3.30 MB, 2.75 MB stripped (+5 KB unstripped: the README's figures do not change).
- Snapshots of the columns, grid and details views (offscreen, also under ASan): the same icons
  as before, no warning about a missing name.

**Decisions**
- Embedded rather than installed under `share/nordend/fonts`: 14 KB is nothing, and the
  program then needs no data files at all.
- No fallback to Noctalia's font, as explained above (the plan said "ours first, Noctalia's
  as fallback"; the author is told).

**Tested by the author** (2026-10-30, on Umbriel): the icons everywhere as before, the full
Trash icon, `loaded font tabler-icons.ttf` in the log, no `no icon named`, and Noctalia's font
not open among the process's file descriptors.

**Open**
- A new icon means: add its name to `icons.txt`, run `scripts/make-icon-font.sh`, update the
  sha256 in `THIRD_PARTY.md`.
- Next in 7.4: the fallback cursor, then client-side decorations.

---

## 2026-10-30 — Removable disks through udisks2 (ROADMAP 7.3)

"Fatto quando" proposed and approved by the author the same day, with sdbus-c++ 2.3 as the
D-Bus library (a system library: `sdbus-c++-devel`, recorded in DESIGN and README), no automount,
LUKS after the MVP.

**Done**
- `platform/volumes`: plain data and pure functions. `UdisksObject` (Block, Filesystem, Drive
  properties), `volumes_from` (a Block with a Filesystem, usage "filesystem", not HintIgnore;
  the system's own only while mounted; external = removable, ejectable or on USB/SD/FireWire),
  `size_name` ("466 GB Volume"), `with_volumes` (mounted devices learn their volume and become
  ejectable with a USB icon; volumes not mounted are added without a path), `on_same_drive`.
- `platform::Udisks`: a system bus connection whose socket, event fd and timeout are watched by
  `core::EventLoop` (`getEventLoopPollData`/`processPendingEvent`, re-armed after every pump);
  `GetManagedObjects` read at start and 150 ms after any burst of udisks2 signals; `mount`,
  `unmount`, `eject` (every mounted file system of the drive unmounted in turn, then `PowerOff`,
  else `Eject`), all asynchronous; errors made readable ("in use", "not allowed", the text after
  udisks2's last ": "). Without a bus or udisks2: a warning, `available()` false.
- `fs::Place::Disk` (volume, mounted, ejectable). Sidebar: disks not mounted drawn quieter, an
  eject icon on external disks (its own click, hover), the place handed whole to the open
  handler; a disk not mounted is never a drop target and holds no folder.
- Browser: `set_devices`; a click on a disk not mounted mounts it, opens it, then checks on a
  worker whether it came read-only ("“kinetic” is read-only: it was not closed properly
  (Windows' chkdsk or ntfsfix -d fixes it)"); eject and unmount first take the view home when it
  shows that disk; messages for in use, errors, "can be unplugged now"; a disk's right click:
  Mount, Unmount, Eject.
- Fixed on the way (since 6.x): a right click that opened a menu left `button_held_` set (the
  release went to the menu), so the next click in the view still went to the sidebar.
- Snapshot: `--fake-disks`.
- Tests (242): which volumes show and their names, the author's machine as udisks2 describes it
  (USB Kinetic, internal DATA, the other system's partition hidden, swap/crypto/ignored left
  out); the places merged; the drive's file systems for eject; mount then open, a mount error;
  eject and unmount leaving the disk first, in use, unplugged, not allowed; the eject icon and
  the disk menu clicked for real (offscreen); a drop refused onto a disk not mounted; udisks2 on
  the real system bus listing the disks, read only (/boot/efi, DATA, the root here).

**Measured**
- The sidebar snapshot with a disk not mounted and one mounted, both ejectable. ASan and TSan
  clean, the real-bus test included.

**Open**
- LUKS volumes are not listed (after the MVP).

**Tested by the author**: the Kinetic plugged in shows up quieter without mounting, a click
mounts and opens it, eject from inside FOTOGRAFIA goes home and powers it off ("can be unplugged
now"), unplugged and plugged in again, "in use" while a terminal stood in it, and Unmount, Mount,
Eject from the right click.

---

## 2026-10-30 — Sidebar favourites (ROADMAP 7.2)

"Fatto quando" proposed and approved by the author the same day (a separate drop zone for
bookmarks; reordering and renaming after the MVP).

**Done**
- `core::write_file_atomically` (hidden temporary beside the file, fsync, rename; the folder
  made, the permissions kept, 0644 for a new file) and `core::file_uri`, moved from `thumbs`
  (`thumbs::file_uri` is now an alias) so `fs` writes URIs exactly as GLib does.
- `fs::bookmarks_file`, `fs::add_bookmark`, `fs::remove_bookmark`: text in, text out; the other
  lines (`sftp://`, labels, order, a missing final newline) stay; the same folder with or without
  a trailing slash is one bookmark. `PlacesWatcher::reload()` is public and starts watching a
  file whose folder appeared since (`gtk-3.0` made by the first bookmark).
- Browser: `add_to_sidebar`, `remove_from_sidebar`, applied one at a time on a worker (read,
  change, write), then the places are read again; messages "“Foto” added to the sidebar", "2
  folders added", "Already in the sidebar", "Only folders go in the sidebar", "“Foto” removed",
  "The sidebar could not be changed: …". Ctrl+D (the folder shown), "Add to Sidebar" in the menu
  of selected folders and of a folder's empty part, "Remove from Sidebar" on a bookmark's right
  click.
- Sidebar: `in_bookmark_zone` (the "BOOKMARKS" title, or the space below the last place) and a
  line where the bookmarks would go; dropping folders there adds them (Nordend's own drag only
  when every entry is a folder; another program's: whatever is not a folder is left out), and
  nothing moves. Dropping onto a place still moves into it.
- Tests (235): the text functions (GLib escaping with a space and "ï", duplicates, removal, other
  lines kept); the atomic write (folder made, permissions, nothing left beside it, an error);
  Ctrl+D twice, the menus, removal, a write error; folders dropped below the places and on the
  title, a file refused, the bookmark's menu.

**Measured**
- Snapshots: the insertion line in the sidebar, the menu with "Add to Sidebar  Ctrl+D", the
  three-column overlay with the new line. ASan and TSan clean.

**Decisions**
- Bookmarks are written by Nordend, not through GLib: the file format is a line per bookmark,
  and the URIs match GLib's byte for byte (`core::file_uri`, checked against gio in 5.x).

**Tested by the author**: Ctrl+D and "Already in the sidebar", "Add to Sidebar" on two folders,
folders dragged onto the "BOOKMARKS" title (the line after the last bookmark), a file refused
there and moved when dropped onto a bookmark, "Remove from Sidebar", the file's escaped name,
and the bookmarks in Firefox's Open dialog (Ctrl+O).

**My slip**
- The first full test run after adding Ctrl+D wrote three bookmarks of temporary test folders
  into the author's real `~/.config/gtk-3.0/bookmarks` (the test that presses every key pressed
  Ctrl+D, and tests read `$XDG_CONFIG_HOME`). The file did not exist before (the sidebar showed no
  bookmarks); removing it was left to the author. Fixed for good: the test runner points
  `XDG_CONFIG_HOME`, `XDG_DATA_HOME` and `XDG_CACHE_HOME` at its own temporary root before any
  test runs; a full run now leaves the real file byte for byte as it was (checked with `cmp`).

---

## 2026-10-30 — Drag and drop (ROADMAP 7.1)

"Fatto quando" proposed and approved by the author the same day (copy only towards other apps;
the sidebar's Trash trashes; a drag image; spring-loading after the MVP).

**Done**
- `ui::Clipboard` carries drag and drop on its `wl_data_device` (v3): `start_drag` (a second
  `wl_data_source`, offered for copy only, its own formats; `dnd_finished`/`cancelled` end it),
  `cancel_drag`; the drop side reports enter/motion/leave/drop (`DropEvent`), keeps the offer's
  `source_actions` and agreed `action`, `accept_drop` (accept + set_actions), `read_drop` (then
  `wl_data_offer_finish`) and `finish_drop` (Nordend's own drag, not read). A dropped offer
  survives the leave that may follow the drop.
- `ui::Window::make_drag_icon`: a second surface (viewport at fractional scales) drawn once with
  the window's renderer into its own EGL surface; `ui::Seat::modifiers()`.
- `ui::Offscreen` (surfaceless EGL + framebuffer), moved out of the snapshot tool, so tests can
  draw: the drag and drop tests lay the browser out for real and point at what it shows.
- Views: `hit(x, y)` (folder and entry under a point), a `DropMark` drawn as the target (an
  entry: primary container with a ring; a whole column or view: an outline); sidebar
  `place_under` and breadcrumb `path_at`, both with a marked drop target.
- `Folder::press`/`release_press`: a plain click on an entry selected with others keeps the
  selection until the button goes up, so the press can drag them all (as Finder and GTK do).
- Browser: a press on an entry moved 6 px starts a drag (the selection when the entry is in it;
  never from the trash); offered as `text/uri-list`, `text/plain;charset=utf-8` and `text/plain`
  (paths). Escape cancels. Over the window: the target is a folder entry, the empty part of a
  column or view (its folder), a breadcrumb step or a sidebar place; refused (not accepted, not
  marked) when it is the dragged folder or inside it, or where the items already are; from
  another program, only drags carrying `text/uri-list`. Drop: Nordend's own drag decides by
  itself (Ctrl copies, Shift moves, else a `stat` on a worker: same device moves, another
  copies); another program's follows the action agreed (copy unless the source allows move and
  the compositor chose it); the Trash place trashes; non-file URLs: "Only files can be
  dropped". Everything goes through `transfer_paths` (one batch: Ctrl+Z, sidecars, conflicts,
  progress). Near the top or bottom edge of the view (40 px) a timer scrolls it while the drag
  stays there.
- Snapshot: `--drag NAME` (the drag image drawn by the pointer), `--drop-over X,Y`.
- Tests (231): the drag of a selection (and a click that collapses it on release), the formats,
  Escape; drops in the columns moving and Ctrl copying, then Ctrl+Z; Shift onto a column's empty
  part; refusals into itself and where they are, leave clearing the mark; another program's file
  with a space and "ï" copied, a web URL refused, a drag without files ignored; the breadcrumb, a
  sidebar place, the trash.

**Measured**
- Snapshots with the author's palette: a folder row, a column, a breadcrumb step, the sidebar's
  Trash as targets, the drag image. ASan and TSan: tests and a drag snapshot clean.

**Decisions**
- Towards other programs only copy (the source offers `copy`): Nordend never deletes its files
  because another program says it took them. Inside Nordend the move is Nordend's own decision.
- *To be verified on Umbriel*: whether the keyboard's modifiers and Escape still reach Nordend
  during its drag (the compositor holds the pointer); whether the compositor applies Ctrl/Shift
  to the action of another program's drag.
- Fixed on the way: `drop_paths` read `paths.front()` in one lambda while another moved `paths`
  in the same call (argument order is unspecified): a crash under the first test.

**Open**
- Spring-loading (hovering a folder opens it) after the MVP, as agreed.
- Escape does not stop a drag on Umbriel: during the drag no key reaches Nordend, and Umbriel
  does not cancel by itself (buglog NB-002, suspected). Ctrl and Shift do reach it.

**Tested by the author**: moves and copies in the columns with Ctrl and Shift (sidecars along),
Ctrl+Z, a whole selection dragged, refusals into itself and where they are, the breadcrumb, the
sidebar and the Trash, a path dropped into a terminal, a photo into Firefox and a download from
Firefox into Nordend, a web image refused, auto-scroll near the edges. Escape did not stop a drag
(see Open).

---

## 2026-10-30 — Navigation history and hidden files (ROADMAP 7.0)

First step of phase 7; the "Fatto quando" proposed and approved by the author the same day.

**Done**
- `fs::Listing` leaves hidden entries (dotfiles) out on request: kept aside unsorted
  (`hidden()`), still updated by additions, replacements and removals, merged back in order when
  shown again; one name (`keep`) stays shown either way. `set_filter` returns the names that left
  and came. `DirectoryModel::set_filter` notifies them as removed and added, so the views and the
  selection follow with no new path; its trash diff and its known names count the hidden too.
- `Folder(…, show_hidden)` and `Folder::set_show_hidden(show, keep_cursor)`: left out, hidden
  names leave the selection (Ctrl+A then Delete never touches what is not seen); a cursor on one
  moves to the next entry shown, else the one before, the rest of the selection kept; with
  keep_cursor the entry stays (the columns before the focused one keep the way). `select_name`
  and `expect_name` keep a hidden name in sight: Backspace from `.config` opened from a bookmark,
  a reveal through it, a rename (F2) to ".name".
- The three views create their folders with the setting and apply it (`set_show_hidden`); the
  browser's Ctrl+H toggles it for all, with a message. Hidden by default; dotfiles shown are
  still dimmed. Status bar: "6 items, 5 hidden". The info pane counts a folder's items as it will
  show them (`preview::Info::hidden_items`).
- History in the browser: `note_location()` after every key, button press/release and
  open/go_to/reveal (and before every key and button, so the step left keeps its cursor); a new
  folder is a step (in the columns, the focused column's; not while a reveal is still unfolding),
  otherwise the step's cursor is updated. Alt+Left/Alt+Right and the mouse's side buttons
  (`travel`) go to the step with `go_to` and put the cursor back by name; a new step after going
  back drops the ones ahead; at most 100. A folder gone is found at once (opening it fails to
  watch: ENOENT), dropped from the history, the next one tried; nothing left: back where it was;
  "a no longer exists" / "2 folders no longer exist". The side buttons no longer go to the parent
  in the views (Backspace and Alt+Up still do).
- `ColumnView::sync_after` now drops a reveal whose way is lost (an entry not found), which
  otherwise stayed pending forever; `revealing()` tells the browser.
- Shortcuts: Ctrl+H "Hidden files" (General); "Alt+←  Alt+→" "Back, forward (mouse too)"
  (Moving), the second chord a line without keys (the overlay skips those).
- Tests (225): back and forward with the cursor in all three views, ends of the history; side
  buttons; steps ahead dropped; folders gone, skipped and none left; 100 steps; Ctrl+H in all
  views with status and selection; the cursor's nearest entry after and before; the way to a
  hidden folder kept in the columns; a rename to a hidden name; `Listing` filtering; the info
  count.

**Measured**
- Snapshots (columns, grid, details, hidden off and on; the overlay at 1400×800 and 1000×800)
  looked at. ASan and TSan: tests and a snapshot clean.

**Decisions**
- The filter lives in `fs::Listing`, not in the views: every view, count and selection reads the
  same `entries()`, so nothing can act on an entry it does not show.
- Existence is not checked on a worker before going back: opening a missing folder fails at once
  on the loop thread (the watch), the same syscall any open makes.

**Open**
- The two-column overlay (window narrower than 1200 px) now cuts Quick Look's lines at 800 px
  of height: two lines more than before (already an open point).
- Renaming the cursor's entry to a hidden name from another program loses the cursor (only F2
  keeps it).

**Tested by the author**: hidden files off at start with the count, Ctrl+H both ways, the
cursor moving off a dotfile, Ctrl+A leaving dotfiles out, `.config` kept in the column before;
back and forward with keys and the mouse's side buttons in all three views, cursor restored; a
folder deleted meanwhile skipped with its message; the overlay's new lines.

---

## 2026-10-30 — The README, ready to go public

Asked for by the author ("rendi il readme carino", the repository goes public soon).

**Done**
- The README opens with the name, a one-line pitch, the badges and a screenshot of the column
  view (the author's, `docs/images/screenshot-columns.png`), then "Why Nordend" (photos first,
  three views, measured speed, natural scrolling, care with files, keyboard), the status as a
  note, and the sections as before. The tests badge counts 215 under ASan and TSan. "Building"
  says plainly that the icons come from Noctalia's font for now.
- The screenshot showed people's names among the author's folders: three rows are blurred in the
  committed copy (`magick`, `-blur 0x7` over each row); the original was never committed.
- `docs/benchmarks.md` no longer names the author's photo folders by path.

---

## 2026-10-30 — Undo the last operation on files (ROADMAP 6.7)

Proposed after 6.6 and approved by the author, with two choices of theirs: an undone copy goes
to the trash (not deleted), and Ctrl+Shift+Z redoes.

**Done**
- `fs::TransferResult::items` (`PlacedItem{from, to}`): for each item placed, sidecars left out,
  where it was and where it is now: copies and moves (`to` with the name kept, "P1 2.ORF"), the
  trash (`to` in files/, `trash_item` now returns it), restores (`from` in files/).
- `views::FileOperation` (`undo.hpp`): kind (rename, create, copy, move, trash, restore, erase)
  and items; `describe()` for the status bar. Undoing one runs its inverse, and the inverse's
  own result is recorded as what Ctrl+Shift+Z undoes: rename ↔ rename back; move ↔ move back;
  trash ↔ restore; copy, create → trash (whose inverse, restore, is the redo). A permanent
  delete (and Empty Trash) is recorded as `erase`: "A permanent delete cannot be undone".
- Browser: every transfer goes through `submit(request, batch)`; a batch (`open_batch`) is one
  operation, possibly several transfers (a paste from two folders, a move back to two folders);
  when its last transfer is done, `record()` makes it what Ctrl+Z undoes (the user's: redo
  forgotten; an undo's: what Ctrl+Shift+Z redoes; a redo's: what Ctrl+Z undoes again). Renames
  (F2) and new folders/files are recorded when the worker answers. `undoing_` refuses another
  Ctrl+Z while one is on its way ("Still undoing the last one"). A new folder or file is taken
  away only if it is as it was made (an empty folder; a file as long as its template, or empty),
  checked on a worker.
- What changed meanwhile goes through the engines' own errors and messages ("Moved 0 files; 1
  could not be: P2.ORF: ..."); nothing undone, nothing to redo, and Ctrl+Z answers again.
- Shortcuts: Ctrl+Z "Undo the last operation", Ctrl+Shift+Z "Redo it" (views, Quick Look); the
  name's field keeps its own Ctrl+Z while editing. The two-column overlay moved Quick Look to the
  left column, the shorter one.
- The shortcuts test's reset now cancels a conflict card: pressing every key undoes a Delete,
  whose restore finds the name made again and asks.
- Tests (215): rename with sidecars undone, redone, undone again, "Nothing to undo/redo"; a move
  back and forth; a copy to the trash and restored by redo; Delete restored and trashed again; a
  permanent delete refused; a new folder kept while it holds a file, then trashed; a moved file
  removed meanwhile; the messages.

**Tested by the author**: a rename with its sidecar undone, "Nothing to undo", redone; a move
to DATA undone and redone; a copy undone into the trash (`trash-list` showed the copy and its
sidecar) and redone; a Delete undone; a new folder undone; a permanent delete refused.

**Decisions**
- Only the last operation, in memory: a history with a list is ROADMAP 9.
- An undo's undo is a redo by construction (the inverse of the inverse), so Ctrl+Shift+Z never
  needs its own engine.

**Open**
- A moved item that took another name ("P1 2.ORF", keep both) comes back under that name.
- An undone copy that had replaced a file (Replace on the card) cannot bring the replaced file
  back: only the copy goes to the trash.
- Ctrl+Z inside the trash folder works on the last operation, wherever it was.

---

## 2026-10-30 — Undo in the name's field (ROADMAP 6.6)

Asked for by the author after 6.5 ("premo Ctrl+Z e non torna indietro"); the order 6.6 (the
field), 6.7 (the last file operation), then phase 7 agreed with them.

**Done**
- `ui::TextField` keeps the states before each edit (text, caret, selection; up to 100) and
  those undone: Ctrl+Z restores the last, Ctrl+Shift+Z gives it back; any new edit forgets what
  could be redone, `set_text` forgets everything. Grouping: characters typed one after another
  are one edit, the selection they replaced included (F2, "Lake", one Ctrl+Z: the old name);
  characters removed one after another (Backspace, Delete) are one; a selection or a word
  removed (Ctrl+Backspace) is one; `insert` (a paste) is one; moving the caret or selecting ends
  the edit going on.
- The shortcuts list "Ctrl+Z — Renaming: undo (Shift: redo)" (`any_shift`).
- The shortcuts overlay had outgrown two columns: at 800 px of height Quick Look's lines were
  cut off since 6.5's Ctrl+V line (a slip of mine: I did not look at the overlay then). It now
  shows three columns when the window has 1 200 px for the card (General, Grid, Quick Look |
  Moving, Selecting, Sidebar | Files), two otherwise as before. Checked at 1280x800 and
  1100x900.
- Tests (208): grouping, the selection given back, redo, redo forgotten after a new edit, a
  paste undone; in the browser, F2, Ctrl+V of a text, Ctrl+Z gives the name back.

**Tested by the author**: F2 and "Lake" undone and redone, a pasted "Lago di Como" undone,
three Backspaces undone at once, the overlay in three columns and back to two when narrowed.

**Open**
- In a narrow, short window (two columns, under ~850 px of height) the overlay still cuts its
  last lines: it does not scroll.

---

## 2026-10-30 — The system clipboard (ROADMAP 6.5)

Proposed to the author with its "Fatto quando" and approved; Firefox is the other program to try.

**Done**
- `ui::Clipboard` on `wl_data_device` (manager bound up to v3): offers a `wl_data_source` with
  several types and writes each request to its pipe from the loop (non-blocking, `POLLOUT`, a
  slow reader never blocks the window); reads the selection's offer through a non-blocking pipe
  the loop watches, up to 64 MB, given up after 3 s; keeps every offer's types; drag offers are
  dropped when the drag leaves (drag and drop is not taken yet); `cancelled` on Nordend's source
  = another program copied: `set_lost_handler`. SIGPIPE is blocked with the other signals and
  ignored when it arrives (a reader that went away: the write fails with EPIPE); applications
  started from Nordend get an empty mask as before.
- `views/file_clipboard`: `uri_list` (CRLF, URIs from `thumbs::file_uri`, GLib's escaping),
  `gnome_copied_files`, `path_lines`; `path_from_uri` (file://, empty host or localhost,
  case-insensitive scheme, `%XX` undone, `?#` cut, a NUL refused, a trailing slash dropped),
  `parse_uri_list` (comments, CRLF or LF), `parse_gnome_copied_files`. `SystemClipboard`: the
  browser's view of it as functions (offer, clear, owned, types, read); the app wires the real
  one, tests a fake.
- Browser: Ctrl+C / Ctrl+X offer `x-special/gnome-copied-files`, `text/uri-list`,
  `text/plain;charset=utf-8`, `UTF8_STRING`, `text/plain` (paths), and
  `application/x-kde-cutselection` "1" for a cut. Ctrl+V: Nordend's own offer → the internal
  clipboard as before (a cut pasted once now also empties the system clipboard); another
  program's → its files (`gnome-copied-files` first, else `text/uri-list`, with KDE's cut type
  read first when there), one transfer per source folder, cut = move; text only → "The clipboard holds no files"; other hosts or schemes → "Only files
  on this computer can be pasted". Losing the clipboard forgets what Ctrl+C/X took and the cut
  marks. Ctrl+V in the name's field reads the first text type offered and inserts its first line
  without control characters (`NameEditor::paste`); listed in the shortcuts (`kEditor`).
- Another program's cut empties the clipboard only once something arrived, and only if the
  clipboard still holds it (`Clipboard::generation()`, changed by every new selection): every
  transfer goes through `Browser::submit`, which keeps, in the queue's order, the generation to
  empty after it. Found in the author's test: a cut of files that did not exist (a fish variable
  unset in the second terminal) had emptied the clipboard anyway.
- Tests (207 with the raw samples): URIs and lists both ways with odd names; what a list
  refuses; Ctrl+C/X offering the formats; Nordend's own cut pasted once and the clipboard
  emptied; losing it; another program's URI list from two folders (copied), GNOME's and KDE's
  cut (moved, sidecar along); a web URL refused; a cut that moved nothing, or a clipboard that
  changed meanwhile, not emptied; Ctrl+V in the name's field.

**Tested by the author** (the Wayland side: no headless compositor here): `wl-paste
--list-types` showed the five types, `wl-paste -t text/uri-list` the URIs of `a b%#?.txt` and
`caffè.txt` as written above, plain `wl-paste` the paths; Firefox pasted a file copied in
Nordend; a `wl-copy -t text/uri-list` of `P1.ORF` and `x.txt` was copied with the sidecar; a
GNOME cut of `z.txt` moved it; `wl-copy hello` ended the cut fading; Ctrl+V in the name's field;
0 CPU ticks in 30 s idle. After the GNOME cut, `wl-paste --list-types` listed text types
(`TEXT`, `STRING`, ...) that Nordend does not offer: likely a clipboard manager putting back its
last text entry (*to be verified*); there were no files on it. After the fix: a cut of a missing
file left `x-special/gnome-copied-files` on the clipboard; a cut of `w.txt` moved it and took it
off.

**Decisions**
- When Nordend owns the clipboard, Ctrl+V uses its own copy of what was taken, not a read
  through the compositor: no round trip, and "a cut is pasted once" stays exact.
- After pasting another program's cut, Nordend empties the clipboard, as GNOME's file manager
  does: the files are no longer where its URIs say.
- Text is offered as full paths, one per line (what a terminal wants), not names.

**Open**
- Images on the clipboard (`image/png`, a screenshot) are not pasted as new files yet.
- The clipboard dies with Nordend unless a clipboard manager keeps it (Wayland's way).
- Ctrl+C in the name's field does not copy text yet.

---

## 2026-10-30 — Empty Trash and Delete Permanently (ROADMAP 6.4d)

The half of 6.4c that deletes, as split with the author's OK.

**Done**
- `fs/erase` (`plan_erase`, `run_erase`; `TransferKind::erase` and `empty_trash`, run by the
  transfer queue): deletes for good, what a folder holds first, through folder descriptors
  (`openat` + `fstatat` + `unlinkat`), links never followed; a folder of the user's without
  u+rwx is opened up first (it is going anyway). A regular file takes its darktable sidecars; a
  photo that stays keeps them. Errors per entry ("Trip/x.txt: Permission denied"), cancellation
  between entries. Progress counts entries: a first walk adds them up.
- From the trash folder (`source` = `trash_path()`, names = paths in files/): each item's
  `.trashinfo` goes once the item is gone, the sidecars are those Restore would take along
  (`trashed_groups`, sharing `trashed_sidecars` with Restore), and `directorysizes` loses the
  folder's line (`prune_directory_sizes`; the file goes when empty). Empty Trash: every
  existing trash (home and disks, `existing_trashes`, found on the worker), everything in files/,
  then every `.trashinfo` whose item is gone (orphans too), then `directorysizes`.
- `views::ConfirmDialog`: a card like the conflict one: title, what goes, "Nothing goes to the
  trash: this cannot be undone.", Cancel (Escape) and a red Delete / Empty (Enter); it takes every
  key and click while open. It opens at once with "Adding up the size…"; `plan_erase` runs on a
  worker and fills in "With its darktable sidecar · 21.0 MB · 55 files and folders in all". Enter
  before the count arrives works: the delete is submitted when it does.
- Shift+Delete (views and Quick Look; `Command::erase`), Delete in the trash folder; menus:
  "Delete Permanently…" on entries (and on trashed ones, under Restore), "Empty Trash…" on the
  trash's empty space and on a right click on Trash in the sidebar (`Sidebar::set_menu_handler`,
  the sidebar's first menu; disabled while the trash is empty). Status bar: "Deleting 30 of 120
  files and folders", then "Deleted “P1.ORF” permanently (21.0 MB)" / "Emptied the trash: 12
  items deleted (412 MB)".
- Shortcut scopes are 16 bits now (`kConfirm` was the ninth); the overlay lists Shift+Delete and
  the card's keys. `nordend-transfer --erase` for measuring.
- Tests (201 with the raw samples): the engine (sidecars, a read-only folder, a hard link counted
  once, a link not followed, cancel, from the trash with `directorysizes`, Empty Trash over two
  trashes with an orphan `.trashinfo` and an item without one); the browser: Escape keeps
  everything, Enter before the count, the cursor goes on, every key listed for the card, Delete
  and Empty Trash in the trash folder, the icon back to empty, "The trash is empty already".

**Tested by the author**: on screen with the real trash (home, DATA and /tmp's): Shift+Delete
on a photo and on a folder holding a read-only one, Delete and Empty Trash in the trash folder,
Empty Trash from the sidebar's menu; afterwards `trash-list` showed nothing and the home's files/
and info/ and DATA's files/ were empty.

**Measured** (benchmarks): 10 101 entries in 0.20 s on ext4, `rm -rf` 0.17 s; on tmpfs 80 ms
against 54 ms. The difference is the counting walk.

**Decisions**
- Enter confirms (the Delete button is the default): the user already pressed Shift+Delete or
  chose the menu item on purpose; Escape and Cancel are the way out. Proposed to the author.
- Sizes are the files' sizes (`st_size`, what the views show), not blocks as in
  `directorysizes`.
- A sidecar selected along with its photo is counted as an item, not again as "with its
  sidecar".

**Open**
- No undo, by definition: the card is the only guard.
- The sidebar's menu has only the trash's item; other places have none yet.
- The progress bar counts entries, not bytes: one huge file is one step.

---

## 2026-10-29 — The trash in the sidebar, and Restore (ROADMAP 6.4c)

Asked for by the author after 6.4b ("manca l'icona del cestino?"); split in two with the author's
OK: this step only gives back, 6.4d (Empty Trash, Delete Permanently) is the one that deletes.

**Done**
- A "Trash" place after "File system", with Noctalia's `trash` icon, `trash-filled` while
  anything is in a trash (the Browser keeps a `DirectoryModel` on the trash for it).
- The trash folder, `fs::trash_path()` ("trash:///", not a real path): `DirectoryModel` lists it
  from every trash, the home trash and each mount point's `.Trash/uid` and `.Trash-uid`
  (`existing_trashes`), each item by its original name; when several share one, the oldest
  keeps it and the others are numbered as GLib numbers trashed names ("a.2.txt"). Items without
  a `.trashinfo`, and `.trashinfo` without an item, are left out (as `trash-list` does). The
  info/ folders are watched; any change reads the whole trash again on a worker and applies the
  differences. The home trash is made when missing, so it is watched from the start.
- `.trashinfo` parsing (`parse_trash_info`): `[Trash Info]` first, the first `Path` and
  `DeletionDate`, escapes undone, relative paths joined to the top directory (to
  `$XDG_DATA_HOME` for the home trash).
- `Folder::path_of(entry)`: where an entry really is. The ten places that joined the folder's
  path and the name (previews, thumbnails, Quick Look, the info pane, the columns, entering a
  folder, sniffing types) use it, so the trash folder shows previews, thumbnails and Quick Look
  like any other; a trashed folder can be entered (it is a real folder in files/).
- Read-only: Delete, F2, Ctrl+Shift+N, Ctrl+C/X/V say so; Enter on a file says "Restore it first
  to open it"; no menu on empty space. The breadcrumb shows the trash icon and "Trash". The info
  pane adds "Original location" and "Deleted".
- Restore (`TransferKind::restore`, `run_restore`), from the menu: each item goes back to its
  original path, the folder made again (0777 less the umask) if it is gone; a taken name asks on
  the conflict card (Skip, Keep both, Replace for a file over a file; a folder is never merged,
  `Conflict::merge` false hides Replace there); the `.trashinfo` is removed after the move; a
  photo takes along its sidecars trashed from the same folder (the newest of each name), renamed
  with it when kept both. The cursor goes on to the next entry, as Delete does.
- Slip, found in the author's first try (the DATA trash was missing from the view): the mount
  points were read with `core::read_text_file`, which reads as many bytes as a file reports, and
  `/proc` files report 0. `places` already had the right reader; it is now `fs::read_proc_file`,
  used by both, and a test reads the real `/proc/self/mountinfo` through `fs::mount_points`.
- Tests (194 with the raw samples): `.trashinfo` parsing; two trashes merged, names numbered,
  orphans left out; restore with a missing folder and sidecars; conflicts (skip, keep both with
  sidecars, replace, a folder not replaced); the model following an external trashing and a
  restore; the browser: what is refused, the menu's Restore, the icon full then empty. The test
  binary now sets its own trash (`tests/main.cpp`): no test reads or makes the user's.

**Tested by the author**: on screen with the real trash (items left by the day's tests, from
folders since deleted) and one on DATA. The view showed what `trash-list` showed (after the
mountinfo fix); `gio trash` from a terminal appeared at once; Restore brought `P1.ORF` back with
its sidecar into a folder made again, Keep both gave `uno 2.txt` next to the new `uno.txt`, five
items restored at once; `trash-list` afterwards: only the author's own item.

**Decisions**
- Original names, not the names in files/: what the user knows. The numbering only shows when
  the same name was trashed twice.
- One model for the trash rather than one folder per trash: the photos trashed on DATA show
  with the home's, as `trash-list` shows them.
- The sidebar's icon follows the trashes watched when the browser started: a trash made later
  on another disk (the first file ever trashed there) shows at the next start.

**Open**
- No sorting by deletion date, no "Original location" column in the details.
- A trash on a disk mounted after the trash folder was opened appears when it is opened again.

---

## 2026-10-29 — Context menu (ROADMAP 6.4b)

Asked for by the author after 6.4 ("manca il menu contestuale?"); added to DESIGN with the
author's OK.

**Done**
- `views::ContextMenu`: a menu drawn by Nordend over the window, in the theme's colors (panel
  `surface_container_high`, highlight `hover` / `on_hover` like Noctalia's menus), items with the
  shortcut at the right, separators, one level of submenus. Moved to stay whole: it opens to the
  left of the point near the right edge, a submenu to the left of its menu, both pushed up near
  the bottom. Keys: Up/Down over what can be chosen (wrapping), Home/End, Enter, Right into a
  submenu, Left and Escape out of it, Escape closes; every key is the menu's while it is open.
  Pointer: hovering highlights and opens a submenu, a click runs an item, a click outside only
  closes it.
- Right click (and two-finger tap, which libinput sends as the right button) in the three views:
  on an entry, selected first unless it already was (a right click inside a selection keeps it);
  on empty space, the selection cleared and the folder's own menu; on the info pane, the menu of
  what it shows. Menu and Shift+F10 open it from the keyboard, under the cursor's row or cell
  (`cursor_anchor()` in each view).
- On entries: Open (as Enter), Open With ▸, Copy, Cut, Paste, Rename… (one entry), Move to
  Trash. On a folder's empty space: New Folder, New File ▸, Paste. Paste is dimmed with nothing
  taken, Rename with several entries, Open With is left out for a folder. The keys shown come
  from the shortcuts table (`keys_for(Command)`).
- Open With: `Applications::all_for(type, parents)`, GLib's `g_app_info_get_all_for_type` /
  `_recommended_for_type` (read in `gdesktopappinfo.c` 2.88.3: added associations and cache
  entries dir by dir, removed ones blocked from then on, parents after). The menu lists the
  default first ("Darktable (default)"), then the rest once each; the chosen application opens
  every selected file (`plan_open(..., app_id)`, `Opener::open(..., app_id)`), folders left out.
  Found on a worker (`Opener::choices`); the submenu says "Looking for applications…" until then,
  "No applications" when there are none.
- New File: "Empty File" makes "New File" ("New File 2"...), a template makes a copy under its
  name ("Letter.txt", "Letter 2.txt"), with its permissions, created with `O_EXCL` (never over
  anything), removed if the copy fails; the name is edited at once, as for a new folder. The
  templates are the visible regular files of `XDG_TEMPLATES_DIR` (user-dirs.dirs), else
  `~/Templates` when there is no user-dirs.dirs (as the sidebar's user dirs); backups `~` left
  out. Looked for on a worker every time the menu opens.
- Empty files open as their name says (asked for after the author's test, where an "Empty
  File" renamed `Lettera.txt` answered "No application opens application/x-zerosize"):
  `MimeDatabase::to_open(mime, name)` turns `application/x-zerosize` (GLib's type for any empty
  file) into the name's type when it has one; opening, Open With and `nordend-open --dry-run`
  use it, and so do icons (which already followed the name). The type shown in the info pane is
  still GLib's. An empty file without a known extension still has no application. Nautilus does
  something similar for empty files: *to be verified*.
- `nordend-open --apps TYPE...` prints both lists; `nordend-snapshot` gets an Opener (lists only,
  nothing launched) so "Open With" can be checked offscreen.
- Tests (187 with the raw samples): the menu's keyboard (skipping, wrapping, submenus, a submenu
  filled while the keyboard waits in it, Escape twice, a click outside); the items for one entry,
  several, a folder, empty space, before and after Copy; Open With choosing an application and
  opening only the files; Move to Trash and Rename from the menu; New File empty and from a
  template twice (permissions kept); `make_file` numbering and failure; `templates_in`,
  `templates_dir`; `all_for` and `choices_for` against a fake system (added, cached, removed,
  parents, not installed).
- Slip found on the way, and fixed: `Environment::from_environment()` (6.1) looped over views
  into a temporary string (`split(env("XDG_CURRENT_DESKTOP"), ':')`): use after scope whenever
  `XDG_CURRENT_DESKTOP` is set (here: `umbriel`), so the desktop-specific `umbriel-mimeapps.list`
  could be read under a garbled name. ASan caught it in the new snapshot. No other loop does
  this.

**Tested by the author**: on screen, with a temporary config holding `XDG_TEMPLATES_DIR`. The
menu in the three views, Open With opening Darktable and Helix, New File from the template twice
("Lettera 2.txt", "Lettera 3.txt", both "Caro,"), an empty `Lettera.txt` opening in Helix after
the fix. The first try used empty test files (`touch`): GLib's `application/x-zerosize`, no
application, which led to the fix above. The theme did not follow Noctalia during the test only
because the temporary config had a copy of `colors.toml`; without it, "theme: reloaded".

**Measured** (see benchmarks): `nordend-open --apps` against `gio mime` on all 1 232 types and
aliases: 0 differences in both lists ("Registered" and "Recommended"); 564 types have
applications, 380 of them different lists with and without parents.

**Decisions**
- In the window, not an `xdg_popup`: simpler and like the conflict card; it cannot go past the
  window's edges. A real popup can come later.
- Activation on press, not release: the press that opens the menu never runs an item.

**Open**
- Snapshot keys reach the menu before the templates are found, so `--keys` cannot choose a
  template (the tests can).
- No icons in the menu (applications' icons would need their icon themes).
- Open With offers the cursor's file's applications even for a mixed selection.

---

## 2026-10-29 — Trash (ROADMAP 6.4)

**Done**
- `fs/trash`: the freedesktop Trash specification 1.0 (read from its source, `xdg-specs`
  `trash/index.rst`), with GLib's `g_local_file_trash` (2.88, `glocalfile.c`) as the reference
  for the details the spec leaves open.
  - Which trash: the home trash (`$XDG_DATA_HOME/Trash`, else `~/.local/share/Trash`, made 0700
    when missing) when the folder is on its device; else the top directory (`mount_top`: the
    highest ancestor on the same device) holds it: `$topdir/.Trash/$uid` when `.Trash` is a real
    folder with the sticky bit, else `$topdir/.Trash-$uid`, made when missing; either must be a
    folder owned by the user. Never copied between disks: no trash there, the item stays and the
    status bar says why.
  - Each item: `info/NAME.trashinfo` created with `O_EXCL` and written whole, then a rename into
    `files/` without replacing; a failed rename removes the info. Names as GLib numbers them
    (`P1.ORF`, `P1.2.ORF`; `Trip.2`); a name too long for `.trashinfo` is shortened from its
    front at a character boundary. `Path=` absolute in the home trash, relative to the top
    directory elsewhere, escaped like `g_uri_escape_string(path, "/")`; `DeletionDate` in local
    time.
  - Folders go to `directorysizes` (`du -B1`, a hard link counted once; the info file's mtime),
    rewritten through a temporary and a rename; lines of folders no longer in `files/` are
    dropped. gio does not keep this file; KDE reads it.
  - Not trashed again: what is in the trash (refused), the folder that holds it (an error).
  - Darktable sidecars go with their photo, each with its own `.trashinfo` (restored one by one
    by any trash program).
- `TransferKind::trash`: the transfer queue runs `run_trash` for it, so Delete waits behind a
  copy and shows its progress the same way ("Moving 3 of 120 items to the trash").
- Delete (and keypad Delete) in the views and Quick Look: the selection, or the cursor's entry.
  The cursor moves at once to the first entry after the last one trashed that stays (else the
  one before). Messages: "“P2.ORF” with 1 darktable sidecar moved to the trash", "3 items moved
  to the trash; 1 could not be: ...".
- `nordend-transfer --trash SOURCE_DIR NAME...` trashes from the command line (the real trash).
- Tests (179 with the raw samples): escaping, numbering, `.trashinfo`; the home trash with
  sidecars, a folder with a hard link (size checked against `du -B1`), the same names twice, a
  stale file in `files/`; a read-only folder and a missing name (nothing lost, no info left);
  the trash not trashed; the top directory's trash (`.Trash-uid`, `.Trash` without sticky bit or
  as a link refused, `.Trash/uid` with it; paths relative); `mount_top`; Delete in the browser,
  the cursor moving on and back.
- Slip, fixed before anything else: the shortcuts test presses every listed key, so Delete
  trashed its temporary files into a real `/tmp/.Trash-1000` (tmpfs is not the home's disk).
  The fixture now uses a trash inside its temporary folder and makes its files again; the stray
  trash was removed. The author's home trash was not touched.

**Measured** (see benchmarks): 1 001 items in 24-25 ms, `gio trash` 69-71 ms. The
`.trashinfo` files match gio's for awkward names.

**Decisions**
- No permanent delete (Shift+Delete) and no trash view in the sidebar in this step: the first
  needs its own confirmation card, the second (with restore) is a step of its own.
- The trash goes through the transfer queue rather than a worker: trashing a folder on a slow
  disk is quick, but its `du` can take a while, and the order with a copy running matters.

**Tested by the author**: on screen, in `~/nordend-trash-test` and on DATA. `trash-list`
(trash-cli, installed for this) lists the five items Nordend trashed, the home ones with their
absolute paths and `x.txt` from DATA's `.Trash-1000` (relative `Path=`); `trash-restore` put
`Viaggio` back in its folder. The read-only folder's file stayed. `directorysizes` gave
`Viaggio` 0 bytes: right, on btrfs a folder and an empty file take no blocks (`du -B1` says 0
too).

**Open**
- No check that a folder's contents can be deleted later (GLib's `check_removing_recursively`):
  a folder with a read-only subfolder goes to the trash and then needs `chmod` to be emptied.
- Bind mounts of one file system: the top directory is found by device, so a rename across them
  fails (EXDEV) and is reported, as in GLib.

---

## 2026-10-29 — Giving decoded photos back (ROADMAP 6.3c)

**Done**
- `core::give_back_large_blocks()`: `mallopt(M_MMAP_THRESHOLD, 16 MB)`, first thing in `main()`
  (and in `nordend-snapshot`, which has `--default-malloc` to compare). Blocks of 16 MB or more
  get mappings of their own and go back to the system when freed; glibc's dynamic threshold
  would otherwise grow to the decode buffers' size and serve them from the workers' arenas.
- Quick Look drops the fitted pixels once they are a texture (the size stays for the caption).
- The memory report also gives the peak (`VmHWM`).
- Tests (173 with the raw samples): eight 20 MB blocks allocated and freed leave nothing kept
  (checked to fail without the call: 21 MB kept); skipped under ASan and TSan, whose malloc
  replaces glibc's. Quick Look's pixels need GL to be uploaded, so that one is checked with
  `nordend-snapshot --memory` ("pixels kept 0.0 MB"), not a unit test.

**Measured** (see benchmarks): offscreen, a screenful of new thumbnails 205-215 ms instead of
188-201 ms (+7 %), 152 MB afterwards instead of 386 MB; Quick Look over 20 photos 155 MB
instead of 584 MB. Limiting the thumbnails made at once (4 or 2) lowers the peak but costs 20 %
or twice the time: not kept. The thumbnail texture budget (256 MB, in VRAM) stays.

**Tested by the author**: the five steps on screen (`giro.sh dopo`): 111, 120, 141, 143 and
143 MB back home, instead of 545 MB; peak 240 MB. Thumbnail textures reached 120 MB of VRAM
(257 thumbnails, grid and info pane sizes), kept under the budget.

---

## 2026-10-29 — Memory with photos (ROADMAP 6.3b)

Asked for after `top` showed 290 MB (6.2) and 412 MB (6.3) resident. Measure first; fixes are
their own step, with the author's OK.

**Done**
- `core::read_memory_report` / `describe` (`core/memory`): `smaps_rollup` (RSS, anonymous,
  private, shared), `smaps` grouped by mapping (`[heap]`, `[anon]`, libraries, fonts, DRM),
  `mallinfo2` (in use, freed but kept, arenas, mmapped), the DRM fdinfo (`drm-memory-vram`,
  `-gtt`, `-cpu`, one count per client id).
- Nordend's own counts: `Thumbnails::usage()` (slots, textures, bytes, pixels waiting),
  `QuickLook::usage()` (slots, texture bytes, pixels kept), `Renderer::atlas_bytes()`,
  `Browser::memory_lines()` (folders, entries).
- The app logs the report on SIGUSR1 (the signalfd now reads which signal came); the snapshot
  tool prints it with `--memory`, and `--trim` calls `malloc_trim(0)` first and times it.
- A test reads the report around a 64 MB block. 172 test cases with the raw samples.
- A script for the author's run (the first try had no reports: the `pkill` in a second terminal
  was forgotten): it starts Nordend with a fresh cache, prompts each step and sends the signal
  itself, checking the report reached the log.

**Measured** (see benchmarks, with all the numbers)
- On screen, the author's five steps: home 111 MB, `vero` 130 MB, the grid with every thumbnail
  526 MB, after Quick Look 545 MB, **back home still 545 MB**. Of it, 430 MB is memory malloc has
  been given back and keeps: only 21 MB are in use.
- heaptrack: the kept memory is the thumbnails' decode buffers (the ORF's embedded JPEG decoded
  whole by wuffs: about 30 MB of pixels and 15 MB of work buffer each, eight workers at once).
  Freed at once; glibc's dynamic mmap threshold grows to their size, so they are served from the
  workers' arenas, which glibc does not give back.
- Not the cause: thumbnail textures (14-89 MB, and in VRAM), the 3 GB copy (11.6 MB peak),
  libraries (89 MB, shared), GPU memory (separate from the RSS).
- With the mmap threshold fixed at 16 MB (`GLIBC_TUNABLES`): the same steps stay at 111, 120, 139,
  139, 140 MB, a 405 MB saving; offscreen the grid's first screenful of new thumbnails takes
  199-201 ms instead of 183-189 ms.

**Tested by the author**: the five steps on screen, both runs, through the script (the numbers
above); the step ticked on the author's OK.

**Proposed** (a step of its own, after the author's OK)
1. `mallopt(M_MMAP_THRESHOLD, 16 MB)` at startup (the app and the tools): -405 MB with photos,
   +12 ms on a screenful of new thumbnails; 1 MB saves a little more (138 MB) for +25 ms.
2. The transient peak while thumbnails are made is still eight decodes at once (about 370 MB for
   a moment): fewer concurrent full-size decodes, or decoding the preview scaled down
   (libjpeg-turbo's DCT scaling, which would also be faster), *to be measured*.
3. Quick Look keeps the fitted pixels after uploading them (10.5 MB for three slots): drop them.
4. Thumbnail textures stay on the GPU after leaving a folder, up to the 256 MB budget (89 MB
   seen): a smaller budget, or dropping a folder's thumbnails when it is left, *to be measured*.

---

## 2026-10-29 — Copy and move (ROADMAP 6.3)

**Done**
- `fs::run_transfer` (`fs/transfer`), the engine, blocking:
  - a move on one file system is `renameat2(RENAME_NOREPLACE)`, instant (falls back to copying
    on EXDEV, a mount point below the source);
  - a copy, or a move between file systems, copies data with `copy_file_range` in 8 MB pieces
    (read/write when the kernel refuses across file systems) into a hidden
    `.nordend-part-<pid>-<n>` next to the target, renamed to its name only when complete; a new
    folder is built the same way, as a whole. Cancelled or failed: the temporary is removed, so
    nothing half-written ever carries a real name;
  - between file systems the copy is `fsync`ed (and its folder) before the source is deleted; a
    folder's source goes only if all of it arrived;
  - permissions, access and modification times (nanoseconds) and symbolic links are kept;
    special files are reported, not copied;
  - darktable sidecars travel with their photo (`sidecars_of`, now split out of
    `sidecar_renames` with `sidecar_name`), named after the photo's final name; a sidecar picked
    with its photo goes with it once; a taken sidecar name is a conflict on the photo;
  - conflicts go to a `resolve` callback: replace (a file replaced only once the new one is
    complete, by an atomic rename over it; folders merged, same names replaced), skip, keep both
    (`free_name`: "P1 2.ORF" with "P1 2.ORF.xmp", "Trip 2"), cancel. A file never replaces a
    folder nor the reverse. A copy into its own folder keeps both by itself; a move into its own
    folder does nothing;
  - refused before anything moves: a folder into itself or below ("Cannot copy Trip into
    itself"), or more bytes than the destination's free space ("Not enough space: 3.4 GB
    needed, 1.2 GB free").
- `fs::TransferQueue`: one thread, one transfer at a time in order. Progress reaches the loop at
  most every 100 ms; a conflict is posted as a question and the thread waits for the answer
  (cancel and destruction answer it); the result is posted. Callbacks check an `alive` flag.
- `Browser`: Ctrl+C / Ctrl+X take the selection (or the cursor's entry); cut entries fade in all
  three views (`CutMarks`, `ItemState::cut`, thumbnails drawn at 45 %); Escape keeps them where
  they are. Ctrl+V submits a copy or a move into the folder shown (a cut is pasted once). The
  status bar shows "Copying 163 files · 2.1 GB of 3.4 GB · 180 MB/s · 8 s left" (rate measured
  from the first byte, restarted after a question), a 2 px bar along its top and an × that
  stops the transfer; the result is one line ("Copied 342 files (3.1 GB)", "Stopped: 40 files
  copied so far", "Copied 161 files; 2 could not be: ..."), and what arrived is selected when
  its folder is shown (`Folder::expect_names`). `loading()` counts transfers.
- `views::ConflictDialog`: a card ("“P1.ORF” is already in album"), a note on what Replace does
  (or that it cannot, file against folder), "Do the same for the rest" (A), Skip (S), Replace
  (R), Keep both (Enter), Escape stops. It takes every key and click while open.
- Shortcuts: Ctrl+C, Ctrl+X, Ctrl+V, and the card's keys under a new `kConflict` scope (a test
  presses each on a real conflict). The overlay's columns were rebalanced (Grid moved left).
- Tools: `nordend-transfer [--move] [--force-copy] SRC DEST [NAME...]` (the engine, timed);
  `nordend-snapshot --transfer TEXT` and `--conflict NAME` to look at the bar and the card.
- Tests (171 with the raw samples, 13 new): copy (contents, nanosecond times, permissions,
  links, recursive folders, sidecars, progress totals), move on one disk (same inode), move
  between disks forced (new inode, sources gone), each resolution, sidecar names on keep both,
  folder merge, file against folder, own-folder copy, into itself, not enough space (a 1 PiB
  sparse file), cancel after the first piece (no temporary left, source whole), a read-only
  destination, the queue (question answered later, destroyed while asking), and in the browser:
  copy and paste with selection, cut and Escape, move once, each answer, "for the rest", stop,
  every listed key. Release, ASan, TSan pass (transfer and paste ones 6 times in Release and
  TSan); snapshots under ASan and TSan; tidy and format clean.

**Measured** (see benchmarks): the author's `vero` folder (342 files, 3.1 GB) from the SATA SSD
into `/tmp` (tmpfs), page cache warm, three runs each: Nordend 1.38-1.39 s (about 2.2 GB/s),
`cp -a` 1.38-1.40 s. The same speed.

**Tested by the author**
- On screen: the 342-file copy of `vero` with progress, rate and time left (`diff -r` equal,
  dates kept), a cancelled copy leaving no temporary, the conflict card (skip, replace, keep both
  for the rest, Escape), a cut fading and Escape, a move on one disk and one onto the DATA disk,
  the three refusals (into itself, not enough space, read-only), F1, `top` at 0.0 % CPU
  afterwards: "il resto funziona tutto".
- `top` showed 412 MB resident after the 3 GB copy and browsing the photos: memory with photos is
  still *to be measured* (open since 5.8; next, before 6.4, is the proposal).

**Open**
- Cold-cache throughput (after `drop_caches`, the author's `sudo`) and a real move between disks
  (an SD card or a USB disk) are for the author's test.
- No drag and drop yet: proposed as a next step on the same engine.
- The temporary is visible (dimmed, hidden name) in the destination while a file is copied.
- Merging folders replaces same-named files without asking again inside them.
- No key stops a transfer (the × only); copying files between Nordend and other apps is 6.5.

---

## 2026-10-29 — New folder and rename (ROADMAP 6.2)

**Done**
- `fs/operations`: `name_problem` (empty, `.`/`..`, `/`, NUL, more than 255 bytes),
  `make_folder` ("New Folder", else "New Folder 2"...: `mkdir` itself decides, the next name on
  EEXIST, so two makers never collide), `rename_entry` (`renameat2(RENAME_NOREPLACE)`, with a
  check-then-rename fallback only on file systems that answer EINVAL/ENOSYS) and
  `sidecar_renames`: a regular file takes its darktable sidecars along, `X.ORF.xmp` and the
  duplicates `X_NN.ORF.xmp` (two digits or more), unless a file has the duplicate's name
  (`X_1234.ORF` next to `X.ORF`: that sidecar is its own). If a sidecar's new name is taken,
  nothing is renamed; if one fails after the photo moved, the message says which.
- `ui::TextField`: one line of UTF-8 text with a caret and a selection; typing (dead keys and
  compose arrive as `KeyEvent::text`), Backspace, Delete, arrows, Home/End (Up/Down too), Shift
  to select, Ctrl to go by words (spaces, dots, dashes, underscores end them), Ctrl+A; a click
  places the caret, Shift+click extends, a double click selects all. Scrolls sideways to keep
  the caret in sight. The caret does not blink, so an open field costs no frames.
- `views::NameEditor` + `EditSlot`: the browser names the entry; the view showing it (column
  rows, grid cells, details rows) leaves the name out and notes where it would be, and the
  editor draws the field there after the view. The name is selected without its extension (a
  folder's whole name). Enter confirms, Escape cancels, a press outside confirms and then does
  what it does. A bad name is refused at once; a good one goes to a worker and the field waits
  (border dimmed, keys swallowed): on success it closes, on failure ("P1.ORF already exists",
  "Permission denied") it stays for another try. While it is open every key is the field's.
- `Browser`: F2 (`start_rename`) and Ctrl+Shift+N (`new_folder`), both closing Quick Look and
  doing nothing in the sidebar (generalised: in the sidebar a command not listed for it is
  nothing, as Space already was). `loading()` also counts new folders and renames on their way.
  A field whose entry disappears (another program) closes.
- `Folder::expect_name`: the cursor waits for an entry Nordend just made (a plain `select_name`
  gives up at the next change). After a rename the cursor follows by itself (the watcher reports
  renames), and a new folder renamed before the watcher saw it moves the wait to the new name:
  found by a test in the columns, where typing is faster than inotify's settle.
- Shortcuts: a "Files" group (F2, Ctrl+Shift+N, and the field's Enter/Escape and Ctrl+arrows under
  a new `kEditor` scope). The overlay listed its groups by hand and left "Files" out (seen in a
  snapshot): `overlay_groups()` now feeds the test, which fails on a group the overlay does not
  show. The "nothing else does anything" sweep now includes Ctrl+Shift.
- `nordend-snapshot --keys`: a single letter types itself ("F2,L,a,k,e").
- Tests (158 with the raw samples, 11 new): names, new folder names, sidecar matching (decoys
  included), renames that must not replace (the photo or a sidecar), folders without sidecars,
  a read-only folder, the text field's keys and UTF-8, F2 in all three views with sidecars,
  refused and failed names, Escape, Ctrl+Shift+N in all three views, F2 without a cursor and in
  the sidebar, the editor's shortcuts. Release, ASan, TSan pass (the browser ones 6 times in
  Release and TSan); snapshots under ASan and TSan; tidy and format clean.

**Measured**: scrolling 10 000 entries (600 frames at 2880x1620, three runs) is unchanged:
details 0.37-0.50 ms, grid 0.30-0.32 ms, columns 0.25-0.27 ms median; details with a field open
0.50 ms.

**Tested by the author**
- On screen: F2 with the extension left out, accents typed, an ORF renamed with its two
  sidecars, a taken name and a `/` refused with the field kept, Escape, Ctrl+Shift+N in the three
  views, a click outside, clicks in the field, a read-only folder's messages, F1's "Files", and
  `top` at 0.0 % CPU with the field open: "funziona tutto perfetto".
- `top` showed 290 MB resident in a folder with an ORF (117 MB measured on 2026-10-28 in a source
  folder): previews and thumbnails held as textures, likely; memory with photos is still *to be
  measured* (an open point since 5.8).

**Open**
- Renaming only the case of a name on a case-insensitive file system (an exFAT SD card) is
  refused as "already exists": `RENAME_NOREPLACE` sees the same entry. A two-step rename would do
  it; not done yet.
- Paste into the field comes with the clipboard (6.5); no undo (after the MVP's operation queue).
- The pointer does not turn into a text cursor over the field.
- Caret moves by code points, not grapheme clusters (an emoji with a modifier takes two steps).

---

## 2026-10-29 — Open with the default application (ROADMAP 6.1)

**Decision** (with the author, DESIGN "Apertura dei file"): no `xdg-open`, no `gio open`; Nordend
reads the two freedesktop specs itself. Measured first on this machine (xdg-utils 1.2.1, generic
mode under Umbriel): `xdg-open` waits for the application to quit (it runs it as a child of its
shell), ignores `Terminal=true` (Helix, the default for text/plain, would start without a
terminal and fail silently) and takes the type from `file`, which differs from ours (`.xmp`:
`text/xml` against `application/xml`).

**Done**
- New module `platform/`:
  - `desktop_entry`: the [Desktop Entry] keys Nordend needs, string escapes, lists, the localized
    `Name` by the spec's locale matching; `Environment` (XDG dirs, desktops, locale, PATH,
    `$TERMINAL`, read once at startup); `find_program`.
  - `exec`: `split_exec` (the spec's double quotes, plus single quotes and backslashes as GLib's
    shell parser reads them), `expand_exec` (`%f %F %u %U %i %c %k %%`, deprecated codes dropped,
    a list code gives one process, a single one a process per file, no code appends the file as
    GLib does), `in_terminal` (`$TERMINAL`, `xdg-terminal-exec`, then foot, ghostty, kitty,
    alacritty, wezterm, konsole, gnome-terminal, xterm: forms of the ones not installed here *to
    be verified*).
  - `applications`: GLib's order: per type, then its parents (aliases resolved, subclasses
    breadth first), the defaults of every dir, then added associations and cached entries minus
    removed ones; the first installed wins (not Hidden, an Application, `TryExec` and the `Exec`
    program in the PATH; the highest copy of an ID decides and hides the lower ones).
  - `launcher`: `pidfd_spawnp` (glibc 2.39) with `POSIX_SPAWN_SETSID`, an empty signal mask and
    default dispositions (Nordend blocks SIGINT/SIGTERM for its signalfd: without this every
    application it starts would inherit them blocked), stdio on `/dev/null`,
    `addclosefrom_np(3)`, `addchdir_np`. The pidfd sits in the event loop; the child is reaped
    there (`waitid(P_PIDFD)`): no thread, no zombie, children survive Nordend.
  - `opener`: types (the view's, else detected), applications and command lines on a worker
    (`plan_open`, pure), processes on the loop thread, each with its own activation token; one
    line reported.
- `ui::Activation` (xdg-activation-v1, offered by Umbriel): a token tied to the serial of the last
  key or button press (`Seat::input_serial()`) and to the window's surface, passed as
  `XDG_ACTIVATION_TOKEN` and `DESKTOP_STARTUP_ID`; given up after 500 ms.
- `Browser`: Enter (and keypad Enter) on a file opens the selected files (folders in the
  selection left out; a cursor outside the selection opens only itself), with the types the view
  already knows; on a folder the view goes in as before. A double click on a file does the same in
  the three views. `Browser::set_open_handler` keeps views free of `platform`.
- `StatusBar::show_message`: a line for 4 s in place of the status ("Opening 5 files with
  Darktable…" in the text color, problems in the error color): no application for a type, no
  terminal, a program that cannot start, or one that quits with an error within 2 s.
- `MimeDatabase::parents()` (the subclasses file, as `g_content_type_get_parents`); duplicate
  parent lines from several mime dirs are no longer stored twice.
- `nordend-open` (dev tool): `--dry-run FILE...` prints types, applications, folders and command
  lines; `--default TYPE...`, `--types`; without options it opens for real (no token).
- `nordend-snapshot --message TEXT` (`!TEXT`: an error) to look at the status bar's message.
- Shortcuts table: Enter is now "Open (files in their app)".
- Tests (147 with the raw samples, 11 new): desktop entry parsing and locales, lists, quoting,
  field codes, the terminal order, defaults/associations/cache by priority, desktop-specific
  lists, removed associations, parent types, aliases, hiding by ID, Hidden/TryExec/missing
  program/wrong Type, IDs from subfolders, the plan (grouping, terminal, problems), the launcher
  (exit status, signal, missing program, environment, folder, empty signal mask read from
  `/proc/self/status`), the opener end to end with fake XDG dirs and scripts, and the browser's
  Enter. Release, ASan, TSan pass (timing-based ones 8 times in Release and TSan); tidy and format
  clean.

**Measured** (see benchmarks)
- Reading every desktop entry and list: 0.42-0.48 ms for 21 entries, so it is done again on every
  open (on a worker): new applications and changed defaults count at once, nothing is watched.
- Against `gio mime TYPE` (GLib 2.88.3) on every type of the shared-mime-info database and every
  alias (1 232 names, 564 with an application): 0 differences. The first run had 7; they taught
  two things about GLib, both now done the same way (and checked with gio on scratch XDG dirs):
  - the entries declaring a type come from `mimeinfo.cache`, not from the entries' `MimeType`
    keys: an entry missing from the cache is not offered, a stale cache line still is, and a
    user copy of an entry in a dir without a cache hides the system copy's associations (only
    `mimeapps.list` defaults still reach it);
  - types in the cache and in `mimeapps.list` are filed under their canonical name, merged in
    file order (`image/x-targa` and `image/x-tga` both reach `image/x-tga`; swayimg comes first
    because its alias line comes first).
- Binary: 2 254 728 bytes (1 918 968 stripped), +275 KB; 18 direct libraries, unchanged.

**Tested by the author**
- On screen: an ORF in darktable, five ORFs in one darktable, a JPEG in swayimg (double click
  too), a text file in Helix inside foot, the focus on every new window (Umbriel honours the
  token), the zip's error, Enter on a folder, F1, no zombies, swayimg left open after Ctrl+Q:
  "funziona tutto, anche il focus".
- A file swayimg cannot decode (`broken.jpg`) did not give an error: the author's swayimg config
  has `imagelist.adjacent = true`, so swayimg loads the folder, skips the broken file, shows the
  next one (an ORF) and exits 0. Right from Nordend's side: the application started and is
  running; only a quick non-zero exit is reported.

**Open**
- "Open with…" (every application for a type, choosing one) needs a menu: a later step, on the
  same `Applications`.
- The terminal cannot be set in the config yet (`$TERMINAL` can): it comes with the external
  editor's `editor.terminal` (DESIGN appendix).
- `DBusActivatable=true` applications start from their `Exec` line (none installed here).
- `Path` of an entry wins; otherwise the process starts in the first file's folder (handy for
  terminal applications; GLib keeps the caller's folder).
- Whether Umbriel gives the focus to the new window with the token: to be seen on screen.

---

## 2026-10-28 — Status bar and shortcuts (ROADMAP 5.11)

**Done**
- `views::StatusBar` under every view: at the left the folder's status (moved from the top
  bar: "12 items", "1 item", "3 of 12 selected", the error) and "824.7 GB free of 1.0 TB"
  (`statvfs` on a worker, `f_bavail`: what the user may write); at the right a button per view
  (layout-columns, layout-grid, list-details; the one shown marked) and "?" (help).
- The free space is read again when the folder or its listing changes:
  `DirectoryModel::revision()` counts the notifications, so a copy into the folder shown
  (any program) refreshes it.
- `views::shortcuts()`: one table of every shortcut (group, keys as shown, action, where it
  works, modifiers, keysyms, the browser's command if any). The browser runs its commands from
  it (Ctrl+1/2/3, Ctrl+I, Ctrl+B, F6, Space, F1/?) and passes a view or the sidebar only the
  chords listed for it (`listed()`). The views' `key()` now returns whether it used the key.
  Consequence: chords that worked by accident are gone (Alt+Left, Shift+Enter, Ctrl+Escape,
  Alt+Page Down... did the same as the plain key).
- `views::ShortcutsOverlay`: F1, "?" (Shift ignored: it needs it on most layouts) or the "?"
  button show the table on a card, two columns of groups; F1, "?", Escape or a click close it,
  and nothing under it reacts meanwhile.
- Tests (136 with the raw samples): every chord of every shortcut does something in every view
  it is listed for, in the sidebar and in Quick Look (checked to fail with a bogus Ctrl+Z
  entry); a sweep of ~70 keys with no modifier, Ctrl, Shift, Alt finds nothing that works
  without being listed; Ctrl+Q stays the window's; the overlay swallows keys; the status texts;
  the free space read once per listing and again after a file arrives. Release (6 runs of the
  new tests), ASan, TSan pass; snapshots under ASan and TSan; tidy and format clean.

**Measured**: nothing per frame (see benchmarks).

**Tested by the author**
- On screen (screenshots with Noctalia's `screenshot-window`): the status bar, the view
  buttons, the shortcuts overlay, the free space after a copy, and the breadcrumb fix below:
  "funziona tutto".

**Breadcrumb fix** (found by the author on screen, from 5.10): in the columns, a click on a step
whose column was open only focused it, and the deeper columns stayed. Now
`ColumnView::focus_folder` closes the columns after the next one and that one, the folder we
came from, shows again without a cursor (test updated).

**Open**
- Ctrl+Q is in the table but handled by the app (`main.cpp`), so the tests cannot drive it.
- The sweep leaves out Ctrl+Shift and Ctrl+Alt combinations: they are not listed, so the
  browser filters them, but no test sends them.
- Buttons have no tooltips.

---

## 2026-10-27 — A breadcrumb (ROADMAP 5.10)

Asked by the author with the status bar, the shortcuts overlay and a settings panel; agreed
order: breadcrumb (5.10), status bar and shortcuts (5.11), phase 6, then settings with the
remembered state (7.0b). System information (distro, Noctalia, Umbriel) goes to the settings'
"About", not the status bar.

**Done**
- `views::Breadcrumb`, drawn by the `Browser` above every view (the views no longer draw a
  title bar: they get the bounds under it). The folder's path as steps: the home as its icon
  when the folder is in it, else the root as the file system's icon, then the folders' names
  separated by chevrons; the last step (the folder shown) brighter. Too long: the first step,
  "…", and as many of the last steps as fit; the last one is cut in the middle when even it
  does not fit. Hover box on the steps; the status ("12 items"...) stays at the right until
  5.11 moves it.
- A click on a step: `Browser::go_to(folder, child)`. In the grid and the details the folder
  opens with the cursor on the step after it. In the columns, the column showing that folder
  is focused (its cursor is already on the way down; the columns after it stayed: fixed with
  5.11, they close); above the
  first column, the columns are revealed from it down to the step after it.
- The pointer goes to the sidebar, the breadcrumb or the view, and stays with the one that got
  a press until the release: a drag of the info pane's edge that crosses the sidebar no longer
  loses its release.
- The details table leaves out the type, then the date, then the size when the name would be
  narrower than its 160 px minimum. It overlapped before (with the info pane, under about
  700 px of view); the sidebar made that common at ordinary window widths.
- Tests (130 with the raw samples): the steps from home or root, going up in the grid, the
  details and the columns (focused column, revealed from above). Release, ASan, TSan pass;
  snapshots under ASan and TSan; tidy and format clean.

**Measured**: no measurable cost per frame (see benchmarks).

**Tested by the author**
- On screen: the columns (focusing an open column, revealing from above the first), the grid
  and the details, the home icon, a narrow window: "funziona tutto quello che dici".

**Open**
- The "…" is not clickable (the steps it hides are reachable from the first one).

---

## 2026-10-27 — The places sidebar (ROADMAP 5.9)

Asked by the author before phase 6: the classic sidebar was in neither the DESIGN nor the
ROADMAP (only udisks2, after the MVP). Added to both with their OK.

**Done**
- `fs::load_places` (`places.*`): Home; the XDG user dirs that exist, from `user-dirs.dirs`
  (Desktop, Documents, Downloads, Music, Pictures, Videos; templates and public share left
  out; without the file, the folders with those English names in the home, as on the author's
  machine, which has no `user-dirs.dirs`); the file system; GTK bookmarks
  (`~/.config/gtk-3.0/bookmarks`, local `file://` ones, percent-decoded); mounts under
  `/run/media`, `/media`, `/mnt` from `/proc/self/mountinfo` (octal escapes decoded). Labels are
  the folders' names, so localized user dirs show as they are on disk.
- `fs::PlacesWatcher`: reloads when the mount table signals POLLPRI (in the one `poll()`) and
  when `user-dirs.dirs` or the bookmarks change (`core::FileWatcher`, only for folders that
  exist, since it creates missing ones); tells the browser only when the list differs.
  `/proc` files report size 0, so the mount table is read in chunks, not by
  `core::read_text_file`.
- `views::Sidebar`: sections PLACES / BOOKMARKS / DEVICES, the place holding the current folder
  marked (longest matching path, whole names: Pictures before Home before the file system),
  hover, a click opens the place in the view shown (`Browser::open`), scrolls when taller than
  the window, right edge draggable (140-400 px, default 200, double click restores), left out
  when the view would keep less than 480 px.
- `Browser`: draws it at the left, the view in the rest; routes the pointer (leave sent to
  whichever loses it; a drag stays with the sidebar). Ctrl+B shows/hides it. F6 moves the
  keyboard in (the cursor starts on the current place) and out; inside, Up/Down/Home/End move,
  Enter opens and goes back to the view, Escape goes back; a click in the view also takes the
  keyboard back. Ctrl+1/2/3, Ctrl+I, Ctrl+B work from the sidebar too.
- Keyboard: the proposal said "Ctrl+B and arrows"; Ctrl+B both showing and entering was
  ambiguous, so entering is F6 (as in GTK file managers).
- `nordend-snapshot` lists the places of `$HOME`/`$XDG_CONFIG_HOME`; "Ctrl+B" and "F6" in
  `--keys`.
- Tests (127 with the raw samples): parsing user dirs, bookmarks and mounts; loading with and
  without `user-dirs.dirs`; the watcher following a bookmarks change; every place icon in the
  font; `place_holding`; F6/Down/Enter/Escape/Ctrl+B through the browser. Release (8 runs of
  the places tests), ASan, TSan pass; snapshots under ASan and TSan; tidy and format clean.

**Measured** (see benchmarks): the sidebar costs 0.03-0.06 ms per frame; the mount table's
POLLPRI fires once per mount or unmount and never while idle.

**Tested by the author**
- On screen: the three views, Ctrl+B, F6, dragging the edge. A loop-mounted ext4 image under
  `/mnt` appeared under DEVICES and went away on `umount` without a restart; so did an NTFS
  disk mounted by hand (ntfs3, read-only) under `/run/media`.
- Idle CPU with the window open: 0 ticks in 30 s (`/proc/PID/stat` utime+stime).
- Found while testing: a folder with a localized name ("Scaricati") does not show without
  `user-dirs.dirs` (only the English names are guessed): the file has to name it.

**Open**
- Mounting, unmounting and ejecting: udisks2 (ROADMAP 11). Mounts show the "database" icon,
  removable or not (telling them apart needs sysfs or udisks).
- Adding and removing bookmarks from Nordend (Ctrl+D): a possible follow-up.
- While the keyboard is in the sidebar, the view's selection still looks focused.
- The width and the shown/hidden state are not remembered across runs.
- The home is not watched: a standard-named folder created while Nordend runs shows after a
  restart (or a change to `user-dirs.dirs`).

---

## 2026-10-27 — The info pane's width (5.8, follow-up)

Asked by the author after testing 5.8.

**Done**
- In the grid and the details, the line between the view and the info pane can be dragged:
  the pointer turns into ew-resize over it (4 px either side), the line turns primary while
  grabbed, a double click brings the pane back to 300 px (as for the columns). The width is
  `InfoPane`'s, so both views share it; it stays within 240-720 px, and the view keeps at least
  320 px (`InfoPane::width_in`: the pane narrows, then is left out). In the grid the cells per
  row change while dragging and the cursor's row stays where it is (`keep_place`).
- The preview follows the pane: as wide as the pane (at most half its height), so a wider pane
  shows a larger photo; the rows keep to their centered 360 px column.
- `nordend-snapshot --info-width W`. Test (118 with the raw samples): the width's limits and
  `width_in`. Release (5 runs), ASan, TSan pass; snapshots with a 600 px pane walking an ORF
  folder in the grid and the details under ASan and TSan; tidy and format clean.

**Tested by the author**
- On screen, in the grid and the details: dragging, limits, double click, narrow windows:
  "perfetto, funziona tutto". With this, phase 5 is complete.

**Open**
- The columns' pane is not draggable: it is their last column and takes what is left.
- The width is not remembered across runs.

---

## 2026-10-27 — A preview in the info pane (ROADMAP 5.8)

Asked by the author: Ctrl+I only where the pane is optional, a preview in it, the content
centered and structured, more information behind a "more details" control.

**Done**
- `views::InfoPane` (public now, owned by the `Browser`, shared by the three views): the
  content is one centered column (at most 360 px) with, from the top: the preview (a photo's
  or raw file's thumbnail in a square reserved at once, so nothing moves when it arrives; a
  text file's first 14 lines on a small page; else the icon), name and type, then sections:
  INFORMATION (size or items, pixel dimensions, created, modified), PHOTO (camera, lens,
  exposure "1/2000 s · F5.6 · ISO 200", focal length, date taken, the darktable rating), and
  "More details" (a click) for MORE: permissions, owner, location, link target, and the
  exposure program, compensation, metering, white balance, flash, software. Nothing selected:
  the folder itself; several: how many and how big. It scrolls (wheel, touchpad) when taller
  than the window. What the entry says shows at once; the rest is read on a worker
  (`preview::summarize`), the thumbnail comes from the grid's `Thumbnails` store at the size
  the square needs (xx-large at scale 2).
- `image::read_photo_info`: EXIF as exiv2 prints it, size upright, the maker left out when
  the model names it or the maker is a long name ("OM-1", "Canon EOS 5DS", "SONY ILCE-7M2"),
  and `Xmp.xmp.Rating` from `FILE.xmp` (darktable; -1 is "Rejected"). Checked on the 22 raw
  samples and the author's OM-1 ORF, JPEG and WebP: 0.1-7.6 ms each.
- `preview::Info` has the birth time (statx); `exiv2` is set up once for both its users.
- Ctrl+I acts on the grid and the details only; the columns always show the pane, their last
  column.
- `nordend-snapshot --info-expanded` and `--walk N` (the cursor walks down a frame at a time,
  workers' results delivered between frames; only frames are timed).
- Tests (117 with the raw samples): the shot read from a new fixture `shot.jpg` (written by
  `make.sh` with exiftool, with a sidecar rated 3), a plain PNG, a missing file; the summary of
  a photo, a long text file (40 lines kept) and a folder; Ctrl+I leaves the columns alone.
  Release (5 runs), ASan, TSan pass; `nordend-snapshot` walking an ORF folder with the pane
  expanded in all three views under ASan and TSan; tidy and format clean.

**Measured** (docs/benchmarks.md), the "Fatto quando": walking a folder of 163 ORF one entry
per frame at 2880x1620 with the pane: 0.41-0.61 ms median, 0.62-1.39 ms p95, 3.7 ms at most
(thumbnail uploads) per frame, cold and warm cache. Once the cursor stops, the shot appears in
about 4 ms and the thumbnail in about 107 ms the first time (xx-large made from the 3200x2400
preview), 10 ms when cached.

**Decisions**
- One pane, the same everywhere: only its place changes with the view.
- Types are still MIME names; dimensions of Panasonic raw files are the sensor's (GX8:
  5264x3904), as exiv2 reports them.

**Tested by the author**
- On screen, walking ORF folders in the three views, "More details", Ctrl+I: "molto molto
  bene, perfetto, funziona tutto". Step 5.8 closed. Asked next: a draggable divider to widen
  the pane in the grid and the details.

**Open**
- The first thumbnail at pane size takes about 0.1 s per ORF: the grid's smaller one could stand
  in meanwhile, as in Quick Look.
- Fuji RAF dimensions are not reported; some exposure times print as decimals ("0.769231 s").
- Human-readable type names, a hand cursor over "More details", remembering it across runs.

---

## 2026-10-27 — Info pane in every view (ROADMAP 5.7)

Asked by the author after testing 5.6; shortcut and behaviour agreed before starting.

**Done**
- The columns' info pane is now `draw_info_pane` (`src/views/src/info_pane.*`), shared: the
  columns draw it after their last column as before, the grid and the details at their right,
  300 px wide, the view narrower by as much. Besides a file and a multiple selection, it shows a
  folder under the cursor (icon, type, date) and, with nothing selected, the folder itself
  ("Items"). It is left out when the view would keep less than 320 px.
- Ctrl+I shows and hides it, one setting for the three views (`Browser::set_info_visible`),
  shown at start. In the grid the cells per row change: the zoom's anchor (now `keep_place`)
  keeps the cursor's row where it was on screen.
- Tests (115 with the raw samples): Ctrl+I toggles it in every view and through view switches,
  cursor and selection untouched, Caps Lock's keysym included. Release (5 runs), ASan, TSan
  pass; `nordend-snapshot` toggling the pane in grid, details and columns under ASan and TSan;
  tidy and format clean.

**Measured** (docs/benchmarks.md): scrolling the 10 030-entry folder at 2880x1620 with the pane
open or closed: details 0.33-0.46 ms median (it varies between runs either way), 0.50-0.54 ms
p95; grid 0.28-0.30 ms median, 0.46-0.48 ms p95. Checked on screen offscreen: hiding the pane in
a grid scrolled to entry 5 000 keeps the cursor's row in place (5 → 7 cells per row).

**Tested by the author**
- On screen, in the three views, over photo folders: "funziona tutto". Step 5.7 closed.

**Open**
- The pane could show the thumbnail of an image; the setting is not remembered across runs.

---

## 2026-10-27 — Details view (ROADMAP 5.6)

**Done**
- `views::DetailsView` (Ctrl+3): a row per entry with name (icon, name cut in the middle), size
  (right-aligned, "—" for folders), date and type, under a header. A click on a title sorts by
  it (a chevron shows the direction), a second click reverses; dragging a column's left edge
  resizes it (the pointer becomes ew-resize, the name takes what is left, every column keeps a
  minimum). Keys and mouse as in the grid, with Right/Left opening the folder / the parent as
  in the columns. Selection, hover, scrolling with inertia, Quick Look over it: as elsewhere.
- `Browser` has three modes and switches between any two (Ctrl+1/2/3) handing over the same
  `Folder`: cursor, selection and sniffed types stay.
- Sorting: `fs::SortKey::type` (by type name, types the name does not tell last, then the
  name) and `Folder::set_order` (cursor and selection follow their entries by name, the view
  jumps to the cursor). By size, directories keep their name order: their own size is their
  listing's (found by a test: on tmpfs it depends on the content).
- `nordend-snapshot --sort FIELDS` clicks titles offscreen ("modified,modified": newest first);
  `--bench` scrolls the details.
- Tests (114 with the raw samples): switching through all three views keeps the folder, cursor
  and selection; sorting by size, reversed, by type; the cursor stays on its entry; a folder
  opened in the details is sorted the same way, and the columns show it so; Right/Enter and
  Left/Backspace; type order with unknown types last; directories by size. Release (5 runs),
  ASan, TSan pass; `nordend-snapshot` over the details (sorted, benchmark, Quick Look, switching
  views) under ASan and TSan; tidy and format clean.

**Measured** (docs/benchmarks.md), the "Fatto quando": a 10 030-entry folder (sparse files of
random sizes and dates) sorts by size in 1.1 ms, by date in 1.1-2.0 ms, by type in 3.2 ms; it
scrolls at 0.31-0.45 ms per frame (median), 0.53 ms p95, at 1440x810 and 2880x1620.

**Decisions**
- The order belongs to the details view and is applied to each folder it shows; a folder keeps
  it in the other views (the columns show the same folder sorted the same way). The columns and
  the grid have no sort control yet.
- Types are shown as MIME names ("image/x-olympus-orf"): readable descriptions need
  shared-mime-info's comments, which the database does not load yet.
- By type, the type the name tells is used (sniffed types arrive later and would make rows
  jump).

**Tested by the author**
- On screen, over photo folders: sorting by every column and reversed, resizing columns,
  keys, scrolling, switching views: "funziona tutto". Step 5.6 closed. Asked next: the info
  pane of the columns in the grid and the details too, shown and hidden with a shortcut.

**Open**
- Human-readable type names; remembering the column widths and the order across runs (config
  or state file).

---

## 2026-10-27 — Text previews and the information card (ROADMAP 5.5)

**Done**
- New module `src/preview` (already in DESIGN): the `Provider` interface (`can_handle`, `kind`,
  `load(request, cancel)`) and three providers, chosen by `provider_for(mime)` in this order:
  images and camera raw files (`image::load_to_fit`, as before), text (the whole text/plain
  family: source code, JSON, XML, shell scripts, darktable `.xmp` sidecars), and the
  information card, which takes everything else. Quick Look keeps one `preview::Content`
  (image, text or information) per entry instead of an image; the provider is part of the slot
  key, so an entry whose type is sniffed later gets the right one.
- Text: the first 256 KB, split into lines (`\n` and `\r\n`), tabs expanded to 4 columns,
  lines cut at 1 KB on a character boundary, a UTF-8 BOM dropped; a NUL byte means binary (or
  UTF-16), and the card is shown instead. Drawn on a page up to 120 characters wide, with line
  numbers, long lines ending in an ellipsis; only the visible lines are shaped. The page
  scrolls with the wheel and the touchpad (the same `Scroller` as the views, with inertia);
  arrows still walk the folder. The caption says how many lines ("first N lines" when cut).
- Monospace text: `FontSystem` has a second fallback chain, looked up on first use, sharing the
  fonts both chains have; `Typeface::monospace` on `layout` and `draw_text`. New config key
  `font.monospace` (default "monospace": DejaVu Sans Mono here).
- The information card reads, on a worker: permissions as `ls -l` shows them (in monospace),
  owner and group names, a link's target, and a folder's number of entries.
- `nordend-snapshot --bench` scrolls Quick Look's text page when one is open.
- Tests (110 with the raw samples): lines, CRLF, tabs, BOM, binary and UTF-16, cuts by bytes and
  by line length (UTF-8 kept whole), permissions with the sticky bit, folder entries, links
  (dangling too), the provider per type; Quick Look shows text and a folder's card, and a wheel
  turn over text does not close it. Release (5 runs), ASan, TSan pass; `nordend-snapshot` over
  text, XMP, binary, folder and link, and the text benchmark, under ASan and TSan; tidy and
  format clean.

**Measured** (docs/benchmarks.md): a 1.6 MB log read (its first 256 KB, 3 196 lines) in
0.67 ms, a 10 000-entry folder counted in 4.3 ms; scrolling that log's page at 2880x1620:
0.82 ms median per frame, 1.03 ms p95.

**Found**: Quick Look over a card (also on the previous commit) makes TSan report a race
inside `libdrm_amdgpu`, between Mesa's gl0 thread and a glyph upload: both sides are driver
code. With the author's OK the suppression now covers `libdrm_amdgpu` too; it relies on GL
being called from the main thread only, as the renderer already requires.

**Decisions**
- The provider interface follows DESIGN, plus `kind()`: the view must know what to draw before
  the content arrives (the thumbnail for an image, nothing for text, which takes a few ms, the
  card at once for the rest).
- Text is not wrapped: code reads better with whole lines, and the ellipsis shows a line goes
  on. Tabs are 4 columns.

**Tested by the author**
- On screen, over source code, long files, folders, binaries and darktable sidecars, scrolling
  with touchpad and wheel: "tutto perfetto". Step 5.5 closed. Asked for later: syntax
  highlighting and text selection in the preview (ROADMAP 15, DESIGN "Dopo l'MVP").

**Open**
- Text in other encodings (Latin-1) shows replacement characters; UTF-16 gets the card. (Fixed
  in 3b-1, 2026-11-08.)
- No syntax highlighting, no horizontal scrolling, no selection or copy in the text page
  (highlighting and selection: ROADMAP 15).

---

## 2026-10-27 — Raw previews (ROADMAP 5.4)

The "Fatto quando" (Quick Look of an ORF < 100 ms) was already met in 5.3 (51-55 ms); this step
went through the open points of 5.2 with the rawsamples.ch files of other cameras.

**Done**
- TIFF previews: `EmbeddedPreview` now carries `data` (a JPEG or a TIFF) and `image::decode_preview`
  decodes either, cropped. Our reader for the simple kind raw files carry (uncompressed, chunky,
  8-bit RGB, in strips) in `src/image/src/tiff.{hpp,cpp}`, with the TIFF integer reader moved
  there from `decode.cpp`. A TIFF is taken only when no JPEG is large enough and the TIFF is
  larger: the Hasselblad 3FR now has a thumbnail and a Quick Look (320x240, its only preview).
- Canon's TIFF previews are never taken: extracted and looked at, they are sensor data without
  white balance (green, dark), 16 bits on the 1D X and 5DS. So the Canon 5DS large thumbnail
  stays at ~370 ms (its next preview is the 50 MP JPEG).
- Panasonic crop: the GX8 stores a 4:3 preview whatever the aspect ratio, with the frame in its
  first IFD (CropTop/Left/Bottom/Right, tags 0x2F-0x32, which exiv2 0.28.9 does not expose),
  relative to the sensor borders (0x04-0x07). Read by us; the crop applies to every preview of
  the file, and the full size reported is the cropped one (16:9: 5184x2920). The GX7 has no
  crop tags (it stores the preview cropped).
- Previews shaped unlike the largest one (over 2 %) are dropped: the 4:3 EXIF thumbnail of 3:2,
  16:9 and 1:1 photos has black bars (Canon, Nikon, Sony, GX7 1:1, LX3 16:9; seen in the grid,
  then measured). A centered crop was tried on paper and rejected: the bars are not exactly
  where the aspect ratio puts them (Nikon: 104 content rows, not 106.7), a dark line would stay.
- `image/tiff` is in `thumbs::can_make` (Nikon TIFF, 3FR; a TIFF without a preview is tried once
  and remembered as failed).
- `decode_file` reads 16 bytes and sniffs before reading the rest: Quick Look of a raw file
  whose preview is smaller than the screen no longer reads (and zeroes) the whole file first.
- Tests (104 in total): a TIFF-preview fixture (96x64 in strips of 7 rows, from `make.sh`),
  cut TIFF, crop in and out of bounds, `can_make`; one test over the raw samples, skipped when
  the folder is missing: every file and size gives a decodable preview of the size it says;
  3FR TIFF, no Canon TIFF, no barred EXIF thumbnail, GX8 crops, GX7 untouched. Release (5
  runs), ASan, TSan pass, also `nordend-snapshot` (grid and Quick Look over the samples, Quick
  Look over JPEG/WebP) under ASan and TSan; tidy and format clean.

**Measured** (docs/benchmarks.md): Quick Look of the 3FR 42 → 0.4 ms, Nikon TIFF 89 → 2.3 ms,
GX8 18 → 11 ms, ARW 16 → 13 ms; the ORF unchanged (51.6 ms). Normal-size (128) thumbnails of
3:2 and 1:1 photos got slower, since the barred EXIF thumbnail is skipped: Canon 5DS 9 → 372 ms,
1D X 9 → 143 ms, ARW 7.5 → 22 ms. Large (256), which the grid uses at scale 2, was already
that slow before.

**Decisions**
- Correct shape over speed for the normal size: a thumbnail with black bars is wrong forever (it
  is cached), the cost is paid once per file.
- TIFF previews are a last resort, behind JPEGs, because only some can be read and some are
  not images at all (Canon).

**Tested by the author**
- On screen, with a fresh cache over the raw samples (grid: every file has a thumbnail, no
  black bars, GX8 in its aspect ratio; Quick Look of the GX8 and the 3FR) and over ORF folders:
  "funziona tutto come hai detto". Step 5.4 closed.

**Open**
- Canon 5DS / 1D X: thumbnails and Quick Look decode the full-size JPEG (370 / 143 ms). Only a
  JPEG decoder with reduced-scale decoding would fix it (TurboJPEG 1/8: measured in 5.2 at half
  the cost for an OM-1 JPEG); not needed for the author's ORFs.
- Thumbnails already in `~/.cache/thumbnails` from before (barred, or GX8 uncropped) stay until
  the file changes: the freedesktop cache has no version. Deleting the folder remakes them.
- The 3FR shows at 320x240 in Quick Look: never enlarged, as every image.

---

## 2026-10-27 — Quick Look (ROADMAP 5.3)

**Done**
- `image::load_to_fit(file, w, h)`: the image to show in w x h physical pixels: an embedded
  preview when it is at least that large (or the file is raw), else the whole image; scaled
  first, turned after; reports the full image's size (exiv2's pixel size for previews).
  `EmbeddedPreview` now carries the main image's size.
- `views::QuickLook`, owned by `Browser`: Space (no modifiers) opens it on the cursor entry of
  the current folder (focused column, or the grid's). It follows the cursor: every other key goes
  to the view underneath (arrows, Page, Home/End, also Enter or Ctrl+1/2) and Quick Look shows
  where the cursor lands, closing when it is on nothing; Space, Escape or a click close it
  (Escape does not reach the view: a multiple selection survives). Over a scrim with a 140 ms
  fade: images and raw files fitted to the window and never enlarged past their pixels, rounded,
  with a shadow and a caption (name, full size, file size). Two stages: the cached thumbnail at
  once (stretched where the image will land), then the sharp image. The entries before and after
  are made in advance (nearby priority); everything else is cancelled. Other entries get an
  information card (icon, name, type, size, date); text previews are ROADMAP 5.5.
- `image::scale_to_fit` rewritten as a streaming area filter: no intermediate image (it was 83 MB
  for 3200x2400 → 2160x1620), an opaque fast path, one reciprocal per pixel: 84 ms → 27 ms.
  Thumbnails gain too (xx-large from an ORF preview: 26 → 16 ms).
- Textures dropped outside a draw are kept until the next draw, so GL deletes always happen with
  the context current.
- 4 new test cases (101 in total): open on the cursor, arrows follow, upright size, information
  card, Escape and Space close; Escape does not collapse a selection, Ctrl+Space stays
  selection; over the grid, a click closes; an image whose type is not known yet is shown once
  sniffed. Release (5 runs), ASan, TSan pass, also `nordend-snapshot` with Quick Look open over
  JPEGs and ORFs under ASan and TSan; tidy clean apart from reference members.

**Found by the tests**: `a.png` has no type from its name alone: shared-mime-info gives `*.png`
to both image/png (weight 50) and image/apng (40), so, as in GLib, the content decides and the
type arrives when the views sniff the visible rows. Quick Look decided only once whether an
entry was an image; now `follow()` checks again every frame (cheap, idempotent) and asks for the
sniffing of the shown entry and its neighbours itself.

**Measured** (docs/benchmarks.md): an OM-1 ORF fitted to a 2880x1620 screen in 51-55 ms (the
5.4 target is < 100 ms), a 20 MP JPEG in 212 ms (its preview is smaller than the screen, so the
whole image is decoded; the thumbnail shows meanwhile).

**Decisions**
- Quick Look follows the view's cursor rather than keeping its own position: one cursor, and
  closing leaves it on the last entry seen (as Finder).
- The sharp stage is made for the image area of the last draw (the window size), not the
  image's full size: no zoom yet.

**Tested by the author**
- On screen, in columns and grid, over ORF, WebP, JPEG and the raw samples: "tutto perfetto,
  fluidità ottima, caricamento fulmineo". Step 5.3 closed.

**Open**
- GIF and WebP animations show their first frame.
- No zoom or 1:1 view; a window made larger after opening keeps the image made for the old size
  until the next entry.

---

## 2026-10-27 — Thumbnails (ROADMAP 5.2)

**Done**
- Measured first (docs/benchmarks.md): TurboJPEG's DCT scaling only halves the cost of an OM-1
  JPEG (145 ms with wuffs, 75 ms at 1/8), while the files carry a 1600x1200 preview (JPEG) or a
  3200x2400 one (ORF) that decodes in 8 / 30 ms. With the author's OK: no libjpeg-turbo, exiv2
  now (planned for 5.4) to find the embedded previews. `nordend-bench-jpeg` stays as a dev tool,
  built only when libjpeg-turbo-devel is installed.
- `image::embedded_preview(path, min_side)`: exiv2's previews (EXIF thumbnail, raw previews),
  plus the Olympus / OM System maker-note preview that exiv2's PreviewManager does not list for
  JPEGs: at TIFF start + `Exif.MakerNote.Offset` + `Exif.OlympusCs.PreviewImageStart` (checked
  against exiftool on a JPEG and an ORF). Picks the smallest one big enough; reports the main
  image's orientation, which the previews share (they are stored as the sensor sees, checked on
  screen).
- `image::scale_to_fit` (area average, alpha weighted, 2x2 halving first), `image::encode_png`
  (our writer on zlib, level 1, per-row filter by minimum sum; tEXt chunks) and
  `image::png_text`. A shared JPEG segment walker gives EXIF and frame size.
- New module `src/thumbs` (already in DESIGN): the freedesktop cache. MD5 of the file URI (our
  RFC 1321 code), URI escaped exactly as GLib's `g_filename_to_uri` (checked with `gio`), sizes
  normal/large/x-large/xx-large, valid while Thumb::URI and Thumb::MTime match, written through
  a 0600 temporary file and a rename, directories 0700, failures remembered in
  `fail/nordend-<version>/` until the file changes, the cache never thumbnails itself. Source
  order: valid cached thumbnail, embedded preview big enough, whole image, else the largest
  preview (raw files). Scaled first, turned upright after. `thumbs::can_make(mime)`: the five
  decoded formats and the camera raw types.
- UI: `ui::Texture` (RGBA, premultiplied on upload, mipmapped) and `Renderer::draw_image`
  (textured quad cut to rounded corners by the box distance function, clipped like the rest).
- `views::Thumbnails`: thumbnails as textures for the views. Asked for per frame by what is
  drawn, made on workers (visible priority) through the cache, cancelled when no longer drawn,
  textures kept within 256 MB (least recently drawn dropped first); textures are created while
  drawing. The grid shows them in the icon's square at the cache size that covers the icon in
  physical pixels; the icon shows until the thumbnail is ready. The app uses
  `~/.cache/thumbnails`; `nordend-snapshot --thumbnails DIR` takes another cache (none by default).
- `nordend-thumb [--size ...] [--cache DIR] [--make] [--ppm OUT] FILE...`.
- Fixture `with-thumbnail.jpg` (480x320 with a 192x128 EXIF thumbnail, orientation 6); `make.sh`
  now writes PNGs without date chunks (reproducible). 10 new test cases (97 in total): scaling
  and alpha, PNG round trip with text, embedded preview and orientation, MD5 (RFC suite and the
  spec's example), GLib URI escaping, sizes, cache hit / miss on mtime change / preview or
  decode by size / permissions, failures remembered and retried, the cache not thumbnailing
  itself. Release, ASan and TSan pass, also `nordend-snapshot` making thumbnails on all workers
  under ASan and TSan; tidy clean apart from reference members.

**Measured** (docs/benchmarks.md): a thumbnail from an OM-1 JPEG in about 12 ms (large), from
an ORF in about 36 ms; a screenful of the grid in 0.15-0.4 s with an empty cache, 12-22 ms warm;
scrolling with thumbnails 0.62 ms per frame.

**Decisions**
- zlib level 1 for the thumbnail PNGs: 5 times faster than level 6 for files 13 % larger.
- Thumbnails are made only for the types listed in `thumbs::can_make`; other images (SVG, TIFF,
  ...) keep their icon rather than filling the failure cache.
- The thumbnail cache is the shared one (`~/.cache/thumbnails`), as the ROADMAP asks; no
  private cache.

**Tested by the author**
- On screen: thumbnails in the grid, scrolling, every cell size, switching views, the cache
  reused on the next start, `nordend-thumb` reading from the cache: "funziona tutto". Step 5.2
  closed.

**Open**
- `views::Thumbnails` has no unit test: it needs a GL context for its textures; it is covered by
  the snapshots (also under ASan/TSan).
- The Olympus maker-note path is covered by real files only (no small fixture carries one).
- `nordend-bench-dir` makes empty files: a folder of 10 000 real-looking images would stress the
  request/cancel path; not done yet.
- Thumbnails are not removed when their file is deleted (the spec leaves that to cleaners).
- Raw samples from rawsamples.ch (22 files, Canon, Fuji, Hasselblad, Nikon, Panasonic, Sony;
  kept by the author in `DATA/fotografia/raw_text_examples`, not in the repo): 20 of 22 get a
  thumbnail from their JPEG preview (5-25 ms at large). Found, for ROADMAP 5.4: (1) Hasselblad
  3FR carries only an uncompressed TIFF preview (320x240), which we skip; Canon 5DS has a TIFF
  preview of 556x370 next to its 50 MP JPEG, so "large" decodes the 50 MP one (366 ms): reading
  simple uncompressed TIFF previews would fix both. (2) Panasonic GX8 stores the 4:3 sensor
  preview for every aspect ratio and records the crop in tags (`Crop*`); the GX7 stores it
  already cropped. (3) `image/tiff` is not in `can_make` (the D800 TIFF has a 570x375 JPEG
  preview); 3FR is detected as `image/tiff` too.

---

## 2026-10-27 — Image decoding (ROADMAP 5.1)

**Done**
- New module `src/image` (`nordend::image`, added to DESIGN's architecture with the author's
  OK): `decode(bytes)` / `decode_file(path)` give RGBA pixels with straight alpha, the format and
  the EXIF orientation, or an error; never throw on bad input; thread-safe for the workers.
  `sniff()` tells the format from the first bytes. Images over 200 MP are refused before any
  pixel memory is allocated.
- PNG, JPEG, GIF with wuffs, vendored (author's OK) from google/wuffs `v0.4.0-alpha.12`, file
  unmodified in `third_party/wuffs`, listed in `THIRD_PARTY.md`. Built as its own static library
  with only the PNG/JPEG/GIF modules and the `wuffs_aux` API, without our warnings; decoding goes
  straight into the Image's vector (no copy).
- WebP with libwebp (still images, the first frame of animations through the demuxer, EXIF
  chunk) and JPEG XL with libjxl (first frame, orientation reported rather than applied, so all
  formats go through the same turn). New build dependencies: `libwebp-devel`, `libjxl-devel`.
- EXIF orientation: a small TIFF reader for the orientation tag (both byte orders, bounds
  checked) and `apply_orientation()` for the eight cases. wuffs does not report JPEG metadata
  (its `set_report_metadata` is empty for JPEG), so the JPEG segments before the image data are
  walked for the APP1 "Exif" one.
- `nordend-decode [--runs N] [--stored] [--ppm OUT] FILE...`: format, size, orientation, read
  and decode times; the PPM to look at the result.
- Fixtures in `tests/data/image` (a 48x32 six-block image in every format, an orientation-6
  JPEG, an animated WebP, a truncated JPEG), generated by `make.sh` there. 6 new test cases (87
  in total): formats, same pixels from every format, alpha, orientation applied or not, the
  eight orientations, EXIF in both byte orders and malformed, truncated/missing/too large files.
  Release, ASan, TSan pass (and `nordend-decode` under ASan on real 20 MP photos); tidy clean
  apart from reference members.

**Measured** (docs/benchmarks.md): a 20 MP OM-1 JPEG decodes in about 149 ms as stored, 218 ms
turned upright; PNG 229 ms, WebP 210 ms, JPEG XL 394 ms (one thread) at the same size.

**Decisions**
- The full-size EXIF turn (about 65 ms at 20 MP) stays out of the hot paths: thumbnails turn
  after scaling, Quick Look on the GPU. `DecodeOptions::apply_orientation = false` is there for
  that.
- wuffs's JPEG has no decoding at reduced size; libjpeg-turbo does (DCT scaling). Whether to
  use it for thumbnails is decided in 5.2, with a real measurement (ImageMagick suggests a large
  gain, *to be verified*).

**Tested by the author**
- Tests pass on their machine; `nordend-decode` on two OM-1 JPEGs: 3888x5184, orientation 8,
  214-218 ms; the decoded photo, opened in swayimg, stands upright with the right colors; every
  fixture decodes, the truncated JPEG fails cleanly. Step 5.1 closed.

**Open**
- JPEG XL decodes on one thread; libjxl's parallel runner could help Quick Look (5.3).
- No cancellation inside a decode: callers check their token before and after.
- EXIF orientation of PNG (eXIf chunk) comes through wuffs's metadata report; not tested with a
  real file yet.

---

## 2026-10-27 — Grid view (ROADMAP 4.4)

**Done**
- `views::Folder`: the per-folder state split out of `Column` (model, cursor, selection, anchor,
  sniffed types), with nothing about drawing. Views hold it by `shared_ptr`, so the same object
  passes between the columns and the grid: cursor, selection and sniffed types stay, nothing
  reloads. Where the cursor sits on screen is the view's job: the Folder only records how to
  bring it into sight (`take_reveal()`: glide after a move, jump after it lands or is asked for
  by name), and each view applies it at its next draw with its own geometry. `Column` is now
  only drawing, scrolling and hit testing.
- `views::GridView`: one folder as a grid of cells (icon and name, middle-ellipsized), filled row
  by row, stretched to the width, virtualized by rows like the list. Six sizes, 48 to 256 px
  (default 96): Ctrl+plus/minus, Ctrl+wheel, Ctrl+0; a zoom keeps the cursor (or the first
  visible cell) where it was on screen. Keys: arrows (Up/Down by a row, not past the first or
  last row), Page Up/Down, Home/End, the selection keys of the column view; Enter, Alt+Down or a
  double click open a folder, Backspace, Alt+Up or mouse back open the parent with the cursor on
  where we were. Plain Space stays free for Quick Look.
- `views::Browser`: owns both views; Ctrl+1 columns, Ctrl+2 grid. The grid takes the focused
  column's folder; the column view meanwhile is suspended (its folders' changes are not synced
  while hidden). Coming back, `ColumnView::show(folder)` brings the columns up to date and
  focuses the column holding that folder; if the grid went elsewhere, the columns unfold from
  the deepest column that leads to the grid's folder (its parent, when none does) and the grid's
  folder object becomes the last column as it is.
- Title bar and the item colors are shared by both views (`draw_helpers`, `item_style`).
- `nordend-snapshot --keys "Ctrl+2,..."` reaches the grid; `--bench` scrolls the grid when it
  is shown.
- 4 new test cases (81 in total): the grid shares the column's folder and the columns follow its
  changes; Enter/Backspace in the grid and the columns rebuilt around the grid's folder (also
  from above the first column); cell sizes and their limits. Release (10 runs), ASan, TSan
  (5 runs) pass; tidy clean apart from the reference members the views already had.

**Measured**: scrolling the 10 000-entry folder at 2880x1620, 0.33 ms median at the default size,
0.84 ms with the smallest cells all selected (docs/benchmarks.md).

**Decisions**
- Ctrl+1 / Ctrl+2 as in Nautilus (the Linux habit), not Finder's Cmd+1..4.
- The grid has no info pane: Quick Look (5.3) is its preview.
- A folder opened from the grid starts without a cursor (the first arrow puts it on the first
  cell); the columns put it on the first row when entering, because the next column shows it.
- Cells are at least 132 px wide, so a `P1234567.ORF` name fits at the smallest size.

**Tested by the author**
- On screen: switching columns ↔ grid with the selection kept, keys and clicks in the grid,
  zoom, touchpad and wheel scrolling, opening folders and going back: "all smooth", and the
  test suite passes on their machine. Step 4.4 closed, and with it phase 4's views.

**Open**
- The cell size is not remembered across sessions (a config entry, later).
- Keys pressed before the grid's first draw step one cell at a time (the row width is known at
  the draw): only the snapshot tool can do that.
- Thumbnails replace the icons in phase 5; names on one line for now (Finder uses two).

---

## 2026-10-26 — Selection (ROADMAP 4.3)

**Done**
- `views::Column` has a selection next to its cursor: a set of names plus an anchor, both kept by
  name like the cursor (they follow renames, lose deleted entries, and names that do not come
  back after a reload are dropped). Moving the cursor takes a `SelectMode`: replace (plain click,
  arrows), extend (Shift: from the anchor to the cursor), keep (Ctrl+arrows: cursor alone), toggle
  (Ctrl+click). Plus `select_all` (Ctrl+A), `collapse_selection` (Escape: back to the cursor's
  entry) and Ctrl+Space (toggle the cursor's entry, for non-contiguous selection from the
  keyboard). Plain Space is left free for Quick Look (5.3).
- The column invariant now reads "what column i has selected, when it is one entry": with several
  entries selected (or the cursor away from the selection) no column opens on the right and the
  info pane sums the selection up (count, folders and files, size of the files; folder sizes are
  not computed). The title shows "N of M selected".
- Drawing: selected rows are filled (primary in the focused column, quiet elsewhere); when the
  cursor is not the whole selection, a ring marks it.
- `nordend-snapshot --keys "Right,Shift+Down,Ctrl+a"` presses keys in the column view before the
  snapshot (xkb key names, Ctrl+/Shift+/Alt+ prefixes): offscreen checks of states that need
  input.
- 3 new test cases (77 in total): Shift ranges (grow, shrink back, Escape), Ctrl moves and
  toggles, Ctrl+A, plain Space ignored, toggling and ranges by row, selection across a rename and
  a deletion (inotify). Release, ASan, TSan pass (column tests 10 times in Release, 5 in TSan);
  tidy clean.

**Measured**: scrolling a 10 000-entry column at 2880x1620, 0.26 ms median with nothing selected,
0.52 ms with everything selected (docs/benchmarks.md).

**Decisions**
- A Shift range replaces the selection with anchor..cursor (Finder, Nautilus); Ctrl+click is the
  way to add to it.
- Selection lives in the focused column only in practice: clicking another column replaces its
  selection there; columns on the left keep theirs, which is the path to the focused one.
- No rubber-band (drag) selection: not in the ROADMAP; with the drag and drop of later phases.

**Tested by the author**
- On screen in the photo library: clicks with and without modifiers, keyboard ranges and toggles,
  Ctrl+A, Escape, the summary pane: "works by the book". Step 4.3 closed.

**Open**
- The summary counts what the listing holds; folder sizes would need a recursive walk (a worker
  job, later, if wanted).

---

## 2026-10-26 — Miller columns (ROADMAP 4.2)

**Done**
- `views::Column`: one folder as a virtualized list of icon, name and a chevron for folders,
  built from the 4.1 list (same scroller, same visible-rows drawing, same sniffing of visible
  entries). It has a cursor, kept by name: it follows its entry when rows move or the entry is
  renamed, is dropped when the entry goes, and can name an entry not loaded yet (pending: it
  lands when the entry arrives). Moving it glides the list just enough to keep it in view; a
  cursor that lands later jumps there, centered.
- `views::ColumnView`: Miller columns. One invariant does the work (`sync_after`): the column
  after column i always shows what column i's cursor is on (same folder: kept as is; another
  folder: a new column; a file or nothing: no column, the info pane shows the file). Keys and
  clicks only move cursors and focus. Up/Down, Page Up/Down, Home/End move the cursor (a folder
  opens at once in the next column); Right/Enter go into it; Left goes back one column (the
  column left behind stays open, without cursor), and from the first column opens the parent as
  a new first column, with the cursor on where we were (Backspace, Alt+Up, mouse back do the
  same). A click puts the cursor on a row (an empty area clears it).
- Resizing: drag a column's right edge (ew-resize cursor, the edge lights up), 140-800 px; a
  double click on the edge restores 260 px. A new column takes the width of the one it replaces,
  so a widened column stays wide while browsing.
- Horizontal scrolling: a second `ui::Scroller` for the row of columns, which glides by itself
  to show the focused column and what it opens. Touchpad (with inertia), tilt wheel and
  Shift+wheel scroll it by hand; a touchpad gesture locks to the axis it starts on and to the
  column it starts over, so a vertical swipe does not drift sideways. A parent column opened from
  the left slides in. Thin scrollbar at the bottom when the columns do not fit.
- Info pane, after the last column when its cursor is on a file: the file's icon (64 px), name,
  MIME type, size and date. It is the place the previews of phase 5 will take.
- `ui::Scroller::glide_to` and `destination()` (where a glide is heading: keeping a cursor in
  view while a key repeats must count from there, not from the current offset).
- `views::ListView` is gone: the columns replace it in the app and in `nordend-snapshot`
  (`--list` became `--columns DIR [--reveal PATH]`).
- 5 new test cases (74 in total): navigation with the arrows, Left from the first column, reveal,
  a folder renamed and then deleted under the cursor (inotify), `glide_to`. Release, ASan, TSan
  pass (column tests 10 times in Release, 5 in TSan); tidy clean.

**Measured**: scrolling a 10 000-entry last column with the whole view drawn, 0.19 ms median at
960x640, 0.23 ms at 2880x1620, as the list (docs/benchmarks.md).

**Decisions**
- Each column is its own DirectoryModel (its own watch and load), created when the cursor lands
  on a folder: holding Down through a hundred folders opens and cancels a hundred loads. Loads
  cancel cleanly and each costs well under a millisecond for ordinary folders; to measure on
  screen before adding any delay.
- No selection yet beyond the cursor (ROADMAP 4.3), and nothing opens files (6.1).

**Tested by the author**
- On screen: navigation, resizing, horizontal scrolling, info pane, and a folder created,
  renamed and removed from a terminal while shown: "everything works". Step 4.2 closed.

**Open**
- When the columns shrink (a cursor moved to a file in a left column), the horizontal offset is
  clamped to the new width at once instead of gliding: to judge on screen.
- The info pane shows the MIME type name (`image/x-olympus-orf`), not a readable description:
  shared-mime-info's comments live in the XML files, not parsed yet.

---

## 2026-10-26 — Virtualized scrolling list (ROADMAP 4.1)

**Done**
- `ui::Scroller`: scrolling physics with no clock of its own (time comes with input and
  frames, so tests replay it exactly). Wheel: 48 px per notch toward a target, gliding there
  (exponential approach, 50 ms time constant); notches during a glide add up. Touchpad: 1:1
  while the fingers move, then inertia from the release velocity (movement of the last 100 ms),
  decaying with a 330 ms time constant, stopping under 15 px/s or at an edge. Clamped to the
  content. Frames are requested only while a glide or inertia runs: idle stays at zero CPU.
- New module `views`. `views::ListView` shows a DirectoryModel: title (folder, item count,
  loading, error), column headers, rows of icon, name (middle ellipsis), size (SI units) and
  date ("2026-10-26 14:03", local), hover highlight, a thin scrollbar. Only the rows inside the
  viewport are drawn (`visible_rows`). Entries the name does not type are sniffed on a worker
  when they become visible (batches of 64, dropped when another folder opens): the "visible
  entries" half of ROADMAP 3.2. Double click on a folder opens it; Backspace, Alt+Up or the
  mouse back button go to the parent; arrows, Page Up/Down glide, Home/End jump.
- `nordend [DIR]` now opens DIR (default: home) in the list; `--test-scene` keeps the renderer
  scene. The 3.4 icon grid (`--icons`) is gone: the list replaces it.
- `nordend-snapshot --list DIR [--bench FRAMES]`: the list offscreen, and a scrolling benchmark
  (top to bottom at constant speed, glFinish after every frame). Also fixed: the first draft of
  the release velocity counted the first finger sample, overestimating by 20 % (caught by a
  test).
- ThreadSanitizer: Mesa's gallium GL thread reports races inside libgallium in every GL
  program (12 reports with the old test scene already). They are suppressed by a
  `__tsan_default_suppressions` entry for libgallium only; with it, the list benchmark and the
  tests report nothing in our threads.
- 6 new test cases (69 in total): wheel glide and accumulation, edges, inertia distance and
  stop, no coasting after a slow move or a pause, extent changes, size formatting, visible rows.
  Release, ASan, TSan pass; tidy clean.

**Measured** (offscreen, Release, glFinish per frame, 600 frames scrolling the whole list)

| List | Buffer | median | p95 | max |
|------|--------|-------:|----:|----:|
| 10 000 entries | 960x640 | 0.20 ms | 0.28 ms | 5.6 ms (first frame, glyph rasterization) |
| 10 000 entries | 2880x1620 (1920x1080 at 1.5) | 0.25 ms | 0.45 ms | 1.5 ms |
| 123 entries (photo folder) | 960x640 | 0.17 ms | 0.21 ms | 5.9 ms |

A 60 Hz frame has 16.7 ms: drawing the list takes about 1.5 % of it.

**Tested by the author**
- On screen: wheel, touchpad inertia, keys, opening folders: "the touch experience is
  magnificent", the keyboard handling "very smart". Step 4.1 closed. At the author's request the
  README gained a Features section, with scrolling as "Natural touch".

**Open**
- Text layouts are cached up to 8 192 and then the cache is cleared whole: fine at these numbers,
  to revisit (LRU) if long scrolls through huge folders show a hitch.

---

## 2026-10-26 — File-type icons (ROADMAP 3.4)

**Done**
- Icons are Tabler line icons, as in Noctalia (DESIGN: "Icone UI: Tabler"), drawn from an icon
  font and tinted by the palette. The font is found in the XDG data dirs: Nordend's own copy
  (`nordend/fonts/tabler-icons.ttf`, none yet) first, then Noctalia's
  (`noctalia/assets/fonts/noctalia-tabler.ttf`, the full set: 5 958 icons). Nothing is copied
  into the repository. Without a font Nordend runs without icons and says so in the log.
- Icons are looked up by glyph name in the font (`FT_Get_Name_Index`: the font keeps Tabler's
  names), so no codepoint table and no JSON parser.
- `FontSystem::add_font_file` (outside the fontconfig fallback chain: text never picks it),
  `glyph_by_name`, `glyph_layout` (one glyph with its bounding box centered in a size x size box:
  Tabler glyphs sit off-center in their em square). `ui::IconSet` finds the font, caches glyphs,
  draws by name; an unknown name draws "file".
- `fs::icon_name(db, entry)`, in order: kind (folder, folder-symlink, link-off for a dangling
  link, plug for devices/sockets/fifos); darktable `.xmp` sidecars (adjustments-horizontal:
  shared-mime-info has no type for them); types worth their own icon, matched with `is_a` so
  subtypes count (image/x-dcraw -> aperture for every camera RAW format, PDF, SVG, Markdown,
  SQLite, desktop files, source code and config formats -> file-code); then the freedesktop
  generic icon (`MimeDatabase::generic_icon`: the generic-icons file, else
  `<media>-x-generic`) mapped to Tabler (archives, office documents, spreadsheets,
  presentations, executables, fonts, disc images, images, video, audio, text). An empty file
  gets the icon of its name's type (its MIME type stays x-zerosize, as in GLib).
- `nordend --icons DIR`: the window shows DIR's entries in a grid, icon and name, live (the 3.3
  model), with entries the name does not type sniffed on a worker and redrawn. Not a view: no
  scrolling or selection (phase 4). `nordend-snapshot --icons DIR` draws the same offscreen.
- Fixed in `nordend-snapshot`: with a colors file (6th argument) the width, height and scale
  were ignored.
- 3 new test cases (63 in total): kinds, 21 file types on the system database, every icon name
  in use exists in the font (skipped without the font). Release, ASan, TSan pass; tidy clean.
- Checked offscreen at scale 1 and 1.5, default and Nord palettes: a sample folder with 28
  types, and `DATA/fotografia/liguria` (ORF, ORF.xmp, a subfolder).

**Tested by the author**
- `nordend --icons` on the photo folder and on the repository, live changes and a theme switch:
  "everything perfect, consistent and beautiful". Step 3.4 closed; phase 3 complete.

**Open**
- Proposal for ROADMAP 7.0 (other desktops, no Noctalia): ship a subset of Tabler (MIT) with
  Nordend, only the icons in use, in `nordend/fonts/tabler-icons.ttf`, listed in
  `THIRD_PARTY.md`. Needs the author's OK (copying third-party material).
- Special folders (home, Downloads, Pictures from xdg-user-dirs) could get their own icons in the
  views.

---

## 2026-10-26 — inotify watcher and directory model (ROADMAP 3.3)

**Done**
- `fs::DirectoryWatcher`: one inotify instance for every watched directory (the kernel caps
  instances per user, about 128, and Miller columns open several). Watch descriptors are shared
  when one directory is watched twice; events are delivered per directory, one batch per read,
  on the loop thread: created, deleted, moved_from/moved_to with the cookie, changed
  (close_write, attrib), directory_gone (delete_self, move_self, unmount), overflow.
- `fs::DirectoryModel`: what a view will show. `open()` watches first, then loads in chunks
  (3.1), so nothing that happens during the load is lost; changes seen meanwhile are re-checked
  when the load ends. Changed names are gathered for 30 ms, stat'ed together on a worker
  (`fs::read_entries`) and applied in one batch (`Listing::replace`: one erase pass, one merge):
  no I/O on the loop thread, and a burst is a few updates. Notifications (`ModelChange`) carry
  names, not indices: added, removed, updated, renamed (old, new) when moved_from/moved_to pair
  up, plus reset (open, error, reload) and loaded.
- Directory deleted or moved: the model empties with an error (ENOENT). Queue overflow: full
  reload. inotify watch limit reached (ENOSPC): the directory is still shown, without live
  updates, with a warning.
- `nordend-watch DIR` (not installed): loads a directory and prints changes live.
- 7 new test cases (60 in total): load and order, create / write / delete / mkdir, rename
  inside, moves out and in, a burst of 500 files, changes during the load (no loss, no
  duplicates), directory deleted, a directory that cannot be opened, two models sharing a watch.
  Ten Release runs without a failure (timing-based tests); ASan and TSan pass; tidy clean.

**Measured**
- 500 files created back to back: one notification. 200 files created by a shell loop
  (about 150 ms): five updates of 17-49 changes, one every 30 ms. 200 files removed with one
  `rm`: one update.

**Tested by the author** (`nordend-watch` on a test folder in `~`, steps one second apart)
- create -> `+`, rename -> `old -> new`, append -> `~`, mkdir -> `+`; 1000 files created by a
  fish loop: 32 updates of about 32 files, one every 30 ms, ending at 1002 entries; `rm file*`:
  one update of 1000 removals; moved out -> `-`; folder deleted -> error. Step 3.3 closed.
- A first attempt ran two watchers at once and pasted every command in one go: doubled,
  interleaved output, still correct. `nordend-watch` now lists names only for updates of up to
  10 changes and summarises bigger ones on one line.

---

## 2026-10-26 — MIME types (ROADMAP 3.2)

**Done**
- `fs::MimeDatabase`: our own reader of shared-mime-info's `globs2`, `magic` (binary format:
  indents, masks, word sizes, ranges), `aliases` and `subclasses`, from `$XDG_DATA_HOME/mime`
  and each `$XDG_DATA_DIRS/mime` (`__NOGLOBS__` / `__NOMAGIC__` honoured). No `mime.cache`, no
  library. Types are interned ids (`MimeId`); the database is immutable and shared by workers.
- Detection follows GLib's `g_content_type_guess` + xdgmime, rule for rule, so Nordend agrees
  with GTK apps and `gio`. Each rule below fixed a real disagreement found by comparing:
  - name first: a literal name wins; otherwise the longest matching suffix; full fnmatch patterns
    only without a suffix match; case-sensitive globs beat case-insensitive ones (`main.c` vs
    `*.C`); ties by weight, then pattern length, then file order (`*.json` -> application/json);
  - several types from the name, at any weight, mean the content decides (`*.py` with a
    `python3` shebang);
  - a magic match of priority >= 80 wins; a weaker one only breaks ties between name types,
    preferring the name type that is a subtype of it (`libEGL.so.1`: shared library, not
    executable);
  - text: no control byte but tab/LF/CR in the first 128 bytes, else GLib's looks_like_text;
  - empty regular files are `application/x-zerosize` whatever their name;
  - security: content that looks like a `.desktop` launcher in a file not named `*.desktop` is
    `text/plain`.
- The loader fills `Entry::mime` from kind and name only (no file opened); entries the name does
  not decide stay unknown and `detect()` reads their head (up to the magic extent, 18.7 KB here,
  capped at 64 KB) when a view needs them.
- `nordend-mime PATH...` (not installed) prints the detected types; `nordend-bench-dir` gained a
  "mime" column (types from names) and types entries in the async path.
- Suffix lookup made allocation-free (transparent hash) and limited to positions whose byte can
  start a suffix pattern: 5.9 -> 1.7 ms for 10k names.
- 8 new test cases (53 in total) on a synthetic two-directory database: globs, case, weights,
  `__NOGLOBS__`, aliases, subclasses, magic (children, mask, word size, range), detection order,
  loader types. Release, ASan, TSan pass; tidy clean.

**Measured**
- Agreement with `gio info -a standard::content-type`: **0 differences on about 4 750 files**
  (`/usr/bin`, `/usr/lib`, `/usr/lib/python3.14`, `/etc`, `/usr/include`, xkb symbols, fonts, a
  photo folder, and a sample of 387 files with 261 different extensions from `DATA`).
- Database load: 1.0 ms (1 231 globs, 507 magic sections).
- Types from names: 1.7 ms for 10k entries (warm, median of 20); 66 % of the generated folder is
  decided by name (the `.ORF.xmp` sidecars have no glob in shared-mime-info).
- By content, warm: 163 `.xmp` sidecars 7 ms; `/usr/bin` (1 475 files, nearly all without
  extension) 52 ms. This is why content detection is left to the views, for visible entries, on
  workers.

**Tested by the author**
- Tests green; `DATA/fotografia/liguria` (123 entries: ORF, ORF.xmp sidecars, a subfolder)
  compared file by file with `gio`: no difference. Step 3.2 closed.
- DESIGN updated with the author's OK: own parser of globs2 and magic, GLib's detection order.

---

## 2026-10-26 — Async directory loader (ROADMAP 3.1)

**Done**
- ROADMAP 2.6 (template PR to Noctalia) postponed to after 7.3: a builtin template makes sense for
  an installable app. The template keeps working from `void-noctalia`.
- `WorkerPool::submit_streaming`: work gets an `emit(value)`; values reach the loop thread in
  order, before `done`, with the same cancel / pool-gone rules. Both submit paths share `deliver`.
- New module `fs` (`nordend::fs`):
  - `Entry`: name (bytes as on disk), kind, `is_link`, size, mtime in ns, `hidden()`. A link takes
    its target's kind; a dangling link is `other`; an entry that vanishes during the read is skipped.
  - `read_directory`: `opendir`/`readdir` + `fstatat` per entry (a second one for links), chunks
    of 128 doubling up to 4096; cancellation checked every 64 entries and before every chunk.
  - `load_directory`: the same on a worker (priority visible), chunks and result on the loop thread.
  - Sorting: `natural_compare` (digit runs by value, ASCII case ignored, ties by bytes so the order
    is total), `SortOrder` (name / size / modified, descending, directories first and not reversed),
    `Listing`: each chunk sorted and merged (`inplace_merge`), `set_order` re-sorts.
    Known limit: no locale collation, accented letters sort after z.
- `nordend-bench-dir` (not installed): `--make N DIR` creates a camera-like folder (ORF, ORF.xmp
  sidecars, JPG, subfolders); a run measures readdir alone, readdir + stat, sort by name, and the
  async path to the first merged chunk and to the complete sorted list. Run 1 apart (may be cold),
  min / median / max over the rest.
- 12 new test cases (45 in total). Release, ASan, TSan pass (bench too); tidy clean.

**Measured** (warm cache, Release, 8 workers, 20 runs, median ms)

| Directory | Entries | readdir | + stat | sort | 1st chunk | complete |
|-----------|--------:|--------:|-------:|-----:|----------:|---------:|
| generated, tmpfs | 10 000 | 2.08 | 15.44 | 3.61 | 0.52 | 16.93 |
| `/usr/lib`, btrfs on NVMe | 1 862 | 0.51 | 4.49 | 0.49 | 0.77 | 5.19 |
| `/usr/bin`, btrfs on NVMe | 1 493 | 0.40 | 2.89 | 0.31 | 0.79 | 3.26 |
| `DATA/fotografia/vero`, ext4 on SSD | 327 | 0.13 | 0.52 | 0.07 | 0.40 | 0.75 |

- DESIGN target "10k folder: first frame < 50 ms, full list in background": the first chunk is
  ready in about 0.5 ms and the whole sorted list in about 17 ms, warm. stat is most of the
  cost (about 1.3 us per entry, more for links: `/usr/lib`); the sort is about 3.6 ms for 10k.
- `/usr/bin` run 1: 19 ms for readdir + stat against 2.9 ms afterwards: a cold inode cache costs
  much more than the work itself.

**Tested by the author** (10k generated files on the SATA SSD, ext4)
- Warm, median of 20: readdir 3.69, + stat 18.01, sort 3.47, 1st chunk 0.80, complete 19.51 ms.
- Cold (after drop_caches), run 1: + stat 79.09 ms. The tool then read synchronously before the
  async path, so the async columns were already warm: the bench now runs the async path first.
  Rerun cold: **1st chunk 1.72 ms, complete 81.27 ms** (warm median 0.75 / 19.59 ms). Disk
  latency stays off the first frame.
- Natural order in `--list` checked ("Session 2" before "Session 10", IMG_2, IMG_5, ... IMG_11).
- Results collected in `docs/benchmarks.md`, summary in the README (author's request).
- Step 3.1 closed.

---

## 2026-10-26 — Noctalia theme (ROADMAP 2.5)

**Done**
- Noctalia template `nordend` (in `void-noctalia/config/noctalia/templates/nordend`, user
  template entry in its `config.toml`): `mode` plus a `[colors]` table written by a `for` loop over
  every Noctalia token (`name = "#rrggbb"`), output `~/.config/nordend/colors.toml`. Nordend reads
  the roles it knows and ignores the rest, so the template never has to change when Nordend uses
  a new role: good for the upstream PR (2.6). Checked with `noctalia theme <image> -r in:out`: 79
  lines, 48 Material tokens + hover + terminal tokens, parsed by Nordend with no problem.
- How Noctalia writes outputs (5.3.0, `template_engine.cpp`, `renderFile`): only when the content
  changed, in place (`std::ofstream`, truncate + write), no temp file + rename.
- `theme::Palette`: 33 roles in Noctalia's names, plus dark/light. Defaults = Noctalia's own
  built-in "Noctalia" palette, dark, expanded the way Noctalia does it (values dumped with a
  scratch build of its `fixed_palette.cpp`; listed in `THIRD_PARTY.md`). `parse_palette` /
  `load_palette`: missing roles keep the default and are listed in one line; bad values
  (`#rrggbb`/`#rrggbbaa` only) are reported where they are; extra tokens are ignored.
- `theme::Theme`: loads at startup, reloads on change. A missing file means the default palette;
  a file that does not parse keeps the palette in use (no flash of default colors).
- `core::FileWatcher`: inotify on the parent directory (created if missing), so a file that does
  not exist yet, rename-replacement and removal are seen. `IN_CLOSE_WRITE`, not `IN_MODIFY`
  (which fires after the truncation, before the content). Events are coalesced: one reload 50 ms
  after the last event. A queue overflow counts as a change.
- `core::read_text_file`, shared by config and palette loading.
- The window clears to `surface`; test scene, button and status lines use palette roles.
  `nordend-snapshot` takes an optional colors file (default palette otherwise, so snapshots stay
  reproducible); both the default and a generated palette checked offscreen.
- 10 new test cases (33 in total): palette parsing, reload rules, watcher (coalescing, other
  files, rename, removal then rewrite, missing directory). Release, ASan, TSan pass; tidy clean.

**Tested by the author**
- Tests green. Template deployed, `noctalia msg config-reload` + `templates-apply` write
  `~/.config/nordend/colors.toml` (Nord). Palette changes in Noctalia reach the open window at
  once (seven reloads in one session).
- Broken file at startup: warning with line/column, default palette. With the window open:
  `templates-apply` restores Nord; a broken file keeps the current palette; removal gives the
  default palette; `templates-apply` again brings Nord back. Step 2.5 closed.
- `void-noctalia`: template and config entry committed there.

---

## 2026-10-26 — Config (ROADMAP 2.4)

**Done**
- `core::load_config` / `parse_config` (toml++ 3.4, Void package `tomlplusplus-devel`, linked as
  its shared library and included only in `config.cpp`). Path: `$XDG_CONFIG_HOME/nordend/config.toml`,
  else `~/.config/nordend/config.toml` (a relative XDG value is ignored, as the spec says).
- Reading never fails: missing file = defaults; syntax error = the whole file ignored; unknown
  key or section, wrong type, empty string or out-of-range value = reported, that key keeps its
  default, the rest applies. Every problem is one line `file:line:column: what; using X`, sorted
  in file order (toml++ tables iterate by key, so problems are collected and sorted).
- First keys, only things Nordend already uses: `font.family` (fontconfig pattern, default
  `sans-serif`), `pointer.double_click_ms` (100-2000, default 400), `pointer.double_click_distance`
  (0-50 logical px, default 4). The Seat constants became `Seat::set_multi_click()`.
- Command line: `--config FILE`, `--check-config` (problems on stderr, verdict on stdout, status 1
  if any problem; no Wayland connection), unknown options exit with status 2 and a usage text.
  Problems are also logged as warnings at startup.
- `docs/config.example.toml`: every key with its default; a unit test checks it parses clean and
  matches the built-in defaults, so it cannot drift.
- 12 new test cases (23 in total). Release, ASan and TSan pass; tidy and format clean.
- GCC 14 `-Wnull-dereference` fires inside libstdc++ for `std::string(istreambuf_iterator...)`:
  the file is read with a sized `read()` instead.

**Tested by the author**
- Tests green; `--check-config` on a missing file (status 0), the example (ok, 0) and a file with a
  second `[pointer]` table (toml++: "cannot redefine existing table", whole file ignored, 1).
- In the window: `family = "serif"` picks DejaVu Serif, `double_click_ms = 1500` turns two slow
  clicks into a double click; no warnings in the log. Step 2.4 closed.

---

## 2026-10-26 — Event loop and worker pool (ROADMAP 2.3)

**Done**
- `core::EventLoop`: one `poll()` over fd sources, timers (timerfd, one-shot or repeating), and
  `post()` from any thread (mutex-guarded queue + eventfd). Sources can be added/removed from
  inside callbacks (removal is deferred to the next iteration; a source removed earlier in the
  same dispatch is skipped). `FdOptions::before_poll` / `notify_always` carry libwayland's
  prepare_read / read_events / cancel_read protocol, now inside `Display::attach()`.
  `FdOptions` lives at namespace scope: GCC rejects a nested struct with default member
  initializers as a default argument of the enclosing class.
- `core::WorkerPool`: fixed threads (default half the hardware threads, at least 2: 8 here),
  three FIFO queues by priority (visible, nearby, prefetch), `CancelToken`; results are posted to
  the loop and delivered only if neither the job was cancelled nor the pool destroyed. Destruction
  drops queued jobs and joins running ones.
- `main.cpp`: temporary loop removed. SIGINT/SIGTERM blocked before any thread exists (so no
  worker can receive them), signalfd is a loop source; Window gained a close handler; Seat
  `attach()`es its key repeat timer.
- First unit tests (doctest, Void package `doctest-devel`: `doctest` alone ships only the
  license): 11 cases, 5043 assertions. Loop: post order from another thread, timers, removal from a
  callback, sources added from a callback, before_poll/notify_always. Pool: threads of work and
  delivery, priority order, cancel while queued / while running, destruction with 100 queued jobs,
  2000 jobs across all threads delivered exactly once. A use-after-free in a test (detached thread
  touching a destroyed blocker) was caught in review before the first run.
- Results: Release and ASan pass; ThreadSanitizer 5 runs out of 5 with no warning.
- ROADMAP: new step 7.0, compositor compatibility (cursor fallback, client-side decorations,
  checklist on Umbriel, niri, KDE, GNOME), after the author asked about niri.

**Tested by the author**
- The app behaves as before on the new loop: keys, pointer, Ctrl+Q, close. Step 2.3 closed.

**Open**
- Run on the author's niri VM: postponed by the author, to do before ROADMAP 7.0.

---

## 2026-10-26 — Pointer (ROADMAP 2.2)

**Done**
- `wl_seat` bound at v8 for `axis_value120`. `Seat` gets a `wl_pointer` when the seat has one:
  enter/leave/motion/button delivered at once as `PointerEvent`s (logical surface coordinates,
  modifiers); scroll axes, axis source, axis_stop and value120 gathered until `wl_pointer.frame`
  and delivered as one scroll event (surface pixels + wheel notches + touchpad flag + stop flag
  for future kinetic scrolling).
- Click counting: same button, within 400 ms and 4 px (Wayland shares no double-click setting;
  configurable in 2.4).
- Cursor shapes through `cursor-shape-v1` (bound v1; Umbriel offers v2), set again on every
  enter as the protocol requires. Its XML references `zwp_tablet_tool_v2`, so tablet-v2 glue is
  generated too. The listener fills `axis_relative_direction` (v9) and `warp` (v11) with null:
  libwayland 1.26's headers know them, but they are never sent to a v8 client.
- Test scene: a button with hover color + hand cursor and a click counter, and a status line
  with the last press (button, click count, position) or scroll.
- Verified by Claude: Release/ASan builds clean, tidy clean, no-display path.

**Tested by the author**
- Hover color + hand cursor, left/middle/right presses, multi-click counting (slow clicks or a
  moved pointer restart at 1), wheel: one notch = 15 px and value120 = 1.00 per notch.
  Step 2.2 closed.
- Click counts are unbounded (x10 on rapid clicking): consumers map 1/2/3 to their actions.
- Twice two presses arrived 13 ms apart (179.772 -> 179.785, 180.624 -> 180.637): faster than a
  human double click. Likely switch chatter in the mouse, not Nordend; if it shows up as
  accidental double-clicks in the views, consider a small debounce (< 30 ms) on presses.

---

## 2026-10-26 — Keyboard (ROADMAP 2.1)

**Done**
- `Display` binds the first `wl_seat` (v7, Umbriel offers v9). `Seat` (ui) gets a `wl_keyboard`
  when the seat has one, compiles the compositor's keymap (fd mapped `MAP_PRIVATE`, as required
  since seat v7) with xkbcommon, tracks modifiers, and turns key presses into `KeyEvent`s:
  keysym, UTF-8 text, Ctrl/Shift/Alt/Super, repeat flag. Control characters are not text.
- Dead keys / compose via `xkb_compose` with the locale's table (LC_ALL, LC_CTYPE, LANG).
- Key repeat from `repeat_info` (rate, delay) through a `timerfd`, polled by the temporary loop;
  at most 2 repeats per wake-up so a stalled loop does not burst-type; stops on release and on
  focus loss.
- Test scene shows the last key; Ctrl+Q quits (first keyboard command).
- Verified by Claude: Release/ASan builds clean, tidy clean, no-display path.

**Tested by the author** (Umbriel, layout 'Italian', repeat 25/s after 600 ms)
- è é ò ç à ° ù § + * from the Italian keys, Ctrl+Shift+A without text, arrows/F-keys/Escape
  without text. Repeat measured in the log: first repeat 600 ms after the press, then every
  40 ms (25/s), stops on release. Step 2.1 closed.

---

## 2026-10-26 — Text (ROADMAP 1.5)

**Done**
- `FontSystem` (ui): fontconfig fallback chain for a family (default `sans-serif`, like Noctalia:
  DejaVu Sans here, 184 fallbacks), text split into runs by font coverage, harfbuzz shaping per
  run with the whole text as context, FreeType rasterization (light hinting, grayscale AA) at the
  physical pixel size. Marks, ZWJ, variation selectors, skin-tone modifiers and tags stay in the
  font of the previous character. Emoji-presentation characters (or followed by VS16) prefer a
  color font: fontconfig ranks a monochrome symbol font above Noto Color Emoji, which gave
  grey emoji and a tofu skin-tone modifier before the fix.
- Own UTF-8 decoder (U+FFFD per invalid byte, same policy as harfbuzz) for itemization.
- Truncation at whole clusters: `Ellipsis::end` and `Ellipsis::middle` (Finder-style, keeps the
  extension of a file name).
- Layout cache (8192 entries, cleared when full). Color emoji strikes (CBDT, 109 px) are
  downscaled on the CPU with an area filter.
- Renderer: second pipeline for glyphs (textured instanced quads, coverage R8 + color RGBA8
  atlases 2048x2048, shelf packing, 1 px padding; when an atlas is full, pending glyphs are drawn
  and the cache starts over). Box/glyph batches flush on switch to keep paint order.
- Test scene: sizes 11-32, accents (precomposed and decomposed), emoji with ZWJ family and skin
  tone, CJK (Noto Sans CJK installed by the author), invalid UTF-8, end/middle truncation.
- LeakSanitizer: fontconfig keeps 320 bytes of global state after the first FcFontSort, constant
  with 1 or 5 calls and freed only by FcFini (standalone repro, fontconfig 2.17.1). Suppressed via
  `__lsan_default_suppressions` compiled into sanitizer builds only; FcFini is not called because
  it would break any other fontconfig user in the process.
- Checked by Claude on snapshots at 1, 1.25 (ASan), 1.5 (ASan), 2 (ASan): all samples correct,
  no sanitizer report.
- DESIGN: text stack is harfbuzz + FreeType + fontconfig, no pango/cairo.

**Tested by the author**
- Release and ASan on screen: all samples correct; ASan exit 0, only the fontconfig suppression
  used. Startup to font + GL ready: 38 ms (debug log timestamps). Step 1.5 closed: phase 1 done.

**Open**
- Not done yet: bidirectional reordering (RTL), subpixel glyph positioning, font family from
  config (ROADMAP 2.4).

---

## 2026-10-26 — Drawing primitives (ROADMAP 1.4)

**Done**
- `Renderer` (ui): batched, instanced quads; one shader evaluates the signed distance to a
  rounded box with per-corner radii in physical pixels. 1-pixel antialiasing from coverage,
  inside borders from an inner offset, soft shadows from a Gaussian falloff of the distance
  (erf approximation; exact on straight edges, close at corners). Premultiplied alpha. Up to
  4096 instances per draw call; the buffer is orphaned each flush.
- Logical-pixel API (`Rect`, `Corners`, `Color`, `BoxStyle`); box edges and border widths snap to
  whole physical pixels (a 1 px border is never thinner than one physical pixel).
- Clip stack via scissor (axis-aligned; rounded clipping not needed yet).
- `Window` owns a `Renderer` and calls a draw handler each frame; the temporary
  `NORDEND_SCALE_TEST` pattern is gone.
- Test scene (`src/app/test_scene.cpp`): solid and fractional rects, radii, per-corner radii, pill,
  borders (1, 2, 4 px, border-only), shadows (sigma 4/12/24), translucent overlap, nested clips,
  physical-pixel hairlines.
- `nordend-snapshot`: renders the test scene offscreen through a surfaceless EGL context into an
  FBO and writes a PPM. Lets Claude check rendering without touching the author's screen.
- Checked by Claude on snapshots at scale 1, 1.25 (ASan), 1.5, 2 (ASan): shapes, AA, borders and
  hairlines crisp; no sanitizer report. Found and fixed a paint-order bug in the scene (a wide
  shadow painted over the neighbouring card).

**Tested by the author**
- Release and ASan on screen, with resizing: everything works, exit 0, no sanitizer report.
  Step 1.4 closed.
- Side finding: switching Noctalia to light does not recolor open foot windows. Not an override:
  Noctalia rewrites `~/.config/foot/themes/noctalia` (light palette, in `[colors-dark]`), but foot
  reads its config only at startup; its runtime switch (SIGUSR1/2) only toggles between the
  already-loaded `[colors-dark]`/`[colors-light]`. New foot windows get the new colors. Nordend
  must do better: live reload of `colors.toml` (ROADMAP 2.5).

---

## 2026-10-26 — Fractional scaling (ROADMAP 1.3)

**Done**
- Umbriel's globals listed with a scratch registry dump: it offers `wp_fractional_scale_manager_v1`,
  `wp_viewporter`, `wp_cursor_shape_manager_v1` v2, `zwp_text_input_manager_v3`, `wl_compositor`
  v5 (so no `preferred_buffer_scale`), among others. Author's monitor: DP-3, 2560x1440@59.95,
  scale 1.
- `Display` binds viewporter and fractional-scale (optional). `Window` keeps the scale as
  numerator/120, renders a physical-size buffer (logical x scale, rounded half away from zero)
  and sets the viewport destination to the logical size on every frame.
- Temporary `NORDEND_SCALE_TEST=1`: 1-physical-pixel line pattern to check that the buffer maps
  1:1 onto the screen. Removed when ROADMAP 1.4 brings a real test scene.

**Tested by the author** (Umbriel `[output.DP-3] scale = ...`, `NORDEND_SCALE_TEST=1`)
- 1.25: 2048x1152 logical -> 2560x1440 buffer fullscreen; 1.5: 837x878 -> 1256x1317 (1255.5 rounded
  half away from zero); 2.0: 624x638 -> 1248x1276. Lines crisp, no moiré. Step 1.3 closed.
- Interactive resize logs frames ~16.7 ms apart: one frame per refresh at 60 Hz, as designed.
- A shimmer of the 1-px line pattern while resizing from a left corner: the pattern is anchored to
  the moving left edge; an optical effect of the test pattern, re-check with real content in 1.4.
- Umbriel config (void-noctalia + deployed): `[appearance.title_bar] mode = "on"` for every window;
  the Nordend-only `title_bar = "forced"` rule removed as redundant.

**Open**
- No integer-scale fallback (`wl_output.scale` / `preferred_buffer_scale`) for compositors without
  fractional-scale: those render at scale 1. Add it if Nordend ever targets such a compositor.

---

## 2026-10-26 — EGL rendering and frame pacing (ROADMAP 1.2)

**Done**
- `EglContext`: platform display from the wl_display, opaque RGB888 config, OpenGL ES 3.0
  context (ES 3 for instancing and R8 textures later; DESIGN updated).
- `Window` renders with `wl_egl_window` + EGL surface; the shm buffer and the `wl_shm` binding
  are gone.
- Frame pacing: `eglSwapInterval(0)` so swaps never block the loop; at most one frame in flight,
  the next one waits for the frame callback; frames only when dirty (configure or
  `request_redraw`). Configure acks are applied by the next frame, so interactive resize runs at
  display rate.
- EGL attribute tables use `// clang-format off`: clang-format cannot lay out key/value tables.
- Verified by Claude: Release and ASan builds without warnings, tidy/format clean, no-display path.

**Tested by the author**
- "Everything works, no lag": interactive resize now smooth. Step 1.2 closed.
- GL: AMD Radeon RX 6650 XT (radeonsi, navi23, ACO), OpenGL ES 3.2, Mesa 26.2.5, EGL 1.5.
- Startup to GL context ready: 30 ms (from the debug log timestamps; not yet a proper benchmark).

**Open**
- A hidden window gets no frame callbacks, so a configure arriving while hidden is acked but
  committed only when it becomes visible again. Fine for now; revisit if Umbriel complains.

---

## 2026-10-26 — First Wayland window (ROADMAP 1.1, in progress)

**Done**
- Module layout as in quark++: `src/<module>/include/nordend/<module>/`, one static library per
  module. `core`: stderr logger, level from `$NORDEND_LOG`. `ui`: `Display` (connection, registry,
  globals bound at capped versions), `Window` (xdg-shell toplevel, server-side decoration request,
  configure/resize, close), temporary `ShmBuffer` for a solid background until EGL (1.2).
- wayland-scanner generates xdg-shell and xdg-decoration glue at build time.
- `WlPtr<T>`: RAII for proxies. The scanner's `*_destroy` helpers are `static inline`; used as
  template arguments they gave the owning types internal linkage (`-Wsubobject-linkage`), so the
  deleters are out-of-line overloads in `wl_ptr.cpp`.
- Temporary event loop in `main.cpp`: libwayland prepare_read/poll protocol + signalfd, so
  Ctrl+C/SIGTERM exit through every destructor.
- Verified by Claude: build without warnings, clang-tidy and clang-format clean, no-display error
  path (exit 1), SIGTERM path (exit 0).

**Mistake**
- A test meant to run without a display unset `WAYLAND_DISPLAY`; libwayland falls back to
  `wayland-0`, so a window opened on the author's screen (closed with SIGTERM). Rule added to
  CLAUDE.md: use `WAYLAND_DISPLAY=nordend-test-none`.

**Tested by the author on Umbriel**
- Window, server-side decorations, resize, close button: OK.
- No title bar by default: Umbriel is a scrolling tiler; `Mod+W` (`column-toggle-tabbed` in the
  author's Umbriel config) shows the tab with the "Nordend" title. Expected, not a bug.

- ASan/UBSan build (after installing `libsanitizer-devel`): maximize, fullscreen, interactive
  resize as a floating window with a forced title bar, close button: no sanitizer report, exit 0.
  Step 1.1 closed.
- Umbriel config (in void-noctalia): window rule `match.app_id = "^nordend$"`, `title_bar = "forced"`
  for testing; works.

**Measured: resize lag**
- The author saw lag while interactively resizing (Firefox does not lag). Cost of one
  `ShmBuffer` at 2560x1440 (memfd + ftruncate + mmap + fill + munmap, 50 runs, scratch benchmark):
  **14.1 ms Release, 41.5 ms ASan -O0**. A new buffer is made for every configure during a resize,
  with no frame pacing, so in the ASan build that is ~24 fps at best.
- Not fixing the shm path, which is temporary: ROADMAP 1.2 replaces it with EGL
  (`wl_egl_window_resize` + GPU clear) and frame callbacks. Re-measure resize there.

---

## 2026-10-26 — CMake skeleton (ROADMAP 0.1, 0.3)

**Done**
- DESIGN approved by the author.
- `CMakeLists.txt`: C++20, Release by default, `compile_commands.json`, `-Werror` with a strict
  warning set (conversion, sign, old-style casts, null deref, ...), `NORDEND_SANITIZER` and
  `NORDEND_CLANG_TIDY` options. wayland-client, wayland-egl, egl, glesv2 and xkbcommon found via
  pkg-config (not linked yet).
- `src/app/main.cpp`: exits 0, answers `--version`.
- Verified: Release build with no warnings, exit 0, `--version`, clang-format clean, clang-tidy clean.

**Open**
- Sanitizer build fails to link: `libasan` / `libubsan` runtimes not installed. Needed before
  ROADMAP 1.1 (package `libsanitizer-devel`, to be verified).

---

## 2026-10-26 — Project kickoff

**Done**
- Surveyed the installed stack of Noctalia (wayland-client, EGL/GLESv2, harfbuzz/pango, toml++,
  Luau, fzy, wuffs, libwebp, libjxl; MIT) and Umbriel (wlroots 0.20, Lua 5.4; MIT).
- Drafted `docs/DESIGN.md`; created `docs/ROADMAP.md`, `docs/buglog.md`, this devlog, `README.md`
  and `.gitignore`.
- Initialized git (`main`, no remote), MIT license, `.editorconfig`; copied the quark++ style
  (`.clang-format`, `.clang-tidy`, `.clangd`, `scripts/format.sh`) with includes regrouped for `nordend/`.
- Toolchain: clang-format / clang-tidy 21.1.8 installed (`clang-tools-extra`); `.clang-format` parses.
  The default Void mirror (Helsinki, `repo-default`) timed out on HTTPS while ping and port 80 worked:
  switched to `repo-de` via `/etc/xbps.d/00-repository-main.conf`.

**Decisions**
- C++20, CMake, stack aligned with Noctalia.
- Finder-style column view (Miller columns) + grid.
- Server-side decorations.
- `nordend` color template proposed upstream to Noctalia as a PR.
- Future scripting in Luau.
- External editor (helix) moved to the end: not a must-have.
- Languages: code, comments, commits, README, buglog and devlog in English; DESIGN and ROADMAP in Italian.
- Claude is credited in the README as copilot, not listed as a commit co-author. `CLAUDE.md` stays
  out of the repository.
- Nordend's code is written from scratch in C++; copying from Noctalia/Umbriel only case by case,
  always announced to the author first and tracked in `THIRD_PARTY.md`.
- GitHub: private `pankaspe/nordend-fm`; repo-local noreply email (account email privacy).

**Open**
- DESIGN approval (ROADMAP 0.1).
