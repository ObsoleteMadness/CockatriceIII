/*
 *  prefs_parse.cpp - Splitting one line of a CockatriceIII_Prefs file
 *
 *  Cockatrice III (C) 2026
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Kept apart from prefs.cpp, and free of any other dependency, so the test
 *  suites (which stub out the prefs store) can link and exercise the parser.
 *
 *  Line syntax:
 *
 *      keyword value          # optional trailing comment
 *
 *  - A line whose first non-blank character is '#' or ';' is a comment.
 *  - A '#' or ';' that follows whitespace starts a trailing comment, which is
 *    dropped with the whitespace before it. A '#' or ';' inside a word is
 *    kept, so a path such as /Volumes/Mac#2.hda survives.
 *  - The value is everything after the whitespace that follows the keyword,
 *    up to the comment or end of line, with surrounding whitespace removed. It
 *    may itself contain spaces (a path such as "/Users/me/Mac Disks/HD.hda").
 *  - CR, LF and trailing whitespace are ignored, so CRLF files read the same
 *    as LF files.
 */

#include <ctype.h>
#include <string.h>

#include "sysdeps.h"
#include "prefs.h"

/*
 * Returns whether c is blank for the purposes of the prefs syntax.
 *
 * Arguments:
 *   c: Character to test.
 */
static bool prefs_is_blank(char c)
{
	return isspace((unsigned char)c) != 0;
}

/*
 * Splits one prefs-file line, in place, into a keyword and a value.
 *
 * Arguments:
 *   line: NUL-terminated line as read from the file; modified in place (the
 *         keyword and value are terminated inside it).
 *   keyword: Receives a pointer to the keyword inside line.
 *   value: Receives a pointer to the value inside line.
 *
 * Returns:
 *   true if the line holds a keyword and a non-empty value; false for blank
 *   lines, comment lines and a keyword with no value, which the caller skips.
 */
bool PrefsParseLine(char *line, char **keyword, char **value)
{
	// Skip leading whitespace; a line that is blank or a comment ends here.
	char *p = line;
	while (*p && prefs_is_blank(*p))
		p++;
	if (*p == 0 || *p == '#' || *p == ';')
		return false;

	// Cut the line at the first comment marker that follows whitespace.
	for (char *q = p + 1; *q; q++) {
		if ((*q == '#' || *q == ';') && prefs_is_blank(q[-1])) {
			*q = 0;
			break;
		}
	}

	// Drop the line ending (CR and/or LF) and any trailing whitespace.
	size_t len = strlen(p);
	while (len > 0 && prefs_is_blank(p[len - 1]))
		p[--len] = 0;

	// The keyword runs to the first whitespace; nothing after it means no value.
	char *k = p;
	while (*p && !prefs_is_blank(*p))
		p++;
	if (*p == 0)
		return false;
	*p++ = 0;

	// The value starts at the next non-blank character.
	while (*p && prefs_is_blank(*p))
		p++;
	if (*p == 0)
		return false;

	*keyword = k;
	*value = p;
	return true;
}
