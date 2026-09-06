# Guest window mirroring

Status: **experiment, Phase 1 working.** Off by default.

| Pref | Effect |
|---|---|
| `toolbox_hooks true` | Required: the master switch for the whole Toolbox integration |
| `mdi_windows true` | Mirror each guest window into a host window |
| `window_redirect true` | Give each window its own offscreen buffer (see below). Off is the compatible mode |
| `native_alerts true` | Rebuild dialogs out of host controls instead of mirroring their pixels |

`toolbox_hooks` overrides the rest. With it false nothing in this subsystem touches guest
state, whatever the other prefs say: no trampolines, no `jGNEFilter` stub, no `MenuList`
poll, no `WindowList` walk, no queued Window Manager calls. Only the trap hooks proper are
gated by registration refusing; the periodic work is not a hook and has to check
`ToolboxTrap_HooksEnabled()` itself, which every entry point that runs from the 60 Hz drain
now does.

Each Mac OS window becomes a real host window, so a Classic application can wear native
chrome while its content stays authentically Classic. The long-term goal is app "packages":
boot straight into one application instead of the Finder.

This is the second client of the Toolbox trap registry, after
[`toolbox_menu.cpp`](../BasiliskII/toolbox_menu.cpp). See
[classic-menu-bar-compositor.md](classic-menu-bar-compositor.md) §4.5, which parked this
work until the menu bar was stable.

## Design: hooks are hints, the walk is the truth

A trap hook runs *before* the ROM routine it fronts, so hooking `_NewWindow` would fire at
the one moment the window does not yet exist. Instead `Toolbox_ProcessPendingWindowSync()`
walks `WindowList` on the IRQ path and diffs against the previous walk. Creation, move,
resize, retitle, show/hide and z-order all fall out of the diff, which keeps the hook
surface tiny:

| Trap | Why it must be a hook |
|---|---|
| `_CloseWindow` `$A92D`, `_DisposeWindow` `$A914` | The port must be restored while it is still intact |
| `_BeginUpdate` `$A922`, `_EndUpdate` `$A923` | Only with `window_redirect`: suspends `visRgn` re-assertion while the update region is deliberately narrowed |

```
60 Hz IRQ  ->  MenuQueue_Drain()          [SDL/menu_bar.cpp]
               Toolbox_ProcessPendingMenuBarSync()
               Toolbox_ProcessPendingWindowSync()   <- walk, diff, callbacks
VideoInterrupt() -> Toolbox_PresentWindows()        <- pixels to the host
```

## Reading the guest

Layouts are from the Apple SuperMario ROM sources.

- **`WindowList` (`$09D6`) is global, not per-process.** `ProcessMgr/LomemTab.Color.a:41`
  saves "toolbox globals except WindowList", so one walk sees every application's windows.
- **Layers group windows by application.** A layer record is a window whose port carries
  `txSize == $DEAD` (`Toolbox/WindowMgr/LayerMgr.c:1060`, `FastIsLayer`). The list is flat
  with each layer immediately followed by its own windows — `LayerMgr.c:1617` walks a
  layer's children from `layer->nextWindow` — so the most recently passed layer owns
  everything until the next one.
- **Geometry comes from region bounding boxes**, which are already global. `contRgn` is the
  blit rect; cropping to it is what lets the host draw its own title bar.
- **Trap numbers** are `OS/DispTable.a` offsets biased by `$A800`: `ToolBox $113,NewWindow`
  is `$A913`.

Two windows are deliberately excluded. The **desktop** is a window covering the whole
`GrayRgn`; it is latched by pointer on first sight, because its content region shrinks to
whatever part of the desktop is still uncovered as soon as anything opens on top of it.
Windows with an **empty content rect** are skipped and adopted on a later walk, once the
application has laid them out.

## Window types

A Mac OS window's procID says what kind of window it is, and the Window Manager keeps its
variation code — the low four bits — in the high byte of the `windowDefProc` handle, where
the handle itself only needs 24 bits. Each kind maps to the nearest host window:

| procID | Classic | Host window |
|---|---|---|
| 0 | `documentProc` | titled, closable, miniaturizable, resizable; zoom button disabled |
| 1 | `dBoxProc` | titled only, floating level — a modal dialog: not movable by its frame, not resizable, not closable |
| 2 | `plainDBox` | borderless, floating |
| 3 | `altDBoxProc` | borderless, floating (every macOS window has the shadow already) |
| 4 | `noGrowDocProc` | titled, miniaturizable, no resize, no zoom |
| 5 | `movableDBoxProc` | titled, floating |
| 8 | `zoomDocProc` | titled, miniaturizable, resizable, zoom enabled |
| 12 | `zoomNoGrow` | titled, miniaturizable, zoom enabled, not resizable |

The close box is not implied by the type — an application passes `goAwayFlag` to
`_NewWindow` separately — so it is taken from the window record instead.

Without this every window came out as a resizable document window, so alerts arrived with a
grow box and modal dialogs could be zoomed.

## Alerts are mapped, not mirrored

A `windowKind == dialogKind (2)` window is a `DialogRecord`, so its `'DITL'` item list can be
decoded (`Toolbox_SnapshotDialogItems()`): message text and button titles come out, with
item rectangles converted to global coordinates.

With `native_alerts true` the host then builds a **native alert** — a real `NSTextField` and
real `NSButton`s — rather than showing a picture of a Classic alert. Pressing a button does
not reach into `ModalDialog`; it drives the guest's own input path, so the Dialog Manager
sees exactly what a real click or keypress would produce:

- **Default button** → Return key, which the Dialog Manager maps to the default item itself.
  Needs no coordinates and works even when the item rects will not decode.
- **Any other button** → a synthetic click at the middle of the item's rectangle.

Both are spread over several interrupts. The guest samples input from its own polling loop,
so a press and release inside one tick can pass unnoticed; and a click must let the pointer
move land in the low memory globals via `ADBInterrupt` before the button goes down, or the
guest registers the press at the old location.

It is **off by default**. Rebuilding a dialog out of host controls reads better in isolation
but is inconsistent beside the windows that cannot be rebuilt — the item list has to decode,
and only some do — so one application ends up with two different-looking kinds of window.
Mirroring every window's pixels and letting the type mapping above supply the right host
frame is the more consistent result. Decoding still happens either way, because the item
rects are what a host click is mapped onto.

`COCKATRICE_AUTO_DISMISS_ALERTS=1` presses the default button of the first alert
automatically. This is how the startup "computer was not shut down properly" alert gets out
of the way unattended, and it is the seed of booting straight into an application.

## Input and window control

Two different paths, because they need two different things.

**Mouse and keys** are delivered as synthetic ADB events. The application sees exactly the
event sequence a real click or keystroke produces, and nothing has to execute guest code.
Key codes need no translation: macOS virtual key codes *are* the original ADB key codes.
Mouse coordinates arrive content-local and are converted to guest global; Cocoa's y runs up
from the bottom where the guest's runs down from the top, and the view is scaled to the
guest pixel size in case the host was resized before the guest caught up.

Hover is carried by an `NSTrackingArea`, not by `-mouseMoved:` alone. AppKit only delivers
`-mouseMoved:` to the key window, so moving over an unfocused mirrored window would leave
the guest cursor wherever it was last put — and the guest decides the cursor shape, and the
Finder its highlighting, from that position. `NSTrackingActiveAlways` gets the events
regardless of focus, and `-mouseEntered:` places the guest cursor the moment the pointer
arrives rather than on the first movement after that.

**Select, resize, move and close** call the real Window Manager routines. Synthesising a
title-bar drag or a grow-box drag for these was tried and works, but it only works where
there is something to aim at: a window with no grow box cannot be resized, and a window
whose title bar is covered on the guest screen cannot be selected. Calling
`_SelectWindow`, `_SizeWindow`, `_MoveWindow` and `_CloseWindow` has neither limit.

### Where those calls can be made from

Three contexts were tried. Two do not work, and the reasons are different:

- **The 60 Hz interrupt.** Calling QuickDraw (`_RectRgn`, `_InvalRect`) there killed the
  guest with a **Type 10 at `PC=$A0000000`** — the CPU jumped into the framebuffer. The
  interrupt can land anywhere, including inside QuickDraw's own non-reentrant code.
- **A trap hook on `_WaitNextEvent`.** An **Address Error about thirty seconds in**; the
  identical run without the hook was clean. Replacing that trap's address is not
  transparent, whatever the hook does.
- **`jGNEFilter` (`$029A`)**, the low-memory vector the Event Manager calls on the way out
  of `GetNextEvent` and `EventAvail`. This is an application's own context with no Toolbox
  call in progress, and it is where the system does this kind of work itself — the
  Notification Manager's filter draws in the menu bar from there
  (`Toolbox/NotificationMgr/NotificationMgrPatch.a:90`). This one works.

### Reaching the safe point is not enough

The obvious way to use it — an `EmulOp` in the filter stub that calls `Execute68k` — does
not work either, and the failure is quiet rather than fatal. Probes established the shape
of it exactly: from inside that `EmulOp`, a bare `RTS` returns, and `_FrontWindow` returns
a sensible `WindowPtr`, but `_BringToFront` and `_SizeWindow` **never return at all**.

The reason is that in System 7 those entry points belong to the Process Manager
(`Toolbox/WindowMgr/LayerMgr.c`), and moving a window between layers may yield to the
scheduler. A yield inside a nested `Execute68k` resumes a *different* process, so control
never comes back to the C++ frame that started the nested loop, and the emulator sits there
with the guest apparently alive and the flush half done.

So the `EmulOp` handler calls nothing. It **assembles** the pending operations into a short
68k routine in our arena and sets a flag; the stub then `JSR`s to that routine as ordinary
guest code:

```
        MOVEM.L D0-D7/A0-A6,-(SP)
        <EmulOp>                     ; assemble the routine, set the flag
        TST.B   (flag).L
        BEQ.S   chain
        CLR.B   (flag).L             ; one shot
        ST      (busy).L             ; a nested filter call must not overwrite it
        JSR     (scratch).L          ; the Window Manager calls, in guest flow
        CLR.B   (busy).L
chain:  MOVEM.L (SP)+,D0-D7/A0-A6
        MOVE.L  (oldFilter).L,-(SP)  ; chain to the filter that was there before
        TST.L   (SP)
        BNE.S   *+4
        ADDQ.L  #4,SP                ; no previous filter: return to the caller
        RTS
```

Now a yield is just a yield: the application blocks inside its own `GetNextEvent`, which is
where it was going anyway, and resumes normally afterwards. The assembled routine ends by
storing a sequence number, so the next pass can report that the guest really ran it.

Toolbox traps use the Pascal convention — the callee removes the arguments, as Apple's own
declarations say (`pascal void __CloseWindow(WindowPtr window)`, `LayerMgr.c:2044`) — so
each call is its arguments pushed followed by the trap word, with **no** stack adjustment
afterwards. Adding one pops the `RTS` address instead.

| Host action | Guest call |
|---|---|
| Window focused (`windowDidBecomeKey`) | `_SelectWindow` |
| Window resized (`windowDidResize`) | `_SizeWindow`, `fUpdate` true |
| Window dragged (`windowDidMove`) | `_MoveWindow`, `front` false |
| Close button (`windowShouldClose`) | `_CloseWindow` — returns `NO`, so the app can still ask about saving |
| Mouse move / down / up, key down / up | Synthetic ADB event at the translated position |
| Window redirected to a new buffer | `_InvalRect` over the content, in the window's own port |

Writing `updateRgn` directly is not enough to make an application repaint into a fresh
buffer: the Window Manager keeps its own idea of what is dirty, and some windows never come
back for an update event they were not properly told about. So a real `_InvalRect` is
queued as well, wrapped in `_GetPort`/`_SetPort` — it works on the current port, and this
runs inside the application's `GetNextEvent`, which will carry on drawing afterwards.


Geometry pushed *from* the guest sets `applyingSnapshot` around `setFrame:`, so the move and
resize notifications Cocoa raises in response are not sent straight back and the two sides
do not chase each other.

`COCKATRICE_WINDOW_SELFTEST` stands in for host input on a headless run. Its value selects
the calls to make on the largest window — `front`, `hilite`, `select`, `size`, `size0`,
`move`, `close`, comma-separated; anything else means `size,select` — and later walks report
whether the guest acted on them. This is how the three contexts above were told apart.

## Threading

The 68k CPU thread **is** the Cocoa main thread: `Start680x0()` is called from
`applicationDidFinishLaunching:`, so `[NSApp run]`'s loop is blocked and the main queue is
only serviced incidentally by `SDL_PollEvent()` inside `VideoInterrupt()`. So:

- Callbacks into the bridge are already on the right thread and call Cocoa directly.
- Cocoa event handlers run re-entrantly *inside* the 68k interrupt and must never call into
  the guest. They post to `MenuQueue` (`MENU_CMD_GUEST_WINDOW_CLOSE`,
  `MENU_CMD_GUEST_DIALOG_CLICK`) and the CPU thread acts at a safe point.
- Never `dispatch_sync` to the main queue from here; it deadlocks instantly.

## Where the pixels come from

Two modes, chosen by the `window_redirect` pref.

### `window_redirect false` (default) — read the screen

A window's content bounding box is global, so its rows are a sub-rectangle of the
framebuffer. Nothing in the guest is modified, which makes this the compatible mode: it
cannot upset an application however it draws. Its one real limitation is that **overlapping
windows share those pixels**, so a covered window shows whatever is on top of it.

### `window_redirect true` — give each window its own buffer

Each window's port is pointed at a private pixel buffer, so its pixels are always complete
and unobscured no matter what covers it on the guest screen.

The window's **own PixMap record is rewritten in place** rather than swapped for a
fabricated Handle. That is safe because every colour port gets its own PixMap — `OpenCPort`
calls `NewPixMap` for each (`QuickDraw/ColorAsm.a:155`) — and the handle is still compared
against the screen's `gdPMap` first, so a port that turns out to share the screen's PixMap
is left alone rather than redirecting every window at once. `pmTable` keeps pointing at the
same `CTabHandle` so colour matching stays consistent.

#### `bounds` must not be touched

Only `baseAddr` and `rowBytes` change. `bounds` looks like the obvious third field to
rewrite — set it to `portRect` and local (0,0) addresses the start of the buffer — but it
carries two meanings at once. QuickDraw addresses a pixel at local (h,v) as

```
baseAddr + (v - bounds.top) * rowBytes + (h - bounds.left) * depth
```

and the *same* field is the port's position on the screen: `_LocalToGlobal` subtracts
`bounds.topLeft`, which for a window is the negated position of its content. Setting
`bounds` to `portRect` makes the addressing come out right and tells the rest of the
Toolbox that the window is at global (0,0). The Window Manager then rebuilds `contRgn`
there, mouse coordinates land in the wrong place, and an application drawing through a
globally derived rect draws outside its own `visRgn` and appears to paint nothing at all —
which is what a blank mirrored window turned out to be.

So `bounds` is left alone and `baseAddr` absorbs the offset instead:

```
baseAddr = buffer + bounds.top * rowBytes + bounds.left * depth
```

Substituting into the addressing expression gives `buffer + v * rowBytes + h * depth`,
which is what we want, while every coordinate conversion in the system still works.
`baseAddr` then points *below* the buffer and is never dereferenced there; only the sum is.

Because `baseAddr` is re-derived from the current `bounds` on every pass rather than
remembered, a window that has been moved keeps addressing its buffer correctly without the
move having to be noticed separately. The same pass puts `baseAddr` and `rowBytes` back
whenever the Window Manager has recomputed the PixMap from the screen's, which showing,
hiding or moving a window does.

A new buffer is filled with white before use, so a window that has not repainted yet reads
as blank rather than as noise. In an indexed 8-bit port that is index **0** — the standard
Mac colour table puts white first and black last, so filling with `0xff` paints the window
solid black.

GWorlds are not an option: `_QDOffscreen` is absent from `OS/DispTable.a` — it is a
System-file addition, not in the ROM.

Buffers come from a private arena in the NuBus slot space above `MacFrameSize`, committed
with `memory_commit_range()`. The guest never allocates there, so however many windows are
open this cannot exhaust the System heap or move anything the Memory Manager tracks, and the
address is still guest-addressable.

**Do not call QuickDraw from the interrupt.** The first attempt used `_RectRgn`, `_SetPort`
and `_InvalRect` stubs, and the guest died with a Type 10 at `PC=$A0000000` — the CPU
jumped into the framebuffer. All of this runs from the 60 Hz interrupt, and QuickDraw is not
re-entrant, so calling into it while the guest is midway through its own drawing corrupts
QuickDraw's state. Both operations are done as direct memory writes instead:

- **`visRgn`** is set to the full `portRect` (local coordinates). The Window Manager
  recomputes it constantly — `CalcVis`, `PaintOne`, `ClipAbove` — clipping a window to
  whatever is not covering it, which is wrong for a window that owns its buffer. It is
  re-asserted once per interrupt only when it has actually drifted: cheaper and more robust
  than hooking every trap that touches it. The exception is between `_BeginUpdate` and
  `_EndUpdate`, where the narrowing is deliberate and overwriting it would let the
  application draw outside the area it was asked to refresh.
- **`updateRgn`** is set to the content rect (global coordinates) when a window is adopted,
  so the application repaints into the new buffer at its next update event.

A region is a ten byte record — a size word and a bounding box — so making one rectangular
is a handful of writes. The handle's memory block keeps its original size, which is
harmless; only the region's own length field shrinks.

**Colour ports only.** A classic 1-bit `GrafPort` keeps its bitmap inline rather than behind
a PixMap handle, so it needs its own path; such windows fall back to reading the screen. In
practice this catches some system alerts, which are B&W.

## Debugging

`COCKATRICE_WINDOW_DUMP=<dir>` writes each mirrored window's presented pixels to a binary
PPM once, a couple of seconds after adoption, expanded through the guest palette. Convert
with `sips -s format png window_XXXXXXXX.ppm --out out.png`. This is the quickest way to
confirm that what reaches the host really is that window's content.

## Files

| File | Role |
|---|---|
| `BasiliskII/toolbox_window.cpp` / `include/toolbox_window.h` | Portable: walk, diff, DITL decode, synthetic input, the `jGNEFilter` safe point |
| `BasiliskII/bridge/darwin/macos_window_bridge.mm` / `.h` | macOS: one `NSWindow` per guest window, native alerts |
| `BasiliskII/SDL/menu_bar.cpp` | Drives the sync from the IRQ path; new queue commands |
| `BasiliskII/SDL/video_sdl.cpp` | Presentation call site; records the palette |

## Known limitations

1. With `window_redirect false`, overlapping windows share framebuffer pixels.
2. With `window_redirect true`, an application that caches the screen base address, or
   draws straight to the screen rather than through its port, will misbehave. This is why
   it is a separate switch. B&W `GrafPort` windows are not redirected.
3. Window operations are one routine at a time: while the guest is running the assembled
   routine the queue is left alone, so a burst of host resizes collapses into the last one.
4. Desk accessories (`windowKind < 0`) are not mirrored.
5. `rDocProc` (procID 16) cannot be told apart from `documentProc`. The window record keeps
   only the variation code, not the WDEF resource ID, and those two differ in the ID.
6. Absence from `WindowList` is not treated as destruction. A background process's windows
   leave the list for seconds at a time while the Process Manager builds a new layer, and
   retiring on that tears down and rebuilds every host window — which produces a burst of
   focus and geometry notifications from Cocoa that go back to the guest, and the whole
   thing starts again. A genuine close arrives promptly through the `_CloseWindow` and
   `_DisposeWindow` hooks; a window disposed some other way is only cleaned up after ten
   seconds of absence.
7. On macOS one process can own only one Dock icon, so per-application Dock tiles are not
   achievable; windows are grouped by layer instead.
