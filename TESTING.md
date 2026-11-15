# Validation

Linux is the primary platform. This document describes reproducible checks and
their scope; a passing result belongs to a particular revision and environment.
Commands and source-file links refer to a source checkout. The Windows runtime
ZIP includes documentation but not the source or test drivers.

## CI and recorded results

The [Build and test workflow](.github/workflows/build.yml) builds full GUI/media
and core-only configurations on Ubuntu 24.04 and Windows Server 2022 / MSYS2
UCRT64. It runs behavior tests, native GUI checks and Windows package checks.
The [Windows publication workflow](.github/workflows/windows-download.yml)
publishes the rolling download only after both platform jobs succeed for `main`.

On 2026-11-15, [run 43649937030](https://github.com/noderblade/CCurate/actions/runs/43649937030)
was verified successful for commit `378f0773b7cfa4717f700c2e033b03a5c78cdd5e`,
including both Linux and Windows jobs. This is a dated result, not a claim about
later commits. Check [Actions](https://github.com/noderblade/CCurate/actions)
for the revision being installed. Per-suite logs and Windows screenshots are
retained as workflow artifacts according to GitHub's retention policy.

## Run the Linux checks

Install the build dependencies in [README.md](README.md#build-and-run).
Native GUI checks also need Xvfb, xdotool, Python Pillow/fontTools, DejaVu/URW
fonts and a working Vulkan ICD, such as Mesa Lavapipe. Use an unprivileged account
for permission-denied cases. Fixtures are disposable; never point scale tests
at a real curation session.

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --build build --parallel --target previewcheck jpegfixture preview_policy_probe workercheck sortcheck activitycheck hashcheck databasecheck navigationcheck refreshcheck inspectioncheck usagecheck identitycheck filesystemprobecheck

cmake -S . -B build-core -DCMAKE_BUILD_TYPE=Release -DCCURATE_GUI=OFF -DCCURATE_MEDIA=OFF
cmake --build build-core --parallel

python3 tests/run_linux.py build build-core
```

The explicit target list includes optional drivers that are not part of a normal
application build. [`tests/run_linux.py`](tests/run_linux.py)
is the executable list of primary suites and writes diagnostics to
`test-results/`. It runs CTest for both configurations, followed by stress,
preview, desktop-launch and native-window tests. GUI scripts manage their own
Xvfb servers and fixtures.

For a narrower check after building:

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build-core --output-on-failure
python3 tests/gui_usage.py build/ccurate
python3 tests/gui_usage_rings.py build/ccurate
```

CTest registration is defined in [CMakeLists.txt](CMakeLists.txt),
[cmake/Usage.cmake](cmake/Usage.cmake) and
[cmake/Windows.cmake](cmake/Windows.cmake). Test selection differs by platform and
build options; use `ctest --test-dir build -N` to list the configured cases.

## Core and service coverage

| Area | Fixtures | What they check |
| --- | --- | --- |
| Curation and portable history | `integration.py`, `scan_regressions.py` | Real copying and independent SHA-512 verification, duplicate reuse, independent empty files, decisions, retained ancestors/versions, moves, metadata, non-UTF-8 names, schema upgrades and offline JSONL reconstruction |
| Interrupted sessions | `database_recovery.py`, `database_driver.c` | Hard kills, WAL recovery, concurrent instances, external writers, failed commits and interrupted queries |
| Changing source data | `hash_driver.c`, `scan_regressions.py` | Growth, truncation, rewrites and EAGAIN after a read; bounded reads, writable-handle deferral, unchanged/read-only handles and preserved history |
| Lazy refresh | `lazy_refresh.py`, `refresh_driver.c` | Existing/incomplete session resume, unchanged visits without data reads or catalog rewrites, direct-child scope, new/changed/absent paths, failed enumeration, cancellation and selected-subtree discovery before copy |
| Navigation | `navigation_cache.py`, `navigation_driver.c` | First-page publication before recursive counts, parent anchors, bounded caches, invalidation, filter/sort separation and statistics independent of navigation |
| Space estimates | `statistics.py` | Unique/repeated/empty content, rejections, retained old versions, missing sources, pending/UNSCANNED entries, incomplete traversal and corrupt destinations |
| Persistent source identities | `source_identity.py`, `identity_driver.c`, `filesystem_probe_driver.c` | Remount/device changes without rehash or new revisions, real GUID replacement, unknown identity, rollback cache invalidation, schema-2 migration, schema-3 references and ZFS command/mount parsing |
| Legacy history cleanup | `device_history_cleanup.py` | Dry run, consistent backup, transaction rollback, exact merge criteria, retained/empty-copy references, non-UTF-8 paths, offline use, lock refusal, old schemas, vacuum and JSONL regeneration |
| Version presentation | `content_history.py`, `inspection_driver.c` | One row per source path across pages, real metadata/content changes, exact timestamps, unknown hashes, historical/pending paths, independent empty files, offline read-only history and bounded version pages |
| Destination inspection | `destination_inspection.py`, `inspection_driver.c` | Actual destination metadata, indexed source provenance, independent empty files, history and unindexed entries |
| Removal | `removal.py`, `removal_faults.py` | Confirmation, fresh source/destination verification, refusal of unsafe or archived-only removal, recopy, independent empty files, staging collisions, hard kills and failed commits |
| Disk Usage | `usage.py`, `usage_driver.c` | Subtree unions against an independent Python oracle, cross-branch duplicates, prefix-adjacent paths, unknown content, rejection scopes, offline use, bounded chart aggregation and unsafe cache files |

The changing-hash driver injects changes immediately after the first read through
a test-only link wrapper, avoiding timing races. Linux removal fault tests gate
real rename/unlink calls using a disposable preload helper; the application has
no test gates. Some Windows filesystem fixtures are conditional on host support,
such as symlink privileges or NTFS case-sensitive-directory configuration.

## Large catalogs, live listings and sorting

`stress.py` exercises two synthetic million-entry catalogs: repeated nonempty
content and independent empty files. It covers paging, filters, ordering,
selection anchors, cancellation, storage metrics and saved preferences, then
scans real small files and a sparse file to check unchanged-hash reuse.
`live_scan.py` checks committed enumeration batches, pending metadata,
cancellation/restart, retained old versions and audit. `sorting.py` checks all
sort columns in both directions across destination, source and history views,
including page boundaries, ties, directory grouping and pending observations.

```sh
python3 tests/stress.py build/ccurate build/workercheck
python3 tests/live_scan.py build/ccurate build/workercheck
python3 tests/sorting.py build/ccurate build/sortcheck
```

These tests validate behavior and bounded paging, not latency on an arbitrary
source drive. The larger optional measurements are documented below.

## Native Linux interface

| Script in `tests/` | Coverage |
| --- | --- |
| `gui_smoke.py` | Keyboard curation, scan/cancel, scaling and session persistence |
| `gui_actions.py` | Live listings, mouse menus, contextual help and splitters |
| `gui_activity.py` | Click-only selection, JPEG preview, log levels, themes and status legend |
| `gui_fonts.py` | Installed-font chooser, TTF/OTF/TTC faces, filtering and persistence |
| `gui_navigation_open.py` | Parent focus/scroll restoration and default-app opening |
| `gui_scroll_sort.py` | Sort headers, global list scrolling, horizontal tables and text scrollbars |
| `gui_columns.py` | Dividers, visible-row auto-fit, keyboard sizing/reset and saved geometry |
| `gui_startup.py` | Graphical root selection, cancellation, root validation, offline browsing and session-open errors |
| `gui_lazy_refresh.py` | Baseline startup, visited-folder updates, copy preemption and explicit full rehash |
| `gui_removal.py` | Default Cancel, confirmation, focus restrictions, menus, F9, recopy and high-scale scrolling |
| `gui_inspection.py` | Focused-pane preview bytes and details, including identical names with different source/destination contents; history toggle, keyboard paging and mouse collapse without changing preview context |
| `gui_usage.py` | Workspace isolation, tree/chart navigation, resizing, scope, columns and persisted state |
| `gui_usage_rings.py` | Rendered sector fills and joined rings at depths 1, 2, 3 and 5, different scales and dark/light themes |

The ring fixture represents 7.85 TiB logically, including one-byte leaves,
without allocating that payload. It checks for degenerate-triangle rectangles
and internal antialiasing seams. The live-listing GUI fixture pauses enumeration
after committed batches so cancellation does not depend on directory speed.

`desktop_open.py` uses isolated GIO associations pointing to a recording helper.
It checks exact filenames, shell punctuation, non-UTF-8 bytes, missing files,
symlinks, FIFOs and path escapes. It does not change the user's associations or
validate a particular production viewer/editor.

## Preview workers and optional diagnostics

```sh
python3 tests/preview_process.py build/previewcheck
python3 tests/preview_large_jpeg.py build/previewcheck build/jpegfixture
python3 tests/preview_limits.py build/previewcheck build/libpreview_policy_probe.so
python3 tests/desktop_open.py build/ccurate
```

The subprocess fixtures cover image formats, EXIF orientation, corrupt inputs,
cancellation, scratch/memfd writes, resource-limit failures and retained decoder
diagnostics. Large-JPEG cases include baseline 96 MP/200 MP images, progressive
96 MP, CMYK, rotation/mirroring and explicit refusal of an over-budget progressive
fixture. Source SHA-512 values must remain unchanged.

Optional scripts outside the primary runner include `preview_smoke.py`,
`preview_jpeg_safety.py` and `activity_test.py`. They exercise direct decoding,
malformed EXIF/truncated JPEGs and diagnostic logging. Consult each script's
arguments before running it. Instrumented direct-decoder tests run outside the
production preview worker's address-space limit.

A separate core build can enable AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S . -B build-asan -DCCURATE_GUI=OFF -DCCURATE_MEDIA=OFF -DCCURATE_SANITIZE=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

The removal preload fault cases are excluded from this configuration.
If process-inspection restrictions prevent LeakSanitizer from running,
`ASAN_OPTIONS=detect_leaks=0` disables leak detection; record that limitation
rather than reporting a leak-check pass. A normal CI pass does not imply a
sanitizer run.

## Native Windows and packaged-runtime checks

Use the [Windows build guide](docs/WINDOWS_PORT.md#build-from-source). The workflow
runs full/core CTest and native-window scripts, then repeats applicable integration,
preview, startup and GUI checks against the packaged application with only Windows
system directories on its `PATH`.

- `windows_integration.py` covers Unicode/emoji and long names, case collisions,
  hidden/system attributes, root/path validation, locking, metadata, stream/junction
  refusal, curation, recovery, audit and foreign POSIX historical names.
- `windows_previews.py` drives the real restricted subprocess with image, large
  JPEG, text/binary, archive, PDF, cancellation and worker-refusal fixtures.
- `windows_gui.py` exercises native root selection, curation, removal/recopy,
  history, help, Disk Usage and session persistence in real Win32 windows.
- `windows_software_rendering.py` checks automatic/forced CPU rendering, a visible
  first frame, the software launcher and missing-driver diagnostics. `graphicscheck`
  covers repeated device initialization/render/shutdown. No system ICD registration
  or software-driver directory on `PATH` is required.
- `windows_startup.py` checks redirected/detached launches, debug logs, exceptions,
  worker protocols and launcher diagnosis of missing DLLs. The deliberately broken
  fixture does not modify the built package; the diagnostic probe is not shipped.

This validates more than compilation, but the hosted Windows Server runner is
not a substitute for every consumer Windows installation or VM driver setup.

## Dated scale measurements

These are historical development measurements on synthetic catalogs, not fresh
results for every commit. Neither fixture creates or hashes 17 million physical
files. Use a separate scratch directory with ample capacity, approximately
1 KiB per cataloged file plus temporary files; results depend on hardware, cache
warmth, directory shape and duplicate distribution.

```sh
python3 tests/navigation_scale.py build/ccurate build/navigationcheck /path/to/navigation-scratch 17000000
python3 tests/usage_scale.py build/ccurate build/usagecheck /path/to/usage-scratch 17000000
```

**Navigation / statistics, 2026-11-09.** The fixture held 17,000,000 file records
under eight directories, each with a `deep` child. All files shared one nonempty
hash and a logical size of 4,096 bytes. Before deferred counters/page caching,
the first root listing exceeded the 180-second test wait. After the change,
initial root responses took 3.2–4.3 ms across two runs and parent returns
1.1–2.2 ms. The statistics pass took about 56.5 s and reported 69,632,000,000
logical bytes and 4,096 deduplicated bytes. These are service timings excluding
render scheduling. First-time sorting/filtering of millions of direct children
can still be expensive; these results do not make every query constant-time.

**Disk Usage, 2026-11-13.** The fixture held 17,000,000 occurrences in 17,000 leaf
directories, with 1,000 SHA-512 groups spanning all directories. Each file was
512 KiB logically. The catalog occupied 11.243 GiB; initial analysis took
554.996 s, its completed cache occupied 4.016 MiB and peak driver RSS was about
82.3 MiB. Separate subsequent driver launches returned leaf/root views in
20 ms / 61 ms on a warm filesystem cache; repeated requests in one process
were generally 1–2 ms. The small cache reflects this particular distribution.

## Validation limits

The local Linux development environment used Debian 13, GCC 14, X11/Xvfb and
Mesa software Vulkan; hosted Linux CI uses Ubuntu 24.04. The documented automated
checks do not cover a complete live 8 TB/ZFS drive, native Wayland interaction,
a root account's real desktop or every external viewer. The launcher's Linux
privilege-drop path has not been validated in a real root desktop session.
Fault injection does not replace power-loss testing on target storage hardware.

Windows filesystem and isolation differences are listed in
[docs/WINDOWS_PORT.md](docs/WINDOWS_PORT.md). The platform boundary and the
build-time Raven geometry adaptations are described in
[ARCHITECTURE.md](ARCHITECTURE.md). Passing either platform's suite does not
establish that the other platform or every filesystem is regression-free.

Identity tests use disposable catalogs. The Linux fixture provider simulates
persistent GUID changes without changing host mounts; a separate provider test
wraps mount discovery and subprocess launch to validate ZFS pool/dataset property
arguments and output handling. These are not tests on a live ZFS pool. The cleanup
suite does not touch an existing user database. Schema/JSONL/maintenance fixtures
also run in the native Windows CI configuration.
