/*
 *  host_paths.cpp - Where the host looks for the prefs file and the Mac ROM
 *
 *  Cockatrice III (C) 2026
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  The search order is documented in include/host_paths.h. This file
 *  deliberately does not include sysdeps.h: the host headers it needs
 *  (windows.h, mach-o/dyld.h) collide with the emulator's classic-Mac type
 *  definitions, and nothing here touches emulator state.
 */

#include "host_paths.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <limits.h>
#include <pwd.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef _WIN32
static const char PATH_SEP = '\\';
#else
static const char PATH_SEP = '/';
#endif

/*
 *  Returns true when `path` is absolute on this host.
 *  path: a file path; NULL or empty counts as relative.
 *  Windows accepts a drive letter ("C:\...", "C:/...") or a UNC/rooted path.
 */
bool HostPaths_IsAbsolute(const char *path)
{
	if (path == NULL || path[0] == '\0')
		return false;
#ifdef _WIN32
	// A rooted path ("\foo", "//server/share") or a drive-qualified one ("C:\foo").
	if (path[0] == '\\' || path[0] == '/')
		return true;
	return path[1] == ':' && (path[2] == '\\' || path[2] == '/');
#else
	return path[0] == '/';
#endif
}

/*
 *  Joins a directory and a relative name with the host separator.
 *  dir: directory without a trailing separator (one is tolerated).
 *  name: relative file name or path.
 *  Returns the joined path.
 */
static std::string join_path(const std::string &dir, const char *name)
{
	std::string out = dir;
	// Avoid doubling the separator when the directory already ends in one.
	if (!out.empty() && out[out.size() - 1] != '/' && out[out.size() - 1] != '\\')
		out += PATH_SEP;
	out += name;
	return out;
}

/*
 *  Returns the directory holding the running executable, symlinks resolved,
 *  or an empty string when the host cannot report it.
 *  On macOS inside a bundle this is <App>.app/Contents/MacOS.
 */
static std::string executable_dir(void)
{
	char path[PATH_MAX];
#if defined(_WIN32)
	// GetModuleFileNameA(NULL) names the .exe of this process.
	DWORD len = GetModuleFileNameA(NULL, path, sizeof(path));
	if (len == 0 || len >= sizeof(path))
		return std::string();
#elif defined(__APPLE__)
	// _NSGetExecutablePath may return a symlinked or relative path, so
	// realpath() canonicalises it before the file name is stripped.
	char raw[PATH_MAX];
	uint32_t raw_size = sizeof(raw);
	if (_NSGetExecutablePath(raw, &raw_size) != 0 || realpath(raw, path) == NULL)
		return std::string();
#else
	// /proc/self/exe is a symlink to the running binary; readlink does not
	// terminate the result, so that is done by hand.
	ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (len <= 0)
		return std::string();
	path[len] = '\0';
#endif
	// Strip the executable's own name to leave its directory.
	char *slash = strrchr(path, PATH_SEP);
#ifdef _WIN32
	char *fwd = strrchr(path, '/');
	if (fwd > slash)
		slash = fwd;
#endif
	if (slash == NULL)
		return std::string();
	*slash = '\0';
	return std::string(path);
}

/*
 *  Returns the user's home directory, or an empty string if unknown.
 *  $HOME is preferred so a user can redirect it; the password database is
 *  the fallback for launchd/daemon contexts where $HOME is unset.
 */
#ifndef _WIN32
static std::string home_dir(void)
{
	const char *home = getenv("HOME");
	if (home && home[0])
		return std::string(home);
	struct passwd *pw = getpwuid(getuid());
	if (pw && pw->pw_dir)
		return std::string(pw->pw_dir);
	return std::string();
}
#endif

/*
 *  Returns the per-user directory for step 3, without creating it.
 *  kind: prefs or ROM; on Linux both live directly in $HOME.
 *  Returns an empty string when the base location cannot be determined.
 */
static std::string user_dir(HostPathKind kind)
{
	(void)kind;
#if defined(_WIN32)
	const char *appdata = getenv("APPDATA");
	if (appdata == NULL || appdata[0] == '\0')
		return std::string();
	return join_path(appdata, "CockatriceIII");
#elif defined(__APPLE__)
	std::string home = home_dir();
	if (home.empty())
		return home;
	return join_path(home, "Library/CockatriceIII");
#else
	return home_dir();
#endif
}

/*
 *  Returns the file name used in the per-user directory.
 *  Linux keeps prefs as a dotfile in $HOME; every other case uses `name`.
 */
static std::string user_file_name(HostPathKind kind, const char *name)
{
#if !defined(_WIN32) && !defined(__APPLE__)
	if (kind == HOST_PATH_PREFS)
		return std::string(".") + name;
#endif
	(void)kind;
	return std::string(name);
}

/*
 *  Returns the per-user (step 3) path for `name`, creating the containing
 *  directory if needed so the caller can write there (used by SavePrefs).
 *  kind: prefs or ROM.  name: bare file name.
 *  Returns an empty string when no per-user location is available.
 */
std::string HostPaths_UserPath(HostPathKind kind, const char *name)
{
	std::string dir = user_dir(kind);
	if (dir.empty())
		return dir;
	// Create the directory; an existing one makes mkdir fail harmlessly.
#ifdef _WIN32
	_mkdir(dir.c_str());
#else
	mkdir(dir.c_str(), 0755);
#endif
	return join_path(dir, user_file_name(kind, name).c_str());
}

/*
 *  Returns every candidate path for `name`, in search order (see header).
 *  kind: prefs or ROM.  name: relative file name or path; an absolute path
 *  is returned alone since there is nothing to search.
 */
std::vector<std::string> HostPaths_SearchList(HostPathKind kind, const char *name)
{
	std::vector<std::string> list;
	if (name == NULL || name[0] == '\0')
		return list;
	if (HostPaths_IsAbsolute(name)) {
		list.push_back(name);
		return list;
	}

	// 1. Beside the executable.
	std::string exe = executable_dir();
	if (!exe.empty())
		list.push_back(join_path(exe, name));

#ifdef __APPLE__
	// 2. Contents/Resources, but only when the executable really sits in
	// <App>.app/Contents/MacOS; a bare build-tree binary has no bundle.
	const char *suffix = "/Contents/MacOS";
	size_t slen = strlen(suffix);
	if (exe.size() > slen && exe.compare(exe.size() - slen, slen, suffix) == 0) {
		std::string contents = exe.substr(0, exe.size() - strlen("/MacOS"));
		list.push_back(join_path(contents + "/Resources", name));
	}
#endif

	// 3. The per-user location.
	std::string user = user_dir(kind);
	if (!user.empty())
		list.push_back(join_path(user, user_file_name(kind, name).c_str()));

	return list;
}

/*
 *  Finds the first candidate for `name` that exists as a regular file.
 *  kind: prefs or ROM.  name: file name or path.
 *  found: receives the winning path on success.
 *  Returns true if one was found.
 */
bool HostPaths_FindExisting(HostPathKind kind, const char *name, std::string &found)
{
	std::vector<std::string> list = HostPaths_SearchList(kind, name);
	for (size_t i = 0; i < list.size(); i++) {
		struct stat st;
		// stat() rather than fopen() so a directory of the same name is skipped.
		if (stat(list[i].c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG) {
			found = list[i];
			return true;
		}
	}
	return false;
}
