/*
 *  video.cpp - Video/graphics emulation
 *
 *  Basilisk II (C) 1997-1999 Christian Bauer
 *  Portions (C) 1997-1999 Marc Hellwig
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 *  SEE ALSO
 *    Inside Macintosh: Devices, chapter 1 "Device Manager"
 *    Designing Cards and Drivers for the Macintosh Family, Second Edition
 *
 *  Resolution and depth switching:
 *    This driver follows Apple Video.h / Displays.h and the later Basilisk II
 *    slot driver. csMode is the Apple depth (sResource 0x80..0x85). csData is
 *    the DisplayModeID (0x80 + preset index, or a one-shot custom id).
 *    cscSetMode changes depth only;
 *    cscSwitchMode changes depth and resolution together. Status calls
 *    cscGetMode, cscGetCurMode, cscGetNextResolution and cscGetVideoParameters
 *    are what Display Manager walks. The connection type is kModelessConnect
 *    so DM uses our resolution list instead of a fixed CRT sense-code table.
 *
 *    After a switch the matching slot-ROM VModeParms (rowBytes / bounds) is
 *    patched and SUpdateSRT is called, which is what lets pre-7.6 InitGDevice
 *    and the Monitors CDEV see the new size. Host Video-menu and drag-resize
 *    queue a size and call cscSwitchMode from the jGNEFilter safe point, then
 *    copy pixmap/port/cursor geometry and _AllocCursor. _InitGDevice is not
 *    called from that stub (it jumped to $2E inside GetNextEvent).
 *    MacFrameSize is reserved for the host desktop at 32 bpp at boot and never
 *    grows, so toolbox_window.cpp's pixel arena stays put. The guest boots at
 *    1152x870 x 8-bit.
 */

#include <stdio.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "macos_util.h"
#include "cpu_emulation.h"
#include "slot_rom.h"
#include "video.h"
#include "video_defs.h"

#define DEBUG 0
#include "debug.h"


// Description of the main monitor
video_desc VideoMonitor;

/*
 * Classic Mac / VESA sizes offered until Video_BuildPresets() sees the host.
 * DisplayModeIDs are 0x80 + index. They share a number space with Apple depth
 * modes but live in csData, not csMode.
 */
static const struct video_preset kBasePresets[] = {
	{ 640,  480 },
	{ 800,  600 },
	{1024,  768 },
	{1152,  870 },
	{1280,  720 },
	{1280, 1024 },
	{1600,  900 },
	{1920, 1080 }
};
enum { kBasePresetCount = (int)(sizeof(kBasePresets) / sizeof(kBasePresets[0])),
       kMaxPresets = kBasePresetCount + 1 };

struct video_preset VideoPresets[kMaxPresets] = {
	{ 640,  480 },
	{ 800,  600 },
	{1024,  768 },
	{1152,  870 },
	{1280,  720 },
	{1280, 1024 },
	{1600,  900 },
	{1920, 1080 }
};
int VideoPresetCount = kBasePresetCount;

static int s_max_width = VIDEO_MAX_WIDTH;
static int s_max_height = VIDEO_MAX_HEIGHT;

// First Apple depth mode / first DisplayModeID (Video.h firstVidMode)
static const uint16 kFirstAppleMode = 0x80;

// DisplayModeID of the logical size now in VideoMonitor; 0 means a custom size
static uint32 s_current_id = kFirstAppleMode;
// Apple depth mode (0x80 + VMODE_*) currently installed
static uint16 s_current_apple = 0x83; // boot default is 8-bit
static uint16 s_preferred_apple = 0x83;
static uint32 s_preferred_id = kFirstAppleMode;
// First cscGetVideoParameters means Display Manager is present
static bool s_dm_present = false;
// Slot Manager SPBlock allocated in VideoDriverOpen, used by SUpdateSRT
static uint32 s_slot_param = 0;
static uint8 s_dce_slot = 0;
static uint8 s_dce_slot_id = 0x80;
// Live DCE and Control param blocks for host-initiated cscSwitchMode
static uint32 s_dce = 0;
static uint32 s_cntrl_pb = 0;
static uint32 s_switch_info = 0;
// One-shot DisplayModeID base for a drag-resize that is not in VideoPresets.
// The live custom size is always this id or a later one in the same range so
// a second free-drag (1024x512 → 1024x1024) is not the same csData as before.
static const uint32 kCustomDisplayModeID = 0xC0;
static const uint32 kLastCustomDisplayModeID = 0xCF;
static uint32 s_custom_id = 0;
static int s_custom_width = 0;
static int s_custom_height = 0;

// Local variables (per monitor)
struct {
	video_desc *desc;			// Pointer to monitor description
	uint8 palette[256 * 3];		// Color palette, 256 entries, RGB
	bool luminance_mapping;		// Luminance mapping on/off
	bool interrupts_enabled;	// VBL interrupts on/off
} VidLocal;


/*
 * Rebuilds VideoPresets to fit the host desktop and records the reservation cap.
 *
 * Arguments:
 *   host_width, host_height: Current host desktop size in window pixels.
 */
void Video_BuildPresets(int host_width, int host_height)
{
	if (host_width < VIDEO_MIN_WIDTH)
		host_width = VIDEO_MIN_WIDTH;
	if (host_height < VIDEO_MIN_HEIGHT)
		host_height = VIDEO_MIN_HEIGHT;

	int count = 0;
	for (int i = 0; i < kBasePresetCount; i++) {
		if (kBasePresets[i].width <= host_width && kBasePresets[i].height <= host_height)
			VideoPresets[count++] = kBasePresets[i];
	}

	bool have_host = false;
	for (int i = 0; i < count; i++) {
		if (VideoPresets[i].width == host_width && VideoPresets[i].height == host_height) {
			have_host = true;
			break;
		}
	}
	if (!have_host && count < kMaxPresets) {
		VideoPresets[count].width = host_width;
		VideoPresets[count].height = host_height;
		count++;
	}
	VideoPresetCount = count;

	// Reservation and drag-resize use the bounding box of the advertised modes
	s_max_width = VIDEO_MIN_WIDTH;
	s_max_height = VIDEO_MIN_HEIGHT;
	for (int i = 0; i < count; i++) {
		if (VideoPresets[i].width > s_max_width)
			s_max_width = VideoPresets[i].width;
		if (VideoPresets[i].height > s_max_height)
			s_max_height = VideoPresets[i].height;
	}

	printf("VID: %d presets, reserved %dx%d x 32-bit (%u bytes)\n",
	       VideoPresetCount, s_max_width, s_max_height, Video_ReservedFrameBytes());
	fflush(stdout);
}

/*
 * Live reservation / drag-resize width cap (host desktop after BuildPresets).
 */
int Video_MaxWidth(void)
{
	return s_max_width;
}

/*
 * Live reservation / drag-resize height cap (host desktop after BuildPresets).
 */
int Video_MaxHeight(void)
{
	return s_max_height;
}

/*
 * Returns the number of entries in VideoPresets.
 */
int Video_PresetCount(void)
{
	return VideoPresetCount;
}

/*
 * Writes the width and height of one preset.
 *
 * Arguments:
 *   index: 0-based index into VideoPresets.
 *   width, height: Optional out-parameters.
 *
 * Returns:
 *   true if index is in range.
 */
bool Video_GetPreset(int index, int *width, int *height)
{
	if (index < 0 || index >= VideoPresetCount)
		return false;
	if (width)
		*width = VideoPresets[index].width;
	if (height)
		*height = VideoPresets[index].height;
	return true;
}

/*
 * Returns the DisplayModeID for an exact preset match, or 0 for a custom size.
 *
 * Arguments:
 *   width, height: Pixel size to look up.
 *
 * Returns:
 *   0x80 + preset index, the live custom DisplayModeID, or 0 if unknown.
 */
uint32 Video_ResolutionIDForSize(int width, int height)
{
	for (int i = 0; i < VideoPresetCount; i++) {
		if (VideoPresets[i].width == width && VideoPresets[i].height == height)
			return kFirstAppleMode + (uint32)i;
	}
	// Drag-resize can land on a size that is not in the advertised table
	if (s_custom_id && s_custom_width == width && s_custom_height == height)
		return s_custom_id;
	return 0;
}

/*
 * Looks up a DisplayModeID from the preset table or the live custom size.
 *
 * Arguments:
 *   id: DisplayModeID as reported by cscGetNextResolution (0x80 + index)
 *       or the current custom id ($C0..).
 *   width, height: Optional out-parameters.
 *
 * Returns:
 *   true if id names a known size.
 */
bool Video_SizeForResolutionID(uint32 id, int *width, int *height)
{
	if (s_custom_id && id == s_custom_id && s_custom_width > 0 && s_custom_height > 0) {
		if (width)
			*width = s_custom_width;
		if (height)
			*height = s_custom_height;
		return true;
	}
	if (id < kFirstAppleMode || id >= kFirstAppleMode + (uint32)VideoPresetCount)
		return false;
	int index = (int)(id - kFirstAppleMode);
	if (width)
		*width = VideoPresets[index].width;
	if (height)
		*height = VideoPresets[index].height;
	return true;
}

/*
 * Maps a VMODE_* depth to the contiguous Apple mode in the slot ROM.
 */
uint16 Video_AppleModeForDepth(int mode)
{
	if (mode < VMODE_1BIT || mode > VMODE_32BIT)
		return 0;
	return (uint16)(kFirstAppleMode + mode);
}

/*
 * Inverse of Video_AppleModeForDepth. Returns -1 if the id is not advertised.
 */
int Video_DepthForAppleMode(uint16 apple_mode)
{
	if (apple_mode < kFirstAppleMode || apple_mode > kFirstAppleMode + VMODE_32BIT)
		return -1;
	return (int)(apple_mode - kFirstAppleMode);
}

/*
 * Returns true if the card advertises this VMODE_* depth.
 */
bool Video_HasDepth(int mode)
{
	return mode >= VMODE_1BIT && mode <= VMODE_32BIT;
}

/*
 * Highest Apple depth mode reported in csMaxDepthMode.
 */
uint16 Video_MaxAppleMode(void)
{
	return Video_AppleModeForDepth(VMODE_32BIT);
}

/*
 * Returns the current Apple depth mode.
 */
uint16 Video_CurrentAppleMode(void)
{
	return s_current_apple;
}

/*
 * Returns the DisplayModeID currently in force (a preset id, or 0 for custom).
 */
uint32 Video_CurrentResolutionID(void)
{
	return s_current_id;
}

/*
 * Bytes per row for a given pixel width at an explicit VMODE_* depth.
 *
 * PixMap.rowBytes must be even (Inside Macintosh: Imaging). An odd guest
 * width at 1/2/4/8-bit would otherwise produce an illegal pitch.
 */
uint32 Video_BytesPerRowForMode(int width, int mode)
{
	if (width < 0)
		width = 0;
	uint32 row;
	switch (mode) {
		case VMODE_1BIT:  row = (uint32)((width + 7) / 8); break;
		case VMODE_2BIT:  row = (uint32)((width + 3) / 4); break;
		case VMODE_4BIT:  row = (uint32)((width + 1) / 2); break;
		case VMODE_8BIT:  row = (uint32)width; break;
		case VMODE_16BIT: row = (uint32)width * 2; break;
		case VMODE_32BIT: row = (uint32)width * 4; break;
		default:          row = (uint32)width; break;
	}
	if (row & 1)
		row++;
	return row;
}

/*
 * Bytes per row for a given pixel width at the current VideoMonitor.mode.
 */
uint32 Video_BytesPerRow(int width)
{
	return Video_BytesPerRowForMode(width, VideoMonitor.mode);
}

/*
 * Framebuffer bytes reserved at boot: the largest advertised mode at 32 bpp.
 * Video_SwitchToModeDepth() never grows this.
 */
uint32 Video_ReservedFrameBytes(void)
{
	return Video_BytesPerRowForMode(s_max_width, VMODE_32BIT) * (uint32)s_max_height;
}

/*
 * Records the logical size/depth that VideoMonitor now describes and patches
 * the matching slot-ROM VModeParms so InitGDevice re-reads the new bounds.
 *
 * Arguments:
 *   width, height: Live pixel dimensions after a successful switch or boot.
 */
void Video_NoteCurrentMode(int width, int height)
{
	s_current_id = Video_ResolutionIDForSize(width, height);
	s_current_apple = Video_AppleModeForDepth(VideoMonitor.mode);
	if (SlotROMOffset)
		SlotROM_PatchMode(VideoMonitor.mode, width, height,
		                  Video_BytesPerRowForMode(width, VideoMonitor.mode));
}

/*
 * Size-only switch: keep the current VMODE_* depth.
 */
bool Video_SwitchToMode(int width, int height)
{
	return Video_SwitchToModeDepth(width, height, VideoMonitor.mode);
}

// Guest-notify gate: off around cscSetMode / cscSwitchMode so InitGDevice owns the GDevice
static bool s_guest_notify = true;

/*
 * Guest-notify gate used by Video_SwitchToModeDepth.
 */
void Video_EnableGuestNotify(bool enable)
{
	s_guest_notify = enable;
}

/*
 * Returns whether host-side switches should queue Toolbox_NotifyScreenResized.
 */
bool Video_GuestNotifyEnabled(void)
{
	return s_guest_notify;
}

/*
 * Driver refNum from the DCE saved at VideoDriverOpen.
 */
int16 Video_DriverRefNum(void)
{
	if (!s_dce)
		return 0;
	return (int16)ReadMacInt16(s_dce + dCtlRefNum);
}

/*
 * Drops VideoDriverOpen's guest pointers and puts VideoMonitor / slot-ROM
 * VModeParms back to the 1152x870 x 8-bit boot desktop.
 *
 * Warm reset zeros Mac RAM but leaves the host VideoMonitor at whatever
 * size the user last selected. Booting at 640x480 then switching to the
 * 32-bit reservation crashed ROM cursor expand at $4082E78A.
 */
void Video_ResetForWarmStart(void)
{
	s_dce = 0;
	s_cntrl_pb = 0;
	s_switch_info = 0;
	s_slot_param = 0;
	s_custom_width = 0;
	s_custom_height = 0;
	s_custom_id = 0;
	s_dm_present = false;
	s_current_id = kFirstAppleMode;
	s_current_apple = Video_AppleModeForDepth(VMODE_8BIT);
	s_preferred_apple = s_current_apple;
	s_preferred_id = kFirstAppleMode;

	// Host switch only; RAM is already gone so do not queue a guest notify
	bool notify = s_guest_notify;
	s_guest_notify = false;
	Video_SwitchToModeDepth(VIDEO_DEFAULT_WIDTH, VIDEO_DEFAULT_HEIGHT, VMODE_8BIT);
	s_guest_notify = notify;
}

/*
 * Calls the video driver's cscSwitchMode for a host-requested size.
 *
 * Builds a VDSwitchInfo (current Apple depth, DisplayModeID for the size)
 * and a CntrlParam and dispatches through VideoDriverControl, which is the
 * same entry the Device Manager uses for the Monitors CDEV. A size that is
 * not in VideoPresets is registered as kCustomDisplayModeID first so the
 * driver's SizeForResolutionID check succeeds.
 *
 * Arguments:
 *   width, height: Requested pixel size.
 *
 * Returns:
 *   noErr on success, paramErr if the driver is not open, or the driver's
 *   Control error.
 */
int16 Video_GuestSwitchToSize(int width, int height)
{
	if (!s_dce || !s_cntrl_pb || !s_switch_info)
		return paramErr;
	if (width < VIDEO_MIN_WIDTH || height < VIDEO_MIN_HEIGHT)
		return paramErr;

	uint32 id = Video_RegisterGuestSize(width, height);

	WriteMacInt16(s_switch_info + csMode, s_current_apple);
	WriteMacInt32(s_switch_info + csData, id);
	WriteMacInt16(s_switch_info + csPage, 0);
	WriteMacInt32(s_switch_info + csBaseAddr, VidLocal.desc ? VidLocal.desc->mac_frame_base : 0);

	WriteMacInt16(s_cntrl_pb + ioRefNum, (uint16)Video_DriverRefNum());
	WriteMacInt16(s_cntrl_pb + csCode, cscSwitchMode);
	WriteMacInt32(s_cntrl_pb + csParam, s_switch_info);

	printf("VID: cscSwitchMode %dx%d apple %04x id %08lx\n",
	       width, height, (unsigned)s_current_apple, (unsigned long)id);
	fflush(stdout);
	return VideoDriverControl(s_cntrl_pb, s_dce);
}

/*
 * Advertises width x height as a DisplayModeID the driver and Display
 * Manager can look up. Presets keep their 0x80+index id; anything else
 * becomes the one-shot custom mode used by drag-resize.
 *
 * Arguments:
 *   width, height: Pixel size to register.
 *
 * Returns:
 *   DisplayModeID for that size.
 */
uint32 Video_RegisterGuestSize(int width, int height)
{
	uint32 id = Video_ResolutionIDForSize(width, height);
	if (id)
		return id;
	s_custom_width = width;
	s_custom_height = height;
	// Bump csData so a second free-drag is not the same DisplayModeID.
	if (s_custom_id < kCustomDisplayModeID || s_custom_id >= kLastCustomDisplayModeID)
		s_custom_id = kCustomDisplayModeID;
	else
		s_custom_id++;
	return s_custom_id;
}

/*
 * Returns whether Display Manager has queried cscGetVideoParameters.
 */
bool Video_DisplayManagerPresent(void)
{
	return s_dm_present;
}

/*
 * Fills VidLocal.palette with 50% gray and pushes it to the host, matching
 * Basilisk II monitor_desc::set_gray_palette() so a depth switch is not
 * drawn with a stale 1-bit CLUT.
 */
/*
 * Pauses or resumes the cursor VBL (CrsrBusy at $8CD).
 *
 * DisplayMgr.c stuffs CrsrBusy around a mode change because the cursor
 * blit is not reentrant. InitGDevice then rebuilds JAllocCrsr / CrsrRow.
 *
 * Arguments:
 *   busy: Non-zero to hold the VBL, zero to let it run again.
 */
static void set_cursor_busy(uint8 busy)
{
	WriteMacInt8(0x8cd, busy);
}

static void set_gray_palette(void)
{
	for (int i = 0; i < 256; i++) {
		VidLocal.palette[i * 3 + 0] = 127;
		VidLocal.palette[i * 3 + 1] = 127;
		VidLocal.palette[i * 3 + 2] = 127;
	}
	video_set_palette(VidLocal.palette);
}

/*
 * Basilisk II switch_mode(): locate the video sResource, patch minorBase and
 * the current depth's VModeParms, checksum the declaration ROM, SUpdateSRT,
 * and (only when Display Manager is absent) poke ScrnBase / CrsrBase /
 * MainDevice.baseAddr. InitGDevice owns pixelSize and the CLUT.
 *
 * Arguments:
 *   dce: Driver DCE from VideoDriverControl.
 *   param: VDSwitchInfo / VDPageInfo; csBaseAddr is rewritten.
 */
void Video_UpdateSlotTable(uint32 dce, uint32 param)
{
	const uint32 frame_base = VidLocal.desc->mac_frame_base;
	if (dce) {
		s_dce_slot = ReadMacInt8(dce + dCtlSlot);
		s_dce_slot_id = ReadMacInt8(dce + dCtlSlotId);
		WriteMacInt32(dce + dCtlDevBase, frame_base);
	}
	if (param)
		WriteMacInt32(param + csBaseAddr, frame_base);

	if (!s_slot_param)
		return;

	M68kRegisters r;
	r.a[0] = s_slot_param;
	WriteMacInt8(s_slot_param + spSlot, s_dce_slot);
	WriteMacInt8(s_slot_param + spID, s_dce_slot_id);
	WriteMacInt8(s_slot_param + spExtDev, 0);
	r.d[0] = 0x0016;
	Execute68kTrap(0xa06e, &r); // SRsrcInfo()
	uint32 rsrc = ReadMacInt32(s_slot_param + spPointer);

	WriteMacInt8(s_slot_param + spID, 0x0a); // minorBase
	r.d[0] = 0x0006;
	Execute68kTrap(0xa06e, &r); // SFindStruct()
	uint32 minor_ptr = ReadMacInt32(s_slot_param + spPointer);
	if (minor_ptr >= ROMBaseMac && minor_ptr + 4 <= ROMBaseMac + ROMSize) {
		uint32 minor_base = minor_ptr - ROMBaseMac;
		ROMBaseHost[minor_base + 0] = (uint8)(frame_base >> 24);
		ROMBaseHost[minor_base + 1] = (uint8)(frame_base >> 16);
		ROMBaseHost[minor_base + 2] = (uint8)(frame_base >> 8);
		ROMBaseHost[minor_base + 3] = (uint8)frame_base;
	}

	WriteMacInt32(s_slot_param + spPointer, rsrc);
	WriteMacInt8(s_slot_param + spID, (uint8)s_current_apple);
	r.d[0] = 0x0006;
	Execute68kTrap(0xa06e, &r); // SFindStruct() depth sResource
	WriteMacInt8(s_slot_param + spID, 0x01);
	r.d[0] = 0x0006;
	Execute68kTrap(0xa06e, &r); // SFindStruct() VModeParms
	uint32 parms_ptr = ReadMacInt32(s_slot_param + spPointer);
	if (parms_ptr >= ROMBaseMac && parms_ptr + 18 <= ROMBaseMac + ROMSize) {
		uint32 p = parms_ptr - ROMBaseMac;
		uint32 row_bytes = VidLocal.desc->bytes_per_row;
		uint32 y = VidLocal.desc->y;
		uint32 x = VidLocal.desc->x;
		ROMBaseHost[p +  8] = (uint8)(row_bytes >> 8);
		ROMBaseHost[p +  9] = (uint8)row_bytes;
		ROMBaseHost[p + 14] = (uint8)(y >> 8);
		ROMBaseHost[p + 15] = (uint8)y;
		ROMBaseHost[p + 16] = (uint8)(x >> 8);
		ROMBaseHost[p + 17] = (uint8)x;
	}
	ChecksumSlotROM();

	WriteMacInt8(s_slot_param + spID, s_dce_slot_id);
	r.d[0] = 0x002b;
	Execute68kTrap(0xa06e, &r); // SUpdateSRT()

	/*
	 * Pre-7.6 / no Display Manager: only the frame-buffer base is patched.
	 * Basilisk II must not touch pixelSize here — InitGDevice rebuilds the
	 * PixMap and CLUT. Doing that ourselves is what hung 1-bit → 8-bit.
	 */
	if (!s_dm_present) {
		WriteMacInt32(0x824, frame_base); // ScrnBase
		WriteMacInt32(0x898, frame_base); // CrsrBase
		uint32 gdev = ReadMacInt32(0x8a4); // MainDevice
		if (gdev != 0 && gdev != 0xffffffff) {
			gdev = ReadMacInt32(gdev);
			if (gdev) {
				uint32 pmap = ReadMacInt32(gdev + 0x16); // gdPMap
				if (pmap)
					WriteMacInt32(ReadMacInt32(pmap), frame_base); // baseAddr
			}
		}
	}
}

/*
 * Fills a VDResolutionInfo record for one DisplayModeID.
 *
 * Arguments:
 *   param: Pointer to the VDResolutionInfo in guest memory.
 *   id: DisplayModeID to report.
 *   width, height: Pixel size of that mode.
 */
static void write_resolution_info(uint32 param, uint32 id, uint32 width, uint32 height)
{
	WriteMacInt32(param + csRIDisplayModeID, id);
	WriteMacInt32(param + csHorizontalPixels, width);
	WriteMacInt32(param + csVerticalLines, height);
	WriteMacInt32(param + csRefreshRate, 75 << 16);
	WriteMacInt16(param + csMaxDepthMode, Video_MaxAppleMode());
	WriteMacInt32(param + csResolutionFlags, 0);
}


/*
 *  Driver Open() routine
 */

int16 VideoDriverOpen(uint32 pb, uint32 dce)
{
	D(bug("VideoDriverOpen\n"));
	(void)pb;

	// Init local variables
	VidLocal.desc = &VideoMonitor;
	VidLocal.luminance_mapping = false;
	VidLocal.interrupts_enabled = false;
	s_dm_present = false;

	s_dce = dce;
	s_dce_slot = dce ? ReadMacInt8(dce + dCtlSlot) : 0;
	s_dce_slot_id = dce ? ReadMacInt8(dce + dCtlSlotId) : 0x80;

	// Slot Manager SPBlock for SUpdateSRT after a live switch
	if (!s_slot_param) {
		M68kRegisters r;
		r.d[0] = SIZEOF_SPBlock;
		Execute68kTrap(0xa71e, &r); // NewPtrSysClear()
		s_slot_param = r.a[0];
	}

	// CntrlParam + VDSwitchInfo so the host Video menu can call cscSwitchMode
	if (!s_cntrl_pb) {
		M68kRegisters r;
		r.d[0] = SIZEOF_IOParam;
		Execute68kTrap(0xa71e, &r); // NewPtrSysClear()
		s_cntrl_pb = r.a[0];
	}
	if (!s_switch_info) {
		M68kRegisters r;
		r.d[0] = 16;
		Execute68kTrap(0xa71e, &r); // NewPtrSysClear()
		s_switch_info = r.a[0];
	}

	// Match status-call DisplayModeIDs to the size/depth VideoInit installed
	s_preferred_apple = s_current_apple = Video_AppleModeForDepth(VidLocal.desc->mode);
	s_preferred_id = Video_ResolutionIDForSize((int)VidLocal.desc->x, (int)VidLocal.desc->y);
	Video_NoteCurrentMode((int)VidLocal.desc->x, (int)VidLocal.desc->y);

	// Init color palette (solid gray)
	if (!IsDirectMode(VidLocal.desc->mode)) {
		for (int i=0; i<256; i++) {
			VidLocal.palette[i * 3 + 0] = 127;
			VidLocal.palette[i * 3 + 1] = 127;
			VidLocal.palette[i * 3 + 2] = 127;
		}
		video_set_palette(VidLocal.palette);
	}
	return noErr;
}


/*
 *  Driver Control() routine
 */

int16 VideoDriverControl(uint32 pb, uint32 dce)
{
	uint16 code = ReadMacInt16(pb + csCode);
	uint32 param = ReadMacInt32(pb + csParam);
	D(bug("VideoDriverControl %d\n", code));
	switch (code) {

		case cscSetMode: {		// Set color depth only (Video.h: csMode is Apple depth)
			uint16 apple = ReadMacInt16(param + csMode);
			D(bug(" SetMode %04x\n", apple));
			WriteMacInt32(param + csBaseAddr, VidLocal.desc->mac_frame_base);
			if (ReadMacInt16(param + csPage))
				return paramErr;
			int depth = Video_DepthForAppleMode(apple);
			if (depth < 0)
				return paramErr;
			if (apple != s_current_apple) {
				set_gray_palette();
				set_cursor_busy(1);
				Video_EnableGuestNotify(false);
				bool ok = Video_SwitchToModeDepth((int)VidLocal.desc->x, (int)VidLocal.desc->y, depth);
				Video_EnableGuestNotify(true);
				set_cursor_busy(0);
				if (!ok)
					return controlErr;
				Video_UpdateSlotTable(dce, param);
			}
			WriteMacInt32(param + csBaseAddr, VidLocal.desc->mac_frame_base);
			return noErr;
		}

		case cscSetEntries:		// Set palette (CLUT depths)
		case cscDirectSetEntries: {
			D(bug(" (Direct)SetEntries table %08lx, count %d, start %d\n", ReadMacInt32(param + csTable), ReadMacInt16(param + csCount), ReadMacInt16(param + csStart)));
			bool is_direct = IsDirectMode(VidLocal.desc->mode);
			if (code == cscSetEntries && is_direct)
				return controlErr;
			if (code == cscDirectSetEntries && !is_direct)
				return controlErr;

			uint32 s_pal = ReadMacInt32(param + csTable);	// Source palette
			uint8 *d_pal;									// Destination palette
			uint16 count = ReadMacInt16(param + csCount);
			if (!s_pal || count > 255)
				return paramErr;

			if (ReadMacInt16(param + csStart) == 0xffff) {	// Indexed
				for (uint32 i=0; i<=count; i++) {
					d_pal = VidLocal.palette + ReadMacInt16(s_pal) * 3;
					uint8 red = (uint16)ReadMacInt16(s_pal + 2) >> 8;
					uint8 green = (uint16)ReadMacInt16(s_pal + 4) >> 8;
					uint8 blue = (uint16)ReadMacInt16(s_pal + 6) >> 8;
					if (VidLocal.luminance_mapping)
						red = green = blue = (red * 0x4ccc + green * 0x970a + blue * 0x1c29) >> 16;
					*d_pal++ = red;
					*d_pal++ = green;
					*d_pal++ = blue;
					s_pal += 8;
				}
			} else {										// Sequential
				d_pal = VidLocal.palette + ReadMacInt16(param + csStart) * 3;
				for (uint32 i=0; i<=count; i++) {
					uint8 red = (uint16)ReadMacInt16(s_pal + 2) >> 8;
					uint8 green = (uint16)ReadMacInt16(s_pal + 4) >> 8;
					uint8 blue = (uint16)ReadMacInt16(s_pal + 6) >> 8;
					if (VidLocal.luminance_mapping)
						red = green = blue = (red * 0x4ccc + green * 0x970a + blue * 0x1c29) >> 16;
					*d_pal++ = red;
					*d_pal++ = green;
					*d_pal++ = blue;
					s_pal += 8;
				}
			}
			video_set_palette(VidLocal.palette);
			return noErr;
		}

		case cscSetGamma:		// Set gamma table
			D(bug(" SetGamma\n"));
			return noErr;

		case cscGrayPage: {		// Fill page with dithered gray pattern
			D(bug(" GrayPage %d\n", ReadMacInt16(param + csPage)));
			if (ReadMacInt16(param + csPage))
				return paramErr;

			uint32 pattern[6] = {
				0xaaaaaaaa,		// 1 bpp
				0xcccccccc,		// 2 bpp
				0xf0f0f0f0,		// 4 bpp
				0xff00ff00,		// 8 bpp
				0xffff0000,		// 16 bpp
				0xffffffff		// 32 bpp
			};
			uint32 p = VidLocal.desc->mac_frame_base;
			uint32 pat = pattern[VidLocal.desc->mode];
			for (uint32 y=0; y<VidLocal.desc->y; y++) {
				uint32 p2 = p;
				for (uint32 x=0; x<VidLocal.desc->bytes_per_row / 4; x++) {
					WriteMacInt32(p2, pat);
					p2 += 4;
					if (VidLocal.desc->mode == VMODE_32BIT)
						pat = ~pat;
				}
				p += VidLocal.desc->bytes_per_row;
				pat = ~pat;
			}
			return noErr;
		}

		case cscSetGray:		// Enable/disable luminance mapping
			D(bug(" SetGray %02x\n", ReadMacInt8(param + csMode)));
			VidLocal.luminance_mapping = ReadMacInt8(param + csMode);
			return noErr;

		case cscSetDefaultMode: { // Remember the preferred Apple depth
			uint16 apple = ReadMacInt8(param + csMode);
			D(bug(" SetDefaultMode %02x\n", apple));
			if (Video_DepthForAppleMode(apple) < 0)
				return paramErr;
			s_preferred_apple = apple;
			return noErr;
		}

		case cscSwitchMode: {	// Switch depth (csMode) and resolution (csData)
			uint16 apple = ReadMacInt16(param + csMode);
			uint32 id = ReadMacInt32(param + csData);
			D(bug(" SwitchMode %04x, %08lx\n", apple, id));
			WriteMacInt32(param + csBaseAddr, VidLocal.desc->mac_frame_base);
			if (ReadMacInt16(param + csPage))
				return paramErr;
			int depth = Video_DepthForAppleMode(apple);
			if (depth < 0)
				return paramErr;
			int width = 0, height = 0;
			if (id == kDisplayModeIDCurrent) {
				width = (int)VidLocal.desc->x;
				height = (int)VidLocal.desc->y;
			} else if (!Video_SizeForResolutionID(id, &width, &height)) {
				return paramErr;
			}
			// $C0 is reused for every non-preset drag-resize. 1024x512 and
			// 1024x1024 share that id, so compare the pixel size too.
			bool size_changed = (width != (int)VidLocal.desc->x ||
			                     height != (int)VidLocal.desc->y);
			if (apple != s_current_apple || id != s_current_id || size_changed) {
				// Grey only on a depth change. A size-only switch left the
				// host CLUT grey forever (InitGDevice does not SetEntries).
				if (apple != s_current_apple)
					set_gray_palette();
				set_cursor_busy(1);
				Video_EnableGuestNotify(false);
				bool ok = Video_SwitchToModeDepth(width, height, depth);
				Video_EnableGuestNotify(true);
				set_cursor_busy(0);
				if (!ok)
					return controlErr;
				Video_UpdateSlotTable(dce, param);
			}
			WriteMacInt32(param + csBaseAddr, VidLocal.desc->mac_frame_base);
			return noErr;
		}

		case cscSavePreferredConfiguration: {
			uint16 apple = ReadMacInt16(param + csMode);
			uint32 id = ReadMacInt32(param + csData);
			D(bug(" SavePreferredConfiguration %04x, %08lx\n", apple, id));
			if (Video_DepthForAppleMode(apple) < 0)
				return paramErr;
			if (id != kDisplayModeIDCurrent && !Video_SizeForResolutionID(id, NULL, NULL))
				return paramErr;
			s_preferred_apple = apple;
			s_preferred_id = (id == kDisplayModeIDCurrent) ? s_current_id : id;
			return noErr;
		}

		case cscSetInterrupt:	// Enable/disable VBL
			D(bug(" SetInterrupt %02x\n", ReadMacInt8(param + csMode)));
			VidLocal.interrupts_enabled = (ReadMacInt8(param + csMode) == 0);
			return noErr;

		default:
			//printf("WARNING: Unknown VideoDriverControl(%d)\n", code);
			return controlErr;
	}
}


/*
 *  Driver Status() routine
 */

int16 VideoDriverStatus(uint32 pb, uint32 dce)
{
	uint16 code = ReadMacInt16(pb + csCode);
	uint32 param = ReadMacInt32(pb + csParam);
	D(bug("VideoDriverStatus %d\n", code));
	switch (code) {

		case cscGetMode:			// Get current Apple depth (VDPageInfo)
			D(bug(" GetMode -> %04x\n", s_current_apple));
			WriteMacInt16(param + csMode, s_current_apple);
			WriteMacInt16(param + csPage, 0);
			WriteMacInt32(param + csBaseAddr, VidLocal.desc->mac_frame_base);
			return noErr;

		case cscGetPageCnt:			// Get number of pages
			D(bug(" GetPageCnt\n"));
			WriteMacInt16(param + csPage, 1);
			return noErr;

		case cscGetPageBase:		// Get page base address
			D(bug(" GetPageBase\n"));
			WriteMacInt32(param + csBaseAddr, VidLocal.desc->mac_frame_base);
			return noErr;

		case cscGetGray:			// Get luminance mapping flag
			D(bug(" GetGray\n"));
			WriteMacInt8(param + csMode, VidLocal.luminance_mapping ? 1 : 0);
			return noErr;

		case cscGetInterrupt:		// Get interrupt disable flag
			D(bug(" GetInterrupt\n"));
			WriteMacInt8(param + csMode, VidLocal.interrupts_enabled ? 0 : 1);
			return noErr;

		case cscGetDefaultMode:		// Get default Apple depth
			D(bug(" GetDefaultMode -> %02x\n", s_preferred_apple));
			WriteMacInt8(param + csMode, (uint8)s_preferred_apple);
			return noErr;

		case cscGetCurMode:			// Get current depth + DisplayModeID
			D(bug(" GetCurMode -> %04x/%08lx\n", s_current_apple, s_current_id));
			WriteMacInt16(param + csMode, s_current_apple);
			WriteMacInt32(param + csData, s_current_id ? s_current_id : kFirstAppleMode);
			WriteMacInt16(param + csPage, 0);
			WriteMacInt32(param + csBaseAddr, VidLocal.desc->mac_frame_base);
			return noErr;

		case cscGetConnection:		// Modeless: DM walks cscGetNextResolution
			s_dm_present = true;
			D(bug(" GetConnection\n"));
			WriteMacInt16(param + csDisplayType, kModelessConnect);
			WriteMacInt8(param + csConnectTaggedType, 0);
			WriteMacInt8(param + csConnectTaggedData, 0);
			WriteMacInt32(param + csConnectFlags,
			              (1u << kAllModesValidBit) |
			              (1u << kAllModesSafeBit) |
			              (1u << kTaggingInfoNonStandardBit));
			WriteMacInt32(param + csDisplayComponent, 0);
			return noErr;

		case cscGetModeTiming: {	// Timing flags for one DisplayModeID
			uint32 id = ReadMacInt32(param + csTimingMode);
			D(bug(" GetModeTiming %08lx\n", id));
			if (id != kDisplayModeIDCurrent &&
			    id != s_current_id &&
			    !Video_SizeForResolutionID(id, NULL, NULL))
				return paramErr;
			WriteMacInt32(param + csTimingFormat, kDeclROMTimingFormat);
			WriteMacInt32(param + csTimingData, 0);
			uint32 flags = (1u << kModeValidBit) | (1u << kModeSafeBit) | (1u << kModeShowNowBit);
			if (id == s_preferred_id || (id == kDisplayModeIDCurrent && s_current_id == s_preferred_id))
				flags |= (1u << kModeDefaultBit);
			WriteMacInt32(param + csTimingFlags, flags);
			return noErr;
		}

		case cscGetPreferredConfiguration:
			D(bug(" GetPreferredConfiguration -> %04x/%08lx\n", s_preferred_apple, s_preferred_id));
			WriteMacInt16(param + csMode, s_preferred_apple);
			WriteMacInt32(param + csData, s_preferred_id);
			return noErr;

		case cscGetModeBaseAddress:	// Get frame buffer base address
			D(bug(" GetModeBaseAddress\n"));
			WriteMacInt32(param + csBaseAddr, VidLocal.desc->mac_frame_base);
			return noErr;

		case cscGetNextResolution: {	// Walk the preset table for the Monitors panel
			uint32 id = ReadMacInt32(param + csPreviousDisplayModeID);
			D(bug(" GetNextResolution %08lx\n", id));

			switch (id) {
				case kDisplayModeIDCurrent:
					write_resolution_info(param,
					                      s_current_id ? s_current_id : kFirstAppleMode,
					                      VidLocal.desc->x, VidLocal.desc->y);
					return noErr;

				case kDisplayModeIDFindFirstResolution:
					write_resolution_info(param, kFirstAppleMode,
					                      (uint32)VideoPresets[0].width,
					                      (uint32)VideoPresets[0].height);
					return noErr;

				default: {
					if (!Video_SizeForResolutionID(id, NULL, NULL))
						return paramErr;
					uint32 next = id + 1;
					if (!Video_SizeForResolutionID(next, NULL, NULL)) {
						WriteMacInt32(param + csRIDisplayModeID, kDisplayModeIDNoMoreResolutions);
						return noErr;
					}
					int width = 0, height = 0;
					Video_SizeForResolutionID(next, &width, &height);
					write_resolution_info(param, next, (uint32)width, (uint32)height);
					return noErr;
				}
			}
		}

		case cscGetVideoParameters: {	// VPBlock for one (DisplayModeID, Apple depth)
			uint32 id = ReadMacInt32(param + csDisplayModeID);
			uint16 apple = ReadMacInt16(param + csDepthMode);
			D(bug(" GetVideoParameters %04x/%08lx\n", apple, id));
			s_dm_present = true;

			int depth = Video_DepthForAppleMode(apple);
			if (depth < 0)
				return paramErr;

			int width = 0, height = 0;
			if (id == kDisplayModeIDCurrent) {
				width = (int)VidLocal.desc->x;
				height = (int)VidLocal.desc->y;
			} else if (!Video_SizeForResolutionID(id, &width, &height)) {
				return paramErr;
			}

			uint32 vp = ReadMacInt32(param + csVPBlockPtr);
			if (!vp)
				return paramErr;

			uint32 row_bytes = Video_BytesPerRowForMode(width, depth);
			WriteMacInt32(vp + vpBaseOffset, 0);
			WriteMacInt16(vp + vpRowBytes, (uint16)row_bytes);
			WriteMacInt16(vp + vpBounds, 0);
			WriteMacInt16(vp + vpBounds + 2, 0);
			WriteMacInt16(vp + vpBounds + 4, (uint16)height);
			WriteMacInt16(vp + vpBounds + 6, (uint16)width);
			WriteMacInt16(vp + vpVersion, 0);
			WriteMacInt16(vp + vpPackType, 0);
			WriteMacInt32(vp + vpPackSize, 0);
			WriteMacInt32(vp + vpHRes, 0x00480000);
			WriteMacInt32(vp + vpVRes, 0x00480000);

			uint16 pix_type = 0, pix_size = 8, cmp_count = 1, cmp_size = 8, dev_type = 0;
			switch (depth) {
				case VMODE_1BIT:
					pix_size = 1; cmp_size = 1;
					break;
				case VMODE_2BIT:
					pix_size = 2; cmp_size = 2;
					break;
				case VMODE_4BIT:
					pix_size = 4; cmp_size = 4;
					break;
				case VMODE_8BIT:
					pix_size = 8; cmp_size = 8;
					break;
				case VMODE_16BIT:
					pix_type = 0x10; pix_size = 16;
					cmp_count = 3; cmp_size = 5;
					dev_type = 2;
					break;
				case VMODE_32BIT:
					pix_type = 0x10; pix_size = 32;
					cmp_count = 3; cmp_size = 8;
					dev_type = 2;
					break;
				default:
					break;
			}
			WriteMacInt16(vp + vpPixelType, pix_type);
			WriteMacInt16(vp + vpPixelSize, pix_size);
			WriteMacInt16(vp + vpCmpCount, cmp_count);
			WriteMacInt16(vp + vpCmpSize, cmp_size);
			WriteMacInt32(vp + vpPlaneBytes, 0);
			WriteMacInt32(param + csPageCount, 1);
			WriteMacInt32(param + csDeviceType, dev_type);
			return noErr;
		}

		case cscGetMultiConnect: {
			uint32 conn = ReadMacInt32(param + csDisplayCountOrNumber);
			D(bug(" GetMultiConnect %08lx\n", conn));
			if (conn == 0xffffffff) {
				WriteMacInt32(param + csDisplayCountOrNumber, 1);
				return noErr;
			}
			if (conn == 1) {
				WriteMacInt16(param + csConnectInfo + csDisplayType, kModelessConnect);
				WriteMacInt8(param + csConnectInfo + csConnectTaggedType, 0);
				WriteMacInt8(param + csConnectInfo + csConnectTaggedData, 0);
				WriteMacInt32(param + csConnectInfo + csConnectFlags,
				              (1u << kAllModesValidBit) |
				              (1u << kAllModesSafeBit) |
				              (1u << kTaggingInfoNonStandardBit));
				WriteMacInt32(param + csConnectInfo + csDisplayComponent, 0);
				return noErr;
			}
			return paramErr;
		}

		default:
			//D(bug("WARNING: Unknown VideoDriverStatus(%d)\n", code));
			return statusErr;
	}
}
