/*
 *  video_blit.cpp - Mac framebuffer to host surface pixel conversion
 *
 *  See video_blit.h. Timed at 1710x1112 on Apple Silicon, the 32-bit
 *  SDL_MapRGB loop this replaces took about 15 ms per redraw, nearly a whole
 *  60Hz tick of emulation-thread time; the byte swap takes well under 1 ms.
 */

#include "sysdeps.h"

#include <string.h>

#include "video_blit.h"

/*
 * Byte-expansion tables: entry [b] holds the palette indices of the pixels
 * packed in Mac byte b, leftmost first. Built once on first use.
 */
static uint8 s_expand1[256][8];
static uint8 s_expand2[256][4];
static uint8 s_expand4[256][2];
static bool s_expand_ready = false;

/*
 * Fills the three byte-expansion tables.
 */
static void build_expand_tables(void)
{
	for (int b = 0; b < 256; b++) {
		// Mac packs pixels most significant bits first
		for (int i = 0; i < 8; i++)
			s_expand1[b][i] = (uint8)((b >> (7 - i)) & 1);
		for (int i = 0; i < 4; i++)
			s_expand2[b][i] = (uint8)((b >> (6 - i * 2)) & 3);
		for (int i = 0; i < 2; i++)
			s_expand4[b][i] = (uint8)((b >> (4 - i * 4)) & 0x0f);
	}
	s_expand_ready = true;
}

void VideoBlit_ExpandIndexed(const uint8 *src, int src_bpr, uint8 *dst, int dst_pitch,
                             int width, int height, int bits)
{
	if (!s_expand_ready)
		build_expand_tables();

	// Pick the table for this depth; its row length is the pixels per byte
	const uint8 *table;
	int per_byte;
	switch (bits) {
	case 1: table = &s_expand1[0][0]; per_byte = 8; break;
	case 2: table = &s_expand2[0][0]; per_byte = 4; break;
	case 4: table = &s_expand4[0][0]; per_byte = 2; break;
	default: return;
	}

	int whole = width / per_byte;	// Source bytes whose pixels are all visible
	int tail = width % per_byte;	// Visible pixels in the last, partial byte

	for (int y = 0; y < height; y++) {
		const uint8 *s = src + y * src_bpr;
		uint8 *d = dst + y * dst_pitch;
		// Each source byte becomes per_byte palette indices in one copy
		for (int x = 0; x < whole; x++) {
			memcpy(d, table + s[x] * per_byte, per_byte);
			d += per_byte;
		}
		// Copy only the visible part of a trailing partial byte
		if (tail)
			memcpy(d, table + s[whole] * per_byte, tail);
	}
}

void VideoBlit_IndexedToPixels32(const uint8 *src, int src_bpr, uint8 *dst, int dst_pitch,
                                 int width, int height, int bits, const uint32 *palette)
{
	for (int y = 0; y < height; y++) {
		const uint8 *s = src + y * src_bpr;
		uint32 *d = (uint32 *)(dst + y * dst_pitch);
		if (bits == 8) {
			// One palette lookup per byte
			for (int x = 0; x < width; x++)
				d[x] = palette[s[x]];
			continue;
		}
		// Pixel x sits in byte x / per_byte, most significant bits first
		const int per_byte = 8 / bits;
		const int mask = (1 << bits) - 1;
		for (int x = 0; x < width; x++) {
			int shift = (per_byte - 1 - (x % per_byte)) * bits;
			d[x] = palette[(s[x / per_byte] >> shift) & mask];
		}
	}
}

void VideoBlit_BuildRGB555Table(uint32 *table, uint32 (*map)(void *ctx, uint8 r, uint8 g, uint8 b),
                                void *ctx)
{
	for (int p = 0; p < 32768; p++) {
		// Same 5-to-8-bit scaling the per-pixel loop used
		uint8 r = (uint8)(((p >> 10) & 0x1f) * 255 / 31);
		uint8 g = (uint8)(((p >> 5) & 0x1f) * 255 / 31);
		uint8 b = (uint8)((p & 0x1f) * 255 / 31);
		table[p] = map(ctx, r, g, b);
	}
}

void VideoBlit_RGB555BE(const uint8 *src, int src_bpr, uint8 *dst, int dst_pitch,
                        int width, int height, const uint32 *table, int dst_bytes_per_pixel)
{
	for (int y = 0; y < height; y++) {
		const uint8 *s = src + y * src_bpr;
		uint8 *d = dst + y * dst_pitch;
		if (dst_bytes_per_pixel == 4) {
			uint32 *d32 = (uint32 *)d;
			// Big-endian Mac pixel; bit 15 is unused, so mask it off the index
			for (int x = 0; x < width; x++)
				d32[x] = table[((s[x * 2] << 8) | s[x * 2 + 1]) & 0x7fff];
		} else {
			uint16 *d16 = (uint16 *)d;
			for (int x = 0; x < width; x++)
				d16[x] = (uint16)table[((s[x * 2] << 8) | s[x * 2 + 1]) & 0x7fff];
		}
	}
}

void VideoBlit_XRGB8888BE(const uint8 *src, int src_bpr, uint8 *dst, int dst_pitch,
                          int width, int height)
{
	for (int y = 0; y < height; y++) {
		const uint8 *s = src + y * src_bpr;
		uint32 *d = (uint32 *)(dst + y * dst_pitch);
		for (int x = 0; x < width; x++) {
			// memcpy keeps the load legal for any row alignment; the compiler
			// turns load + bswap into vector byte reverses
			uint32 v;
			memcpy(&v, s + x * 4, 4);
			d[x] = __builtin_bswap32(v);
		}
	}
}
