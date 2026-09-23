#include "sysdeps.h"

//#include <pthread.h>
#include <errno.h>
#include <string.h>


#include "cpu_emulation.h"
#include "cpu_engine.h"
#include "main.h"
#include "adb.h"
#include "macos_util.h"
#include "prefs.h"
#include "user_strings.h"
#include "video.h"
#include "version.h"
#include "menu_bar.h"
#include "toolbox_window.h"
#include "video_blit.h"

#define DEBUG 0
#include "debug.h"

#include <SDL/SDL.h>
#if defined(WIN32) || defined(_WIN32)
#include <SDL/SDL_syswm.h>
#endif
static SDL_Surface *SDLscreen = NULL;
static bool use_keycodes = false;	// Flag: Use keycodes rather than keysyms
static int keycode_table[256];		// X keycode -> Mac keycode translation table

// Last palette, reapplied after SDL_SetVideoMode recreates the surface
static uint8 s_saved_palette[256 * 3];
// The palette as host pixels of the 32-bit surface, for the indexed modes
static uint32 s_palette_pixels[256];
static bool s_have_palette = false;
static bool s_in_mode_switch = false;
// No SDL_HWPALETTE: sdl12-compat never records it on the surface, and on
// Windows it recreates the window on every SDL_SetVideoMode whose flags differ
// from the surface's, which dropped the Win32 menu bar on each mode change.
// Palettes are set with SDL_SetColors either way.
static const Uint32 kVideoSDLFlags = (SDL_SWSURFACE | SDL_RESIZABLE);

// Global variables
static int32 frame_skip;
static bool hide_cursor = true;
static int32 skip_count=0;
static int32 quitcount=0;
static int32 bytes_per_pixel;
int depth;	//how deep is the display
// Prefs items

static int16 mouse_wheel_mode = 1;
static int16 mouse_wheel_lines = 3;


static bool ctrl_down = false;						// Flag: Ctrl key pressed
static bool caps_on = false;						// Flag: Caps Lock on
static bool quit_full_screen = false;				// Flag: DGA close requested from redraw thread
static bool emerg_quit = false;						// Flag: Ctrl-Esc pressed, emergency quit requested from MacOS thread
static bool emul_suspended = false;					// Flag: Emulator suspended

static uint8 *the_buffer;                                                       // Mac frame buffer
static bool redraw_thread_active = false;                       // Flag: Redraw thread installed
static volatile bool redraw_thread_cancel = false;      // Flag: Cancel Redraw thread
static bool classic_mode = false;					// Flag: Classic Mac video mode
//static pthread_t redraw_thread;                                         // Redraw thread
//prototypes

static bool init_window(int width, int height);
void set_video_monitor(int width, int height, int bytes_per_row, int mac_mode);
static bool is_modifier_key(SDL_KeyboardEvent const & e);
static int event2keycode(SDL_KeyboardEvent const &ev, bool key_down);
void doevents(void);
static int kc_decode(SDL_keysym const & ks, bool key_down);
static bool is_ctrl_down(SDL_keysym const & ks);





////////////////////////////////////////


/*
 * Sets the 256-color palette on the active SDL video screen surface.
 *
 * Arguments:
 *   pal: Pointer to 256*3 RGB bytes (values 0..255).
 */
void video_set_palette(uint8 *pal)
{
	/* Recorded before the SDLscreen check: mirrored guest windows are drawn
	   from this table too, and they exist whether or not the main screen does. */
	if (pal) {
		memcpy(s_saved_palette, pal, sizeof(s_saved_palette));
		s_have_palette = true;
	}
	ToolboxWindow_SetPalette(pal);

	if (!SDLscreen)
		return;

	// Indexed Mac modes are drawn to the 32-bit surface through this table
	if (SDLscreen->format->BytesPerPixel == 4) {
		for (int i = 0; i < 256; i++)
			s_palette_pixels[i] = SDL_MapRGB(SDLscreen->format, pal[i * 3 + 0],
			                                 pal[i * 3 + 1], pal[i * 3 + 2]);
		return;
	}

	// Classic mode still draws to an 8-bit palettized surface
	SDL_Color colors[256];
	for (int i = 0; i < 256; i++) {
		colors[i].r = pal[i * 3 + 0];
		colors[i].g = pal[i * 3 + 1];
		colors[i].b = pal[i * 3 + 2];
		colors[i].unused = 0;
	}
	SDL_SetColors(SDLscreen, colors, 0, 256);
}

/*
 * Reads the largest window content size the host can show.
 *
 * On Windows that is the desktop work area (the screen without the taskbar)
 * less the frame, title bar and menu bar of a normal window, so the largest
 * Video preset still fits once the menu is attached. Elsewhere it is the
 * desktop size SDL 1.2 reports.
 *
 * Arguments:
 *   width, height: Out-parameters; set to VIDEO_MAX_* if the query fails.
 */
static void query_host_desktop(int *width, int *height)
{
	*width = VIDEO_MAX_WIDTH;
	*height = VIDEO_MAX_HEIGHT;
#if defined(WIN32) || defined(_WIN32)
	RECT work;
	if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
		// A zero-size client rect grows by exactly the non-client parts
		RECT frame = { 0, 0, 0, 0 };
		AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, TRUE /* bMenu */, 0);
		int w = (work.right - work.left) - (frame.right - frame.left);
		int h = (work.bottom - work.top) - (frame.bottom - frame.top);
		if (w > 0 && h > 0) {
			*width = w;
			*height = h;
			return;
		}
	}
#endif
	const SDL_VideoInfo *info = SDL_GetVideoInfo();
	if (info && info->current_w > 0 && info->current_h > 0) {
		*width = info->current_w;
		*height = info->current_h;
	}
}

/*
 * Attaches the host menu bar to the SDL window after an SDL_SetVideoMode().
 *
 * On Windows the menu is part of SDL's own window, which a mode change may
 * replace, so this runs after every mode set; MenuBar_Init() re-attaches the
 * bar and the WM_COMMAND hook to a new window and keeps the client area at the
 * mode size. The other hosts have an application-level menu bar that only
 * needs setting up once.
 *
 * Arguments:
 *   first: True for the first mode set, from VideoInit().
 */
static void attach_host_menu(bool first)
{
#if defined(WIN32) || defined(_WIN32)
	(void)first;
	SDL_SysWMinfo wminfo;
	SDL_VERSION(&wminfo.version);
	if (SDL_GetWMInfo(&wminfo) && wminfo.window)
		MenuBar_Init((void *)wminfo.window);
	else
		printf("VID: SDL_GetWMInfo failed; Win32 menu bar not attached\n");
#else
	if (first)
		MenuBar_Init(NULL);
#endif
}

bool VideoInit(bool classic)
{
	const char *mode_str;
	if (classic)
		mode_str = "win/512/342";
	else
		mode_str = PrefsFindString("screen");

	classic_mode = classic;
	D(bug(" VideoInit %d\n",classic));
	/* The host surface is 32-bit for every colour Mac mode: sdl12-compat keeps
	   a reused window's surface format, so a surface of the Mac's depth would
	   not follow the guest's depth changes. Classic stays on its 1-bit path. */
	if (classic)
		depth = 1;
	else
		depth = 32;

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		printf("There was an issue with SDL trying to initalize video.\n");
		exit(0);
	}

	int host_w = VIDEO_MAX_WIDTH, host_h = VIDEO_MAX_HEIGHT;
	query_host_desktop(&host_w, &host_h);
	Video_BuildPresets(host_w, host_h);

	int width = VIDEO_DEFAULT_WIDTH, height = VIDEO_DEFAULT_HEIGHT;

	if (mode_str) {
		int pref_w = 0, pref_h = 0;
		if (sscanf(mode_str, "win/%d/%d", &pref_w, &pref_h) == 2) {
			width = pref_w;
			height = pref_h;
		}
	}

	if (width > Video_MaxWidth())
		width = Video_MaxWidth();
	if (height > Video_MaxHeight())
		height = Video_MaxHeight();
	if (width < VIDEO_MIN_WIDTH)
		width = VIDEO_MIN_WIDTH;
	if (height < VIDEO_MIN_HEIGHT)
		height = VIDEO_MIN_HEIGHT;

	if (!init_window(width, height))
		return false;

	// Framebuffer is Host_Mem_Base + MacFrameBaseMac (set in init_window)
	MacFrameBaseHost = the_buffer;
	if (classic) {
		MacFrameLayout = FLAYOUT_NONE;
		MacFrameSize = VideoMonitor.bytes_per_row * VideoMonitor.y;
	} else {
		/*
		 * Reserve VRAM for the largest advertised mode at 32 bpp once.
		 * Video_SwitchToModeDepth() only changes the logical VideoMonitor size
		 * and depth inside this range, so the pixel arena in toolbox_window.cpp
		 * (MacFrameBaseMac + MacFrameSize) stays put.
		 */
		MacFrameSize = Video_ReservedFrameBytes();
	}

	// Commit the reserved screen bytes; Classic keeps 0xA0000000 as a hole
	InitFrameBufferMapping();
	Video_NoteCurrentMode(width, height);

	// Initialize default gray palette for the guest's initial 8-bit mode
	if (!classic) {
		uint8 init_pal[256 * 3];
		for (int i = 0; i < 256; i++) {
			init_pal[i * 3 + 0] = 127;
			init_pal[i * 3 + 1] = 127;
			init_pal[i * 3 + 2] = 127;
		}
		video_set_palette(init_pal);
	}

	return true;
}

// Init window mode
static bool init_window(int width, int height)
{
	int flags;
D(bug(" init_window w%d,h%d d%d\n",width,height,depth));
        // Set absolute mouse mode
        ADBSetRelMouseMode(false);

        // Read frame skip prefs: redraw every frame_skip'th 60Hz tick
        frame_skip = PrefsFindInt32("frameskip");
        if (frame_skip < 1)
                frame_skip = 1;
        hide_cursor = PrefsFindBool("hide_cursor");
//SDL
        flags=kVideoSDLFlags;
        if (!(SDLscreen = SDL_SetVideoMode(width, height, depth, flags)))
        printf("VID: Couldn't set video mode: %s\n", SDL_GetError());
        // No focus event arrives if the pointer already sits over the window
        SDL_ShowCursor(hide_cursor ? SDL_DISABLE : SDL_ENABLE);
        SDL_WM_SetCaption(VERSION_STRING,VERSION_STRING);
	attach_host_menu(true);
//SDL

                // The guest starts in 8-bit colour (1-bit on Classic)
                int bytes_per_row = width;
                switch (classic_mode ? 1 : 8) {
                        case 1:
                                bytes_per_row *= 1;
				bytes_per_pixel=1;
                                break;
			case 8:
				bytes_per_row *=1;
				bytes_per_pixel=1;
				break;
                        case 15:
                        case 16:
                                bytes_per_row *= 2;
				bytes_per_pixel=2;
                                break;
                        case 24:
                        case 32:
                                bytes_per_row *= 4;
				bytes_per_pixel=4;
                                break;
                }
		D(bug(" bytes per row %d\n",bytes_per_row));
	// Guest framebuffer is the NuBus slot at MacFrameBaseMac inside the 4GB window
	if (!Host_Mem_Base) {
		printf("VID: unified 4GB Host_Mem_Base window is not allocated\n");
		return false;
	}
	the_buffer = Host_Mem_Base + MacFrameBaseMac;
        //the_buffer_copy = (uint8 *)malloc((height + 2) * img->bytes_per_line);

set_video_monitor(width, height, bytes_per_row, (depth == 1) ? VMODE_1BIT : VMODE_8BIT);

        VideoMonitor.mac_frame_base = MacFrameBaseMac;
  return true;
}


/*
 * Host SDL depth used to present a Mac VMODE_* framebuffer: 32-bit for every
 * mode, with the indexed and 16-bit modes converted in the blit. A fixed
 * depth means a Mac depth change never needs a new SDL surface, which
 * sdl12-compat would not provide when it reuses the window.
 *
 * Arguments:
 *   mac_mode: VMODE_* depth of the guest framebuffer (unused).
 *
 * Returns:
 *   The SDL surface depth in bits.
 */
static int host_depth_for_mode(int mac_mode)
{
	(void)mac_mode;
	return 32;
}

/*
 * Packed host bytes per pixel for the SDL surface that presents mac_mode.
 *
 * Arguments:
 *   mac_mode: VMODE_* depth of the guest framebuffer (unused).
 *
 * Returns:
 *   4, matching host_depth_for_mode().
 */
static int host_bytes_per_pixel(int mac_mode)
{
	(void)mac_mode;
	return 4;
}

/*
 * Writes VideoMonitor for a logical Mac size and VMODE_* depth.
 *
 * Arguments:
 *   width, height: Guest pixel size.
 *   bytes_per_row: Packed Mac rowBytes at that depth.
 *   mac_mode: VMODE_* depth of the guest framebuffer.
 */
void set_video_monitor(int width, int height, int bytes_per_row, int mac_mode)
{
	D(bug("set_video_monitor %d %d %d mode %d\n", width, height, bytes_per_row, mac_mode));
	if (mac_mode < VMODE_1BIT || mac_mode > VMODE_32BIT)
		mac_mode = VMODE_8BIT;
	VideoMonitor.mode = mac_mode;
	VideoMonitor.x = width;
	VideoMonitor.y = height;
	VideoMonitor.bytes_per_row = bytes_per_row;
	bytes_per_pixel = host_bytes_per_pixel(mac_mode);
	MacFrameLayout = FLAYOUT_DIRECT;
	printf("SDL_Video %dx%d mac-mode %d host %d-bit\n", width, height, mac_mode, depth);
}

/*
 * Clamps a requested mode to the reserved framebuffer and the live-switch floor.
 *
 * Arguments:
 *   width, height: In/out pixel size.
 */
static void clamp_mode_size(int *width, int *height)
{
	if (*width < VIDEO_MIN_WIDTH)
		*width = VIDEO_MIN_WIDTH;
	if (*height < VIDEO_MIN_HEIGHT)
		*height = VIDEO_MIN_HEIGHT;
	if (*width > Video_MaxWidth())
		*width = Video_MaxWidth();
	if (*height > Video_MaxHeight())
		*height = Video_MaxHeight();
}

/*
 * Switches the host window and the logical guest screen to width x height
 * at VMODE_* depth `mode`.
 *
 * Recomputes VideoMonitor, re-calls SDL_SetVideoMode (SDL 1.2 supports this
 * on a live window; 1/2/4-bit Mac depths stay on an 8-bit host surface),
 * clears the newly visible guest framebuffer, and patches the slot-ROM
 * VModeParms via Video_NoteCurrentMode. Guest cscSetMode / cscSwitchMode
 * leave Toolbox_NotifyScreenResized off so InitGDevice owns the GDevice.
 * Host Video-menu / drag-resize queue a size and call cscSwitchMode from
 * the jGNEFilter safe point (then geometry + _AllocCursor) instead of
 * invoking this directly.
 * MacFrameSize is not changed.
 * SDL_SetVideoMode is skipped when the host surface size and depth are
 * unchanged (1-bit and 8-bit both use an 8-bit SDL surface).
 *
 * This can run from doevents() inside VideoInterrupt, so it must not call
 * the Window Manager itself.
 *
 * Arguments:
 *   width, height: Requested pixel size.
 *   mode: VMODE_* depth to install.
 *
 * Returns:
 *   true if the logical mode matches the request on return.
 */
bool Video_SwitchToModeDepth(int width, int height, int mode)
{
	if (classic_mode)
		return false;
	if (!SDLscreen || !the_buffer)
		return false;
	if (mode < VMODE_1BIT || mode > VMODE_32BIT)
		return false;

	clamp_mode_size(&width, &height);
	if ((uint32)width == VideoMonitor.x && (uint32)height == VideoMonitor.y &&
	    mode == VideoMonitor.mode)
		return true;

	if (s_in_mode_switch)
		return false;
	s_in_mode_switch = true;

	int old_width = (int)VideoMonitor.x;
	int old_height = (int)VideoMonitor.y;
	int old_bpr = (int)VideoMonitor.bytes_per_row;
	int old_mode = VideoMonitor.mode;
	int old_host = depth;
	uint32 bytes_per_row = Video_BytesPerRowForMode(width, mode);
	int next_host = host_depth_for_mode(mode);
	bool host_surface_unchanged = ((uint32)width == (uint32)old_width &&
	                               (uint32)height == (uint32)old_height &&
	                               next_host == old_host);

	set_video_monitor(width, height, (int)bytes_per_row, mode);
	VideoMonitor.mac_frame_base = MacFrameBaseMac;

	if (!host_surface_unchanged) {
		SDL_Surface *next = SDL_SetVideoMode(width, height, next_host, kVideoSDLFlags);
		if (!next) {
			printf("VID: SDL_SetVideoMode(%dx%d @ %d) failed: %s; keeping %dx%d mode %d\n",
			       width, height, next_host, SDL_GetError(), old_width, old_height, old_mode);
			fflush(stdout);
			depth = old_host;
			set_video_monitor(old_width, old_height, old_bpr, old_mode);
			VideoMonitor.mac_frame_base = MacFrameBaseMac;
			s_in_mode_switch = false;
			return false;
		}
		SDLscreen = next;
		depth = next_host;
		SDL_ShowCursor(hide_cursor ? SDL_DISABLE : SDL_ENABLE);
		if (s_have_palette)
			video_set_palette(s_saved_palette);
		attach_host_menu(false);
	}

	// Newly revealed (or re-packed) pixels would otherwise show leftover VRAM
	Mac_memset(MacFrameBaseMac, 0, bytes_per_row * (uint32)height);

	Video_NoteCurrentMode(width, height);
	// Guest SetMode/SwitchMode leave this off so InitGDevice owns the GDevice
	if (Video_GuestNotifyEnabled())
		Toolbox_NotifyScreenResized((int16)width, (int16)height);

	if (SDLscreen && SDLscreen->format && (mode == VMODE_16BIT || mode == VMODE_32BIT)) {
		printf("VID: SDL format %d-bpp R%08x G%08x B%08x\n",
		       (int)SDLscreen->format->BitsPerPixel,
		       (unsigned)SDLscreen->format->Rmask,
		       (unsigned)SDLscreen->format->Gmask,
		       (unsigned)SDLscreen->format->Bmask);
	}
	printf("VID: switched to %dx%d mode %d (rowBytes %u, host %d-bit)\n",
	       width, height, mode, bytes_per_row, depth);
	fflush(stdout);

	s_in_mode_switch = false;
	return true;
}


void VideoExit(void)
{}

/*
 * SDL_MapRGB adapter for VideoBlit_BuildRGB555Table.
 *
 * Arguments:
 *   ctx: The host SDL_PixelFormat.
 *   r, g, b: 8-bit channels.
 *
 * Returns:
 *   The host pixel value.
 */
static uint32 map_rgb_sdl(void *ctx, uint8 r, uint8 g, uint8 b)
{
	return SDL_MapRGB((SDL_PixelFormat *)ctx, r, g, b);
}

/*
 * Returns the 15-bit Mac colour to host pixel table for fmt, rebuilding it
 * when the host surface layout differs from the one it was built for (a
 * mode switch may hand back a surface with a different format).
 *
 * Arguments:
 *   fmt: Current host surface format.
 *
 * Returns:
 *   32768 host pixel values, indexed by the low 15 bits of a Mac pixel.
 */
static const uint32 *rgb555_table_for(SDL_PixelFormat *fmt)
{
	static uint32 table[32768];
	static uint32 key_r = 0, key_g = 0, key_b = 0, key_a = 0;
	static int key_bpp = 0;

	// Masks and depth fully determine what SDL_MapRGB returns for a direct surface
	if (fmt->BytesPerPixel != key_bpp || fmt->Rmask != key_r || fmt->Gmask != key_g ||
	    fmt->Bmask != key_b || fmt->Amask != key_a) {
		VideoBlit_BuildRGB555Table(table, map_rgb_sdl, fmt);
		key_bpp = fmt->BytesPerPixel;
		key_r = fmt->Rmask;
		key_g = fmt->Gmask;
		key_b = fmt->Bmask;
		key_a = fmt->Amask;
	}
	return table;
}

/*
 * Tells whether a Mac 32-bit pixel can be copied to the host surface with a
 * byte swap alone (host XRGB8888, no alpha, little-endian host).
 *
 * Arguments:
 *   fmt: Current host surface format.
 *
 * Returns:
 *   true if VideoBlit_XRGB8888BE produces what SDL_MapRGB would.
 */
static bool is_host_xrgb8888(const SDL_PixelFormat *fmt)
{
#ifdef WORDS_BIGENDIAN
	return false;
#else
	return fmt->BytesPerPixel == 4 && fmt->Rmask == 0x00ff0000 && fmt->Gmask == 0x0000ff00 &&
	       fmt->Bmask == 0x000000ff && fmt->Amask == 0;
#endif
}

void VideoInterrupt(void)
{
int lx,ly=0;
#if defined(HEARTBEAT_DEBUG) && HEARTBEAT_DEBUG
static int s_heartbeat_ticks = 0;
if (++s_heartbeat_ticks % 60 == 0) {
	printf("[HEARTBEAT] 680x0 CPU active at PC=0x%08X\n", cpu_engine_last_pc);
	fflush(stdout);
}
#endif
uint8 *src_buf = MacFrameBaseHost ? MacFrameBaseHost : the_buffer;
if(++skip_count>=frame_skip){
	if(classic_mode)
		Mac2Host_memcpy(src_buf, 0x3fa700, VideoMonitor.bytes_per_row * VideoMonitor.y);
	else
	switch (VideoMonitor.mode) {
		case VMODE_1BIT:
		case VMODE_2BIT:
		case VMODE_4BIT:
		case VMODE_8BIT: {
			static const int kBits[] = { 1, 2, 4, 8 };
			int bits = kBits[VideoMonitor.mode - VMODE_1BIT];
			// Colour modes draw to the 32-bit surface through the palette
			if (SDLscreen->format->BytesPerPixel == 4) {
				VideoBlit_IndexedToPixels32(src_buf, VideoMonitor.bytes_per_row,
				                            (uint8 *)SDLscreen->pixels, SDLscreen->pitch,
				                            VideoMonitor.x, VideoMonitor.y, bits, s_palette_pixels);
				break;
			}
			// An 8-bit palettized surface takes the indices themselves
			if (bits < 8) {
				VideoBlit_ExpandIndexed(src_buf, VideoMonitor.bytes_per_row, (uint8 *)SDLscreen->pixels,
				                        SDLscreen->pitch, VideoMonitor.x, VideoMonitor.y, bits);
				break;
			}
			if (SDLscreen->pitch == (int)VideoMonitor.bytes_per_row)
				memcpy(SDLscreen->pixels, src_buf, VideoMonitor.bytes_per_row * VideoMonitor.y);
			else {
				for (ly = 0; ly < (int)VideoMonitor.y; ly++)
					memcpy((uint8 *)SDLscreen->pixels + ly * SDLscreen->pitch,
					       src_buf + ly * VideoMonitor.bytes_per_row,
					       VideoMonitor.bytes_per_row);
			}
			break;
		}
		case VMODE_16BIT:
			// Mac 16-bit is big-endian 1-5-5-5; SDL is host 5-6-5 or 5-5-5
			VideoBlit_RGB555BE(src_buf, VideoMonitor.bytes_per_row, (uint8 *)SDLscreen->pixels,
			                   SDLscreen->pitch, VideoMonitor.x, VideoMonitor.y,
			                   rgb555_table_for(SDLscreen->format),
			                   SDLscreen->format->BytesPerPixel);
			break;
		case VMODE_32BIT:
			// Mac 32-bit is 00 RR GG BB; memcpy onto LE SDL looks yellow (B=0)
			if (is_host_xrgb8888(SDLscreen->format)) {
				VideoBlit_XRGB8888BE(src_buf, VideoMonitor.bytes_per_row, (uint8 *)SDLscreen->pixels,
				                     SDLscreen->pitch, VideoMonitor.x, VideoMonitor.y);
				break;
			}
			// Any other host layout: map each pixel through SDL
			for (ly = 0; ly < (int)VideoMonitor.y; ly++) {
				const uint8 *src = src_buf + ly * VideoMonitor.bytes_per_row;
				uint8 *dst = (uint8 *)SDLscreen->pixels + ly * SDLscreen->pitch;
				for (lx = 0; lx < (int)VideoMonitor.x; lx++) {
					uint32 pix = SDL_MapRGB(SDLscreen->format, src[1], src[2], src[3]);
					if (SDLscreen->format->BytesPerPixel == 4)
						*(uint32 *)dst = pix;
					else if (SDLscreen->format->BytesPerPixel == 2)
						*(uint16 *)dst = (uint16)pix;
					src += 4;
					dst += SDLscreen->format->BytesPerPixel;
				}
			}
			break;
		default:
			break;
		}
	SDL_UpdateRect(SDLscreen,0,0,0,0);
	skip_count=0;
		}

	/* Mirrored guest windows draw into their own buffers rather than into the
	   screen, so they are presented separately from the blit above. */
	Toolbox_PresentWindows();
//if(count>0)
//	printf("drew %d/%d pels\n",count,VideoMonitor.bytes_per_row*VideoMonitor.y);
doevents();
}

void doevents(void)
{
 SDL_Event event;
	int mb,x,y;
int emul_suspended=0;
	/* A mode set can post several resizes at once: Cocoa repeats the new size,
	   and on Windows attaching the menu bar shrinks the client area before
	   the window is grown back. Acting on each would queue a guest mode switch
	   to a size that no longer applies, so only the final one is used. */
	bool resize_pending = false;
	int resize_w = 0, resize_h = 0;
    while(SDL_PollEvent(&event))
    {
        switch (event.type) {
	case SDL_KEYDOWN: {
			int code = -1;
			if (use_keycodes && !is_modifier_key(event.key)) {
				if (event2keycode(event.key, true) != -2)	// This is called to process the hotkeys
					code = keycode_table[event.key.keysym.scancode & 0xff];
			} else
				code = event2keycode(event.key, true);
			if (code >= 0) {
				if (!emul_suspended) {
					if (code == 0x39) {	// Caps Lock pressed
						if (caps_on) {
							ADBKeyUp(code);
							caps_on = false;
						} else {
							ADBKeyDown(code);
							caps_on = true;
						}
					} else
						ADBKeyDown(code);
					if (code == 0x36)
						ctrl_down = true;
				} else {
				//	if (code == 0x31)
				//		drv->resume();	// Space wakes us up
				}
			}
			break;
		}
	case SDL_KEYUP: {
				int code = -1;
				if (use_keycodes && !is_modifier_key(event.key)) {
					if (event2keycode(event.key, false) != -2)	// This is called to process the hotkeys
						code = keycode_table[event.key.keysym.scancode & 0xff];
				} else
					code = event2keycode(event.key, false);
				if (code >= 0) {
					if (code == 0x39) {	// Caps Lock released
						if (caps_on) {
							ADBKeyUp(code);
							caps_on = false;
						} else {
							ADBKeyDown(code);
							caps_on = true;
						}
					} else
						ADBKeyUp(code);
					if (code == 0x36)
						ctrl_down = false;
				}
				break;
			}
	break;
	// Mouse button
	case SDL_MOUSEBUTTONDOWN: {
			unsigned int button = event.button.button;
			if (button < 4)
				ADBMouseDown(button - 1);
			else if (button < 6) {	// Wheel mouse
				if (mouse_wheel_mode == 0) {
					int key = (button == 5) ? 0x79 : 0x74;	// Page up/down
					ADBKeyDown(key);
					ADBKeyUp(key);
				} else {
					int key = (button == 5) ? 0x3d : 0x3e;	// Cursor up/down
					for(int i=0; i<mouse_wheel_lines; i++) {
						ADBKeyDown(key);
						ADBKeyUp(key);
					}
				}
			}
			break;
		}
		case SDL_MOUSEBUTTONUP: {
			unsigned int button = event.button.button;
			if (button < 4)
				ADBMouseUp(button - 1);
			break;
		}
	case SDL_MOUSEMOTION:
	ADBMouseMoved(event.motion.x, event.motion.y);
	break;

	case SDL_VIDEORESIZE:
		// Only the last resize of a batch counts (see below)
		resize_pending = true;
		resize_w = event.resize.w;
		resize_h = event.resize.h;
		break;

	case SDL_ACTIVEEVENT:
		if (hide_cursor && (event.active.state & SDL_APPMOUSEFOCUS))
			SDL_ShowCursor(event.active.gain ? SDL_DISABLE : SDL_ENABLE);
		break;

	case SDL_QUIT:
		quitcount++;
		ADBKeyDown(0x7f);	// Power key
		ADBKeyUp(0x7f);
		if(quitcount>2)
			QuitEmulator();	//this should be the nice shutdown
	break;

	default:
	break;
	}//end switch
   }//end while

	// Ask the guest for the window's final size unless it already has it
	if (resize_pending) {
		clamp_mode_size(&resize_w, &resize_h);
		if ((uint32)resize_w != VideoMonitor.x || (uint32)resize_h != VideoMonitor.y)
			Toolbox_NotifyScreenResized((int16)resize_w, (int16)resize_h);
	}
}


static bool is_modifier_key(SDL_KeyboardEvent const & e)
{
	switch (e.keysym.sym) {
	case SDLK_NUMLOCK:
	case SDLK_CAPSLOCK:
	case SDLK_SCROLLOCK:
	case SDLK_RSHIFT:
	case SDLK_LSHIFT:
	case SDLK_RCTRL:
	case SDLK_LCTRL:
	case SDLK_RALT:
	case SDLK_LALT:
	case SDLK_RMETA:
	case SDLK_LMETA:
	case SDLK_LSUPER:
	case SDLK_RSUPER:
	case SDLK_MODE:
	case SDLK_COMPOSE:
		return true;
	}
	return false;
}


static int event2keycode(SDL_KeyboardEvent const &ev, bool key_down)
{
	return kc_decode(ev.keysym, key_down);
}

/*
 *  Translate key event to Mac keycode, returns -1 if no keycode was found
 *  and -2 if the key was recognized as a hotkey
 */

static int kc_decode(SDL_keysym const & ks, bool key_down)
{
	switch (ks.sym) {
	case SDLK_a: return 0x00;
	case SDLK_b: return 0x0b;
	case SDLK_c: return 0x08;
	case SDLK_d: return 0x02;
	case SDLK_e: return 0x0e;
	case SDLK_f: return 0x03;
	case SDLK_g: return 0x05;
	case SDLK_h: return 0x04;
	case SDLK_i: return 0x22;
	case SDLK_j: return 0x26;
	case SDLK_k: return 0x28;
	case SDLK_l: return 0x25;
	case SDLK_m: return 0x2e;
	case SDLK_n: return 0x2d;
	case SDLK_o: return 0x1f;
	case SDLK_p: return 0x23;
	case SDLK_q: return 0x0c;
	case SDLK_r: return 0x0f;
	case SDLK_s: return 0x01;
	case SDLK_t: return 0x11;
	case SDLK_u: return 0x20;
	case SDLK_v: return 0x09;
	case SDLK_w: return 0x0d;
	case SDLK_x: return 0x07;
	case SDLK_y: return 0x10;
	case SDLK_z: return 0x06;

//	case SDLK_1: case SDLK_EXCLAIM: return 0x12;
//	case SDLK_2: case SDLK_AT: return 0x13;
//	case SDLK_3: case SDLK_numbersign: return 0x14;
//	case SDLK_3: case '#': return 0x14;
//	case SDLK_4: case SDLK_DOLLAR: return 0x15;
//	case SDLK_4: case '$': return 0x15;
//	case SDLK_5: case SDLK_percent: return 0x17;
	case SDLK_1: return 0x12;
	case SDLK_2: return 0x13;
	case SDLK_3: return 0x14;
	case SDLK_4: return 0x15;
	case SDLK_5: return 0x17;
	case SDLK_6: return 0x16;
	case SDLK_7: return 0x1a;
	case SDLK_8: return 0x1c;
	case SDLK_9: return 0x19;
	case SDLK_0: return 0x1d;

//	case SDLK_BACKQUOTE: case SDLK_asciitilde: return 0x0a;
	case SDLK_BACKQUOTE: return 0x32;
	case SDLK_BACKSLASH: return 0x2a;
	case SDLK_MINUS: case SDLK_UNDERSCORE: return 0x1b;
	case SDLK_EQUALS: case SDLK_PLUS: return 0x18;
	case SDLK_LEFTBRACKET: return 0x21;
	case SDLK_RIGHTBRACKET: return 0x1e;
	case SDLK_SEMICOLON: case SDLK_COLON: return 0x29;
	case SDLK_QUOTE: case SDLK_QUOTEDBL: return 0x27;
//	case SDLK_apostrophe: case SDLK_QUOTEDBL: return 0x27;
	case SDLK_COMMA: case SDLK_LESS: return 0x2b;
	case SDLK_PERIOD: case SDLK_GREATER: return 0x2f;
	case SDLK_SLASH: case SDLK_QUESTION: return 0x2c;

	case SDLK_TAB: /*if (is_ctrl_down(ks)) {
					if (!key_down) 
						drv->suspend(); 	
					return -2;} 
					else */return 0x30;
	case SDLK_RETURN: return 0x24;
	case SDLK_SPACE: return 0x31;
	case SDLK_BACKSPACE: return 0x33;

	case SDLK_DELETE: return 0x75;
	case SDLK_INSERT: return 0x72;
	case SDLK_HOME: case SDLK_HELP: return 0x73;
	case SDLK_END: return 0x77;
	case SDLK_PAGEUP: return 0x74;
	case SDLK_PAGEDOWN: return 0x79;

	case SDLK_LCTRL: return 0x36;
	case SDLK_RCTRL: return 0x36;
	case SDLK_LSHIFT: return 0x38;
	case SDLK_RSHIFT: return 0x38;
#if (defined(__APPLE__) && defined(__MACH__))
	case SDLK_LALT: return 0x3a;
	case SDLK_RALT: return 0x3a;
	case SDLK_LMETA: return 0x37;
	case SDLK_RMETA: return 0x37;
#else
	case SDLK_LALT: return 0x37;
	case SDLK_RALT: return 0x37;
	case SDLK_LMETA: return 0x3a;
	case SDLK_RMETA: return 0x3a;
#endif
	case SDLK_MENU: return 0x32;
	case SDLK_CAPSLOCK: return 0x39;
	case SDLK_NUMLOCK: return 0x47;

	case SDLK_UP: return 0x3e;
	case SDLK_DOWN: return 0x3d;
	case SDLK_LEFT: return 0x3b;
	case SDLK_RIGHT: return 0x3c;

	case SDLK_ESCAPE: if (is_ctrl_down(ks)) {if (!key_down) { quit_full_screen = true; emerg_quit = true; } return -2;} else return 0x35;

	//case SDLK_F1: if (is_ctrl_down(ks)) {if (!key_down) SysMountFirstFloppy(); return -2;} else return 0x7a;
	case SDLK_F1: return 0x7a;
	case SDLK_F2: return 0x78;
	case SDLK_F3: return 0x63;
	case SDLK_F4: return 0x76;
	//case SDLK_F5: if (is_ctrl_down(ks)) {if (!key_down) drv->toggle_mouse_grab(); return -2;} else return 0x60;
	case SDLK_F5: return 0x60;
	case SDLK_F6: return 0x61;
	case SDLK_F7: return 0x62;
	case SDLK_F8: return 0x64;
	case SDLK_F9: return 0x65;
	case SDLK_F10: return 0x6d;
	case SDLK_F11: return 0x67;
	case SDLK_F12: return 0x6f;

	case SDLK_PRINT: return 0x69;
	case SDLK_SCROLLOCK: return 0x6b;
	case SDLK_PAUSE: return 0x71;

	case SDLK_KP0: return 0x52;
	case SDLK_KP1: return 0x53;
	case SDLK_KP2: return 0x54;
	case SDLK_KP3: return 0x55;
	case SDLK_KP4: return 0x56;
	case SDLK_KP5: return 0x57;
	case SDLK_KP6: return 0x58;
	case SDLK_KP7: return 0x59;
	case SDLK_KP8: return 0x5b;
	case SDLK_KP9: return 0x5c;
	case SDLK_KP_PERIOD: return 0x41;
	case SDLK_KP_PLUS: return 0x45;
	case SDLK_KP_MINUS: return 0x4e;
	case SDLK_KP_MULTIPLY: return 0x43;
	case SDLK_KP_DIVIDE: return 0x4b;
	case SDLK_KP_ENTER: return 0x4c;
	case SDLK_KP_EQUALS: return 0x51;
	}
	D(bug("Unhandled SDL keysym: %d\n", ks.sym));
	return -1;
}

static bool is_ctrl_down(SDL_keysym const & ks)
{
	return ctrl_down || (ks.mod & KMOD_CTRL);
}

