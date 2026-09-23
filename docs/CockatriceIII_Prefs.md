# CockatriceIII_Prefs

Cockatrice III reads its settings from a plain-text file named `CockatriceIII_Prefs`. This page
covers where that file is looked for, its syntax, and every option it understands.

## Where the file is searched for

The first `CockatriceIII_Prefs` found wins. The current working directory is **not** searched,
so the emulator finds the same file however it was launched.

| Order | Location | Hosts |
|-------|----------|-------|
| 1 | The directory holding the executable | all |
| 2 | `CockatriceIII.app/Contents/Resources/` (when running from the app bundle) | macOS |
| 3 | The per-user location, below | all |

Per-user locations:

| Host | Prefs file |
|------|------------|
| Windows | `%APPDATA%\CockatriceIII\CockatriceIII_Prefs` |
| macOS | `~/Library/CockatriceIII/CockatriceIII_Prefs` |
| Linux | `~/.CockatriceIII_Prefs` |

If no file is found, one containing the defaults is written to the per-user location (its
directory is created if needed). The path of the file in use is printed at startup:

```
Prefs: /Users/me/Library/CockatriceIII/CockatriceIII_Prefs
```

The **Save Configuration** menu command writes the current settings back to the same file.

### The ROM file

A relative `rom` path is looked for in the same three places, in the same order; on Linux step 3
is `~/<rom>` rather than a file inside a folder. An absolute path is used as given. The ROM in use
is printed at startup, and when none is found every path that was tried is listed:

```
ROM: /Applications/CockatriceIII/Quadra800.rom
```

Disk image paths (`scsi0`–`scsi6`, `disk`, `floppy`, `cdrom`) are **not** searched this way.
Give them as absolute paths.

## Syntax

```text
# A comment line
keyword value          # a trailing comment
```

- One setting per line: a keyword, whitespace, then the value.
- A line starting with `#` or `;` is a comment. So is anything after a `#` or `;` that follows
  whitespace. A `#` or `;` inside a word is kept, so `/Volumes/Mac#2.hda` is a valid path, but
  `HD #2.hda` would be cut at ` #`.
- The value runs to the end of the line (or the comment), so it may contain spaces:
  `scsi0 /Users/me/Mac Disks/HD.hda`.
- Leading and trailing whitespace, and Windows (CRLF) line endings, are ignored.
- Booleans are `true`; any other value means false.
- Numbers are decimal and may be negative.
- Unknown keywords, and keywords without a value, are ignored silently. Check the spelling if a
  setting seems to have no effect.
- A keyword that appears more than once takes its last value, except `disk`, `floppy` and
  `cdrom`, which may be repeated to add several drives.
- **Save Configuration** rewrites the whole file from the settings in memory, so comments in the
  file are not preserved.

## Example

```text
rom Quadra800.rom
modelid 29
ramsize 67108864
screen win/1152/870

scsi0 /Users/me/Mac/System753.hda
scsi6 /Users/me/Mac/Apps.iso

cpu_emulator uae       # musashi, uae or m68k_rs
jit true
jitdirect true         # fastest: RAM/ROM/video accessed inline

ether slirp
ltoudp true            # LocalTalk over UDP on the printer port
```

## Options

### Machine

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `rom` | path | `Quadra800.rom` | Mac ROM image. See [The ROM file](#the-rom-file). Cockatrice is developed against the Quadra 800 ROM. |
| `modelid` | number | `29` | Gestalt machine ID minus 6, written into the ROM's product information. `29` is the Quadra 800. |
| `ramsize` | number | `67108864` | Guest RAM in bytes (64 MB). Rounded down to a whole megabyte; minimum 1 MB. |
| `cpu` | number | `4` | Ignored. The emulated CPU is always a 68040. Kept so older prefs files still load. |
| `fpu` | boolean | `false` | Ignored. The emulated 68040 always has its FPU. Kept so older prefs files still load. |
| `yearoffset` | number | `0` | Moves the guest clock back by this many × 100,000,000 seconds (about 3.17 years each), so time-limited software can run. |

### CPU engine and JIT

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `cpu_emulator` | text | `musashi` | 680x0 engine: `musashi` (portable interpreter), `uae` (uae-portable-cpu: interpreter or JIT) or `m68k_rs` (Rust interpreter / batch executor). Asking for `uae` or `m68k_rs` in a build without it stops at startup; any other unknown value falls back to `musashi` with a warning. |
| `jit` | boolean | `false` | Use the engine's JIT: translated code for `uae`, the batch executor for `m68k_rs`. Ignored by `musashi`. |
| `jitdirect` | boolean | `false` | `uae` only, needs `jit true`. Translated code reads and writes RAM, ROM and video memory directly instead of calling the memory handlers. This is the fastest mode. The ROM is write-protected on the host while the guest runs, so guest writes to ROM are dropped, as on real hardware. |
| `jitfpu` | boolean | `false` | Also translate FPU instructions. Needs `jit true`; works with or without `jitdirect` (without it, FPU operands in memory go through the memory handlers). The FPU then computes in host doubles, interpreted instructions included, so results have double rather than 80-bit extended precision. |
| `jitcachesize` | number | `16384` | Translation cache size in KB. Smaller caches thrash on Mac OS 8. |
| `m68k_rs_fastmem` | text | `off` | `m68k_rs` only: `off`, `ram`, `multi` or `legacy` direct-RAM window. See [cpu-engine-m68k-rs.md](cpu-engine-m68k-rs.md). |

### Disks

SCSI is the main way to attach disks. Images can also be attached, swapped and detached at run
time from the **Disk** menu.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `scsi0` … `scsi6` | path | none | Disk or CD-ROM image on SCSI ID 0–6. The image is opened read/write if possible, read-only otherwise. It is a CD-ROM if the ID is 3 or 6, or if the file ends in `.iso`, `.cdr`, `.cue` or `.toast`; `.hda`, `.dsk`, `.img` and `.vhd` are hard disks. For a CD image, a `.cue` file with the same name next to it is used automatically. |
| `bootdriver` | number | `-33` | Driver reference number to boot from. SCSI driver numbers run from `-33` (ID 0) to `-39` (ID 6); `0` boots the first bootable volume. |
| `bootdrive` | number | `0` | Drive number to boot from; `0` means any. |
| `disk` | path | none | Disk image for the Basilisk II disk driver rather than SCSI. May be repeated. Prefix the path with `*` to open it read-only. With no `disk` lines, Linux scans `/etc/fstab` for HFS volumes. |
| `floppy` | path | none | Floppy image for the `.Sony` driver. May be repeated; `*` prefix for read-only. |
| `cdrom` | path | none | CD-ROM device or image for the Basilisk II CD-ROM driver. May be repeated. With no `cdrom` lines, Linux adds `/dev/cdrom`. |
| `nocdrom` | boolean | `false` | Don't add the default host CD-ROM device when no `cdrom` line is given. Doesn't affect SCSI CD-ROMs. |
| `extfs` | path | none | Ignored: host folder sharing (ExtFS) isn't included in this build. |
| `scsi_debug` | boolean | `true` | Verbose SCSI and CD-ROM logging on the console. |

### Video, sound and input

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `screen` | text | `win/1152/870` | Initial window size, as `win/<width>/<height>`. Clamped to the host display and to the supported range. It can be changed at run time from the **Video** menu. |
| `frameskip` | number | `2` | Screen refresh divider: redraw every *n*th 60 Hz tick, so `1` is 60 Hz, `2` is 30 Hz, `3` is 20 Hz. `0` is treated as `1`. |
| `hide_cursor` | boolean | `true` | Hide the host mouse cursor while it is over the emulator window, leaving only the Mac's own cursor. |
| `nosound` | boolean | `false` | Disable sound output. |
| `idlewait` | boolean | `false` | Patch Mac OS's idle routine so the emulator sleeps while the guest has no events to process, which saves host CPU. |
| `nogui` | boolean | `false` | Skip the startup preferences editor and report warnings on the console. This build has no preferences editor, so it makes little difference. |

### Networking and serial ports

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `ether` | text | `slirp` | Ethernet: `slirp` for built-in user-mode NAT (no setup or privileges needed); any other name is a host interface opened through libpcap / Npcap for native networking, which must be installed on the host. `tun`/`tap` are not supported. |
| `ltoudp` | boolean | `false` | Connect the printer port to LocalTalk over UDP, compatible with Mini vMac's LToUDP. Enable AppleTalk on the printer port in the guest's AppleTalk control panel. |
| `seriala` | text | `/dev/ttyS0` on Linux | Host device for the modem port. Accepted, but the serial ports aren't connected to host devices in this build. |
| `serialb` | text | `/dev/ttyS1` on Linux | Host device for the printer port. Same as `seriala`; with `ltoudp true` the printer port carries LocalTalk instead. |

### Debugging

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `dump_memory` | boolean | `false` | On an unhandled guest system error, write a binary snapshot of guest RAM to `dump_file` before halting. |
| `dump_file` | path | `/tmp/memory.bin` | Where `dump_memory` writes the snapshot. |

### Experimental host integration (macOS)

These need `toolbox_hooks true`. See [guest-window-mirroring.md](guest-window-mirroring.md).

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `toolbox_hooks` | boolean | `false` | Install Toolbox trap hooks at boot. Required by the options below, and by the bridge that copies the guest's menus into the macOS menu bar. |
| `mdi_windows` | boolean | `false` | Show each guest window in its own macOS window instead of one screen-sized window. |
| `window_redirect` | boolean | `false` | Give each mirrored window its own offscreen buffer, so covered windows draw correctly. Less compatible than the default. |
| `native_alerts` | boolean | `false` | Rebuild guest alert and dialog boxes as native macOS controls. |
