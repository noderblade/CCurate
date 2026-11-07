# Windows build and platform boundary

Linux is CCurate's primary platform. The native Windows x64 port is experimental;
read the filesystem and preview differences below before using it for an archive.
It uses the same curation workflow, SHA-512 verification, SQLite sessions and
JSONL history. It does not require an MSYS2 shell at runtime.

## Run the Windows package

Download **CCurate-windows-x64** from a successful **Build and test** run in the
repository's Actions tab. Extract the complete ZIP into a writable directory.
Keep `ccurate.exe`, its DLLs and the `etc`, `lib` and `share` directories together.
Double-click `ccurate.exe`, then browse for the source and destination folders.
The Windows folder dialog includes drives, network locations and hidden folders.

The GUI requires Windows 10/11 x64 and a working Vulkan graphics driver. CI uses
Windows Server 2022 with software Vulkan; this does not certify every graphics
card, network share or removable drive. The package is unsigned and is not an
installer. The application does not install drivers, partition disks or import
ZFS pools. The operating system must already expose the source and destination.

PowerShell examples:

```powershell
.\ccurate.exe 'E:\Old archive' 'F:\Curated'
.\ccurate.exe --browse 'F:\Curated'
$result = Start-Process .\ccurate.exe -ArgumentList '--cli "E:\Old archive" "F:\Curated" scan' -Wait -PassThru -NoNewWindow
$result.ExitCode
```

The executable uses the Windows GUI subsystem. For scripted CLI commands,
`Start-Process -Wait -PassThru` waits for completion and exposes the exit code;
replace `scan` with `audit` to verify the destination.

Prefer ordinary desktop execution. Administrator execution can read additional
files permitted by Windows, but does not bypass sharing violations or encryption.
External default-application launching is disabled in an elevated CCurate process
so it cannot launch an editor with administrator privileges. Restricted previews
remain separate; failure to establish their restrictions blocks decoding.

## Filesystem behavior and limits

| Area | Native Windows behavior |
| --- | --- |
| Source | Opened for reading and metadata queries; curation has no source-delete operation |
| Names | UTF-8 catalog, UTF-16 Windows calls, long-path-aware executable and extended paths |
| Enumeration | Hidden and system attributes never filter the source listing |
| File identity | Volume serial number and 64-bit file ID, size and high-resolution timestamps |
| Traversal | Handle-relative opens, with reparse points inspected rather than followed |
| Publication | Exclusive temporary file, SHA-512 verification, no-overwrite rename and recovery journal |
| Case conflicts | Existing names are not overwritten, including case-insensitive collisions |
| Session | Native exclusive byte-range lock, SQLite transactions and portable JSONL export |
| Metadata | Creation/modification/change times; original Windows attributes and owner/group/DACL SDDL stored in the index |
| Permissions | Display mode bits are synthetic; UID/GID are -1. Destination files inherit destination ACLs; the original SDDL is historical data, not reapplied security policy |
| Reparse points | Symlink/junction targets or raw reparse metadata retained; targets are not traversed or recreated |
| Alternate data streams | Named streams are detected and copying is blocked with a visible error; this version does not retain their content |
| Durability | File data and SQLite are flushed. Windows has no equivalent unprivileged directory `fsync` in this adapter; namespace power-loss durability is weaker than the Linux path |

A file with a named alternate stream (including `Zone.Identifier`) is deliberately
left unresolved. It must not receive a secured status after losing that stream.
The application does not remove the stream or change the original file. This is
a material limitation of the first Windows port, not an instruction to discard
stream metadata. Use the Activity log and balance report to identify these files.

Catalog paths remain independent of destination naming rules. Historical Linux
names can be retained in JSONL; Windows cannot materialize every POSIX filename.
Invalid destination components, traversal attempts and reserved device names are
refused. Case-sensitive NTFS directories are refused explicitly rather than
resolving two distinct names to the same file. UNC/SMB semantics, ReFS, FAT/exFAT, EFS,
cloud placeholders and actual power-loss recovery need additional validation.
Do not infer support for those scenarios from NTFS CI fixtures.

## Preview implementation

Windows workers use a restricted low-integrity token and a Job Object with one
process, 1 GiB committed-memory limit, 12 CPU seconds and kill-on-close. The
parent enforces a 20-second deadline, bounded output and cancellation. Only the
input file and stdout/stderr pipes are inherited. The worker verifies its limits
before decoding; it never silently falls back to an unrestricted process.

The same JPEG, GdkPixbuf, Poppler, FFmpeg and libarchive decoders are used. PDFs
are read through a bounded stream (256 MiB input maximum). Large JPEGs use the
existing reduced-resolution scanline decoder. Preview output is at most 1000 by
800 pixels; archives are listed without extraction.

Low integrity plus a Job Object is **not an AppContainer** and does not reproduce
Linux's socket filter or per-scratch-file limit. It is resource and privilege
restriction, not complete filesystem/network isolation. Restricted workers may
be unable to reopen administrator-only inputs. Decoder refusal is visible in the
preview and log; copying is independent of preview availability.

## Build from source

Use the **MSYS2 UCRT64** terminal with native UCRT64 packages. MSVC and 32-bit
builds are not supported by this first port. The exact dependency list lives in
[the workflow](../.github/workflows/build.yml), including GCC, CMake, Ninja,
pkgconf, Python/Pillow, OpenSSL, SQLite, libarchive, file/libmagic, Vulkan,
Fontconfig, GLib, libjpeg-turbo, GdkPixbuf, Poppler and FFmpeg.

```sh
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows --parallel 4
ctest --test-dir build-windows --output-on-failure
python scripts/package_windows.py build-windows/ccurate.exe dist/CCurate-windows-x64
```

The packager walks PE import tables recursively, includes dynamically loaded
image loaders and their resources, rejects an MSYS runtime dependency, and emits
a SHA-256 `FILES.json` manifest and third-party license files. A complete package
must also pass integration and GUI checks with MSYS2 removed from `PATH`.
CI stages and registers Mesa's software Vulkan driver on its disposable runner;
it is not
bundled with the application or used to satisfy application DLL dependencies.

## Source separation

All Windows implementation files are under `src/platform/windows/`; their
interfaces are under `include/platform/windows/`. They are compiled only by
`cmake/Windows.cmake`. The original Linux CMake graph and Linux service
implementations remain in place. Vendored Raven is unchanged.

| Windows file | Responsibility |
| --- | --- |
| `fs_windows.c` / `fs_windows.h` | Descriptor adapter, paths, handle-relative I/O, enumeration, locking, durability and space queries |
| `metadata_windows.c` | Attributes, SDDL, stream detection and reparse metadata |
| `process_windows.c` / `process_windows.h` | Restricted process creation, pipes, handle inheritance and Job Objects |
| `preview_windows.c` / `preview_windows.h` | Preview protocol, cancellation and worker restrictions |
| `preview_media_windows.c` | Bounded Poppler stream input |
| `desktop_windows.c` | Default-application opening and elevation checks |
| `fonts_windows.c` / `fonts_windows.h` | System and per-user font discovery |
| `dialog_windows.c` / `dialog_windows.h` | Native folder browsing, including hidden folders |
| `main_windows.c` / `native_windows.h` | UTF-16 entry point, runtime paths and Win32 helper declarations |
| `ccurate.rc` / `ccurate.manifest` | Long paths, DPI behavior and as-invoker execution |

Small `_WIN32` selections connect shared services to those implementations.
Windows adapters are never included by Raven or a Linux build. Shared curation,
SQL, hashing, sorting and GUI behavior stays common.

## Automated validation

Every push and pull request runs two independent jobs:

- **Linux primary regression suite:** full and core builds, integration tests,
  million-entry paging, live scans, sorting, media processes and seven native GUI
  suites under Xvfb/software Vulkan.
- **Windows x64 UCRT64:** native compilation, disposable NTFS integration fixtures,
  real restricted preview workers, Win32/Vulkan GUI checks, packaging and tests
  without the development toolchain on the application's `PATH`.

A green compile alone is insufficient. Both jobs must pass before integrating
platform work into the primary branch. See [TESTING.md](../TESTING.md) for the
recorded results and remaining validation limits. CI artifacts contain logs and
Windows GUI screenshots.
