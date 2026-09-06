/*
 *  target.h - Minimal Amiberry target bits for the hosted 680x0 CPU
 *
 *  The real Amiberry target.h pulls SDL3 and the Amiga GUI. Cockatrice
 *  only needs the fopen wrapper and TARGET_NAME so sysdeps.h compiles.
 */

#ifndef WINUAE_HOSTED_TARGET_H
#define WINUAE_HOSTED_TARGET_H

#include <cstdio>
#include "uae/types.h"

#define TARGET_NAME _T("cockatrice")
#define NO_MAIN_IN_MAIN_C
#define OPTIONSFILENAME _T("default")

STATIC_INLINE FILE *uae_tfopen(const TCHAR *path, const TCHAR *mode)
{
	return fopen(path, mode);
}

#endif /* WINUAE_HOSTED_TARGET_H */
