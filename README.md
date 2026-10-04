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
Windows itself, and needs no installer — put it anywhere, or drop a shortcut in
`shell:startup` to have it run at login.

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
sha256sum -c winswapper-1.01-x64.zip.sha256
```

## Use

Run with no arguments and it goes to the notification area. Right-click the icon for *Rotate left*,
*Rotate right*, *Open log*, *About* and *Exit*; double-click it to rotate left.

The two hotkeys are registered independently, so if another application has claimed one of them the
other still works — and the tray menu always does.

There are also four command-line modes, which print to the console they were launched from and to
the log:

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

`--selftest` exits 0 when every check ran and passed, 3 when it passed everything it could but
skipped the window-move tests for want of a second display, and 1 on a real failure. Usage errors
exit 2 in every mode. The rotation arithmetic is checked regardless of displays, which is why CI
runs it on a single-display runner.

The log is at `%LOCALAPPDATA%\WinSwapper\winswapper.log`.

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

## Known limitations

- **Elevated windows cannot be moved.** A process running as you cannot reposition a window owned
  by a process running as administrator; Windows blocks it (UIPI). Those windows are skipped and
  counted, and a tray balloon reports how many. Running WinSwapper itself as administrator would
  fix it, at the cost of a UAC prompt every launch.
- **Owned windows are skipped.** The filter matches Alt-Tab semantics, so the occasional
  application whose main window is *owned* by another window will be left alone.
- **Only the current virtual desktop is touched.** A deliberate consequence of the cloak filter.
- **Re-maximizing activates.** Because a maximized window has to be restored and re-maximized,
  the last maximized window processed ends up focused, and there is a brief flash as it moves.
- **Different DPI per display.** The app is Per-Monitor-V2 aware, so coordinates are real physical
  pixels. If displays run different scaling factors, applications will re-layout their contents on
  arrival, which the proportional mapping accounts for but cannot make invisible.
- **The hotkeys are not configurable.** Ctrl+Alt+S and Ctrl+Alt+Shift+S are compiled in. If another
  application owns one, the tray menu is the fallback.
- **Displays are matched by position, not identity.** Plugging in or unplugging a display changes
  the ordering, so a rotation begun on one arrangement and finished on another will not round-trip.
  This is why the two directions only cancel out while the display layout stays put.

## Layout

```
src/main.cpp      WinMain, hidden window, tray icon, hotkey, menu, message loop
src/swapper.cpp   display and window enumeration, remap arithmetic, applying moves
src/log.cpp       log file plus console output for the command-line modes
res/app.manifest  Per-Monitor-V2 DPI awareness, common controls v6, asInvoker
res/make-icon.ps1 regenerates res/app.ico
```

`res/app.ico` is committed so the build needs no extra tooling, and `res/make-icon.ps1` is the
script that produced it — the icon is not an opaque binary you have to take on trust.

## License

BSD 2-Clause. See [LICENSE.txt](LICENSE.txt).
