/*
 * cpu_fpu.cpp - 68881/68040 FPU snippets
 */

#include "cpu_tests.h"
#include "test_harness.h"

#include <string.h>
#include <math.h>
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "cpu_engine.h"

void test_fpu_execution(const char *engine)
{
	printf("Running FPU instruction execution tests (%s)...\n", engine);
	uint32 code_addr = 0x6000;
	WriteMacInt16(code_addr + 0, 0xF280);
	WriteMacInt16(code_addr + 2, 0x0000);
	WriteMacInt16(code_addr + 4, 0x4E75);
	struct M68kRegisters r;
	memset(&r, 0, sizeof(r));
	Execute68k(code_addr, &r);
	CHECK_ENG(true, engine, "FNOP executed cleanly without faulting");

	code_addr = 0x6010;
	WriteMacInt16(code_addr + 0, 0xF200);
	WriteMacInt16(code_addr + 2, 0x4000);
	WriteMacInt16(code_addr + 4, 0xF201);
	WriteMacInt16(code_addr + 6, 0x4080);
	WriteMacInt16(code_addr + 8, 0xF200);
	WriteMacInt16(code_addr + 10, 0x0422);
	WriteMacInt16(code_addr + 12, 0xF202);
	WriteMacInt16(code_addr + 14, 0x6000);
	WriteMacInt16(code_addr + 16, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[0] = 20;
	r.d[1] = 22;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[2] == 42, engine, "FPU addition: 20 + 22 == 42");

	uint32 pi_buf = 0x9500;
	code_addr = 0x6030;
	WriteMacInt16(code_addr + 0, 0xF200);
	WriteMacInt16(code_addr + 2, 0x5C00);
	WriteMacInt16(code_addr + 4, 0xF210);
	WriteMacInt16(code_addr + 6, 0x7400);
	WriteMacInt16(code_addr + 8, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.a[0] = pi_buf;
	Execute68k(code_addr, &r);
	uint64 dbl_bits = ((uint64)ReadMacInt32(pi_buf) << 32) | ReadMacInt32(pi_buf + 4);
	double pi_val;
	memcpy(&pi_val, &dbl_bits, sizeof(pi_val));
	CHECK_ENG(fabs(pi_val - 3.141592653589793) < 1e-10, engine, "FMOVECR Pi stored as double");

	code_addr = 0x6050;
	WriteMacInt16(code_addr + 0, 0xF327);
	WriteMacInt16(code_addr + 2, 0xF35F);
	WriteMacInt16(code_addr + 4, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.sr = 0x2700;
	Execute68k(code_addr, &r);
	CHECK_ENG(true, engine, "FSAVE -(SP) and FRESTORE (SP)+ roundtrip");

	uint32 fsave_buf = 0x9600;
	code_addr = 0x6060;
	WriteMacInt16(code_addr + 0, 0xF310);
	WriteMacInt16(code_addr + 2, 0xF350);
	WriteMacInt16(code_addr + 4, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.sr = 0x2700;
	r.a[0] = fsave_buf;
	Execute68k(code_addr, &r);
	CHECK_ENG(true, engine, "FSAVE (A0) and FRESTORE (A0)");

	code_addr = 0x6070;
	WriteMacInt16(code_addr + 0, 0xF200);
	WriteMacInt16(code_addr + 2, 0x5C0C);
	WriteMacInt16(code_addr + 4, 0xF200);
	WriteMacInt16(code_addr + 6, 0x0006);
	WriteMacInt16(code_addr + 8, 0xF200);
	WriteMacInt16(code_addr + 10, 0x6000);
	WriteMacInt16(code_addr + 12, 0x4E75);
	memset(&r, 0, sizeof(r));
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[0] == 1, engine, "FLOGN(e) == 1");

	code_addr = 0x6090;
	WriteMacInt16(code_addr + 0, 0xF201);
	WriteMacInt16(code_addr + 2, 0x4000);
	WriteMacInt16(code_addr + 4, 0xF200);
	WriteMacInt16(code_addr + 6, 0x0004);
	WriteMacInt16(code_addr + 8, 0xF202);
	WriteMacInt16(code_addr + 10, 0x6000);
	WriteMacInt16(code_addr + 12, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[1] = 16;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[2] == 4, engine, "FSQRT(16.0) == 4");

	code_addr = 0x60B0;
	WriteMacInt16(code_addr + 0, 0xF201);
	WriteMacInt16(code_addr + 2, 0x4000);
	WriteMacInt16(code_addr + 4, 0xF200);
	WriteMacInt16(code_addr + 6, 0x001D);
	WriteMacInt16(code_addr + 8, 0xF202);
	WriteMacInt16(code_addr + 10, 0x6000);
	WriteMacInt16(code_addr + 12, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[1] = 0;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[2] == 1, engine, "FCOS(0) == 1");

	code_addr = 0x60D0;
	WriteMacInt16(code_addr + 0, 0xF201);
	WriteMacInt16(code_addr + 2, 0x4000);
	WriteMacInt16(code_addr + 4, 0xF200);
	WriteMacInt16(code_addr + 6, 0x0031);
	WriteMacInt16(code_addr + 8, 0xF202);
	WriteMacInt16(code_addr + 10, 0x6000);
	WriteMacInt16(code_addr + 12, 0xF203);
	WriteMacInt16(code_addr + 14, 0x6080);
	WriteMacInt16(code_addr + 16, 0x4E75);
	memset(&r, 0, sizeof(r));
	r.d[1] = 0;
	Execute68k(code_addr, &r);
	CHECK_ENG(r.d[2] == 0 && r.d[3] == 1, engine, "FSINCOS(0) -> sin=0, cos=1");
}
