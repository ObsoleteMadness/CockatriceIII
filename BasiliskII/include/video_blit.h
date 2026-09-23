/*
 *  video_blit.h - Mac framebuffer to host surface pixel conversion
 *
 *  The SDL video driver redraws the host window from the 60Hz interrupt, on
 *  the emulation thread, so every millisecond spent converting pixels is a
 *  millisecond the 68k does not run. These converters replace the per-pixel
 *  shift/mask and SDL_MapRGB loops with byte-expansion tables, a 15-bit
 *  colour table and a straight byte swap. They take plain pointers and
 *  pitches and know nothing about SDL, so they can be tested on their own.
 */

#ifndef VIDEO_BLIT_H
#define VIDEO_BLIT_H

/*
 * Expands 1, 2 or 4-bit packed Mac pixels (most significant pixel first)
 * into one palette index per byte, as an 8-bit host surface wants them.
 *
 * Arguments:
 *   src: First Mac framebuffer row.
 *   src_bpr: Bytes per Mac row.
 *   dst: First host surface row.
 *   dst_pitch: Bytes per host row.
 *   width, height: Pixels to convert; width need not be a multiple of the
 *     pixels per byte.
 *   bits: Mac pixel depth, 1, 2 or 4.
 */
void VideoBlit_ExpandIndexed(const uint8 *src, int src_bpr, uint8 *dst, int dst_pitch,
                             int width, int height, int bits);

/*
 * Converts 1, 2, 4 or 8-bit indexed Mac pixels (most significant pixel first)
 * to 32-bit host pixels through the current palette.
 *
 * The host surface is 32-bit for every Mac depth, so a depth change never
 * needs a new SDL surface (sdl12-compat keeps the old surface's format when
 * it reuses the window).
 *
 * Arguments:
 *   src, src_bpr: Mac framebuffer and its bytes per row.
 *   dst, dst_pitch: 32-bit host surface and its bytes per row.
 *   width, height: Pixels to convert; width need not be a multiple of the
 *     pixels per byte.
 *   bits: Mac pixel depth, 1, 2, 4 or 8.
 *   palette: Host pixel for each of the 256 palette entries (only the first
 *     2^bits are used).
 */
void VideoBlit_IndexedToPixels32(const uint8 *src, int src_bpr, uint8 *dst, int dst_pitch,
                                 int width, int height, int bits, const uint32 *palette);

/*
 * Converts big-endian x-1-5-5-5 Mac pixels through a 32768-entry table of
 * host pixel values (see VideoBlit_BuildRGB555Table).
 *
 * Arguments:
 *   src, src_bpr: Mac framebuffer and its bytes per row.
 *   dst, dst_pitch: Host surface and its bytes per row.
 *   width, height: Pixels to convert.
 *   table: Host pixel for each 15-bit Mac colour.
 *   dst_bytes_per_pixel: 2 or 4.
 */
void VideoBlit_RGB555BE(const uint8 *src, int src_bpr, uint8 *dst, int dst_pitch,
                        int width, int height, const uint32 *table, int dst_bytes_per_pixel);

/*
 * Fills the table used by VideoBlit_RGB555BE. Each 5-bit channel is scaled
 * to 8 bits (c * 255 / 31) and passed to map, so the table holds exactly
 * what calling map per pixel would have produced.
 *
 * Arguments:
 *   table: 32768 entries, indexed by the low 15 bits of the Mac pixel.
 *   map: Returns the host pixel for an 8-bit r, g, b (SDL_MapRGB in the
 *     driver).
 *   ctx: Passed through to map.
 */
void VideoBlit_BuildRGB555Table(uint32 *table, uint32 (*map)(void *ctx, uint8 r, uint8 g, uint8 b),
                                void *ctx);

/*
 * Converts Mac 32-bit pixels (bytes x, R, G, B) to host XRGB8888 by
 * swapping each word's bytes. Only valid when the host surface is 32-bit
 * with R at 0x00ff0000, G at 0x0000ff00, B at 0x000000ff and no alpha, on
 * a little-endian host; the caller checks that.
 *
 * Arguments:
 *   src, src_bpr: Mac framebuffer and its bytes per row.
 *   dst, dst_pitch: Host surface and its bytes per row.
 *   width, height: Pixels to convert.
 */
void VideoBlit_XRGB8888BE(const uint8 *src, int src_bpr, uint8 *dst, int dst_pitch,
                          int width, int height);

#endif
