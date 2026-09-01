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

When debugging CPU engine opcode battery failures or `Execute68k` / `0x7100`
handling, read [docs/cpu-engine-opcode-fixes.md](docs/cpu-engine-opcode-fixes.md)
for the UAE and Emu68 fixes already landed (syn68k is still open).

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
