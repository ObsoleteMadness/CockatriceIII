/*
 *  toolbox_window.cpp - Window Manager client of the Toolbox trap registry
 *
 *  Cockatrice III
 *  (C) 2026 Cockatrice III Project
 *
 *  High-Level Architectural Context:
 *  ==================================
 *  toolbox_traps.cpp owns the mechanism; toolbox_menu.cpp is the worked example
 *  of a client. This is the second client, and it deliberately follows the same
 *  shape: a handler, a registration entry point, and all guest-state decoding
 *  kept on this side of the platform boundary.
 *
 *  What it is for: giving each Mac OS window a real host window, so a Classic
 *  application can wear native chrome while its content stays authentically
 *  Classic.
 *
 *  Discovery is by walk-and-diff, not by hooks:
 *  --------------------------------------------
 *  A hook runs before the ROM routine it fronts, so hooking _NewWindow would
 *  produce a callback at the one moment the window does not yet exist. Instead
 *  Toolbox_ProcessPendingWindowSync() walks WindowList on the IRQ path and
 *  diffs against the previous walk. Creation, move, resize, retitle, show/hide
 *  and z-order all fall out of that diff, so the hook surface stays tiny:
 *
 *    _CloseWindow / _DisposeWindow  must un-redirect a window's port while that
 *                                   port is still intact, so they have to be
 *                                   seen before the ROM runs.
 *    _BeginUpdate / _EndUpdate      bracket the period when the Window Manager
 *                                   has deliberately narrowed visRgn to the
 *                                   update region, which we must not overwrite.
 *
 *  Guest structure layouts here are taken from the Apple SuperMario ROM sources:
 *  WindowRecord from Interfaces/CIncludes/Windows.h, the layer convention from
 *  Toolbox/WindowMgr/LayerMgr.c, and trap numbers from OS/DispTable.a.
 *
 *  Threading:
 *  ----------
 *  Everything here runs on the CPU thread -- handlers inside the trap, syncs
 *  from the IRQ path, presentation from the video refresh. Platform callbacks
 *  are invoked on that same thread and must not call back into the guest.
 */

#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "emul_op.h"
#include "main.h"
#include "macos_util.h"
#include "adb.h"
#include "prefs.h"
#include "toolbox_traps.h"
#include "toolbox_menu.h"
#include "toolbox_window.h"
#include "video.h"
#include <stdlib.h>

#define DEBUG 0
#include "debug.h"

/*
 * Low memory globals used by the Window Manager.
 */
enum {
	LM_WindowList = 0x09d6, // WindowPtr to the frontmost window; head of the list
	LM_GrayRgn    = 0x09ee, // Desktop region; its bounding box is the whole desktop
	LM_TheGDevice = 0x0cc8, // GDHandle of the current graphics device (the screen)
	LM_JGNEFilter = 0x029a, // Event Manager filter vector; our safe point hook
	LM_ScreenBits = 0x0260, // BitMap of the main screen
	LM_ScrnBase   = 0x0824, // Color QD frame-buffer base
	LM_WMgrPort    = 0x09de, // Window Manager GrafPort
	LM_WMgrCPort   = 0x0d2c, // Window Manager colour GrafPort
	LM_ScreenRow   = 0x0106, // rowBytes of the main screen
	LM_CrsrPin     = 0x0834, // cursor pinning rect; must match gdRect
	LM_CrsrBase    = 0x0898, // frame-buffer base the cursor blit uses
	LM_CrsrRow     = 0x08ac, // rowBytes the cursor blit uses
	LM_CrsrBusy    = 0x08cd, // non-zero holds the cursor VBL
	LM_RowBits     = 0x0c20, // screen width in pixels
	LM_ColLines    = 0x0c22, // screen height in pixels
	LM_ScreenBytes = 0x0c24, // rowBytes * height
	LM_ChunkyDepth = 0x0d60  // bits per pixel the cursor expand uses
};

/*
 * GrafPort / CGrafPort field offsets.
 *
 * The two records are deliberately the same size (108 bytes) and agree on every
 * field from portRect onward, so the shared offsets below are valid for both.
 * They differ in the first 16 bytes: a GrafPort has an inline BitMap, while a
 * CGrafPort has a PixMapHandle plus a version word.
 */
enum {
	kPort_Size          = 108,
	kPort_PortPixMap    = 2,  // CGrafPort only: PixMapHandle
	kPort_PortBitsBase  = 2,  // GrafPort only: portBits.baseAddr
	kPort_PortBitsRow   = 6,  // GrafPort only: portBits.rowBytes
	kPort_PortBitsBound = 8,  // GrafPort only: portBits.bounds
	kPort_PortVersion   = 6,  // CGrafPort only: high 2 bits always set
	kPort_PortRect      = 16, // Rect, local coordinates
	kPort_VisRgn        = 24, // RgnHandle
	kPort_ClipRgn       = 28, // RgnHandle
	kPort_TxSize        = 74  // Layer records park the kIsLayer sentinel here
};

/*
 * WindowRecord field offsets, following the 108-byte port.
 */
enum {
	kWin_WindowKind   = 108,
	kWin_Visible      = 110, // Boolean, one byte
	kWin_Hilited      = 111,
	kWin_GoAwayFlag   = 112,
	kWin_SpareFlag    = 113,
	kWin_StrucRgn     = 114, // RgnHandle, global coordinates
	kWin_ContRgn      = 118, // RgnHandle, global coordinates
	kWin_UpdateRgn    = 122,
	kWin_WindowDefProc= 126, // Handle to the WDEF; variation code in the high byte
	kWin_DataHandle   = 130,
	kWin_TitleHandle  = 134, // StringHandle
	kWin_TitleWidth   = 138,
	kWin_ControlList  = 140,
	kWin_NextWindow   = 144, // WindowPeek
	kWin_WindowPic    = 148,
	kWin_RefCon       = 152
};

/*
 * DialogRecord field offsets. A DialogRecord is a WindowRecord (156 bytes) with
 * the dialog's own fields appended, so a dialogKind window can be read as either.
 */
enum {
	kDlg_Items    = 156, // Handle to the 'DITL' item list
	kDlg_TextH    = 160,
	kDlg_EditField= 164,
	kDlg_EditOpen = 166,
	kDlg_ADefItem = 168  // 1-based default item, the one Return activates
};

/*
 * Region record: a size word then the bounding box, then optional inline data.
 * Rects are stored top, left, bottom, right.
 */
/*
 * PixMap field offsets, as laid out in Interfaces/CIncludes/QuickDraw.h.
 */
enum {
	kPix_BaseAddr  = 0,
	kPix_RowBytes  = 4,  // High two bits are flags, not part of the count
	kPix_Bounds    = 6,
	kPix_PixelType = 30,
	kPix_PixelSize = 32,
	kPix_CmpCount  = 34,
	kPix_CmpSize   = 36,
	kPix_PmTable   = 42,
	kPix_RowBytesFlag = 0x8000 // Marks the record as a PixMap rather than a BitMap
};

/*
 * GDevice field offsets (Quickdraw.h). gdPMap identifies the screen PixMap
 * so we refuse to redirect it. After cscSwitchMode we copy GetDevPixMap's
 * geometry (gdRect / pixmap rowBytes and bounds) from VideoMonitor and
 * then call _AllocCursor; we do not call _InitGDevice (its port walk
 * JSR'd to $2E from the jGNEFilter stub).
 */
enum {
	kGD_Type    = 4,   // 0 = CLUT, 2 = direct
	kGD_PMap    = 22,
	kGD_Rect    = 34,
	kGD_CCDepth = 48   // gdCCDepth; 0 forces AllocCursor to rebuild
};

enum {
	kRgn_Size   = 0,
	kRgn_BBox   = 2,
	kRect_Top   = 0,
	kRect_Left  = 2,
	kRect_Bottom= 4,
	kRect_Right = 6
};

/*
 * A layer record is a window whose port carries this sentinel in txSize.
 * From Toolbox/WindowMgr/LayerMgr.c:1060 -- #define kIsLayer ((short) 0xDEAD),
 * tested there as FastIsLayer(w) == (((GrafPtr)w)->txSize == kIsLayer).
 */
static const uint16 kIsLayer = 0xdead;

// Sanity ceiling on the walk, so a corrupt nextWindow chain cannot spin forever
static const int kMaxWindows = 128;

// Registered platform bridge, or all-NULL when no host is attached
static ToolboxWindowCallbacks s_callbacks;
static bool s_callbacks_set = false;

// The previous walk, held to diff against
static std::vector<MacWindowSnapshot> s_last_snapshot;

// True once registration succeeded, so the sync path stays inert otherwise
static bool s_active = false;

// Guest colour table, mirrored from the screen palette so the host can draw
// indexed pixels with the same colours the guest is using
static uint8 s_palette[256 * 3];
static bool s_palette_valid = false;

/*
 * Whether the jGNEFilter safe point is wanted at all.
 *
 * Set by either client: window mirroring makes its Window Manager calls from
 * there, and the Menu Manager client hands over its menu-bar event there. So
 * the filter is not tied to mdi_windows -- a build with only the host menu bar
 * turned on still needs it, or a menu choice made on the host would never be
 * dispatched. Screen-resize notification also uses it, independently of the
 * toolbox_hooks pref, because _InvalRect is only safe from this context.
 */
static bool s_safepoint_wanted = false;

/*
 * Consecutive walks a window must be absent from before it is believed gone.
 *
 * Ten seconds at 60 Hz, which is deliberately far longer than any window is
 * really missing for. Absence from WindowList is a poor signal for destruction:
 * launching an application takes a background process's windows out of view for
 * seconds at a time while the Process Manager builds the new layer, and retiring
 * on that tears down and rebuilds every host window -- which then produces a
 * burst of focus and geometry notifications from Cocoa that go back to the
 * guest, and the whole thing starts again.
 *
 * A genuine close does not depend on this at all: the _CloseWindow and
 * _DisposeWindow hooks retire the window as it happens, before the ROM tears it
 * down. This is only the safety net for a window disposed some other way.
 */
enum { kMissingWalksToRetire = 600 };

// WindowPtr -> consecutive walks it has been missing from
static std::map<uint32, int> s_missing;

// Whether windows get their own offscreen pixel buffer (window_redirect pref)
static bool s_redirect_enabled = false;

/*
 * Bookkeeping for one window whose port has been pointed at a private buffer.
 *
 * Everything needed to put the port back exactly as it was is kept here: the
 * PixMap record is modified in place rather than swapped for another handle, so
 * the original three fields are all that has to be remembered.
 */
struct RedirectedWindow {
	uint32 window_ptr;
	uint32 pixmap_addr;       // Dereferenced PixMap record we rewrote
	uint32 saved_base;        // Original baseAddr
	uint16 saved_row_bytes;   // Original rowBytes, flag bits included
	int16 saved_bounds[4];    // Original bounds: top, left, bottom, right
	uint32 buffer_addr;       // Our pixel buffer, in the arena
	uint32 buffer_size;
	int32 row_bytes;          // Ours, flag bits masked off
	int32 width;
	int32 height;
	int16 pixel_size;
	bool in_update;           // Between _BeginUpdate and _EndUpdate
};
static std::vector<RedirectedWindow> s_redirected;

/*
 * Pixel buffers are carved out of the NuBus video slot space above the real
 * framebuffer rather than allocated from the Mac heap.
 *
 * The guest never allocates there, so however many windows are open this cannot
 * exhaust the System heap or move anything the Memory Manager is tracking; and
 * the address is still guest-addressable, so QuickDraw writes into it happily
 * and the host reads it back with Mac2HostAddr().
 */
static const uint32 kArenaAlign = 0x10000;    // 64K, comfortably over any host page
static const uint32 kArenaLimit = 64u << 20;  // Ceiling on total pixel memory
static uint32 s_arena_base = 0;
static uint32 s_arena_cursor = 0;

// Blocks handed back by closed windows, reused before the cursor advances
struct ArenaBlock { uint32 addr; uint32 size; };
static std::vector<ArenaBlock> s_arena_free;

/*
 * Reserves a block of guest-addressable pixel memory.
 *
 * Returns:
 *   Guest address of the block, or 0 if the arena is exhausted.
 */
static uint32 arena_alloc(uint32 size)
{
	size = (size + 15) & ~15u;

	for (size_t i = 0; i < s_arena_free.size(); i++) {
		if (s_arena_free[i].size >= size) {
			uint32 addr = s_arena_free[i].addr;
			s_arena_free.erase(s_arena_free.begin() + i);
			return addr;
		}
	}

	if (!s_arena_base) {
		s_arena_base = (MacFrameBaseMac + MacFrameSize + kArenaAlign - 1) & ~(kArenaAlign - 1);
		s_arena_cursor = s_arena_base;
	}
	if (s_arena_cursor + size > s_arena_base + kArenaLimit)
		return 0;

	uint32 addr = s_arena_cursor;
	memory_commit_range(addr, size, MEMORY_PROT_READ | MEMORY_PROT_WRITE);
	if (!memory_is_mapped(addr, size))
		return 0;
	s_arena_cursor += size;
	return addr;
}

/*
 * Reserves a block of guest memory that the 68k may execute from.
 *
 * Used for the handful of thunks this module installs. Taking them from the
 * arena rather than the System heap means installing them needs no Memory
 * Manager call, which would itself be a Toolbox call made from the interrupt.
 * Pages stay data-mapped: the 68k JIT fetches opcodes as loads, and host
 * W^X rejects PROT_EXEC on the 4GB window.
 */
static uint32 arena_alloc_exec(uint32 size)
{
	uint32 addr = arena_alloc(size);
	if (!addr)
		return 0;
	// 68k fetch is a data load; host W^X rejects PROT_EXEC on this mapping
	memory_commit_range(addr, size, MEMORY_PROT_READ | MEMORY_PROT_WRITE);
	return addr;
}

/*
 * Returns a block to the arena. The pages stay committed; only the address is
 * recycled.
 */
static void arena_free(uint32 addr, uint32 size)
{
	if (!addr)
		return;
	ArenaBlock block;
	block.addr = addr;
	block.size = (size + 15) & ~15u;
	s_arena_free.push_back(block);
}

/*
 * Returns true when a guest address is plausible as a pointer into Mac RAM.
 *
 * Same defensive idiom as toolbox_menu.cpp: the window list is followed by
 * dereferencing guest pointers, and a half-built or torn-down record can hold
 * anything, so every hop is checked before it is taken.
 */
static inline bool valid_guest_ptr(uint32 addr)
{
	return addr >= 0x1000 && addr < RAMSize;
}

/*
 * Reads a Rect from guest memory into four int16s.
 */
static void read_rect(uint32 addr, int16 &top, int16 &left, int16 &bottom, int16 &right)
{
	top    = (int16)ReadMacInt16(addr + kRect_Top);
	left   = (int16)ReadMacInt16(addr + kRect_Left);
	bottom = (int16)ReadMacInt16(addr + kRect_Bottom);
	right  = (int16)ReadMacInt16(addr + kRect_Right);
}

/*
 * Writes a Rect to guest memory as top, left, bottom, right.
 */
static void write_rect(uint32 addr, int16 top, int16 left, int16 bottom, int16 right)
{
	WriteMacInt16(addr + kRect_Top, (uint16)top);
	WriteMacInt16(addr + kRect_Left, (uint16)left);
	WriteMacInt16(addr + kRect_Bottom, (uint16)bottom);
	WriteMacInt16(addr + kRect_Right, (uint16)right);
}

/*
 * Reads a region handle's bounding box. Regions are handles, so this is a
 * double dereference, both hops checked.
 *
 * Returns:
 *   true if the region was readable; the rect is left untouched otherwise.
 */
static bool read_region_bbox(uint32 rgn_handle, int16 &top, int16 &left, int16 &bottom, int16 &right)
{
	if (!valid_guest_ptr(rgn_handle))
		return false;
	uint32 rgn = ReadMacInt32(rgn_handle);
	if (!valid_guest_ptr(rgn))
		return false;
	read_rect(rgn + kRgn_BBox, top, left, bottom, right);
	return true;
}

/*
 * Returns true if the port at this address is a colour CGrafPort rather than a
 * classic B&W GrafPort.
 *
 * The discriminator is the standard one: a CGrafPort's portVersion word has its
 * top two bits set, where a GrafPort has portBits.rowBytes in the same place and
 * a row byte count never reaches 0x4000.
 */
static inline bool port_is_color(uint32 port)
{
	return (ReadMacInt16(port + kPort_PortVersion) & 0xc000) == 0xc000;
}

/*
 * Returns true if this window record is really a Process Manager layer rather
 * than an application window.
 */
static inline bool window_is_layer(uint32 window)
{
	return ReadMacInt16(window + kPort_TxSize) == kIsLayer;
}

/*
 * Returns true if this window is the desktop backdrop rather than an
 * application's window.
 *
 * The test is against GrayRgn, the region describing the whole desktop across
 * every screen: the desk window is by definition the one that covers it. Going
 * by title or windowKind would be guesswork, but the grey region is what the
 * Window Manager itself uses to mean "the desktop".
 */
// Latched once identified, because the desk window's content region shrinks to
// whatever part of the desktop is still uncovered as soon as anything is opened
// on top of it, and would stop matching the grey region
static uint32 s_desktop_window = 0;

enum {
	kWinOp_Select     = 0,
	kWinOp_Size       = 1,
	kWinOp_Move       = 2,
	kWinOp_Close      = 3,
	kWinOp_BringFront = 4, // diagnostic: the two halves of Select, on their own
	kWinOp_Hilite     = 5,
	kWinOp_SizeNoUpd  = 6, // diagnostic: _SizeWindow with fUpdate false
	kWinOp_Invalidate = 7, // _InvalRect over the whole content, in the window's port
	kWinOp_ScreenResized = 8 // guest screen changed; a/b are the new width/height
};

// MOVEM.L D0-D7/A0-A6 at the top of the filter stub: fifteen registers
enum { kFilterSavedRegs = 60 };

enum {
	// The stub occupies the first 56 bytes; the rest is its data and the routine
	kCode_InvalRect  = 56,  // unused; kept so later offsets stay put
	kCode_DMState    = 56,  // Handle for DMBeginConfigureDisplays
	kCode_DMDepth    = 60,  // unsigned long* desiredDepthMode
	kCode_Flag       = 64,  // byte: a routine is waiting to be run
	kCode_Busy       = 65,  // byte: the routine is running now
	kCode_Done       = 66,  // word: sequence number of the last routine to finish
	kCode_OldFilter  = 68,  // long: the filter we chain to
	kCode_SavedPort  = 72,  // long: GetPort's result while a routine runs
	kCode_Scratch    = 76,  // the assembled routine
	kCode_ScratchMax = 180,
	kCode_Size       = 256
};

static uint32 s_code_base = 0;
static uint32 s_gne_stub = 0;
static uint32 s_gne_old_filter = 0; // storage holding the previous filter
static uint32 s_flag_addr = 0;
static uint32 s_busy_addr = 0;
static uint32 s_done_addr = 0;
static uint32 s_scratch = 0;
static uint32 s_saved_port = 0;     // scratch space for the port GetPort saves
static uint16 s_scratch_seq = 0;    // bumped per assembled routine
static uint16 s_scratch_reported = 0;
static bool s_gne_installed = false;
static bool s_screen_resize_pending = false;
static int16 s_applied_w = 0, s_applied_h = 0;

// Defined with the safe-point machinery further down
static void queue_window_op(int type, uint32 window, int16 a, int16 b);

static bool window_is_desktop(uint32 window, const MacWindowSnapshot &w)
{
	if (s_desktop_window && window == s_desktop_window)
		return true;

	int16 top, left, bottom, right;
	if (!read_region_bbox(ReadMacInt32(LM_GrayRgn), top, left, bottom, right))
		return false;
	if (w.content_top <= top && w.content_left <= left &&
	    w.content_bottom >= bottom && w.content_right >= right) {
		s_desktop_window = window;
		return true;
	}
	return false;
}

/*
 * Walks WindowList and decodes every application window, frontmost first.
 *
 * WindowList is global rather than per-process: the Process Manager's low memory
 * save table explicitly excludes it (ProcessMgr/LomemTab.Color.a:41 saves
 * "toolbox globals except WindowList"), so a single walk sees every running
 * application's windows.
 *
 * The list is flat, with each layer record immediately followed by the windows
 * belonging to it -- LayerMgr.c:1617 walks a layer's children starting from
 * layer->nextWindow. So the most recently passed layer record is the owner of
 * every window until the next one, which is what gives each window its
 * layer_ptr and hence its application grouping.
 */
bool Toolbox_SnapshotWindowList(std::vector<MacWindowSnapshot> &snapshot_out)
{
	snapshot_out.clear();

	uint32 window = ReadMacInt32(LM_WindowList);
	uint32 current_layer = 0;
	int z = 0;

	for (int guard = 0; guard < kMaxWindows && valid_guest_ptr(window); guard++) {
		uint32 next = ReadMacInt32(window + kWin_NextWindow);

		if (window_is_layer(window)) {
			// Layer record: not a window of its own, but it names the owner
			// of everything that follows until the next layer.
			current_layer = window;
			window = next;
			continue;
		}

		int16 window_kind = (int16)ReadMacInt16(window + kWin_WindowKind);

		// Desk accessories and system windows are left alone for now: they are
		// owned by the system rather than by an application, and mirroring them
		// would put DA windows in an application's host window group.
		if (window_kind >= 0) {
			// Zero the geometry explicitly rather than memset()ing the whole
			// record: it holds a std::string, which must not be overwritten.
			MacWindowSnapshot w;
			w.content_top = w.content_left = w.content_bottom = w.content_right = 0;
			w.struc_top = w.struc_left = w.struc_bottom = w.struc_right = 0;
			w.window_ptr = window;
			w.layer_ptr = current_layer;
			w.window_kind = window_kind;
			w.is_dialog = (window_kind == kWindowKind_Dialog);

			/*
			 * The Window Manager stores the procID's variation code in the high
			 * byte of the windowDefProc handle, where the handle itself only
			 * needs 24 bits. That is the only part of the procID the record
			 * keeps, and it is enough to tell a document window from an alert.
			 */
			w.variant = (int16)((ReadMacInt32(window + kWin_WindowDefProc) >> 24) & 0x0f);
			w.go_away = ReadMacInt8(window + kWin_GoAwayFlag) != 0;
			w.visible = ReadMacInt8(window + kWin_Visible) != 0;
			w.hilited = ReadMacInt8(window + kWin_Hilited) != 0;
			w.z_order = z++;

			if (!read_region_bbox(ReadMacInt32(window + kWin_ContRgn),
			                      w.content_top, w.content_left,
			                      w.content_bottom, w.content_right)) {
				// A window with no content region is mid-construction; it will
				// be picked up on a later pass once the Window Manager has
				// finished building it.
				window = next;
				continue;
			}
			read_region_bbox(ReadMacInt32(window + kWin_StrucRgn),
			                 w.struc_top, w.struc_left,
			                 w.struc_bottom, w.struc_right);

			// A window with an empty content rectangle has been created but not
			// yet given a size. Leaving it out rather than mirroring a zero-sized
			// host window means it is adopted properly on a later walk, once the
			// application has laid it out.
			if (w.content_right <= w.content_left || w.content_bottom <= w.content_top) {
				window = next;
				continue;
			}

			// The desktop is a window too, covering the whole grey region. It is
			// the backdrop rather than anything an application owns, so mirroring
			// it would put a full-screen host window behind everything else.
			if (window_is_desktop(window, w)) {
				window = next;
				continue;
			}

			uint32 title_handle = ReadMacInt32(window + kWin_TitleHandle);
			if (valid_guest_ptr(title_handle)) {
				uint32 title_ptr = ReadMacInt32(title_handle);
				if (valid_guest_ptr(title_ptr))
					w.title = ToolboxArgs::ReadPascalString(title_ptr);
			}

			snapshot_out.push_back(w);
		}

		window = next;
	}

	return !snapshot_out.empty();
}

/*
 * Decodes a dialog window's item list.
 *
 * A 'DITL' resource is a count word followed by variable-length entries, each
 * being a 4-byte placeholder the Dialog Manager overwrites with the item's
 * handle, the item rectangle in local coordinates, a type byte, a length byte
 * and that many bytes of data, padded to an even boundary.
 *
 * Rects are converted to global coordinates on the way out so a host click can
 * be aimed back at the item without the caller redoing the arithmetic.
 */
bool Toolbox_SnapshotDialogItems(uint32 window_ptr,
                                 std::vector<MacDialogItem> &items_out,
                                 int *default_item_out)
{
	items_out.clear();
	if (default_item_out)
		*default_item_out = 0;

	if (!valid_guest_ptr(window_ptr))
		return false;
	if ((int16)ReadMacInt16(window_ptr + kWin_WindowKind) != kWindowKind_Dialog)
		return false;

	uint32 items_handle = ReadMacInt32(window_ptr + kDlg_Items);
	if (!valid_guest_ptr(items_handle))
		return false;
	uint32 ditl = ReadMacInt32(items_handle);
	if (!valid_guest_ptr(ditl))
		return false;

	if (default_item_out)
		*default_item_out = (int16)ReadMacInt16(window_ptr + kDlg_ADefItem);

	// Local-to-global offset: the port's local origin sits at the content
	// region's top-left corner.
	int16 c_top, c_left, c_bottom, c_right;
	if (!read_region_bbox(ReadMacInt32(window_ptr + kWin_ContRgn),
	                      c_top, c_left, c_bottom, c_right))
		return false;
	int16 p_top, p_left, p_bottom, p_right;
	read_rect(window_ptr + kPort_PortRect, p_top, p_left, p_bottom, p_right);
	const int16 dx = c_left - p_left;
	const int16 dy = c_top - p_top;

	// The count word holds one less than the number of items
	int count = (int16)ReadMacInt16(ditl) + 1;
	if (count <= 0 || count > 256)
		return false;

	uint32 cursor = ditl + 2;
	for (int i = 1; i <= count; i++) {
		if (!valid_guest_ptr(cursor))
			return false;

		cursor += 4; // placeholder the Dialog Manager fills in with the handle

		MacDialogItem item;
		item.index = i;
		read_rect(cursor, item.top, item.left, item.bottom, item.right);
		cursor += 8;
		item.top    = (int16)(item.top + dy);
		item.bottom = (int16)(item.bottom + dy);
		item.left   = (int16)(item.left + dx);
		item.right  = (int16)(item.right + dx);

		uint8 raw_type = (uint8)ReadMacInt8(cursor++);
		item.type = raw_type & kDialogItem_TypeMask;
		item.enabled = (raw_type & kDialogItem_DisableBit) == 0;

		uint8 len = (uint8)ReadMacInt8(cursor++);
		// Only text-bearing items carry a title worth reading; icons and
		// pictures store a resource ID in the same bytes.
		if (item.type == kDialogItem_Button || item.type == kDialogItem_CheckBox ||
		    item.type == kDialogItem_RadioButton || item.type == kDialogItem_StaticText ||
		    item.type == kDialogItem_EditText) {
			item.text.reserve(len);
			for (uint8 j = 0; j < len; j++)
				item.text.push_back((char)ReadMacInt8(cursor + j));
		}
		cursor += len;
		if (len & 1)
			cursor++; // entries are word aligned

		items_out.push_back(item);
	}

	return !items_out.empty();
}

/*
 * A click the host asked for, played back into the guest over the next few
 * interrupts.
 *
 * The press and release are deliberately spread across separate interrupts: the
 * guest samples the mouse from its own polling loop, and a down and up delivered
 * within one tick can be missed entirely.
 */
static int16 s_click_x = 0;
static int16 s_click_y = 0;
static int s_click_phase = -1; // -1 when idle

// Opt-in automatic dismissal of alerts, enabled with COCKATRICE_AUTO_DISMISS_ALERTS
static uint32 s_auto_dismiss_window = 0;
static int s_auto_dismiss_countdown = 0;

// Set when the synthetic input is a keypress rather than a click
static bool s_click_is_key = false;
static const uint8 kKeyReturn = 0x24;

/*
 * Advances the synthetic input, one step per interrupt.
 *
 * The steps are spread over several interrupts rather than done at once. The
 * guest samples input from its own polling loop, so a press and release inside a
 * single tick can pass unnoticed; and for a click, the pointer has to be moved
 * and that movement pushed to the low memory globals by ADBInterrupt before the
 * button goes down, or the guest registers the press at the old location.
 */
static void process_pending_click(void)
{
	if (s_click_phase < 0)
		return;

	if (s_click_is_key) {
		if (s_click_phase == 0)
			ADBKeyDown(kKeyReturn);
		else if (s_click_phase >= 3) {
			ADBKeyUp(kKeyReturn);
			s_click_phase = -1;
			return;
		}
	} else {
		if (s_click_phase == 0)
			ADBMouseMoved(s_click_x, s_click_y);   // let the position land first
		else if (s_click_phase == 2)
			ADBMouseDown(0);
		else if (s_click_phase >= 6) {
			ADBMouseUp(0);
			s_click_phase = -1;
			return;
		}
	}
	s_click_phase++;
}

/*
 * Presses the dialog's default button using the Return key.
 *
 * The Dialog Manager maps Return onto the default item itself, so this needs no
 * coordinates and works even for a dialog whose item rectangles we could not
 * decode.
 */
void Toolbox_PressDialogDefault(void)
{
	s_click_is_key = true;
	s_click_phase = 0;
	printf("[TOOLBOX-WIN] pressing Return to activate the dialog's default button\n");
	fflush(stdout);
}

/*
 * Clicks a dialog item on the user's behalf.
 *
 * The click is aimed at the middle of the item's rectangle and delivered through
 * the ordinary ADB mouse path, so the guest's Dialog Manager sees exactly what it
 * would see from a real click -- no reaching inside ModalDialog required.
 */
void Toolbox_ClickDialogItem(uint32 window_ptr, int item_index)
{
	std::vector<MacDialogItem> items;
	int default_item = 0;
	if (!Toolbox_SnapshotDialogItems(window_ptr, items, &default_item))
		return;

	for (size_t i = 0; i < items.size(); i++) {
		if (items[i].index != item_index)
			continue;
		s_click_x = (int16)((items[i].left + items[i].right) / 2);
		s_click_y = (int16)((items[i].top + items[i].bottom) / 2);
		s_click_is_key = false;
		s_click_phase = 0;
		printf("[TOOLBOX-WIN] clicking dialog item %d (\"%s\") at (%d,%d); ADBBase($0CF8)=0x%08X\n",
		       item_index, items[i].text.c_str(), s_click_x, s_click_y,
		       (unsigned)ReadMacInt32(0xcf8));
		fflush(stdout);
		return;
	}
}

// Defined below, next to the queues they drain
static void process_input_queue(void);
static void ensure_gne_filter(void);

/*
 * Rewrites a region handle to be a plain rectangle.
 *
 * This is done by writing the region record rather than by calling _RectRgn,
 * because all of this runs from the 60 Hz interrupt and QuickDraw is not
 * re-entrant: calling into it while the guest happens to be midway through its
 * own drawing corrupts QuickDraw's state. A rectangular region is just a ten
 * byte record -- a size word and the bounding box -- so it can be written
 * directly and safely. The handle's memory block keeps its original size, which
 * is harmless; only the region's own length field shrinks.
 */
static void set_rect_region(uint32 rgn_handle, int16 top, int16 left, int16 bottom, int16 right)
{
	if (!valid_guest_ptr(rgn_handle))
		return;
	uint32 rgn = ReadMacInt32(rgn_handle);
	if (!valid_guest_ptr(rgn))
		return;

	WriteMacInt16(rgn + kRgn_Size, 10);
	WriteMacInt16(rgn + kRgn_BBox + kRect_Top, (uint16)top);
	WriteMacInt16(rgn + kRgn_BBox + kRect_Left, (uint16)left);
	WriteMacInt16(rgn + kRgn_BBox + kRect_Bottom, (uint16)bottom);
	WriteMacInt16(rgn + kRgn_BBox + kRect_Right, (uint16)right);
}

/*
 * Bits per pixel for the logical VideoMonitor.mode, used as ChunkyDepth
 * so the cursor expand picks the matching blit (CCrsrCore.a JSR (A3)).
 */
static int guest_pixel_size(void)
{
	switch (VideoMonitor.mode) {
	case VMODE_1BIT:  return 1;
	case VMODE_2BIT:  return 2;
	case VMODE_4BIT:  return 4;
	case VMODE_8BIT:  return 8;
	case VMODE_16BIT: return 16;
	case VMODE_32BIT: return 32;
	default:          return 8;
	}
}

/*
 * Copies the new screen size into the GDevice pixmap, Window Manager
 * ports, and the cursor low-memory Display Manager writes after
 * cscSwitchMode (DisplayMgr.c FixLowMem / DMMoveCursor).
 *
 * CrsrRow / CrsrPin / CrsrBase are what the cursor VBL blits with. Leaving
 * them at the old pitch is why a shrink garbled the cursor and a grow
 * address-errored at ROM $4082E78A (JSR (A3) with a stale expand vector).
 * pixelSize and the CLUT stay with InitGDevice; we only call _AllocCursor
 * afterwards so the expanded cursor matches this pitch.
 *
 * Arguments:
 *   width, height: Pixel size VideoMonitor now describes.
 */
static void apply_guest_screen_geometry(int16 width, int16 height)
{
	if (width <= 0 || height <= 0)
		return;

	uint32 row_bytes = VideoMonitor.bytes_per_row;
	uint32 base = VideoMonitor.mac_frame_base;

	uint32 gd_handle = ReadMacInt32(LM_TheGDevice);
	if (valid_guest_ptr(gd_handle)) {
		uint32 gd = ReadMacInt32(gd_handle);
		if (valid_guest_ptr(gd)) {
			write_rect(gd + kGD_Rect, 0, 0, height, width);
			// 0 forces AllocCursor to rebuild the expanded cursor for this pitch
			WriteMacInt16(gd + kGD_CCDepth, 0);
			uint32 pm_handle = ReadMacInt32(gd + kGD_PMap);
			if (valid_guest_ptr(pm_handle)) {
				uint32 pixmap = ReadMacInt32(pm_handle);
				if (valid_guest_ptr(pixmap)) {
					WriteMacInt32(pixmap + kPix_BaseAddr, base);
					uint16 flags = (uint16)ReadMacInt16(pixmap + kPix_RowBytes) & 0xc000;
					WriteMacInt16(pixmap + kPix_RowBytes, (uint16)(row_bytes | flags));
					write_rect(pixmap + kPix_Bounds, 0, 0, height, width);
				}
			}
		}
	}

	WriteMacInt32(LM_ScrnBase, base);
	WriteMacInt16(LM_ScreenBits + 4, (uint16)row_bytes);
	write_rect(LM_ScreenBits + 6, 0, 0, height, width);
	WriteMacInt16(LM_ScreenRow, (uint16)row_bytes);

	// DisplayMgr.c DMMoveCursor: cursor blit uses these, not the pixmap
	WriteMacInt32(LM_CrsrBase, base);
	WriteMacInt16(LM_CrsrRow, (uint16)(row_bytes & 0x7fff));
	write_rect(LM_CrsrPin, 0, 0, height, width);
	WriteMacInt16(LM_ChunkyDepth, (uint16)guest_pixel_size());
	WriteMacInt16(LM_RowBits, (uint16)width);
	WriteMacInt16(LM_ColLines, (uint16)height);
	WriteMacInt32(LM_ScreenBytes, row_bytes * (uint32)height);

	set_rect_region(ReadMacInt32(LM_GrayRgn), 0, 0, height, width);

	uint32 wm_cport = ReadMacInt32(LM_WMgrCPort);
	if (valid_guest_ptr(wm_cport)) {
		write_rect(wm_cport + kPort_PortRect, 0, 0, height, width);
		if (port_is_color(wm_cport)) {
			uint32 pm_handle = ReadMacInt32(wm_cport + kPort_PortPixMap);
			if (valid_guest_ptr(pm_handle)) {
				uint32 pixmap = ReadMacInt32(pm_handle);
				if (valid_guest_ptr(pixmap)) {
					WriteMacInt32(pixmap + kPix_BaseAddr, base);
					uint16 flags = (uint16)ReadMacInt16(pixmap + kPix_RowBytes) & 0xc000;
					WriteMacInt16(pixmap + kPix_RowBytes, (uint16)(row_bytes | flags));
					write_rect(pixmap + kPix_Bounds, 0, 0, height, width);
				}
			}
		}
	}

	uint32 wm_port = ReadMacInt32(LM_WMgrPort);
	if (valid_guest_ptr(wm_port)) {
		WriteMacInt32(wm_port + kPort_PortBitsBase, base);
		WriteMacInt16(wm_port + kPort_PortBitsRow, (uint16)row_bytes);
		write_rect(wm_port + kPort_PortBitsBound, 0, 0, height, width);
		write_rect(wm_port + kPort_PortRect, 0, 0, height, width);
	}

	if (s_desktop_window && valid_guest_ptr(s_desktop_window)) {
		write_rect(s_desktop_window + kPort_PortRect, 0, 0, height, width);
		if (port_is_color(s_desktop_window)) {
			uint32 pm_handle = ReadMacInt32(s_desktop_window + kPort_PortPixMap);
			if (valid_guest_ptr(pm_handle)) {
				uint32 pixmap = ReadMacInt32(pm_handle);
				if (valid_guest_ptr(pixmap)) {
					uint16 flags = (uint16)ReadMacInt16(pixmap + kPix_RowBytes) & 0xc000;
					WriteMacInt16(pixmap + kPix_RowBytes, (uint16)(row_bytes | flags));
					write_rect(pixmap + kPix_Bounds, 0, 0, height, width);
				}
			}
		}
	}

	printf("[TOOLBOX-WIN] screen geometry now %dx%d mode %d (rowBytes %u)\n",
	       (int)width, (int)height, VideoMonitor.mode, row_bytes);
	fflush(stdout);
}

/*
 * Finds the redirection record for a window, or NULL.
 */
static RedirectedWindow *find_redirected(uint32 window_ptr)
{
	for (size_t i = 0; i < s_redirected.size(); i++) {
		if (s_redirected[i].window_ptr == window_ptr)
			return &s_redirected[i];
	}
	return NULL;
}

/*
 * Returns the PixMap handle belonging to the screen, so we can tell it apart
 * from a window's own.
 */
static uint32 screen_pixmap_handle(void)
{
	uint32 gd_handle = ReadMacInt32(LM_TheGDevice);
	if (!valid_guest_ptr(gd_handle))
		return 0;
	uint32 gd = ReadMacInt32(gd_handle);
	if (!valid_guest_ptr(gd))
		return 0;
	return ReadMacInt32(gd + kGD_PMap);
}

/*
 * Points a window's port at a private pixel buffer, so its drawing no longer
 * lands in the screen framebuffer.
 *
 * The window's own PixMap record is rewritten in place rather than swapped for a
 * fabricated handle. That is safe because every colour port gets its own PixMap:
 * OpenCPort calls NewPixMap for each one (QuickDraw/ColorAsm.a:155). The handle
 * is still compared against the screen's before anything is touched, so a port
 * that turns out to share the screen's PixMap is left alone rather than
 * redirecting every window at once.
 *
 * Returns:
 *   true if the window is now drawing into its own buffer.
 */
/*
 * The baseAddr to install so that local (0,0) addresses the start of our buffer
 * while the port's bounds -- and with it every local/global conversion in the
 * Toolbox -- is left alone. See the note in redirect_window().
 *
 * Args:
 *   pixmap: the window's PixMap record.
 *   buffer: guest address of the offscreen pixels.
 *   row_bytes: our buffer's row stride.
 *   pixel_size: bits per pixel.
 */
static uint32 redirected_base_addr(uint32 pixmap, uint32 buffer,
                                   int32 row_bytes, int16 pixel_size)
{
	int16 b_top, b_left, b_bottom, b_right;
	read_rect(pixmap + kPix_Bounds, b_top, b_left, b_bottom, b_right);
	return buffer + (uint32)((int32)b_top * row_bytes +
	                         (int32)b_left * (pixel_size / 8));
}

static bool redirect_window(const MacWindowSnapshot &w)
{
	if (!s_redirect_enabled || find_redirected(w.window_ptr))
		return false;

	const uint32 port = w.window_ptr;

	// Only colour ports for now. A classic 1-bit GrafPort keeps its bitmap
	// inline instead of behind a PixMap handle, so it needs its own path.
	if (!port_is_color(port)) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			printf("[TOOLBOX-WIN] window \"%s\" uses a B&W GrafPort; redirection covers colour ports only\n",
			       w.title.c_str());
			fflush(stdout);
		}
		return false;
	}

	uint32 pixmap_handle = ReadMacInt32(port + kPort_PortPixMap);
	if (!valid_guest_ptr(pixmap_handle))
		return false;
	if (pixmap_handle == screen_pixmap_handle()) {
		printf("[TOOLBOX-WIN] window \"%s\" shares the screen PixMap; not redirecting\n",
		       w.title.c_str());
		fflush(stdout);
		return false;
	}
	uint32 pixmap = ReadMacInt32(pixmap_handle);
	if (!valid_guest_ptr(pixmap))
		return false;

	int16 p_top, p_left, p_bottom, p_right;
	read_rect(port + kPort_PortRect, p_top, p_left, p_bottom, p_right);
	int32 width = p_right - p_left;
	int32 height = p_bottom - p_top;
	if (width <= 0 || height <= 0)
		return false;

	int16 pixel_size = (int16)ReadMacInt16(pixmap + kPix_PixelSize);
	if (pixel_size != 8 && pixel_size != 16 && pixel_size != 32)
		return false;

	// PixMap rows are padded to a multiple of four bytes
	int32 row_bytes = (((width * pixel_size) + 31) / 32) * 4;
	uint32 size = (uint32)row_bytes * (uint32)height;
	uint32 buffer = arena_alloc(size);
	if (!buffer) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			printf("[TOOLBOX-WIN] offscreen arena exhausted; further windows stay on the screen buffer\n");
			fflush(stdout);
		}
		return false;
	}
	/*
	 * Start the buffer white, so a window that has not repainted yet reads as
	 * blank rather than as noise. In an indexed 8-bit port that is index 0 --
	 * the standard Mac colour table puts white first and black last, so filling
	 * with 0xff would paint the window solid black.
	 */
	Mac_memset(buffer, pixel_size == 8 ? 0x00 : 0xff, size);

	RedirectedWindow rec;
	rec.window_ptr = w.window_ptr;
	rec.pixmap_addr = pixmap;
	rec.saved_base = ReadMacInt32(pixmap + kPix_BaseAddr);
	rec.saved_row_bytes = (uint16)ReadMacInt16(pixmap + kPix_RowBytes);
	read_rect(pixmap + kPix_Bounds, rec.saved_bounds[0], rec.saved_bounds[1],
	          rec.saved_bounds[2], rec.saved_bounds[3]);
	rec.buffer_addr = buffer;
	rec.buffer_size = size;
	rec.row_bytes = row_bytes;
	rec.width = width;
	rec.height = height;
	rec.pixel_size = pixel_size;
	rec.in_update = false;

	/*
	 * Point the port at our buffer, and leave bounds exactly as it was.
	 *
	 * bounds is not free to change. QuickDraw addresses a pixel at local (h,v)
	 * as baseAddr + (v - bounds.top) * rowBytes + (h - bounds.left) * depth, and
	 * it is the *same* field that carries the port's position on the screen:
	 * _LocalToGlobal is a subtraction of bounds.topLeft, which for a window is
	 * the negated position of its content. Setting bounds to portRect makes the
	 * addressing come out right and tells the rest of the Toolbox that the
	 * window sits at global (0,0) -- after which the Window Manager rebuilds
	 * contRgn there, mouse coordinates land in the wrong place, and an
	 * application drawing through a globally derived rect draws outside its own
	 * visRgn and appears to paint nothing at all.
	 *
	 * So bounds stays, and baseAddr absorbs the offset instead: with
	 *   baseAddr = buffer + bounds.top * rowBytes + bounds.left * depth
	 * the expression above gives buffer + v * rowBytes + h * depth for local
	 * (h,v), which is what we want, while every coordinate conversion in the
	 * system still works. baseAddr itself then points below the buffer and is
	 * never dereferenced there; only the sum is.
	 */
	WriteMacInt32(pixmap + kPix_BaseAddr, redirected_base_addr(pixmap, buffer, row_bytes, pixel_size));
	WriteMacInt16(pixmap + kPix_RowBytes, (uint16)(row_bytes | kPix_RowBytesFlag));

	s_redirected.push_back(rec);

	// Nothing clips this window any more, so open its visible region up to the
	// whole port. visRgn is in local coordinates.
	set_rect_region(ReadMacInt32(port + kPort_VisRgn), p_top, p_left, p_bottom, p_right);

	/*
	 * Mark the whole content as needing redraw, so the application repaints into
	 * the new buffer at its next update event. updateRgn is in global
	 * coordinates.
	 *
	 * Writing updateRgn is not by itself enough for every application. The
	 * Window Manager keeps its own idea of what is dirty, and some windows never
	 * come back for an update event they were not properly told about -- which
	 * is what leaves a redirected window showing nothing but its blank buffer.
	 * So a real _InvalRect is queued as well, for the safe point to make.
	 */
	set_rect_region(ReadMacInt32(port + kWin_UpdateRgn),
	                w.content_top, w.content_left, w.content_bottom, w.content_right);
	queue_window_op(kWinOp_Invalidate, w.window_ptr, 0, 0);

	printf("[TOOLBOX-WIN] redirected \"%s\" to buffer 0x%08X (%dx%d, %d bpp, %d bytes/row); "
	       "portRect (%d,%d)-(%d,%d), bounds (%d,%d)-(%d,%d), baseAddr 0x%08X\n",
	       w.title.c_str(), (unsigned)buffer, (int)width, (int)height,
	       (int)pixel_size, (int)row_bytes,
	       (int)p_left, (int)p_top, (int)p_right, (int)p_bottom,
	       (int)rec.saved_bounds[1], (int)rec.saved_bounds[0],
	       (int)rec.saved_bounds[3], (int)rec.saved_bounds[2],
	       (unsigned)ReadMacInt32(pixmap + kPix_BaseAddr));
	fflush(stdout);
	return true;
}

/*
 * Undoes a redirection.
 *
 * Arguments:
 *   window_ptr: The window to release.
 *   port_valid: true while the port still exists, in which case its PixMap is
 *               restored first. False when the window has already gone, where
 *               writing to it would corrupt whatever now owns that memory.
 */
static void restore_window(uint32 window_ptr, bool port_valid)
{
	for (size_t i = 0; i < s_redirected.size(); i++) {
		RedirectedWindow &rec = s_redirected[i];
		if (rec.window_ptr != window_ptr)
			continue;

		if (port_valid && valid_guest_ptr(rec.pixmap_addr)) {
			WriteMacInt32(rec.pixmap_addr + kPix_BaseAddr, rec.saved_base);
			WriteMacInt16(rec.pixmap_addr + kPix_RowBytes, rec.saved_row_bytes);
			WriteMacInt16(rec.pixmap_addr + kPix_Bounds + kRect_Top, (uint16)rec.saved_bounds[0]);
			WriteMacInt16(rec.pixmap_addr + kPix_Bounds + kRect_Left, (uint16)rec.saved_bounds[1]);
			WriteMacInt16(rec.pixmap_addr + kPix_Bounds + kRect_Bottom, (uint16)rec.saved_bounds[2]);
			WriteMacInt16(rec.pixmap_addr + kPix_Bounds + kRect_Right, (uint16)rec.saved_bounds[3]);
		}

		arena_free(rec.buffer_addr, rec.buffer_size);
		s_redirected.erase(s_redirected.begin() + i);
		return;
	}
}

/*
 * Puts each redirected window's visible region back to its full port rectangle.
 *
 * The Window Manager recomputes visRgn whenever windows are shown, selected,
 * moved or repainted -- CalcVis, PaintOne, ClipAbove and friends -- clipping a
 * window to whatever is not covering it. A redirected window has its own buffer
 * and nothing covers it, so that clipping is wrong and has to be undone.
 *
 * Re-asserting once per interrupt is deliberately cheaper and more robust than
 * hooking every trap that touches visRgn: whichever one did it, the region is
 * corrected within a frame. The only period left alone is between _BeginUpdate
 * and _EndUpdate, where the Window Manager has narrowed visRgn to the update
 * region on purpose and overwriting it would let the application draw outside
 * the area it was asked to refresh.
 */
/*
 * Whether a redirection record still describes the window it was made for.
 *
 * baseAddr is re-derived from the PixMap's live bounds on every pass, which is
 * what lets a moved window keep addressing its buffer. That makes the bounds
 * field trusted input, and it must not be: a disposed window's PixMap record is
 * ordinary heap that the guest will reuse for something else, and a garbage
 * bounds turns into a baseAddr the guest then draws through.
 *
 * The decisive check is that the window's port still points at the same PixMap.
 * The range check behind it catches a record that is stale but not yet reused,
 * where the fields are plausible individually and wrong together.
 */
static bool redirect_still_ours(const RedirectedWindow &rec)
{
	if (!valid_guest_ptr(rec.window_ptr) || !valid_guest_ptr(rec.pixmap_addr))
		return false;

	uint32 handle = ReadMacInt32(rec.window_ptr + kPort_PortPixMap);
	if (!valid_guest_ptr(handle) || ReadMacInt32(handle) != rec.pixmap_addr)
		return false;

	int16 b_top, b_left, b_bottom, b_right;
	read_rect(rec.pixmap_addr + kPix_Bounds, b_top, b_left, b_bottom, b_right);
	if (b_right - b_left < 1 || b_bottom - b_top < 1)
		return false;

	// A window's bounds is the screen rect offset by the negated window origin,
	// so both corners stay within a screen's reach of zero
	const int32 kReach = 32768;
	if (b_top < -kReach || b_top > kReach || b_left < -kReach || b_left > kReach)
		return false;

	return true;
}

static void reassert_vis_rgns(void)
{
	for (size_t i = 0; i < s_redirected.size(); i++) {
		RedirectedWindow &rec = s_redirected[i];
		if (rec.in_update || !valid_guest_ptr(rec.window_ptr))
			continue;

		/*
		 * Put the port back on our buffer if the Window Manager has moved it.
		 * Showing, hiding or moving a window recomputes its PixMap from the
		 * screen's, which quietly points the window back at the framebuffer.
		 *
		 * baseAddr is derived from the *current* bounds every time rather than
		 * remembered, so a window that has been moved keeps addressing the
		 * buffer correctly without needing the move to be noticed separately.
		 */
		if (redirect_still_ours(rec)) {
			uint32 want_base = redirected_base_addr(rec.pixmap_addr, rec.buffer_addr,
			                                        rec.row_bytes, rec.pixel_size);
			uint16 want_row = (uint16)(rec.row_bytes | kPix_RowBytesFlag);
			if (ReadMacInt32(rec.pixmap_addr + kPix_BaseAddr) != want_base)
				WriteMacInt32(rec.pixmap_addr + kPix_BaseAddr, want_base);
			if (ReadMacInt16(rec.pixmap_addr + kPix_RowBytes) != want_row)
				WriteMacInt16(rec.pixmap_addr + kPix_RowBytes, want_row);
		}

		int16 p_top, p_left, p_bottom, p_right;
		read_rect(rec.window_ptr + kPort_PortRect, p_top, p_left, p_bottom, p_right);

		uint32 vis_rgn = ReadMacInt32(rec.window_ptr + kPort_VisRgn);
		int16 v_top, v_left, v_bottom, v_right;
		if (!read_region_bbox(vis_rgn, v_top, v_left, v_bottom, v_right))
			continue;

		if (v_top != p_top || v_left != p_left || v_bottom != p_bottom || v_right != p_right)
			set_rect_region(vis_rgn, p_top, p_left, p_bottom, p_right);
	}
}

/*
 * Returns true if two snapshots of the same window differ in anything the host
 * needs to act on.
 */
static bool window_changed(const MacWindowSnapshot &a, const MacWindowSnapshot &b)
{
	return a.content_top    != b.content_top    ||
	       a.content_left   != b.content_left   ||
	       a.content_bottom != b.content_bottom ||
	       a.content_right  != b.content_right  ||
	       a.visible        != b.visible        ||
	       a.hilited        != b.hilited        ||
	       a.z_order        != b.z_order        ||
	       a.layer_ptr      != b.layer_ptr      ||
	       a.title          != b.title;
}

/*
 * Finds a window in a snapshot vector by its WindowPtr identity.
 *
 * Returns:
 *   Index into the vector, or -1 if absent.
 */
static int find_window(const std::vector<MacWindowSnapshot> &list, uint32 window_ptr)
{
	for (size_t i = 0; i < list.size(); i++) {
		if (list[i].window_ptr == window_ptr)
			return (int)i;
	}
	return -1;
}

/*
 * Re-walks the window list and reconciles it against the previous walk.
 *
 * Runs on the CPU thread from the IRQ path, which is the same place the menu
 * bar sync runs and for the same reason: the guest is between instructions, so
 * its window records are in a consistent state and reading them is safe.
 */
void Toolbox_ProcessPendingWindowSync(void)
{
	if (!s_active)
		return;

	/*
	 * The interrupt fires while the guest is part way through the window calls
	 * we assembled for it. Walking and re-asserting visRgn in the middle of a
	 * _SizeWindow means fighting the Window Manager over the same fields, so
	 * skip the frame; the next one sees the settled result.
	 */
	if (s_busy_addr && ReadMacInt8(s_busy_addr))
		return;

	ensure_gne_filter();
	process_pending_click();
	process_input_queue();
	reassert_vis_rgns();

	// Alerts scheduled for automatic dismissal are left a moment first: the
	// window exists as soon as the walk sees it, but the Dialog Manager is not
	// yet inside ModalDialog watching for the click.
	if (s_auto_dismiss_countdown > 0 && --s_auto_dismiss_countdown == 0) {
		std::vector<MacDialogItem> items;
		int default_item = 0;
		if (Toolbox_SnapshotDialogItems(s_auto_dismiss_window, items, &default_item))
			Toolbox_PressDialogDefault();
		s_auto_dismiss_window = 0;
	}

	std::vector<MacWindowSnapshot> current;
	Toolbox_SnapshotWindowList(current);

	// Announce the first successful walk, for the same reason the menu client
	// does: every stage of a working hook is silent, so without this the log
	// can show the subsystem registered and never say whether it sees anything.
	static bool announced = false;
	if (!announced && !current.empty()) {
		announced = true;
		printf("[TOOLBOX-WIN] window mirroring is live; %d window(s):\n", (int)current.size());
		for (size_t i = 0; i < current.size(); i++) {
			const MacWindowSnapshot &w = current[i];
			printf("[TOOLBOX-WIN]   \"%s\" ptr=0x%08X kind=%d content=(%d,%d)-(%d,%d) struc=(%d,%d)-(%d,%d) %s\n",
			       w.title.c_str(), (unsigned)w.window_ptr, w.window_kind,
			       w.content_left, w.content_top, w.content_right, w.content_bottom,
			       w.struc_left, w.struc_top, w.struc_right, w.struc_bottom,
			       w.visible ? "visible" : "hidden");
		}
		fflush(stdout);
	}

	/*
	 * Retire windows that have gone. Walk the old list so a window disposed
	 * without passing through our teardown hook is still noticed.
	 *
	 * A window has to be missing from several consecutive walks before it is
	 * believed. WindowList is a global that the Process Manager swaps as part of
	 * a context switch, so a walk can land mid-switch and come back short --
	 * once with every window absent. Retiring on that destroys and rebuilds
	 * every host window, which in turn produces a burst of focus and move
	 * notifications from Cocoa that get sent back to the guest. Waiting a few
	 * frames costs nothing: a genuinely closed window stays closed.
	 */
	for (size_t i = 0; i < s_last_snapshot.size(); i++) {
		uint32 ptr = s_last_snapshot[i].window_ptr;
		if (find_window(current, ptr) >= 0) {
			s_missing.erase(ptr);
			continue;
		}

		if (++s_missing[ptr] < kMissingWalksToRetire) {
			// Keep it in the snapshot so the next walk still knows about it
			current.push_back(s_last_snapshot[i]);
			continue;
		}

		printf("[TOOLBOX-WIN] window 0x%08X \"%s\" missing from %d walks and never "
		       "closed through a hook; retiring\n",
		       (unsigned)ptr, s_last_snapshot[i].title.c_str(),
		       (int)kMissingWalksToRetire);
		fflush(stdout);
		s_missing.erase(ptr);
		// The window is already off the list, so its port must not be touched
		restore_window(ptr, false);
		if (s_callbacks_set && s_callbacks.window_removed)
			s_callbacks.window_removed(ptr);
	}

	// Adopt new windows and report changes to surviving ones.
	for (size_t i = 0; i < current.size(); i++) {
		const MacWindowSnapshot &w = current[i];
		int prev = find_window(s_last_snapshot, w.window_ptr);
		if (prev < 0) {
			redirect_window(w);
			if (s_callbacks_set && s_callbacks.window_added)
				s_callbacks.window_added(&w);
			if (w.is_dialog && !s_auto_dismiss_window &&
			    getenv("COCKATRICE_AUTO_DISMISS_ALERTS")) {
				s_auto_dismiss_window = w.window_ptr;
				s_auto_dismiss_countdown = 60; // about a second at 60 Hz
			}
		} else if (window_changed(s_last_snapshot[prev], w)) {
			// A resized window needs a buffer of the new size, so its old one is
			// handed back and the port redirected again from scratch.
			RedirectedWindow *rec = find_redirected(w.window_ptr);
			if (rec) {
				int16 p_top, p_left, p_bottom, p_right;
				read_rect(w.window_ptr + kPort_PortRect, p_top, p_left, p_bottom, p_right);
				if ((p_right - p_left) != rec->width || (p_bottom - p_top) != rec->height) {
					restore_window(w.window_ptr, true);
					redirect_window(w);
				}
			}
			if (s_callbacks_set && s_callbacks.window_changed)
				s_callbacks.window_changed(&w);
		}
	}

	/*
	 * Self-test for the safe-point path, enabled with COCKATRICE_WINDOW_SELFTEST.
	 *
	 * Host input cannot be driven from a headless run, so this stands in for it:
	 * it queues what a focused, dragged or closed host window would queue, and
	 * the later walks report whether the guest actually acted on it.
	 *
	 * The variable's value picks which calls to make, so that one build can
	 * establish which Window Manager entry points survive being called from the
	 * jGNEFilter safe point:
	 *
	 *   front | hilite | select | size | size0 | move | close
	 *
	 * Several may be given, separated by commas; anything else means
	 * "size,select", which is what a host resize followed by a focus produces.
	 */
	if (getenv("COCKATRICE_WINDOW_SELFTEST")) {
		static int countdown = -1;
		static uint32 target = 0;

		// Start counting only once real application windows exist, not from the
		// first walk -- during boot that is still minutes away.
		if (countdown < 0) {
			for (size_t k = 0; k < s_last_snapshot.size(); k++) {
				if (!s_last_snapshot[k].is_dialog) {
					countdown = 180; // three seconds for the Finder to settle
					break;
				}
			}
		}

		if (countdown > 0 && --countdown == 0) {
			// The largest window: most likely a document window, so it has a
			// grow box for the resize half of the test to drag.
			size_t best = s_last_snapshot.size();
			int32 best_area = 0;
			for (size_t k = 0; k < s_last_snapshot.size(); k++) {
				const MacWindowSnapshot &c = s_last_snapshot[k];
				if (c.is_dialog)
					continue;
				int32 area = (int32)(c.content_right - c.content_left) *
				             (int32)(c.content_bottom - c.content_top);
				if (area > best_area) { best_area = area; best = k; }
			}
			for (size_t k = best; k < s_last_snapshot.size(); k = s_last_snapshot.size()) {
				const MacWindowSnapshot &w = s_last_snapshot[k];
				target = w.window_ptr;
				int16 nw = (int16)((w.content_right - w.content_left) - 80);
				int16 nh = (int16)((w.content_bottom - w.content_top) - 60);

				std::string what = getenv("COCKATRICE_WINDOW_SELFTEST");
				if (what.find("front")  == std::string::npos &&
				    what.find("hilite") == std::string::npos &&
				    what.find("select") == std::string::npos &&
				    what.find("size")   == std::string::npos &&
				    what.find("move")   == std::string::npos &&
				    what.find("close")  == std::string::npos)
					what = "size,select";

				printf("[TOOLBOX-WIN] selftest: \"%s\" z=%d %dx%d at (%d,%d); running %s\n",
				       w.title.c_str(), w.z_order,
				       w.content_right - w.content_left,
				       w.content_bottom - w.content_top,
				       w.content_left, w.content_top, what.c_str());
				fflush(stdout);

				if (what.find("front") != std::string::npos)
					queue_window_op(kWinOp_BringFront, target, 0, 0);
				if (what.find("hilite") != std::string::npos)
					queue_window_op(kWinOp_Hilite, target, 1, 0);
				if (what.find("size0") != std::string::npos)
					queue_window_op(kWinOp_SizeNoUpd, target, nw, nh);
				else if (what.find("size") != std::string::npos)
					Toolbox_ResizeGuestWindow(target, nw, nh);
				if (what.find("move") != std::string::npos)
					Toolbox_MoveGuestWindow(target,
					                        (int16)(w.content_left + 40),
					                        (int16)(w.content_top + 30));
				if (what.find("select") != std::string::npos)
					Toolbox_SelectGuestWindow(target);
				if (what.find("close") != std::string::npos)
					Toolbox_DispatchGuestWindowClose(target);
				break;
			}
		}
		if (target) {
			int idx = find_window(s_last_snapshot, target);
			if (idx >= 0) {
				static int reported = 0;
				const MacWindowSnapshot &w = s_last_snapshot[idx];
				if (reported++ == 180) {
					printf("[TOOLBOX-WIN] selftest: \"%s\" is now %dx%d at (%d,%d), front=%s\n",
					       w.title.c_str(),
					       w.content_right - w.content_left,
					       w.content_bottom - w.content_top,
					       w.content_left, w.content_top,
					       w.z_order == 0 ? "yes" : "no");
					fflush(stdout);
				}
			}
		}
	}

	s_last_snapshot.swap(current);
}

/*
 * Returns the screen depth in bits per pixel, or 0 for a mode we cannot slice
 * into per-window rectangles by simple pointer arithmetic.
 */
static int screen_pixel_size(void)
{
	switch (VideoMonitor.mode) {
		case VMODE_8BIT:  return 8;
		case VMODE_16BIT: return 16;
		case VMODE_32BIT: return 32;
		default:          return 0; // sub-byte pixels need bit addressing
	}
}

/*
 * Writes one window's presented pixels to a binary PPM, for checking by eye that
 * what we hand the host is really that window's content.
 *
 * Enabled by setting COCKATRICE_WINDOW_DUMP to a directory. Each window is
 * dumped once, after its content has had time to settle. 8-bit indexed pixels
 * are expanded through the guest palette so the file has true colour.
 */
static void dump_window_ppm(const MacWindowSnapshot &w, const MacWindowBuffer &buf)
{
	const char *dir = getenv("COCKATRICE_WINDOW_DUMP");
	if (!dir || buf.pixel_size != 8 || !s_palette_valid)
		return;

	char path[1024];
	snprintf(path, sizeof(path), "%s/window_%08X.ppm", dir, (unsigned)w.window_ptr);
	FILE *f = fopen(path, "wb");
	if (!f)
		return;

	fprintf(f, "P6\n%d %d\n255\n", (int)buf.width, (int)buf.height);
	for (int32 y = 0; y < buf.height; y++) {
		uint32 row = buf.base_addr + (uint32)y * (uint32)buf.row_bytes;
		for (int32 x = 0; x < buf.width; x++) {
			uint8 idx = (uint8)ReadMacInt8(row + (uint32)x);
			fwrite(&s_palette[idx * 3], 1, 3, f);
		}
	}
	fclose(f);

	printf("[TOOLBOX-WIN] dumped \"%s\" pixels to %s\n", w.title.c_str(), path);
	fflush(stdout);
}

/*
 * Hands each mirrored window's current pixels to the platform bridge.
 *
 * Until port redirection lands, the pixels come straight out of the screen
 * framebuffer: a window's content region bounding box is in global coordinates,
 * so the rows of that rectangle are simply a sub-rectangle of the screen. That
 * is enough to prove the whole path -- discovery, geometry, presentation --
 * before anything starts rewriting guest ports, and it doubles as the fallback
 * if redirection turns out to be unworkable.
 *
 * The known limitation of reading the screen is that overlapping windows share
 * those pixels, so a window with something on top of it shows whatever is
 * covering it. Redirection is what fixes that.
 */
void Toolbox_PresentWindows(void)
{
	if (!s_active || !s_callbacks_set || !s_callbacks.window_present)
		return;

	int pixel_size = screen_pixel_size();
	if (pixel_size == 0)
		return;

	const uint32 row_bytes = VideoMonitor.bytes_per_row;
	const int32 bytes_per_pixel = pixel_size / 8;

	for (size_t i = 0; i < s_last_snapshot.size(); i++) {
		const MacWindowSnapshot &w = s_last_snapshot[i];
		if (!w.visible)
			continue;

		// A redirected window owns its pixels outright, so they are handed over
		// as they stand -- no clipping, and nothing another window has drawn.
		const RedirectedWindow *rec = find_redirected(w.window_ptr);
		if (rec) {
			MacWindowBuffer rbuf;
			rbuf.base_addr = rec->buffer_addr;
			rbuf.row_bytes = rec->row_bytes;
			rbuf.width = rec->width;
			rbuf.height = rec->height;
			rbuf.pixel_size = rec->pixel_size;
			s_callbacks.window_present(w.window_ptr, &rbuf);

			static int rframe = 0;
			if (rframe == 120)
				dump_window_ppm(w, rbuf);
			if (i + 1 == s_last_snapshot.size() && rframe <= 120)
				rframe++;
			continue;
		}

		// Clip the content rect to the screen before turning it into an
		// address: a window dragged partly off screen would otherwise index
		// outside the framebuffer.
		int32 left   = w.content_left   < 0 ? 0 : w.content_left;
		int32 top    = w.content_top    < 0 ? 0 : w.content_top;
		int32 right  = w.content_right  > (int32)VideoMonitor.x ? (int32)VideoMonitor.x : w.content_right;
		int32 bottom = w.content_bottom > (int32)VideoMonitor.y ? (int32)VideoMonitor.y : w.content_bottom;
		if (right <= left || bottom <= top)
			continue;

		MacWindowBuffer buf;
		buf.base_addr = VideoMonitor.mac_frame_base + (uint32)top * row_bytes +
		                (uint32)left * (uint32)bytes_per_pixel;
		buf.row_bytes = (int32)row_bytes;
		buf.width = right - left;
		buf.height = bottom - top;
		buf.pixel_size = (int16)pixel_size;

		s_callbacks.window_present(w.window_ptr, &buf);

		// Dump each window once, after enough frames that its content has been
		// drawn rather than caught mid-update.
		static int frame = 0;
		if (frame == 120)
			dump_window_ppm(w, buf);
		if (i + 1 == s_last_snapshot.size() && frame <= 120)
			frame++;
	}
}

/*
 * Nothing above this point executes guest code.
 *
 * Everything the walk does is either a direct memory write -- region records,
 * PixMap fields -- or a synthetic ADB event, because it runs from the 60 Hz
 * interrupt, where the Toolbox cannot be entered. Window Manager calls are made
 * instead from the safe point below.
 */

/*
 * Calling the Window Manager, and where from.
 *
 * Selecting, resizing, moving and closing a window are done by calling the real
 * Toolbox routines. The difficulty is never the call itself, it is the context
 * it is made from, and two attempts established where that is not:
 *
 *   - From the 60 Hz interrupt. Calling QuickDraw there killed the guest with a
 *     Type 10 at PC=$A0000000, having jumped into the framebuffer. The interrupt
 *     can land anywhere, including inside QuickDraw's own non-reentrant code.
 *   - From a trap hook on _WaitNextEvent. An Address Error about thirty seconds
 *     in; the same run without the hook was clean. Replacing that trap's address
 *     is not transparent, whatever the hook does.
 *
 * The mechanism that does work is the one Apple provided for it: jGNEFilter, the
 * low memory vector the Event Manager calls on the way out of GetNextEvent and
 * EventAvail. That is an application's own context with no Toolbox call in
 * progress, and it is where the system itself does this kind of work -- the
 * Notification Manager's filter draws in the menu bar from there
 * (Toolbox/NotificationMgr/NotificationMgrPatch.a:90).
 *
 * A stub is installed in that vector holding an EmulOp, so the work lands back
 * in Toolbox_WindowSafePoint() below. The stub then chains to whatever filter
 * was there before, following the same convention the system's own filters use
 * (ToolboxEventMgr.a:265-280).
 *
 * Reaching that context is necessary but not sufficient. Calling the Window
 * Manager from the EmulOp handler with Execute68k does not work either: a bare
 * RTS returns, and so does _FrontWindow, but _BringToFront and _SizeWindow never
 * come back. The reason is that in System 7 those entry points are the Process
 * Manager's (Toolbox/WindowMgr/LayerMgr.c), and moving a window between layers
 * can yield to the scheduler. A yield inside a nested Execute68k resumes some
 * other process, so control never returns to the C++ frame that started it.
 *
 * So the EmulOp handler does not call anything. It assembles the pending calls
 * into a short 68k routine in our arena, sets a flag, and returns; the stub then
 * JSRs to that routine as ordinary guest code. Now a yield is just a yield --
 * the application blocks inside its own GetNextEvent, which is exactly where it
 * was going anyway, and resumes normally afterwards.
 */
struct WindowOp {
	int type;
	uint32 window;
	int16 a; // width, or horizontal position
	int16 b; // height, or vertical position
};
static std::vector<WindowOp> s_pending_ops;

// Guest code, allocated from our own arena so that installing it needs no
// Memory Manager call -- which would itself be a Toolbox call from the interrupt

/*
 * Builds the jGNEFilter stub.
 *
 * The stub saves the registers, drops into our EmulOp, and -- if that left a
 * routine waiting -- JSRs to it before restoring and chaining on to the filter
 * that was there before:
 *
 *      MOVEM.L D0-D7/A0-A6,-(SP)
 *      <EmulOp>                     assemble the routine, set the flag
 *      TST.B   (flag).L
 *      BEQ.S   chain
 *      CLR.B   (flag).L             one shot
 *      ST      (busy).L             so a nested filter call does not overwrite
 *      JSR     (scratch).L          the Window Manager calls, in guest flow
 *      CLR.B   (busy).L
 *  chain:
 *      MOVEM.L (SP)+,D0-D7/A0-A6
 *      MOVE.L  (oldFilter).L,-(SP)
 *      TST.L   (SP)
 *      BNE.S   *+4
 *      ADDQ.L  #4,SP                no previous filter: return to the caller
 *      RTS
 *
 * Returns:
 *   true once the code is in place.
 */
static bool ensure_guest_code(void)
{
	if (s_code_base)
		return true;

	uint32 base = arena_alloc_exec(kCode_Size);
	if (!base)
		return false;

	s_code_base = base;
	s_gne_stub = base;
	s_flag_addr = base + kCode_Flag;
	s_busy_addr = base + kCode_Busy;
	s_done_addr = base + kCode_Done;
	s_gne_old_filter = base + kCode_OldFilter;
	s_saved_port = base + kCode_SavedPort;
	s_scratch = base + kCode_Scratch;

	WriteMacInt16(base + 0, 0x48e7);  // MOVEM.L D0-D7/A0-A6,-(SP)
	WriteMacInt16(base + 2, 0xfffe);
	WriteMacInt16(base + 4, M68K_EMUL_OP_WINDOW_SAFEPOINT);
	WriteMacInt16(base + 6, 0x4a39);  // TST.B (flag).L
	WriteMacInt32(base + 8, s_flag_addr);
	WriteMacInt16(base + 12, 0x6718); // BEQ.S chain (+38)
	WriteMacInt16(base + 14, 0x4239); // CLR.B (flag).L
	WriteMacInt32(base + 16, s_flag_addr);
	WriteMacInt16(base + 20, 0x50f9); // ST (busy).L
	WriteMacInt32(base + 22, s_busy_addr);
	WriteMacInt16(base + 26, 0x4eb9); // JSR (scratch).L
	WriteMacInt32(base + 28, s_scratch);
	WriteMacInt16(base + 32, 0x4239); // CLR.B (busy).L
	WriteMacInt32(base + 34, s_busy_addr);
	// chain:
	WriteMacInt16(base + 38, 0x4cdf); // MOVEM.L (SP)+,D0-D7/A0-A6
	WriteMacInt16(base + 40, 0x7fff);
	WriteMacInt16(base + 42, 0x2f39); // MOVE.L (oldFilter).L,-(SP)
	WriteMacInt32(base + 44, s_gne_old_filter);
	WriteMacInt16(base + 48, 0x4a97); // TST.L (SP)
	WriteMacInt16(base + 50, 0x6602); // BNE.S over the ADDQ
	WriteMacInt16(base + 52, 0x588f); // ADDQ.L #4,SP
	WriteMacInt16(base + 54, 0x4e75); // RTS

	WriteMacInt8(s_flag_addr, 0);
	WriteMacInt8(s_busy_addr, 0);
	WriteMacInt16(s_done_addr, 0);
	WriteMacInt16(s_scratch, 0x4e75); // RTS, in case it is ever entered empty

	cpu_engine_invalidate_code(base, kCode_Size);
	return true;
}

/*
 * Assembles the pending window operations into the scratch routine.
 *
 * Toolbox traps use the Pascal convention, in which the callee removes the
 * arguments -- Apple's own source declares these entries that way, for instance
 * "pascal void __CloseWindow(WindowPtr window)"
 * (Toolbox/WindowMgr/LayerMgr.c:2044) -- so each call is just its arguments
 * pushed followed by the trap word, with no stack adjustment afterwards.
 *
 * Args:
 *   ops: the operations to emit, in order.
 *
 * Returns:
 *   true if anything was emitted and the flag should be set.
 */
static bool assemble_window_calls(const std::vector<WindowOp> &ops)
{
	uint32 pc = s_scratch;
	uint32 limit = s_scratch + kCode_ScratchMax - 12; // room for the tail
	int emitted = 0;

	// MOVE.L #imm,-(SP) / MOVE.W #imm,-(SP)
	#define PUSH_L(v) do { WriteMacInt16(pc, 0x2f3c); WriteMacInt32(pc + 2, (v)); pc += 6; } while (0)
	#define PUSH_W(v) do { WriteMacInt16(pc, 0x3f3c); WriteMacInt16(pc + 2, (uint16)(v)); pc += 4; } while (0)
	#define TRAP(t)   do { WriteMacInt16(pc, (t)); pc += 2; } while (0)

	for (size_t i = 0; i < ops.size(); i++) {
		const WindowOp &op = ops[i];

		// Screen-resize has no window; everything else may have been disposed
		if (op.type != kWinOp_ScreenResized) {
			if (find_window(s_last_snapshot, op.window) < 0) {
				printf("[TOOLBOX-WIN] safe point: op=%d window 0x%08X is gone; skipped\n",
				       op.type, (unsigned)op.window);
				fflush(stdout);
				continue;
			}
		}
		if (pc + 128 > limit)
			break;

		switch (op.type) {
		case kWinOp_Select:
			PUSH_L(op.window);
			TRAP(kTrap_SelectWindow);
			break;

		case kWinOp_BringFront:
			PUSH_L(op.window);
			TRAP(kTrap_BringToFront);
			break;

		case kWinOp_Hilite:
			PUSH_L(op.window);
			PUSH_W(op.a);
			TRAP(kTrap_HiliteWindow);
			break;

		case kWinOp_Size:
		case kWinOp_SizeNoUpd:
			PUSH_L(op.window);
			PUSH_W(op.a);                                // width
			PUSH_W(op.b);                                // height
			PUSH_W(op.type == kWinOp_Size ? 1 : 0);      // fUpdate
			TRAP(kTrap_SizeWindow);
			break;

		case kWinOp_Move:
			PUSH_L(op.window);
			PUSH_W(op.a);                                // hGlobal
			PUSH_W(op.b);                                // vGlobal
			PUSH_W(0);                                   // do not bring to front
			TRAP(kTrap_MoveWindow);
			break;

		case kWinOp_Close:
			PUSH_L(op.window);
			TRAP(kTrap_CloseWindow);
			break;

		/*
		 * GetPort(&saved); SetPort(window); InvalRect(&window->portRect);
		 * SetPort(saved). _InvalRect works on the current port, so the window's
		 * port has to be made current around it and the caller's put back --
		 * this runs inside the application's GetNextEvent, which will carry on
		 * drawing afterwards and would otherwise find itself in the wrong port.
		 */
		case kWinOp_Invalidate:
			WriteMacInt16(pc, 0x4879);            // PEA (saved).L
			WriteMacInt32(pc + 2, s_saved_port);
			pc += 6;
			TRAP(kTrap_GetPort);
			PUSH_L(op.window);
			TRAP(kTrap_SetPort);
			PUSH_L(op.window + kPort_PortRect);
			TRAP(kTrap_InvalRect);
			WriteMacInt16(pc, 0x2f39);            // MOVE.L (saved).L,-(SP)
			WriteMacInt32(pc + 2, s_saved_port);
			pc += 6;
			TRAP(kTrap_SetPort);
			break;

		/*
		 * Display Manager mode switch (Displays.a). Assembled here so a
		 * yield inside InitGDevice / FixPorts is a normal GetNextEvent
		 * yield, not a nested Execute68kTrap that never returns.
		 *
		 *   DMBeginConfigureDisplays(&state)
		 *   DMSetDisplayMode(TheGDevice, modeID, &depth, nil, state)
		 *   DMEndConfigureDisplays(state)
		 *   DMDrawDesktopRect(&newScreen)
		 *
		 * op.window is the DisplayModeID; a/b are the new width/height
		 * used only for the desktop rect. Selectors are
		 * (paramWords<<8)|select from Displays.a.
		 */
		case kWinOp_ScreenResized: {
			uint32 state = s_code_base + kCode_DMState;
			uint32 depth = s_code_base + kCode_DMDepth;
			uint32 mode_id = op.window;

			WriteMacInt16(pc, 0x4267); pc += 2;                 // CLR.W -(SP)
			WriteMacInt16(pc, 0x4879);                          // PEA state
			WriteMacInt32(pc + 2, state);
			pc += 6;
			WriteMacInt16(pc, 0x303c);                          // MOVE.W #$0206,D0
			WriteMacInt16(pc + 2, 0x0206);
			pc += 4;
			TRAP(kTrap_DisplayDispatch);
			WriteMacInt16(pc, 0x301f); pc += 2;                 // MOVE.W (SP)+,D0

			WriteMacInt16(pc, 0x4267); pc += 2;
			WriteMacInt16(pc, 0x2f39);                          // MOVE.L TheGDevice,-(SP)
			WriteMacInt32(pc + 2, LM_TheGDevice);
			pc += 6;
			WriteMacInt16(pc, 0x2f3c);                          // MOVE.L #modeID,-(SP)
			WriteMacInt32(pc + 2, mode_id);
			pc += 6;
			WriteMacInt16(pc, 0x4879);                          // PEA depth
			WriteMacInt32(pc + 2, depth);
			pc += 6;
			WriteMacInt16(pc, 0x42a7); pc += 2;                 // CLR.L -(SP) switchModeInfo
			WriteMacInt16(pc, 0x2f39);                          // MOVE.L state.L,-(SP)
			WriteMacInt32(pc + 2, state);
			pc += 6;
			WriteMacInt16(pc, 0x303c);
			WriteMacInt16(pc + 2, 0x0a11);
			pc += 4;
			TRAP(kTrap_DisplayDispatch);
			WriteMacInt16(pc, 0x301f); pc += 2;

			WriteMacInt16(pc, 0x4267); pc += 2;
			WriteMacInt16(pc, 0x2f39);
			WriteMacInt32(pc + 2, state);
			pc += 6;
			WriteMacInt16(pc, 0x303c);
			WriteMacInt16(pc + 2, 0x0207);
			pc += 4;
			TRAP(kTrap_DisplayDispatch);
			WriteMacInt16(pc, 0x301f); pc += 2;

			// Rect on the stack as top,left,bottom,right (last push is at SP)
			PUSH_W(op.a);                                       // right
			PUSH_W(op.b);                                       // bottom
			PUSH_W(0);                                          // left
			PUSH_W(0);                                          // top
			WriteMacInt16(pc, 0x4857); pc += 2;                 // PEA (SP)
			WriteMacInt16(pc, 0x303c);
			WriteMacInt16(pc + 2, 0x0202);
			pc += 4;
			TRAP(kTrap_DisplayDispatch);
			WriteMacInt16(pc, 0x508f); pc += 2;                 // ADDQ.L #8,SP
			break;
		}

		default:
			continue;
		}

		printf("[TOOLBOX-WIN] safe point: emitted op=%d window=0x%08X a=%d b=%d\n",
		       op.type, (unsigned)op.window, op.a, op.b);
		fflush(stdout);
		emitted++;
	}

	if (!emitted)
		return false;

	// MOVE.W #seq,(done).L, so the next pass can report that the guest ran it
	WriteMacInt16(pc, 0x33fc);
	WriteMacInt16(pc + 2, ++s_scratch_seq);
	WriteMacInt32(pc + 4, s_done_addr);
	pc += 8;
	WriteMacInt16(pc, 0x4e75); // RTS
	pc += 2;

	#undef PUSH_L
	#undef PUSH_W
	#undef TRAP

	cpu_engine_invalidate_code(s_scratch, pc - s_scratch);
	return true;
}

/*
 * Puts our stub into the jGNEFilter vector, chaining whatever was there before.
 *
 * This is only memory writes, so it is safe to do from the interrupt; the code
 * it installs is what later runs in a safe context.
 */
static void ensure_gne_filter(void)
{
	if (!s_safepoint_wanted || !ensure_guest_code())
		return;

	// Reinstall if the ROM overwrote the vector after an early install
	if (s_gne_installed) {
		if (ReadMacInt32(LM_JGNEFilter) == s_gne_stub)
			return;
		s_gne_installed = false;
	}

	uint32 previous = ReadMacInt32(LM_JGNEFilter);

	// Do not chain to ourselves if this is a reinstall after a reset
	if (previous == s_gne_stub)
		previous = 0;

	WriteMacInt32(s_gne_old_filter, previous);
	WriteMacInt32(LM_JGNEFilter, s_gne_stub);
	s_gne_installed = true;

	printf("[TOOLBOX-WIN] installed jGNEFilter stub at 0x%08X (chaining to 0x%08X)\n",
	       (unsigned)s_gne_stub, (unsigned)previous);
	fflush(stdout);
}

/*
 * Queues a window operation for the next safe point.
 */
static void queue_window_op(int type, uint32 window, int16 a, int16 b)
{
	// Nothing will ever run these with the subsystem off, so do not accumulate
	// them either
	if (!s_active || !valid_guest_ptr(window))
		return;

	/*
	 * Collapse repeats: a burst of host resize or drag events should end in one
	 * call. Only geometry collapses -- two selects of the same window are also
	 * redundant, but a queue that swallowed distinct requests would be a trap
	 * for anything queued later.
	 */
	if (type == kWinOp_Size || type == kWinOp_SizeNoUpd ||
	    type == kWinOp_Move || type == kWinOp_Invalidate) {
		for (size_t i = 0; i < s_pending_ops.size(); i++) {
			if (s_pending_ops[i].type == type && s_pending_ops[i].window == window) {
				s_pending_ops[i].a = a;
				s_pending_ops[i].b = b;
				return;
			}
		}
	}

	WindowOp op;
	op.type = type;
	op.window = window;
	op.a = a;
	op.b = b;
	s_pending_ops.push_back(op);
}

/*
 * Asks for the jGNEFilter safe point, and installs it once the arena is usable.
 *
 * Called every interrupt from the queue drain, so that the filter goes in as
 * soon as the guest is far enough along, whether or not window mirroring is on.
 */
void Toolbox_EnableSafePoint(void)
{
	// Installing the filter writes a low-memory vector. toolbox_hooks false
	// normally prevents that, but a pending screen-resize still needs the
	// jGNEFilter path -- that is where cscSwitchMode + geometry copy run.
	if (!ToolboxTrap_HooksEnabled() && !s_screen_resize_pending)
		return;

	s_safepoint_wanted = true;
	ensure_gne_filter();
}

/*
 * Returns true when a screen-resize op is waiting for the jGNEFilter stub.
 */
bool Toolbox_ScreenResizePending(void)
{
	return s_screen_resize_pending;
}

/*
 * Queues a cscSwitchMode + geometry + _AllocCursor for the next jGNEFilter.
 *
 * Must not poke QuickDraw structures here: Notify runs from VideoInterrupt.
 *
 * Arguments:
 *   width, height: New screen size in pixels.
 */
void Toolbox_NotifyScreenResized(int16 width, int16 height)
{
	s_screen_resize_pending = true;
	s_safepoint_wanted = true;

	for (size_t i = 0; i < s_pending_ops.size(); i++) {
		if (s_pending_ops[i].type == kWinOp_ScreenResized) {
			s_pending_ops[i].a = width;
			s_pending_ops[i].b = height;
			ensure_gne_filter();
			return;
		}
	}

	WindowOp op;
	op.type = kWinOp_ScreenResized;
	op.window = 0;
	op.a = width;
	op.b = height;
	s_pending_ops.push_back(op);
	ensure_gne_filter();
}

/*
 * Assembles any queued window operations for the filter stub to run.
 *
 * Called only from the jGNEFilter stub, by way of EmulOp. Window Manager
 * traps are assembled for the stub to run after we return. A screen-resize
 * assembles DMSetDisplayMode when Display Manager is present; otherwise
 * it calls cscSwitchMode here and copies geometry + _AllocCursor.
 */
void Toolbox_WindowSafePoint(M68kRegisters *r)
{
	static long hits = 0;
	if (++hits == 1) {
		printf("[TOOLBOX-WIN] jGNEFilter safe point reached; window calls can run here\n");
		fflush(stdout);
	}

	/*
	 * The Menu Manager client gets first refusal, because what it needs is the
	 * filter's own arguments rather than anything of ours: A1 is the event
	 * record the Event Manager is about to return, and the Boolean it will
	 * return sits above the return address (ToolboxEventMgr.a:265-280). Our stub
	 * saved fifteen registers on entry, so both are that much further up.
	 */
	if (r) {
		uint32 sp = r->a[7] + kFilterSavedRegs;
		Toolbox_MenuSafePoint(r->a[1], sp + 4);
	}

	if (!s_code_base)
		return;
	if (!s_active && s_pending_ops.empty())
		return;

	// Report the guest having finished the previous routine
	uint16 done = ReadMacInt16(s_done_addr);
	if (done != s_scratch_reported) {
		s_scratch_reported = done;
		printf("[TOOLBOX-WIN] safe point: guest completed routine #%u\n", (unsigned)done);
		fflush(stdout);
	}

	if (s_pending_ops.empty())
		return;

	/*
	 * A routine is still running further up the stack, or is waiting to be
	 * entered; leave the queue alone rather than rewriting the code underneath
	 * it. The watchdog covers the one way the busy byte can be left set: the
	 * application yielded inside a window call and never came back, having
	 * quit. Other applications keep reaching this filter, so it is cleared from
	 * here rather than stranding the queue.
	 */
	static int stalled = 0;
	if (ReadMacInt8(s_busy_addr) || ReadMacInt8(s_flag_addr)) {
		if (++stalled < 600)
			return;
		printf("[TOOLBOX-WIN] safe point: routine #%u never finished; clearing\n",
		       (unsigned)s_scratch_seq);
		fflush(stdout);
		WriteMacInt8(s_busy_addr, 0);
		WriteMacInt8(s_flag_addr, 0);
	}
	stalled = 0;

	std::vector<WindowOp> ops;
	ops.swap(s_pending_ops);
	s_screen_resize_pending = false;

	int16 geo_w = 0, geo_h = 0;
	for (size_t i = 0; i < ops.size(); i++) {
		if (ops[i].type == kWinOp_ScreenResized) {
			geo_w = ops[i].a;
			geo_h = ops[i].b;
		}
	}
	if (geo_w > 0 && geo_h > 0) {
		if (geo_w == s_applied_w && geo_h == s_applied_h) {
			std::vector<WindowOp> kept;
			for (size_t i = 0; i < ops.size(); i++) {
				if (ops[i].type != kWinOp_ScreenResized)
					kept.push_back(ops[i]);
			}
			ops.swap(kept);
		} else if (Video_DisplayManagerPresent() && s_code_base) {
			// DMSetDisplayMode from the stub (not Execute68kTrap): InitGDevice
			// with mainScreen cleared, FixPorts, AllocCursor, desktop paint.
			uint32 id = Video_RegisterGuestSize((int)geo_w, (int)geo_h);
			WriteMacInt32(s_code_base + kCode_DMState, 0);
			WriteMacInt32(s_code_base + kCode_DMDepth, Video_CurrentAppleMode());
			for (size_t i = 0; i < ops.size(); i++) {
				if (ops[i].type == kWinOp_ScreenResized)
					ops[i].window = id;
			}
			s_applied_w = geo_w;
			s_applied_h = geo_h;
			printf("[TOOLBOX-WIN] DMSetDisplayMode %dx%d id %08lx apple %04x\n",
			       (int)geo_w, (int)geo_h, (unsigned long)id,
			       (unsigned)Video_CurrentAppleMode());
			fflush(stdout);
		} else {
			std::vector<WindowOp> kept;
			for (size_t i = 0; i < ops.size(); i++) {
				if (ops[i].type != kWinOp_ScreenResized)
					kept.push_back(ops[i]);
			}
			ops.swap(kept);

			WriteMacInt8(LM_CrsrBusy, 1);
			int16 err = Video_GuestSwitchToSize((int)geo_w, (int)geo_h);
			if (err != noErr) {
				printf("[TOOLBOX-WIN] cscSwitchMode %dx%d failed (%d)\n",
				       (int)geo_w, (int)geo_h, (int)err);
				fflush(stdout);
			} else {
				apply_guest_screen_geometry(geo_w, geo_h);
				M68kRegisters cr;
				memset(&cr, 0, sizeof(cr));
				Execute68kTrap(kTrap_AllocCursor, &cr);
				s_applied_w = geo_w;
				s_applied_h = geo_h;
			}
			WriteMacInt8(LM_CrsrBusy, 0);
		}
	}

	if (assemble_window_calls(ops))
		WriteMacInt8(s_flag_addr, 1);
}

/*
 * Brings a guest window to the front and activates it.
 */
void Toolbox_SelectGuestWindow(uint32 window_ptr)
{
	queue_window_op(kWinOp_Select, window_ptr, 0, 0);
}

/*
 * Resizes a guest window to match its host window's content size.
 */
void Toolbox_ResizeGuestWindow(uint32 window_ptr, int16 width, int16 height)
{
	if (width < 64) width = 64;
	if (height < 64) height = 64;
	queue_window_op(kWinOp_Size, window_ptr, width, height);
}

/*
 * Moves a guest window so its content lands at the given global position.
 */
void Toolbox_MoveGuestWindow(uint32 window_ptr, int16 x, int16 y)
{
	queue_window_op(kWinOp_Move, window_ptr, x, y);
}

/*
 * Host input waiting to be played into the guest.
 *
 * Mouse and key events stay on the ADB path rather than becoming Toolbox calls:
 * ADB is driven from the interrupt by design -- ADBInterrupt() runs there
 * already -- and posting a real event is what an application expects to see.
 * What it needs is pacing, since the guest samples input from its own polling
 * loops and a transition delivered inside a single tick can pass unnoticed.
 */
struct InputEvent {
	int kind;
	int16 x;
	int16 y;
	int code;
};
static std::vector<InputEvent> s_input_queue;

/*
 * Interrupts to wait before the next input event, so a press lasts long enough
 * for the guest's tracking loops to see it.
 */
static int s_input_delay = 0;

/*
 * Appends an input event in guest global coordinates.
 */
static void queue_global_input(int kind, int16 x, int16 y, int code)
{
	InputEvent ev;
	ev.kind = kind;
	ev.x = x;
	ev.y = y;
	ev.code = code;

	// Successive moves are only worth the newest position
	if (kind == kGuestInput_MouseMove && !s_input_queue.empty() &&
	    s_input_queue.back().kind == kGuestInput_MouseMove) {
		s_input_queue.back() = ev;
		return;
	}

	if (s_input_queue.size() < 128)
		s_input_queue.push_back(ev);
}

/*
 * Forwards one host input event to the guest.
 */
void Toolbox_ForwardGuestInput(uint32 window_ptr, int kind, int16 x, int16 y, int code)
{
	if (kind == kGuestInput_KeyDown || kind == kGuestInput_KeyUp) {
		queue_global_input(kind, 0, 0, code);
		return;
	}

	// Content-local to guest global. The window has not moved on the guest
	// screen even when its pixels have been redirected elsewhere, so its content
	// origin is still the right offset.
	int idx = find_window(s_last_snapshot, window_ptr);
	if (idx < 0)
		return;
	queue_global_input(kind,
	                   (int16)(s_last_snapshot[idx].content_left + x),
	                   (int16)(s_last_snapshot[idx].content_top + y), 0);
}

/*
 * Plays queued host input into the guest, one transition per interrupt.
 */
static void process_input_queue(void)
{
	if (s_input_delay > 0) {
		s_input_delay--;
		return;
	}

	bool moved = false;

	while (!s_input_queue.empty()) {
		InputEvent ev = s_input_queue.front();

		if (ev.kind == kGuestInput_MouseMove) {
			ADBMouseMoved(ev.x, ev.y);
			s_input_queue.erase(s_input_queue.begin());
			moved = true;
			continue; // coalesce runs of movement
		}

		// A press or release must not follow movement within the same interrupt:
		// a tracking loop samples the pointer on its own schedule, and would see
		// the button go up before it ever saw the new position.
		if (moved) {
			s_input_delay = 3;
			return;
		}

		switch (ev.kind) {
		case kGuestInput_MouseDown:
			ADBMouseMoved(ev.x, ev.y);
			ADBMouseDown(0);
			break;
		case kGuestInput_MouseUp:
			ADBMouseMoved(ev.x, ev.y);
			ADBMouseUp(0);
			break;
		case kGuestInput_KeyDown:
			ADBKeyDown(ev.code);
			break;
		case kGuestInput_KeyUp:
			ADBKeyUp(ev.code);
			break;
		}
		s_input_queue.erase(s_input_queue.begin());
		s_input_delay = 5; // about 80ms, the length of an unhurried click
		return;
	}
}

/*
 * Asks the guest to close one mirrored window.
 *
 * The application still gets to decide: CloseWindow runs in its own context and
 * it can put up a "save changes?" dialog, which will appear as another mirrored
 * window.
 *
 * Arguments:
 *   window_ptr: WindowPtr of the window to close.
 */
void Toolbox_DispatchGuestWindowClose(uint32 window_ptr)
{
	queue_window_op(kWinOp_Close, window_ptr, 0, 0);
}

/*
 * Drops all mirrored window state.
 *
 * On machine reset every WindowPtr we hold points into a heap that no longer
 * exists, so the host must be told to tear its windows down rather than be left
 * matching new windows against stale identities.
 */
void ToolboxWindow_Reset(void)
{
	for (size_t i = 0; i < s_last_snapshot.size(); i++) {
		if (s_callbacks_set && s_callbacks.window_removed)
			s_callbacks.window_removed(s_last_snapshot[i].window_ptr);
	}
	s_last_snapshot.clear();
	s_desktop_window = 0;

	// The heap these windows lived in is gone, so nothing may be written back;
	// drop the records and let the arena start over.
	s_redirected.clear();
	s_arena_free.clear();
	s_arena_cursor = s_arena_base;
	s_input_queue.clear();
	s_pending_ops.clear();
	s_missing.clear();
	s_screen_resize_pending = false;
	s_applied_w = s_applied_h = 0;

	// The vector and the code behind it are gone with the heap; both are
	// rebuilt from the arena on the next walk.
	s_code_base = 0;
	s_gne_stub = 0;
	s_scratch = s_flag_addr = s_busy_addr = s_done_addr = s_saved_port = 0;
	s_scratch_seq = s_scratch_reported = 0;
	s_gne_installed = false;
}

/*
 * Records the guest colour table. Called from the video palette path, which is
 * portable, so the platform bridge does not have to be reachable from there.
 *
 * Arguments:
 *   entries: 256 RGB triples, one byte per component.
 */
void ToolboxWindow_SetPalette(const uint8 *entries)
{
	if (!entries)
		return;
	memcpy(s_palette, entries, sizeof(s_palette));
	s_palette_valid = true;
}

/*
 * Returns the recorded colour table, or NULL if none has been seen yet.
 */
const uint8 *ToolboxWindow_GetPalette(void)
{
	return s_palette_valid ? s_palette : NULL;
}

/*
 * Installs the platform bridge callbacks.
 */
void ToolboxWindow_SetCallbacks(const struct ToolboxWindowCallbacks *callbacks)
{
	if (callbacks) {
		s_callbacks = *callbacks;
		s_callbacks_set = true;
	} else {
		memset(&s_callbacks, 0, sizeof(s_callbacks));
		s_callbacks_set = false;
	}
}

/*
 * Trap hook for window teardown.
 *
 * This is one of the few things that genuinely cannot be discovered by the
 * walk-and-diff: by the time the next walk runs the window is already gone from
 * the list, and any per-window guest state we spliced into its port must be
 * unspliced while that port still exists. So the teardown is done here, before
 * the ROM routine runs, and the diff is left to report the disappearance.
 *
 * Arguments and return value: see ToolboxTrapHandler in toolbox_traps.h.
 */
static TOOLBOX_TRAP_HANDLER(Handle_WindowTeardown)
{
	ToolboxArgs args(r);
	uint32 window = args.PopPtr();

	if (valid_guest_ptr(window)) {
		// Put the port back before the ROM disposes it, while it is still intact
		restore_window(window, true);
		if (s_callbacks_set && s_callbacks.window_removed)
			s_callbacks.window_removed(window);
		int idx = find_window(s_last_snapshot, window);
		if (idx >= 0)
			s_last_snapshot.erase(s_last_snapshot.begin() + idx);
	}

	// Passthrough: the ROM still has to actually close the window
	return TOOLBOX_ACTION_PASSTHROUGH;
}

/*
 * Trap hooks bracketing an update.
 *
 * Between these two calls the Window Manager narrows visRgn to the region that
 * actually needs repainting, and the application relies on that to avoid drawing
 * the whole window. Our own visRgn re-assertion would undo it, so it stands down
 * for the duration.
 *
 * Arguments and return value: see ToolboxTrapHandler in toolbox_traps.h.
 */
static TOOLBOX_TRAP_HANDLER(Handle_BeginUpdate)
{
	ToolboxArgs args(r);
	RedirectedWindow *rec = find_redirected(args.PopPtr());
	if (rec)
		rec->in_update = true;
	return TOOLBOX_ACTION_PASSTHROUGH;
}

static TOOLBOX_TRAP_HANDLER(Handle_EndUpdate)
{
	ToolboxArgs args(r);
	RedirectedWindow *rec = find_redirected(args.PopPtr());
	if (rec)
		rec->in_update = false;
	return TOOLBOX_ACTION_PASSTHROUGH;
}

/*
 * Registers the Window Manager trap hooks with the Toolbox trap registry.
 *
 * Two prefs gate this. toolbox_hooks is the registry's own switch, checked here
 * as well as inside ToolboxTrap_Register() so a disabled subsystem states its
 * reason once rather than failing silently per trap. mdi_windows is this
 * experiment's own switch, so window mirroring can be turned off without giving
 * up the menu bar bridge.
 */
void ToolboxWindow_RegisterTraps(void)
{
	if (!ToolboxTrap_HooksEnabled()) {
		printf("[TOOLBOX-WIN] toolbox_hooks disabled; skipping Window Manager trap registration.\n");
		fflush(stdout);
		return;
	}
	if (!PrefsFindBool("mdi_windows")) {
		printf("[TOOLBOX-WIN] mdi_windows disabled; skipping Window Manager trap registration.\n");
		fflush(stdout);
		return;
	}

	ToolboxTrap_Register(kTrap_CloseWindow,   "_CloseWindow",   Handle_WindowTeardown, NULL);
	ToolboxTrap_Register(kTrap_DisposeWindow, "_DisposeWindow", Handle_WindowTeardown, NULL);

	/*
	 * Offscreen redirection is a separate switch from mirroring itself. It is
	 * the part most likely to upset an application -- anything that caches the
	 * screen base address, or draws straight to the screen rather than through
	 * its port, will misbehave -- so it can be turned off while still getting
	 * host windows, which is the safer configuration for games.
	 */
	s_redirect_enabled = PrefsFindBool("window_redirect");
	if (s_redirect_enabled) {
		ToolboxTrap_Register(kTrap_BeginUpdate, "_BeginUpdate", Handle_BeginUpdate, NULL);
		ToolboxTrap_Register(kTrap_EndUpdate,   "_EndUpdate",   Handle_EndUpdate,   NULL);
	}

	s_active = true;
	s_safepoint_wanted = true;

	printf("[TOOLBOX-WIN] Registered Window Manager trap hooks for host window mirroring "
	       "(offscreen redirection %s).\n",
	       s_redirect_enabled ? "on" : "off; windows read from the screen buffer");
	fflush(stdout);
}
