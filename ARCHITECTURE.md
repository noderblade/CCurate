# CCurate source layout

Implementation lives in separately compiled C translation units. Headers declare
interfaces and types; they have include guards and compile on their own. CCurate
does not include `.c` files or implementation fragments such as `.inc` files.
CMake lists each application source explicitly. Bundled Raven remains an
unmodified third-party dependency.

## Native interface

The public entry points are `gui_main(Catalog *, int)` and `gui_choose_roots`
in [include/gui.h](include/gui.h). They do not expose Raven or font-selection types
to the core. The GUI implementation and its private interfaces live in `src/gui/`.

| Implementation | Interface | Responsibility |
| --- | --- | --- |
| `gui.c` | `include/gui.h` | Application lifecycle, settings restoration, frame loop, shutdown and Raven callback adapters |
| `gui_startup.c` | `include/gui.h` | Pre-session root selection, graphical folder browsing and validation |
| `gui_controller.c` | `gui_controller.h` | Text commands, notices, prompts and saving view preferences |
| `gui_browser.c` | `gui_browser.h` | Paged file lists, selection, navigation and asynchronous view results |
| `gui_navigation.c` | `gui_navigation.h` | Bounded per-pane location bookmarks and path-based parent focus |
| `gui_open.c` | `gui_open.h` | Selected-file resolution, desktop-launch status and activity messages |
| `gui_input.c` | `gui_input.h` | Keyboard and pointer dispatch for the workspace |
| `gui_jobs.c` | `gui_jobs.h` | Scan/copy operation workers and preview requests, cancellation and results |
| `gui_layout.c` | `gui_layout.h` | Panel geometry, hit testing and split proportions |
| `gui_columns.c` | `gui_columns.h` | Shared file-cell formatting, draggable column boundaries, visible-row auto-fit and saved widths |
| `gui_scroll.c` | `gui_scroll.h` | Shared draggable scrollbar geometry, global file-list seeking and cached text viewport layout |
| `gui_render.c` | `gui_render.h` | Canvas rendering and shared text drawing helpers |
| `gui_menu.c` | `gui_menu.h` | Action definitions, footer menus and menu navigation |
| `gui_activity.c` | `gui_activity.h` | Activity log display, scrolling and severity choices |
| `gui_choices.c` | `gui_choices.h` | Log-level, color-scheme, sort-order and column-width popup controls |
| `gui_fonts.c` | `gui_fonts.h` | Installed-font chooser, asynchronous font results and font resource lifetime |
| `gui_theme.c` | `gui_theme.h` | Private palette data, theme application and the file-status legend |

`gui_types.h` forward-declares `Gui` and `GuiPane` for component APIs.
`gui_state.h` defines their private state. Module headers expose only the
operations used by other components; local implementation helpers stay `static`.
Palette and menu tables live in their owning `.c` files.

### State and thread ownership

When GUI arguments are incomplete, `main` runs `gui_choose_roots` before opening
a catalog. Its local `Startup` context owns widget callbacks and updates the
caller's allocated root strings. It uses Raven's built-in modal folder browser,
which receives keyboard events independently of the startup shortcuts. Cancelling
returns without opening a catalog. Acceptance canonicalizes and validates the
roots, then shuts down the chooser's Raven instance. `main` opens the catalog
and `gui_main` initializes the workspace normally. Catalog failures reopen the
chooser with the selected roots and the error. The chooser has no scan workers
or database state; explicit two-path and CLI invocations retain their direct flow.

`gui_main` owns one `Gui` context for the duration of the application. Components
receive that context explicitly. There is no exported mutable GUI singleton.
Raven callbacks receive the context through their user-data argument; the same
rule applies to operation and preview worker entry points.

Widget operations, rendering, navigation and UI settings belong to the main
thread. Filesystem work and decoding remain in workers. The preview request/result
channel uses its existing mutex, condition variable and cancellation atomic.
Operation completion is communicated through atomics, and the main thread joins
the finished worker before reusing its arguments. Shutdown joins workers before
releasing their context. Workers never call Raven.

Both the action menu and the keyboard route through the same command and input
interfaces. Paint modules consume the same live state and background query
results; they do not start synchronous scans or decode files while drawing.

## Core and services

| File | Responsibility |
| --- | --- |
| `src/main.c` | Command-line parsing and selection of GUI, CLI, preview-worker or desktop-launcher mode |
| `src/catalog.c` | Source inventory, versions, catalog queries and JSONL import/export |
| `src/operations.c` | Copy/reuse, destination moves, recovery, audit and reconciliation |
| `src/preview.c` | Format decoders and the isolated preview subprocess protocol |
| `src/preview_jpeg.c` | Direct libjpeg scanline thumbnails and bounded EXIF orientation parsing |
| `src/desktop_open.c` | Asynchronous GIO launcher subprocess, desktop-user privilege drop and result pipe |
| `src/view.c` | Independent view/statistics readers, saved preferences and storage metrics |
| `src/view_cache.c` / `include/view_cache.h` | Worker-owned source/history page cache with SQLite commit invalidation |
| `src/activity.c` | Bounded recent log, asynchronous file writer and log recovery |
| `src/font_catalog.c` | Fontconfig discovery and asynchronous font-file reads |
| `src/util.c` | Hashing, filesystem/path helpers and formatting |
| `include/curator.h` | Shared core types and service interfaces |
| `include/font_catalog.h` | Font service interface |

`CCURATE_GUI=OFF` excludes all GUI modules and the Fontconfig/GIO services.
`CCURATE_MEDIA=OFF` excludes image/PDF/audio/video decoder dependencies. Neither
configuration depends on private GUI headers.

### Navigation and external application boundaries

Navigation bookmarks hold relative paths rather than only row numbers. The view
worker resolves the selection against the same filters and ordering as its page
query. The common source/name-sort path counts preceding indexed entries without
joining all versions; filtered and other column sorts use the listing order.
No directory search is performed on the Raven thread. Bookmarks are bounded and
owned/freed with their pane.

The view worker publishes list rows before computing recursive directory totals.
It completes those counters only after foreground requests and interrupts them
when a newer selection/navigation request arrives. Completed counters are kept
with the cached page. The cache has at most 32 pages and 16 MiB of row/string
payload; keys include path, page, sort, search, history, unsecured and scan mode.
It checks `PRAGMA data_version` on its own connection before reuse and publication.
An anchor is reused only when its path exists on the saved page; otherwise the
normal ranking query resolves it.

`source_refresh.c` / `include/source_refresh.h` implement nonrecursive source
refresh. Existing-session startup uses an indexed existence check instead of
starting a tree scan. Navigation schedules one latest folder request; the GUI
shows catalog rows immediately. The operation worker owns all source-refresh
writes, just as it owns scans and copies. Manual operations cancel lazy work and
run next; the Raven thread never waits for a hash or joins an unfinished worker.
Cancellation is set before launching/superseding work, avoiding a worker-start
race. Copy preflight expands only the selected directory subtree.

The catalog planner compares observed metadata to the saved current version.
Unchanged entries require neither data reads nor main-catalog writes. Temporary
SQLite tables hold the current directory listing and changed-entry queue. New or
changed observations commit in batches; each verified entry commits separately.
Pending entries are excluded from live hash groups while accepted old versions
remain immutable. An interrupted hash rolls back only that entry's verification,
leaving its committed observation pending. Absence updates follow a successful,
stable enumeration and use parent-index traversal of removed catalog subtrees;
SQL cancellation rolls back an unfinished absence update. Full-scan completion
settings are never reset by a local refresh. Lazy visits do not regenerate JSONL
or restart whole-catalog statistics when no catalog content changed.

Global statistics have a separate read-only connection and worker. Navigation
serials do not cancel a whole-drive count; explicit statistics refresh and
shutdown do. Requests during scan/copy are deferred until operation completion,
avoiding a long statistics snapshot retaining the scanner's WAL. File-size and
free-space polling has its own worker and continues during either calculation.
The estimate uses a consistent read transaction, current eligible regular files,
and the indexed nonempty content table. Pending observations and errors cannot
reuse a stale retained hash to reduce estimated space. Empty files remain separate
metadata-bearing paths and never contribute duplicate savings.

Sorting uses whitelisted SQL expressions and a deterministic filename tie-breaker.
It runs before LIMIT/OFFSET in the view worker, including destination listings,
retained history and live unverified observations. The destination cache joins
object status for ordering. Each pane saves its own column, direction and date
choice. Scrollbar dragging translates a global row position into a requested
page; only that page's rows reach the UI. Paint and hit testing share geometry,
and text panes cache their line boundaries and measured width until text or font
changes. No scan, hash, decoder or directory query runs in scrollbar handlers.

The UI starts `--open-worker` with `posix_spawn`, a read-only root descriptor and
a private result pipe. The worker resolves a regular file beneath that root,
closes the source descriptor, drops root privileges when applicable, converts
the absolute filename to a URI, and waits for GIO's asynchronous launch result.
The frame loop polls the result without blocking. Only the launcher is timed out
or stopped on shutdown; a launched application is independent of CCurate.
External opening is an explicit action, separate from the resource-limited
preview worker. The external application retains normal filesystem permissions
and can modify the original file. No shell command is constructed from a filename.

## Validation

The application build enables `-Wmissing-prototypes` and `-Wstrict-prototypes`
alongside the existing warnings. Validation checks individual header inclusion,
full and core-only builds, existing integration tests and native GUI regressions.
See [TESTING.md](TESTING.md) for executed checks and environmental limits.

## Windows boundary

`gui_graphics.h` dispatches Windows initialization and shutdown to
`graphics_windows.c/.h`; Linux calls Raven directly. Windows first tries the
system Vulkan renderer, then loads bundled Lavapipe through direct-driver loading
and gives Raven an external device. CCurate releases that device after Raven
stops rendering. The fallback does not register a system ICD or modify Raven.

Linux is primary. `CMakeLists.txt` selects `cmake/Windows.cmake` only for WIN32,
then returns; the existing Linux target graph remains separate. Windows sources
live in `src/platform/windows/` with interfaces in `include/platform/windows/`.
`fs_windows.h` supplies a private descriptor adapter for the shared core; its
aliases never reach Linux or vendored Raven. Native Windows implementation files
disable those aliases with `CCURATE_WINDOWS_NATIVE`.

Shared catalog, copy verification, SQL/JSONL, paging and UI modules remain common.
Platform selections are guarded by `_WIN32`; Linux filesystem, preview and
desktop behavior is retained. Windows preview process and default-app launch
implementations are separate translation units, not replacements for Linux
services. [The Windows guide](docs/WINDOWS_PORT.md#source-separation) maps each
platform module and documents differences that cannot be emulated faithfully.

## Source read availability

`include/source_access.h` defines the shared writer-availability probe.
`src/platform/linux/source_access_linux.c` caches writable descriptor identities
from `/proc` and queries advisory locks; the cache belongs to the scanner's
Catalog and is reset between scans. `src/platform/windows/source_access_windows.c`
uses a native sharing probe. Neither helper writes source files.
The common hash loop bounds reads by the initial size and validates metadata
after each chunk. Deferred observations leave immutable retained versions intact.

Empty regular files use `empty_copies`, keyed by version, instead of a shared
`objects` hash mapping. Live/history queries, target state, audit, copy recovery,
destination moves and schema-2 export resolve this table alongside nonempty objects.

Windows bootstrap diagnostics live in `diagnostics_windows.c/.h` and are initialized
before runtime setup or Raven initialization. A non-inheritable stderr pipe copies
diagnostics to a per-launch log and the original console/redirected stream. CLI
stdout is preserved; restricted workers bypass this service. Windows-only GUI
hooks record initialization and the first frame. The independent packaged
PowerShell/CMD launcher diagnoses loader failures that occur before C entry.
