# Classic Menu Bar Compositor (Future)

Design note for restoring a **Classic Mac OS** look and feel in Cockatrice III
by **drawing the menu bar in a host layer** instead of mirroring guest menus
into the native macOS (or Windows) system menu bar.

This document describes work we may land later. It builds on the **Modular
Toolbox Traps** subsystem already in the tree (`toolbox_traps.cpp`,
`M68K_EMUL_OP_TOOLBOX_DISPATCH` / `0x7130`).

**Primary sources today**

| Area | File |
|------|------|
| Toolbox trap registry & dispatch | `BasiliskII/toolbox_traps.cpp`, `BasiliskII/include/toolbox_traps.h` |
| Menu Manager hooks (registry client) | `BasiliskII/toolbox_menu.cpp`, `BasiliskII/include/toolbox_menu.h` |
| EmulOp routing | `BasiliskII/emul_op.cpp`, `BasiliskII/include/emul_op.h` |
| Guest menu snapshot (MenuList decode) | `Toolbox_SnapshotMenuBar()` in `toolbox_menu.cpp` |
| macOS NSMenu bridge (current approach) | `BasiliskII/bridge/darwin/macos_menu_bridge.mm` |
| SDL video blit & input | `BasiliskII/SDL/video_sdl.cpp` |
| Thread-safe menu commands | `BasiliskII/SDL/menu_bar.cpp`, `BasiliskII/include/menu_bar.h` |
| Inside Mac Menu Manager reference | Menu Manager chapter (MenuInfo, `hMenuCmd`, low-mem globals) |

---

## 1. Goal

Give the emulator a **single composited window** that looks like Classic Mac OS:

- The **menu bar** (Apple, File, Edit, …) is drawn by the **host** from guest
  `MenuList` state, not by ROM `_DrawMenuBar` into the framebuffer and not as
  native `NSMenu` items in the macOS menu bar.
- The **guest desktop** (Finder, apps, windows) continues to render into the
  NuBus framebuffer (`0xA0000000`) and is blitted **below** the host menu strip.
- **Mouse and keyboard** events pass through to the guest unchanged, except
  when the cursor is over the host-drawn menu bar (or an open host-drawn
  dropdown), where the host handles hit-testing and menu selection.

The user sees one Classic-style surface; input routing is transparent.

---

## 2. Why not the current NSMenu bridge?

The existing path hooks Menu Manager traps in **passthrough** mode
(`ToolboxMenu_RegisterTraps()` in `toolbox_menu.cpp`), snapshots guest
`MenuList` (`0x0A1C`), and rebuilds `NSApp.mainMenu` on the macOS main thread
(`macos_menu_bridge.mm`, wired in as the sync callback). That works for driving
the guest from the system menu bar but:

- Guest `_DrawMenuBar` still paints into the framebuffer → **double menu** unless
  cropped or suppressed.
- Appearance is **native macOS**, not Chicago / Platinum.
- Coordinate space splits between system menu bar and SDL window.

The compositor approach keeps trap hooks and snapshot logic; only the **output
path** changes (draw into a layer vs. build `NSMenu`).

---

## 3. Architecture overview

```mermaid
graph TB
    subgraph guest [Guest 68k]
        APP[Application / Finder]
        MM[Menu Manager ROM]
        FB[NuBus framebuffer 0xA0000000]
    end

    subgraph traps [Toolbox trap hooks 0x7130]
        DMB["_DrawMenuBar → REPLACE"]
        STATE["_InsertMenu / _SetMenuBar / … → passthrough + sync"]
        SEL["_MenuSelect / _HiliteMenu → optional hooks"]
    end

    subgraph host [Host compositor]
        SNAP[Toolbox_SnapshotMenuBar]
        DRAW[HostMenuBar_Draw]
        ROUTE[Input router in doevents]
        BLIT[VideoInterrupt: guest blit with Y crop]
    end

    APP --> MM
    MM --> traps
    traps --> SNAP
    SNAP --> DRAW
    FB --> BLIT
    DRAW --> SDL[SDL surface / future layer]
    BLIT --> SDL
    ROUTE -->|menu bar hit| Toolbox_DispatchGuestMenuSelect
    ROUTE -->|else| ADB[ADB passthrough]
```

**Layer layout (phase 1)**

```
┌──────────────────────────────────────────┐
│ Host menu bar (~20 px, LM_MBarHeight)    │  ← drawn from MacMenuBarSnapshot
├──────────────────────────────────────────┤
│ Guest desktop (framebuffer, Y cropped)   │  ← existing VideoInterrupt blit
└──────────────────────────────────────────┘
```

---

## 4. Toolbox trap strategy

The registry API is in `toolbox_traps.h`:

- `ToolboxTrap_Register(trap, name, handler, user_data)`
- `ToolboxTrap_InstallAll()` — writes the trampolines into guest RAM. Called once
  per boot from `PatchAfterStartup()` (Sony accRun), after the System file has
  installed its own trap patches, so our stub sits at the head of the chain.
  Gated on the `toolbox_hooks` pref, which is off by default.
- `ToolboxTrap_HooksEnabled()` — that same pref, exported. It is the master
  switch for everything built on the registry, not only for registration: the
  `MenuList` poll (§4.3.1) and the `jGNEFilter` stub run from the 60 Hz drain
  rather than from a trap, so they check it themselves. With it false a boot
  differs from an unpatched one only in what it prints.
- Handler returns `TOOLBOX_ACTION_PASSTHROUGH` or `TOOLBOX_ACTION_REPLACE`
- `ToolboxArgs` — Pascal stack helpers for replace-mode handlers

### 4.1 Menu drawing traps (suppress guest pixels)

| Trap | Name | Future action |
|------|------|----------------|
| `0xA937` | `_DrawMenuBar` | **REPLACE** — set host menu dirty flag; do not run ROM |
| `0xA934` | `_ClearMenuBar` | Passthrough or REPLACE + clear host snapshot |
| `0xA81D` | `_InvalMenuBar` | Invalidate host menu rect only |
| `0xA938` | `_HiliteMenu` | Update highlighted menu title on host layer |

### 4.2 Menu state traps (keep sync, no native NSMenu)

| Trap | Name | Future action |
|------|------|----------------|
| `0xA930` | `_InitMenus` | Passthrough + `Toolbox_RequestMenuBarSync()` |
| `0xA935` | `_InsertMenu` | Same |
| `0xA936` | `_DeleteMenu` | Same |
| `0xA93C` | `_SetMenuBar` | Same |
| `0xA933` | `_AppendMenu` | Same |
| `0xA826` | `_InsertMenuItem` | Same |
| `0xA827` | `_DeleteMenuItem` | Same |

Deferred sync (already implemented): trap pre-hook sets
`Toolbox_RequestMenuBarSync()`; next IRQ runs
`Toolbox_ProcessPendingMenuBarSync()` so ROM finishes updating `MenuList`
before snapshot.

### 4.3 Making a host menu choice take effect

There is no Toolbox call that *performs* a menu command. `_MenuSelect` and
`_MenuKey` only report which item the user picked; the application acts on the
result, in its own event loop. So the host cannot dispatch a menu choice by
calling something — it has to get the application to ask, and then answer.

| Trap | Name | Use |
|------|------|-----|
| `0xA93D` | `_MenuSelect` | **Hooked, `TOOLBOX_ACTION_REPLACE`.** With a host choice pending, returns that `menuResult` instead of tracking the mouse. Otherwise passthrough, so the guest's own menu bar still works |
| `0xA93E` | `_MenuKey` | Left to the guest: command-key equivalents are handled by the application |

`Toolbox_DispatchGuestMenuSelect(menuID, itemIndex)` records
`(menuID << 16) | itemIndex`. The mouseDown that makes the application ask is
then written **straight into the event record** the Event Manager is about to
return, from the `jGNEFilter` safe point — `Toolbox_MenuSafePoint()`. The filter
is entered with `A1` pointing at that record and the Boolean `GetNextEvent` will
return sitting above the return address, both documented at
`ToolboxEventMgr.a:265-280`, so the event and the "there is an event" answer can
be supplied together.

Only a **null event** is overwritten, so no real event is ever lost; null events
are frequent enough that the wait is a frame or two. An unanswered choice is
discarded after five seconds, so it cannot be picked up by a later real
selection.

Two details decide whether this works at all:

- **The Boolean goes in the high byte.** `GetNextEvent`'s result occupies a word
  on the stack, and the value is the byte at its *low address*: `GNECommon`
  builds it with `CLR.W` followed by `ADDQ.B #1` at the same address
  (`ToolboxEventMgr.a`), and `WaitNextEvent` reads it back with
  `MOVE.B (SP)+` (`WaitNextEvent.a:56`). Writing a word of `1` sets the other
  byte, and every caller then reads false — the event record was being filled in
  correctly and thrown away unlooked at, with the symptom that the application
  never called `_MenuSelect` and the choice timed out.
- **Only the owning application may be asked.** Every process calls
  `GetNextEvent`, background ones included, so the filter runs in whichever one
  happens to be asking. The Process Manager swaps `MenuList` per process, so the
  list visible from the filter identifies whose event loop this is: the event is
  placed only if the pending menu ID and item are in it. Otherwise the Finder
  would take a menu choice meant for the front application and dispatch some
  unrelated item of its own, or none — swallowing the selection either way.

Nothing about the mouse is touched: the guest pointer does not move, and no
click is injected. This matters because the pointer is the user's — parking it
in the menu bar to dispatch a command taken from the host menu bar is visible,
and it interferes with whatever the user was doing.

**What this replaced.**

1. Calling `_MenuKey` or `_SystemMenu` through 68k stubs. This could not work at
   all: it used `Execute68k` from the 60 Hz interrupt, which is not a context
   the Toolbox may be entered from, and the return value went nowhere — nothing
   in the guest was waiting for it. `_SystemMenu` is for desk accessory menus in
   any case.
2. Injecting a real click through ADB. This worked, but moved the guest pointer
   to the menu bar and left it there.

### 4.3.1 Following the front application

The hooked traps catch an application building its own menu bar, but not the
Process Manager handing the menu bar over when the front application changes: a
context switch swaps the low-memory globals directly, `MenuList` among them
(`ProcessMgr/LomemTab.Color.a`). `poll_menu_list()` therefore fingerprints
`MenuList` — its handle, master pointer, length and the menu handles in it — on
every interrupt and requests a sync when it moves. Without it the host menu bar
keeps showing whichever application last called `_SetMenuBar`, in practice the
Finder, for the whole session.

The first observation is a baseline only. Requesting a sync from it would run
the decode on the first interrupt after boot, when `MenuList` still holds
whatever the ROM left in that longword.

### 4.4 ROM low-memory hooks (alternative / supplement)

Inside Macintosh documents globals the ROM already uses:

| Address | Name | Role |
|---------|------|------|
| `0x0A1C` | `MenuList` | Handle to menu list — source for `Toolbox_SnapshotMenuBar` |
| `0x0A26` | `TheMenu` | Highlighted menu ID |
| `0x0A3C` | `MBarHook` | Menu bar drawing hook proc |
| `0x0A30` | `MenuHook` | `MenuSelect` tracking hook |
| `0x0BAA` | `MBarHeight` | Menu bar height in pixels (typically 20) |

Installing a guest proc on `MBarHook` / `MenuHook` is an alternative to
trapping every draw call; trappable `_DrawMenuBar` REPLACE is simpler for v1.

### 4.5 Window / QuickDraw traps (phase 3+, optional)

Full “Classic window layer” compositing would hook Window Manager and
QuickDraw (e.g. `BeginUpdate`, `InvalWindow`, `CopyBits`). That is **not**
required for the menu bar alone. Defer until menu strip + input routing are
stable.

---

## 5. Host menu renderer (new code, future)

Proposed API (names illustrative):

```c
/* include/host_menu_bar.h */
void HostMenuBar_Init(void);
void HostMenuBar_Invalidate(void);           /* called from trap REPLACE handlers */
void HostMenuBar_Draw(void *surface, int x, int y, int w, int h);
int  HostMenuBar_Height(void);               /* 20 or ReadMacInt16(0x0BAA) */
bool HostMenuBar_HandleMouse(int x, int y, bool down, bool up, bool move);
```

Implementation sketch:

1. On invalidate / deferred sync, call `Toolbox_SnapshotMenuBar()`.
2. Paint menu titles at `MenuList` `leftEdge` coordinates (6-byte entries:
   `MenuHandle` + `leftEdge`).
3. Use Classic styling (white bar, black text, Chicago or host fallback font;
   Apple menu `0x14` →  logo).
4. Respect `enableFlags` (bit 0 = menu; bits 1–31 = items) and `hMenuCmd`
   (`0x1B`) for submenu arrows — do not assign Command-key equivalents to
   submenu items.
5. Phase 2: draw dropdown when tracking; phase 1 can dispatch via
   `Toolbox_DispatchGuestMenuSelect` on title-bar clicks only or synthetic
   `MenuSelect`.

Platform backends:

| Platform | Draw target |
|----------|-------------|
| SDL (all) | Top rows of `SDLscreen` in `VideoInterrupt()` |
| macOS | Same SDL path initially; optional `CGContext` / Metal later |
| Windows | Same SDL path; host File/Disk menus may stay Win32 (`menu_bar_win32.cpp`) |

Replace `Toolbox_SetMenuBarSyncCallback(MacMenuBridge_SyncFromGuest)` with
`HostMenuBar_Invalidate` when this lands.

---

## 6. Video blit adjustment

In `VideoInterrupt()` (`video_sdl.cpp`):

1. Let `mbar_h = HostMenuBar_Height()`.
2. Blit guest framebuffer into `SDLscreen` at **destination Y = `mbar_h`**
   (source unchanged in guest RAM), **or** blit full surface then overdraw
   menu bar.
3. Call `HostMenuBar_Draw(SDLscreen, 0, 0, width, mbar_h)` after guest copy.

Guest ROM may still believe the screen starts at Y=0; only the **host**
presentation offsets the desktop. If apps misbehave, consider adjusting cursor
coordinates in the input router (see below) rather than patching guest globals.

---

## 7. Input passthrough

All SDL events today flow through `doevents()` → `ADBMouseMoved` /
`ADBKeyDown` / `ADBKeyUp` with no region filter (`video_sdl.cpp`).

Future router (pseudocode):

```
mbar_h = HostMenuBar_Height()

SDL_MOUSEMOTION / BUTTONDOWN / BUTTONUP:
  if (y < mbar_h)
    HostMenuBar_HandleMouse(x, y, ...)
    // do not call ADB
  else
    ADBMouseMoved(x, y - mbar_h)   // optional Y adjust
    ADBMouseDown/Up(...)

SDL_KEYDOWN / KEYUP:
  if host dropdown is open
    route arrows / return / escape to host menu controller
  else
    existing kc_decode → ADBKeyDown/Up (unchanged)
```

Menu item activation from host:

- `Toolbox_DispatchGuestMenuSelect(menuID, itemIndex)` — arms the `_MenuSelect`
  hook and injects a menu-bar click; see §4.3.
- Still use `MenuQueue` if UI thread posts commands; IRQ drain on CPU thread.

Command-key shortcuts: **passthrough** to guest; `_MenuKey` in the app handles
them. No host interception needed unless the host dropdown has focus.

---

## 8. Cockatrice host menus vs guest menus

Keep a clear split:

| Menu set | Owner | Future placement |
|----------|--------|------------------|
| File, Disk, Reset, … | Cockatrice emulator | `NSApp.mainMenu` (macOS) or Win32 menu bar — unchanged |
| Apple, File, Edit, … (guest) | Emulated Mac app | Host-drawn strip **inside** SDL window |

Do not merge emulator and guest menus into one native menu bar when Classic
compositing is enabled.

---

## 9. Suggested implementation phases

| Phase | Deliverable |
|-------|-------------|
| **1** | `_DrawMenuBar` REPLACE; crop or overdraw in `VideoInterrupt`; stub `HostMenuBar_Draw` (gray bar + titles) |
| **2** | Input router in `doevents()`; click → `Toolbox_DispatchGuestMenuSelect` |
| **3** | Dropdown tracking (`_MenuSelect` hook or host-side popup); `_HiliteMenu` sync |
| **4** | Classic chrome (font, marks, disabled gray, separators) |
| **5** | Remove or `#ifdef` out `macos_menu_bridge.mm` NSMenu sync when compositor pref is on |
| **6** | (Optional) Window Manager / QuickDraw layer hooks |

Add a preference when implementing, e.g. `classic_menu_bar true`, default off
until phase 2 is stable.

---

## 10. Verification checklist (when implemented)

1. Build: `make -C BasiliskII/OSX64 -j4`
2. Tests: `make -C BasiliskII/tests test`
3. Boot Quadra ROM + disk; confirm `[TOOLBOX-TRAP] Hooked … _DrawMenuBar` and
   no duplicate menu bar pixels in the guest desktop area.
4. Finder loads; host strip shows , File, Edit, View, Special with correct
   titles from `MenuList`.
5. Mouse in desktop region moves guest cursor; clicks reach guest windows.
6. Mouse in menu strip highlights titles; item click runs guest command
   (e.g. About, Quit with shortcut).
7. Command-Q / Command-O still work via guest `_MenuKey` when focus is in
   the emulated desktop.

---

## 11. Relation to existing docs

- Boot and trap install timing: [basilisk-ii-boot-and-patch.md](basilisk-ii-boot-and-patch.md)
  (`InstallDrivers` installs the drivers and the runtime OS traps; the later
  `PatchAfterStartup` is where `ToolboxTrap_InstallAll` runs).
- CPU / EmulOp: [cpu-engine-opcode-fixes.md](cpu-engine-opcode-fixes.md).

When implementing, update `AGENTS.md` with a pointer to this file and note
which phase is complete.
