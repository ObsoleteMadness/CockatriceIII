/*
 *  scc.h - Z8530 SCC & LocalTalk emulation
 *
 *  Adapted for Cockatrice III from Mini vMac (SCCEMDEV.c & LTOVRUDP.h)
 */

#ifndef SCC_H
#define SCC_H

#include "sysdeps.h"

extern void SCCInit(void);
extern void SCCExit(void);

extern void SCC_Reset(void);
extern uint32 SCC_Access(uint32 Data, bool WriteMem, uint32 addr);
extern bool SCC_InterruptsEnabled(void);
extern void LocalTalkTick(void);

extern uint8 SCCInterruptRequest;

#endif
