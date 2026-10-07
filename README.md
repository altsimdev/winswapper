# WinSwapper

[![build](https://github.com/altsimdev/winswapper/actions/workflows/build.yml/badge.svg)](https://github.com/altsimdev/winswapper/actions/workflows/build.yml)

A small Win32 tray utility. It waits for a global hotkey, and when fired it moves every ordinary
application window one display sideways — keeping each window's size and its position within its
display. The end display wraps around, so nothing is lost and no display ends up empty by accident.

- **Ctrl+Alt+S** rotates left
- **Ctrl+Alt+Shift+S** rotates right

The two directions are inverses, so one of each puts everything back — to the exact pixel when the
displays share a resolution, and to within a pixel or two of rounding when they don't. With two
displays both do the same swap; with *N* displays, *N* presses the same way also completes the cycle.

## Build

Requires Visual Studio with the "Desktop development with C++" workload. Open `winswapper.sln` and
build, or from a Developer Command Prompt / Developer PowerShell:

```bash
msbuild winswapper.sln /p:Configuration=Release /p:Platform=x64
```

The binary lands in `build\x64\Release\winswapper.exe`. It has no runtime dependencies beyond
Windows itself, and needs no installer — put it anywhere. To have it run at sign-in, turn on
*Start with Windows* in its tray menu.

The project does not pin a toolset or an SDK: it asks for `$(DefaultPlatformToolset)` and Windows
SDK `10.0`, which resolve to whatever that machine has installed (`v145` on Visual Studio 2026,
`v143` on 2022). Nothing here needs more than C++17. To pin an exact pair, pass them on the command
line:

```bash
msbuild winswapper.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.22621.0
```

`.github/workflows/build.yml` builds both configurations on `windows-latest` for every push and
pull request, smoke-tests the binary, and uploads it as an artifact.

## Releases

Pushing a tag builds Release and attaches `winswapper-<tag>-x64.zip` to a GitHub release for that
tag, alongside a `.sha256` checksum. The zip holds the executable, this README and the license —
nothing to install.

```bash
sha256sum -c winswapper-1.04-x64.zip.sha256
```

Each release's notes are its section of [CHANGELOG.md](CHANGELOG.md), whose heading must match the
tag — `## 1.05 - <date>`. The release workflow refuses to publish a tag that has no section, so write
the entry before tagging. If it is missing, nothing gets published; add the entry and move the tag
to the commit that has it. To preview a release's notes:

```bash
pwsh .github/release-notes.ps1 -Tag 1.04
```

It also refuses to publish an exe that does not report the tag's version everywhere a version shows:
the file and product version strings and the numeric version in `res/winswapper.rc`, the About box
in `src/main.cpp`, and the manifest. Release tags take the form `1.05`, whose numeric form is
`1.0.5.0`. To check a build before tagging it:

```bash
pwsh .github/check-version.ps1 -Exe build\x64\Release\winswapper.exe -Tag 1.05
```

## Use

Run with no arguments and it goes to the notification area. Right-click the icon for *Rotate left*,
*Rotate right*, *Start with Windows*, *Edit settings*, *Reload settings*, *Open log*, *About* and
*Exit*; double-click it to rotate left.

The icon pictures as many displays as are connected — one to four, and four for anything more — and
redraws itself when you plug a display in or take one away. Hovering over it shows the exact count
and the hotkeys.

*Start with Windows* is off until you turn it on. Ticking it registers this copy of the exe to start
when you sign in, for your user only and without administrator rights; it then also appears in Task
Manager's *Startup apps*. The checkmark reflects what will actually happen: it is clear if the entry
was switched off in Task Manager, or if it points at a different copy of the exe, and ticking it then
puts that right.

The two hotkeys are registered independently, so if another application has claimed one of them the
other still works — and the tray menu always does.

### Settings

*Edit settings* opens `%LOCALAPPDATA%\WinSwapper\settings.ini` in Notepad, creating it first with the
defaults and a comment explaining each setting. Until then there is no file and the defaults apply.
After saving, choose *Reload settings* — no restart needed.

```ini
[Hotkeys]
RotateLeft  = Ctrl+Alt+S
RotateRight = Ctrl+Alt+Shift+S

[Ignore]
slack.exe
```

- **Hotkeys** are modifiers and a key joined with `+`. Modifiers are `Ctrl`, `Alt`, `Shift` and
  `Win`, and at least one of `Ctrl`, `Alt` or `Win` is required, so a hotkey can never swallow
  ordinary typing. Keys are `A`–`Z`, `0`–`9`, `F1`–`F24`, `Left`, `Right`, `Up`, `Down`, `Home`,
  `End`, `PageUp`, `PageDown`, `Insert`, `Delete`, `Space`, `Tab`, `Enter` and `Esc`. `none` turns a
  hotkey off.
- **Ignore** lists programs whose windows are never moved, one per line, named as in the *Details*
  tab of Task Manager; `.exe` may be left off. Their windows stay put while everything else rotates
  around them. `--list` shows each ignored window and the line that caught it.

A mistake affects only its own line: a bad hotkey keeps its default, and anything unusable is
reported with its line number in the log and in a notification, while the rest of the file still
applies. The file may be saved as UTF-8 (with or without a byte-order mark) or UTF-16. The
command-line modes read it too, so `--help` shows the hotkeys actually in use.

There are also four command-line modes, which print to the console they were launched from and to
their own log file (see below):

| Command | What it does |
|---|---|
| `winswapper.exe` | Run in the notification area (the normal way to use it) |
| `winswapper.exe --list` | Show the displays and **every** top-level window, with the reason each one was included or excluded |
| `winswapper.exe --dry-run` | Compute the whole rotation and print it — moves nothing |
| `winswapper.exe --rotate` | Perform one rotation and exit (bind this to anything you like) |
| `winswapper.exe --selftest` | Verify the rotation mapping, the remap arithmetic and the three window-move paths |

`--reverse` (or `--right`) added to `--rotate` or `--dry-run` rotates right instead of left. Those
are the only modes it applies to; combined with anything else, or given alone, it is an error rather
than being silently ignored. `--swap` is accepted as a synonym for `--rotate`, since that was its
name before 1.01.

`--selftest` opens test windows and moves them between your displays, which takes the focus for a
few seconds. `--selftest --no-windows` skips just those tests: the rotation arithmetic, the icons,
*Start with Windows* (exercised in a scratch registry key) and the remap round trips all still run,
and nothing on screen is disturbed. Like `--reverse`, `--no-windows` is an error with any other mode.

`--selftest` exits 0 when every check ran and passed, 3 when it passed everything it ran but skipped
the window-move tests — because of `--no-windows`, or for want of a second display — and 1 on a
real failure. Usage errors exit 2 in every mode. The rotation arithmetic is checked regardless of
displays, which is why CI runs it on a single-display runner.

Logs are in `%LOCALAPPDATA%\WinSwapper\`:

- `winswapper.log` — the tray app's log, which *Open log* shows. It accumulates across runs; once it
  passes 1 MB it is renamed to `winswapper.old.log` and a fresh one begins. That is checked as it
  runs, not only at start-up, since with *Start with Windows* on it can run for weeks.
- `winswapper-cli.log` — the latest command-line run only. It is a separate file because the tray
  app keeps its own log open while it runs, which used to lock command-line runs out of a shared one
  and lose their output.

## How it works

**Ordering the displays.** Displays are enumerated and sorted left-to-right by their bounds.
Device-name suffixes are *not* reliable display numbers: a three-display machine can report them
left-to-right as `\\.\DISPLAY6`, `\\.\DISPLAY5`, `\\.\DISPLAY1`. Ordering is therefore by geometry
alone, and `--list` prints the order it settled on.

**The rotation.** Rotating left sends the windows on display *i* to display *i* − 1, with display 0
wrapping around to the last one; rotating right is the same thing with the sign flipped. Every
display takes part either way; there is no "unused" display.

Because each mapping is a single cycle it is a permutation, so no two displays can ever collide on
one destination, and the whole thing is reversible — either by completing the cycle or, more
cheaply, by one rotation the other way. The self-test checks all of that for two through five
displays: that each direction rotates the right way, that each is a permutation, that *N* rotations
return every display to itself, and that left-then-right and right-then-left are both the identity.

With two displays both directions are the same swap, which is the original behaviour unchanged.

**Choosing the windows.** A window is moved only if it is visible, unowned, not a tool window,
not DWM-cloaked, not a shell window, and not exclusive-fullscreen. The cloak check is the one that
matters most: without it, windows belonging to *other virtual desktops* would get hauled onto the
current one.

**Moving them.** Each window's rect is expressed as a fraction of its source display's work area
and reapplied to the destination's, so layouts survive displays of different sizes. When the work
areas are all the same size every scale factor is exactly 1.0, the mapping degenerates to an
integer translation, and a full cycle restores every window to its original pixel.

A window that merely overhangs its display is left exactly where the arithmetic puts it — only one
that would land completely off-screen is rescued. Nudging overhanging windows inward would fight
the invisible resize border that `GetWindowRect` reports on Windows 10/11 and would quietly make
each rotation lossy instead of reversible.

The three window states each need different treatment:

- **Normal** — one `SetWindowPos` with `SWP_ASYNCWINDOWPOS`, so a hung application cannot stall
  the rest of the rotation.
- **Maximized** — un-maximize, move, re-maximize. `SetWindowPlacement` alone does *not* work here:
  Windows leaves a maximized window on its current display and merely rewrites its restore rect.
  The self-test covers this.
- **Minimized** — `SetWindowPlacement` with the remapped restore rect and `showCmd` left alone, so
  the window stays minimized but reappears on its destination display. `WPF_RESTORETOMAXIMIZED` is
  preserved, so a window that was maximized before being minimized still restores maximized.

**Keeping the stacking order.** Re-maximizing a window activates it, and activation pulls it to the
front. Applied in the top-to-bottom order windows are enumerated in, that used to leave two
maximized windows on one display in reverse order, both in front of any normal window that had
been covering them. So once everything has moved, each maximized or minimized window is put back
directly beneath the moved window that had been above it — normal windows never leave their place,
so they anchor the rest — and the window that had the focus gets it back. Always-on-top windows are only
ever re-stacked among themselves, so nothing crosses between the two layers. The self-test stacks a
normal window over two maximized ones and checks they arrive in the same order.

## Known limitations

- **Elevated windows cannot be moved.** A process running as you cannot reposition a window owned
  by a process running as administrator; Windows blocks it (UIPI). Those windows are skipped and
  counted, and a tray balloon reports how many. Running WinSwapper itself as administrator would
  fix it, at the cost of a UAC prompt every launch.
- **Owned windows are skipped.** The filter matches Alt-Tab semantics, so the occasional
  application whose main window is *owned* by another window will be left alone.
- **Only the current virtual desktop is touched.** A deliberate consequence of the cloak filter.
- **Maximized windows flash as they move.** A maximized window has to be restored, moved and
  re-maximized, so it briefly appears at its restored size on the way. Its stacking position and
  the focus are put back afterwards.
- **Stacking is restored among moved windows only.** A window that is not moved — a tool window,
  say — keeps its place, so if one sat between two maximized windows it can end up on the other
  side of one of them.
- **Different DPI per display.** The app is Per-Monitor-V2 aware, so coordinates are real physical
  pixels. If displays run different scaling factors, applications will re-layout their contents on
  arrival, which the proportional mapping accounts for but cannot make invisible.
- **Settings apply when reloaded.** Editing `settings.ini` changes nothing until *Reload settings* or
  a restart; the file is not watched.
- **Store apps are ignored together.** The windows of packaged (Microsoft Store) apps all belong to
  `ApplicationFrameHost.exe`, so ignoring one of them means ignoring all of them.
- **Displays are matched by position, not identity.** Plugging in or unplugging a display changes
  the ordering, so a rotation begun on one arrangement and finished on another will not round-trip.
  This is why the two directions only cancel out while the display layout stays put.

## Layout

```
src/main.cpp      WinMain, hidden window, tray icon, hotkey, menu, message loop
src/swapper.cpp   display and window enumeration, remap arithmetic, applying moves
src/log.cpp       log files plus console output for the command-line modes
src/settings.cpp  settings.ini: hotkey syntax, the ignore list, reading and writing the file
src/tray.cpp      which icon to show, and the Start with Windows setting
src/selftest.cpp  --selftest, with --no-windows to skip everything that opens a window
res/app.manifest  Per-Monitor-V2 DPI awareness, common controls v6, asInvoker
res/make-icon.ps1 regenerates res/displays-1.ico to res/displays-4.ico
```

The icons are committed so the build needs no extra tooling, and `res/make-icon.ps1` is the script
that produced them — they are not opaque binaries you have to take on trust. `displays-3.ico` is
also the exe's own icon.

## License

BSD 2-Clause. See [LICENSE.txt](LICENSE.txt).
