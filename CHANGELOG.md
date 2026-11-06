# Changelog

## Unreleased

- Browse for source and destination folders when GUI arguments are omitted,
  with mouse and keyboard controls, hidden directories and validation before
  session creation. A single source argument prefills the source; bare `--browse`
  selects an existing destination archive.
- Keep selected roots and show session-opening errors in the startup chooser.
- Document the scope of a native Windows port and GitHub Actions build automation.

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
