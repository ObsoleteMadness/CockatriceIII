/*
 *  prefs_dummy.cpp - Preferences handling, dummy implementation
 *
 *  Basilisk II (C) 1997-1999 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"

#include <stdio.h>
#include <stdlib.h>

#include "prefs.h"
#include "host_paths.h"

#include <string>


// Platform-specific preferences items
prefs_desc platform_prefs_items[] = {
	{NULL, TYPE_END, false}	// End of list
};


// Prefs file name; its directory is chosen by HostPaths (see host_paths.h).
const char PREFS_FILE_NAME[] = "CockatriceIII_Prefs";

// Full path of the prefs file that was loaded, so SavePrefs() writes back to
// the same file. Empty until LoadPrefs() has run.
static std::string prefs_path;


/*
 *  Load preferences from the first prefs file on the host search path
 *  (executable dir, macOS bundle Resources, then the per-user location).
 *  When none exists, the defaults are written to the per-user location so
 *  the next launch, and SavePrefs(), have a writable file to use.
 */

void LoadPrefs(void)
{
	// Look for an existing prefs file in search order.
	if (HostPaths_FindExisting(HOST_PATH_PREFS, PREFS_FILE_NAME, prefs_path)) {
		FILE *f = fopen(prefs_path.c_str(), "r");
		if (f != NULL) {
			printf("Prefs: %s\n", prefs_path.c_str());
			LoadPrefsFromStream(f);
			fclose(f);
			return;
		}
	}

	// No prefs file: save defaults to the per-user location, which is
	// writable (unlike a signed bundle or a system install directory).
	prefs_path = HostPaths_UserPath(HOST_PATH_PREFS, PREFS_FILE_NAME);
	printf("Prefs: none found, writing defaults to %s\n", prefs_path.c_str());
	SavePrefs();
}


/*
 *  Save preferences back to the file LoadPrefs() used. Called before
 *  LoadPrefs() (or if no per-user location exists), it falls back to the
 *  per-user path, then to the bare name in the working directory.
 */

void SavePrefs(void)
{
	if (prefs_path.empty())
		prefs_path = HostPaths_UserPath(HOST_PATH_PREFS, PREFS_FILE_NAME);
	const char *path = prefs_path.empty() ? PREFS_FILE_NAME : prefs_path.c_str();
	FILE *f;
	if ((f = fopen(path, "w")) != NULL) {
		SavePrefsToStream(f);
		fclose(f);
	}
}


/*
 *  Add defaults of platform-specific prefs items
 *  You may also override the defaults set in PrefsInit()
 */

void AddPlatformPrefsDefaults(void)
{
}
