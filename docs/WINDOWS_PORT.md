# Windows build and platform boundary

Linux is CCurate's primary platform. The native Windows x64 port is experimental;
read the filesystem and preview differences below before using it for an archive.
It uses the same curation workflow, SHA-512 verification, SQLite sessions and
JSONL history. It does not require an MSYS2 shell at runtime.

## Run the Windows package

**[Download the latest Windows x64 build (.zip)](https://github.com/noderblade/CCurate/releases/download/windows-latest/CCurate-windows-x64.zip).**

Extract the complete ZIP into a writable directory.
Keep `ccurate.exe`, its DLLs and the `etc`, `lib`, `share` and `software-vulkan`
directories together.
Double-click `ccurate.exe`, then browse for the source and destination folders.
The Windows folder dialog includes drives, network locations and hidden folders.

The download URL stays the same across updates. A separate publishing workflow
updates the `windows-latest` prerelease only after **Build and test** succeeds
for `main`, including both Linux and Windows checks. Failed builds leave the
previous download available. This does not publish Linux binaries; building from
source remains the recommended Linux installation method.

The [release page](https://github.com/noderblade/CCurate/releases/tag/windows-latest)
identifies the source commit and test run. A
[SHA-256 checksum](https://github.com/noderblade/CCurate/releases/download/windows-latest/SHA256SUMS)
is provided alongside the ZIP. Downloading the public release does not require
a GitHub account. Actions artifacts remain available for individual test runs.

The GUI requires Windows 10/11 x64. It prefers system Vulkan and automatically
uses the packaged Mesa Lavapipe CPU renderer when Vulkan initialization or Win32
presentation support fails. A hardware Vulkan driver is not required for CPU
rendering. CI uses Windows Server 2022 with software Vulkan; this does not certify every graphics
card, network share or removable drive. The package is unsigned and is not an
installer. The application does not install drivers, partition disks or import
ZFS pools. The operating system must already expose the source and destination.

### Software rendering and virtual machines

Launch `ccurate.exe` normally to use automatic selection. A VM's virtual display
adapter may have no Vulkan window-surface support even though `vulkan-1.dll` is
present. In that case CCurate retries with the bundled CPU driver. The startup log
records the failed system attempt, the selected CPU device and whether the first
GUI frame completed. The folder chooser and curation workspace both support it.

To force CPU rendering, double-click **CCurate-Software.cmd**. For command-line
control, set `CCURATE_RENDERER` to `auto` (default), `software` or `vulkan`.
`vulkan` requires the system driver and disables fallback for diagnostics.
CLI commands and preview workers do not initialize the GUI renderer.

The Windows-only graphics module loads the packaged driver with
[`VK_LUNARG_direct_driver_loading`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_LUNARG_direct_driver_loading.html)
and passes its device to Raven through `Rvulkan_set_external_device`. It works
with ordinary and elevated application processes, without installing an ICD,
changing the registry, or relying on `VK_DRIVER_FILES` overrides. Raven's vendored
code remains unmodified. Linux retains its existing renderer initialization.

CPU rendering can use more CPU time and memory and can be slower at large window
sizes or high UI scales. The additional Mesa/LLVM DLLs also increase the Windows
download size. If the software driver is missing, extract the complete package
again; copying just `ccurate.exe` is insufficient.

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
restriction, not complete filesystem/network isolation. Workers use the inherited
read-only input handle, including bounded mappings for positional reads, without
reopening the source path under a different identity. Decoder refusal is visible
in the preview and log; copying is independent of preview availability.

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
The packager includes Lavapipe and its recursively discovered native dependencies
under `software-vulkan`, plus their licenses. CI uses this same application-local
driver without registering it. Packaged GUI tests run with only Windows system
directories on `PATH`.

## Startup diagnostics

If no window appears, extract the **complete** current Windows ZIP and
run **CCurate-Debug.cmd** from that folder. The launcher waits for the application,
shows console diagnostics and the decimal/hexadecimal Windows exit code, saves
its output, and waits for Enter before closing. It uses the Windows PowerShell
already supplied by Windows; no development tools are required. Its execution
policy option applies only to that PowerShell process.

Normal application startup attempts to create a UTF-8 log under:

```text
%LOCALAPPDATA%\CCurate\Logs\startup-<UTC timestamp>-<PID>.log
```

Paste `%LOCALAPPDATA%\CCurate\Logs` into Explorer's address bar. The launcher
also writes `launcher-*.log` and `console-*.log` there. `CCURATE_LOG_DIR` overrides
the directory; if it is unavailable, diagnostics try the standard directory and
then `%TEMP%\CCurate\Logs`. Each application stderr log is capped at 8 MiB.
The launcher logs are separate from `.ccurate/activity.log`, which belongs to an
opened curation session and may not exist when startup fails.

For terminal use:

```powershell
.\CCurate-Debug.cmd
.\CCurate-Debug.cmd --version
# Skip the launcher's final prompt when collecting diagnostics automatically:
.\CCurate-Debug.cmd -NoPause --version
# Direct application startup with additional diagnostics:
.\ccurate.exe --debug
```

Normal GUI invocations also attach to an available parent console. Redirected
stdout/stderr remain usable, and stderr is copied to the startup log. `--debug`
adds Vulkan-loader and Windows-adapter diagnostics; when no parent console or
redirected streams exist it creates a diagnostic console. The flag precedes the
normal arguments, for example `ccurate.exe --debug --browse D:\Archive`.

Logs record the build revision, Windows build, display adapters, relevant Vulkan
environment overrides, Raven errors, initialization stages, first frame and exit
status. They do not dump file contents or process memory. Paths and diagnostic
messages can include local usernames and directory names. Restricted preview/open
workers bypass the startup logger and keep their inherited binary protocols.

An application cannot write its own log if Windows fails before its entry point.
The independent launcher still records errors such as `0xC0000135` (missing DLL),
`0xC000007B` (invalid/mismatched binary), `0xC0000142` (DLL initialization failure)
and `0xC000001D` (illegal instruction, potentially missing VM CPU features).
A GUI startup failure without a console also displays a message with the log path.
Unhandled Windows exceptions are recorded when the exception handler can run.

A VM's display adapter may not provide Vulkan window-surface support. Look for
`Software rendering active (CPU / Mesa Lavapipe)` after the system-driver failure.
If automatic selection still fails, try `CCurate-Software.cmd` and include the
`[graphics]` messages in the report. The presence of `vulkan-1.dll` alone does not
prove a usable Vulkan graphics device is available.
Send the latest startup and launcher logs when reporting a silent exit.

## Source separation

All Windows implementation files are under `src/platform/windows/`; their
interfaces are under `include/platform/windows/`. They are compiled only by
`cmake/Windows.cmake`. The original Linux CMake graph and Linux service
implementations remain in place. Vendored Raven is unchanged.

| Windows file | Responsibility |
| --- | --- |
| `diagnostics_windows.c` / `diagnostics_windows.h` | Early console attachment, stderr/file capture, startup stages and exception diagnostics |
| `graphics_windows.c` / `graphics_windows.h` | System Vulkan selection, packaged CPU fallback and external-device lifetime |
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
