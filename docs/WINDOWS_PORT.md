# Native Windows port assessment

CCurate currently targets Linux. A native Windows executable is feasible, but
the application does not currently compile or run on Windows. This is a source
review and implementation proposal, not a tested Windows build.

The UI library is not the main obstacle: bundled Raven already selects Win32
platform sources and Windows threads in its CMake build. Most application UI,
SQLite/JSONL logic, SHA-512 verification and format decoders can be reused. The
substantial work is replacing Linux filesystem and subprocess behavior while
preserving the application's data-handling guarantees.

## Work by subsystem

| Area | Current dependency | Required Windows work |
| --- | --- | --- |
| Safe traversal and copying | `util.c`, `catalog.c`, `operations.c`: descriptor-relative `openat`/`fstatat`, `O_NOFOLLOW`, `renameat2(RENAME_NOREPLACE)`, directory syncing | Handle-based traversal and file identity, junction/reparse-point policy, exclusive temporary creation, no-overwrite publication and recovery tests |
| Inventory and metadata | Linux `statx`, inode/device IDs, UID/GID/mode, xattrs/ACLs, virtual-filesystem detection | Windows timestamps and file IDs; explicit handling of attributes, security descriptors and alternate streams; preserve existing POSIX history without inventing equivalent values |
| Paths | POSIX separators and arbitrary filename bytes | UTF-16/UTF-8 conversion, drive/UNC roots, long paths, case collisions and Windows-reserved names; keep historical names independent of valid destination names |
| Sessions and storage metrics | `flock`, `statvfs`, Unix directory enumeration | Native locking, free-space queries and enumeration; retain SQLite transactions and recovery behavior |
| Preview processes | `posix_spawn`, `/proc/self/exe`, inherited descriptors, `poll`, signals, `setrlimit`, `prctl`/seccomp and UID/GID drop | Windows process/pipe/handle management, cancellation and resource limits; restricted execution appropriate to untrusted decoders |
| Desktop integration | POSIX launcher process, GIO default associations, Linux user discovery | Windows default-application launching and elevation handling; validate installed-font discovery and font-file loading |
| Build and packaging | GNU/POSIX assumptions, Unix libraries and dependency discovery | Conditional platform sources and compiler flags, native dependency builds, runtime DLL/resource collection and clean-machine launch tests |
| Tests | POSIX fixtures, signals, permissions, Xvfb/xdotool | Shared content/session tests plus native Windows filesystem, worker and UI fixtures; retain the Linux suite |

Windows worker limits and restricted execution need separate design: process
resource limits alone do not prevent filesystem or network access. Removing the
Linux checks or replacing them with success-returning stubs would not constitute
a usable port. Similarly, a rename fallback must preserve refusal to overwrite
existing content and the ability to recover an interrupted operation.

Alternate data streams need an explicit retention policy. They must not silently
disappear when a Windows file is reported as fully secured. Source-drive format,
available metadata and destination naming limits should remain visible in the
catalog and user-facing diagnostics.

## Recommended approach and effort

Start with a native x64 build using **MSYS2 UCRT64, GCC and CMake/Ninja**. UCRT64
provides a native Windows toolchain while keeping the build close to the current
GCC setup. The intended deliverable is a Windows executable with its required
DLLs and resources, not an application dependent on a running MSYS/Cygwin shell.
MSVC is a possible later target, but adds compiler/runtime compatibility work.
The [MSYS2 environment documentation](https://www.msys2.org/docs/environments/)
describes the toolchain distinction.

Introduce a small application platform interface with separate Linux and Windows
`.c` implementations. Move filesystem/metadata and worker/desktop operations
behind it, rather than spreading conditional compilation through the GUI or
changing vendored Raven. Keep the existing source-read-only behavior, independent
destination verification, deduplication and index format semantics.

Suggested sequence:

1. Build the core and port safe traversal, file identity, locking, copying and
   atomic publication. Run scan/copy/audit/resume/recovery tests on Windows.
2. Enable Raven and adapt path handling, font discovery, startup selection and
   default-application launching. Test Windows and Linux archive metadata.
3. Port preview workers and their limits, package codecs/resources, and test
   cancellation, corrupt inputs, large images and elevated application startup.
4. Validate long/UNC paths, junctions, case collisions, interrupted copies and
   the declared metadata/stream coverage on the target filesystems.

This is a moderate-to-large portability change across several service modules,
not a GUI rewrite or one compiler switch. A rough planning allowance is **2-4
developer-weeks for a usable first native version**, assuming the dependencies
work under UCRT64. Full metadata coverage, isolation hardening and validation on
real archive hardware may take longer. A compile-only experiment could be much
shorter but would not establish safe curation behavior. These estimates come
from static inspection; no Windows compiler or runtime was exercised here.

The port also does not make a ZFS drive accessible to Windows by itself. CCurate
needs paths exposed by the operating system or a mounted network filesystem;
it does not supply filesystem drivers or import pools.

## GitHub Actions automation

GitHub provides Windows-hosted runners, so a separate Windows build machine is
not required for continuous integration. Once the platform code works, the
proposed pipeline is:

| Stage | Proposed action |
| --- | --- |
| Trigger | Push, pull request and manual `workflow_dispatch` |
| Environment | Windows x64 runner, initially a specific supported image such as `windows-2022`; set up MSYS2 UCRT64 |
| Dependencies | Install UCRT64 compiler, CMake/Ninja, pkg-config and the native builds of the existing libraries; record versions in the build log |
| Build | Configure separate core and GUI/media builds; compile with warnings enabled |
| Validate | Run Windows-compatible integration and platform tests; retain independent Linux jobs |
| Package | Stage `ccurate.exe`, required DLLs, decoder resources, documentation and third-party notices in a ZIP |
| Deliver | Upload the ZIP and test logs as workflow artifacts; optionally add tagged-release publication later |

The build must check runtime dependency completeness, not merely whether the
linker produced an executable. Test the packaged application outside the MSYS2
development environment. Interactive Vulkan GUI checks may need a separate
Windows desktop test environment even when compilation and core tests run on
GitHub's hosted runner. A user still needs a suitable Vulkan driver at runtime.

The current change does **not** install a Windows workflow: it would fail on the
Linux-specific source. Add the workflow alongside the first functioning platform
implementation, then expand its tests and packaging as features become available.

Official references:

- [GitHub-hosted runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
- [Build and test artifacts](https://docs.github.com/en/actions/tutorials/store-and-share-data)
- [MSYS2 in CI](https://www.msys2.org/docs/ci/)
