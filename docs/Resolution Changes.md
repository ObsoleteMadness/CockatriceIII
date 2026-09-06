# Resolution and colour-depth changes

Status: **working.** Host Video menu, drag-resize, and the guest Monitors CDEV
all change size and depth in-session.

This is the live-switch path for Cockatrice III's NuBus video card. Apple
already split the two knobs in `Video.h` / `Displays.h`; we follow that
contract instead of inventing a private mode number.

**Primary sources**

| Area | File |
|------|------|
| Apple depth vs DisplayModeID, driver Control/Status | `BasiliskII/video.cpp`, `BasiliskII/include/video.h`, `BasiliskII/include/video_defs.h` |
| Slot declaration ROM (six depth sResources, patchable VModeParms) | `BasiliskII/slot_rom.cpp` |
| Host blit, `SDL_SetVideoMode`, drag-resize | `BasiliskII/SDL/video_sdl.cpp` |
| Host Video menu | `BasiliskII/SDL/menu_bar.cpp` |
| jGNEFilter safe point, Display Manager assembly | `BasiliskII/toolbox_window.cpp` |
| Warm-reset restore of the boot desktop | `BasiliskII/cpu_engine.cpp` → `Video_ResetForWarmStart()` |
| Apple SuperMario Display Manager | `~/Source/supermario/.../Toolbox/DisplayMgr/DisplayMgr.c` |
| Display Manager selectors | `~/Source/supermario/.../Interfaces/AIncludes/Displays.a` |

The guest default is **1152×870 × 8-bit** (21" Macintosh RGB). `VideoInit()`
reserves VRAM once for the largest advertised mode at 32 bpp (the host
desktop, currently up to 1710×1112). `MacFrameSize` never grows afterwards,
so the pixel arena in `toolbox_window.cpp` stays put.

---

## 1. Apple's two knobs

`csMode` is **depth**. `csData` is **resolution**. They share a number space
starting at `$80` but they are different fields.

| Field | Meaning | Values |
|-------|---------|--------|
| `csMode` | Apple depth (sResource) | `$80` 1-bit … `$85` 32-bit (`0x80 + VMODE_*`) |
| `csData` | DisplayModeID | `$80` + preset index, or `$C0` for a one-shot custom size |

`cscSetMode` changes depth only. `cscSwitchMode` changes both. Status calls
`cscGetMode` / `cscGetCurMode` / `cscGetNextResolution` / `cscGetVideoParameters`
are what Display Manager and the Monitors CDEV walk.

Do **not** treat `csData` as a pixel size, and do **not** write `pixelSize`
or the CLUT from the driver. Basilisk II's `monitor_desc::switch_mode()`
greys the palette, switches the host framebuffer, patches the slot ROM, and
leaves `InitGDevice` to rebuild the PixMap. Writing `pixelSize` onto a
1-bit CLUT is what hung 1-bit → 8-bit.

---

## 2. Slot ROM

The declaration ROM advertises all six depths as sResources `$80`–`$85`.
Each has a 50-byte `VPBlock` (VModeParms) whose `rowBytes` and bounds are
patched after a live switch (`SlotROM_PatchMode` + checksum + `SUpdateSRT`).

That is what pre-7.6 `InitGDevice` and Display Manager re-read. The card
reports `kModelessConnect` so DM uses `cscGetNextResolution` rather than a
fixed sResource list.

A drag-resize that is not in `VideoPresets` is registered as
`kCustomDisplayModeID` (`$C0`) via `Video_RegisterGuestSize()` so
`cscGetVideoParameters` and `DMSetDisplayMode` can look it up.

---

## 3. Who initiates a switch

```
Host Video menu / SDL drag-resize
    Toolbox_NotifyScreenResized(w, h)     [IRQ / doevents — queue only]
        jGNEFilter stub (GetNextEvent)
            Toolbox_WindowSafePoint()     [EmulOp: assemble, do not call WM]
            stub JSRs the assembled 68k
                DMBeginConfigureDisplays
                DMSetDisplayMode          → driver cscSwitchMode
                DMEndConfigureDisplays
                DMDrawDesktopRect

Monitors CDEV (depth or resolution)
    Display Manager
        cscSwitchMode / cscSetMode        [guest notify off]
        InitGDevice / FixPorts / AllocCursor
```

Host-initiated work is **queued**. `VideoInterrupt` / `doevents` must not
call the Window Manager or Display Manager; those yield, and a yield inside
a nested `Execute68kTrap` never returns (Process Manager,
`toolbox_window.cpp` header comment). The `jGNEFilter` stub is the same
safe point the Notification Manager uses.

Guest `cscSetMode` / `cscSwitchMode` turn `Video_EnableGuestNotify(false)`
so we do not also queue `Toolbox_NotifyScreenResized` and fight
`InitGDevice`.

If Display Manager has never queried `cscGetConnection` /
`cscGetVideoParameters`, the safe point falls back to
`Video_GuestSwitchToSize` (direct `cscSwitchMode`) plus a geometry copy and
`_AllocCursor`.

---

## 4. What `DMSetDisplayMode` does

Trap `_DisplayDispatch` `$ABEB`, D0 = `(paramWords << 8) | selector`
(`Displays.a`).

| Call | Selector word |
|------|----------------|
| `DMBeginConfigureDisplays` | `$0206` |
| `DMSetDisplayMode` | `$0A11` |
| `DMEndConfigureDisplays` | `$0207` |
| `DMDrawDesktopRect` | `$0202` |

`DM_SetDisplayMode` in SuperMario (`DisplayMgr.c:2762`) is the path we
must not reinvent:

1. `SwitchVideoMode` → our `cscSwitchMode` (host surface + slot ROM).
2. Clear `mainScreen` and `ramInit` on the GDevice.
3. `InitGDevice(refNum, newDepth, device)` — with `mainScreen` clear it
   does **not** walk ports.
4. Restore those flags.
5. `FixLowMem` / `FixPorts` / `FixWindowMgrPorts`.
6. `AllocCursor` so `CrsrRow` / `CrsrPin` / `CrsrBase` match the new pitch.

Calling `_InitGDevice` ourselves from the filter stub, without clearing
`mainScreen`, walked ports and jumped through `$2E` (Type 3). Do not put
that trap in the assembled stub.

`DMAddDisplay` / `DMRemoveDisplay` are **multi-monitor hotplug**, not a
mode switch. They create a second GDevice and tear the first down. Finder
treats that as a display coming and going. Do not use them for resize.

---

## 5. Cursor and the grey screen

The cursor VBL blits with low memory, not the pixmap:

| Global | Address | Role |
|--------|---------|------|
| `CrsrPin` | `$834` | Pin rect; must match `gdRect` |
| `CrsrBase` | `$898` | Frame-buffer base |
| `CrsrRow` | `$8AC` | `rowBytes` the blit uses |
| `ChunkyDepth` | `$D60` | Bits per pixel for the expand (`JSR (A3)`) |
| `CrsrBusy` | `$8CD` | Non-zero holds the VBL |

Leaving `CrsrRow` at the old pitch garbles the cursor on a shrink and
address-errors at ROM `$4082E78A` (`JSR (A3)` with a stale expand vector)
on a grow. A Monitors depth change repaired it because `InitGDevice` calls
`AllocCursor`. Display Manager's own comment is that the blit depends on
`CrsrPin` and `Mouse` staying in sync.

A size-only `cscSwitchMode` used to call `set_gray_palette()`. `InitGDevice`
does not `SetEntries` when depth is unchanged, so the host CLUT stayed 50%
grey — the "grey screen" when going to a larger size. Grey the palette
**only** when `csMode` (depth) actually changes.

---

## 6. Host blit (16-bit and millions)

The guest framebuffer is Classic Mac layout. SDL 1.2 on little-endian is not.

| Guest | Memory | Host |
|-------|--------|------|
| 1/2/4-bit | Packed MSB-first | Expand to 8-bit palette indices |
| 8-bit | Index | `memcpy` + `SDL_SetColors` |
| 16-bit | Big-endian 1-5-5-5 | `SDL_MapRGB` (not host 5-6-5) |
| 32-bit | `00 RR GG BB` | `SDL_MapRGB` (not `memcpy`) |

`memcpy` of 32-bit xRGB onto BGRA puts blue in the unused byte and leaves
the blue channel at 0 — everything goes yellow. That is a blit bug, not a
mode-switch bug.

`MacFrameLayout` stays `FLAYOUT_DIRECT` for the memory window; conversion
is in `VideoInterrupt()`.

---

## 7. Warm reset

`Reset680x0` zeros Mac RAM but leaves `VideoMonitor` and the patched slot
ROM at whatever size the user last selected. Booting the 32-bit reservation
at 640×480 then crashed cursor expand. `cpu_engine_reset_peripherals()`
calls `Video_ResetForWarmStart()`, which drops `VideoDriverOpen`'s guest
pointers and switches the host back to 1152×870 × 8-bit with guest notify
off.

---

## 8. What not to do

- Do not call `_InitGDevice` from the `jGNEFilter` stub.
- Do not call Display Manager or the Window Manager from `Execute68kTrap`
  inside the EmulOp (yield never returns). Assemble into the stub.
- Do not `DMAddDisplay` / `DMRemoveDisplay` to change size.
- Do not write `pixelSize`, `gdType`, or the CLUT in `cscSetMode`.
- Do not grey the host palette on a resolution-only switch.
- Do not `memcpy` 16-bit or 32-bit Mac pixels onto the SDL surface.
- Do not grow `MacFrameSize` after boot.
