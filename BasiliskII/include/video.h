/*
 *  video.h - Video/graphics emulation
 *
 *  Basilisk II (C) 1997-1999 Christian Bauer
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

#ifndef VIDEO_H
#define VIDEO_H

// Description for one (possibly virtual) monitor
enum {
	VMODE_1BIT,
	VMODE_2BIT,
	VMODE_4BIT,
	VMODE_8BIT,
	VMODE_16BIT,
	VMODE_32BIT
};

#define IsDirectMode(x) ((x) == VMODE_16BIT || (x) == VMODE_32BIT)

struct video_desc {
	uint32 mac_frame_base;	// Mac frame buffer address
	uint32 bytes_per_row;	// Bytes per row
	uint32 x;				// X size of screen (pixels)
	uint32 y;				// Y size of screen (pixels)
	int mode;				// Video mode
};

extern struct video_desc VideoMonitor;	// Description of the main monitor, set by VideoInit()

extern int16 VideoDriverOpen(uint32 pb, uint32 dce);
extern int16 VideoDriverControl(uint32 pb, uint32 dce);
extern int16 VideoDriverStatus(uint32 pb, uint32 dce);

// System specific and internal functions/data
extern bool VideoInit(bool classic);
extern void VideoExit(void);

extern void VideoQuitFullScreen(void);

extern void VideoInterrupt(void);

extern void video_set_palette(uint8 *pal);

/*
 * Host framebuffer reservation and the classic/modern preset list.
 *
 * VideoInit() commits MacFrameSize for the largest advertised mode at
 * 32 bpp once, at boot. Video_SwitchToModeDepth() then only changes the
 * logical VideoMonitor size and depth inside that reservation, so
 * toolbox_window.cpp's pixel arena (placed at MacFrameBaseMac + MacFrameSize)
 * never moves. The guest default is 1152x870 at 8-bit.
 *
 * Apple Video.h splits "mode" in two: csMode is the depth (sResource 0x80
 * plus a contiguous Apple mode for each advertised depth) and csData is the
 * DisplayModeID (0x80 plus a preset index). They share a number space but
 * live in different fields.
 */
enum {
	VIDEO_MIN_WIDTH      = 512,
	VIDEO_MIN_HEIGHT     = 384,
	VIDEO_DEFAULT_WIDTH  = 1152, /* 21" Macintosh RGB; boot default */
	VIDEO_DEFAULT_HEIGHT = 870,
	VIDEO_MAX_WIDTH      = 1920, /* fallback until Video_BuildPresets() sees the host */
	VIDEO_MAX_HEIGHT     = 1080
};

struct video_preset {
	int width;
	int height;
};

extern struct video_preset VideoPresets[];
extern int VideoPresetCount;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Rebuilds VideoPresets from the classic list, dropping anything larger
 * than the host desktop, and appends the host size as the last (maximum)
 * mode when it is not already present. Sets the live reservation cap used
 * by Video_ReservedFrameBytes() / drag-resize.
 *
 * Arguments:
 *   host_width, host_height: Current host desktop size in SDL/window pixels.
 */
void Video_BuildPresets(int host_width, int host_height);

/*
 * Live reservation / clamp cap. Equal to the host desktop after
 * Video_BuildPresets(), or VIDEO_MAX_* before that.
 */
int Video_MaxWidth(void);
int Video_MaxHeight(void);

/*
 * Returns the number of entries in VideoPresets.
 */
int Video_PresetCount(void);

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
bool Video_GetPreset(int index, int *width, int *height);

/*
 * Returns the DisplayModeID (0x80 + index) for an exact preset match, or 0
 * if the size is a free-drag custom mode.
 */
uint32 Video_ResolutionIDForSize(int width, int height);

/*
 * Looks up a DisplayModeID from the preset table.
 *
 * Arguments:
 *   id: DisplayModeID as reported by cscGetNextResolution (0x80 + index).
 *   width, height: Optional out-parameters.
 *
 * Returns:
 *   true if id names a preset.
 */
bool Video_SizeForResolutionID(uint32 id, int *width, int *height);

/*
 * Maps a VMODE_* depth to the contiguous Apple mode advertised in the slot
 * ROM (0x80 + depth). All six depths are always present, so the mapping is
 * stable for the life of the card.
 */
uint16 Video_AppleModeForDepth(int mode);

/*
 * Inverse of Video_AppleModeForDepth. Returns -1 if apple_mode is not one
 * of the advertised depths.
 */
int Video_DepthForAppleMode(uint16 apple_mode);

/*
 * Returns true if the card advertises this VMODE_* depth (always true for
 * VMODE_1BIT..VMODE_32BIT on this driver).
 */
bool Video_HasDepth(int mode);

/*
 * Highest Apple depth mode the card reports in csMaxDepthMode (0x85).
 */
uint16 Video_MaxAppleMode(void);

/*
 * Returns the current Apple depth mode (sResource 0x80 + VMODE_*).
 */
uint16 Video_CurrentAppleMode(void);

/*
 * Returns the DisplayModeID currently in force (a preset id, or 0 for custom).
 */
uint32 Video_CurrentResolutionID(void);

/*
 * Bytes per row for a given pixel width at VideoMonitor.mode.
 */
uint32 Video_BytesPerRow(int width);

/*
 * Bytes per row for a given pixel width at an explicit VMODE_* depth.
 */
uint32 Video_BytesPerRowForMode(int width, int mode);

/*
 * Framebuffer bytes reserved at boot (largest preset at 32 bpp).
 */
uint32 Video_ReservedFrameBytes(void);

/*
 * Records the logical size that VideoMonitor now describes, so status calls
 * (cscGetCurMode / cscGetNextResolution with id 0) report the live mode.
 * Also patches the slot-ROM VModeParms for the current depth.
 */
void Video_NoteCurrentMode(int width, int height);

/*
 * Refreshes the Slot Resource Table from the patched declaration ROM.
 * Safe only from a video-driver Control call (has a live DCE).
 *
 * Arguments:
 *   dce: Driver DCE; used for spSlot / spID and dCtlDevBase.
 *   param: VDSwitchInfo / VDPageInfo; csBaseAddr is rewritten.
 */
void Video_UpdateSlotTable(uint32 dce, uint32 param);

/*
 * Guest-notify gate for Video_SwitchToModeDepth. The video driver turns
 * this off around cscSetMode / cscSwitchMode so Display Manager / InitGDevice
 * can rebuild the GDevice. Host Video-menu and drag-resize queue a size
 * via Toolbox_NotifyScreenResized and call cscSwitchMode from the
 * jGNEFilter safe point (geometry copy plus _AllocCursor, not
 * _InitGDevice).
 */
void Video_EnableGuestNotify(bool enable);
bool Video_GuestNotifyEnabled(void);

/*
 * Restores the boot desktop (1152x870 x 8-bit) and drops guest pointers
 * that VideoDriverOpen allocated. Called from the warm-reset path after
 * RAM is wiped so the next VideoDriverOpen / InitGDevice see the same
 * VModeParms as a cold start.
 */
void Video_ResetForWarmStart(void);

/*
 * Runs the video driver's cscSwitchMode for width x height at the current
 * Apple depth. Allocates the Control param blocks in VideoDriverOpen.
 * Used by the jGNEFilter safe point so host Video-menu / drag-resize go
 * through the same path as the Monitors CDEV.
 *
 * Arguments:
 *   width, height: Requested pixel size (preset or a one-shot custom mode).
 *
 * Returns:
 *   noErr, or a Device Manager error if the driver is not open / the
 *   switch failed.
 */
int16 Video_GuestSwitchToSize(int width, int height);

/*
 * Remembers a drag-resize size that is not in VideoPresets so
 * cscGetVideoParameters / DMSetDisplayMode can look up kCustomDisplayModeID.
 *
 * Arguments:
 *   width, height: Pixel size to advertise for the one-shot custom mode.
 *
 * Returns:
 *   The DisplayModeID to pass to DMSetDisplayMode (preset or custom).
 */
uint32 Video_RegisterGuestSize(int width, int height);

/*
 * True after the first cscGetVideoParameters, which is how Display Manager
 * announces itself to the driver.
 */
bool Video_DisplayManagerPresent(void);

/*
 * Driver refNum saved from the DCE at VideoDriverOpen (negative unit
 * number). 0 if the driver has not opened yet.
 */
int16 Video_DriverRefNum(void);

/*
 * Switches the host window and the logical guest screen to width x height
 * at the current VideoMonitor.mode.
 */
bool Video_SwitchToMode(int width, int height);

/*
 * Switches the host window and the logical guest screen to width x height
 * at VMODE_* depth `mode`.
 *
 * Implemented by the platform video backend. Does not change MacFrameSize.
 * Safe to call from the CPU thread (driver control, menu drain, or doevents).
 *
 * Arguments:
 *   width, height: Requested pixel size, clamped to the reservation.
 *   mode: VMODE_* depth to install.
 *
 * Returns:
 *   true if the logical mode matches the request on return.
 */
bool Video_SwitchToModeDepth(int width, int height, int mode);

#ifdef __cplusplus
}
#endif

#endif
