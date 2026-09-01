/*
 *  amiberry_memory.cpp - Amiberry memory_* entry points backed by Mac banks
 *
 *  Amiberry's CPU/JIT call memory_get_long and friends. Those are forwarded
 *  to Basilisk II's banked Macintosh RAM/ROM via the C API in amiberry_cpu_api.h
 *  so this file can compile against Amiberry headers.
 */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "amiberry_cpu_api.h"

uae_u32 memory_get_long(uaecptr addr)
{
	return cockatrice_mac_get_long(addr);
}

uae_u32 memory_get_word(uaecptr addr)
{
	return cockatrice_mac_get_word(addr);
}

uae_u32 memory_get_byte(uaecptr addr)
{
	return cockatrice_mac_get_byte(addr);
}

uae_u32 memory_get_longi(uaecptr addr)
{
	return cockatrice_mac_get_long(addr);
}

uae_u32 memory_get_wordi(uaecptr addr)
{
	return cockatrice_mac_get_word(addr);
}

void memory_put_long(uaecptr addr, uae_u32 v)
{
	if (!memory_valid_address(addr, 4))
		cockatrice_memory_raise_guest_fault((uint32_t)addr);
	cockatrice_mac_put_long(addr, v);
}

void memory_put_word(uaecptr addr, uae_u32 v)
{
	if (!memory_valid_address(addr, 2))
		cockatrice_memory_raise_guest_fault((uint32_t)addr);
	cockatrice_mac_put_word(addr, v);
}

void memory_put_byte(uaecptr addr, uae_u32 v)
{
	if (!memory_valid_address(addr, 1))
		cockatrice_memory_raise_guest_fault((uint32_t)addr);
	cockatrice_mac_put_byte(addr, v);
}

uae_u8 *memory_get_real_address(uaecptr addr)
{
	return cockatrice_mac_host_addr(addr);
}

int memory_valid_address(uaecptr addr, uae_u32 size)
{
	return cockatrice_mac_valid_addr(addr, size);
}

bool real_address_allowed(void)
{
	/* Indirect JIT: host pointers are valid for RAM/ROM xlate but not chipset. */
	return true;
}
