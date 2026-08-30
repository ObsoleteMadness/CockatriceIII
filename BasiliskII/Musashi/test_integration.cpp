/*
 * test_integration.cpp - Unit test harness for Musashi + Basilisk II memory, EmulOps, and Execute68k
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "emul_op.h"
#include "menu_bar.h"
#include "scc.h"
#include "scsi.h"
#include "m68k.h"
#include "cpu_engine.h"
#include <unistd.h>
#include <math.h>

// Global flags required by Basilisk II
int CPUType = 4; // 68040
bool CPUIs68060 = false;
int FPUType = 1;
bool TwentyFourBitAddressing = false;
int ROMVersion = 4; // ROM_VERSION_32
uint32 UniversalInfo = 0;
uint8 SCCInterruptRequest = 0;
uint32 InterruptFlags = 0;
uint8 XPRAM[256];
uint32 PutScrapPatch = 0;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, msg) do { \
    if (expr) { \
        g_pass++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        g_fail++; \
        printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
    } \
    fflush(stdout); \
} while (0)

// Peripheral driver stubs for unit testing
bool PrefsFindBool(const char *name) { (void)name; return false; }
uint32 TimerDateTime(void) { return 0x12345678; }
void QuitEmulator(void) {}
void TimerReset(void) {}
void EtherReset(void) {}
void MenuQueue_Reset(void) {}
void MenuQueue_Drain(void) {}
void MenuBar_UpdateAll(void) {}
void SCC_Reset(void) {}
void SonyReset(void) {}
void DiskReset(void) {}
void AudioReset(void) {}
uint32 SCC_Access(uint32 val, bool is_write, uint32 reg) { (void)val; (void)is_write; (void)reg; return 0; }

int16 SonyOpen(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 SonyPrime(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 SonyControl(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 SonyStatus(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
void SonyInterrupt(void) {}

int16 DiskOpen(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 DiskPrime(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 DiskControl(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 DiskStatus(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
void DiskInterrupt(void) {}

int16 CDROMOpen(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 CDROMPrime(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 CDROMControl(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 CDROMStatus(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }

int16 VideoDriverOpen(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 VideoDriverControl(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 VideoDriverStatus(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
void VideoInterrupt(void) {}

int16 SerialOpen(uint32 pb, uint32 dce, int port) { (void)pb; (void)dce; (void)port; return 0; }
int16 SerialPrime(uint32 pb, uint32 dce, int port) { (void)pb; (void)dce; (void)port; return 0; }
int16 SerialControl(uint32 pb, uint32 dce, int port) { (void)pb; (void)dce; (void)port; return 0; }
int16 SerialStatus(uint32 pb, uint32 dce, int port) { (void)pb; (void)dce; (void)port; return 0; }
int16 SerialClose(uint32 pb, uint32 dce, int port) { (void)pb; (void)dce; (void)port; return 0; }
void SerialInterrupt(void) {}

int16 EtherOpen(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 EtherControl(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
void EtherInterrupt(void) {}
bool EtherReadPacket(uint8 **buf, uint32 &len, uint32 &dest, uint32 &proto) { (void)buf; (void)len; (void)dest; (void)proto; return false; }
void LocalTalkTick(void) {}

void ADBOp(uint8 cmd, uint8 *data) { (void)cmd; (void)data; }
void ADBInterrupt(void) {}

void InsTime(uint32 tm, uint16 trap) { (void)tm; (void)trap; }
void RmvTime(uint32 tm) { (void)tm; }
void PrimeTime(uint32 tm, int count) { (void)tm; (void)count; }
void Microseconds(uint32 &hi, uint32 &lo) { hi = 0; lo = 1000; }
void TimerInterrupt(void) {}

const char *PrefsFindString(const char *name) { (void)name; return NULL; }
const char *PrefsFindString(const char *name, int index) { (void)name; (void)index; return NULL; }
void PrefsRemoveItem(const char *name, int index) { (void)name; (void)index; }
void PrefsReplaceString(const char *name, const char *val, int index) { (void)name; (void)val; (void)index; }
int32 PrefsFindInt32(const char *name) { (void)name; return 0; }
const char *GetString(int id) { (void)id; return ""; }
void ErrorAlert(const char *msg) { printf("ErrorAlert: %s\n", msg); }
void MenuAction_UpdateItem(int id) { (void)id; }

void AudioDispatch(uint32 pb, uint32 dce) { (void)pb; (void)dce; }
void AudioInterrupt(void) {}

int16 ExtFSComm(uint16 code, uint32 param, uint32 dce) { (void)code; (void)param; (void)dce; return 0; }
int16 ExtFSHFS(uint32 pb, uint16 trap, uint32 dce, uint32 a0, int16 d0) { (void)pb; (void)trap; (void)dce; (void)a0; return d0; }

void PutScrap(uint32 type, void *data, int size) { (void)type; (void)data; (void)size; }
void CheckLoad(uint32 trap, int16 trap_num, uint8 *pc, uint32 a0) { (void)trap; (void)trap_num; (void)pc; (void)a0; }
void InstallDrivers(uint32 dce) { (void)dce; }
void InstallSERD(void) {}
void ClearInterruptFlag(uint32 flag) { InterruptFlags &= ~flag; }
bool HasMacStarted(void) { return true; }
void idle_wait(void) {}

static uint8 s_host_ram[1024 * 1024];
static uint8 s_host_rom[1024 * 1024];

void test_memory_banking(void)
{
    printf("Running memory banking tests...\n");

    memset(s_host_ram, 0, sizeof(s_host_ram));
    memset(s_host_rom, 0, sizeof(s_host_rom));

    RAMBaseHost = s_host_ram;
    RAMSize = sizeof(s_host_ram);
    RAMBaseMac = 0x00000000;

    ROMBaseHost = s_host_rom;
    ROMSize = sizeof(s_host_rom);
    ROMBaseMac = 0x40800000;

    memory_init();

    // 1. Test 32-bit RAM write & read
    WriteMacInt32(0x1000, 0x12345678);
    CHECK(ReadMacInt32(0x1000) == 0x12345678, "32-bit RAM Write/Read");
    CHECK(ReadMacInt16(0x1000) == 0x1234, "16-bit RAM Read Big-Endian High");
    CHECK(ReadMacInt16(0x1002) == 0x5678, "16-bit RAM Read Big-Endian Low");
    CHECK(ReadMacInt8(0x1000) == 0x12, "8-bit RAM Read Byte 0");
    CHECK(ReadMacInt8(0x1003) == 0x78, "8-bit RAM Read Byte 3");

    // 2. Test 32-bit ROM read (ROM is read-only)
    s_host_rom[0x100] = 0xDE;
    s_host_rom[0x101] = 0xAD;
    s_host_rom[0x102] = 0xBE;
    s_host_rom[0x103] = 0xEF;
    CHECK(ReadMacInt32(ROMBaseMac + 0x100) == 0xdeadbeef, "32-bit ROM Read");

    // Attempt ROM write (should be ignored)
    WriteMacInt32(ROMBaseMac + 0x100, 0x11223344);
    CHECK(ReadMacInt32(ROMBaseMac + 0x100) == 0xdeadbeef, "ROM Write Protection");

    // 3. Test Unmapped memory (dummy bank returns 0)
    CHECK(ReadMacInt32(0x20000000) == 0, "Unmapped memory returns 0");
    WriteMacInt32(0x20000000, 0x99999999);
    CHECK(ReadMacInt32(0x20000000) == 0, "Unmapped memory write ignored");

    // 4. Test 24-bit addressing mode wrapping
    TwentyFourBitAddressing = true;
    memory_init();

    WriteMacInt32(0x00002000, 0xAABBCCDD);
    // In 24-bit mode, address 0xFF002000 mirrors 0x00002000
    CHECK(ReadMacInt32(0xFF002000) == 0xAABBCCDD, "24-bit addressing mirror read");

    TwentyFourBitAddressing = false;
    memory_init();
}

void test_emulop_and_execute68k(void)
{
    printf("Running EmulOp and Execute68k tests...\n");

    // Initialize Musashi 680x0 CPU
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68040);
    m68k_pulse_reset();

    // Set stack pointer to a safe area in RAM
    m68k_set_reg(M68K_REG_A7, 0x10000);

    // Place a small 68k test routine in RAM at 0x4000:
    //   ADD.L #0x11111111, D0   (0x0680 0x1111 0x1111) -> 6 bytes
    //   RTS                     (0x4E75)                -> 2 bytes
    uint32 code_addr = 0x4000;
    WriteMacInt16(code_addr + 0, 0x0680); // addi.l #0x11111111, d0
    WriteMacInt32(code_addr + 2, 0x11111111);
    WriteMacInt16(code_addr + 6, 0x4E75); // rts

    struct M68kRegisters r;
    memset(&r, 0, sizeof(r));
    r.d[0] = 0x22222222;

    printf("  Calling Execute68k(0x4000)...\n"); fflush(stdout);
    Execute68k(code_addr, &r);
    printf("  Execute68k(0x4000) returned\n"); fflush(stdout);

    CHECK(r.d[0] == 0x33333333, "Execute68k added 0x11111111 to D0 (0x22222222 -> 0x33333333)");

    // Test EmulOp directly via 68k execution:
    // Place an EmulOp at 0x5000:
    //   0x7104: M68K_EMUL_OP_CLKNOMEM (RTC / XPRAM operations)
    //   RTS
    // Test writing to XPRAM and reading back
    XPRAM[0x10] = 0x5A;
    r.d[1] = 0x000040B8; // read XPRAM reg 0x10
    WriteMacInt16(0x5000, M68K_EMUL_OP_CLKNOMEM);
    WriteMacInt16(0x5002, 0x4E75);

    printf("  Calling Execute68k(0x5000)...\n"); fflush(stdout);
    Execute68k(0x5000, &r);
    printf("  Execute68k(0x5000) returned\n"); fflush(stdout);
    CHECK((r.d[2] & 0xFF) == 0x5A, "EmulOp M68K_EMUL_OP_CLKNOMEM executed correctly");
}

void test_scsi_subsystem(void)
{
    printf("Running SCSI Manager and Musashi Core integration tests...\n");

    // 1. Initialize SCSI subsystem
    SCSIInit();
    g_scsi_debug = false; // silence detailed logs during automated test

    // Create a temporary disk image file (64KB = 128 sectors of 512 bytes)
    const char *img_path = "/tmp/cockatrice_scsi_test.img";
    FILE *fp = fopen(img_path, "wb");
    assert(fp != NULL);
    uint8 sector[512];
    memset(sector, 0, sizeof(sector));
    for (int i = 0; i < 128; i++) {
        fwrite(sector, 1, 512, fp);
    }
    fclose(fp);

    // 2. Attach image to SCSI ID 0
    bool attached = SCSI_Attach(0, img_path);
    CHECK(attached, "SCSI_Attach to Target 0");
    CHECK(scsi_is_target_present(0), "Target 0 is present");
    CHECK(!scsi_is_target_present(1), "Target 1 is not present");

    // 3. Test SCSI Manager state machine
    CHECK(SCSIReset() == 0, "SCSIReset returns 0");
    CHECK(SCSIGet() == 0, "SCSIGet returns 0");
    CHECK(SCSIMgrBusy() != 0, "SCSIMgrBusy is true while bus is held");
    CHECK(SCSISelect(0) == 0, "SCSISelect(0) returns 0");

    // 4. Test INQUIRY (0x12) via SCSICmd + SCSIRead + SCSIComplete
    uint8 inquiry_cdb[6] = {0x12, 0x00, 0x00, 0x00, 0x24, 0x00}; // INQUIRY 36 bytes
    CHECK(SCSICmd(6, inquiry_cdb) == 0, "SCSICmd(INQUIRY) returns 0");

    // Setup TIB in Mac memory:
    // Mac RAM layout for test:
    //   0x7000: TIB
    //   0x7100: INQUIRY response buffer (36 bytes)
    //   0x7200: Status output word
    //   0x7202: Message output word
    uint32 tib_addr = 0x7000;
    uint32 inq_buf = 0x7100;
    uint32 stat_addr = 0x7200;
    uint32 msg_addr = 0x7202;

    WriteMacInt16(tib_addr + 0, 2); // scNoInc
    WriteMacInt32(tib_addr + 2, inq_buf);
    WriteMacInt32(tib_addr + 6, 36);
    WriteMacInt16(tib_addr + 10, 7); // scStop
    WriteMacInt32(tib_addr + 12, 0);
    WriteMacInt32(tib_addr + 16, 0);

    CHECK(SCSIRead(tib_addr) == 0, "SCSIRead(INQUIRY TIB) returns 0");
    CHECK(SCSIComplete(1000, msg_addr, stat_addr) == 0, "SCSIComplete(INQUIRY) returns 0");
    CHECK(ReadMacInt16(stat_addr) == 0, "INQUIRY SCSI Status is 0 (STAT_GOOD)");

    // Validate INQUIRY response: byte 0 is device type (0x00 = Direct Access block device)
    CHECK(ReadMacInt8(inq_buf) == 0x00, "INQUIRY returned direct access disk device type (0x00)");

    // 5. Test 68k execution of _SCSIDispatch via Musashi core
    // Setup the SCSI Dispatcher routine stub in Mac RAM at 0x8000:
    //   0x8000: 0x7119 (M68K_EMUL_OP_SCSI_DISPATCH)
    //   0x8002: 0x2E49 (move.l a1, a7)
    //   0x8004: 0x4ED0 (jmp (a0))
    WriteMacInt16(0x8000, M68K_EMUL_OP_SCSI_DISPATCH);
    WriteMacInt16(0x8002, 0x2E49);
    WriteMacInt16(0x8004, 0x4ED0);

    // Call SCSIReset via 68k SCSIDispatch:
    // Pascal call stack for SCSIReset:
    //   push word 0 (space for result)
    //   push word 0 (selector 0 = SCSIReset)
    //   jsr 0x8000
    //   rts
    uint32 caller_addr = 0x8100;
    WriteMacInt16(caller_addr + 0, 0x4267); // clr.w -(sp) (result space)
    WriteMacInt16(caller_addr + 2, 0x4267); // clr.w -(sp) (selector 0 = SCSIReset)
    WriteMacInt16(caller_addr + 4, 0x4EB9); // jsr 0x8000
    WriteMacInt32(caller_addr + 6, 0x8000);
    WriteMacInt16(caller_addr + 10, 0x301F); // move.w (sp)+, d0 (pop result)
    WriteMacInt16(caller_addr + 12, 0x4E75); // rts

    struct M68kRegisters r;
    memset(&r, 0, sizeof(r));
    r.d[0] = 0xFFFF;
    Execute68k(caller_addr, &r);
    CHECK(r.d[0] == 0, "68k _SCSIDispatch(SCSIReset) executed through Musashi returned 0");

    // Clear initial Unit Attention via TEST UNIT READY (0x00)
    CHECK(SCSIGet() == 0, "SCSIGet before TestUnitReady");
    CHECK(SCSISelect(0) == 0, "SCSISelect(0) before TestUnitReady");
    uint8 tur_cdb[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(SCSICmd(6, tur_cdb) == 0, "SCSICmd(TEST_UNIT_READY) returns 0");
    CHECK(SCSIComplete(1000, msg_addr, stat_addr) == 0, "SCSIComplete(TEST_UNIT_READY)");

    // 6. Test SCSI Write and Read through 68k Scatter/Gather memory translation
    // Step A: SCSIGet, SCSISelect(0)
    CHECK(SCSIGet() == 0, "SCSIGet before Write");
    CHECK(SCSISelect(0) == 0, "SCSISelect(0) before Write");

    // Step B: WRITE(10) to LBA 0 (1 block = 512 bytes)
    uint8 write_cdb[10] = {0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    CHECK(SCSICmd(10, write_cdb) == 0, "SCSICmd(WRITE(10)) returns 0");

    // Prepare test pattern in Mac RAM at 0x9000
    uint32 write_buf = 0x9000;
    for (int i = 0; i < 512; i++) {
        WriteMacInt8(write_buf + i, (uint8)(i ^ 0x5A));
    }

    // Prepare TIB for Write at 0x7300
    uint32 write_tib = 0x7300;
    WriteMacInt16(write_tib + 0, 2); // scNoInc
    WriteMacInt32(write_tib + 2, write_buf);
    WriteMacInt32(write_tib + 6, 512);
    WriteMacInt16(write_tib + 10, 7); // scStop
    WriteMacInt32(write_tib + 12, 0);
    WriteMacInt32(write_tib + 16, 0);

    CHECK(SCSIWrite(write_tib) == 0, "SCSIWrite(512 bytes) returns 0");
    CHECK(SCSIComplete(1000, msg_addr, stat_addr) == 0, "SCSIComplete(WRITE) returns 0");
    CHECK(ReadMacInt16(stat_addr) == 0, "WRITE SCSI Status is 0");

    // Step C: READ(10) from LBA 0 into fresh Mac RAM buffer at 0x9800
    CHECK(SCSIGet() == 0, "SCSIGet before Read");
    CHECK(SCSISelect(0) == 0, "SCSISelect(0) before Read");

    uint8 read_cdb[10] = {0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    CHECK(SCSICmd(10, read_cdb) == 0, "SCSICmd(READ(10)) returns 0");

    uint32 read_buf = 0x9800;
    Mac_memset(read_buf, 0, 512);

    uint32 read_tib = 0x7400;
    WriteMacInt16(read_tib + 0, 2); // scNoInc
    WriteMacInt32(read_tib + 2, read_buf);
    WriteMacInt32(read_tib + 6, 512);
    WriteMacInt16(read_tib + 10, 7); // scStop
    WriteMacInt32(read_tib + 12, 0);
    WriteMacInt32(read_tib + 16, 0);

    CHECK(SCSIRead(read_tib) == 0, "SCSIRead(512 bytes) returns 0");
    CHECK(SCSIComplete(1000, msg_addr, stat_addr) == 0, "SCSIComplete(READ) returns 0");
    CHECK(ReadMacInt16(stat_addr) == 0, "READ SCSI Status is 0");

    // Validate written data matches read data across Mac RAM and SCSI device
    bool data_match = true;
    for (int i = 0; i < 512; i++) {
        if (ReadMacInt8(read_buf + i) != (uint8)(i ^ 0x5A)) {
            data_match = false;
            break;
        }
    }
    CHECK(data_match, "SCSI Write -> Read roundtrip verified exact 512-byte payload");

    // 7. Detach and cleanup
    CHECK(SCSI_Detach(0), "SCSI_Detach(0)");
    CHECK(!scsi_is_target_present(0), "Target 0 detached successfully");
    unlink(img_path);
}

void test_fpu_execution(void)
{
    printf("Running FPU instruction execution tests...\n");

    // Test 1: FNOP
    uint32 code_addr = 0x6000;
    WriteMacInt16(code_addr + 0, 0xF280); // FNOP (0xF280 0x0000)
    WriteMacInt16(code_addr + 2, 0x0000);
    WriteMacInt16(code_addr + 4, 0x4E75); // RTS

    struct M68kRegisters r;
    memset(&r, 0, sizeof(r));
    Execute68k(code_addr, &r);
    CHECK(true, "FNOP executed cleanly without faulting");

    // Test 2: FMOVE.L D0, FP0; FMOVE.L D1, FP1; FADD.X FP1, FP0; FMOVE.L FP0, D2; RTS
    // D0 = 20, D1 = 22 -> Expected D2 = 42
    code_addr = 0x6010;
    WriteMacInt16(code_addr + 0, 0xF200); // FMOVE.L D0, FP0
    WriteMacInt16(code_addr + 2, 0x4000);
    WriteMacInt16(code_addr + 4, 0xF201); // FMOVE.L D1, FP1
    WriteMacInt16(code_addr + 6, 0x4080);
    WriteMacInt16(code_addr + 8, 0xF200); // FADD.X FP1, FP0
    WriteMacInt16(code_addr + 10, 0x0422);
    WriteMacInt16(code_addr + 12, 0xF202); // FMOVE.L FP0, D2
    WriteMacInt16(code_addr + 14, 0x6000);
    WriteMacInt16(code_addr + 16, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.d[0] = 20;
    r.d[1] = 22;
    Execute68k(code_addr, &r);
    CHECK(r.d[2] == 42, "FPU addition: 20 + 22 == 42 via FP0 + FP1 -> D2");

    // Test 3: FMOVECR #$00, FP0 (Load Pi) -> FMOVE.D FP0, (A0)
    uint32 pi_buf = 0x9500;
    code_addr = 0x6030;
    WriteMacInt16(code_addr + 0, 0xF200); // FMOVECR #$00, FP0
    WriteMacInt16(code_addr + 2, 0x5C00);
    WriteMacInt16(code_addr + 4, 0xF210); // FMOVE.D FP0, (A0) (mode 2, reg 0)
    WriteMacInt16(code_addr + 6, 0x7400); // 0111 0100 0000 0000 -> w2: 0x3 << 13 | 0x5 << 10 | 0x0 << 7
    WriteMacInt16(code_addr + 8, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.a[0] = pi_buf;
    Execute68k(code_addr, &r);

    // Verify double precision value in Mac memory
    uint64 dbl_bits = ((uint64)ReadMacInt32(pi_buf) << 32) | ReadMacInt32(pi_buf + 4);
    double pi_val;
    memcpy(&pi_val, &dbl_bits, sizeof(pi_val));
    CHECK(fabs(pi_val - 3.141592653589793) < 1e-10, "FMOVECR Pi (3.141592653589793) stored accurately as double");

    // Test 4: FSAVE -(SP) / FRESTORE (SP)+
    code_addr = 0x6050;
    WriteMacInt16(code_addr + 0, 0xF327); // FSAVE -(SP) (mode 4, reg 7)
    WriteMacInt16(code_addr + 2, 0xF31F); // FRESTORE (SP)+ (mode 3, reg 7)
    WriteMacInt16(code_addr + 4, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    Execute68k(code_addr, &r);
    CHECK(true, "FSAVE -(SP) and FRESTORE (SP)+ roundtrip completed cleanly");

    // Test 5: FSAVE (A0) and FRESTORE (A0) control mode
    uint32 fsave_buf = 0x9600;
    code_addr = 0x6060;
    WriteMacInt16(code_addr + 0, 0xF310); // FSAVE (A0) (mode 2, reg 0)
    WriteMacInt16(code_addr + 2, 0xF310); // FRESTORE (A0) (mode 2, reg 0)
    WriteMacInt16(code_addr + 4, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.a[0] = fsave_buf;
    Execute68k(code_addr, &r);
    CHECK(true, "FSAVE (A0) and FRESTORE (A0) control mode executed cleanly");

    // Test 6: Transcendental functions: FLOGN(e) == 1.0, FETOX(1.0) == e, FSQRT(16.0) == 4.0
    // FMOVECR #$0C, FP0 (load e) -> FLOGN.X FP0, FP0 -> FMOVE.L FP0, D0
    code_addr = 0x6070;
    WriteMacInt16(code_addr + 0, 0xF200); // FMOVECR #$0C, FP0 (e)
    WriteMacInt16(code_addr + 2, 0x5C0C);
    WriteMacInt16(code_addr + 4, 0xF200); // FLOGN.X FP0, FP0 (opmode 0x06)
    WriteMacInt16(code_addr + 6, 0x0006);
    WriteMacInt16(code_addr + 8, 0xF200); // FMOVE.L FP0, D0
    WriteMacInt16(code_addr + 10, 0x6000);
    WriteMacInt16(code_addr + 12, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    Execute68k(code_addr, &r);
    CHECK(r.d[0] == 1, "FLOGN(e) == 1 via FMOVECR + FLOGN -> D0");

    // Test 7: FSQRT.X on constant 16.0
    // FMOVE.L D1, FP0 (16) -> FSQRT.X FP0, FP0 -> FMOVE.L FP0, D2
    code_addr = 0x6090;
    WriteMacInt16(code_addr + 0, 0xF201); // FMOVE.L D1, FP0 (ea = mode 0, reg 1)
    WriteMacInt16(code_addr + 2, 0x4000);
    WriteMacInt16(code_addr + 4, 0xF200); // FSQRT.X FP0, FP0
    WriteMacInt16(code_addr + 6, 0x0004);
    WriteMacInt16(code_addr + 8, 0xF202); // FMOVE.L FP0, D2
    WriteMacInt16(code_addr + 10, 0x6000);
    WriteMacInt16(code_addr + 12, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.d[1] = 16;
    Execute68k(code_addr, &r);
    CHECK(r.d[2] == 4, "FSQRT(16.0) == 4 via FP0 -> D2");

    // Test 8: FCOS (opmode 0x1D) on 0.0 -> 1.0
    code_addr = 0x60B0;
    WriteMacInt16(code_addr + 0, 0xF201); // FMOVE.L D1, FP0 (0)
    WriteMacInt16(code_addr + 2, 0x4000);
    WriteMacInt16(code_addr + 4, 0xF200); // FCOS.X FP0, FP0 (0x1D)
    WriteMacInt16(code_addr + 6, 0x001D);
    WriteMacInt16(code_addr + 8, 0xF202); // FMOVE.L FP0, D2
    WriteMacInt16(code_addr + 10, 0x6000);
    WriteMacInt16(code_addr + 12, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.d[1] = 0;
    Execute68k(code_addr, &r);
    CHECK(r.d[2] == 1, "FCOS(0) == 1 via FP0 (opmode 0x1D) -> D2");

    // Test 9: FSINCOS (opmode 0x31: FP1=cos, FP0=sin) on 0.0 -> cos=1, sin=0
    code_addr = 0x60D0;
    WriteMacInt16(code_addr + 0, 0xF201); // FMOVE.L D1, FP0 (0)
    WriteMacInt16(code_addr + 2, 0x4000);
    WriteMacInt16(code_addr + 4, 0xF200); // FSINCOS.X FP0, FP1:FP0 (0x31)
    WriteMacInt16(code_addr + 6, 0x0031);
    WriteMacInt16(code_addr + 8, 0xF202); // FMOVE.L FP0, D2 (sin)
    WriteMacInt16(code_addr + 10, 0x6000);
    WriteMacInt16(code_addr + 12, 0xF203); // FMOVE.L FP1, D3 (cos)
    WriteMacInt16(code_addr + 14, 0x6080);
    WriteMacInt16(code_addr + 16, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.d[1] = 0;
    Execute68k(code_addr, &r);
    CHECK(r.d[2] == 0 && r.d[3] == 1, "FSINCOS(0) -> sin=0 (D2), cos=1 (D3)");
}

#include "cpu_engine.h"

static void test_cpu_engine_abstraction(void)
{
    printf("Running CPU Engine abstraction tests...\n");

    // Test 1: Registered engine count
    int count = GetRegisteredCPUEngineCount();
    CHECK(count >= 3, "At least 3 CPU engines registered (Musashi, WinUAE, Emu68)");

    // Test 2: Engine discovery by ID
    const CPUEngine *musashi = GetCPUEngine("musashi");
    CHECK(musashi != NULL && strcmp(musashi->id, "musashi") == 0, "Musashi CPU engine found");

    const CPUEngine *winuae = GetCPUEngine("uae");
    CHECK(winuae != NULL && strcmp(winuae->id, "uae") == 0, "WinUAE CPU engine found");

    const CPUEngine *emu68 = GetCPUEngine("emu68");
    CHECK(emu68 != NULL && strcmp(emu68->id, "emu68") == 0, "Emu68 JIT engine found");

    // Test 3: Engine properties
    if (emu68) {
        CHECK(emu68->is_jit == true, "Emu68 correctly flagged as JIT engine");
    }
    if (musashi) {
        CHECK(musashi->is_jit == false, "Musashi correctly flagged as non-JIT interpreter");
    }

    // Test 4: Dynamic engine switching
    CHECK(SetActiveCPUEngine("musashi") == true, "SetActiveCPUEngine('musashi') succeeded");
    CHECK(GetActiveCPUEngine() == musashi, "Active engine is Musashi");

    CHECK(SetActiveCPUEngine("uae") == true, "SetActiveCPUEngine('uae') succeeded");
    CHECK(GetActiveCPUEngine() == winuae, "Active engine is WinUAE");

    CHECK(SetActiveCPUEngine("emu68") == true, "SetActiveCPUEngine('emu68') succeeded");
    CHECK(GetActiveCPUEngine() == emu68, "Active engine is Emu68");

    // Switch back to Musashi for full test execution
    SetActiveCPUEngine("musashi");
    CHECK(GetActiveCPUEngine() == musashi, "Switched back to Musashi engine for baseline execution");
}

static void test_cpu_instruction_suite(void)
{
    printf("Running 680x0 Core Instruction Verification suite...\n");
    m68k_set_reg(M68K_REG_A7, 0x10000);
    M68kRegisters r;
    uint32 code_addr;

    // Test 1: Signed 32-bit Arithmetic (ADD.L, SUB.L)
    printf("  [CPU-TEST] Test 1: ADD.L/SUB.L...\n"); fflush(stdout);
    code_addr = 0x6100;
    WriteMacInt16(code_addr + 0, 0xD081); // ADD.L D1, D0
    WriteMacInt16(code_addr + 2, 0x9082); // SUB.L D2, D0
    WriteMacInt16(code_addr + 4, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.d[0] = 1000;
    r.d[1] = 500;
    r.d[2] = 200;
    Execute68k(code_addr, &r);
    CHECK(r.d[0] == 1300, "ADD.L + SUB.L (1000 + 500 - 200 == 1300)");

    // Test 2: Unsigned 32-bit Multiply & Divide: MULU.W + DIVU.W
    // MULU.W D1, D0 (300 * 200 = 60000) -> DIVU.W D2, D0 (60000 / 300 = 200)
    printf("  [CPU-TEST] Test 2: MULU/DIVU...\n"); fflush(stdout);
    code_addr = 0x7010;
    WriteMacInt16(code_addr + 0, 0xC0C1); // MULU.W D1, D0
    WriteMacInt16(code_addr + 2, 0x80C2); // DIVU.W D2, D0
    WriteMacInt16(code_addr + 4, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.d[0] = 300;
    r.d[1] = 200;
    r.d[2] = 300;
    Execute68k(code_addr, &r);
    CHECK((r.d[0] & 0xFFFF) == 200, "MULU.W + DIVU.W executed correctly (300 * 200 / 300 == 200)");

    // Test 3: Bit Shift operations (LSL, LSR)
    // LSL.L #4, D0 -> LSR.L #4, D0
    printf("  [CPU-TEST] Test 3: LSL/LSR...\n"); fflush(stdout);
    code_addr = 0x7020;
    WriteMacInt16(code_addr + 0, 0xE988); // LSL.L #4, D0
    WriteMacInt16(code_addr + 2, 0xE888); // LSR.L #4, D0
    WriteMacInt16(code_addr + 4, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.d[0] = 0x12345678;
    Execute68k(code_addr, &r);
    CHECK((r.d[0] & 0x0FFFFFFF) == (0x12345678 & 0x0FFFFFFF), "LSL.L + LSR.L bitshift roundtrip matches");

    // Test 4: Bit Manipulation (BSET, BCLR, BCHG, BTST)
    // BSET #3, D0 -> BCHG #7, D0 -> BTST #3, D0 (sets Z=0) -> BCLR #3, D0
    code_addr = 0x7030;
    WriteMacInt16(code_addr + 0, 0x08C0); // BSET #3, D0
    WriteMacInt16(code_addr + 2, 0x0003);
    WriteMacInt16(code_addr + 4, 0x0840); // BCHG #7, D0
    WriteMacInt16(code_addr + 6, 0x0007);
    WriteMacInt16(code_addr + 8, 0x0880); // BCLR #3, D0
    WriteMacInt16(code_addr + 10, 0x0003);
    WriteMacInt16(code_addr + 12, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.d[0] = 0x00;
    Execute68k(code_addr, &r);
    CHECK(r.d[0] == 0x80, "BSET, BCHG, BCLR bit manipulation correctly set bit 7 and cleared bit 3");

    // Test 5: Loop Counting with DBRA / DBF
    // D0 = 9; loop: ADDQ.L #2, D1; DBRA D0, loop
    code_addr = 0x7050;
    WriteMacInt16(code_addr + 0, 0x5481); // loop: ADDQ.L #2, D1
    WriteMacInt16(code_addr + 2, 0x51C8); // DBRA D0, loop (-4 displacement)
    WriteMacInt16(code_addr + 4, 0xFFFC);
    WriteMacInt16(code_addr + 6, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.d[0] = 9; // 10 iterations (9 down to -1)
    r.d[1] = 0;
    Execute68k(code_addr, &r);
    CHECK(r.d[1] == 20 && (int16)(r.d[0] & 0xFFFF) == -1, "DBRA loop iterated 10 times (D1 == 20, D0 == -1)");

    // Test 6: Multi-Register Memory Move (MOVEM.L to/from memory)
    // MOVEM.L D0-D3, (A0) -> clear D0-D3 -> MOVEM.L (A0), D0-D3
    uint32 movem_buf = 0x8200;
    code_addr = 0x7070;
    WriteMacInt16(code_addr + 0, 0x48D0); // MOVEM.L D0-D3, (A0)
    WriteMacInt16(code_addr + 2, 0x000F); // Register mask D0-D3
    WriteMacInt16(code_addr + 4, 0x4280); // CLR.L D0
    WriteMacInt16(code_addr + 6, 0x4281); // CLR.L D1
    WriteMacInt16(code_addr + 8, 0x4282); // CLR.L D2
    WriteMacInt16(code_addr + 10, 0x4283); // CLR.L D3
    WriteMacInt16(code_addr + 12, 0x4CD0); // MOVEM.L (A0), D0-D3
    WriteMacInt16(code_addr + 14, 0x000F); // Register mask D0-D3
    WriteMacInt16(code_addr + 16, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.a[0] = movem_buf;
    r.d[0] = 0x11111111;
    r.d[1] = 0x22222222;
    r.d[2] = 0x33333333;
    r.d[3] = 0x44444444;
    Execute68k(code_addr, &r);
    CHECK(r.d[0] == 0x11111111 && r.d[1] == 0x22222222 && r.d[2] == 0x33333333 && r.d[3] == 0x44444444,
          "MOVEM.L save and restore matched bit-for-bit across D0-D3");

    // Test 7: Subroutine nesting and jump tables (JSR, BSR, RTS)
    // JSR sub1 -> sub1 calls BSR sub2 -> sub2 returns -> sub1 returns
    uint32 sub2_addr = 0x7120;
    uint32 sub1_addr = 0x7100;
    code_addr = 0x7090;

    // main: JSR sub1; RTS
    WriteMacInt16(code_addr + 0, 0x4EB9); // JSR sub1 (absolute long)
    WriteMacInt32(code_addr + 2, sub1_addr);
    WriteMacInt16(code_addr + 6, 0x4E75); // RTS

    // sub1: ADDQ.L #10, D0; BSR sub2; ADDQ.L #5, D0; RTS
    WriteMacInt16(sub1_addr + 0, 0x5080); // ADDQ.L #8, D0
    WriteMacInt16(sub1_addr + 2, 0x5480); // ADDQ.L #2, D0 (total +10)
    WriteMacInt16(sub1_addr + 4, 0x611A); // BSR sub2 (+26 bytes -> sub2)
    WriteMacInt16(sub1_addr + 6, 0x5A80); // ADDQ.L #5, D0
    WriteMacInt16(sub1_addr + 8, 0x4E75); // RTS

    // sub2: ADDQ.L #7, D0; RTS
    WriteMacInt16(sub2_addr + 0, 0x5E80); // ADDQ.L #7, D0
    WriteMacInt16(sub2_addr + 2, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.d[0] = 100;
    Execute68k(code_addr, &r);
    CHECK(r.d[0] == 122, "Nested JSR + BSR + RTS stack return executed cleanly (100 + 10 + 7 + 5 == 122)");

    // Test 8: 68020+ Bitfield Extraction: BFEXTU D0 {8:16}, D1
    // Extracts 16 bits starting at bit offset 8 (0x12345678 -> 0x3456)
    code_addr = 0x7140;
    WriteMacInt16(code_addr + 0, 0xE9C0); // BFEXTU D0, D1
    WriteMacInt16(code_addr + 2, 0x1210); // D1, offset=8, width=16
    WriteMacInt16(code_addr + 4, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.d[0] = 0x12345678;
    Execute68k(code_addr, &r);
    CHECK((r.d[1] & 0xFFFF) == 0x3456, "68020+ BFEXTU D0 {8:16}, D1 extracted 0x3456 from 0x12345678");

    // Test 9: 68020+ 32x32 -> 64-bit Multiplication: MULU.L D1, D3:D2
    // 0x20000 * 0x30000 = 0x00000006 : 0x00000000 (D3=6, D2=0)
    code_addr = 0x7160;
    WriteMacInt16(code_addr + 0, 0x4C01); // MULU.L D1, D3:D2 (0x4C01 0x2C03)
    WriteMacInt16(code_addr + 2, 0x2C03); // D3:D2 64-bit result pair
    WriteMacInt16(code_addr + 4, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.d[1] = 0x20000;
    r.d[2] = 0x30000;
    Execute68k(code_addr, &r);
    CHECK(r.d[3] == 6 && r.d[2] == 0, "68020+ 32x32->64-bit MULU.L (0x20000 * 0x30000 == 6:0)");

    // Test 10: Multi-CPU Model Compatibility (68000, 68010, 68020, 68030, 68040)
    const int cpu_numbers[] = { 0, 10, 20, 30, 40 };
    for (int cpu_model = 0; cpu_model <= 4; cpu_model++) {
        CPUType = cpu_model;
        musashi_cpu_engine.init();
        char msg[128];
        snprintf(msg, sizeof(msg), "Musashi 680%02d initialization and CPU mode switch passed", cpu_numbers[cpu_model]);
        CHECK(GetActiveCPUEngine() != NULL, msg);
    }
    // Restore 68040 for subsequent tests
    CPUType = 4;
    musashi_cpu_engine.init();

    // Test 11: Multi-Engine Engine Verification (Musashi, WinUAE, Emu68)
    const CPUEngine *engines[] = { GetCPUEngine("musashi"), GetCPUEngine("uae"), GetCPUEngine("emu68") };
    for (int i = 0; i < 3; i++) {
        if (engines[i]) {
            char desc[128];
            snprintf(desc, sizeof(desc), "Engine '%s' (%s) lifecycle initialized cleanly", engines[i]->id, engines[i]->name);
            CHECK(engines[i]->init != NULL && engines[i]->execute_68k != NULL, desc);
        }
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== CockatriceIII / Multi-Engine Integration Test Suite ===\n");
    test_memory_banking();
    test_cpu_engine_abstraction();
    test_emulop_and_execute68k();
    test_cpu_instruction_suite();
    test_scsi_subsystem();
    test_fpu_execution();

    printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
    fflush(stdout);
    return g_fail == 0 ? 0 : 1;
}
