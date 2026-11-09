# CCurate user guide

Detailed controls, session behavior, preview limits and recovery procedures for
CCurate 0.1.0. For the problem it solves and the main workflow, start with
[README.md](README.md).

Linux remains the primary platform. The experimental native Windows package uses
`ccurate.exe`; see [the Windows guide](docs/WINDOWS_PORT.md) for platform-specific
installation, permissions, stream and preview limitations. Linux build commands
and process restrictions below apply to Linux unless explicitly stated.

The Linux executable is `ccurate`. Session state is stored in `.ccurate` on the
destination. The interface uses [Raven](https://github.com/oscar-soirey/Raven),
bundled with the source. All application text is English; user filenames and
metadata keep their original spelling.

## Inventory and retained history

**Inventory does not mean retention.** Scanning records every encountered path in
the session database. Copying, or linking an occurrence to an existing verified
copy, adds the content and its non-rejected occurrences to the retained archive.
F8 records an intentional rejection; Shift+F8 reverses it. Rejected entries are
excluded from the historical tree. The application does not delete source files.
Rejecting an already copied entry does not delete its physical destination copy.

For nonempty regular files, one SHA-512 content hash has one destination location. If `Photos/IMG.jpg` and
`Backup/copy.jpg` contain identical bytes, both historical paths resolve to the
same physical copy. Each occurrence retains its original name, permissions and
timestamps separately. You can reject one occurrence while retaining another.
The first encountered representative has an `O` marker; duplicates have `D`.
The initial traversal order is deterministic.

Empty (0-byte) regular files are independent entries, not duplicate content.
Copying one preserves only its own metadata and destination; other empty files
remain unhandled. A complete-directory copy still creates its empty marker files.
The occurrence panel explains this exception instead of listing unrelated empty
files. A later metadata version of the same path can reuse its own empty
location, but an unrelated empty destination is a name collision.

Existing CCurate sessions upgrade automatically to schema 2. Previously retained
empty-file paths keep their history and recorded locations, including locations
shared by older CCurate sessions. The upgrade does not recreate, delete or move
physical files. New empty files are never attached to those old mappings.
Use the updated application for upgraded sessions and schema-2 JSONL indexes;
older versions refuse the newer schema.

## Build

Requirements: Linux, a C11 compiler, CMake >= 3.20, pkg-config, and development
headers for:

- OpenSSL, SQLite3 with JSON support, libarchive and libmagic;
- X11, Vulkan, Fontconfig and GIO >= 2.50, plus a working Vulkan driver at runtime;
- libjpeg (including libjpeg-turbo), GdkPixbuf, Poppler GLib and FFmpeg libraries: avformat, avcodec, avutil, swscale.

Raven supports X11 and Wayland without requiring a GNOME or KDE desktop.
GdkPixbuf and Poppler GLib decode content; Raven owns the window and rendering.
ZFS needs no application-specific library: the program works on mounted
filesystems and does not run pool administration commands.

Example development packages on Debian/Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config libssl-dev libsqlite3-dev \
  libarchive-dev libmagic-dev libx11-dev libvulkan-dev libfontconfig1-dev \
  libjpeg-dev libglib2.0-dev libgdk-pixbuf-2.0-dev libpoppler-glib-dev libavformat-dev libavcodec-dev \
  libavutil-dev libswscale-dev
```

On Gentoo, install the corresponding libraries, development headers and required
GdkPixbuf loaders. The graphical build requires `media-libs/fontconfig`.
Available formats and codecs depend on the library build options.

From the project directory:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/ccurate /mnt/source /mnt/destination
```

Both directories must exist. The program creates `.ccurate` inside the chosen
destination; it does not format drives. Raven is included in `vendor/Raven`, so
the build does not download it.

To select the display backend explicitly:

```sh
RAVEN_WINDOW_SYSTEM=x11 ./build/ccurate /mnt/source /mnt/destination
RAVEN_WINDOW_SYSTEM=wayland ./build/ccurate /mnt/source /mnt/destination
```

Running as root is supported, for example through `sudo` with the environment and
display authorization required by your X11/Wayland session. Scanning and copying
have no `geteuid()==0` prohibition. The preview subprocess receives an already open
file and drops privileges to `SUDO_UID`/`SUDO_GID`, or UID/GID 65534. If the system
refuses the privilege drop, the preview reports that condition instead of running
the decoder as root. The core application remains usable.

## Choosing folders at startup

Run `./build/ccurate` without arguments to open **Choose your folders**. Click
**Browse source** to select the original directory and **Browse destination** to
select the directory for curated files and session state. On Linux, the built-in folder
browser shows hidden directories and does not require an external desktop
dialog helper. Browse into a directory and confirm with **Select folder**; you
can also select a directory row and confirm it. The selected paths are displayed
in read-only fields, with full-path tooltips.

Windows uses its native folder picker with hidden folders enabled, drive and
network browsing, and standard Windows keyboard controls. The startup shortcuts
`Alt+S`, `Alt+D`, `Ctrl+Enter` and `Esc` are shared.

Both mouse and keyboard work throughout:

| Key | Startup action |
| --- | --- |
| `Alt+S` / `Alt+D` | Browse source / destination |
| `Tab` / `Shift+Tab` | Move focus between controls |
| Arrows, `Home`, `End` | Navigate the folder list |
| `Enter` on a folder / `Backspace` | Enter the folder / go to its parent |
| `Shift+Tab` from the folder list, then `Enter` | Focus and activate Select folder |
| `Ctrl+Enter` | Start curation or open the archive after choosing roots |
| `Esc` | Cancel the folder browser, or exit from the startup window |

The startup window also accepts `F10` to exit. Cancelling before starting does
not create a session or modify either directory. Identical source/destination
paths and a destination that contains the source are rejected before session
creation. A destination inside the source remains supported: scanning excludes
that subtree. If opening the session fails, the chooser shows the error and
keeps the choices so you can correct them or cancel. An attempted session open
can create session files before a later initialization error.

| Invocation | Behavior |
| --- | --- |
| `ccurate` | Choose both directories |
| `ccurate /mnt/source` | Prefill source and choose destination |
| `ccurate /mnt/source /mnt/destination` | Open directly, as before |
| `ccurate --browse` | Choose a destination archive without requiring the source |
| `ccurate --browse /mnt/destination` | Open that archive directly |

Starting a new session scans the source in the background. Reopening an existing
session loads its saved catalog and checks only the displayed source folder.
Opening an archive with `--browse` uses retained history without a source check. CLI commands
still require explicit arguments; a core-only build cannot show a folder chooser
and reports an incomplete invocation instead.

## Status bar and scan progress

The top bar shows free destination space and session file sizes, sampled about
once per second:

- **Database:** the sum of `catalog.sqlite`, `catalog.sqlite-wal` and
  `catalog.sqlite-shm`.
- **JSONL:** the current `archive.jsonl`, representing the last completed export.
- **SQLite / WAL:** the main database file and its write-ahead log separately.

These are file lengths, not physical block consumption after ZFS compression.
The temporary file of an active export is excluded from JSONL size. Once the
export is published, the counter switches to the new file. SQLite can retain
freed pages for reuse, so the database need not shrink when the scan queue is
dropped. The top bar also shows **Unique** (logical data after deduplication),
**Saved** (duplicate data avoided), **To secure** (additional known content
without a verified destination copy), and secured/rejected counts. These use the
last calculated balance and refresh after an operation. `Unique*` means the scan
coverage is incomplete. **Ctrl+B** or **Help → Statistics / balance** opens the
full report; the `stats` and `balance` commands are equivalent.

Scanning has three visible phases:

1. **ENUMERATING:** walk the tree and count entries and bytes requiring SHA-512.
   The total scope is still unknown, so no percentage or ETA is claimed yet.
   Names, size, permissions and dates appear during this phase, including on a
   first scan. The source pane refreshes about once per second as committed
   batches arrive. `PENDING` means the observed file still needs verification; it
   is not counted as a secured copy. Old retained versions remain in history.
2. **SHA-512 / metadata:** show percentage, completed/total entries, read/planned
   bytes, throughput, approximate ETA, current path and errors. Progress advances
   within a large file. Unchanged files reuse their recorded hashes; a full scan
   forces a fresh read.
3. **FINALIZING:** publish the retained archive index. Hash-phase ETA excludes
   this export. A live source can change file sizes after the plan was created.

Source reads, directory/catalog queries, details, balance, file-size sampling and
preview opening run outside the GUI thread. Obsolete view queries are interrupted
when navigation changes. The scan plan is a disposable table on the destination,
not an in-memory array of the entire drive. Directory traversal still sorts names
in memory for the current directory and retains unvisited siblings of its
ancestors, so memory use depends on the shape of the source tree.

File lists use pages of 1000 entries. Ctrl+PgUp/PgDn changes pages; Home/End goes
to the beginning/end of the full list. The vertical scrollbar spans the whole
directory and loads the required page in the background. Marked selections survive
page changes within the same directory. Filtering and sorting apply to the whole directory.
Occurrence details use pages of 256 records, switched with Ctrl+Alt+PgUp/PgDn.
Recursive directory counters are calculated after list rows become available.
A `counting...` placeholder does not prevent entering a folder or returning to its
parent. Recent source/history pages are cached (up to 32 pages / 16 MiB of row
payload); changes committed by scanning, copying, auditing and curation decisions
invalidate cached pages. The independent statistics worker can finish a whole-drive
count while you keep navigating. During a data-writing operation, statistics wait
until it finishes rather than holding a long-lived database snapshot.

## Mouse, actions and pane layout

The bottom bar exposes **Files, Scan, View, Selection, Preview, Layout and Help**.
Every existing F9 command has a menu entry, including incremental/full scan,
audit, reconciliation, JSONL export and balance. Hover an action to read its
explanation in the menu's help area. Shortcuts appear beside each action.

**F2** opens the same complete action menu from the keyboard: Up/Down selects,
Enter opens a category or executes an action, Left returns to the category list,
Right opens a category or advances to the next one, and Esc closes it. Home/End
and Page Up/Down navigate long menus; the mouse wheel scrolls them. On narrow
windows the footer offers an **Actions** button containing all categories.

Click a file row to select it, Ctrl+click to toggle multi-selection and double-click
a directory to open it. Hovering a file never changes its selection or preview.
The wheel scrolls the pane under the pointer. Dialogs
have clickable Confirm and Cancel buttons as well as Enter and Esc.

Drag the grip between destination/source, between preview/details, between the
upper/lower rows, or above the Activity log. The four proportions are independent
and saved in the session. Double-click a separator to reset that split.
**Ctrl+Alt+L** (or Layout > Set panel proportions) accepts four percentages, for
example `40 60 55 30`: destination width, preview width, upper-row height, log
height. All four values are required. Values range
from 15 to 85; minimum readable pane sizes also constrain small windows.
**Ctrl+Alt+0** resets all four splits. Resizing panes does not change the UI scale.

## Scrolling, sorting and column widths

Destination, source/history, text preview, details and Activity have independent
vertical and horizontal scrollbars. Drag a thumb or click its track to move one
viewport. A muted full-length thumb means the content fits. Images remain fitted
to the preview pane. Font lists and long menus also have draggable scrollbars.

The wheel scrolls the pane under the pointer without selecting a different file;
Shift+wheel scrolls horizontally. **Alt+Left/Right** pans the active file table.
After **F3** or **Ctrl+D**, Up/Down and Page Up/Down scroll preview/details,
Left/Right pans long lines, and Home/End jumps to the first/last visible text page.
Help, the status legend, occurrences and balance use the scrollable details pane.
Long text lines retain their line breaks and can be read by scrolling horizontally.

Click any file-table header to sort ascending; click the active header again to
reverse it. An arrow marks the active column and direction. **View > Sort by
column**, **Ctrl+S**, or F9 **`sort`** offers Name, Status / copies, Size, Modified,
Created and Mode, plus ascending/descending choices. **Ctrl+Shift+S**, View >
Reverse sort direction, or F9 **`reverse`** reverses the current order.
**Ctrl+F3** selects name ascending; **Ctrl+F6** selects size descending.

Both panes sort the full directory before paging. Directories remain first in
either direction; equal values use ascending filename order for stable results.
Size, timestamps, permission bits and duplicate counts use numeric ordering.
Status sorts by its displayed state label, then the numeric duplicate count.
Names use byte order, with case preserved. Unavailable birth times sort as zero.
Live listings include pending observed metadata; retained history uses retained
versions. New scan results can change an entry's position.

Sorting preserves the focused path when it still matches the current filter.
Column, direction and date-column choice are saved independently for each pane.
**Ctrl+T** switches that pane between Modified and Created; when sorting by date,
the ordering switches to the newly displayed date too. Horizontal scrolling
exposes every column even in narrow panes or beside long filenames.

Drag a divider at the right edge of a column header to resize that column.
Double-click the divider to fit the widest **currently visible** value or the
header, including the sort indicator. Off-screen rows and other list pages are
not measured; fitting does not query the catalog or read source files. Header
labels still sort when clicked. Each pane saves its own widths across navigation
and restarts; UI scaling scales those logical widths along with the interface.

**Layout > Set column width** (**Ctrl+Alt+C**, F9 **`columns`**) lets you choose a
column with the mouse or arrow keys, then enter a width from 32 to 32768 logical
pixels or **`auto`** to fit that column. **Layout > Fit columns to visible rows**
(**Ctrl+Alt+W**, F9 **`autofit`**) fits all five columns of the active pane.
**Layout > Reset column widths** (F9 **`resetcolumns`**) restores responsive
defaults for that pane. These actions also work in the archive history view.

## Activity log

Open **View > Activity log** or press **Ctrl+L**. The panel appears below preview
and details. Drag its upper separator to resize it. The **Level** dropdown selects
All, Info, Warnings, Errors or Debug; **Ctrl+Shift+L** opens the same dropdown from
the keyboard. Filtering changes only the panel, not the disk log.

The full append-only history is saved at:

```text
<destination>/.ccurate/activity.log
```

Each text record contains a UTC timestamp, level, event name, source/destination
path and message. Scan read errors include the affected path and OS error;
metadata limitations and failed previews are warnings. Scan phase summaries,
copy/reuse/move decisions, review/rejection actions, audits and session lifecycle
are recorded. Debug records include preview and settings activity. Control bytes
in paths/messages are escaped, so a filename cannot insert a fake log line.

**Ctrl+Shift+D** focuses the panel. Up/Down, Home/End and Page Up/Down scroll;
Left/Right pans long lines. The wheel scrolls vertically, Shift+wheel horizontally.
Manual vertical scrolling pauses following. **Follow: On/Off** or
**Ctrl+Shift+End** toggles following; End follows the newest entry. Hide closes the
panel without stopping file logging. Visibility, level and panel height survive
restart; following starts enabled.

Memory holds the latest **2000 records** and a bounded writer queue. Restart loads
up to the last **1 MiB** of the log in a worker. Older records remain in the file;
use `less` or `tail -f` for the full history. Successful scan entries are not logged
one by one, so a millions-of-files scan does not create millions of routine log
lines. Actual per-path errors and curation actions are retained. No new log table
is added to SQLite. This is a diagnostic history; the catalog/JSONL remains the
source of truth for retained data.

File IO runs in a dedicated worker. A full queue slows background producers,
not the renderer. Write failures are shown in the panel and top status bar.
Orderly exit drains the queue and synchronizes the file; a crash/power loss may
lose recent queued or unsynchronized messages. No automatic log rotation deletes
history. Scan failures are also available in catalog details.

## Monospace fonts

**Layout > Monospace font**, **Ctrl+Alt+F**, or the F9 command **`fonts`** opens
an installed-font chooser. It lists families and styles, with a sample of the
highlighted face. Select a row with the mouse or arrow keys and choose **Apply**;
**Cancel/Escape** leaves the current font unchanged. Double-click also applies.
Nothing needs to be entered by hand. Optional typing filters the list; Ctrl+U
clears the filter. Home/End and Page Up/Down navigate, Tab moves between the list
and buttons, Enter activates the focused control, and **Refresh/F5** rescans fonts.

The default is **System monospace (automatic)**, resolved using Fontconfig.
**Bundled Roboto Mono** is always available, even with no compatible installed
fonts. Font discovery and font-file reads use a background worker. The renderer
validates fixed ASCII character widths before applying a face. The chosen font
covers file panes, previews/details text, menus, logs, native dialogs, inputs,
buttons and tooltips. Existing UI scaling (50–400%) also scales the font.

The chooser includes scalable TrueType/OpenType fonts, compatible TTC collection
faces and styles reported as mono/dual-width by Fontconfig. Symbol-only faces,
bitmap/Type 1 fonts and named variable-font instances are excluded. A face the
renderer cannot load is reported in the chooser. Fonts with wider Unicode glyphs
may use two character cells; fallback glyph coverage depends on the installed face.

Selection is saved per destination/session. If a font file moves, the application
tries the same family and style; if it disappears, the UI falls back to system or
bundled monospace. Standard Fontconfig user and system directories are searched.
When run via `sudo`, the invoking user's `~/.local/share/fonts` and `~/.fonts`
are included as well. Font files are only read, never copied into the archive.

## Color schemes and file-status legend

**Layout > Color scheme** or **Ctrl+Alt+T** opens a mouse/keyboard chooser for
CCurate Dark, Tango Dark, Tango Light, Solarized Dark, Solarized Light, Nord,
Dracula, Gruvbox Dark, Catppuccin Mocha and Tokyo Night (eight dark, two light).
Tango Dark is the initial default. The chosen scheme is saved in the session and
applies to panes, menus, dialogs and logs. Palette shades are adapted for readable
surfaces; all file states also have textual labels.

**Help > File status legend** opens the legend directly. It is also included in
**F1** help; scroll the information pane to read all entries.

| Marker | Meaning |
|---|---|
| PENDING | Observed entry still awaiting scan verification |
| UNSCANNED | Writable, locked or changing file deferred; close its writer and scan again |
| UNCOPIED | No verified destination content secured yet |
| OK | SHA-512-verified destination content, or retained metadata for a non-file object |
| O xN / D xN | First representative / duplicate among N current content occurrences |
| OK O/D xN / -- O/D xN | Duplicate group with / without verified destination content |
| REJECTED | Intentionally excluded; the source file remains untouched |
| ERROR | Scan/read error; inspect file details and Activity |
| CHANGED | Destination audit found a mismatch or read failure |
| MISSING | An indexed destination copy could not be found/accessed |
| UNCHECKED | Destination mapping needs verification; run Audit |
| SESSION | Internal `.ccurate` directory on the destination |
| OK s/n / -- s/n | Retained/complete or incomplete directory; s of n eligible descendants secured |
| REJ s/n | Rejected directory |
| scanning... | Expensive completeness counts deferred during a background job |
| counting... | Secured/eligible descendant counts are being calculated from the catalog in the background; no files are rehashed and navigation remains available |
| r / * before name | Reviewed by you / selected for an operation |

Directory, secured, duplicate, error and rejected entries have semantic colors.
The selected-row highlight marks focus; it does not mean retained. A verified
status describes the last verification, not continuous monitoring. Reviewing a
file does not copy it, and rejecting it never deletes source data. Empty files never show O/D duplicate markers.

## UI scale

**Scale the entire interface from 50% to 400%.** Ctrl+, opens an exact percentage
field. Ctrl++ and Ctrl+- change scale by 12.5 percentage points; Ctrl+0 restores
100%. The top-bar scale button and F9 command `scale 200` also work. The setting
is saved in the session and multiplies the system monitor scale.

In narrow panes, scroll horizontally to reach dates and permissions; they also
remain available in details. Large scales need a sufficiently large window;
Ctrl+0 resets them.

## Workflow and keyboard

The first session starts a whole-tree scan. Let it finish so duplicate groups
cover the source. Subsequent launches use that baseline immediately, even if the
last scan was interrupted. Use Ctrl+R to finish an incomplete global inventory.

Entering a source folder starts a background check of its immediate children,
including hidden entries. It compares size, nanosecond mtime/ctime, device/inode
identity and mode against SQLite. Unchanged entries do not read file contents or
rewrite their catalog rows. New or changed entries appear as PENDING until
verification completes. Old hashes no longer contribute to live duplicate or
secured status for those observed changes; retained history remains available.
Deleted entries and their known descendants become absent only after a complete,
stable directory listing. Unreadable or interrupted listings do not infer deletion.

Navigation supersedes the previous folder check. Copying and other manual write
operations take priority over lazy work; Esc cancels it without losing committed
observations. F5 checks selected source content before copying. For a directory,
this explicitly includes its descendants so new files in unvisited folders are
not silently omitted. Other full scans, copies and audits still serialize writes.

Use **Scan → Refresh this folder** or **F9: refresh** to check a folder again without
leaving it. **Ctrl+R** checks the whole tree; **Ctrl+Shift+R** reads every file again.
There is no periodic or filesystem-event watcher. Unvisited paths and global
statistics remain a baseline, not a claim that the entire live source is current.
Metadata comparison cannot detect changes that preserve every compared field.
Lazy observations are durable in SQLite; the portable JSONL is regenerated by
curation operations, whole-tree scans or **F9: export**, not on every folder visit.

| Key | Action |
|---|---|
| F2 | Open all actions with mouse or keyboard help |
| Tab | Switch left/right pane |
| Arrows, PgUp/PgDn, Home/End | Navigate |
| Enter / Right | Open directory; on a file, focus preview |
| Backspace / Left | Parent directory, restoring the child selection and scroll |
| Ctrl+Enter / double-click | Open the selected regular file in its default desktop application |
| Insert / Space | Select multiple entries |
| `*` | Invert selection on the current page |
| F5 | Copy selected source entries into the current left-hand directory |
| Ctrl+F5 | Preserve a complete directory, including empty directories |
| F6 | Move/rename the selected destination entry and update its mappings |
| F7 | Create a destination directory |
| F8 / Shift+F8 | Reject / restore a source entry or subtree |
| Ctrl+P | Toggle the independent reviewed flag |
| Ctrl+F | Show unsecured entries only; hide rejected entries too |
| Ctrl+H | Switch source / retained historical tree |
| Ctrl+R / Ctrl+Shift+R | Incremental scan / rehash the entire source |
| Ctrl+A | SHA-512 audit of destination content |
| Ctrl+B | Deduplication statistics, balance, gaps, errors and free space |
| Ctrl+E | Audit and find relocated destination content |
| F3 | Focus preview; arrows/PgUp/PgDn scroll text |
| Ctrl+D | Focus details and occurrences; arrows scroll |
| `[` / `]` | Previous/next PDF page or video position in 10-second steps |
| Ctrl+X | Switch format preview / hex |
| Ctrl+T | Date column: modified / created, if available |
| Ctrl+V | In history: cycle retained versions of the selected path |
| `/` | Filter names in the current directory; an empty value clears it |
| Ctrl+G | Go to a directory relative to the pane root |
| Ctrl+S / Ctrl+Shift+S | Choose any sort column / reverse direction |
| Ctrl+Alt+C | Choose a column and set its width; enter `auto` to fit visible rows |
| Ctrl+Alt+W | Fit all columns in the active pane to visible rows |
| Ctrl+F3 / Ctrl+F6 | Sort by name ascending / size descending |
| Alt+Left / Alt+Right | Scroll the active file table horizontally |
| Shift+wheel | Scroll horizontally in the pane under the pointer |
| Ctrl++ / Ctrl+- / Ctrl+0 | Scale the entire UI / reset to 100% |
| Ctrl+, | Set an exact UI scale |
| Ctrl+L | Show/hide Activity log |
| Ctrl+Shift+L | Choose log level |
| Ctrl+Shift+D | Focus Activity log |
| Ctrl+Shift+End | Toggle following the newest log entries |
| Ctrl+Alt+T | Choose color scheme |
| Ctrl+Alt+F | Choose an installed monospace font |
| Ctrl+Alt+L | Set the four panel proportions |
| Ctrl+Alt+0 | Reset all panel splits |
| Ctrl+PgUp / Ctrl+PgDn | Previous / next file-list page |
| Ctrl+Alt+PgUp / Ctrl+Alt+PgDn | Previous / next occurrence page |
| F1 / F9 | Help / text command |
| Esc | Close dialog or cancel the job; completed copies remain |
| F10 | Save the session and exit |

F5 always copies from right to left. F6 and F7 operate on the destination.
UTF-8 filenames are displayed as supplied; non-UTF-8 names use readable escapes.

`OK` means verified content or retained metadata for an object without regular
file content. `PENDING` indicates observed metadata awaiting verification;
a folder refresh or GUI copy retries it. `O xN` / `D xN` shows a group of identical contents. `r` is the
independent reviewed flag. A directory's count covers its current, non-rejected
descendants. Details show destination location, occurrences, SHA-512, original
metadata and retained versions. For directories, retained destination locations
in the subtree are listed up to 2000 entries, with an explicit limit message.

## Returning to a directory

Both panes remember the selected path, page and scroll position of recent
folders during the current run. Backspace/Left and the `..` row focus the child
you just left. If ordering changes, the background view worker locates the path
on its current page. A direct jump into a child also returns to that child in the
parent even without an earlier bookmark. Filters and sort modes have separate
bookmarks; a missing or filtered-out child falls back to a valid visible row.
The cache is bounded to 128 locations per pane and does not grow SQLite.

## Opening an external application

Double-click a regular file, press **Ctrl+Enter**, select **Files > Open in default
application**, or enter **`open`** in F9. A directory still opens inside CCurate.
The active pane determines the file. In retained-history view, the application
opens the mapped destination content, including the currently selected retained
version. Filename spaces, punctuation and non-UTF-8 bytes are passed as data.

GIO uses your desktop's default MIME application. Launching runs in a separate
process so a slow desktop handler does not block navigation. Requests, successful
launches and failures appear in Activity and `activity.log`. A successful launch
means the desktop accepted the request, not that CCurate can verify the external
application's rendering. Missing handlers or inaccessible files produce an error.

**The external application opens the actual file and can modify it.** CCurate's
source copy/preview operations remain read-only, but that does not restrict an
external editor. Source view opens the original; destination and history views
open the curated copy. Symlinks and special objects remain metadata-only.

External applications are never launched as root. When CCurate runs as root, the
launcher uses the account identified by `SUDO_UID` or `PKEXEC_UID`, drops groups
and privileges, and uses that user's home, MIME settings and desktop bus. The
account must be able to read the file and reach its desktop session. A root
session without an identified desktop user receives an explicit error; scanning
and copying remain available. Opening does not change permissions or make a
private temporary copy of the file.

## Complete directories and one physical copy

Ctrl+F5 preserves the structure and metadata of the chosen directory, respecting
earlier rejections. It creates destination directories and copies missing unique
contents. **Completeness applies to the historical view.** If an identical file
already has a copy elsewhere on the destination, no second file or hardlink is
created. A physical project directory may therefore not be independently runnable.
This follows from storing one physical copy per content hash.

F6 updates the locations of all contents below a moved directory without changing
historical names. Cross-filesystem/dataset moves are refused if `renameat2`
returns `EXDEV`; an automatic copy/delete fallback is not implemented.

## Live source and session persistence

Each launch refreshes the tree. Unchanged files are identified by device, inode,
size, mode, mtime and ctime, with nanosecond timestamps. Changed files receive a
new version and SHA-512; earlier retained versions remain archived. A newly found
occurrence of verified nonempty content can be associated with its existing copy.
Ctrl+Shift+R requests verification of every file again. Busy files are still deferred.
There is no continuous filesystem watcher.

**UNSCANNED** entries remain in the live tree, Details and the unsecured filter.
They cannot be copied or counted as secured until a later successful scan. The
scanner continues with other files; reasons are recorded as warnings in Activity
and `.ccurate/activity.log`. Close the editing application and press **Ctrl+R**,
or restart CCurate, to retry. Retained older versions remain available in history.
A harmless read-only open handle is not a reason to skip a file.

Linux checks a cached snapshot of writable descriptors under `/proc` (refreshed
at most once every 30 seconds) and queries advisory write locks when a file is
opened. Windows probes handle sharing and defers sharing/locking conflicts.
Descriptor discovery is best effort: restricted process visibility, writers
opened after a snapshot, memory mappings and remote writers may evade it.
Every hash read is also limited to the initial file size and checks size/mtime/ctime
and identity after each 1 MiB chunk and at completion. A growing file cannot
make the hash loop chase a moving EOF. This does not impose a hard timeout on
kernel I/O stalled by a failing disk or network filesystem.

Copying checks source freshness, hashes while reading and verifies the destination
by reading it again. A change during copying causes an error and requires a retry
after refreshing the entry. CLI `copy` retains its explicit scan/refresh precondition.
History contains retained versions of individual paths, not an atomic snapshot
of the entire live filesystem at one instant.

`DESTINATION/.ccurate/catalog.sqlite` holds inventory, decisions, versions,
content mappings, per-version empty-file locations, operation history and view settings. The database uses WAL and
durable commits. Scanning commits batches of 128 entries. After interruption, the
program reconciles pending copy/move operations and continues from saved state.
The unstarted remainder of a multiple-selection operation is not a durable work
queue; select those entries again after a restart. Cancelled scan observations
remain visible as pending and are verified on a subsequent folder visit or scan. They never
replace or erase earlier retained content versions.

Do not remove `.ccurate`: it contains your working session.
Schema versions are checked; an application refuses a schema newer than it supports.
To copy a session, close the application and preserve the whole `.ccurate`
directory. Keep the source mounted at the same path between sessions.

### Recovery and database locks

After a forced exit or a killed process, reopen the same source and destination.
SQLite recovers committed WAL transactions when the catalog is opened. CCurate
records `scan.recovery` when the last scan was incomplete. It opens the saved
inventory without forcing another whole-tree traversal; Ctrl+R requests one.
Already committed inventory batches, decisions and retained copies
remain available. An unfinished transaction is rolled back; unchanged completed
files can reuse their hashes during an incremental scan.

Keep the complete `.ccurate` directory. Do not delete `catalog.sqlite`, its `-wal`
or `-shm` sidecars, or `session.lock` to clear a lock. The existence of
`session.lock` is normal; ownership is an operating-system lock released when
its owning handles close. WAL can contain committed work not yet checkpointed
into the main database file.

If reopening fails:

1. Close other CCurate instances using this destination. Check Task Manager on
   Windows or the process list on Linux for an instance that is still running.
   Close database viewers that may have an open write transaction too.
2. Reopen CCurate with the same directories. If a scan failed while the window
   stayed open, use **Scan > Scan source** / **Ctrl+R** after resolving the
   conflict. No catalog reset or import is required.
3. If the error persists, inspect `DESTINATION/.ccurate/activity.log`. Database
   errors include the primary/extended SQLite codes and failing operation, for
   example `SQLite busy [5/5] during BEGIN IMMEDIATE`. Startup database failures
   are logged here too, before the GUI session opens. Include the error and
   preceding log lines in a report.

`active session` means the destination's process lock is held. `SQLite busy` or
`SQLite locked` describes a database connection/statement conflict, which may
come from another process or another connection within the application. These
messages do not by themselves establish database corruption. Failed commits and
cancelled view queries release their unfinished transactions before the next
request; a failed JSONL export leaves the previous published index intact.

## Portable index and browsing without the old drive

The portable format is **JSON Lines (`archive.jsonl`)**, one complete JSON object
per line. It can be streamed for large catalogs, is straightforward to parse and
avoids YAML type ambiguity. TOML is less convenient for millions of records.
SQLite handles session transactions; JSONL is the readable standalone export.

`DESTINATION/.ccurate/archive.jsonl` is atomically replaced after a completed
operation or batch. It contains retained contents, historical paths and metadata,
and placement/move events. It does not export the whole unreviewed inventory.
Placement history can still mention an earlier location of a subsequently rejected
entry; that entry does not appear in the retained tree. See [FORMAT.md](FORMAT.md).

```sh
./build/ccurate --browse /mnt/destination
```

The right pane shows the old structure while previews read destination data.
No FUSE is required. With only destination content and JSONL, rebuild an empty catalog:

```sh
./build/ccurate --cli - /mnt/destination import /path/to/archive.jsonl
./build/ccurate --cli - /mnt/destination audit
./build/ccurate --browse /mnt/destination
```

Import requires a catalog with no existing entries and never overwrites an existing
session. It restores the retained archive, not uncopied inventory or working review flags.

## Previews

| Content | Decoder and scope |
|---|---|
| JPEG | libjpeg, reduced-resolution scanline thumbnail, EXIF orientation, CMYK conversion |
| Other images | GdkPixbuf and installed loaders, scaling and basic metadata |
| PDF | Poppler, rendered pages, page count and metadata |
| Video | FFmpeg, single frame and metadata, 10-second position steps |
| Audio | FFmpeg stream information and tags |
| ZIP, TAR and other archives | libarchive member listing without extraction |
| Text | Beginning of the text; hex is also available |
| Other data | MIME via libmagic, hex and ASCII |

Video and audio are not played in real time. Office documents stored in ZIP
containers show their container contents; there is no LibreOffice renderer.
Unsupported or encrypted formats produce a message/hex view without stopping copying.

JPEGs are decoded directly through libjpeg instead of a full-resolution shared
pixel buffer in GdkPixbuf/Glycin. The decoder reduces IDCT resolution before
reading scanlines and stores only one scanline plus a thumbnail of at most
1000 x 800 pixels. EXIF rotations/mirrors are applied to that thumbnail.
Progressive/multi-scan JPEGs additionally need full coefficient arrays; an
estimated 512 MiB coefficient budget rejects oversized inputs with a specific
message. The existing time and total process-memory limits still apply.
This is a thumbnail renderer, not a color-managed photographic editor.

On Linux, preview decoding runs in a separate process with time and memory limits, with
IPv4/IPv6 socket creation and `connect` blocked. Local Unix sockets/socketpairs
and NETLINK_ROUTE remain available for Glycin/Bubblewrap sandbox setup. This is
not complete system isolation. Defaults:
1000 x 800 pixels after EXIF orientation, 3000 archive members, 8192 text bytes,
2048 hex bytes, 12 CPU seconds (14-second hard ceiling), an 18-second worker alarm,
a 20-second parent deadline, 1 GiB process address space, and 512 MiB per temporary
file/shared-memory object. The scratch-file limit must be nonzero: it also applies
to anonymous `memfd` buffers, and newer GdkPixbuf/Glycin loaders need writable
scratch storage even when decoding a small image. Source descriptors are opened
read-only; scratch writes are not writes to the source file. Images larger than
256 MiB are not fully decoded. Files are never executed by the preview worker. Preview failures report
specific decoder/worker diagnostics and generate warning records. Failures before
packet delivery identify libjpeg or the GdkPixbuf runtime version; decoder-returned errors
also retain bounded worker stderr. A resource-limit setup failure is reported
explicitly rather than silently ignored. Changing the
selection cancels the older preview without treating it as a decoder failure.

## All filenames and metadata

There is no hidden/system-name filter. Dotfiles, empty directories, symlinks,
FIFOs, sockets and device nodes are inventoried. Symlinks are not followed; their
textual target is retained. FIFOs, sockets and device nodes retain metadata,
not a copy of their data stream. They are not materialized on the destination;
this also applies to symlinks.

Original mode/UID/GID, timestamps, hardlink identifiers and available xattrs/ACLs
of regular files and directories are included in the index. Symlink/special-object
xattrs are not currently collected. The physical destination copy receives ordinary
permission bits and timestamps. Ownership, ACLs/xattrs and privileged mode bits
remain historical metadata rather than being imposed on the new filesystem.
Creation time may be unavailable; ctime is not shown as creation time.

Virtual kernel filesystems remain visible, but their apparent data files are not
hashed like documents. The condition is recorded in the catalog/balance. A
destination located inside the source is an explicit traversal boundary so the
application does not scan its own copies. Whole-tree scans infer absence only
after a successful traversal. Lazy refresh infers absence only within a fully
listed, stable directory and its removed catalog subtrees. Read errors or
cancellation do not mark an unseen remainder as deleted.

`OK` reflects the last verification, whose timestamp is available in details.
Ctrl+A rereads all destination contents. Balance reports include errors and
unavailable sources; intentional rejections are counted separately. Free space
comes from the filesystem.

The **Deduplication / space estimate** report covers current, present,
non-rejected regular files. It shows:

- Logical bytes before deduplication and the number of source file paths.
- Logical bytes after deduplication: one copy of each known nonempty content.
- Redundant copies, saved bytes and their percentage of successfully hashed bytes.
- Empty files counted separately, each requiring its own metadata/destination.
- Additional known data still needing a verified destination copy.
- Unscanned/changing files and their last observed size, excluded from the unique
  content total until successfully hashed.

An incomplete traversal or unscanned content makes the deduplicated total a known
lower bound, explicitly marked **KNOWN CONTENT ONLY**. It is not proof that all
source data fits. Previously retained historical versions, the database, JSONL,
filesystem blocks and metadata are extra. ZFS compression, sparse allocation and
reservations also affect physical space usage. A complete estimate reads the
catalog, not the source contents; on very large databases it can take time while
the interface remains usable.

## CLI and development

`./build/ccurate --help` lists scan, copy, audit, export/import, move and
marking commands. The empty relative path `''` denotes the root.

Build only the core, without GUI or image/PDF/video decoders:

```sh
cmake -S . -B build-core -DCCURATE_GUI=OFF -DCCURATE_MEDIA=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

Source organization: `catalog.c` handles full scanning, catalog and JSONL;
`source_refresh.c` handles lazy folder checks and copy preflight;
`operations.c` handles copying, moving and auditing; `preview.c` handles decoders
and the preview subprocess; `view.c` provides background queries and storage
metrics; `util.c` provides hashing, path and utility functions. Native UI code is
split into independently compiled `.c` modules and `.h` interfaces under
`src/gui/`, with `include/gui.h` as the public entry point. See
[ARCHITECTURE.md](ARCHITECTURE.md) for the module map and state ownership.
The pinned Raven revision is recorded in [THIRD_PARTY.md](THIRD_PARTY.md).

Application text, documentation and code comments are in English. Original user
filenames and metadata retain their spelling.
