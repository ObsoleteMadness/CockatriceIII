/*
 *  cpu_glue.cpp - Common CPU Emulation Glue and Subsystem Bridge
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *  CockatriceIII Multi-Engine Architecture (C) 2026
 *
 *  This file contains the shared memory pointers and video framebuffer mapping
 *  variables common across all CPU emulation engines in Cockatrice III.
 */

#include <stdio.h>
#include <string.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "main.h"

// System RAM and ROM host/Mac pointer and size definitions
uint32 RAMBaseMac = 0;      // Starting 680x0 address of Mac RAM
uint8 *RAMBaseHost = NULL;  // Host virtual address pointer to Mac RAM
uint32 RAMSize = 0;         // Total size of Mac RAM in bytes
uint32 ROMBaseMac = 0;      // Starting 680x0 address of Mac ROM
uint8 *ROMBaseHost = NULL;  // Host virtual address pointer to Mac ROM
uint32 ROMSize = 0;         // Total size of Mac ROM in bytes

// Mac video display framebuffer variables
uint8 *MacFrameBaseHost = NULL;       // Host virtual address of video framebuffer
uint32 MacFrameSize = 0;              // Framebuffer size in bytes
int MacFrameLayout = FLAYOUT_NONE;    // Framebuffer color/layout mode

/*
 * Commits the real framebuffer bytes after VideoInit sets MacFrameSize.
 *
 * Classic Macs keep the screen in RAM (FLAYOUT_NONE) so 0xA0000000 stays a
 * hole. Mac II / Quadra commit only MacFrameSize at MacFrameBaseMac — the
 * rest of the NuBus slot faults like a historic dummy bank.
 */
void InitFrameBufferMapping(void)
{
	memory_map_framebuffer();
}
