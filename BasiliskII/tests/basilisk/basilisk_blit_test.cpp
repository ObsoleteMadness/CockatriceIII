/*
 * basilisk_blit_test.cpp - Mac framebuffer to host surface conversion (video_blit.cpp)
 *
 * Each table-driven converter is checked pixel for pixel against the
 * straightforward per-pixel loop that video_sdl.cpp used before, on random
 * framebuffers, with widths that leave a partial byte or word at the end of
 * the row and with pitches wider than the visible row. Destination rows are
 * bracketed with guard bytes, so writing past the visible width is caught too.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_harness.h"
#include "test_env.h"
#include "sysdeps.h"
#include "video_blit.h"

// Value every destination byte starts as; converters must not touch bytes past the row
static const uint8 kGuard = 0xA5;

/*
 * Fills buf with deterministic pseudo-random bytes.
 *
 * Arguments:
 *   buf, len: Buffer to fill.
 *   seed: Generator state, so each case gets different data.
 */
static void fill_random(uint8 *buf, size_t len, uint32 seed)
{
	for (size_t i = 0; i < len; i++) {
		seed = seed * 1664525u + 1013904223u;
		buf[i] = (uint8)(seed >> 24);
	}
}

/*
 * Returns true if every byte of buf[from, to) still holds the guard value.
 */
static bool guard_intact(const uint8 *buf, int from, int to)
{
	for (int i = from; i < to; i++)
		if (buf[i] != kGuard)
			return false;
	return true;
}

/*
 * Checks VideoBlit_ExpandIndexed at one depth and width against the
 * shift-and-mask loop it replaced.
 *
 * Arguments:
 *   bits: 1, 2 or 4.
 *   width, height: Visible size; width may end mid-byte.
 */
static void check_indexed(int bits, int width, int height)
{
	int src_bpr = (width * bits + 7) / 8 + 4;	// Rows padded, as Mac rowBytes may be
	int pitch = width + 16;			// Host pitch wider than the visible row
	uint8 *src = (uint8 *)malloc(src_bpr * height);
	uint8 *dst = (uint8 *)malloc(pitch * height);
	fill_random(src, src_bpr * height, bits * 1000 + width);
	memset(dst, kGuard, pitch * height);

	VideoBlit_ExpandIndexed(src, src_bpr, dst, pitch, width, height, bits);

	bool ok = true, guards = true;
	int per_byte = 8 / bits, mask = (1 << bits) - 1;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			// The pre-table loop: pixel x of the row, most significant bits first
			int shift = (per_byte - 1 - (x % per_byte)) * bits;
			uint8 want = (uint8)((src[y * src_bpr + x / per_byte] >> shift) & mask);
			if (dst[y * pitch + x] != want)
				ok = false;
		}
		if (!guard_intact(dst + y * pitch, width, pitch))
			guards = false;
	}

	char msg[96];
	snprintf(msg, sizeof(msg), "%d-bit %dx%d matches the per-pixel expansion", bits, width, height);
	CHECK(ok, msg);
	snprintf(msg, sizeof(msg), "%d-bit %dx%d writes nothing past the visible width", bits, width, height);
	CHECK(guards, msg);
	free(src);
	free(dst);
}

/*
 * Checks VideoBlit_IndexedToPixels32 at one depth and width: every pixel must
 * be the palette entry of the index the per-pixel expansion would give.
 *
 * Arguments:
 *   bits: 1, 2, 4 or 8.
 *   width, height: Visible size; width may end mid-byte.
 */
static void check_indexed32(int bits, int width, int height)
{
	uint32 palette[256];
	for (int i = 0; i < 256; i++)
		palette[i] = 0x00010203u * (uint32)i ^ 0x00a5c3e1u;	// Distinct per entry

	int src_bpr = (width * bits + 7) / 8 + 4;
	int pitch = width * 4 + 16;
	uint8 *src = (uint8 *)malloc(src_bpr * height);
	uint8 *dst = (uint8 *)malloc(pitch * height);
	fill_random(src, src_bpr * height, bits * 2000 + width);
	memset(dst, kGuard, pitch * height);

	VideoBlit_IndexedToPixels32(src, src_bpr, dst, pitch, width, height, bits, palette);

	bool ok = true, guards = true;
	int per_byte = 8 / bits, mask = (1 << bits) - 1;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			int shift = (per_byte - 1 - (x % per_byte)) * bits;
			uint32 want = palette[(src[y * src_bpr + x / per_byte] >> shift) & mask];
			uint32 got;
			memcpy(&got, dst + y * pitch + x * 4, 4);
			if (got != want)
				ok = false;
		}
		if (!guard_intact(dst + y * pitch, width * 4, pitch))
			guards = false;
	}

	char msg[112];
	snprintf(msg, sizeof(msg), "%d-bit %dx%d to 32-bit matches the palette of each index", bits, width, height);
	CHECK(ok, msg);
	snprintf(msg, sizeof(msg), "%d-bit %dx%d to 32-bit writes nothing past the visible width", bits, width, height);
	CHECK(guards, msg);
	free(src);
	free(dst);
}

// Host channel layout used for the 15-bit table checks
struct TestFormat {
	int rshift, gshift, bshift;	// Bit position of each channel
	int rloss, gloss, bloss;	// Low bits dropped from each 8-bit channel
};

/*
 * SDL_MapRGB-style mapping for a TestFormat, used to build the table and to
 * compute the expected pixels.
 */
static uint32 map_test(void *ctx, uint8 r, uint8 g, uint8 b)
{
	const TestFormat *f = (const TestFormat *)ctx;
	return ((uint32)(r >> f->rloss) << f->rshift) | ((uint32)(g >> f->gloss) << f->gshift) |
	       ((uint32)(b >> f->bloss) << f->bshift);
}

/*
 * Checks VideoBlit_RGB555BE for one host format against computing each
 * pixel directly.
 *
 * Arguments:
 *   fmt: Host channel layout.
 *   bpp: Host bytes per pixel, 2 or 4.
 *   name: Printed with the check.
 */
static void check_rgb555(TestFormat fmt, int bpp, const char *name)
{
	static uint32 table[32768];
	const int width = 37, height = 5;
	int src_bpr = width * 2 + 6;
	int pitch = width * bpp + 12;
	uint8 *src = (uint8 *)malloc(src_bpr * height);
	uint8 *dst = (uint8 *)malloc(pitch * height);
	fill_random(src, src_bpr * height, 555 + bpp);
	memset(dst, kGuard, pitch * height);

	VideoBlit_BuildRGB555Table(table, map_test, &fmt);
	VideoBlit_RGB555BE(src, src_bpr, dst, pitch, width, height, table, bpp);

	bool ok = true, guards = true;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			// The pre-table loop, bit 15 included in the source but ignored
			const uint8 *s = src + y * src_bpr + x * 2;
			uint16 p = (uint16)((s[0] << 8) | s[1]);
			uint32 want = map_test(&fmt, (uint8)(((p >> 10) & 0x1f) * 255 / 31),
			                       (uint8)(((p >> 5) & 0x1f) * 255 / 31),
			                       (uint8)((p & 0x1f) * 255 / 31));
			uint32 got;
			if (bpp == 4) {
				uint32 v;
				memcpy(&v, dst + y * pitch + x * 4, 4);
				got = v;
			} else {
				uint16 v;
				memcpy(&v, dst + y * pitch + x * 2, 2);
				got = v;
				want &= 0xffff;
			}
			if (got != want)
				ok = false;
		}
		if (!guard_intact(dst + y * pitch, width * bpp, pitch))
			guards = false;
	}

	char msg[96];
	snprintf(msg, sizeof(msg), "16-bit to %s matches mapping each pixel", name);
	CHECK(ok, msg);
	snprintf(msg, sizeof(msg), "16-bit to %s writes nothing past the visible width", name);
	CHECK(guards, msg);
	free(src);
	free(dst);
}

/*
 * Checks VideoBlit_XRGB8888BE against building each XRGB8888 pixel from the
 * Mac R, G and B bytes, and that it ignores the Mac pad byte's position.
 */
static void check_xrgb8888(void)
{
	const int width = 29, height = 4;
	int src_bpr = width * 4 + 8;
	int pitch = width * 4 + 20;
	uint8 *src = (uint8 *)malloc(src_bpr * height);
	uint8 *dst = (uint8 *)malloc(pitch * height);
	fill_random(src, src_bpr * height, 8888);
	// Mac 32-bit pixels have a zero pad byte first
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++)
			src[y * src_bpr + x * 4] = 0;
	memset(dst, kGuard, pitch * height);

	VideoBlit_XRGB8888BE(src, src_bpr, dst, pitch, width, height);

	bool ok = true, guards = true;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			const uint8 *s = src + y * src_bpr + x * 4;
			uint32 want = ((uint32)s[1] << 16) | ((uint32)s[2] << 8) | s[3];
			uint32 got;
			memcpy(&got, dst + y * pitch + x * 4, 4);
			if (got != want)
				ok = false;
		}
		if (!guard_intact(dst + y * pitch, width * 4, pitch))
			guards = false;
	}
	CHECK(ok, "32-bit byte swap matches R, G, B placed in an XRGB8888 word");
	CHECK(guards, "32-bit byte swap writes nothing past the visible width");
	free(src);
	free(dst);
}

int main(void)
{
	test_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("=== basilisk_blit_test ===\n");

	// Widths that end on, and part way through, a source byte
	static const int widths[] = {1, 7, 8, 9, 64, 67};
	for (int bits = 1; bits <= 4; bits *= 2)
		for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++)
			check_indexed(bits, widths[i], 3);

	for (int bits = 1; bits <= 8; bits *= 2)
		for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++)
			check_indexed32(bits, widths[i], 3);

	TestFormat rgb565 = {11, 5, 0, 3, 2, 3};
	TestFormat rgb555 = {10, 5, 0, 3, 3, 3};
	TestFormat xrgb8888 = {16, 8, 0, 0, 0, 0};
	check_rgb555(rgb565, 2, "host RGB565");
	check_rgb555(rgb555, 2, "host RGB555");
	check_rgb555(xrgb8888, 4, "host XRGB8888");

	check_xrgb8888();

	printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
