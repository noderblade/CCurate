# Changelog

## Unreleased

- Show each source path once in content occurrences, including clearly labelled
  historical-only links. Page distinct paths instead of individual metadata records.
- Add expandable version changes to source/destination Details with buttons,
  Preview menu actions, Ctrl+Shift+V and Ctrl+Shift+PgUp/PgDn. Compare recorded
  SHA-512 content and metadata separately; display timestamp changes with nanoseconds
  and UTC offset. Reuse existing catalogs without a rescan or format migration.

- Use square corners across application-drawn panels, buttons, tabs, scrollbars,
  menus, dialogs and tooltips. Adapt build-directory copies of Raven geometry
  sources while preserving the vendored snapshot and licenses.
- Add the Disk Usage workspace with a catalog-backed source tree, logical-size
  rings/treemap, exact subtree unique/repeated totals, unverified coverage and
  duplicate occurrence details. Keep a disposable persistent analysis index,
  bounded navigation caches and saved view preferences; no new source scan is needed.
- Fix tiny ring sectors and seams between levels; use alternating branch shades
  instead of separator borders. Add native rendering checks across chart depths,
  themes and UI scales.
- Follow the focused file pane for preview and details, including destination
  metadata, recorded source occurrences and retained history.
- Consolidate documentation around current behavior, scan completion, unknown
  deduplication coverage and reproducible checks. Remove obsolete preview notes
  while preserving dated scale measurements in TESTING.md.

- Add Remove destination copy to Files, Ctrl+Delete, F9 and the CLI. Require
  confirmation plus fresh SHA-512 checks of the destination and a live source
  occurrence; refuse deletion of archived-only versions and modified content.
- Detach all affected content mappings without losing retained paths or review
  decisions; update unsecured status, balance, occurrence details and JSONL events.
  Keep empty-file copies independent. Journal and recover interrupted removals
  without continuing a deletion on startup.
- Cover removal/refusal/recopy/recovery on Linux and Windows, failed database
  commits, and the native GUI confirmation's default Cancel, menu and shortcuts.

- Close statistics-test SQLite connections explicitly before fixture cleanup,
  fixing Windows `WinError 32` failures in the full and core regression suites.

- Resize each file-browser column by dragging its header divider; double-click
  to fit the header and currently visible values without querying the catalog.
  Save widths separately per pane, with keyboard/menu sizing, fit and reset actions.
- Explain the `counting...` directory placeholder in the in-app status legend
  and user guide: background completeness counts do not rehash source files.

- Resume existing catalogs without a startup tree scan. Check source folders
  lazily on entry, reuse unchanged hashes without catalog rewrites, and publish
  changed entries as pending before verification. Keep manual tree/full-hash scans.
- Let navigation and manual operations preempt lazy hashing. Refresh selected
  directory subtrees before GUI copy so unvisited new files are included.
- Preserve retained history on change/cancellation, scope absence to stable
  listings, and label global totals as catalog observations. Add folder-refresh
  regressions and a native GUI resume/priority test.

- Publish source rows before recursive directory counters, cache recent source and
  history pages with commit-aware invalidation, and calculate global statistics
  independently of navigation. Preserve selection and sorting on return.
- Add explicit before/after-deduplication bytes, saved bytes, redundant copies,
  independent empty files, unscanned coverage and remaining data to Ctrl+B / Help
  → Statistics / balance. Show unique contents and savings on the top bar.
- These navigation/statistics changes reuse recorded hashes without requiring
  a database rebuild or full rehash. Add large-tree navigation and space-estimate
  regressions.

- Bundle Mesa Lavapipe for automatic CPU rendering on Windows when system Vulkan
  cannot initialize or present to a window. Add `CCurate-Software.cmd` and the
  `CCURATE_RENDERER=auto|software|vulkan` override.
- Keep software-driver loading and external-device ownership in separate Windows
  files, using Raven's public API without modifying its vendored code. No system
  ICD registration or administrator-only installation is required.
- Test automatic/forced software rendering, missing-driver diagnostics and
  repeated GUI initialization on native Windows and in the portable package.

- Release unfinished SQLite transactions after failed commits and cancelled view
  queries. Stop failed scan batches cleanly and allow retry in the same session.
- Start a new session's initial GUI scan before view requests so occurrence-cache
  rebuilding does not compete with startup scanning. Preserve the previous JSONL
  index when its database snapshot cannot be read or committed.
- Log incomplete-scan recovery and startup database failures, including SQLite
  error codes and the failing operation; distinguish session-lock access errors
  from a live second instance. Keep existing catalogs and WAL data in place.
- Add Linux/Windows hard-kill recovery, concurrent-instance, external-writer,
  failed-commit and interrupted-query regressions.

- Add persistent Windows startup logs, console diagnostics for normal GUI launches,
  `--debug`, startup error messages and a packaged `CCurate-Debug.cmd` launcher
  that records loader failures and process exit codes before application entry.
- Handle the Windows no-console `-2` standard handles safely; preserve redirected
  streams and restricted worker protocols. Add missing-driver/DLL, detached-launch,
  exception and launcher regressions to native Windows CI.

- Exclude zero-byte files from duplicate groups and retain their metadata and
  destinations independently, including audit, moves, recovery and JSONL export.
- Upgrade CCurate sessions and portable exports to schema 2 while preserving
  existing empty-file history and accepting schema-1 imports.
- Defer detected writable/locked/changing source files as UNSCANNED, continue
  scanning other entries and retry on the next scan without losing retained history.
- Bound hash/copy reads to the initial size and detect changes after each chunk.
  Isolate Linux process-descriptor checks and Windows sharing probes by platform.

- Browse for source and destination folders when GUI arguments are omitted,
  with mouse and keyboard controls, hidden directories and validation before
  session creation. A single source argument prefills the source; bare `--browse`
  selects an existing destination archive.
- Keep selected roots and show session-opening errors in the startup chooser.
- Add an experimental native Windows x64/UCRT64 port with separate platform
  sources and headers; preserve the primary Linux build and test paths.
- Add handle-relative Windows file operations, Unicode/long paths, native locking,
  metadata capture, restricted preview processes and native desktop integration.
- Refuse unsafe destination names, junction traversal and unsupported named streams.
- Add independent Linux/Windows GitHub Actions builds, native regression suites
  and a portable Windows runtime package with dependency checks.

## 0.1.0 - 2026-11-06

Initial CCurate release.

- Manual disk curation with a native C/Raven interface, keyboard and mouse controls.
- Background inventory and SHA-512 hashing with live listings and progress.
- Content deduplication, independently verified copies and retained source paths.
- Persistent `.ccurate` sessions and a portable JSON Lines historical index.
- Reviewed/rejected decisions, complete-directory retention, audit and balance.
- Destination reorganization with historical mappings kept current.
- Image, PDF, video-frame, audio-metadata, archive, text and hex previews.
- Reduced-resolution JPEG decoding, EXIF orientation and bounded preview workers.
- Parent-directory focus restoration and opening files in their default desktop apps.
- Resizable panels, configurable monospace fonts, ten themes and 50-400% UI scale.
- Draggable scrollbars in all content panes, with horizontal scrolling and global file-list seeking.
- Whole-directory sorting by any column, clickable headers, keyboard controls and saved per-pane preferences.
- Filterable Activity panel and persistent diagnostic log.
- README workspace screenshot and English documentation.
