/*
 *  host_paths.h - Where the host looks for the prefs file and the Mac ROM
 *
 *  Cockatrice III (C) 2026
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

/*
 *  Search order
 *  ------------
 *  The prefs file and a ROM given by a relative path are resolved against the
 *  same ordered list of directories. The current working directory is not in
 *  the list: a launch from Finder, Explorer or a shell in another directory
 *  must find the same files as a launch from the build directory.
 *
 *    1. The directory holding the executable (all hosts).
 *    2. macOS only: <App>.app/Contents/Resources, when running from a bundle.
 *    3. The per-user location:
 *         Windows  %APPDATA%\CockatriceIII\<name>
 *         macOS    ~/Library/CockatriceIII/<name>
 *         Linux    ~/.CockatriceIII_Prefs for prefs, ~/<name> for a ROM
 *
 *  The first existing file wins. An absolute ROM path is used as given.
 */

#ifndef HOST_PATHS_H
#define HOST_PATHS_H

#include <string>
#include <vector>

// Which kind of file is being looked up; only step 3 on Linux differs.
enum HostPathKind {
	HOST_PATH_PREFS,
	HOST_PATH_ROM
};

// Returns every candidate path for `name`, in search order.
extern std::vector<std::string> HostPaths_SearchList(HostPathKind kind, const char *name);

// Finds the first candidate that exists; returns false when none does.
extern bool HostPaths_FindExisting(HostPathKind kind, const char *name, std::string &found);

// Returns the per-user (step 3) path for `name`, creating its directory.
extern std::string HostPaths_UserPath(HostPathKind kind, const char *name);

// True when `path` is absolute on this host.
extern bool HostPaths_IsAbsolute(const char *path);

#endif
