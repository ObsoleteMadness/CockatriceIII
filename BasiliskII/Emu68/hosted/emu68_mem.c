/*
 *  emu68_mem.c - Rewrite JIT load/store through the hosted 4GB window
 *
 *  Bare-metal Emu68 identity-maps 680x0 addresses, so `ldr [An]` is a host
 *  pointer. Darwin PAGEZERO occupies the low 4GB, so An is a 32-bit guest
 *  offset. HOST_MEM_BASE lives in v22.d[0] (loaded from Host_Mem_Base);
 *  x12 is the upstream-reserved scratch (RA never allocates it;
 *  ExecutionLoop pins it as ARMCode).
 *
 *  64-bit loads/stores and Rn==SP stay as-is (CTX, JIT metadata, ARM frames).
 *  A 32-bit base with high bits set is already a host pointer (CTX); those
 *  skip the window add. NZCV is not touched so 68k CCR emission stays valid.
 */

#include "support.h"
#include "M68k.h"
#include "emu68_hosted.h"

/* Upstream RA never hands out x12; it is the hosted window-address scratch. */
#define HOST_ADDR_TMP 12
#define HOST_MEM_V    22

enum {
	IDX_UNSIGNED = 0,
	IDX_UNSCALED,
	IDX_POST,
	IDX_PRE,
	IDX_REGOFF
};

/*
 * Writes one already-encoded AArch64 word into the translation buffer.
 *
 * Parameters:
 *   ctx  - Translator context whose tc_CodePtr is the emit cursor.
 *   insn - Instruction word from an A64.h helper (already I32()'d).
 */
static void raw_put(struct TranslatorContext *ctx, uint32_t insn)
{
	*(ctx->tc_CodePtr)++ = insn;
}

/*
 * Adds a signed 32-bit immediate to a W register (guest An writeback).
 *
 * Parameters:
 *   ctx - Translator context.
 *   rd  - Destination W register (usually the same as rn).
 *   rn  - Source W register.
 *   imm - Signed displacement; magnitudes fit add/sub_immed's 12-bit field.
 */
static void emit_add_s32(struct TranslatorContext *ctx, uint8_t rd, uint8_t rn, int32_t imm)
{
	if (imm == 0) {
		if (rd != rn)
			raw_put(ctx, mov_reg(rd, rn));
		return;
	}
	if (imm > 0)
		raw_put(ctx, add_immed(rd, rn, (uint16_t)imm));
	else
		raw_put(ctx, sub_immed(rd, rn, (uint16_t)(-imm)));
}

/*
 * Materializes a host pointer for a 32-bit guest address, or keeps a 64-bit
 * host pointer (CTX) unchanged. Result is always in x12. Does not write NZCV.
 *
 * Parameters:
 *   ctx - Translator context.
 *   rn  - Base register from the original load/store (An, EA temp, or CTX).
 */
static void emit_host_addr(struct TranslatorContext *ctx, uint8_t rn)
{
	/* x12 = HOST_MEM_BASE + UXTW(rn) is the guest-window candidate. */
	raw_put(ctx, umov64_d(HOST_ADDR_TMP, HOST_MEM_V, 0));
	raw_put(ctx, add64_reg_ext(HOST_ADDR_TMP, HOST_ADDR_TMP, rn, UXTW, 0));
	/* Stash the candidate in v22.d[1] so the high-bit test can reuse x12. */
	raw_put(ctx, ins64_d(HOST_MEM_V, 1, HOST_ADDR_TMP));
	/* x12 = 0 iff rn's high 32 bits are clear (a 680x0 address). */
	raw_put(ctx, sub64_reg_ext(HOST_ADDR_TMP, rn, rn, UXTW, 0));
	/* Host path: skip the umov/b and use rn as-is. Offset 3 lands on mov. */
	raw_put(ctx, cbnz_64(HOST_ADDR_TMP, 3));
	raw_put(ctx, umov64_d(HOST_ADDR_TMP, HOST_MEM_V, 1));
	raw_put(ctx, b(2));
	raw_put(ctx, mov64_reg(HOST_ADDR_TMP, rn));
}

/*
 * Byteswaps a loaded 16/32-bit value and sign-extends if the original insn did.
 *
 * Parameters:
 *   ctx         - Translator context.
 *   size        - 0=byte, 1=halfword, 2=word.
 *   is_signed   - Nonzero if the original encoding was ldrsb/ldrsh/ldrsw.
 *   is_signed64 - Nonzero if the signed form writes a 64-bit destination.
 *   rt          - Data register. XZR is skipped.
 */
static void emit_endian_after_load(struct TranslatorContext *ctx, int size, int is_signed,
				   int is_signed64, uint8_t rt)
{
	if (rt == 31)
		return;
	if (size == 2) {
		raw_put(ctx, rev(rt, rt));
		if (is_signed)
			raw_put(ctx, sxtw64(rt, rt));
	} else if (size == 1) {
		raw_put(ctx, rev16(rt, rt));
		if (is_signed) {
			if (is_signed64)
				raw_put(ctx, sxth64(rt, rt));
			else
				raw_put(ctx, sxth(rt, rt));
		}
	} else if (size == 0 && is_signed) {
		if (is_signed64)
			raw_put(ctx, sxtb64(rt, rt));
		else
			raw_put(ctx, sxtb(rt, rt));
	}
}

/*
 * Byteswaps a 16/32-bit register, stores it, then restores the 68k register.
 *
 * Parameters:
 *   ctx  - Translator context.
 *   size - 0=byte (no swap), 1=halfword, 2=word.
 *   rt   - Data register. XZR stores zero in any endianness.
 */
static void emit_endian_before_store(struct TranslatorContext *ctx, int size, uint8_t rt)
{
	if (rt == 31 || size == 0)
		return;
	if (size == 2)
		raw_put(ctx, rev(rt, rt));
	else
		raw_put(ctx, rev16(rt, rt));
}

/*
 * Undoes emit_endian_before_store so Dn/An keep their guest-endian value.
 *
 * Parameters:
 *   ctx  - Translator context.
 *   size - 0=byte, 1=halfword, 2=word.
 *   rt   - Data register that was swapped in place.
 */
static void emit_endian_restore_store(struct TranslatorContext *ctx, int size, uint8_t rt)
{
	emit_endian_before_store(ctx, size, rt);
}

/*
 * Replaces Rn in an already-encoded load/store with the window scratch.
 *
 * Parameters:
 *   insn - Original I32()'d instruction word.
 *
 * Returns:
 *   The same encoding with bits [9:5] set to x12.
 */
static uint32_t retarget_rn(uint32_t insn)
{
	return (insn & ~0x3E0u) | ((uint32_t)HOST_ADDR_TMP << 5);
}

/*
 * Forces a signed load encoding to the unsigned load of the same size so the
 * following rev/sxth sequence sees raw memory bytes.
 *
 * Parameters:
 *   insn - Original load encoding (opc in bits 23:22).
 *
 * Returns:
 *   Encoding with opc=01 (unsigned load).
 */
static uint32_t as_unsigned_load(uint32_t insn)
{
	return (insn & ~0x00C00000u) | 0x00400000u;
}

/*
 * Emits an unsigned size-matched load or store at [x12] with offset 0.
 *
 * Parameters:
 *   ctx     - Translator context.
 *   size    - 0=byte, 1=halfword, 2=word.
 *   is_load - Nonzero to load, zero to store.
 *   rt      - Data register.
 */
static void emit_mem_x12_0(struct TranslatorContext *ctx, int size, int is_load, uint8_t rt)
{
	if (is_load) {
		if (size == 0)
			raw_put(ctx, ldrb_offset(HOST_ADDR_TMP, rt, 0));
		else if (size == 1)
			raw_put(ctx, ldrh_offset(HOST_ADDR_TMP, rt, 0));
		else
			raw_put(ctx, ldr_offset(HOST_ADDR_TMP, rt, 0));
	} else {
		if (size == 0)
			raw_put(ctx, strb_offset(HOST_ADDR_TMP, rt, 0));
		else if (size == 1)
			raw_put(ctx, strh_offset(HOST_ADDR_TMP, rt, 0));
		else
			raw_put(ctx, str_offset(HOST_ADDR_TMP, rt, 0));
	}
}

/*
 * Sign-extends a 9-bit unscaled/pre/post immediate from bits [20:12].
 *
 * Parameters:
 *   insn - AArch64 load/store instruction word.
 *
 * Returns:
 *   Signed displacement in the range -256..255.
 */
static int32_t imm9_of(uint32_t insn)
{
	int32_t v = (int32_t)((insn >> 12) & 0x1ff);
	if (v & 0x100)
		v |= ~0x1ff;
	return v;
}

/*
 * Sign-extends a 7-bit pair immediate from bits [21:15], scaled by 4.
 *
 * Parameters:
 *   insn - 32-bit ldp/stp instruction word.
 *
 * Returns:
 *   Byte displacement applied to the 32-bit pair.
 */
static int32_t pair_imm_of(uint32_t insn)
{
	int32_t v = (int32_t)((insn >> 15) & 0x7f);
	if (v & 0x40)
		v |= ~0x7f;
	return v * 4;
}

/*
 * Rewrites one 8/16/32-bit integer load/store through HOST_MEM_BASE.
 *
 * Parameters:
 *   ctx  - Translator context.
 *   insn - Single AArch64 word from EMIT. Already I32()'d.
 *
 * Returns:
 *   1 if this function consumed the insn, 0 if the caller should emit as-is.
 */
int emu68_hosted_rewrite_mem(struct TranslatorContext *ctx, uint32_t insn)
{
	uint8_t rn = (insn >> 5) & 31;
	uint8_t rt = insn & 31;
	int size;
	int opc;
	int is_load;
	int is_signed;
	int is_signed64;
	int idx;

	/* 32-bit integer pair (MOVEM). 64-bit ldp/stp and ARM SP stay native. */
	if ((insn & 0xFE000000u) == 0x28000000u) {
		uint8_t rt2 = (insn >> 10) & 31;
		int is_pair_load = (insn >> 22) & 1;
		int mode = (insn >> 23) & 3;
		int32_t imm;

		if (rn == 31 || rn == HOST_ADDR_TMP)
			return 0;
		if (!is_pair_load && (rt == HOST_ADDR_TMP || rt2 == HOST_ADDR_TMP))
			return 0;

		imm = pair_imm_of(insn);
		if (mode == 3)
			emit_add_s32(ctx, rn, rn, imm);
		emit_host_addr(ctx, rn);
		if (is_pair_load) {
			raw_put(ctx, ldp(HOST_ADDR_TMP, rt, rt2, (mode == 2) ? imm : 0));
			emit_endian_after_load(ctx, 2, 0, 0, rt);
			emit_endian_after_load(ctx, 2, 0, 0, rt2);
		} else {
			emit_endian_before_store(ctx, 2, rt);
			emit_endian_before_store(ctx, 2, rt2);
			raw_put(ctx, stp(HOST_ADDR_TMP, rt, rt2, (mode == 2) ? imm : 0));
			emit_endian_restore_store(ctx, 2, rt);
			emit_endian_restore_store(ctx, 2, rt2);
		}
		if (mode == 1)
			emit_add_s32(ctx, rn, rn, imm);
		return 1;
	}

	/* Integer ldr/str: bits [29:27]=111, not SIMD, not 64-bit. */
	if (((insn >> 26) & 1) != 0)
		return 0;
	if (((insn >> 27) & 7) != 7)
		return 0;
	size = (int)(insn >> 30);
	if (size == 3)
		return 0;
	if (rn == 31 || rn == HOST_ADDR_TMP)
		return 0;

	opc = (int)((insn >> 22) & 3);
	is_load = (opc != 0);
	is_signed = (opc >= 2);
	is_signed64 = (opc == 2);
	if (!is_load && rt == HOST_ADDR_TMP)
		return 0;

	if (((insn >> 24) & 3) == 1)
		idx = IDX_UNSIGNED;
	else if ((insn & (1u << 21)) != 0)
		idx = IDX_REGOFF;
	else {
		unsigned bits = (insn >> 10) & 3;
		if (bits == 0)
			idx = IDX_UNSCALED;
		else if (bits == 1)
			idx = IDX_POST;
		else if (bits == 3)
			idx = IDX_PRE;
		else
			return 0;
	}

	if (idx == IDX_PRE)
		emit_add_s32(ctx, rn, rn, imm9_of(insn));

	emit_host_addr(ctx, rn);

	if (idx == IDX_POST || idx == IDX_PRE) {
		if (!is_load)
			emit_endian_before_store(ctx, size, rt);
		emit_mem_x12_0(ctx, size, is_load, rt);
		if (is_load)
			emit_endian_after_load(ctx, size, is_signed, is_signed64, rt);
		else
			emit_endian_restore_store(ctx, size, rt);
		if (idx == IDX_POST)
			emit_add_s32(ctx, rn, rn, imm9_of(insn));
		return 1;
	}

	if (!is_load)
		emit_endian_before_store(ctx, size, rt);

	if (is_signed)
		raw_put(ctx, retarget_rn(as_unsigned_load(insn)));
	else
		raw_put(ctx, retarget_rn(insn));

	if (is_load)
		emit_endian_after_load(ctx, size, is_signed, is_signed64, rt);
	else
		emit_endian_restore_store(ctx, size, rt);
	return 1;
}
