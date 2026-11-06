# Third-party code

Raven is included unmodified under `vendor/Raven`:

- Repository: https://github.com/oscar-soirey/Raven
- Revision: `ede33eb15f986766f063aadb109f3c379cd8f6ea`
- CMake project version: 0.2.1
- Main license: Apache-2.0, see `vendor/Raven/LICENSE`.

Keep Raven's bundled dependency notices/licenses with its source. Its existing
third-party assets and libraries retain their respective original terms.

System dependencies are not copied into this distribution: OpenSSL, SQLite,
libarchive, libmagic, Fontconfig, X11/Vulkan, libjpeg/libjpeg-turbo, GdkPixbuf, Poppler, Cairo/GLib/GIO, and FFmpeg.
Their respective licenses and build configurations apply when linking or
redistributing binaries. This package contains source code and documentation,
not a statically linked redistribution of those system libraries.

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
