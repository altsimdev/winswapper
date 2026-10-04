# Changelog

What changed in each release. When a version is tagged, the release workflow publishes the section
whose heading matches the tag as that release's notes - and refuses to publish a release that has
no section here, so add the entry before pushing the tag.

## Unreleased

- The exe is about 19 KB smaller: the three-display icon was embedded twice.

## 1.05 - 2026-10-04

- **New:** *Start with Windows* in the tray menu, off by default. It registers WinSwapper for your
  user only, and the checkmark stays accurate if you switch it off in Task Manager instead.
- **New:** the tray icon pictures as many displays as are connected, and changes when you plug one
  in or take one away. Hovering over it shows the count.
- **Fix:** command-line runs (`--list`, `--dry-run`, `--selftest`) lost all their output while the
  tray app was running. They now write their own log, `winswapper-cli.log`.

## 1.04 - 2026-10-04

- **Fix:** maximized windows keep their order. Two maximized windows on one display used to arrive
  reversed, and both ended up in front of any normal window that had been covering them.
- **Fix:** the window you were working in keeps the keyboard focus after a rotation.
- The license is now `LICENSE.txt`, so it opens straight from the zip.

## 1.03 - 2026-10-03

- **New:** Ctrl+Alt+Shift+S rotates windows one display to the **right**, undoing Ctrl+Alt+S. It is
  also on the tray menu, and available as `--reverse` / `--right` on the command line.
- The two hotkeys are registered separately, so if another app already uses one, the other still
  works.
- `--selftest` now checks the rotation logic even with a single display, and no longer reports a
  false failure when displays have different resolutions.

## 1.02 - 2026-09-25

The first public release.

- Ctrl+Alt+S moves every application window one display to the left, with the leftmost display
  wrapping around to the rightmost. With two displays it is a straight swap.
- Works with any number of displays, ordered left to right by position.
- Each window keeps its size and its position within its display; maximized and minimized windows
  move too.
- Leaves alone tool windows, windows on other virtual desktops, and fullscreen games.
- Command line: `--list`, `--dry-run`, `--rotate`, `--selftest`.

This build's About box and file version say 1.01 - the version number was not updated for this
release.
