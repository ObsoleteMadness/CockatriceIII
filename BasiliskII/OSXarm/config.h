/*
 * config.h for macOS ARM64 (macOS 26)
 */

#ifndef CONFIG_H
#define CONFIG_H

#define STDC_HEADERS 1
#define TIME_WITH_SYS_TIME 1
#define X_DISPLAY_MISSING 1

#include <sys/types.h>
typedef off_t loff_t;

/* Type sizes on 64-bit ARM macOS (LP64) */
#define SIZEOF_SHORT 2
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_FLOAT 4
#define SIZEOF_DOUBLE 8
#define SIZEOF_LONG_DOUBLE 8
#define SIZEOF_CHAR_P 8
#define SIZEOF_VOID_P 8

#define HAVE_CFMAKERAW 1
#define HAVE_NANOSLEEP 1
#define HAVE_PTHREAD_CANCEL 1
#define HAVE_SEM_INIT 1
#define HAVE_STRDUP 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_LIBPTHREAD 1

#define O_BINARY 0

#endif /* CONFIG_H */
