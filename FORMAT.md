# CCurate portable archive format — schema 1

The file `.ccurate/archive.jsonl` is UTF-8 JSON Lines. Each line is one
complete JSON object. This is a full export, not a journal to replay. Publishing
uses a temporary file, fsync, rename, and a directory fsync. SQLite remains the
authoritative transactional session while a batch is in progress.

## Records

| `type` | Purpose | Principal fields |
|---|---|---|
| `header` | Format and original root hint | `schema`, `hash_algorithm`, `generated_ns`, `source`, `source_b64` |
| `object` | One physical copy per content hash | `sha512`, `size`, `destination`, `destination_b64`, `state`, `verified_ns` |
| `entry` | Retained occurrence/version at an original path | `path`, `path_b64`, `kind`, `version`, `sha512` when regular, original metadata |
| `event` | Placement history | `id`, `time_ns`, `operation`, `from`, `from_b64`, `to`, `to_b64`, optional `sha512` |

An `entry` hash resolves through an `object` to a path relative to the archive
root. Several entries may resolve to one object. Paths are relative, without
leading `/`, `..`, or embedded NUL. The empty entry path represents the original
root. The header's `source` is an absolute historical hint, not a required mount.

The corresponding `_b64` field is authoritative for every path or symlink text.
It preserves arbitrary POSIX filename bytes, including non-UTF-8 names. The
readable field is a display spelling and may contain escaped byte values.
New software must use `_b64` for exact reconstruction.

SHA-512 is 128 lowercase hexadecimal characters. It identifies regular-file
content, not names, permissions, xattrs or symbolic-link text. A hardlink's
original relationship can be inferred from retained `dev` and `ino`; the archive
does not create hardlinks on the destination.

Entry metadata:

- `mode`: original numeric mode, including object type and permission bits.
- `size`, `uid`, `gid`: integer values from the source.
- `mtime_ns`, `ctime_ns`, `atime_ns`, `btime_ns`: decimal strings, Unix nanoseconds.
  A zero birth time means unavailable. `ctime` means inode-change time.
- `dev`, `ino`: decimal strings for original device/inode identity.
- `link`, `link_b64`: symbolic-link target, when applicable.
- `xattrs`: array of `{name_b64, value_b64}`; POSIX ACL xattrs are retained here
  when the filesystem exposes them. `metadata_error` records incomplete reads.
- `current_at_export`: whether this version was the latest observed version of
  its source path. Older retained versions are still valid archive entries.

The GUI displays the newest retained version at a path and lets the user cycle
older versions. These versions are not a global filesystem snapshot. Directories
and metadata-only objects can have no `sha512`; opening them never invents bytes.

Object `state` is `verified`, `missing`, `corrupt`, or `unchecked`. A successful
audit rehashes actual destination bytes. Import deliberately assigns `unchecked`
regardless of the last exported status. `verified_ns` refers to the last actual
verification, not continuous monitoring.

Events describe physical copy/reuse, destination move, and recovered locations.
They can mention a historical source name whose occurrence was later rejected.
Events are provenance, not instructions to create virtual tree nodes. Build the
virtual tree from `entry` records and resolve bytes through `object` records.

The JSONL export is sufficient to rebuild the historical browser with the copied
data present. It intentionally does not reconstruct rejected/unhandled inventory,
review markers, pending operations or UI state; those belong to `catalog.sqlite`.

## Session persistence

`PRAGMA user_version=1`. The catalog stores `settings`, `entries`, `versions`,
`rejected_retention`, `objects`, `operations`, `events`, `hash_groups`,
`scan_queue` and `discoveries`. Indexes support paged listing, content lookups and
retained-history queries. The application checks the schema version when opening
a session and refuses one newer than it supports.

For a regular file, the write sequence is:

1. Validate the observed source version and record a durable copy intent.
2. Write a unique temporary file in the destination directory, hashing the source.
3. Confirm the source stayed unchanged; fsync and independently hash the copy.
4. Publish with `renameat2(RENAME_NOREPLACE)` and fsync the directory.
5. Commit object mapping and all non-rejected occurrences in SQLite.
6. Export JSONL at the end of the batch.

Recovery validates any published content before completing an interrupted intent.
An existing unrelated destination is never silently overwritten. Cancellation
keeps already committed copies and does not mean the whole batch was rolled back.
The application uses one writer job at a time and a separate GUI read connection.

## Inventory and query caches

`hash_groups` is a rebuildable duplicate-count cache. A trigger updates it when
the current version or source presence changes. `scan_queue` is a disposable
on-disk work plan created for a scan and removed on completion or cancellation.
A subsequent scan rebuilds the plan and reuses hashes of completed, unchanged files.
Neither table adds unretained entries to the portable archive.

`discoveries` stores the latest unverified metadata for each encountered path:
entry ID, size, mtime, birth time, mode, UID/GID and device/inode. Entries become
visible as enumeration commits batches, before the SHA-512 pass starts. Live
views overlay these observations and use `pending` status without inventing a
hash or verified copy. Historical views use retained `versions`.

Successful verification removes an observation; cancellation leaves it for the
next scan. Pending entries cannot be copied or marked secured by the copy path.
Observations are session inventory, not part of JSONL. Earlier retained versions
continue to export while a changed live path awaits verification.

## Activity log

`activity.log` is an append-only UTF-8 diagnostic file in the state directory.
Each line has a UTC timestamp, DEBUG/INFO/WARN/ERROR level, event, JSON-quoted
source/target display paths and message. It contains no file payloads and is not
part of the archive format. Display escaping does not replace the lossless
`_b64` path fields in JSONL.

## Interface preferences

The `settings` table stores these independent UI preferences:

| Key | Meaning |
| --- | --- |
| `ui.left`, `ui.right` | Last destination and source directories |
| `ui.selected`, `ui.page` | Selected row and page in the right pane |
| `ui.filter` | Unsecured-only filter |
| `ui.sort.left`, `ui.sort.right` | Sort column (0 name, 1 status/copies, 2 size, 3 modified, 4 created, 5 mode), plus 256 for descending |
| `ui.date.left`, `ui.date.right` | Date column: 0 modified, 1 created |
| `ui.scale` | Interface scale from 0.5 to 4.0 |
| `ui.split.top`, `ui.split.bottom` | Destination and preview width proportions |
| `ui.split.rows`, `ui.split.log` | Upper-row and activity-log height proportions |
| `ui.theme` | Color-scheme ID, listed below |
| `ui.log.visible` | Whether the Activity panel is open |
| `ui.log.level` | -1 all, 0 debug, 1 info, 2 warning, 3 error |
| `ui.font.file` | Empty for automatic system monospace, `@bundled` for Roboto Mono, or an installed font path |
| `ui.font.family`, `ui.font.style` | Fontconfig family/style used to resolve a moved installed font |
| `ui.font.index` | Zero-based face index in a font collection |

Theme IDs: 0 CCurate Dark, 1 Tango Dark, 2 Tango Light, 3 Solarized Dark,
4 Solarized Light, 5 Nord, 6 Dracula, 7 Gruvbox Dark, 8 Catppuccin Mocha,
9 Tokyo Night. Unknown IDs fall back to Tango Dark.

These preferences are separate from retained source metadata. Font bytes are
not stored in SQLite or JSONL. Navigation bookmarks within a running session
are bounded, transient UI state.
