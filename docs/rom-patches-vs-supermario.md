# Cockatrice ROM patches vs. the SuperMario ROM sources

Cockatrice III's ROM and resource patches were written by disassembling Mac ROMs.
The **SuperMario** source tree is Apple's own source for most of what those patches
touch, so it tells us what each patched routine is actually called, what it does,
and — in several cases — that Apple provided a documented mechanism for the thing
Cockatrice does by overwriting ROM bytes.

Read this alongside:

- [basilisk-ii-boot-and-patch.md](basilisk-ii-boot-and-patch.md) — the host call
  chain and boot order
- [quadra-32bit-boot-crashes.md](quadra-32bit-boot-crashes.md) — Type 10 / Type 4
  debugging

**Scope note.** SuperMario is System 7.1-era. Cockatrice's *ROM* patches target
ROMs of roughly that vintage, so the mapping is close. Its *resource* patches
target System 7.5-8.1 files, so byte signatures often differ from the 7.1 sources
even where the routine and its purpose are identical. Where that happens it is
called out per entry.

---

## 1. Navigating the tree

```
~/Source/supermario/                       wrapper repo (patchsets, build tooling)
~/Source/supermario/base/SuperMarioProj.1994-02-09/     the pristine source tree
```

Below, `$SM` means that second path.

Three practical notes:

- **Files are CR-terminated** (classic Mac). `grep -n` reports the whole file as
  line 1. Always filter first: `tr '\r' '\n' < "$SM/OS/StartMgr/StartInit.a" | sed -n '1600,1700p'`
- **Ignore `.idump` / `.rdump` sidecars.** Every source file has two, holding the
  MPW type/creator and resource fork. They roughly triple the apparent file count.
- The tree **does not build as-is**; `~/Source/supermario/patchset/` holds patch
  series that make it buildable. We only read it, so that does not matter here.

Layout worth knowing:

| Path | Contents |
|---|---|
| `$SM/OS/StartMgr/` | Start Manager — `StartInit.a` (the ROM's startup), `StartBoot.a`, `Boot1/2/3.a` (the `'boot'` resources) |
| `$SM/OS/` | OS managers: `TimeMgr/`, `ADBMgr/`, `SCSIMgr/`, `MemoryMgr/`, `MMU/`, `SlotMgr/`, `Gestalt/`, plus `Clock.a`, `InterruptHandlers.a`, `DispTable.a`, `Universal.a`, `UniversalTables.a` |
| `$SM/Patches/` | Per-old-ROM RAM patch files (`PatchIIROM.a`, `PatchIIciROM.a`, …) and misc patches |
| `$SM/LinkedPatches/` | The `'lpch'` loader and linked-patch runtime |
| `$SM/Internal/Asm/` | Private equates — `UniversalEqu.a`, `HardwarePrivateEqu.a`, `InternalOnlyEqu.a`, `LinkedPatchMacros.a` |
| `$SM/Interfaces/AIncludes/` | Public equates — `SysEqu.a` (low-memory globals), `HardwareEqu.a`, `Private.a` |
| `$SM/DeclData/` | NuBus/slot declaration ROMs (`DeclData.r`) and video drivers |
| `$SM/Drivers/Sony/` | The `.Sony` floppy driver |
| `$SM/Resources/` | `RomResources.r` (ROM resource manifest), `Sys.r` (System file build) |

**The single most useful cross-reference** is `$SM/OS/DispTable.a` — the complete
68k trap number → ROM routine name table.

---

## 2. The rule that decodes `lpch` and `ptch` IDs

`$SM/Internal/Asm/LinkedPatchMacros.a` declares the master ROM table:

```
DefineConditions$ (Plus,$0075),(SE,$0276),(II,$0178),(Portable,$037A),(IIci,$067C),(SuperMario,$077D), \
    noPatchProtector, notVM, notAUX, hasHMMU, hasPMMU, hasMemoryDispatch, …
```

and the macro that consumes it says:

```
; check to see that we can use resource IDs to express ROM combinations
    if NumROMs$ > 15 then
        aerror 'too many ROMs; up to 15 allowed'
```

So each ROM gets a condition bit, and **an `lpch` resource's ID is the bitmask of
the ROMs its patches apply to**:

| Bit | ROM | Version word |
|---|---|---|
| 0 | Plus | `$0075` |
| 1 | SE | `$0276` |
| 2 | II | `$0178` |
| 3 | Portable | `$037A` |
| 4 | IIci | `$067C` |
| 5 | SuperMario | `$077D` |

That decodes the resource IDs Cockatrice patches:

| ID | Binary | Meaning |
|---|---|---|
| `lpch` 24 | `0b011000` | Portable + IIci |
| `lpch` 31 | `0b011111` | all five pre-SuperMario ROMs ("universal", when there were five) |
| `lpch` 32 | `0b100000` | SuperMario only |
| `lpch` 63 | `0b111111` | universal, all six |

Confirmed by `LinkedPatchMacros.a` change `<49>` (1/31/92), which introduced 32 and
63 when the SuperMario ROM was added, and by the patch sources themselves — see the
`lpch` 24 and `lpch` 31 entries in §4.

### The version word does not uniquely identify a ROM

`LinkedPatchMacros.a` change `<45>` adds `hasTERROR` / `notTERROR` conditionals
"to identify the TERROR `$067C` overpatch ROM". Apple needed extra predicates
because **several different ROMs report `$067C`**. Cockatrice keys its entire patch
strategy off that word (`ROM_VERSION_32`), and the Quadra 800 image used by the
tests reports `0x067c` too. Anything that must distinguish those ROMs has to use
the checksum at `ROMBase+0` or `ProductKind` from the UniversalInfo record.

---

## 3. Mechanisms Cockatrice bypasses

These are the cases where SuperMario shows a documented, version-independent
mechanism for something Cockatrice currently does by rewriting ROM code.

### 3.1 `jCheckLoad` — the Resource Manager load hook

`$SM/Interfaces/AIncludes/Private.a:386`

```
JCheckLoad      EQU     (764-512)*4+OSTable
```

with `OSTable EQU $0400` (`Private.a:235`), so **`jCheckLoad` is at `$07F0`**.

Apple's own install idiom, `$SM/Patches/BeforePatches.a:690-697`:

```
        Lea     OldCheckLoadJump+2,A0   ; < Denman 1/17/90 >
        Move.L  jCheckLoad,(A0)         ; store the old address in our code
        ...
        Lea     MyCheckLoad,A0          ; compute the address of the patch code.
        Move.L  A0,jCheckLoad           ; stuff in my checkLoad hook.
```

Cockatrice instead byte-patches the ROM at the magic offset `0x1b8f4`
(rom_patches.cpp:1519) to jump to a stub squatting inside the `.Sony` resource at
`+0x300` — a stub which then calls through `$07F0` anyway.

**Scheduling constraint:** `$07F0` is populated by `InitRomVectors`
(`$SM/OS/StartMgr/StartInit.a:1424`, re-run at `:1517`), and resources begin
loading before `EMUL_OP_INSTALL_DRIVERS` runs. So switching to the vector does not
remove ROM patching entirely — it reduces it to one verified hook placed after
`InitRomVectors`, which then installs the vector.

### 3.2 `Lvl1DT` — the VIA1 interrupt dispatch table

`$SM/Interfaces/AIncludes/SysEqu.a:1203`

```
Lvl1DT      EQU     $192    ; Interrupt level 1 dispatch table [32 bytes]
Lvl2DT      EQU     $1B2    ; Interrupt level 2 dispatch table [32 bytes]
```

`$SM/OS/InterruptHandlers.a:489-496` gives the slot assignments:

```
Via1DT      equ Lvl1DT                  ; dispatch table for VIA1 interrupts
jOneSecInt  equ Via1DT+4*ifCA2          ; valid on all machines
jVBLInt     equ Via1DT+4*ifCA1          ; valid on all machines
jKbdAdbInt  equ Via1DT+4*ifSR           ; not valid when IopADB or PwrMgrADB
```

with `ifCA2 = 0`, `ifCA1 = 1`, `ifSR = 2` (`$SM/Interfaces/AIncludes/HardwareEqu.a:374-376`).
The secondary dispatcher is `Level1Via1Int` (`InterruptHandlers.a:1558`).

**Apple installs handlers by writing this table directly.**
`$SM/OS/TimeMgr/TimeMgrPatch.a:197-198`:

```
        leaResident Timer2IntNewFreezeTime,a0
        move.l  a0,Lvl1DT+(T2IntBit*4)  ; put into interrupt table
```

Cockatrice instead byte-patches ROM `0x9bc4` to `moveq #2,d0` — forcing the
level-1 dispatcher to *always* select slot 2 — and overwrites the handler at
`0xa296` (rom_patches.cpp:1559-1571). That does not merely hardcode two offsets: it
collapses three distinct interrupt sources into one, so one-second interrupts
(`ifCA2`) and ADB/keyboard shift-register interrupts (`ifSR`) can never be
delivered on their own vectors.

### 3.3 `_SetTrapAddress` — trap replacement

`$SM/OS/TimeMgr/TimeMgrPatch.a:184-202` replaces four traps the ordinary way:

```
InstallTimeMgrPortableIIci InstallProc (Portable,IIci,notAUX)
        move.w  sr,-(sp)                ; save interrupt level
        ori.w   #$0700,sr               ; no interrupts while swapping Time Mgrs
        leaResident RmvTimeNewFreezeTime,a0
        moveq   #$59,d0                 ; _RmvTime
        _SetTrapAddress newOS
        leaResident PrimeTimeNewFreezeTime,a0
        moveq   #$5A,d0                 ; _PrimeTime
        _SetTrapAddress newOS
        leaResident Timer2IntNewFreezeTime,a0
        move.l  a0,Lvl1DT+(T2IntBit*4)  ; put into interrupt table
        leaResident __Microseconds,a0
        moveq   #$93-$100,d0            ; SetTrapAddress(os) only looks at the low byte
        _SetTrapAddress newOS
```

Cockatrice locates traps with `find_rom_trap()` (rom_patches.cpp:108) and
byte-patches the ROM routine bodies. `find_rom_trap()` returns 0 both for
"unimplemented trap" and "trap not found", and **none of its eight callers check
the result** (rom_patches.cpp:1488, :1491, :1494, :1501, :1513, :1533, :1538,
:1547), so a miss writes over the ROM header.

Note that `InstallDrivers()` already installs `Microseconds` the correct way, via
`SetOSTrapAddress` — so the mechanism is present, just not used generally.

---

## 4. Patch map — `rsrc_patches.cpp` (`CheckLoad`)

Signatures here come from 7.5-8.1 System files; the SuperMario column gives the
7.1 source for the same routine.

| Resource | Cockatrice | Purpose | SuperMario |
|---|---|---|---|
| `boot` 3 | :100-111 | Boot stack computation → `EMUL_OP_FIX_BOOTSTACK` | `$SM/OS/StartMgr/Boot3.a:1066-1106` (boot-world relocation), `kBootStackSizeNeeded equ $2000` at :491. **7.1 computes MemTop/2; the patched signature computes ¾·MemTop** — same job, different arithmetic |
| `boot` 3 / `boot` 2 | :114-159 | Fake handle at address 0 | `$SM/OS/StartMgr/Boot3.a`, `Boot2.a`. **Dead code** — inside `#if !ROM_IS_WRITE_PROTECTED`, which is 1 everywhere in-tree; the `#endif` also spans an `else if` boundary, so the whole `boot 2` arm is unreachable |
| `PTCH` 630 | :161-186 | Don't replace Time Manager (System 6.0.3 / 6.0.8) | `$SM/OS/TimeMgr/TimeMgrPatch.a:140` `InstallTimeMgrPlusSEII InstallProc (Plus,SE,II,notAUX)` |
| `ptch` 26 | :188-200 | Trap `$ABC4` initialised with an absolute ROM address → rewritten to `ROMBase+0x33610` | `$ABC4` is `_GetPMData`. `$SM/Resources/Sys.r:2555` builds `'ptch' 26` from `QDciPatchROM.a.rsrc`; `$SM/QuickDraw/Patches/QDciPatchROM.a:19383` `EntryTable GetPMData, $ABC4`. The `ROMBind` macro (`LinkedPatchMacros.a:658`) is how Apple bound such addresses per ROM |
| `ptch` 34 | :202-224 | (1) Don't wait for VIA; (2) don't replace `ADBOp()` | `$SM/OS/ADBMgr/ADBMgrPatch.a:159-174` `InitADB InstallProc (SE,II,notAUX)` — the `movea.l VIA,a1 / @wait: move.b vBufB(a1),d0 / andi.b #$30,d0 / cmpi.b #$30,d0 / bne.s @wait` spin, matching Cockatrice's signature exactly; and `:110` `patchADBOp PatchProc _ADBOp,(SE,II,notAUX)` |
| `gpch` 669 / `lpch` 63 | :227-484 | Thread Manager 68060 FPU frames | No SuperMario equivalent (68060 postdates it). **Compiled out** (`#if !EMULATED_68K`) and JMPs to host pointers, so 64-bit-broken too |
| `gpch` 750 | :486-500 | `BlockMove` PTEST; `SynchIdleTime` | `$SM/OS/MemoryMgr/BlockMove.a:435` "Set DFC register before doing a PTEST"; `$SM/Patches/MiscPatches.a:192` `SynchIdleTimeProc PatchProc _SynchIdleTime` (trap `$ABF7`, `$SM/Internal/Asm/TrapsPrivate.a:95`) |
| `lpch` 24 | :502-517 | Don't replace Time Manager | `$SM/OS/TimeMgr/TimeMgrPatch.a:184-202` (quoted in §3.3). Cockatrice's signature `70 59 a2 47` is `moveq #$59,d0 / _SetTrapAddress` — the `_RmvTime` line at :190-191. It NOPs three `_SetTrapAddress` calls (`_RmvTime`, `_PrimeTime`, `__Microseconds`) and **leaves the `Lvl1DT` write at :198 intact** |
| `lpch` 31 | :519-545 | (1) `vSoundDead` VIA write → RTS; (2) don't replace SCSI Manager; (3) `SynchIdleTime` | `jSoundDead` is `$SM/Interfaces/AIncludes/Private.a:411` (`($B8)*4+nOSTable`); callers `$SM/OS/MemoryMgr/MemoryMgr.a:993`, `MemoryMgrPatches.a:133`. SCSI: `$SM/OS/SCSIMgr/SCSILinkPatch.a:221` `SCSIDispatchCommon PatchProc _SCSIDispatch,(Plus,SE,Portable,II,IIci,notAUX)` — note the ROM list is exactly the five bits of ID 31 |
| `scod` -16463/-16464 | :548-574 | Process Manager 68060 FP frames | No equivalent; compiled out, host pointers |
| `thng` -16563 | :576-581 | Audio component flags | **No SuperMario source** — Sound Manager ships as a binary blob (§6) |
| `sift` -16563 | :583-598 | Replace audio component | **No SuperMario source** (§6) |
| `inst` -19069 | :600-611 | QuickTime 2.0 replacing `Microseconds` | Related: `$SM/OS/Gestalt/GestaltExtensions.a:30-31` — the `gestaltFixedMicroseconds` bit exists precisely to keep "QuickTime from patching PrimeTime and Microseconds" |
| `DRVR` -20066 | :613-624 | `.Infra` driver SCC access | No SuperMario source (infrared driver postdates it) |
| `ltlk` 0 | :626-636 | Disable LocalTalk | **No SuperMario source** — AppleTalk is a binary blob (§6) |

Helper `patch_idle_time()` (:68-87) searches `70 03 a0 9f`, then backs up `0x80`
bytes to find `20 78 02 b6 41 e8 00 80` (an `ExpandMem` + `$80` reference).

---

## 5. Patch map — `rom_patches.cpp`

### 5.1 `patch_rom_32()` is `StartInit.a` with the BSRs removed

`$SM/OS/StartMgr/StartInit.a` is the ROM's startup routine (`MyROM MAIN Export` at
:1139). Its IMPORT block (:1039-1093) names the owning file for each init routine,
and its call sequence is the order Cockatrice suppresses them in:

| `StartInit.a` | Routine | Owning source | Cockatrice |
|---|---|---|---|
| :1331 | `GetHardwareInfo` | `$SM/OS/EgretMgr.a`, `CudaMgr.a` | :999 — 2× NOP at `0xc2` |
| :1352 | `InitVIAs` | `$SM/OS/Universal.a` | :1004 — 15× NOP at `0xc6` |
| :1376 | `InitMMU` | `$SM/OS/MMU/MMUTables.a`, `MMU.a` | :1036-1076 — three signature patches |
| :1424 / :1517 | `InitRomVectors` | `$SM/Internal/Asm/VectorTableInit.a` | *(not patched — the anchor for §3.1)* |
| :1553 | `InitSCC` | `StartInit.a:1146` (exported locally) | :1124 — RTS |
| :1556 | `InitIWM` | `$SM/Drivers/Sony/SonyMFM.a` | :1141 — RTS at `0x9c0` |
| :1604 | `EnableExtCache` | `$SM/OS/HwPriv.a` | :1160 — 2× NOP at `0x190` |
| :1606 | `DisableIntSources` | `$SM/OS/InterruptHandlers.a` | :1165 — RTS at `0x9f4c` |
| :1607 | `SetUpTimeK` | `StartInit.a:2032`, `TimingTable` :2151 | :1169-1182 — writes constant 100 to `TimeDBRA`/`TimeSCCDBRA`/`TimeSCSIDBRA`/`TimeRAMDBRA`. **Site of the UAE Type 4 zero-divide** |
| :1635 | `InitDispatcher` | `$SM/OS/TrapDispatcher/Dispatch.a` | *(not patched — the boundary for §3.3)* |
| :1637 | `InitMemMgr` | `$SM/OS/MemoryMgr/MemoryMgr.a` | — |
| :1642 | `CompBootStack` | `StartInit.a` | :1283 — hand-assembled stack calc + `EMUL_OP_FIX_MEMSIZE` at `0x490` |
| :1661 | `InitIntHandler` | `$SM/OS/InterruptHandlers.a` | *(not patched — the anchor for §3.2)* |
| :1689 | `InitTimeMgr` | `$SM/OS/TimeMgr/TimeMgr.a` | :1212 — RTS at `0xb0e2` (VIA write) |
| :1694 | `InitSlots` | `$SM/OS/SlotMgr/SlotMgrInit.a` | :1245 — NOPs the NuBus probe |
| :1758 | `InitSCSIMgr` | `$SM/OS/SCSIMgr/SCSIMgrInit.a:161` | :1146 — RTS at `0x9a0` |
| :1765 | `InitADB` | `$SM/OS/ADBMgr/ADBMgr.a` | :1320-1355 — NOPs VIA writes at `0xa8a8`/`0xb2c6a`/`0xb2d2e`/`0xa662` |

Also `EnableOneSecInts` / `Enable60HzInts` / `EnableSlotInts`
(`$SM/OS/InterruptHandlers.a`, exported at :549/:590/:612) — NOPed at
rom_patches.cpp:1256, :1267, :1358.

### 5.2 Identity and tables

| Cockatrice | SuperMario |
|---|---|
| `UniversalInfo` located by scanning for `dc 00 05 05` (:970); fields at `+12/+16/+18/+22` (:976-989) | `$SM/Internal/Asm/UniversalEqu.a:519-557` — the `ProductInfo` record: `DecoderInfoPtr` 0, `RamInfoPtr` 4, `VideoInfoPtr` 8, `NuBusInfoPtr` 12, `HwCfgWord` 16, `ProductKind` 18, `DecoderKind` 19, `Rom85Word` 20, `DefaultRSRCs` 22, `ProductInfoVers` 23. Per-machine tables: `$SM/OS/UniversalTables.a` (7799 lines). Probing engine: `$SM/OS/Universal.a` |
| `print_universal_info()` reads `+18` as model id, maps via `MacDesc[]` with `id + 6` | `$SM/Internal/Asm/InternalOnlyEqu.a:625+` — 135 authoritative `box*` equates (`boxUnknown $FD`, `boxPlus $FE`, `boxSE $FF`, `boxMacII 0` … `boxPowerBook150`). The `+6` is the boxflag → gestalt machine-type shift |
| `find_rom_trap()` (:108) decodes the compressed dispatch table at `ROMBase+0x22` | `$SM/OS/DispTable.a` — the authoritative trap → routine table (and a second `OS2` table used when the Figment memory manager is installed) |

### 5.3 Replaced drivers and ROM resources

| Cockatrice | SuperMario |
|---|---|
| `.Sony` (`DRVR` 4) replaced by `sony_driver[]` (:304-350, installed :1456) | `$SM/Drivers/Sony/Sony.a` — `DiskOpen` :253, `DiskClose` :194, `DiskPrime` jump table :199, `CtlTbl` :495 (Kill/Verify/Format/Eject dispatch), `DiskCtlErr` :444. `$SM/Drivers/Sony/SonyHdr.a` defines the driver header |
| Disk/drive icons at `.Sony+0x400/0x600/0x800` | `$SM/Drivers/Sony/SonyIcon.a` |
| `.EDisk` (`DRVR` 51) ROM-scan limit zeroed (:1443) | `$SM/Drivers/EDisk/EDiskDriver.a`, `$SM/Internal/Asm/EDiskEqu.a` |
| `SERD` 0 + `.AIn/.AOut/.BIn/.BOut` stubs (:1476) | **No SuperMario source** — ships as `Serial.rsrc` (§6). Manifest entry: `$SM/Resources/RomResources.r:615` |
| `PACK` 4 presence check (:1553) | `$SM/Resources/RomResources.r` `'rrsc' (120,"InSane")`, `(130,"Sane1")`, `(140,"Sane2")` |
| `InstallSlotROM()` — hand-built declaration ROM ([slot_rom.cpp](../BasiliskII/slot_rom.cpp)) | `$SM/DeclData/DeclData.r` (9518 lines of real sResource definitions), `$SM/DeclData/DeclVideo/` per-chip video drivers, `$SM/OS/SlotMgr/SlotMgr.a` + `SlotMgrInit.a` |

### 5.4 Verified fixed offsets

Most sites are located by signature, but a handful are bare offsets. Those now
check the instruction bytes they expect to be replacing before writing, so a ROM
whose layout differs fails with a named `VERIFY FAILED` instead of scribbling on
unrelated code. The expected bytes (from `dist/Quadra800.rom`):

| Offset | Site | Unpatched bytes | Reading |
|---|---|---|---|
| `0x1142` | `.Sound` open hook | `a0 00 22 78 01 34` | `_Open` / `movea.l SonyVars,a1` |
| `0x1b8f4` | `vCheckLoad` | `20 78 07 f0 4e d0` | `movea.l $07F0,a0` / `jmp (a0)` |
| `0x5b78` | `GetDevBase` | `02 81 00 ff ff ff` | `andi.l #$00FFFFFF,d1` — the 24-bit strip |
| `0x9bc4` | Level-1 dispatcher | `70 7f c0 29 1a 00 c0 29 1c 00` | `moveq #$7F,d0` / `and.b $1A00(a1),d0` / `and.b $1C00(a1),d0` — IFR masked against IER |
| `0xa296` | 60 Hz handler | `52 b8 01 6a 13 7c 00 02` | `addq.l #1,Ticks` / `move.b #2,$1A00(a1)` |
| `0xb2c6a` | `InitADB` VIA write | `11 7c 00 84 1c 00 4e 75` | `move.b #$84,$1C00(a0)` / `rts` |
| `0xb2d2e` | `InitADB` state wait | `c2 11 0c 01 00 30 66` | `and.b (a1),d1` / `cmpi.b #$30,d1` / `bne.s` |

Two of these are worth noting.

`0x1b8f4` decodes to **`movea.l $07F0,a0` / `jmp (a0)`** — the ROM's `vCheckLoad`
already dispatches through the `jCheckLoad` vector. Cockatrice overwrites those
six bytes with a jump to a stub which then does `movea.l $07f0,a0` / `jsr (a0)`
itself. Writing our stub address into `$07F0` and leaving the ROM's own indirect
jump alone would achieve the same thing through the documented mechanism (§3.1).

`0xb2d2e` is `cmpi.b #$30,d1` — the state-3 test from
`$SM/OS/ADBMgr/ADBMgrPatch.a:166-174`, independently confirming that mapping.

`0xa662` (the pre-ROM22 `InitADB` branch) is not verified: it is only reached
when the word at `0xa8a8` is non-zero, which is not the case for any ROM
currently exercised, so there is no known-good byte pattern to check against.

### 5.5 `patch_rom_classic()`

rom_patches.cpp:792-957 is **entirely hard-coded offsets with no verification and
no failure path**. Apple's equivalents — the RAM patch files that a System 7.1
System applies to each of these ROMs — are
`$SM/Patches/PatchPlusROM.a`, `PatchSEROM.a`, `PatchIIROM.a`,
`PatchPortableROM.a`, `PatchIIciROM.a`. They are the reference if this path is ever
rewritten signature-based.

---

## 6. What SuperMario cannot tell us

These ship in the ROM as pre-built binary resources; the source is not in the tree.

| Subsystem | Evidence | Affects |
|---|---|---|
| **Sound Manager** | `$SM/Resources/RomResources.r` `'rrsc' (490,"Sound")` pulls `SoundMgr.rsrc`, which is absent. `$SM/OS/Gestalt/GestaltFunction.a` imports `GetSoundAttributes` "found in SndLowLevel.a" — also absent. Only `$SM/OS/IoPrimitives/SndPrimitives.a` (low-level ASC/AWACS) is present | `thng`/`sift` -16563 audio patches; `.Sound` open hook |
| **SCC serial driver** | `RomResources.r:615/:627/:639` pull `Serial.rsrc` / `SerialDMA.rsrc`, absent. Only IOP firmware `$SM/Drivers/IOP/SCCIOP.aii` is present | `SERD` patch, `.AIn/.AOut/.BIn/.BOut` |
| **AppleTalk** | `$SM/Misc/AppleTalk.rsrc` is a binary; `$SM/Resources/Sys.r:1092` builds `'lmgr' 0` from it | `ltlk` 0 |
| **Virtual Memory** | `$SM/Misc/VM.rsrc` is a binary; loaded as `'ptch' 42` per `$SM/OS/StartMgr/Boot3.a:2589` | — |

---

## 7. Verified ABI references

Facts worth quoting rather than re-deriving.

### `Microseconds` (`_A093`)

`$SM/OS/TimeMgr/TimeMgr.a:736-751`:

```
;  Routine:     MicroSeconds
;  Inputs:      none
;  Outputs:     A0/D0 - 64 bit counter (A0=High, D0=Low)
;  Destroys:    none
;  Called by:   OsTrap dispatcher
__MicroSeconds: proc  export          ; a0-a2/d1-d2 saved by dispatcher
```

This is the authoritative statement of the ABI whose violation in commit `23e7721`
produced the Type 10 at `0x65AAx` — see
[quadra-32bit-boot-crashes.md](quadra-32bit-boot-crashes.md). `A0` is the **high
word of the result**, not a pointer to an `UnsignedWide`. `A1`, `A2`, `D1` and `D2`
are preserved by the dispatcher.

Trap wiring: `$SM/OS/DispTable.a:1451` `OS $93,__Microseconds`.
Trap number: `$SM/Internal/Asm/TrapsPrivate.a:88` `_Microseconds OPWORD $A193`.

### Time Manager traps

`$SM/OS/DispTable.a:1394` `OS $5A,__PrimeTime`; body at
`$SM/OS/TimeMgr/TimeMgr.a:482`. `_RmvTime` is `$59`, `_InsTime` `$58` — the
`moveq #$58,d0` / `moveq #$59,d0` values that appear in the `PTCH 630` and
`lpch 24` signatures.

### `_SCSIDispatch`

`$SM/OS/DispTable.a:377` `ToolBox $015,SCSIDispatchCommon`; body
`$SM/OS/SCSIMgr/SCSILinkPatch.a:221-547`. The selector range check and the
`rtd`-style stack shuffle Cockatrice emulates by hand
([emul_op.cpp](../BasiliskII/emul_op.cpp) `M68K_EMUL_OP_SCSI_DISPATCH`) are at
`SCSILinkPatch.a:248-330`.

### Low-memory globals used by the patches

| Global | Address | Source |
|---|---|---|
| `Lvl1DT` | `$192` | `$SM/Interfaces/AIncludes/SysEqu.a:1203` |
| `Lvl2DT` | `$1B2` | `$SM/Interfaces/AIncludes/SysEqu.a:1204` |
| `OSTable` | `$0400` | `$SM/Interfaces/AIncludes/Private.a:235` |
| `jCheckLoad` | `$07F0` | `$SM/Interfaces/AIncludes/Private.a:386` |
| `jSoundDead` | `$06E0` (`$B8*4+$400`) | `$SM/Interfaces/AIncludes/Private.a:411` |

---

## 8. Verified against a live boot

The offline patch manifest
(`BasiliskII/tests/basilisk/fixtures/quadra800_patches.txt`, asserted by
`basilisk_patches_test`) was cross-checked against a real Musashi boot to
`HasMacStarted: warm-start flag WLSC is set` on the Quadra 800 ROM
(`modelid 29`, `cpu 4`, `ramsize 67108864`).

57 of the 59 records are identical. The two differences are both expected:

| Record | Offline | Live boot | Why |
|---|---|---|---|
| `InstallSlotROM` | `100000` | `0ffc18` | The test stubs out `InstallSlotROM()` (it needs the video subsystem) and reports `ROMSize`; the real one writes at `ROMSize - p`. |
| `SERD 0 + serial drivers` | present | absent | The test pins `ltoudp false`; the boot config used `ltoudp true`, which skips the SERD branch. |

So the manifest genuinely reflects what a boot does, and a diff against it is
meaningful. Re-run the comparison after any patch change:

```
grep -a "ROM-PATCH" boot.log | sed -E 's/^\[ROM-PATCH\] //; s/ +@ /|/; s/ +MISSED.*/|MISSED/'
```

The same boot also confirmed the `'boot' 2` fix: the log now carries
`[RSRC-PATCH] boot 2 fake handle @0 MISSED`, where previously that entire case
was compiled out and invisible.

## 9. ROM images

`roms.txt` in the repo root lists 61 known images with md5 and checksum. It notes
"not all are compatible" without saying which. The corpus test
(`tests/basilisk/basilisk_romcorpus_test.cpp`, `COCKATRICE_ROM_DIR`) is intended to
make that list specific; results will be recorded here as it runs.

`dist/Quadra800.rom` is the reference image used by
`tests/basilisk/basilisk_patches_test.cpp`. It reports version `$067C`.
