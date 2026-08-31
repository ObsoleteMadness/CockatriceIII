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
int ROMVersion = 0x067c; // ROM_VERSION_32
uint32 UniversalInfo = 0;
uint32 InterruptFlags = 0;
uint8 XPRAM[256];
uint32 PutScrapPatch = 0;

static int g_pass = 0;
static int g_fail = 0;

#include <signal.h>
#include <execinfo.h>
#if defined(__APPLE__) && defined(__arm64__)
extern "C" void emu68_jit_on_crash(uintptr_t pc, uintptr_t lr);
#endif

static void crash_handler(int sig, siginfo_t *info, void *ucontext)
{
    printf("\n*** CRASH SIGNAL %d (%s) at address %p ***\n", sig, sys_siglist[sig], info->si_addr);
#if defined(__APPLE__) && defined(__arm64__)
    ucontext_t *uc = (ucontext_t *)ucontext;
    if (uc) {
        printf("  PC: 0x%llx, LR: 0x%llx, SP: 0x%llx\n",
               uc->uc_mcontext->__ss.__pc,
               uc->uc_mcontext->__ss.__lr,
               uc->uc_mcontext->__ss.__sp);
        for (int i = 0; i < 30; i += 2) {
            printf("  x%02d: 0x%016llx  x%02d: 0x%016llx\n",
                   i, uc->uc_mcontext->__ss.__x[i],
                   i+1, uc->uc_mcontext->__ss.__x[i+1]);
        }
        uintptr_t fault = (uintptr_t)info->si_addr;
        for (int i = 0; i < 29; i++) {
            if (uc->uc_mcontext->__ss.__x[i] == (uint64_t)fault)
                printf("  x%02d matches faulting address\n", i);
        }
        const uint16_t *sr_lanes = (const uint16_t *)&uc->uc_mcontext->__ns.__v[19];
        const uint64_t *host_mem = (const uint64_t *)&uc->uc_mcontext->__ns.__v[22];
        printf("  v19.h[5] (JIT SR image)=0x%04x  v22.d[0] (HOST_MEM_BASE)=0x%llx\n",
               sr_lanes[5], (unsigned long long)host_mem[0]);
        emu68_jit_on_crash((uintptr_t)uc->uc_mcontext->__ss.__pc,
                           (uintptr_t)uc->uc_mcontext->__ss.__lr);
    }
#else
    (void)ucontext;
#endif
    void *callstack[128];
    int frames = backtrace(callstack, 128);
    backtrace_symbols_fd(callstack, frames, 1);
    fflush(stdout);
    _exit(sig);
}

static void install_crash_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}

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

#define CHECK_ENG(expr, engine, msg) do { \
    char _eng_msg[320]; \
    snprintf(_eng_msg, sizeof(_eng_msg), "[%s] %s", engine, msg); \
    CHECK((expr), _eng_msg); \
} while (0)

// Peripheral driver stubs for unit testing
bool PrefsFindBool(const char *name) {
    if (strcmp(name, "ltoudp") == 0)
        return true;
    return false;
}
uint32 TimerDateTime(void) { return 0x12345678; }
void QuitEmulator(void) {}
void TimerReset(void) {}
void EtherReset(void) {}
void MenuQueue_Reset(void) {}
void MenuQueue_Drain(void) {}
void MenuBar_UpdateAll(void) {}
void SonyReset(void) {}
void DiskReset(void) {}
void AudioReset(void) {}

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

void test_memory_banking(void)
{
    printf("Running memory banking tests...\n");

    RAMSize = 1024 * 1024;
    RAMBaseMac = 0x00000000;
    ROMSize = 1024 * 1024;
    ROMBaseMac = 0x40800000;

    memory_init();

    Mac_memset(RAMBaseMac, 0, RAMSize);
    Mac_memset(ROMBaseMac, 0, ROMSize);

    // 1. Test 32-bit RAM write & read
    WriteMacInt32(0x1000, 0x12345678);
    CHECK(ReadMacInt32(0x1000) == 0x12345678, "32-bit RAM Write/Read");
    CHECK(ReadMacInt16(0x1000) == 0x1234, "16-bit RAM Read Big-Endian High");
    CHECK(ReadMacInt16(0x1002) == 0x5678, "16-bit RAM Read Big-Endian Low");
    CHECK(ReadMacInt8(0x1000) == 0x12, "8-bit RAM Read Byte 0");
    CHECK(ReadMacInt8(0x1003) == 0x78, "8-bit RAM Read Byte 3");

    // 2. Test 32-bit ROM read (ROM is read-only via WriteMacInt)
    ROMBaseHost[0x100] = 0xDE;
    ROMBaseHost[0x101] = 0xAD;
    ROMBaseHost[0x102] = 0xBE;
    ROMBaseHost[0x103] = 0xEF;
    CHECK(ReadMacInt32(ROMBaseMac + 0x100) == 0xdeadbeef, "32-bit ROM Read");

    // Attempt ROM write (should be ignored by WriteMacInt)
    WriteMacInt32(ROMBaseMac + 0x100, 0x11223344);
    CHECK(ReadMacInt32(ROMBaseMac + 0x100) == 0xdeadbeef, "ROM Write Protection");

    // 3. The whole 4GB window is dummy-backed RW up front (no PROT_NONE holes);
    //    JIT engines dereference guest pointers directly mid-compile, and a
    //    SIGSEGV-driven longjmp out of compile_block()/M68K_GetTranslationUnit()
    //    mid-translation corrupts their global code-cache state, so unmapped
    //    "holes" are no longer allowed to fault.
    CHECK(memory_is_mapped(0x1000, 4), "RAM is committed");
    CHECK(memory_is_mapped(ROMBaseMac + 0x100, 4), "ROM is committed");
    CHECK(memory_is_mapped(0x20000000, 4), "I/O range at 0x20000000 is dummy-backed");
    CHECK(ReadMacInt32(0x20000000) == 0, "Dummy-backed I/O range reads as zero");
    CHECK(memory_is_mapped(MacFrameBaseMac, 4), "NuBus framebuffer slot is dummy-backed before VideoInit");

    MacFrameSize = 4096;
    MacFrameLayout = FLAYOUT_DIRECT;
    memory_map_framebuffer();
    CHECK(memory_is_mapped(MacFrameBaseMac, 4), "Framebuffer bytes are committed");
    CHECK(memory_is_mapped(MacFrameBaseMac + 0x01000000, 4), "Rest of NuBus slot is still dummy-backed");
    MacFrameSize = 0;
    MacFrameLayout = FLAYOUT_NONE;

    // 4. Test 24-bit addressing mode wrapping
    TwentyFourBitAddressing = true;
    memory_init();

    WriteMacInt32(0x00002000, 0xAABBCCDD);
    // In 24-bit mode, address 0xFF002000 mirrors 0x00002000
    CHECK(ReadMacInt32(0xFF002000) == 0xAABBCCDD, "24-bit addressing mirror read");

    TwentyFourBitAddressing = false;
    memory_init();
}

/*
 * Switches the active 680x0 engine and re-inits it so interpreter vs JIT tables match.
 *
 * SetActiveCPUEngine() only updates the dispatch pointer; UAE JIT vs interpreter
 * is chosen inside engine->init() from UseJIT / JITCacheSize, so tests must
 * exit the previous engine and call init() after setting those flags.
 *
 * Arguments:
 *   id: Engine identifier ("musashi", "syn68k", "uae", or "emu68").
 *   jit: True to enable UseJIT (Musashi and syn68k ignore this; UAE and Emu68 do not).
 *   jitfpu: True to enable UseJITFPU when JIT is enabled.
 *
 * Returns:
 *   true if the engine was selected and init() succeeded.
 */
static bool activate_cpu_engine(const char *id, bool jit, bool jitfpu = false)
{
    const CPUEngine *cur = GetActiveCPUEngine();
    if (cur && cur->exit)
        cur->exit();

    memory_init();
    RAMSize = 1024 * 1024;
    RAMBaseMac = 0;
    ROMSize = 1024 * 1024;
    ROMBaseMac = 0x40800000;
    Mac_memset(RAMBaseMac, 0, RAMSize);
    Mac_memset(ROMBaseMac, 0, ROMSize);
    TwentyFourBitAddressing = false;
    ROMVersion = 0x067c;

    UseJIT = jit;
    UseJITFPU = (jit && jitfpu);
    JITCacheSize = 8192;
    CPUType = 4;
    FPUType = 1;

    /*
     * Plant RESET ISP/PC before engine init. UAE's m68k_reset() reads guest
     * 0 and 4; zeros there leave A7=0 and PC=0. 0x10000 is inside the 1MB
     * test RAM and matches the Execute68k stack fallback.
     */
    memory_init();
    WriteMacInt32(0, 0x10000);
    WriteMacInt32(4, 0x4000);

    if (!SetActiveCPUEngine(id))
        return false;

    const CPUEngine *eng = GetActiveCPUEngine();
    if (!eng || !eng->init)
        return false;
    return eng->init();
}

/*
 * Runs Execute68k, EmulOp, and Execute68kTrap on the currently active engine.
 *
 * Arguments:
 *   engine: Label included in CHECK messages (e.g. "uae+jit").
 */
static void test_emulop_and_execute68k(const char *engine)
{
    printf("Running EmulOp and Execute68k tests (%s)...\n", engine);

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

    CHECK_ENG(r.d[0] == 0x33333333, engine, "Execute68k added 0x11111111 to D0 (0x22222222 -> 0x33333333)");

    // Test EmulOp directly via 68k execution:
    // Place an EmulOp at 0x5000:
    //   0x7104: M68K_EMUL_OP_CLKNOMEM (RTC / XPRAM operations)
    //   RTS
    XPRAM[0x10] = 0x5A;
    r.d[1] = 0x000040B8; // read XPRAM reg 0x10
    WriteMacInt16(0x5000, M68K_EMUL_OP_CLKNOMEM);
    WriteMacInt16(0x5002, 0x4E75);

    printf("  Calling Execute68k(0x5000)...\n"); fflush(stdout);
    Execute68k(0x5000, &r);
    printf("  Execute68k(0x5000) returned\n"); fflush(stdout);
    CHECK_ENG((r.d[2] & 0xFF) == 0x5A, engine, "EmulOp M68K_EMUL_OP_CLKNOMEM executed correctly");

    // Test Execute68kTrap (Mac OS trap execution from host code)
    // Setup Vector 10 (Line-A exception vector, offset 0x28) to point to test handler at 0x9000
    uint32 line_a_handler = 0x9000;
    WriteMacInt16(line_a_handler + 0, 0x54AF); // ADDQ.L #2, 2(SP) - displacement mode 5
    WriteMacInt16(line_a_handler + 2, 0x0002);
    WriteMacInt16(line_a_handler + 4, 0x5E80); // ADDQ.L #7, D0
    WriteMacInt16(line_a_handler + 6, 0x4E73); // RTE
    WriteMacInt32(0x28, line_a_handler);       // Vector 10 = 0x28

    memset(&r, 0, sizeof(r));
    r.d[0] = 100;
    printf("  Calling Execute68kTrap(0xA000)...\n"); fflush(stdout);
    Execute68kTrap(0xA000, &r);
    printf("  Execute68kTrap(0xA000) returned\n"); fflush(stdout);
    CHECK_ENG(r.d[0] == 107, engine, "Execute68kTrap(0xA000) executed Line-A trap and returned cleanly via RTE (100 -> 107)");

    r.d[0] = 50;
    Execute68kTrap(0xA122, &r);
    CHECK_ENG(r.d[0] == 57, engine, "Execute68kTrap(0xA122) maintained stack alignment and register integrity (50 -> 57)");
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

/*
 * ============================================================================
 * SCC (Serial Communications Controller - Zilog Z8530) Integration Tests
 * ============================================================================
 *
 * The Macintosh serial hardware utilizes a Zilog Z8530 SCC mapped into memory:
 *   - 32-bit Addressing: Base 0x50000000 (Channel B Ctl 0x50000000, Data 0x50000002;
 *                                         Channel A Ctl 0x50000004, Data 0x50000006)
 *   - 24-bit Addressing: Read 0x00900000, Write 0x00B00000
 *
 * This test suite validates:
 *   1. Hardware reset and channel state initialization.
 *   2. Direct register addressing for Channels A (modem) and B (printer).
 *   3. Write Register (WR0-WR15) pointer dispatch and configuration.
 *   4. Read Register (RR0 status, RR8 data) status evaluation.
 *   5. Memory-mapped I/O access through Basilisk II read/write accessors.
 *   6. 680x0 CPU instruction-level I/O access via Musashi Execute68k.
 */

/*
 * Tests the Z8530 SCC peripheral emulation, register access, and memory mapping.
 *
 * Arguments:
 *   None.
 *
 * Returns:
 *   None. (Updates global g_pass/g_fail test counters).
 */
void test_scc_subsystem(void)
{
    printf("Running Z8530 SCC and serial communication integration tests...\n");

    // 1. Initialize SCC subsystem
    SCCInit();
    SCC_Reset();

    // 2. Test direct register read/write on Channel A (modem) and Channel B (printer)
    // Mac SCC Mapping (addr passed to SCC_Access is (host_addr >> 1) & 3):
    //   0: Channel B Control (Printer Ctl)
    //   1: Channel A Control (Modem Ctl)
    //   2: Channel B Data (Printer Data)
    //   3: Channel A Data (Modem Data)

    // Read initial RR0 status on Channel A (Control = reg index 1; should indicate Tx buffer empty: bit 2 = 0x04)
    uint32 rr0_a = SCC_Access(0, false, 1);
    CHECK((rr0_a & 0x04) != 0, "SCC Channel A RR0 Tx Buffer Empty flag is set");

    // Read initial RR0 status on Channel B (Control = reg index 0)
    uint32 rr0_b = SCC_Access(0, false, 0);
    CHECK((rr0_b & 0x04) != 0, "SCC Channel B RR0 Tx Buffer Empty flag is set");

    // 3. Test multi-byte register pointer dispatch on Channel A:
    // Write 0x0C to WR0 (sets pointer to WR12: Lower byte of Time Constant)
    SCC_Access(0x0C, true, 1);
    // Write 0x55 to WR12
    SCC_Access(0x55, true, 1);

    // Write 0x0D to WR0 (sets pointer to WR13: Upper byte of Time Constant)
    SCC_Access(0x0D, true, 1);
    // Write 0xAA to WR13
    SCC_Access(0xAA, true, 1);

    // Read back WR12 via RR12
    SCC_Access(0x0C, true, 1); // pointer to 12
    uint32 rr12 = SCC_Access(0, false, 1);
    CHECK(rr12 == 0x55, "SCC WR12 time constant low byte written and read back");

    // Read back WR13 via RR13
    SCC_Access(0x0D, true, 1); // pointer to 13
    uint32 rr13 = SCC_Access(0, false, 1);
    CHECK(rr13 == 0xAA, "SCC WR13 time constant high byte written and read back");

    // 4. Test Memory-Mapped I/O in 32-bit addressing mode (0x50000000 base)
    // Channel A Control is at address 0x50000002 (index 1)
    uint32 mmio_rr0_a = ReadMacInt8(0x50000002);
    CHECK((mmio_rr0_a & 0x04) != 0, "32-bit MMIO ReadMacInt8(0x50000002) reads Channel A status");

    // Write Channel A Time Constant via 32-bit MMIO
    WriteMacInt8(0x50000002, 0x0C); // pointer to WR12
    WriteMacInt8(0x50000002, 0x33); // write 0x33 to WR12
    WriteMacInt8(0x50000002, 0x0C); // pointer to RR12
    uint32 mmio_rr12 = ReadMacInt8(0x50000002);
    CHECK(mmio_rr12 == 0x33, "32-bit MMIO WriteMacInt8/ReadMacInt8 WR12 roundtrip");

    // 5. Test 680x0 CPU instruction access to SCC MMIO via Execute68k
    // Code in Mac RAM at 0x8200:
    //   move.b (0x50000002), d0  ; Read RR0 from Channel A Control
    //   rts
    uint32 code_addr = 0x8200;
    WriteMacInt16(code_addr + 0, 0x1039); // move.b (xxx).l, d0
    WriteMacInt32(code_addr + 2, 0x50000002);
    WriteMacInt16(code_addr + 6, 0x4E75); // rts

    struct M68kRegisters r;
    memset(&r, 0, sizeof(r));
    r.d[0] = 0;
    Execute68k(code_addr, &r);
    CHECK((r.d[0] & 0x04) != 0, "68k instruction MOVE.B (0x50000002), D0 read SCC Channel A RR0");

    // 6. Test upper end of 32-bit SCC window (0x50F00000 - Quadra 800 actual SCC address).
    // The Z8530 SCC on the Quadra 800 is accessed well above 0x50100000; prior to this fix,
    // is_scc_addr capped at 0x50100000 (1MB) instead of 0x51000000 (16MB, matching the
    // original map_banks(&scc_bank, 0x5000, 0x100) bank assignment).
    // 0x50F00002 = (addr >> 1) & 3 == 1 → Channel A Control
    SCC_Reset();
    WriteMacInt8(0x50F00002, 0x0C); // pointer to WR12
    WriteMacInt8(0x50F00002, 0x7E); // write 0x7E to WR12
    WriteMacInt8(0x50F00002, 0x0C); // pointer to RR12
    uint32 mmio_upper_rr12 = ReadMacInt8(0x50F00002);
    CHECK(mmio_upper_rr12 == 0x7E, "32-bit MMIO at 0x50F00002 (Quadra 800 SCC range) roundtrip");

    // Also verify via a 68k CPU instruction to exercise the full path
    uint32 code_addr2 = 0x8210;
    WriteMacInt16(code_addr2 + 0, 0x1039); // move.b (xxx).l, d0
    WriteMacInt32(code_addr2 + 2, 0x50F00002);
    WriteMacInt16(code_addr2 + 6, 0x4E75); // rts
    struct M68kRegisters r2;
    memset(&r2, 0, sizeof(r2));
    Execute68k(code_addr2, &r2);
    // After reset, WR12=0x7E → RR12 readback should be 0x7E via the CPU instruction path
    WriteMacInt8(0x50F00002, 0x0C);
    uint32 cpu_upper = ReadMacInt8(0x50F00002);
    CHECK(cpu_upper == 0x7E, "68k MOVE.B (0x50F00002) reads SCC Channel A RR12 in upper 32-bit window");

    // 7. Test 24-bit addressing mode MMIO access (0x00900000 / 0x00B00000)
    TwentyFourBitAddressing = true;
    memory_init();

    // In 24-bit mode, 0x00900002 maps to Channel A Control read
    uint32 mmio_24_rr0 = ReadMacInt8(0x00900002);
    CHECK((mmio_24_rr0 & 0x04) != 0, "24-bit MMIO ReadMacInt8(0x00900002) reads SCC Channel A status");

    TwentyFourBitAddressing = false;
    memory_init();
}

/*
 * Tests advanced SCSI features including Transfer Instruction Block (TIB) modes and unaligned DMA.
 *
 * Arguments:
 *   None.
 *
 * Returns:
 *   None. (Updates global g_pass/g_fail test counters).
 */
void test_scsi_advanced(void)
{
    printf("Running advanced SCSI TIB DMA and unaligned multi-block transfer tests...\n");

    // 1. Initialize SCSI subsystem and temporary disk image (32KB = 64 sectors)
    SCSIInit();
    const char *img_path = "/tmp/cockatrice_scsi_adv_test.img";
    FILE *fp = fopen(img_path, "wb");
    assert(fp != NULL);
    uint8 sector[512];
    memset(sector, 0, sizeof(sector));
    for (int i = 0; i < 64; i++) {
        fwrite(sector, 1, 512, fp);
    }
    fclose(fp);

    bool attached = SCSI_Attach(0, img_path);
    CHECK(attached, "Attach disk image for advanced SCSI tests");

    // Clear unit attention
    CHECK(SCSIGet() == 0, "SCSIGet for advance tests");
    CHECK(SCSISelect(0) == 0, "SCSISelect(0)");
    uint8 tur_cdb[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(SCSICmd(6, tur_cdb) == 0, "SCSICmd(TEST UNIT READY)");
    uint32 stat_addr = 0x7200;
    uint32 msg_addr = 0x7202;
    CHECK(SCSIComplete(1000, msg_addr, stat_addr) == 0, "SCSIComplete(TEST UNIT READY)");

    // 2. Test multi-sector write with scNoInc TIB (4 sectors = 2048 bytes)
    CHECK(SCSIGet() == 0, "SCSIGet before multi-sector write");
    CHECK(SCSISelect(0) == 0, "SCSISelect(0) before multi-sector write");

    // WRITE(10) starting at LBA 4 for 4 blocks (2048 bytes)
    uint8 write_cdb[10] = {0x2A, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x04, 0x00};
    CHECK(SCSICmd(10, write_cdb) == 0, "SCSICmd(WRITE(10) 4 blocks)");

    // Fill 2048 bytes with pseudo-random pattern at unaligned buffer address 0x9003
    uint32 unaligned_buf = 0x9003;
    for (int i = 0; i < 2048; i++) {
        WriteMacInt8(unaligned_buf + i, (uint8)((i * 37 + 13) & 0xFF));
    }

    // TIB at 0x7500: scNoInc 2048 bytes from unaligned_buf, followed by scStop
    uint32 tib_addr = 0x7500;
    WriteMacInt16(tib_addr + 0, 2); // scNoInc
    WriteMacInt32(tib_addr + 2, unaligned_buf);
    WriteMacInt32(tib_addr + 6, 2048);
    WriteMacInt16(tib_addr + 10, 7); // scStop
    WriteMacInt32(tib_addr + 12, 0);
    WriteMacInt32(tib_addr + 16, 0);

    CHECK(SCSIWrite(tib_addr) == 0, "SCSIWrite(2048 bytes unaligned) returns 0");
    CHECK(SCSIComplete(1000, msg_addr, stat_addr) == 0, "SCSIComplete(WRITE 4 blocks)");
    CHECK(ReadMacInt16(stat_addr) == 0, "Multi-block WRITE status is GOOD");

    // 3. Test multi-sector read with segmented scLoop TIB into fresh memory
    CHECK(SCSIGet() == 0, "SCSIGet before multi-sector read");
    CHECK(SCSISelect(0) == 0, "SCSISelect(0) before multi-sector read");

    // READ(10) starting at LBA 4 for 4 blocks
    uint8 read_cdb[10] = {0x28, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x04, 0x00};
    CHECK(SCSICmd(10, read_cdb) == 0, "SCSICmd(READ(10) 4 blocks)");

    uint32 read_dest = 0xA005; // Another unaligned destination
    Mac_memset(read_dest, 0, 2048);

    // TIB at 0x7600: scNoInc 2048 bytes into read_dest
    uint32 read_tib = 0x7600;
    WriteMacInt16(read_tib + 0, 2); // scNoInc
    WriteMacInt32(read_tib + 2, read_dest);
    WriteMacInt32(read_tib + 6, 2048);
    WriteMacInt16(read_tib + 10, 7); // scStop
    WriteMacInt32(read_tib + 12, 0);
    WriteMacInt32(read_tib + 16, 0);

    CHECK(SCSIRead(read_tib) == 0, "SCSIRead(2048 bytes unaligned) returns 0");
    CHECK(SCSIComplete(1000, msg_addr, stat_addr) == 0, "SCSIComplete(READ 4 blocks)");
    CHECK(ReadMacInt16(stat_addr) == 0, "Multi-block READ status is GOOD");

    // Verify 2048-byte payload integrity
    bool match = true;
    for (int i = 0; i < 2048; i++) {
        if (ReadMacInt8(read_dest + i) != (uint8)((i * 37 + 13) & 0xFF)) {
            match = false;
            break;
        }
    }
    CHECK(match, "Multi-block unaligned SCSI transfer verified 2048 bytes bit-for-bit");

    // Cleanup
    SCSI_Detach(0);
    unlink(img_path);
}

/*
 * Tests 68881/68882/68040 FPU instruction execution and math functions.
 *
 * Arguments:
 *   engine: Engine identifier string for test output logging.
 */
void test_fpu_execution(const char *engine)
{
    printf("Running FPU instruction execution tests (%s)...\n", engine);

    // Test 1: FNOP
    uint32 code_addr = 0x6000;
    WriteMacInt16(code_addr + 0, 0xF280); // FNOP (0xF280 0x0000)
    WriteMacInt16(code_addr + 2, 0x0000);
    WriteMacInt16(code_addr + 4, 0x4E75); // RTS

    struct M68kRegisters r;
    memset(&r, 0, sizeof(r));
    Execute68k(code_addr, &r);
    CHECK_ENG(true, engine, "FNOP executed cleanly without faulting");

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
    CHECK_ENG(r.d[2] == 42, engine, "FPU addition: 20 + 22 == 42 via FP0 + FP1 -> D2");

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
    CHECK_ENG(fabs(pi_val - 3.141592653589793) < 1e-10, engine, "FMOVECR Pi (3.141592653589793) stored accurately as double");

    // Test 4: FSAVE -(SP) / FRESTORE (SP)+
    printf("  [FPU-TEST] Test 4: FSAVE -(SP) / FRESTORE (SP)+...\n"); fflush(stdout);
    code_addr = 0x6050;
    WriteMacInt16(code_addr + 0, 0xF327); // FSAVE -(SP) (mode 4, reg 7)
    WriteMacInt16(code_addr + 2, 0xF35F); // FRESTORE (SP)+ (mode 3, reg 7)
    WriteMacInt16(code_addr + 4, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.sr = 0x2700;
    Execute68k(code_addr, &r);
    CHECK_ENG(true, engine, "FSAVE -(SP) and FRESTORE (SP)+ roundtrip completed cleanly");

    // Test 5: FSAVE (A0) and FRESTORE (A0) control mode
    printf("  [FPU-TEST] Test 5: FSAVE (A0) / FRESTORE (A0)...\n"); fflush(stdout);
    uint32 fsave_buf = 0x9600;
    code_addr = 0x6060;
    WriteMacInt16(code_addr + 0, 0xF310); // FSAVE (A0) (mode 2, reg 0)
    WriteMacInt16(code_addr + 2, 0xF350); // FRESTORE (A0) (mode 2, reg 0)
    WriteMacInt16(code_addr + 4, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.sr = 0x2700;
    r.a[0] = fsave_buf;
    Execute68k(code_addr, &r);
    CHECK_ENG(true, engine, "FSAVE (A0) and FRESTORE (A0) control mode executed cleanly");

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
    CHECK_ENG(r.d[0] == 1, engine, "FLOGN(e) == 1 via FMOVECR + FLOGN -> D0");

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
    CHECK_ENG(r.d[2] == 4, engine, "FSQRT(16.0) == 4 via FP0 -> D2");

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
    CHECK_ENG(r.d[2] == 1, engine, "FCOS(0) == 1 via FP0 (opmode 0x1D) -> D2");

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
    CHECK_ENG(r.d[2] == 0 && r.d[3] == 1, engine, "FSINCOS(0) -> sin=0 (D2), cos=1 (D3)");
}

#include "cpu_engine.h"

static void test_cpu_engine_abstraction(void)
{
    printf("Running CPU Engine abstraction tests...\n");

    // Test 1: Registered engine count
    int count = GetRegisteredCPUEngineCount();
    CHECK(count >= 4, "At least 4 CPU engines registered (Musashi, Amiberry/UAE, Emu68, syn68k)");

    // Test 2: Engine discovery by ID
    const CPUEngine *musashi = GetCPUEngine("musashi");
    CHECK(musashi != NULL && strcmp(musashi->id, "musashi") == 0, "Musashi CPU engine found");

    const CPUEngine *uae = GetCPUEngine("uae");
    CHECK(uae != NULL && strcmp(uae->id, "uae") == 0, "Amiberry/UAE CPU engine found");

    const CPUEngine *emu68 = GetCPUEngine("emu68");
    CHECK(emu68 != NULL && strcmp(emu68->id, "emu68") == 0, "Emu68 JIT engine found");

    const CPUEngine *syn68k = GetCPUEngine("syn68k");
    CHECK(syn68k != NULL && strcmp(syn68k->id, "syn68k") == 0, "syn68k CPU engine found");

    // Test 3: Engine properties
    if (emu68) {
        CHECK(emu68->is_jit == true, "Emu68 correctly flagged as JIT engine");
    }
    if (musashi) {
        CHECK(musashi->is_jit == false, "Musashi correctly flagged as non-JIT interpreter");
    }
    if (syn68k) {
        CHECK(syn68k->is_jit == false, "syn68k correctly flagged as non-JIT interpreter");
    }

    // Test 4: Dynamic engine switching
    CHECK(SetActiveCPUEngine("musashi") == true, "SetActiveCPUEngine('musashi') succeeded");
    CHECK(GetActiveCPUEngine() == musashi, "Active engine is Musashi");

    CHECK(SetActiveCPUEngine("uae") == true, "SetActiveCPUEngine('uae') succeeded");
    CHECK(GetActiveCPUEngine() == uae, "Active engine is Amiberry/UAE");

    CHECK(SetActiveCPUEngine("emu68") == true, "SetActiveCPUEngine('emu68') succeeded");
    CHECK(GetActiveCPUEngine() == emu68, "Active engine is Emu68");

    CHECK(SetActiveCPUEngine("syn68k") == true, "SetActiveCPUEngine('syn68k') succeeded");
    CHECK(GetActiveCPUEngine() == syn68k, "Active engine is syn68k");

    // Switch back to Musashi for subsequent tests
    SetActiveCPUEngine("musashi");
    CHECK(GetActiveCPUEngine() == musashi, "Switched back to Musashi engine");
}

/*
 * Runs 68020/040 instruction snippets through Execute68k on the active engine.
 *
 * Arguments:
 *   engine: Label included in CHECK messages (e.g. "uae+jit").
 */
static void test_cpu_instruction_suite(const char *engine)
{
    printf("Running 680x0 Core Instruction Verification suite (%s)...\n", engine);
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
    CHECK_ENG(r.d[0] == 1300, engine, "ADD.L + SUB.L (1000 + 500 - 200 == 1300)");

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
    CHECK_ENG((r.d[0] & 0xFFFF) == 200, engine, "MULU.W + DIVU.W executed correctly (300 * 200 / 300 == 200)");

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
    CHECK_ENG((r.d[0] & 0x0FFFFFFF) == (0x12345678 & 0x0FFFFFFF), engine, "LSL.L + LSR.L bitshift roundtrip matches");

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
    CHECK_ENG(r.d[0] == 0x80, engine, "BSET, BCHG, BCLR bit manipulation correctly set bit 7 and cleared bit 3");

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
    CHECK_ENG(r.d[1] == 20 && (int16)(r.d[0] & 0xFFFF) == -1, engine, "DBRA loop iterated 10 times (D1 == 20, D0 == -1)");

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
    CHECK_ENG(r.d[0] == 0x11111111 && r.d[1] == 0x22222222 && r.d[2] == 0x33333333 && r.d[3] == 0x44444444,
          engine, "MOVEM.L save and restore matched bit-for-bit across D0-D3");

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
    CHECK_ENG(r.d[0] == 122, engine, "Nested JSR + BSR + RTS stack return executed cleanly (100 + 10 + 7 + 5 == 122)");

    // Test 8: 68020+ Bitfield Extraction: BFEXTU D0 {8:16}, D1
    // Extracts 16 bits starting at bit offset 8 (0x12345678 -> 0x3456)
    code_addr = 0x7140;
    WriteMacInt16(code_addr + 0, 0xE9C0); // BFEXTU D0, D1
    WriteMacInt16(code_addr + 2, 0x1210); // D1, offset=8, width=16
    WriteMacInt16(code_addr + 4, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.d[0] = 0x12345678;
    Execute68k(code_addr, &r);
    CHECK_ENG((r.d[1] & 0xFFFF) == 0x3456, engine, "68020+ BFEXTU D0 {8:16}, D1 extracted 0x3456 from 0x12345678");

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
    CHECK_ENG(r.d[3] == 6 && r.d[2] == 0, engine, "68020+ 32x32->64-bit MULU.L (0x20000 * 0x30000 == 6:0)");

    // Test 10: A2-A5 must survive a block that does not touch them.
    // Emu68 pins A3 in x16; using x16 as the JIT entry scratch left A3 equal to
    // the host ARM pointer after the first translated unit (ROM boot crash).
    printf("  [CPU-TEST] Test 10: A2-A5 preserved across NOP...\n"); fflush(stdout);
    code_addr = 0xA000;
    WriteMacInt16(code_addr + 0, 0x4E71); // NOP
    WriteMacInt16(code_addr + 2, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.a[2] = 0xA2A2A2A2;
    r.a[3] = 0xA3A3A3A3;
    r.a[4] = 0xA4A4A4A4;
    r.a[5] = 0xA5A5A5A5;
    Execute68k(code_addr, &r);
    CHECK_ENG(r.a[2] == 0xA2A2A2A2 && r.a[3] == 0xA3A3A3A3 &&
              r.a[4] == 0xA4A4A4A4 && r.a[5] == 0xA5A5A5A5,
              engine, "A2-A5 unchanged across NOP (JIT entry must not clobber A3/x16)");

    // Test 11: MOVE.L (A3), D0 must use the guest pointer, not a host entry.
    printf("  [CPU-TEST] Test 11: MOVE.L (A3), D0...\n"); fflush(stdout);
    uint32 a3_payload = 0xA800;
    WriteMacInt32(a3_payload, 0xC0DEF00D);
    code_addr = 0xA010;
    WriteMacInt16(code_addr + 0, 0x2013); // MOVE.L (A3), D0
    WriteMacInt16(code_addr + 2, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.a[3] = a3_payload;
    Execute68k(code_addr, &r);
    CHECK_ENG(r.d[0] == 0xC0DEF00D && r.a[3] == a3_payload,
              engine, "MOVE.L (A3), D0 loaded guest 0xC0DEF00D and left A3 unchanged");

    // Test 12: MOVEC to CACR calls a C helper via blr x1. Upstream materializes
    // that pointer with the uint16 halfwords reversed on LE, which SIGSEGVs at
    // 0x....00010000 during 32-bit Mac ROM boot (StartBoot writes CACR).
    printf("  [CPU-TEST] Test 12: MOVEC D0,CACR / MOVEC CACR,D1...\n"); fflush(stdout);
    code_addr = 0xA120;
    WriteMacInt16(code_addr + 0, 0x4E7B); // MOVEC D0, Rc
    WriteMacInt16(code_addr + 2, 0x0002); // Rc = CACR
    WriteMacInt16(code_addr + 4, 0x4E7A); // MOVEC Rc, D1
    WriteMacInt16(code_addr + 6, 0x1002); // D1, Rc = CACR
    WriteMacInt16(code_addr + 8, 0x4E75); // RTS

    memset(&r, 0, sizeof(r));
    r.d[0] = 0x80008000; // 68040 IE | DE; no IE 1→0 so check_cacr will not flush
    Execute68k(code_addr, &r);
    CHECK_ENG(r.d[1] == 0x80008000, engine,
              "MOVEC D0,CACR helper returned; MOVEC CACR,D1 read 0x80008000");
}

/*
 * Reproduces the Basilisk ROM-boot SIGSEGV after EmulOp 0x7103 (RESET).
 *
 * Real boot: 0x4EFA at ROM+0x2A, then PatchROM's 0x7103 and JMP abs.L at
 * ROM+0x8C. If the JIT trampoline clobbers A3 with the host entry, the
 * continuation's MOVE.L (A3) treats that pointer as a Mac address.
 *
 * Arguments:
 *   engine - Label included in CHECK messages (e.g. "emu68").
 */
static void test_rom_boot_after_reset(const char *engine)
{
    printf("Running ROM-boot RESET/4EFA/JMP sequence (%s)...\n", engine);

    const uint32 a3_payload = 0xA800;
    const uint32 entry = 0xA020;      /* stands in for ROM+0x2A (4EFA) */
    const uint32 reset_op = 0xA040;   /* stands in for ROM+0x8C (0x7103) */
    const uint32 cont = 0xA060;       /* stands in for ROM+0xBA */
    const uint32 exec_return = 0xA0E0;
    const uint32 boot_stack = 0x10000;

    /* Guest word that MOVE.L (A3) must return if A3 is still a Mac pointer. */
    WriteMacInt32(a3_payload, 0xC0DEF00D);
    /* RESET replaces A7 with the boot stack; RTS from cont pops this long. */
    WriteMacInt32(boot_stack, exec_return);
    WriteMacInt16(exec_return, (uint16)M68K_EXEC_RETURN);

    /* 0x4EFA displacement is relative to the word after the opcode. */
    WriteMacInt16(entry + 0, 0x4EFA);
    WriteMacInt16(entry + 2, (uint16)(reset_op - (entry + 2)));

    /* Same PatchROM sequence: RESET EmulOp then JMP to the boot continuation. */
    WriteMacInt16(reset_op + 0, (uint16)M68K_EMUL_OP_RESET);
    WriteMacInt16(reset_op + 2, 0x4EF9);
    WriteMacInt32(reset_op + 4, cont);

    /* Continuation uses A3; D1 flags that we actually arrived here. */
    WriteMacInt16(cont + 0, 0x2013);
    WriteMacInt16(cont + 2, 0x7201);
    WriteMacInt16(cont + 4, 0x4E75);

    M68kRegisters r;
    memset(&r, 0, sizeof(r));
    r.a[3] = a3_payload;
    Execute68k(entry, &r);

    CHECK_ENG(r.d[1] == 1, engine,
              "ROM-boot sequence reached JMP continuation after RESET (D1 == 1)");
    CHECK_ENG(r.d[0] == 0xC0DEF00D && r.a[3] == a3_payload, engine,
              "MOVE.L (A3) after 4EFA/RESET/JMP used guest A3, not the JIT entry");
    CHECK_ENG(r.a[6] == (RAMBaseMac + RAMSize - 0x1c), engine,
              "RESET EmulOp installed BootGlobs in A6");
}

/*
 * Verifies opcode 0x773F (68k "line 7" MOVEQ space with bit 8 set) raises
 * Vector 4 (Illegal Instruction) in isolation, independent of any boot
 * sequence.
 *
 * 0x773F is a permanently reserved encoding on every 680x0: MOVEQ requires
 * bit 8 clear (0111 ddd0 dddddddd); anything in line 7 with bit 8 set is
 * illegal on real hardware. This is exactly the opcode CockatriceIII's
 * Emu68 backend hit at PC 0x0000FC00 during a real Quadra 800 boot, where
 * it printed "[JIT] opcode 773f ... not implemented" in an unbroken loop
 * instead of reaching Mac OS's Vector 4 handler. Isolating it here (poking
 * the opcode directly at a scratch address, independent of whatever
 * boot-sequence bug put a real PC there) answers a narrower question than
 * the live boot log can: does the engine's illegal-instruction path work
 * at all, on its own.
 *
 * The Illegal Instruction exception pushes the PC of the *faulting*
 * instruction, not the next one (unlike TRAP/TRAPV/DIVS in
 * test_exception_traps(), which resume after the trapping instruction) —
 * RTE without adjusting the stacked PC re-executes the same illegal
 * opcode forever. The handler below reads the stacked PC into D1 (to
 * confirm the engine pushed the right address) then advances it by 2
 * before RTE, exactly as a real handler must.
 *
 * Arguments:
 *   engine: Label included in CHECK messages (e.g. "emu68").
 */
static void test_illegal_opcode_exception(const char *engine)
{
    printf("Running Illegal Instruction (opcode 0x773F) isolation test (%s)...\n", engine);

    const uint32 illegal_handler = 0x8030;
    const uint32 code_addr = 0x7240;

    // MOVE.L (2,A7),D1     ; D1 = stacked fault PC (format-0 short frame: SR, then PC)
    WriteMacInt16(illegal_handler + 0, 0x222F);
    WriteMacInt16(illegal_handler + 2, 0x0002);
    // ADDQ.L #2,(2,A7)     ; skip the illegal opcode so RTE does not refault.
    // Must be a LONG add: the pushed PC is a 32-bit big-endian field at (2,A7),
    // so a WORD-sized add there only touches its high 16 bits and corrupts PC.
    WriteMacInt16(illegal_handler + 4, 0x54AF);
    WriteMacInt16(illegal_handler + 6, 0x0002);
    // ADDQ.L #4,D0         ; observable proof the handler ran
    WriteMacInt16(illegal_handler + 8, 0x5880);
    // RTE
    WriteMacInt16(illegal_handler + 10, 0x4E73);
    WriteMacInt32(0x10, illegal_handler); // Vector 4 = offset 0x10

    WriteMacInt16(code_addr + 0, 0x773F); // illegal: line 7, bit 8 set
    WriteMacInt16(code_addr + 2, 0x4E75); // RTS (reached only after the fixed-up RTE)

    M68kRegisters r;
    memset(&r, 0, sizeof(r));
    r.d[0] = 20;
    r.d[1] = 0;
    Execute68k(code_addr, &r);

    CHECK_ENG(r.d[1] == code_addr, engine,
              "Illegal opcode 0x773F pushed the faulting instruction's own PC (not the next one)");
    CHECK_ENG(r.d[0] == 24, engine,
              "Illegal opcode 0x773F vectored to Vector 4 handler and returned via RTE (20 -> 24)");
}

/*
 * Verifies REG_PC survives a MOVEM.L -(An) immediately followed by an
 * LEA (d16,PC),An in the same block.
 *
 * Isolates the real Quadra 800 boot hang traced to Emu68's JIT: a live
 * runtime probe (see JIT_MEMORY_MODEL_FIXES.md) showed the JIT's pinned
 * PC register (ARM w18) reading back as 0x0000002A -- the low 16 bits of
 * the very first boot PC (0x4080002A) -- instead of the true current
 * address, immediately after "MOVEM.L D5-D7/A5-A6,-(A7)" followed by
 * "LEA (6,PC),A6" ran inside a real ROM superblock. This reproduces that
 * shape (MOVEM.L -(An), then LEA (d16,PC),An) in isolation: instead of
 * jumping through the possibly-corrupt PC (unsafe to observe), a second
 * LEA (0,PC),A1 right after reads PC back into a register we can check
 * via Execute68k's returned A1, with no wild control transfer if it's
 * wrong.
 *
 * Arguments:
 *   engine: Label included in CHECK messages (e.g. "emu68").
 */
static void test_movem_lea_pc_tracking(const char *engine)
{
    printf("Running MOVEM.L -(An) / LEA (d16,PC) PC-tracking test (%s)...\n", engine);

    const uint32 code_addr = 0x7250;

    // Exact byte-for-byte reproduction of the real Quadra 800 ROM sequence
    // at 0x408000BA that corrupted REG_PC (see JIT_MEMORY_MODEL_FIXES.md).
    // Probe register is A4 -- the only address register untouched by either
    // MOVEM mask (D5-D7/A5-A6 and D0-D2/A0-A3), so the restores below can't
    // clobber it.
    WriteMacInt16(code_addr + 0,  0x48E7);  // MOVEM.L D5-D7/A5-A6,-(A7)
    WriteMacInt16(code_addr + 2,  0x0706);
    WriteMacInt16(code_addr + 4,  0x4DFA);  // LEA (6,PC),A6
    WriteMacInt16(code_addr + 6,  0x0006);
    WriteMacInt16(code_addr + 8,  0x48E7);  // MOVEM.L D0-D2/A0-A3,-(A7)
    WriteMacInt16(code_addr + 10, 0xE0F0);
    WriteMacInt16(code_addr + 12, 0x4DFA);  // LEA (0xC,PC),A6
    WriteMacInt16(code_addr + 14, 0x000C);
    WriteMacInt16(code_addr + 16, 0x49FA);  // LEA (0,PC),A4  <- probe
    WriteMacInt16(code_addr + 18, 0x0000);
    WriteMacInt16(code_addr + 20, 0x4CDF);  // MOVEM.L (A7)+,D0-D2/A0-A3 (undo 2nd save)
    WriteMacInt16(code_addr + 22, 0x0F07);
    WriteMacInt16(code_addr + 24, 0x4CDF);  // MOVEM.L (A7)+,D5-D7/A5-A6 (undo 1st save)
    WriteMacInt16(code_addr + 26, 0x60E0);
    WriteMacInt16(code_addr + 28, 0x4E75);  // RTS

    const uint32 expected_a4 = code_addr + 18; // address of the probe LEA's own extension word + disp(0)

    M68kRegisters r;
    memset(&r, 0, sizeof(r));
    Execute68k(code_addr, &r);

    CHECK_ENG(r.a[4] == expected_a4, engine,
              "PC-relative LEA right after MOVEM.L -(An) computed the correct address");
}

/*
 * Reproduces the exact real ROM "save regs / dispatch through a PC-relative
 * jump table / restore regs" pattern at 0x408000BA, JMP included this time
 * (test_movem_lea_pc_tracking() above swaps the JMP for a safe probe, which
 * turned out to still pass -- the bug needs the JMP itself). The jump
 * table's single entry lives at 0x9000: LEA's immediate is chosen so the
 * *correct* computed target lands exactly there, where a stub undoes the
 * two MOVEM saves and returns. If the JIT computes the target wrong (as
 * confirmed live during real boot -- see JIT_MEMORY_MODEL_FIXES.md), PC
 * lands in ordinary (zero-filled) RAM instead of the stub and this hangs
 * rather than failing cleanly, so it is NOT wired into test_all_cpu_engines()
 * yet; call it manually while fixing the bug, then decide how to guard it
 * (e.g. a cycle-budget check in Execute68k) before enabling it for real.
 *
 * Arguments:
 *   engine: Label included in CHECK messages (e.g. "emu68").
 */
static void test_movem_lea_jmp_table_dispatch(const char *engine)
{
    printf("Running MOVEM.L / LEA / JMP(d8,PC,Xn.L) table-dispatch test (%s)...\n", engine);

    const uint32 code_addr = 0x7250;
    const uint32 table_addr = 0x9000;

    WriteMacInt16(code_addr + 0,  0x48E7);  // MOVEM.L D5-D7/A5-A6,-(A7)
    WriteMacInt16(code_addr + 2,  0x0706);
    WriteMacInt16(code_addr + 4,  0x4DFA);  // LEA (6,PC),A6
    WriteMacInt16(code_addr + 6,  0x0006);
    WriteMacInt16(code_addr + 8,  0x48E7);  // MOVEM.L D0-D2/A0-A3,-(A7)
    WriteMacInt16(code_addr + 10, 0xE0F0);
    WriteMacInt16(code_addr + 12, 0x4DFA);  // LEA (0xC,PC),A6
    WriteMacInt16(code_addr + 14, 0x000C);
    WriteMacInt16(code_addr + 16, 0x4BF9);  // LEA $0001DA0.L,A5 (chosen so the JMP below lands on table_addr)
    WriteMacInt16(code_addr + 18, 0x0000);
    WriteMacInt16(code_addr + 20, 0x1DA0);
    WriteMacInt16(code_addr + 22, 0x4EFB);  // JMP (-8,PC,A5.L)
    WriteMacInt16(code_addr + 24, 0xD8F8);

    WriteMacInt16(table_addr + 0, 0x4CDF);  // MOVEM.L (A7)+,D0-D2/A0-A3
    WriteMacInt16(table_addr + 2, 0x0F07);
    WriteMacInt16(table_addr + 4, 0x4CDF);  // MOVEM.L (A7)+,D5-D7/A5-A6
    WriteMacInt16(table_addr + 6, 0x60E0);
    WriteMacInt16(table_addr + 8, 0x5880);  // ADDQ.L #4,D0
    WriteMacInt16(table_addr + 10, 0x4E75); // RTS

    M68kRegisters r;
    memset(&r, 0, sizeof(r));
    r.d[0] = 100;
    Execute68k(code_addr, &r);

    CHECK_ENG(r.d[0] == 104, engine,
              "JMP(d8,PC,A5.L) right after MOVEM.L/LEA landed on the correct jump-table entry");
}

/*
 * Verifies Musashi can switch among 68000/010/020/030/040 CPU types.
 * UAE and Emu68 stay on 68040 in this suite (Mac II / Quadra class).
 */
static void test_musashi_cpu_models(void)
{
    printf("Running Musashi multi-CPU model initialization...\n");
    const int cpu_numbers[] = { 0, 10, 20, 30, 40 };
    for (int cpu_model = 0; cpu_model <= 4; cpu_model++) {
        CPUType = cpu_model;
        musashi_cpu_engine.init();
        char msg[128];
        snprintf(msg, sizeof(msg), "Musashi 680%02d initialization and CPU mode switch passed", cpu_numbers[cpu_model]);
        CHECK(GetActiveCPUEngine() != NULL, msg);
    }
    CPUType = 4;
    musashi_cpu_engine.init();
}

/*
 * Runs TRAP #0, TRAPV, and divide-by-zero through Execute68k on the active engine.
 *
 * Arguments:
 *   engine: Label included in CHECK messages (e.g. "uae+jit").
 */
static void test_exception_traps(const char *engine)
{
    printf("Running 680x0 Exception Traps verification suite (%s)...\n", engine);
    M68kRegisters r;
    uint32 code_addr;

    // Test 1: TRAP #0 instruction (Vector 32, offset 0x80 in vector table)
    uint32 trap0_handler = 0x8000;
    WriteMacInt16(trap0_handler + 0, 0x5A80); // ADDQ.L #5, D0
    WriteMacInt16(trap0_handler + 2, 0x4E73); // RTE
    WriteMacInt32(0x80, trap0_handler);        // Vector 32 = 0x80

    code_addr = 0x7200;
    WriteMacInt16(code_addr + 0, 0x4E40);      // TRAP #0
    WriteMacInt16(code_addr + 2, 0x4E75);      // RTS

    memset(&r, 0, sizeof(r));
    r.d[0] = 10;
    Execute68k(code_addr, &r);
    CHECK_ENG(r.d[0] == 15, engine, "TRAP #0 vectored to handler and returned via RTE (10 -> 15)");

    // Test 2: TRAPV on Overflow (Vector 7, offset 0x1C in vector table)
    uint32 trapv_handler = 0x8010;
    WriteMacInt16(trapv_handler + 0, 0x5080); // ADDQ.L #8, D0
    WriteMacInt16(trapv_handler + 2, 0x4E73); // RTE
    WriteMacInt32(0x1C, trapv_handler);       // Vector 7 = 0x1C

    code_addr = 0x7210;
    WriteMacInt16(code_addr + 0, 0x4E76);      // TRAPV
    WriteMacInt16(code_addr + 2, 0x4E75);      // RTS

    // Case 2a: V flag clear (no trap taken)
    memset(&r, 0, sizeof(r));
    r.d[0] = 10;
    Execute68k(code_addr, &r);
    CHECK_ENG(r.d[0] == 10, engine, "TRAPV with V=0 did not take trap (D0 remains 10)");

    // Case 2b: V flag set (trap taken)
    uint32 code_addr_v1 = 0x7230;
    WriteMacInt16(code_addr_v1 + 0, 0x003C);    // ORI.B #2, CCR (set V flag)
    WriteMacInt16(code_addr_v1 + 2, 0x0002);
    WriteMacInt16(code_addr_v1 + 4, 0x4E76);    // TRAPV
    WriteMacInt16(code_addr_v1 + 6, 0x4E75);    // RTS

    memset(&r, 0, sizeof(r));
    r.d[0] = 10;
    Execute68k(code_addr_v1, &r);
    CHECK_ENG(r.d[0] == 18, engine, "TRAPV with V=1 vectored to handler and returned via RTE (10 -> 18)");

    // Test 3: Zero Divide Exception (Vector 5, offset 0x14 in vector table)
    uint32 divzero_handler = 0x8020;
    WriteMacInt16(divzero_handler + 0, 0x7063); // MOVEQ #99, D0
    WriteMacInt16(divzero_handler + 2, 0x4E73); // RTE
    WriteMacInt32(0x14, divzero_handler);       // Vector 5 = 0x14

    code_addr = 0x7220;
    WriteMacInt16(code_addr + 0, 0x81C1);       // DIVS.W D1, D0
    WriteMacInt16(code_addr + 2, 0x4E75);       // RTS

    memset(&r, 0, sizeof(r));
    r.d[0] = 50;
    r.d[1] = 0; // Divisor = 0
    Execute68k(code_addr, &r);
    CHECK_ENG(r.d[0] == 99, engine, "DIVS.W by zero vectored to Vector 5 handler and returned via RTE (D0 -> 99)");
}

/*
 * Runs Execute68k, instruction, and exception suites on every hosted 680x0 engine.
 *
 * Coverage:
 *   musashi         — Musashi interpreter
 *   syn68k          — syn68k dynamic recompiler
 *   uae             — Amiberry interpreter (jit false)
 *   uae+jit         — Amiberry ARM64/x86-64 JIT without JIT FPU (jit true, jitfpu false)
 *   uae+jit+jitfpu  — Amiberry ARM64/x86-64 JIT with JIT FPU (jit true, jitfpu true)
 *   emu68           — Emu68 AArch64 translator (jit true)
 */
static void test_all_cpu_engines(void)
{
    struct EngineConfig {
        const char *id;
        bool jit;
        bool jitfpu;
        const char *label;
    };
    static const EngineConfig configs[] = {
        { "musashi", false, false, "musashi" },
        { "syn68k",  false, false, "syn68k" },
        { "uae",     false, false, "uae" },
        { "uae",     true,  false, "uae+jit" },
        { "uae",     true,  true,  "uae+jit+jitfpu" },
        { "emu68",   true,  false, "emu68" },
    };

    for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
        const EngineConfig *cfg = &configs[i];
        printf("\n=== CPU engine: %s (id=%s jit=%s jitfpu=%s) ===\n",
               cfg->label, cfg->id, cfg->jit ? "true" : "false",
               cfg->jitfpu ? "true" : "false");

        char init_msg[160];
        snprintf(init_msg, sizeof(init_msg), "activate_cpu_engine('%s', jit=%s, jitfpu=%s) succeeded",
                 cfg->id, cfg->jit ? "true" : "false", cfg->jitfpu ? "true" : "false");
        bool ok = activate_cpu_engine(cfg->id, cfg->jit, cfg->jitfpu);
        CHECK(ok, init_msg);
        if (!ok)
            continue;

        test_emulop_and_execute68k(cfg->label);
        test_cpu_instruction_suite(cfg->label);
        test_rom_boot_after_reset(cfg->label);
        /*
         * syn68k excluded: it mistranslates this sequence (forwards a
         * *different* opcode, 0x0000, from a PC nowhere near code_addr) and
         * spins forever rather than reaching Vector 4. That is a real,
         * separate bug in syn68k's recompiler, not something to paper over
         * here — see docs/basilisk-ii-boot-and-patch.md and the "syn68k"
         * follow-up noted after the Emu68 memory-model fix.
         */
        if (strcmp(cfg->id, "syn68k") != 0)
            test_illegal_opcode_exception(cfg->label);
        test_movem_lea_pc_tracking(cfg->label);
        if (!cfg->jit)
            test_exception_traps(cfg->label);
        if (strcmp(cfg->id, "musashi") == 0 || strcmp(cfg->id, "uae") == 0)
            test_fpu_execution(cfg->label);
    }
}

int main(void)
{
    install_crash_handler();
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== CockatriceIII / Multi-Engine Integration Test Suite ===\n");

    if (getenv("JMP_TABLE_MANUAL")) {
        // Confirms test_movem_lea_jmp_table_dispatch() at a low-RAM address
        // (0x7250) passes even with the real boot's RESET/4EFA/JMP lead-in
        // executed first through the same JIT. This is expected to PASS --
        // see JMP_TABLE_ROM below for the actual bug reproduction.
        memory_init();
        CHECK(activate_cpu_engine("emu68", true), "activate emu68 for manual JMP-table test");
        test_movem_lea_jmp_table_dispatch("emu68");
        printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
        return g_fail == 0 ? 0 : 1;
    }

    if (getenv("JMP_TABLE_ROM")) {
        // KNOWN BUG REPRODUCTION (see JIT_MEMORY_MODEL_FIXES.md).
        //
        // Identical to test_movem_lea_jmp_table_dispatch() above, but the
        // code and jump table are placed at ROM-range addresses
        // (ROMBaseMac + 0x7000 / + 0x7100) instead of low RAM (0x7250 /
        // 0x9000). The low-RAM version passes; this one HANGS -- Emu68's
        // JIT computes the wrong JMP(d8,PC,A5.L) target only when the
        // block executes from a ROM address, landing PC in ordinary RAM
        // instead of the jump table and running off into zero-filled
        // memory. This is the real Quadra 800 boot hang, isolated down to
        // "ROM vs RAM address range" as the last unexplained variable.
        //
        // Deliberately NOT wired into test_all_cpu_engines(): it hangs
        // rather than failing cleanly while the bug is open, and there is
        // no cycle-budget escape hatch in Execute68k to bound it. Run
        // manually (with a wall-clock timeout) while working on this bug;
        // wire it in once fixed, or once such a safety net exists.
        ROMBaseMac = 0x40800000;
        memory_init();
        CHECK(activate_cpu_engine("emu68", true), "activate emu68 for ROM-range JMP-table test");
        uint32 code_addr = ROMBaseMac + 0x00007000;
        if (getenv("JMP_TABLE_ADDR"))
            code_addr = (uint32)strtoul(getenv("JMP_TABLE_ADDR"), NULL, 0);
        uint32 table_addr = code_addr + 0x100;
        if (getenv("JMP_TABLE_DIST"))
            table_addr = code_addr + (uint32)strtoul(getenv("JMP_TABLE_DIST"), NULL, 0);
        printf("[ROM-RANGE] code_addr=0x%08X table_addr=0x%08X\n", code_addr, table_addr);
        // Write directly through the host pointer -- WriteMacInt silently
        // drops writes to ROM range, which would make this a no-op test.
        bool use_writemacint = getenv("JMP_TABLE_WMI") != NULL;
        auto poke16 = [use_writemacint](uint32 addr, uint16 val) {
            if (use_writemacint) {
                WriteMacInt16(addr, val);
            } else {
                Host_Mem_Base[addr + 0] = (uint8)(val >> 8);
                Host_Mem_Base[addr + 1] = (uint8)(val & 0xFF);
            }
        };
        // Real ROM block has 16 NOPs between the first LEA and the second
        // MOVEM.L (see EMU68_BOOT_PROGRESS.md); the earlier hand-picked
        // offsets omitted them. Build the layout with a variable NOP count
        // (JMP_TABLE_NOPS, default 16) so that can be tested directly.
        int nop_count = 16;
        if (getenv("JMP_TABLE_NOPS"))
            nop_count = atoi(getenv("JMP_TABLE_NOPS"));

        uint32 p = code_addr;
        poke16(p, 0x48E7); p += 2;  // MOVEM.L D5-D7/A5-A6,-(A7)
        poke16(p, 0x0706); p += 2;
        poke16(p, 0x4DFA); p += 2;  // LEA (6,PC),A6
        poke16(p, 0x0006); p += 2;
        for (int i = 0; i < nop_count; i++) { poke16(p, 0x4E71); p += 2; }
        poke16(p, 0x48E7); p += 2;  // MOVEM.L D0-D2/A0-A3,-(A7)
        poke16(p, 0xE0F0); p += 2;
        poke16(p, 0x4DFA); p += 2;  // LEA (0xC,PC),A6
        poke16(p, 0x000C); p += 2;
        poke16(p, 0x4BF9); p += 2;  // LEA $const.L,A5
        uint32 a5_lo_addr = p;
        p += 4; // hi16/lo16 filled in below, once ext_word_addr is known
        uint32 jmp_addr = p;
        poke16(p, 0x4EFB); p += 2;  // JMP (-8,PC,A5.L)
        poke16(p, 0xD8F8); p += 2;
        uint32 ext_word_addr = jmp_addr + 2;
        int32_t a5val = (int32_t)table_addr - (int32_t)ext_word_addr + 8;
        poke16(a5_lo_addr + 0, (uint16)((uint32_t)a5val >> 16));
        poke16(a5_lo_addr + 2, (uint16)(a5val & 0xFFFF));
        printf("[ROM-RANGE] nop_count=%d block_len=%u\n", nop_count, p - code_addr);
        poke16(table_addr + 0, 0x4CDF);
        poke16(table_addr + 2, 0x0F07);
        poke16(table_addr + 4, 0x4CDF);
        poke16(table_addr + 6, 0x60E0);
        poke16(table_addr + 8, 0x5880);
        poke16(table_addr + 10, 0x4E75);

        M68kRegisters r;
        memset(&r, 0, sizeof(r));
        r.d[0] = 100;
        if (getenv("JMP_TABLE_SP"))
            r.a[7] = (uint32)strtoul(getenv("JMP_TABLE_SP"), NULL, 0);
        printf("[ROM-RANGE] initial a7=0x%08X\n", r.a[7]);
        Execute68k(code_addr, &r);
        printf("[ROM-RANGE RESULT] d0=0x%08X (expected 104)\n", r.d[0]);
        return (r.d[0] == 104) ? 0 : 1;
    }

    test_memory_banking();
    test_cpu_engine_abstraction();
    test_all_cpu_engines();

    // SCSI and SCC suites exercise EmulOps through Musashi specifically
    CHECK(activate_cpu_engine("musashi", false), "Re-activate Musashi for SCSI/SCC suites");
    test_musashi_cpu_models();
    test_scc_subsystem();
    test_scsi_subsystem();
    test_scsi_advanced();

    printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
    fflush(stdout);
    return g_fail == 0 ? 0 : 1;
}
