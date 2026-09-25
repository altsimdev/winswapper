# WinSwapper

A small Win32 tray utility. It waits for a global hotkey, and when fired it exchanges every
ordinary application window on one display with those on the other — keeping each window's size
and its position within its display.

Press **Ctrl+Alt+S**. Press it again to put everything back.

## Build

Requires Visual Studio 2026 (v18) with the C++ workload. Open `winswapper.sln` and build, or from
a shell:

```bash
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" winswapper.sln /p:Configuration=Release /p:Platform=x64
```

The binary lands in `build\x64\Release\winswapper.exe`. It has no runtime dependencies beyond
Windows itself, and needs no installer — put it anywhere, or drop a shortcut in
`shell:startup` to have it run at login.

Targets `PlatformToolset v145` and Windows SDK `10.0.26100.0`. To retarget, edit those two
properties in `winswapper.vcxproj`.

## Use

Run with no arguments and it goes to the notification area. Right-click the icon for *Swap now*,
*Open log*, *About* and *Exit*; double-click it to swap.

There are also four command-line modes, which print to the console they were launched from and to
the log:

| Command | What it does |
|---|---|
| `winswapper.exe` | Run in the notification area (the normal way to use it) |
| `winswapper.exe --list` | Show the displays and **every** top-level window, with the reason each one was included or excluded |
| `winswapper.exe --dry-run` | Compute the whole swap and print it — moves nothing |
| `winswapper.exe --swap` | Perform one swap and exit (bind this to anything you like) |
| `winswapper.exe --selftest` | Verify the remap arithmetic and the three window-move paths |

The log is at `%LOCALAPPDATA%\WinSwapper\winswapper.log`.

## How it works

**Choosing the two displays.** Displays are enumerated and sorted left-to-right. Device-name
suffixes are *not* reliable display numbers — this machine reports `\\.\DISPLAY5` and
`\\.\DISPLAY6` — so ordering is by geometry. With exactly two displays the swap is symmetric, so
which one you call "1" makes no difference. With more than two, the two leftmost are swapped.

**Choosing the windows.** A window is swapped only if it is visible, unowned, not a tool window,
not DWM-cloaked, not a shell window, and not exclusive-fullscreen. The cloak check is the one that
matters most: without it, windows belonging to *other virtual desktops* would get hauled onto the
current one.

**Moving them.** Each window's rect is expressed as a fraction of its source display's work area
and reapplied to the destination's, so layouts survive displays of different sizes. When the two
work areas are the same size every scale factor is exactly 1.0, the mapping degenerates to an
integer translation, and swapping twice restores every window to its original pixel.

A window that merely overhangs its display is left exactly where the arithmetic puts it — only one
that would land completely off-screen is rescued. Nudging overhanging windows inward would fight
the invisible resize border that `GetWindowRect` reports on Windows 10/11 and would quietly make
each swap lossy instead of reversible.

The three window states each need different treatment:

- **Normal** — one `SetWindowPos` with `SWP_ASYNCWINDOWPOS`, so a hung application cannot stall
  the rest of the swap.
- **Maximized** — un-maximize, move, re-maximize. `SetWindowPlacement` alone does *not* work here:
  Windows leaves a maximized window on its current display and merely rewrites its restore rect.
  The self-test covers this.
- **Minimized** — `SetWindowPlacement` with the remapped restore rect and `showCmd` left alone, so
  the window stays minimized but reappears on the other display. `WPF_RESTORETOMAXIMIZED` is
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
  pixels. If the two displays run different scaling factors, applications will re-layout their
  contents on arrival, which the proportional mapping accounts for but cannot make invisible.

## Layout

```
src/main.cpp      WinMain, hidden window, tray icon, hotkey, menu, message loop
src/swapper.cpp   display and window enumeration, remap arithmetic, applying moves
src/log.cpp       log file plus console output for the command-line modes
res/app.manifest  Per-Monitor-V2 DPI awareness, common controls v6, asInvoker
res/make-icon.ps1 regenerates res/app.ico
```
