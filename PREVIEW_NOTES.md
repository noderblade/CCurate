# Remove destination copy change notes

Base: `9f4df55a0f9ac9e9f15b942ca89b88f8176f0604`.
These notes describe the removal feature relative to the base commit.
Windows downloads are published only after the build and test workflow passes.
Linux remains primary. Build instructions below reuse the existing session.

## What changed

Select one indexed regular file in Destination and use **Files > Remove
destination copy**, **Ctrl+Delete**, or F9 **`remove`**. The confirmation defaults
to Cancel and supports mouse and keyboard. Its message scrolls at large UI scales
or with long paths while the buttons remain visible.

The worker verifies the destination SHA-512 and searches the recorded occurrences
for an identical, stable live source file. It refuses removal when the source is
missing, changed, open for writing or unavailable and no other matching occurrence
can be verified. It also refuses modified destination bytes, directories,
symlinks, unindexed files and session files. Source operations stay read-only.

Successful removal detaches the content mapping: all linked occurrences become
unsecured, the remaining-byte estimate updates, and a later F5 can place the
content elsewhere. Empty-file copies remain independent. Historical paths,
metadata, reviewed flags and rejection decisions stay in the catalog. JSONL and
Activity include a removal event; Details no longer labels accepted but unsecured
occurrences as `[OK]`.

Removal journals intent before staging beside the target, checks the staged file
again, deletes it, then commits the mapping change. An interrupted operation
restores staged bytes instead of resuming deletion. Recovery never overwrites a
new file at the old name; it retains both and records the recovery location.
Pending JSONL publication survives a crash after the SQLite commit.

The source is live, not frozen by a filesystem snapshot; verification cannot
prevent subsequent external edits. There is no recycle bin. Recovery uses the
retained source and F5. The operation targets one focused destination file at a
time, including an independent 0-byte file; it is not recursive deletion.

CLI: `ccurate --cli SOURCE DESTINATION remove TARGET_REL --confirm`.
See [USER_GUIDE.md](USER_GUIDE.md#remove-destination-copy) for details and
[TESTING.md](TESTING.md#destination-copy-removal) for validation and platform scope.

## Included column sizing and legend behavior

The status legend in F1 and Help now explains `counting...`: secured/eligible
descendant counts are calculated from the catalog in the background, without
rehashing files or preventing navigation.

Both file panes support dragging each header divider to resize a column.
Double-click a divider to fit its header and widest currently visible value.
Off-screen rows and other pages do not affect fitting. The same cell formatting
and font measurement are used for drawing and fitting, including name prefixes,
file sizes, dates, permissions and status markers. Widths are saved separately
for each pane and survive navigation, UI scaling and restarts.

Layout > Set column width / Ctrl+Alt+C opens a column chooser, then accepts a
width in logical pixels or `auto`. Ctrl+Alt+W / F9 `autofit` fits all columns in
the active pane; Layout > Reset column widths / F9 `resetcolumns` restores its
responsive defaults. Column dragging and fitting preserve sort and selection;
clicking the header label still sorts. Horizontal scrolling reaches columns
that do not fit inside the pane.

## Included baseline behavior

The following lazy-resume, navigation and statistics changes are already part of
the base commit. They remain available with this change.

Existing sessions now open their saved catalog without a whole-drive scan. A new
session still starts its initial scan automatically. Each source-folder visit
checks only that directory's immediate children in a worker. Metadata comparisons
reuse unchanged hashes and verify only new, changed or previously unscanned
entries. No-op visits do not rewrite catalog rows. Navigation replaces pending
folder work; manual operations preempt it. Copying a directory explicitly checks
its descendants first to include new files in unvisited folders.

Scan → Refresh this folder / F9: refresh checks the current folder. Ctrl+R still
checks the whole tree; Ctrl+Shift+R forces SHA-512 for every regular file. Global
statistics and unvisited paths reflect the saved baseline, not continuous live
monitoring. Pending observations cannot reuse an old live secured/duplicate status.
Retained historical versions are preserved. SQLite saves observations immediately;
JSONL is regenerated on curation operations, whole-tree scans or explicit export.

Opening a source directory previously waited for recursive counts for each
listed subdirectory. Returning to a parent could repeatedly visit millions of
descendants before displaying any rows.

The new view publishes rows first and calculates folder counters afterwards.
Until then, the counter reads `counting...`. Navigation cancels obsolete work.
Recent source/history pages are cached, including their selection anchor lookup;
the cache is limited to 32 pages and 16 MiB of row/string payload. A committed
catalog change invalidates the cache. Existing sort, filter, page and parent-focus
behavior is retained.

**Ctrl+B**, **Help → Statistics / balance**, and the `stats` / `balance` commands
show before/after-deduplication logical bytes, savings, redundant copies,
independent empty files and remaining content without a verified destination.
Unscanned files are explicit unknowns; their stale hashes are not used to reduce
the estimate. The top bar includes Unique, Saved and To secure. `Unique*` marks
incomplete scan coverage.

Global statistics run on their own read-only SQLite connection. Navigation does
not restart or wait for a whole-drive total, and storage/free-space polling
continues independently. Statistics requested during a scan/copy wait for it to
finish. Large-catalog statistics still need a pass over the catalog; this is not
an instant precomputed total.

## Build and reuse the existing session

Extract the complete source archive, including `vendor/Raven`, into a source-code
directory. From that directory, using the dependencies listed in README.md:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/ccurate /path/to/source /path/to/destination
```

Close the old CCurate process before opening the same destination with this build.
Use the same source/destination paths and keep the complete existing `.ccurate`
directory. There is no database schema migration, index rebuild or forced full
rehash. Startup no longer traverses the whole source when a session exists.
An interrupted previous inventory also opens without an automatic restart;
request Ctrl+R when you want to complete global discovery.

The estimate covers present, non-rejected regular files, once per known nonempty
content. It is a logical-byte estimate. ZFS compression, sparse allocation,
filesystem metadata, CCurate session files and retained older versions affect
actual destination usage. It cannot provide an exact physical-space prediction.

## Measurement scope

The preceding navigation/statistics preview's disposable synthetic fixture had
17,000,000 file records below eight
directories, each with a `deep` subdirectory. File records shared one nonempty
SHA-512 value. It is a stressful descendant-count case, not a reproduction of a
specific ZFS drive or an 8 TB content scan.

Before the change, the first root listing exceeded the test's 180-second wait.
With deferred counters and page caching, the initial root listing took about
3.2-4.3 ms across two runs; parent returns took about 1.1-2.2 ms. These are service response
times, excluding renderer frame scheduling. The whole-drive statistics pass took
about 56.5 seconds and reported the expected 69,632,000,000 logical bytes before
deduplication and 4,096 bytes after it.

First-time sorting/filtering a directory with millions of direct children can
still take time. This change removes recursive counters from the normal list's
critical path and avoids repeating unchanged page queries; it does not make every
possible database query constant-time.

See TESTING.md for regression coverage. Linux remains the primary platform;
GitHub Actions validates both Linux and native Windows before Windows publication.
