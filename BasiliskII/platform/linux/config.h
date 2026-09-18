/*
 * config.h for Linux (AMD64 and ARM64), SDL 1.2 video/audio.
 *
 * Hand-maintained rather than generated: the supported matrix is narrow
 * enough that probing for these answers at configure time would only be a
 * slower way of writing them down. See platform/sysdeps.h for the shared
 * definitions this feeds.
 */

#ifndef CONFIG_H
#define CONFIG_H

#define STDC_HEADERS 1
#define TIME_WITH_SYS_TIME 1

/* SDL provides video; the X11 and GTK ports are gone. */
#define X_DISPLAY_MISSING 1

/* glibc declares loff_t itself, so unlike the other hosts nothing is needed. */

/* Type sizes on 64-bit Linux (LP64) */
#define SIZEOF_SHORT 2
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_FLOAT 4
#define SIZEOF_DOUBLE 8
#define SIZEOF_LONG_DOUBLE 16
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

/*
 * Deliberately not defined, matching macOS and Windows: it selects a
 * struct timespec tm_time_t in sysdeps.h, and the Time Manager emulation
 * is only exercised against the struct timeval form.
 */
/* #undef HAVE_CLOCK_GETTIME */

/* No O_BINARY on POSIX; opening in binary mode is the only mode. */
#define O_BINARY 0

#endif /* CONFIG_H */
