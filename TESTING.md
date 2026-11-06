# Validation

CCurate 0.1.0, tested on 6 November 2026.

## Build and core behavior

The graphical/media build and the core-only build pass the 24 black-box
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

## Large catalogs and background work

`tests/stress.py` passes a synthetic one-million-entry catalog through paged
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

All six real-window suites pass under Xvfb with Mesa software Vulkan:

| Suite | Coverage |
| --- | --- |
| `gui_smoke.py` | Keyboard curation, scan/cancel, 50-400% scaling and session persistence |
| `gui_actions.py` | Live listings, mouse menus, contextual help, splitters, four-value layout input and persistence |
| `gui_activity.py` | Click-only selection, JPEG preview, log levels, resizing, ten themes and status legend |
| `gui_fonts.py` | Font chooser, keyboard/mouse controls, TTF/OTF/TTC faces, filtering, cancellation and persistence |
| `gui_navigation_open.py` | Parent focus and scroll restoration across pages; double-click, keyboard and menu application opening |
| `gui_scroll_sort.py` | Clickable sort headers, keyboard sorting, per-pane persistence, global list dragging across 2,205 rows, horizontal tables and preview/details/log scrollbars |

Install Xvfb, xdotool, Python Pillow and a working Vulkan ICD for these optional
checks. Each script manages its own disposable X server and fixture directories.

```sh
python3 tests/gui_smoke.py build/ccurate
python3 tests/gui_actions.py build/ccurate
python3 tests/gui_activity.py build/ccurate
python3 tests/gui_fonts.py build/ccurate
python3 tests/gui_navigation_open.py build/ccurate
python3 tests/gui_scroll_sort.py build/ccurate
```

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
