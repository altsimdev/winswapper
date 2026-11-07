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

It also moves and resizes single windows the way many Linux desktops do: hold **Alt** and drag
anywhere in a window — with the left mouse button to move it, with the right to resize it. No need to
find its title bar or its border, and its edges snap to the edges of the screen. Alt and a click of
the middle button sends a window behind all the others.

## Build

Requires Visual Studio with the "Desktop development with C++" workload. Open `winswapper.sln` and
build, or from a Developer Command Prompt / Developer PowerShell:

```bash
msbuild winswapper.sln /p:Configuration=Release /p:Platform=x64
```

The binary lands in `build\x64\Release\winswapper.exe`. It needs nothing beyond Windows 10 or 11
and no installer — put it anywhere. To have it run at sign-in, turn on *Start with Windows* in its
tray menu.

That holds because of how the runtime is linked. The Visual C++ runtime is built into the exe, while
the C runtime it uses is the Universal CRT that ships with Windows 10 and 11 — the "hybrid CRT", set
up in `winswapper.vcxproj`. Releases up to 1.06 left this at Visual Studio's default instead, and
needed the Visual C++ Redistributable installed: on a clean Windows install they would not start.

The project does not pin a toolset or an SDK: it asks for `$(DefaultPlatformToolset)` and Windows
SDK `10.0`, which resolve to whatever that machine has installed (`v145` on Visual Studio 2026,
`v143` on 2022). Nothing here needs more than C++17. To pin an exact pair, pass them on the command
line:

```bash
msbuild winswapper.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.22621.0
```

`.github/workflows/build.yml` builds both configurations on `windows-latest` for every push and
pull request, smoke-tests the binary, and uploads it as an artifact. Before running it, the smoke
test lists the DLLs the exe imports and fails on any that do not ship with Windows. Simply running
it there would prove nothing: GitHub's build machines have the Visual C++ Redistributable, which is
how the problem above went unnoticed. To check a build locally:

```bash
pwsh .github/check-dependencies.ps1 -Exe build\x64\Release\winswapper.exe
```

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
redraws itself when you plug a display in or take one away. Hovering over it shows the exact count,
the hotkeys and what the mouse does with Alt held.

*Start with Windows* is off until you turn it on. Ticking it registers this copy of the exe to start
when you sign in, for your user only and without administrator rights; it then also appears in Task
Manager's *Startup apps*. The checkmark reflects what will actually happen: it is clear if the entry
was switched off in Task Manager, or if it points at a different copy of the exe, and ticking it then
puts that right.

The two hotkeys are registered independently, so if another application has claimed one of them the
other still works — and the tray menu always does.

### Dragging

Hold Alt and press the left mouse button anywhere in a window — not just on its title bar — then
drag: the window comes to the front and follows the pointer until you let go. A maximized window
first returns to its normal size, under the pointer, as it does when you drag its title bar.

Press the right button instead to resize the window: the corner or side nearest the pointer follows
it, and the opposite ones stay put. Think of the window as cut into thirds each way: press in a
corner third and that corner moves; in the middle third of a side, just that side; in the very
middle, the nearest corner. The window stops at its smallest size, as when you drag its border. A
maximized window first becomes an ordinary one filling the screen, and is resized from there.
Windows without a sizing border — most dialogs — keep their size.

Either way, a window brought within 10 pixels of the edge of a display — or of the taskbar — snaps
flush against it: moved, the whole window jumps the last few pixels; resized, the side being dragged
does. It goes by the edge you can see: Windows 10 and 11 give most windows an invisible border,
several pixels wide on the left, right and bottom, to grab them by, and that border then hangs over
the edge. Between two displays, a window snaps to either side of the line where they meet.

Click the middle button — the wheel — with Alt held, and the window goes behind every other window,
uncovering whatever it was hiding, without being minimized. If it had the keyboard, the window now
under the pointer gets it, as on KDE. A window kept on top of others goes behind only those that are
kept on top too, and stays in front of the rest.

An Alt+click or Alt+right-click that doesn't move still reaches the program, delivered when the
button comes up, so the right-click menu still opens; an Alt+middle-click never does. Only Alt on its
own counts: with Shift or Ctrl held as well, the drag belongs to the program, so Alt+Shift+drag still
selects a column in an editor that has one. The desktop, the taskbar, full-screen games and videos,
windows that have stopped responding and windows of programs running as administrator are left alone,
and their Alt+clicks go through untouched.

The keys for moving, resizing and lowering can each be changed or turned off in the settings, and so
can snapping — and programs with an Alt+drag of their own can be left out.

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

[Drag]
Move   = Alt
Resize = Alt
Lower  = Alt
Snap   = 10
Skip   = notepad++.exe
```

- **Hotkeys** are modifiers and a key joined with `+`. Modifiers are `Ctrl`, `Alt`, `Shift` and
  `Win`, and at least one of `Ctrl`, `Alt` or `Win` is required, so a hotkey can never swallow
  ordinary typing. Keys are `A`–`Z`, `0`–`9`, `F1`–`F24`, `Left`, `Right`, `Up`, `Down`, `Home`,
  `End`, `PageUp`, `PageDown`, `Insert`, `Delete`, `Space`, `Tab`, `Enter` and `Esc`. `none` turns a
  hotkey off.
- **Ignore** lists programs whose windows stay put when the others rotate, one per line, named as in
  the *Details* tab of Task Manager; `.exe` may be left off. Everything else rotates around them.
  `--list` shows each ignored window and the line that caught it. Dragging still moves them.
- **Drag** has five settings. `Move` is the keys held while dragging with the left button to move
  a window, `Resize` the keys held while dragging with the right to resize one, and `Lower` the keys
  held while clicking the middle button to send one to the back: modifiers joined with `+`, as for a
  hotkey but with no key, again including at least one of `Ctrl`, `Alt` or `Win` — programs already
  use Shift with the mouse. Exactly those keys must be held, no more. `none` turns any of them off.
  `Snap` is how near, in pixels, an edge has to come to snap: a whole number from `0` to `100`, at
  100% display scaling and larger in proportion above it; `0`, `off` or `none` turns snapping off.
  `Skip` names a program whose windows dragging leaves alone, so that its own drag with the same keys
  keeps working — column selection in Notepad++ or Visual Studio, or sizing the brush in Photoshop,
  say. Give one `Skip` line per program, or several programs on one line separated by commas, named
  as under `[Ignore]`. A setting left out keeps its default, so a file written by an earlier version
  — without `Lower` and `Snap`, or with no `[Drag]` section at all — gets Alt for each, and snapping
  within 10 pixels.

A mistake affects only its own line: a bad hotkey or drag setting keeps its default, and anything
unusable is reported with its line number in the log and in a notification, while the rest of the
file still applies. The file may be saved as UTF-8 (with or without a byte-order mark) or UTF-16.
The command-line modes read it too, so `--help` shows the hotkeys and drag keys actually in use.

There are also four command-line modes, which print to the console they were launched from and to
their own log file (see below):

| Command | What it does |
|---|---|
| `winswapper.exe` | Run in the notification area (the normal way to use it) |
| `winswapper.exe --list` | Show the displays and **every** top-level window, with the reason each one was included or excluded |
| `winswapper.exe --dry-run` | Compute the whole rotation and print it — moves nothing |
| `winswapper.exe --rotate` | Perform one rotation and exit (bind this to anything you like) |
| `winswapper.exe --selftest` | Verify the rotation mapping, the remap arithmetic, the three window-move paths, and dragging, snapping and lowering |

`--reverse` (or `--right`) added to `--rotate` or `--dry-run` rotates right instead of left. Those
are the only modes it applies to; combined with anything else, or given alone, it is an error rather
than being silently ignored. `--swap` is accepted as a synonym for `--rotate`, since that was its
name before 1.01.

`--selftest` opens test windows, which takes the focus for a few seconds: one that it drags, resizes,
maximizes, snaps into a corner and sends to the back on the main display, with two more for it to go
behind, then — given a second display — more that it moves between your displays. The drag test
feeds the window made-up mouse events, so the real pointer never moves.
`--selftest --no-windows` skips just those tests: the rotation arithmetic, the icons, *Start with
Windows* (exercised in a scratch registry key), the drag logic on made-up mouse events and the remap
round trips all still run, and nothing on screen is disturbed. Like `--reverse`, `--no-windows` is
an error with any other mode.

`--selftest` exits 0 when every check ran and passed, 3 when it passed everything it ran but skipped
the window-move tests — because of `--no-windows`, or for want of a second display — and 1 on a
real failure. Usage errors exit 2 in every mode. The rotation arithmetic and the drag test need no
second display, which is why CI runs them on a single-display runner.

Logs are in `%LOCALAPPDATA%\WinSwapper\`:

- `winswapper.log` — the tray app's log, which *Open log* shows. It accumulates across runs; once it
  passes 1 MB it is renamed to `winswapper.old.log` and a fresh one begins. That is checked as it
  runs, not only at start-up, since with *Start with Windows* on it can run for weeks. When Windows
  signs out, shuts down or restarts, or an installer asks the app to close, the log says so as it
  goes; an exit from the menu ends with `WinSwapper exited.` A log that stops without either means
  the app was ended some other way — from Task Manager, say. Up to 1.09 a sign-out was not logged,
  and the log just stopped.
- `winswapper-cli.log` — the latest command-line run only. It is a separate file because the tray
  app keeps its own log open while it runs, which used to lock command-line runs out of a shared one
  and lose their output.

If a log cannot be opened — another program holding the file, say — nothing is lost and nothing
fails quietly. What would have been written is kept in memory (up to 256 KB), and the file is tried
again with every new line — and every 10 seconds, if the tray app could not open it at start-up,
since an idle tray app may log nothing for hours. Once it opens, a note comes first
saying since when it could not be opened and the Windows error that stopped it, then the kept lines
with their original times. Until then *About* says the log is not being written and why, and *Open
log* says so before showing the file. Up to 1.07 the log was tried only once, at start-up, so a
failure there left the whole run unlogged without a word.

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

**Choosing the windows.** A window is moved only if it is visible, not a tool window, not
DWM-cloaked, not a shell window, and not exclusive-fullscreen. The cloak check is the one that
matters most: without it, windows belonging to *other virtual desktops* would get hauled onto the
current one.

Dialogs, floating toolbars and palettes are *owned* windows: each belongs to an application window,
its owner. An owned window moves only when the window at the top of its owner chain moves too, so a
dialog goes wherever its application goes — an open *File Open* dialog included, even while it
blocks the window behind it — and stays wherever its application stays, ignored or not. That is
decided once every window has been collected, because a dialog is stacked above its owner and so is
reached first. Owned windows may be tool windows; an unowned tool window is never moved. Like every
other window, an owned one rotates from the display it is on, so a dialog dragged onto a different
display from its application stays one display apart from it.

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

A window's own dialogs are no anchor for it. Windows keeps an owned window above its owner, so the
activation took them to the front with it, and putting the owner back takes them back too. The
self-test checks this with a maximized window whose dialog is open, under a normal window that
covered both.

**Dragging.** A low-level mouse hook, on a thread of its own, sees each mouse event before any
program does. When the left button goes down with exactly the `Move` keys held, or the right with
exactly the `Resize` keys, over a window that may be dragged, the hook keeps the press from the
program. If the pointer then moves further than Windows' own drag threshold (`SM_CXDRAG` and
`SM_CYDRAG`, a few pixels), it is a drag: the window comes to the front and follows the pointer — or,
for a resize, the corner or side grabbed does — measured from where the button went down. If not,
then when the button comes up the press and release are sent again with `SendInput`, marked so that
the hook lets them through, and the program gets its click after all. Mouse moves are never held
back, so the pointer itself never stutters. A press of the other button in the middle of a drag is
kept from the program too, release and all.

Every mouse event on the machine waits for the hook, and Windows quietly removes a hook that is slow
to answer, so the hook never waits on anything. Each drag gets a thread that moves or resizes the
window and applies only the newest position each time it is ready for another, so a slow or hung
program holds up its own window and nothing else. Finding the window under the pointer
(`WindowFromPoint`) and reading its title for the log (`InternalGetWindowText`) send it no messages.

A resize sets the new size with one `SetWindowPos`, never smaller than Windows' own minimum
(`SM_CXMINTRACK` and `SM_CYMINTRACK`) unless the window started out smaller. A program may insist on
a larger minimum of its own, and Windows then keeps the window that size — growing it from its
top-left corner, which would push it right or down when its left or top side is the one being
dragged. So the thread checks where the window ended up and puts its far side back; from then on in
that drag it asks for no less than the program allowed, so the window doesn't flicker back and forth.

Snapping adjusts each position before it is applied. An edge of the window within the `Snap`
distance of the same edge of a display's work area — left to left, top to top — is moved onto it;
when both sides are in reach, the nearer one wins. Only a display beside the window counts: its left
and right edges if the window overlaps it from top to bottom, its top and bottom if it overlaps it
across, so a display off at a diagonal has no pull. The work areas, which leave out the taskbar, are
read once per drag. The edges compared are the visible frame's, from DWM
(`DWMWA_EXTENDED_FRAME_BOUNDS`), rather than `GetWindowRect`'s, which take in the invisible border.
A maximized window hides its borders off the screen, so for one being un-maximized they are read
only once it has left its maximized size. The distance is scaled by the system DPI, so `10` is 15
pixels at 150%.

An Alt+middle-click does its work as the button goes down, and the press and release are both kept
from the program. The window at the top of the clicked window's owner chain — the application, if a
dialog was clicked — is placed beneath the lowest window showing in its layer, the always-on-top one
or the other, that is neither its own nor the desktop's. That is `SetWindowPos` with that window to
go beneath, rather than `HWND_BOTTOM`: a window placed beneath another takes the windows it owns
along, as the stacking repair above relies on, and an always-on-top window stays one. Its program
has to answer for that, so it is done on a thread of its own, never the hook's. If the window had
the keyboard, the thread then gives it to the window under the pointer — or that window's open
dialog — and the log says if Windows would not allow it.

Alt and Win each do something on their own when let go: Alt opens the window's menu bar, Win opens
Start — unless a key or click came in between. The program never saw the click, so while the key is
down the hook taps a key that does nothing (`0xE8`, which Windows leaves unassigned), and letting go
opens nothing.

A window is not dragged when it is the desktop or the taskbar, not showing, an unowned tool window,
cloaked, full screen (covering its display without being maximized: games, videos, presentations),
not responding, or run as administrator — Windows would refuse to move it, and by then its click
would be gone. Nor is one whose program is on the `Skip` list, and a window without a sizing border
(`WS_THICKFRAME`) is not resized. The press goes through untouched, and the log says why. The same
goes for sending a window to the back.

## Known limitations

- **Elevated windows cannot be moved.** A process running as you cannot reposition a window owned
  by a process running as administrator; Windows blocks it (UIPI). Those windows are skipped and
  counted, and a tray balloon reports how many. Dragging leaves them alone too. Running WinSwapper
  itself as administrator would fix it, at the cost of a UAC prompt every launch.
- **Alt+drag is WinSwapper's.** While dragging is on, a program with an Alt+drag, Alt+right-drag or
  Alt+middle-click of its own — column selection in Notepad++, Visual Studio or Word, duplicating in
  some graphics programs, sizing the brush in Photoshop, orbiting, panning or zooming in some 3D
  programs — no longer gets it. Add the program to `Skip`, or choose other keys. Alt+Shift+drag and
  the like still reach programs.
- **An Alt+click arrives late.** An Alt+click or Alt+right-click reaches the program only when the
  button comes up, as a quick press and release where the button came up, so an Alt+press-and-hold
  does nothing in the program.
- **Snapping is to displays only.** A dragged window snaps to the edges of the displays and the
  taskbar, but not to other windows. Nor are there Snap layouts, or a window filling half the screen
  when dragged against its side: Windows keeps those for dragging by the title bar.
- **Resizing skips a program's size steps.** A program can adjust a size while you drag its border
  — a terminal to whole character cells, a video player to its picture's shape — but Windows asks it
  only while that border is being dragged. A resize with Alt+right-drag sets each size directly, so
  such a program gets the size asked for and fits itself in as best it can. Minimum sizes are still
  respected.
- **Windows owned by a hidden window are skipped.** A few applications, mostly older ones, make
  their main window *owned* by a hidden one. With nothing visible to travel with, those windows are
  left alone.
- **A minimized window's dialogs stay behind.** While a window is minimized, Windows hides the
  dialogs it owns, so they cannot be told apart from windows hidden for good and are not moved.
  Restoring the window brings them back on the display they were on.
- **Only the current virtual desktop is touched.** A deliberate consequence of the cloak filter.
- **Maximized windows flash as they move.** A maximized window has to be restored, moved and
  re-maximized, so it briefly appears at its restored size on the way. Its stacking position and
  the focus are put back afterwards.
- **Stacking is restored among moved windows only.** A window that is not moved — a tool window,
  say — keeps its place, so if one sat between two maximized windows it can end up on the other
  side of one of them.
- **Different DPI per display.** The app is Per-Monitor-V2 aware, so coordinates are real physical
  pixels. If displays run different scaling factors, applications will re-layout their contents on
  arrival, which the proportional mapping accounts for but cannot make invisible. A window dragged
  across to such a display may change size as it arrives, and the pointer's grip on it shift.
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
src/drag.cpp      the mouse: the hook, which presses to take, moving, resizing, snapping and lowering
src/log.cpp       log files plus console output for the command-line modes
src/settings.cpp  settings.ini: hotkey and drag-key syntax, program lists, reading and writing the file
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
