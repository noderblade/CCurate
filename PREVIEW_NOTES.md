# Lazy resume, large-catalog navigation and space-statistics preview

Date: 2026-11-09. Base: `6e7ab866533093fae87dd26733a10dda0fadf3cc`.
This source package contains uncommitted changes for local evaluation. It does
not correspond to a new GitHub commit or a replacement Windows release.

## What changed

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

See TESTING.md for regression coverage. Linux is the validated primary platform.
No GitHub workflow was run for this uncommitted preview.
