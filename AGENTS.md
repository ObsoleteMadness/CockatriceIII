# Cockatrice III Agent Guidelines

## CPU Emulation Debugging
Musashi is the golden path. 
Alternate CPU bugs are usually in the glue code to Cockatrice. Eg the configuration of the emulator (ie headers), poor architectural assumptions (eg 32bit vs 64bit), trap handlers, IRQs, endian issues, etc. 

Do not reach for the Basilisk/Cockatrice resource patching, boot processes etc, as they've been tested with Musashi and a legacy UAE cpu build. 

If you have do make changes to Cockatrice core files for CPU tests, they must be in a #ifdef. eg `#ifdef _JIT_TEST`.


## Boot and ROM patches

When debugging Mac ROM startup, ROM patches, EmulOps (`0x71xx`), or
volume mount, read [docs/basilisk-ii-boot-and-patch.md](docs/basilisk-ii-boot-and-patch.md)
first. It maps the host call chain (`SDL_main` → `InitAll` → `CheckROM` →
`Init680x0` → `PatchROM` → `Start680x0`) onto `EmulOp()` / `CheckLoad()`
and lists expected console progress.

When debugging Quadra / 32-bit ROM **Type 10** (Line-F at `0x65AAx`) or UAE
interpreter **Type 4** (zero divide in TimeDBRA calibration), read
[docs/quadra-32bit-boot-crashes.md](docs/quadra-32bit-boot-crashes.md). Those
are EmulOp ABI and Time Manager glue, not ROM/SCSI patch bugs.

When changing, extending or debugging a specific ROM/resource patch, read
[docs/rom-patches-vs-supermario.md](docs/rom-patches-vs-supermario.md). It maps
each patch in `rom_patches.cpp` / `rsrc_patches.cpp` onto the Apple SuperMario ROM
source that justifies it (`~/Source/supermario`), decodes the `lpch`/`ptch`
resource IDs, and records the documented vectors (`jCheckLoad` `$07F0`, `Lvl1DT`
`$192`, `_SetTrapAddress`) that several patches currently bypass.

When debugging CPU engine opcode battery failures or `Execute68k` / `0x7100`
handling, read [docs/cpu-engine-opcode-fixes.md](docs/cpu-engine-opcode-fixes.md)
for the UAE and Emu68 fixes already landed (syn68k is still open).

When porting a CPU core or adding a hook to any engine, read
[docs/uae-portable-cpu-host-hooks.md](docs/uae-portable-cpu-host-hooks.md).
It lists every host hook Cockatrice relies on (EmulOp traps, nested
Execute68k, emulated clock, MMU-less Line-F, bus faults, JIT invalidation)
with Musashi evidence and the status in uae-portable-cpu. The Amiberry tree
it refers to has since been removed: `BasiliskII/vendor/uae-portable-cpu`
(GPL-2) replaced it and took over the `uae` engine id.

## Building

CMake only, 64-bit only, SDL on all three hosts:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
ctest --test-dir build -L gate --output-on-failure
```

`-L gate` (the twelve `basilisk_*` suites) must pass. `-L cpu` is engine
accuracy and is reported rather than gated — see
[BasiliskII/tests/README.md](BasiliskII/tests/README.md).

When adding, renaming or reordering anything in the **host menu bar**, edit
`build_model()` in `BasiliskII/SDL/menu_model.cpp` and nothing else. That file is
the single description of the menus, their labels, shortcuts, enable rules and
actions; `platform/darwin/sdlmain.m`, `platform/windows/menu_bar_win32.cpp` and
`platform/linux/menu_bar_linux.cpp` only translate it into NSMenu / HMENU /
GtkMenu calls and hand the row's `command_id` back to `MenuModel_Invoke()`.
Adding a native menu call in a platform file is how the ports drifted apart
before; don't. The guest's own (Mac OS Toolbox) menus are a separate concern and
still live in `bridge/darwin/macos_menu_bridge.mm` and `toolbox_menu.cpp`.

When planning or implementing a **Classic in-window menu bar** (host-drawn
menu strip + passthrough input, using toolbox trap hooks instead of native
`NSMenu` sync), read
[docs/classic-menu-bar-compositor.md](docs/classic-menu-bar-compositor.md).
That work is future / optional; the NSMenu bridge in
`BasiliskII/bridge/darwin/macos_menu_bridge.mm` is the current macOS path.

## Code Documentation and Clarity Standards

### 1. Method and Function Documentation
- Every function and method MUST have a preceding function comment describing:
  - The purpose of the function.
  - Its input arguments and their meaning/constraints.
  - Its return value (if any).
  - Any architectural context or hardware/emulator specifics.

### 2. In-Function Step Descriptions
- Inside function bodies, non-trivial lines and logic blocks MUST include single-sentence descriptions explaining what the code is doing and why.
- For opaque operations (e.g., stack pointer validation, trap stub allocation, bitfield offsets, hardware register emulation, memory bank remaps, JIT barriers), provide explicit rationale and domain explanations.

### 3. High-Level Concept Blocks
- If a subsystem, module, or algorithm requires deeper context (e.g., 680x0 CPU context switches, JIT write-protect toggling, exception stack frames, return hook mechanics), add an expanded comment block above the relevant functions or at the top of the file.
