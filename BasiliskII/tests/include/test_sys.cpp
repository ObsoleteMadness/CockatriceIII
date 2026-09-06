/*
 * test_sys.cpp - Minimal POSIX Sys_* so disk.cpp can attach temp images
 *
 * The full SDL sys_unix_sdl.cpp pulls mount tables, CD-ROM ioctls, and
 * GetString warnings. Tests only need open/read/write/size on a regular file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#include "sysdeps.h"
#include "sys.h"
#include "macos_util.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

struct test_file_handle {
	int fd;
	bool read_only;
	loff_t start_byte;
	loff_t file_size;
};

void SysInit(void) {}
void SysExit(void) {}

void *Sys_open(const char *name, bool read_only)
{
	int flags = (read_only ? O_RDONLY : O_RDWR) | O_BINARY;
	int fd = open(name, flags);
	if (fd < 0 && !read_only) {
		read_only = true;
		fd = open(name, O_RDONLY | O_BINARY);
	}
	if (fd < 0)
		return NULL;

	test_file_handle *fh = new test_file_handle;
	fh->fd = fd;
	fh->read_only = read_only;
	fh->start_byte = 0;

	struct stat st;
	if (fstat(fd, &st) == 0)
		fh->file_size = st.st_size;
	else
		fh->file_size = lseek(fd, 0, SEEK_END);

	uint8 data[256];
	memset(data, 0, sizeof(data));
	lseek(fd, 0, SEEK_SET);
	read(fd, data, 256);
	FileDiskLayout(fh->file_size, data, fh->start_byte, fh->file_size);
	lseek(fd, 0, SEEK_SET);
	return fh;
}

void Sys_close(void *h)
{
	test_file_handle *fh = (test_file_handle *)h;
	if (!fh)
		return;
	close(fh->fd);
	delete fh;
}

size_t Sys_read(void *h, void *buffer, loff_t offset, size_t length)
{
	test_file_handle *fh = (test_file_handle *)h;
	if (!fh)
		return 0;
	if (lseek(fh->fd, (off_t)(fh->start_byte + offset), SEEK_SET) < 0)
		return 0;
	ssize_t n = read(fh->fd, buffer, length);
	return n < 0 ? 0 : (size_t)n;
}

size_t Sys_write(void *h, void *buffer, loff_t offset, size_t length)
{
	test_file_handle *fh = (test_file_handle *)h;
	if (!fh || fh->read_only)
		return 0;
	if (lseek(fh->fd, (off_t)(fh->start_byte + offset), SEEK_SET) < 0)
		return 0;
	ssize_t n = write(fh->fd, buffer, length);
	return n < 0 ? 0 : (size_t)n;
}

loff_t SysGetFileSize(void *h)
{
	test_file_handle *fh = (test_file_handle *)h;
	return fh ? fh->file_size : 0;
}

void SysEject(void *h) { (void)h; }
bool SysFormat(void *h) { (void)h; return true; }
bool SysIsReadOnly(void *h)
{
	test_file_handle *fh = (test_file_handle *)h;
	return fh ? fh->read_only : true;
}
bool SysIsFixedDisk(void *h) { (void)h; return true; }
bool SysIsDiskInserted(void *h) { (void)h; return true; }
void SysPreventRemoval(void *h) { (void)h; }
void SysAllowRemoval(void *h) { (void)h; }

bool SysCDReadTOC(void *h, uint8 *toc) { (void)h; (void)toc; return false; }
bool SysCDGetPosition(void *h, uint8 *pos) { (void)h; (void)pos; return false; }
bool SysCDPlay(void *h, uint8 a, uint8 b, uint8 c, uint8 d, uint8 e, uint8 f)
{
	(void)h; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
	return false;
}
bool SysCDPause(void *h) { (void)h; return false; }
bool SysCDResume(void *h) { (void)h; return false; }
bool SysCDStop(void *h, uint8 a, uint8 b, uint8 c)
{
	(void)h; (void)a; (void)b; (void)c;
	return false;
}
bool SysCDScan(void *h, uint8 a, uint8 b, uint8 c, bool rev)
{
	(void)h; (void)a; (void)b; (void)c; (void)rev;
	return false;
}
void SysCDSetVolume(void *h, uint8 left, uint8 right)
{
	(void)h;
	(void)left;
	(void)right;
}
void SysCDGetVolume(void *h, uint8 &left, uint8 &right)
{
	(void)h;
	left = 0;
	right = 0;
}
