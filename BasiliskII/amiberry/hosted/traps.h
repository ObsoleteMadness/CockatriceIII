/*
 *  traps.h - Minimal UAE trap interface for hosted Amiberry CPU builds
 *
 *  options.h includes this. Cockatrice III does not emulate Amiga
 *  uaelib traps; the types exist only so the CPU core compiles.
 */

#ifndef WINUAE_HOSTED_TRAPS_H
#define WINUAE_HOSTED_TRAPS_H

#include "uae/types.h"

struct TrapContext;
typedef struct TrapContext TrapContext;
typedef uae_u32 (*TrapHandler)(TrapContext *);

#endif /* WINUAE_HOSTED_TRAPS_H */
