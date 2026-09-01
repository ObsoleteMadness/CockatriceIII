/*
 *  tchar.h - POSIX stand-in for Win32 TCHAR used by the hosted Amiberry CPU
 *
 *  Cockatrice III compiles the Amiberry 680x0 core on macOS where TCHAR is
 *  a UTF-8 char, matching FS-UAE / Amiberry rather than wchar_t Windows.
 */

#ifndef WINUAE_HOSTED_TCHAR_H
#define WINUAE_HOSTED_TCHAR_H

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef TCHAR
typedef char TCHAR;
#endif

#ifndef _T
#define _T(x) x
#endif

#ifndef TEXT
#define TEXT(x) x
#endif

#ifndef _tcsdup
#define _tcsdup strdup
#endif
#ifndef _tcslen
#define _tcslen strlen
#endif
#ifndef _tcscpy
#define _tcscpy strcpy
#endif
#ifndef _tcscat
#define _tcscat strcat
#endif
#ifndef _tcsncpy
#define _tcsncpy strncpy
#endif
#ifndef _tcschr
#define _tcschr strchr
#endif
#ifndef _tcsstr
#define _tcsstr strstr
#endif
#ifndef _tcscspn
#define _tcscspn strcspn
#endif
#ifndef _tcsncmp
#define _tcsncmp strncmp
#endif
#ifndef _tcsicmp
#define _tcsicmp strcasecmp
#endif
#ifndef _tunlink
#define _tunlink unlink
#endif
#ifndef _totupper
#define _totupper toupper
#endif
#ifndef _totlower
#define _totlower tolower
#endif
#ifndef _tstol
#define _tstol(s) strtol((s), NULL, 10)
#endif
#ifndef _tcstol
#define _tcstol strtol
#endif
#ifndef _tcstoul
#define _tcstoul strtoul
#endif
#ifndef _tcsnicmp
#define _tcsnicmp strncasecmp
#endif
#ifndef _wmkdir
#define _wmkdir(path) mkdir(path, 0755)
#endif

#endif /* WINUAE_HOSTED_TCHAR_H */
