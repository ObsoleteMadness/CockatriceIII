/*
 *  emu68_lru.c - Set-associative JIT LRU for the hosted TARGET
 *
 *  Copied from upstream ExecutionLoop.c so Cockatrice does not compile that
 *  file's bare-metal MainLoop (register asm PC in w18, WFI, PiStorm IPL).
 *  Translator hash-table LRU (struct List LRU) is separate and stays in
 *  M68k_Translator.c; these helpers keep GetTranslationUnit's eviction
 *  path from calling empty stubs.
 */

#include <stdint.h>
#include <stddef.h>

#include "../include/config.h"
#include "support.h"
#include "M68k.h"

struct Entry {
	uintptr_t m68k;
	uint32_t *arm;
};

static struct Entry LRU_cache[EMU68_LRU_WAY_COUNT * EMU68_LRU_SET_COUNT] __attribute__((aligned(64)));
static uint32_t LRU_alloc[EMU68_LRU_SET_COUNT];

#define ADDR_2_SET(addr) (((addr) >> 4) % EMU68_LRU_SET_COUNT)

/*
 * Looks up a translation-unit entry point by 680x0 PC in the set-associative
 * cache. Touches the matching way so it is not the next eviction victim.
 *
 * Parameters:
 *   address - Guest PC used as the block key.
 *
 * Returns:
 *   ARM entry point, or NULL on miss.
 */
uint32_t *LRU_FindBlock(uint32_t address)
{
	const uint32_t set = ADDR_2_SET(address);
	struct Entry *e = &LRU_cache[set * EMU68_LRU_WAY_COUNT];
	uint32_t mask = 0x80000000u;

	for (int i = 0; i < EMU68_LRU_WAY_COUNT; i++, mask >>= 1) {
		if (e[i].m68k == address) {
			uint32_t current = LRU_alloc[set] & ~mask;
			if (current >> (32 - EMU68_LRU_WAY_COUNT) == 0)
				current = ~mask;
			LRU_alloc[set] = current;
			return e[i].arm;
		}
	}
	return NULL;
}

/*
 * Marks a unit so the next execute path will CRC-verify it (self-modifying
 * code). The high-byte tag matches upstream ExecutionLoop.c.
 *
 * Parameters:
 *   addr - ARM entry point stored in the cache.
 */
void LRU_MarkForVerify(uint32_t *addr)
{
	for (int i = 0; i < EMU68_LRU_SET_COUNT * EMU68_LRU_WAY_COUNT; i++) {
		if (LRU_cache[i].arm == addr) {
			uintptr_t e = (uintptr_t)addr;
			e &= 0x00ffffffffffffffULL;
			e |= 0xaa00000000000000ULL;
			LRU_cache[i].arm = (uint32_t *)e;
			break;
		}
	}
}

/*
 * Drops the way whose ARM entry matches addr.
 *
 * Parameters:
 *   addr - ARM entry point to invalidate.
 */
void LRU_InvalidateByARMAddress(uint32_t *addr)
{
	for (int i = 0; i < EMU68_LRU_SET_COUNT * EMU68_LRU_WAY_COUNT; i++) {
		if (LRU_cache[i].arm == addr) {
			const uint32_t set = (uint32_t)i / EMU68_LRU_WAY_COUNT;
			const uint32_t way = (uint32_t)i % EMU68_LRU_WAY_COUNT;
			LRU_cache[i].arm = (void *)0;
			LRU_cache[i].m68k = 0xffffffffu;
			LRU_alloc[set] |= (0x80000000u >> way);
			break;
		}
	}
}

/*
 * Drops the way whose guest PC matches addr.
 *
 * Parameters:
 *   addr - 680x0 block start address.
 */
void LRU_InvalidateByM68kAddress(uint32_t addr)
{
	const uint32_t set = ADDR_2_SET(addr);
	struct Entry *e = &LRU_cache[set * EMU68_LRU_WAY_COUNT];

	for (int i = 0; i < EMU68_LRU_WAY_COUNT; i++) {
		if (e[i].m68k == addr) {
			e[i].arm = (void *)0;
			e[i].m68k = 0xffffffffu;
			LRU_alloc[set] |= (0x80000000u >> i);
			break;
		}
	}
}

/*
 * Clears every way. Called on CACR I-cache disable and hosted cache_invalidate_all.
 */
void LRU_InvalidateAll(void)
{
	for (int i = 0; i < EMU68_LRU_SET_COUNT * EMU68_LRU_WAY_COUNT; i++) {
		LRU_cache[i].m68k = 0xffffffffu;
		LRU_cache[i].arm = (void *)0;
	}
	for (int i = 0; i < EMU68_LRU_SET_COUNT; i++)
		LRU_alloc[i] = 0xffffffffu;
}

/*
 * Inserts a newly translated unit into the set-associative cache.
 *
 * Parameters:
 *   unit - Translation unit whose mt_M68kAddress / mt_ARMEntryPoint are keyed.
 */
void LRU_InsertBlock(struct M68KTranslationUnit *unit)
{
	const uint32_t set = ADDR_2_SET(unit->mt_M68kAddress);
	struct Entry *e = &LRU_cache[set * EMU68_LRU_WAY_COUNT];
	int loc = __builtin_clz(LRU_alloc[set]);
	uint32_t mask = 0x80000000u >> loc;

	e[loc].m68k = unit->mt_M68kAddress;
	e[loc].arm = (uint32_t *)unit->mt_ARMEntryPoint;

	uint32_t current = LRU_alloc[set] & ~mask;
	if (current >> (32 - EMU68_LRU_WAY_COUNT) == 0)
		current = ~mask;
	LRU_alloc[set] = current;
}
