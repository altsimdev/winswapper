# Changelog

What changed in each release. When a version is tagged, the release workflow publishes the section
whose heading matches the tag as that release's notes - and refuses to publish a release that has
no section here, so add the entry before pushing the tag.

## Unreleased

- **New:** a window dragged with Alt snaps to the edges of the displays and the taskbar. Within 10
  pixels, its edge - the one you can see, not the invisible border Windows puts around it - jumps
  flush with the edge, and a side being resized does the same. A new `Snap` setting under `[Drag]`
  changes the distance, and `0` turns snapping off.
- **New:** hold Alt and click the middle mouse button (the wheel) to send a window behind all the
  others, as on KDE. If it had the keyboard, the window now under the pointer gets it. A window kept
  on top of others stays in front of the rest. It is on by default, with its own `Lower` setting
  under `[Drag]`.
- **New:** the settings file explains itself. Whenever WinSwapper finds it missing or blank - at
  start-up, and on *Reload settings* and *Edit settings* - it writes every setting there, described
  and shown at its default, with examples, all commented out: delete the `;` to use one. Before,
  only *Edit settings* wrote the file, and an empty one stayed empty. A file with anything in it is
  never touched.
- **New:** saving the settings file applies it straight away, with a notification showing the
  settings now in use, or what is wrong with them. Before, nothing changed until *Reload settings*
  or a restart. A save that changes no setting goes unremarked.
- **Fix:** when Windows signed out, shut down or restarted, the tray log just stopped, with nothing
  to tell that from a crash. It now says which it was - or that an installer asked WinSwapper to
  close.

## 1.09 - 2026-11-07

- **New:** hold Alt and drag anywhere in a window to move it, as on many Linux desktops - no need
  to find the title bar. A maximized window returns to its normal size under the pointer. An
  Alt+click without a drag still reaches the program. It is on by default; a new `[Drag]` section
  in the settings changes the keys, turns it off, or leaves out programs with an Alt+drag of their
  own.
- **New:** hold Alt and drag with the right mouse button to resize a window by the corner or side
  nearest the pointer - no need to find its border. It stops at the window's smallest size, and a
  maximized window first becomes an ordinary one filling the screen. An Alt+right-click without a
  drag still opens the program's menu. It is on by default, with its own `Resize` setting under
  `[Drag]`.

## 1.08 - 2026-11-04

- **Fix:** dialogs move with their window. An open dialog, such as *File Open*, used to stay behind
  while the window it belongs to moved to another display, which left that window unusable until
  you found the dialog. Floating toolbars and palettes now come along too. Dialogs of a program on
  the ignore list stay put with it.
- **Fix:** the tray log could stay empty for a whole run. If the file could not be opened when
  WinSwapper started - another program holding it, say - nothing was written until the next
  restart, and nothing said so. Now WinSwapper keeps what it logs meanwhile and keeps trying. Once
  the file opens, a note says what went wrong, followed by everything kept. Until then, *About* and
  *Open log* say the log is not being written, and why.

## 1.07 - 2026-10-06

- **Fix:** WinSwapper no longer needs the Microsoft Visual C++ Redistributable. Every earlier release
  did, and would not start on a Windows installation that lacked it ("VCRUNTIME140.dll was not
  found"). It now needs nothing beyond Windows 10 or 11, and the build checks that this stays true.

## 1.06 - 2026-10-06

- **New:** the hotkeys can be changed. *Edit settings* in the tray menu opens a settings file
  (created on first use, with every option explained), and *Reload settings* applies it. A hotkey
  can also be turned off.
- **New:** an ignore list in the same file: programs whose windows always stay where they are while
  everything else rotates around them.
- **New:** `--selftest --no-windows` runs every check except the ones that open and move windows,
  so it can run without disturbing anything on screen.
- The exe is about 19 KB smaller: the three-display icon was embedded twice.
- **Fix:** the tray log's 1 MB cap is now checked while the app runs, not only when it starts, so a
  copy left running for weeks can no longer grow its log without limit.

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
