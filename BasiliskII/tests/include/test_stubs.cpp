/*
 * test_stubs.cpp - Basilisk II globals and peripheral stubs for unit tests
 *
 * DiskOpen/Prime come from disk.cpp. Sony/CD-ROM/video/serial/ether/ADB/timer
 * stay stubbed so SCSI/SCC/CPU tests do not pull the full emulator.
 */

#include <stdio.h>
#include <string.h>
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "cpu_engine.h"
#include "sony.h"
#include "cdrom.h"
#include "video.h"
#include "ether.h"
#include "extfs.h"
#include "slot_rom.h"
#include "menu_bar.h"
#include "rom_patches.h"

int CPUType = 4;
bool CPUIs68060 = false;
int FPUType = 1;
bool TwentyFourBitAddressing = false;
uint32 InterruptFlags = 0;
uint8 XPRAM[256];

uint32 TimerDateTime(void) { return 0x12345678; }
void QuitEmulator(void) {}
void TimerReset(void) {}
void EtherReset(void) {}
void MenuQueue_Reset(void) {}
void MenuQueue_Drain(void) {}
void MenuBar_UpdateAll(void) {}
void SonyReset(void) {}
void AudioReset(void) {}

int16 SonyOpen(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 SonyPrime(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 SonyControl(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 SonyStatus(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
void SonyInterrupt(void) {}
void SonyInit(void) {}
void SonyExit(void) {}
bool SonyMountVolume(void *fh) { (void)fh; return false; }

const uint8 SonyDiskIcon[258] = { 0 };
const uint8 SonyDriveIcon[258] = { 0 };
uint32 SonyDiskIconAddr = 0;
uint32 SonyDriveIconAddr = 0;

int16 CDROMOpen(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 CDROMPrime(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 CDROMControl(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
int16 CDROMStatus(uint32 pb, uint32 dce) { (void)pb; (void)dce; return 0; }
void CDROMInit(void) {}
void CDROMExit(void) {}
void CDROMInterrupt(void) {}
bool CDROMMountVolume(void *fh) { (void)fh; return false; }
const uint8 CDROMIcon[258] = { 0 };
uint32 CDROMIconAddr = 0;

struct video_desc VideoMonitor;
bool VideoInit(bool classic) { (void)classic; return true; }
void VideoExit(void) {}
void VideoQuitFullScreen(void) {}
void video_set_palette(uint8 *pal) { (void)pal; }

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
void EtherReadPacket(uint8 **src, uint32 &dest, uint32 &len, uint32 &remaining)
{
	(void)src;
	(void)dest;
	(void)len;
	(void)remaining;
}

void ADBOp(uint8 cmd, uint8 *data) { (void)cmd; (void)data; }
void ADBInterrupt(void) {}

void InsTime(uint32 tm, uint16 trap) { (void)tm; (void)trap; }
void RmvTime(uint32 tm) { (void)tm; }
void PrimeTime(uint32 tm, int count) { (void)tm; (void)count; }
void Microseconds(uint32 &hi, uint32 &lo) { hi = 0; lo = 1000; }
void TimerInterrupt(void) {}

const char *GetString(int id) { (void)id; return ""; }
void ErrorAlert(const char *msg) { printf("ErrorAlert: %s\n", msg); }
void WarningAlert(const char *msg) { printf("WarningAlert: %s\n", msg); }
void MenuAction_UpdateItem(int id) { (void)id; }

int32 AudioDispatch(uint32 params, uint32 ti)
{
	(void)params;
	(void)ti;
	return 0;
}
void AudioInterrupt(void) {}

int16 ExtFSComm(uint16 code, uint32 param, uint32 dce) { (void)code; (void)param; (void)dce; return 0; }
int16 ExtFSHFS(uint32 pb, uint16 trap, uint32 dce, uint32 a0, int16 d0)
{
	(void)pb;
	(void)trap;
	(void)dce;
	(void)a0;
	return d0;
}
void InstallExtFS(void) {}
void ExtFSInit(void) {}
void ExtFSExit(void) {}

void PutScrap(uint32 type, void *data, int size) { (void)type; (void)data; (void)size; }
void CheckLoad(uint32 type, int16 id, uint8 *p, uint32 size)
{
	(void)type;
	(void)id;
	(void)p;
	(void)size;
}
void ClearInterruptFlag(uint32 flag) { InterruptFlags &= ~flag; }
void idle_wait(void) {}

void FlushCodeCache(void *start, uint32 size)
{
	cpu_engine_invalidate_code(Host2MacAddr((uint8 *)start), size);
}

bool InstallSlotROM(void)
{
	return true;
}

void SysAddFloppyPrefs(void) {}
void SysAddCDROMPrefs(void) {}
void SysAddSerialPrefs(void) {}
void SysAddDiskPrefs(void) {}
