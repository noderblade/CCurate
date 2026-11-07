# Third-party code

Raven is included unmodified under `vendor/Raven`:

- Repository: https://github.com/oscar-soirey/Raven
- Revision: `ede33eb15f986766f063aadb109f3c379cd8f6ea`
- CMake project version: 0.2.1
- Main license: Apache-2.0, see `vendor/Raven/LICENSE`.

Keep Raven's bundled dependency notices/licenses with its source. Its existing
third-party assets and libraries retain their respective original terms.

The source repository does not vendor its system dependencies: OpenSSL, SQLite,
libarchive, libmagic, Fontconfig, X11/Vulkan, libjpeg/libjpeg-turbo, GdkPixbuf, Poppler, Cairo/GLib/GIO, and FFmpeg.
Their respective licenses and build configurations apply when linking or
redistributing binaries.

The Windows runtime ZIP includes native DLLs and resources from MSYS2 UCRT64
packages. Their notices are copied into `licenses/`; `BUILD_ENVIRONMENT.txt`
records the installed package versions used for that build. Package build recipes
are maintained at https://github.com/msys2/MINGW-packages and link to each
dependency's upstream source. The included `FILES.json` records the packaged
files and their SHA-256 values. Raven's license is also included. These notices
describe the bundled components; they do not replace their respective terms.

## Color palette references

The application's semantic colors are adaptations of these public palettes;
no artwork or third-party theme implementation is copied:

- Tango: https://wiki.gnome.org/Design/OS/Colors
- Solarized, Ethan Schoonover: https://ethanschoonover.com/solarized/
- Nord: https://www.nordtheme.com/docs/colors-and-palettes/
- Dracula: https://draculatheme.com/contribute
- Gruvbox: https://github.com/morhetz/gruvbox
- Catppuccin Mocha: https://catppuccin.com/palette/
- Tokyo Night: https://github.com/folke/tokyonight.nvim

CCurate Dark is the application's own original palette. Status labels remain
available independently of color.

## Font selection

Installed fonts are discovered through Fontconfig (https://www.fontconfig.org/).
They remain external system/user files under their own licenses. Raven's bundled
Roboto Mono and its font license are retained unmodified with the vendor tree.
No additional font files are shipped in this release. GUI tests create temporary
fixtures from locally installed DejaVu/URW fonts; those fixtures are not included.
