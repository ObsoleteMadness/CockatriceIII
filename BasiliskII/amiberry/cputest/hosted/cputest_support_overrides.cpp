/*
 * cputest_support_overrides.cpp - Cockatrice fixes for vendored WinUAE cputest_support
 *
 * Upstream cputest_support calls exit() from fp_init_native* stubs; hosted CI must not.
 * Also supplies globals/helpers the minimal cputest link omits from full Amiberry.
 */

#include "sysdeps.h"

bool canbang = false;

void to_upper(TCHAR *s, int len)
{
	for (int i = 0; i < len && s[i]; i++)
		s[i] = (TCHAR)_totupper((unsigned char)s[i]);
}

void to_lower(TCHAR *s, int len)
{
	for (int i = 0; i < len && s[i]; i++)
		s[i] = (TCHAR)_totlower((unsigned char)s[i]);
}

char *ua_copy(char *dst, int maxlen, const char *src)
{
	if (maxlen <= 0)
		return dst;
	strncpy(dst, src, maxlen - 1);
	dst[maxlen - 1] = '\0';
	return dst;
}
