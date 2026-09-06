/*
 *  cputest_host.cpp - Hosted stubs for building Amiberry uae_cputest on Cockatrice
 *
 *  Links with vendored cputest.cpp (CPU_TESTER core). Provides symbols that
 *  full Amiberry/WinUAE supply but Cockatrice omits.
 */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "newcpu.h"
#include "fpp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct uae_prefs currprefs;
struct uae_prefs changed_prefs;

bool canbang = false;

void write_log(const TCHAR *fmt, ...)
{
	(void)fmt;
}

void f_out(void *f, const TCHAR *fmt, ...)
{
	(void)f;
	(void)fmt;
}

TCHAR *buf_out(TCHAR *buffer, int *bufsize, const TCHAR *format, ...)
{
	(void)buffer;
	(void)bufsize;
	(void)format;
	return buffer;
}

void fpux_restore(int *v)
{
	(void)v;
}

void fp_init_native(void)
{
}

bool fp_init_native_80(void)
{
	return false;
}

void init_fpucw_x87(void)
{
}

void init_fpucw_x87_80(void)
{
}

int debugmem_get_segment(uaecptr addr, bool *exact, bool *ext, TCHAR *out, TCHAR *name)
{
	(void)addr;
	(void)exact;
	(void)ext;
	(void)out;
	(void)name;
	return 0;
}

int debugmem_get_symbol(uaecptr addr, TCHAR *out, int maxsize)
{
	(void)addr;
	(void)out;
	(void)maxsize;
	return 0;
}

int debugmem_get_sourceline(uaecptr addr, TCHAR *out, int maxsize)
{
	(void)addr;
	(void)out;
	(void)maxsize;
	return -1;
}

bool debugger_get_library_symbol(uaecptr base, uaecptr addr, TCHAR *out)
{
	(void)base;
	(void)addr;
	(void)out;
	return false;
}

int debug_safe_addr(uaecptr addr, int size)
{
	(void)addr;
	(void)size;
	return 1;
}

void set_cpu_caches(bool flush)
{
	(void)flush;
}

void (*flush_icache)(int);

void flush_cpu_caches_040(uae_u16 opcode)
{
	(void)opcode;
}

void mmu_tt_modified(void)
{
}

uae_u16 REGPARAM2 mmu_set_tc(uae_u16 tc)
{
	(void)tc;
	return 0;
}

void mmu_op(uae_u32 opcode, uae_u32 extra)
{
	(void)opcode;
	(void)extra;
}

uae_u16 mmu030_state[3];
int mmu030_opcode;
int mmu030_idx, mmu030_idx_done;
uae_u32 mmu030_disp_store[2];
uae_u32 mmu030_fmovem_store[2];
uae_u32 mm030_stageb_address;
struct mmu030_access mmu030_ad[16];

uae_u32 mmu040_move16[4];

cpuop_func *loop_mode_table[65536];
int cpuipldelay2, cpuipldelay4;

void disasm_init(void)
{
}

void disasm_open(void)
{
}

void disasm_close(void)
{
}

int disasm(uae_u8 *mem, int max, uaecptr pc, TCHAR *out, int more)
{
	(void)mem;
	(void)max;
	(void)pc;
	(void)more;
	if (out)
		out[0] = 0;
	return 0;
}

void cputester_fault(void)
{
}
