# Validation

## Disk Usage workspace

`usagecheck` exercises the same asynchronous service used by the native UI.
`tests/usage.py` is registered with CTest on Linux and Windows. It compares
subtree unions with an independent Python oracle and covers cross-branch
duplicates, prefix-adjacent paths, unknown/changed content, missing versions,
rejections, zero-byte files, metadata-only entries, destination provenance,
offline operation, bounded chart aggregation and unsafe cache files.

`tests/gui_usage.py` runs under Xvfb and exercises tab isolation, non-selecting
hover, tree expansion, parent focus, rings/treemap, chart depth, resizing,
column choices, help, reveal and persisted session preferences. It is included
in the primary Linux regression runner.

The optional scale test creates only disposable catalog records and checks
logical and unique totals for a duplicate-heavy dataset:

```sh
python3 tests/usage_scale.py build/ccurate build/usagecheck /path/to/scratch 17000000
```

Use a scratch filesystem with ample capacity (roughly 1 KiB per file); no file
payloads are created. It reports initial and saved-index navigation times,
repeated in-process requests and cache size. Results depend on directory shape,
duplicate distribution, hardware and catalog fragmentation. This optional test
does not run on every CI build.

The development run on 2026-11-13 passed with **17,000,000 file occurrences** in
17,000 leaf directories and 1,000 SHA-512 groups spanning all directories. Each
synthetic file was 512 KiB logically; no corresponding payload was read or
created. The catalog occupied **11.243 GiB**. Initial analysis took **554.996 s**,
the finished analysis cache occupied **4.016 MiB**, and peak RSS of the headless
analysis driver was about **82.3 MiB**. Separate subsequent driver launches
returned the leaf/root views in **20 ms / 61 ms**; repeated requests in one
process were generally **1–2 ms**. These are backend timings on a warm filesystem
cache, not GUI frame timings or guarantees for other catalog shapes. The compact
cache size reflects this fixture's duplicate distribution.

The Linux full/core CTest suites and the 21-suite regression runner passed,
including native GUI tests. The analysis tests also passed AddressSanitizer and
UBSan; LeakSanitizer was disabled because the execution sandbox cannot support
its process inspection. GitHub Actions additionally validates native Windows.

CCurate 0.1.0, including destination-copy removal and the Windows port.
Linux remains the primary platform.

## Destination copy removal

The local Linux regression run passed all **19 suites**, including both full
GUI/media and core-only configurations (eight CTest cases each), large-catalog
stress, live scan, previews, desktop opening and ten native-window scripts.
The removal and database-recovery suites also pass with AddressSanitizer and
UndefinedBehaviorSanitizer. Leak detection was disabled because LeakSanitizer
cannot run under this environment's process tracing; this is not a leak test.

`tests/removal.py` contains 18 portable real-file cases: duplicate status and
balance, recopy, preserved decisions/history, required confirmation, live-source
and destination edits, archived-only versions, independent empty files, invalid
paths, busy writers, offline refusal, staged recovery, naming collisions,
post-unlink recovery and JSONL import/export recovery. Its symlink case runs on
Linux; native Windows skips it when optional symlink privileges are unavailable.

`tests/removal_faults.py` gates real rename/unlink calls in a disposable Linux
process. Seven scenarios exercise hard kills before/after unlink, cancellation,
source edits, a newly opened writer, staged destination edits and replacement of
the original destination name. The application contains no test gates. SQLite
authorizer tests additionally fail the intent, mapping and JSONL-export commits.

`tests/gui_removal.py` exercises source-focus refusal, default Cancel, keyboard
confirmation, mouse menu/tooltips/buttons, F9, F5 recopy, help and scrolling at
400% scale. Confirmation, cleared statuses and updated remaining bytes are
visually checked in a native Raven window.

The portable removal suite is registered for both Windows build configurations;
the Windows native-window smoke test also removes and recopies a fixture. The
new Windows checks run in GitHub Actions. The local results above do not imply
a native Windows result; check the workflow for the revision being installed.

## Column resizing and legend update

The full GUI/media and core-only Linux builds each pass all six CTest cases.
Five native-window scripts also pass: `gui_columns.py`, `gui_scroll_sort.py`,
`gui_actions.py`, `gui_activity.py` and `gui_fonts.py`. The new test covers all
five dividers in both panes, sorting after resizing, visible-only double-click
auto-fit, the last divider after horizontal scrolling, keyboard sizing, invalid
widths, empty folders, reset, UI scaling and restored geometry after restart.
The fitted table and updated directory-status legend were visually inspected.
The shared GUI module is included in both platform builds. GitHub Actions runs
the Linux regression set and the native Windows full/core, GUI and packaged
runtime tests for each pushed revision.

The statistics fixture closes SQLite connections explicitly as well as ending
their transactions. A connection context alone leaves its handle open, causing
Windows cleanup to fail with `WinError 32` in the two tests that modify the
fixture database. Assertions and cleanup remain enabled on both platforms.

## Lazy resume and large-catalog navigation (2026-11-09)

`navigation_cache` exercises bounded first-page work before descendant aggregation,
parent anchors, SQLite commit invalidation after decisions, distinct search/sort/
history/filter keys, eventual recursive counters and statistics completing during
repeated navigation. The fixture uses 12,000 descendants and a SQLite instruction
budget so the first-page check does not rely only on wall-clock timing.

`statistics` covers seven real-file scenarios: duplicate and independent empty
files, verified copies, rejection/restoration of individual occurrences and whole
groups, retained older versions, absent source paths, pending observations,
UNSCANNED paths, incomplete traversal, corrupt destinations and an empty plan.
Both full and core Linux builds include these CTest cases. The Windows CMake
selection also includes the statistics cases.

`tests/navigation_scale.py` additionally generated a disposable 17-million-file
catalog for before/after timing. Measurements and limitations are recorded in
[PREVIEW_NOTES.md](PREVIEW_NOTES.md). These are catalog records, not 17 million
physical files or a scan of the user's source drive.

The Linux regression set passed all 17 suites: both build configurations' CTests,
stress, live scan, sorting, production preview suites, desktop opening and all
eight native-window suites. The new lazy-refresh/navigation/statistics tests also passed with
AddressSanitizer and UndefinedBehaviorSanitizer. Leak detection was disabled for
that run because LeakSanitizer cannot operate under this execution environment's
process tracing; this is not a leak-check result. The Ctrl+B panel was additionally
opened and visually inspected in a native Raven window.


## Build and core behavior

The graphical/media build and the core-only build pass the 32 black-box
integration cases in `tests/integration.py`. Tests use disposable source and
destination directories and exercise real copying, SHA-512 verification,
deduplication, retained ancestors, rejection/restoration, source changes,
metadata-only entries, destination moves, recovery, non-UTF-8 filenames and
JSONL reconstruction without the source.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

cmake -S . -B build-core -DCCURATE_GUI=OFF -DCCURATE_MEDIA=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

Both configurations create and reopen `.ccurate` sessions. The core-only build
excludes Raven, Fontconfig, GIO and image/PDF/audio/video decoder dependencies.
Permission-denied cases require an unprivileged test process.

## Lazy session resume

`tests/lazy_refresh.py` and `refreshcheck` cover new versus existing/incomplete
catalog startup, unchanged visits with zero hashed bytes and no database-version
change, direct-child scope, same-size/mtime-preserving edits, hidden files,
subtree disappearance/reappearance, symlink replacement, failed listings,
cancellation during a large hash, retained history, pending-group invalidation,
writer deferral, independent empty files and complete-copy discovery of new
unvisited descendants. They are part of Linux full/core CTest.

`tests/gui_lazy_refresh.py` opens the native Raven window on a scanned fixture,
checks that startup does not traverse or hash unvisited folders, then verifies
updates when entering one folder. F5 must preempt a 64 GiB sparse-file hash and
copy a different selected file. An explicit full-hash scan must still work.
Reopening an incomplete inventory must also avoid an automatic tree scan.
`tests/run_linux.py` includes this suite alongside the existing GUI regressions.
These are disposable Linux fixtures; they do not substitute for native Windows
execution or a full live 8 TB drive test. This uncommitted preview has not run CI.

## Empty files and changing sources

`tests/scan_regressions.py` contributes eight cases to both platform integration
suites: independent empty copies, collisions and metadata versions, move/audit
and offline reconstruction, interrupted publication, schema-1 session and JSONL
upgrades, writable-handle deferral with retained history, and continuing past a
new busy file. Tests also confirm read-only handles do not block scanning, the
unsecured filter includes deferred files, retries work, and warnings reach disk.

The Linux `changing_hash` CTest runs `tests/hash_driver.c` with a test-only link
wrapper. It grows, truncates or rewrites a real fixture immediately after the
first read, or returns EAGAIN, and requires UNSCANNED after one read with no
published digest. An unchanged fixture must read exactly its initial size.
This avoids timing races and exercises changes after writer discovery.

## Large catalogs and background work

`tests/stress.py` passes two synthetic one-million-entry catalogs (identical
nonempty content and independent empty files) through paged
listing, duplicate counts, history/unsecured/name filters, size sorting,
selection anchors, query cancellation, storage metrics and saved preferences.
The same suite scans 2,000 real small files plus a 1 GiB sparse file and checks
that an unchanged rescan reuses hashes.

`tests/live_scan.py` passes live enumeration, pending metadata, cancellation,
copy refusal before hashing, restart, preservation of an earlier retained
version and destination audit.

```sh
cmake --build build --target workercheck
python3 tests/stress.py build/ccurate build/workercheck
python3 tests/live_scan.py build/ccurate build/workercheck
```

These fixtures test behavior and bounded paging; they are not measurements of
an 8 TB source drive or a guarantee of latency on another machine.

`tests/sorting.py` exercises the real view service against 2,105 files per pane.
All 36 combinations of six columns, two directions and three views (destination,
live source and retained history) pass, including ordering across page boundaries,
numeric values, duplicate/status groups, stable ties, directory grouping,
selection anchors, filters and pending metadata overlays.

```sh
cmake --build build --target sortcheck
python3 tests/sorting.py build/ccurate build/sortcheck
```

## Native interface

All seven real-window suites pass under Xvfb with Mesa software Vulkan. They
were rerun for the Windows port, together with both core integration
configurations, stress/live-scan/sorting and all production preview suites.

| Suite | Coverage |
| --- | --- |
| `gui_smoke.py` | Keyboard curation, scan/cancel, 50-400% scaling and session persistence |
| `gui_actions.py` | Live listings, mouse menus, contextual help, splitters, four-value layout input and persistence |
| `gui_activity.py` | Click-only selection, JPEG preview, log levels, resizing, ten themes and status legend |
| `gui_fonts.py` | Font chooser, keyboard/mouse controls, TTF/OTF/TTC faces, filtering, cancellation and persistence |
| `gui_navigation_open.py` | Parent focus and scroll restoration across pages; double-click, keyboard and menu application opening |
| `gui_scroll_sort.py` | Clickable sort headers, keyboard sorting, per-pane persistence, global list dragging across 2,205 rows, horizontal tables and preview/details/log scrollbars |
| `gui_columns.py` | All five dividers in both panes, viewport-only auto-fit, sorting after resize, horizontal scrolling, keyboard sizing/fit/reset, empty folders and persistence across scaling/restart |
| `gui_startup.py` | No-argument folder selection without typing paths, keyboard and mouse browsing, hidden roots, cancellation, equal-root rejection, session resume, offline browsing and session-open errors |

Install Xvfb, xdotool, Python Pillow/fontTools, DejaVu/URW fonts and a working
Vulkan ICD for these checks. Each script manages its own disposable X server and fixture directories.

```sh
python3 tests/gui_smoke.py build/ccurate
python3 tests/gui_actions.py build/ccurate
python3 tests/gui_activity.py build/ccurate
python3 tests/gui_fonts.py build/ccurate
python3 tests/gui_navigation_open.py build/ccurate
python3 tests/gui_scroll_sort.py build/ccurate
python3 tests/gui_columns.py build/ccurate
python3 tests/gui_startup.py build/ccurate
```

The live-listing/cancellation GUI test compiles `tests/enumeration_gate.c` as a
temporary preload fixture. It pauses source metadata enumeration after committed
inventory batches until the test cancels through the UI, avoiding assumptions
about directory speed. The production application and build are unchanged.


The startup suite checks that cancelling or rejecting equal roots creates no
session and that starting with chosen roots reaches the workspace and completes
the background scan. It also exercises Raven shutdown/reinitialization between
the chooser and workspace, including reopening the chooser after a lock error.

`tests/desktop_open.py` also passes against actual GIO default associations
pointing to an isolated recording application. It verifies exact filenames,
including quotes, shell punctuation and non-UTF-8 bytes, plus missing-file,
symlink, FIFO and path-escape failures. It does not change the user's desktop
associations or test a particular production editor.

## Preview workers

The real preview subprocess passes all 20 `preview_process.py` cases, including
EXIF orientations, several image formats, corrupt input and cancellation.
`preview_large_jpeg.py` passes baseline 96 MP and 200 MP images, progressive
96 MP, EXIF rotations/mirroring and CMYK colors. A progressive 192 MP fixture is
refused with the explicit coefficient-budget diagnostic. Input SHA-512 values
remain unchanged.

The four `preview_limits.py` cases pass scratch/memfd writes with read-only
source input, oversized scratch diagnostics, inherited hard-limit failure and
retained decoder error details.

```sh
cmake --build build --target previewcheck jpegfixture preview_policy_probe
python3 tests/preview_process.py build/previewcheck
python3 tests/preview_large_jpeg.py build/previewcheck build/jpegfixture
python3 tests/preview_limits.py build/previewcheck build/libpreview_policy_probe.so
python3 tests/desktop_open.py build/ccurate
```

Additional optional fixtures are provided by `preview_smoke.py` (direct media
formats), `preview_jpeg_safety.py` (malformed EXIF and truncated JPEGs, suitable
for an ASan/UBSan-instrumented `previewcheck`) and `activity_test.py` (diagnostic
logging). Instrumented direct-decoder tests run outside the production worker's
address-space limit. These additional suites are not included in CTest.

## Environment and limits

Validation used Debian 13, GCC 14, libjpeg-turbo 2.1.5, GdkPixbuf 2.42.12,
GIO 2.84.4, X11/Xvfb and Mesa software Vulkan. Native Wayland, a root account's
real desktop, a complete 8 TB/ZFS drive and real user-selected viewers were not
exercised here. The external-launch privilege-drop branch has not been tested
in a root desktop session. Error injection does not replace power-loss testing
on the target storage hardware.

The bundled Raven source is unchanged. Application implementation uses separately
compiled `.c` files and self-contained `.h` interfaces; no implementation
fragments are included.


## Linux compatibility gate for the Windows port

The baseline is commit `4babb053bb66063e97d27a05b4d6e89668b705ff`. After the
platform selections were added, full and core Linux executables were rebuilt
with the same compiler, configuration and dependencies as that baseline. Both
were **byte-for-byte identical**, with these SHA-256 values:

| Configuration | SHA-256 |
| --- | --- |
| GUI + media | `77f43402f74fbb2f825916d10fe6601f7f42f42afc16129286291eba1a9163a2` |
| Core only | `471f64bdff5ff169047ecb4e6254e4942040f34c6df5c40815cccebf733cba85` |

The local regression suites passed again with software Vulkan explicitly selected.
GitHub Actions also passed the complete Linux suite on Ubuntu 24.04. Initial
runner setup failures involved missing font fixtures and an older Pillow API;
those were test-environment issues, corrected without changing application
behavior. The comparison is specific to the recorded toolchain/build settings,
not a promise about every compiler, driver or filesystem. Vendored Raven is
unchanged. No Windows source file is compiled into the Linux targets.

## Native Windows checks

Native Windows validation passed on Windows Server 2022 with the MSYS2 UCRT64
toolchain in [Actions run 42322961160](https://github.com/noderblade/CCurate/actions/runs/42322961160),
commit `1664887b73229315d4f7561e261f728cc6f67dc1`. Both the full and core builds
passed all 18 integration cases, the full build passed all seven restricted
preview cases, and the real Win32/Vulkan GUI suite passed. The packaged runtime
then passed the same 18 integration cases, seven preview cases and GUI suite with
the development toolchain removed from the application's `PATH`. The NTFS
case-sensitive-directory and junction fixtures ran successfully on this host.

`tests/windows_integration.py` uses real disposable NTFS directories and the
native executable. Its platform-specific cases cover binary bytes and SHA-512, deduplication,
Unicode/emoji names, hidden/system attributes, long paths, case collisions,
traversal refusal, invalid root pairs, session locking, changed source versions,
review/reject/restore, complete directories, target moves, publication recovery,
JSONL rebuild, foreign POSIX historical names, metadata, named-stream refusal,
junctions and audit corruption.
Several related assertions share a test case. A case-sensitive-directory refusal
fixture runs when the host permits enabling that NTFS flag; otherwise it is
reported as skipped.

`tests/windows_previews.py` drives the real restricted subprocess through seven
cases: text/binary, PNG/JPEG, a 96 MP JPEG, ZIP members, PDF, cancellation and
refusal of an unrestricted worker. Fixture SHA-512 values must remain unchanged.
`tests/windows_gui.py` opens real Win32/Vulkan windows, exercises the native folder
picker and cancellation, then copies with F5, marks reviewed, opens help/history,
saves the session and audits the result. Screenshots and diagnostics are retained.

The packaging check runs the complete integration, restricted preview and GUI
scripts against the staged runtime. Core and preview checks have only Windows
system directories on `PATH`, including the GUI checks. The application-local
software Vulkan driver is loaded directly, without registry registration or a
driver directory on PATH. This is
stronger than link success, but still runs on the CI host rather than a fresh
consumer Windows installation. See [the Windows guide](docs/WINDOWS_PORT.md)
for unsupported filesystem features and preview isolation differences.

`tests/windows_software_rendering.py` verifies automatic fallback on a runner
without an installed ICD, explicit CPU mode, a visible/nonblank first frame,
the clickable software launcher, and missing-driver diagnostics with CLI
availability preserved. `graphicscheck` exercises four complete software-device
creation/render/shutdown cycles to cover the folder-chooser/workspace lifecycle.
The native and packaged tests retain screenshots and startup logs. Invalid
driver-path overrides are supplied deliberately; direct driver loading must
work independently of them, including on the elevated hosted runner.

## Windows startup diagnostics

`tests/windows_startup.py` exercises redirected console streams, ordinary and
`--debug` invocation, application errors in persistent logs, unchanged binary
worker protocols, a missing Vulkan driver, startup without standard handles,
a deliberately raised exception, and both successful and missing-DLL launcher
runs. The missing-DLL fixture contains only a copied executable and diagnostics
scripts with development directories removed from PATH; it never modifies the
built package. The native probe is a test executable and is not distributed.

Full and core Windows CTest runs include this suite; GUI-specific cases are
skipped in core builds. Packaging runs the applicable startup cases again against
the runtime with MSYS2 absent from PATH. Existing Windows GUI and preview suites
remain enabled. Linux does not compile or link the diagnostics implementation.
