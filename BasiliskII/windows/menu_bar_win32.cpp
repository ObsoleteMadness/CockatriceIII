/*
 *  menu_bar_win32.cpp - Windows (Win32) Native Menu Bar and Dialogs
 *
 *  Cockatrice III
 */

#ifdef WIN32

#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>

#include "sysdeps.h"
#include "menu_bar.h"
#include "scsi.h"

#define IDM_SAVE_CONFIG     1000
#define IDM_ZAP_PRAM        1001
#define IDM_RESET_MACHINE   1002
#define IDM_SHUTDOWN        1003
#define IDM_FORCE_POWEROFF  1004
#define IDM_ADD_FLOPPY      1005
#define IDM_SCSI_ATTACH_BASE 1010 // 1010..1016
#define IDM_SCSI_DETACH_BASE 1020 // 1020..1026

static HWND g_hwnd = NULL;
static HMENU g_hmenu = NULL;
static HMENU g_hdiskMenu = NULL;
static HMENU g_hscsiSubmenus[7] = {NULL};
static WNDPROC g_prev_wndproc = NULL;

static LRESULT CALLBACK MenuSubclassWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_COMMAND) {
		int id = LOWORD(wParam);
		if (id == IDM_SAVE_CONFIG) {
			MenuAction_SaveConfig();
			return 0;
		} else if (id == IDM_ZAP_PRAM) {
			MenuAction_ZapPRAM();
			return 0;
		} else if (id == IDM_RESET_MACHINE) {
			MenuAction_ResetMachine();
			return 0;
		} else if (id == IDM_SHUTDOWN) {
			MenuAction_Shutdown();
			return 0;
		} else if (id == IDM_FORCE_POWEROFF) {
			MenuAction_ForcePoweroff();
			return 0;
		} else if (id == IDM_ADD_FLOPPY) {
			MenuAction_AddFloppy();
			return 0;
		} else if (id >= IDM_SCSI_ATTACH_BASE && id < IDM_SCSI_ATTACH_BASE + 7) {
			MenuAction_AttachSCSI(id - IDM_SCSI_ATTACH_BASE);
			return 0;
		} else if (id >= IDM_SCSI_DETACH_BASE && id < IDM_SCSI_DETACH_BASE + 7) {
			MenuAction_DetachSCSI(id - IDM_SCSI_DETACH_BASE);
			return 0;
		}
	}
	if (g_prev_wndproc) {
		return CallWindowProc(g_prev_wndproc, hwnd, msg, wParam, lParam);
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

/*
 *  Adjust outer window size so that adding the menu bar does not shrink
 *  the SDL client drawing surface.
 */
static void AdjustWindowForMenu(HWND hwnd)
{
	RECT rcClient;
	if (!GetClientRect(hwnd, &rcClient))
		return;

	DWORD style = (DWORD)GetWindowLong(hwnd, GWL_STYLE);
	DWORD exStyle = (DWORD)GetWindowLong(hwnd, GWL_EXSTYLE);
	RECT rcAdjust = rcClient;

	AdjustWindowRectEx(&rcAdjust, style, TRUE /* bMenu */, exStyle);
	int newWidth = rcAdjust.right - rcAdjust.left;
	int newHeight = rcAdjust.bottom - rcAdjust.top;

	SetWindowPos(hwnd, NULL, 0, 0, newWidth, newHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

void MenuBar_Init(void *native_window_handle)
{
	if (!native_window_handle)
		return;

	g_hwnd = (HWND)native_window_handle;

	// Subclass window procedure to capture WM_COMMAND
	g_prev_wndproc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)MenuSubclassWndProc);

	// Create top-level Menu Bar
	g_hmenu = CreateMenu();

	// File Menu
	HMENU hFileMenu = CreatePopupMenu();
	AppendMenuA(hFileMenu, MF_STRING, IDM_SAVE_CONFIG, "Save Configuration");
	AppendMenuA(hFileMenu, MF_SEPARATOR, 0, NULL);
	AppendMenuA(hFileMenu, MF_STRING, IDM_ZAP_PRAM, "Zap PRAM");
	AppendMenuA(hFileMenu, MF_SEPARATOR, 0, NULL);
	AppendMenuA(hFileMenu, MF_STRING, IDM_RESET_MACHINE, "Reset Machine");
	AppendMenuA(hFileMenu, MF_STRING, IDM_SHUTDOWN, "Shutdown");
	AppendMenuA(hFileMenu, MF_STRING, IDM_FORCE_POWEROFF, "Forced Poweroff and Exit");
	AppendMenuA(g_hmenu, MF_POPUP, (UINT_PTR)hFileMenu, "&File");

	// Disk Menu
	g_hdiskMenu = CreatePopupMenu();
	AppendMenuA(g_hdiskMenu, MF_STRING, IDM_ADD_FLOPPY, "Add Floppy...");
	AppendMenuA(g_hdiskMenu, MF_SEPARATOR, 0, NULL);

	// SCSI 0..6 Submenus
	for (int i = 0; i < 7; i++) {
		g_hscsiSubmenus[i] = CreatePopupMenu();
		AppendMenuA(g_hscsiSubmenus[i], MF_STRING, IDM_SCSI_ATTACH_BASE + i, "Attach...");
		AppendMenuA(g_hscsiSubmenus[i], MF_STRING, IDM_SCSI_DETACH_BASE + i, "Detach");

		char label[64];
		sprintf(label, "SCSI %d", i);
		AppendMenuA(g_hdiskMenu, MF_POPUP, (UINT_PTR)g_hscsiSubmenus[i], label);
	}
	AppendMenuA(g_hmenu, MF_POPUP, (UINT_PTR)g_hdiskMenu, "&Disk");

	// Set menu and compensate client height
	SetMenu(g_hwnd, g_hmenu);
	AdjustWindowForMenu(g_hwnd);

	MenuBar_UpdateAll();
}

void MenuBar_UpdateAll(void)
{
	if (!g_hdiskMenu)
		return;

	for (int i = 0; i < 7; i++) {
		bool present = false, cdrom = false;
		char path[1024] = {0};
		SCSI_GetDeviceInfo(i, &present, &cdrom, path, sizeof(path));

		char label[128];
		if (present && path[0] != '\0') {
			const char *filename = strrchr(path, '\\');
			if (!filename)
				filename = strrchr(path, '/');
			filename = filename ? filename + 1 : path;
			sprintf(label, "SCSI %d: %s (%s)", i, filename, cdrom ? "CD-ROM" : "HDD");
			if (g_hscsiSubmenus[i]) {
				EnableMenuItem(g_hscsiSubmenus[i], IDM_SCSI_DETACH_BASE + i, MF_BYCOMMAND | MF_ENABLED);
			}
		} else {
			sprintf(label, "SCSI %d (Empty)", i);
			if (g_hscsiSubmenus[i]) {
				EnableMenuItem(g_hscsiSubmenus[i], IDM_SCSI_DETACH_BASE + i, MF_BYCOMMAND | MF_GRAYED);
			}
		}

		// Modify submenu label in Disk Menu (position index: 2 + i)
		ModifyMenuA(g_hdiskMenu, 2 + i, MF_BYPOSITION | MF_POPUP, (UINT_PTR)g_hscsiSubmenus[i], label);
	}

	if (g_hwnd) {
		DrawMenuBar(g_hwnd);
	}
}

bool MenuBar_ShowOpenFileDialog(const char *title, const char *filter_desc, const char *filter_exts, char *out_path, size_t max_len)
{
	OPENFILENAMEA ofn;
	char szFile[1024] = {0};
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = g_hwnd;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile);
	ofn.lpstrTitle = title;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

	char filter[512] = {0};
	int pos = 0;
	pos += sprintf(filter + pos, "%s", filter_desc ? filter_desc : "All Files (*.*)");
	filter[pos++] = '\0';
	pos += sprintf(filter + pos, "%s", filter_exts ? filter_exts : "*.*");
	filter[pos++] = '\0';
	filter[pos++] = '\0';
	ofn.lpstrFilter = filter;

	if (GetOpenFileNameA(&ofn)) {
		if (out_path && max_len > 0) {
			strncpy(out_path, szFile, max_len - 1);
			out_path[max_len - 1] = '\0';
			return true;
		}
	}
	return false;
}

#endif // WIN32
