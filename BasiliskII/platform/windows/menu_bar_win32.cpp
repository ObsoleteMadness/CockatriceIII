/*
 *  menu_bar_win32.cpp - Windows (Win32) native menu bar and dialogs
 *
 *  Cockatrice III
 *
 *  This file holds no opinion about what the menu bar contains: the menus,
 *  their order, their labels and their actions all come from the shared model
 *  in SDL/menu_model.cpp, so the Windows build cannot drift away from macOS the
 *  way it did when each host spelled its own bar out by hand. All that lives
 *  here is the translation into HMENU calls, the WM_COMMAND hook, and the
 *  native file dialog.
 */

#if defined(WIN32) || defined(_WIN32)

#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>

#include "menu_bar.h"
#include "menu_model.h"

static HWND    g_hwnd = NULL;
static HMENU   g_hmenu = NULL;
static WNDPROC g_prev_wndproc = NULL;
/* Model revision the current HMENU tree was built from. A bump means rows
   appeared or vanished and the bar has to be rebuilt rather than retitled. */
static unsigned g_modelRevision = 0;

/*
 *  Convert one of the model's UTF-8 labels to UTF-16 for the wide menu APIs.
 *
 *  The ANSI entry points would mangle the multiplication sign in the Video menu
 *  and any non-ASCII character in a disk image's file name, so every label goes
 *  through here and through the *W* variants.
 *
 *  Arguments:
 *    utf8    : NUL-terminated UTF-8 text from the model.
 *    out     : destination buffer.
 *    out_chars: its size in WCHARs; the result is always NUL terminated.
 */
static void label_to_wide(const char *utf8, WCHAR *out, int out_chars)
{
	if (!utf8)
		utf8 = "";
	int written = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, out_chars);
	if (written <= 0) {
		/* Malformed UTF-8 should never reach here, but a menu with an empty
		   caption beats a menu built from uninitialised stack. */
		out[0] = L'\0';
	} else {
		out[out_chars - 1] = L'\0';
	}
}

/*
 *  Build one popup menu from a model node, recursing into submenus.
 *
 *  Arguments:
 *    hmenu: freshly created popup to fill.
 *    node : model node describing the rows.
 *
 *  Keyboard equivalents in the model are ignored: honouring them would need a
 *  HACCEL translated in the message loop, and SDL 1.2 owns that loop.
 */
static void populate_menu_from_model(HMENU hmenu, const MenuNode *node)
{
	WCHAR wlabel[MENU_LABEL_MAX * 2];

	for (int i = 0; i < node->item_count; i++) {
		const MenuItem *desc = &node->items[i];

		if (desc->kind == MENU_ITEM_SEPARATOR) {
			AppendMenuW(hmenu, MF_SEPARATOR, 0, NULL);
			continue;
		}

		label_to_wide(desc->label, wlabel, MENU_LABEL_MAX * 2);

		if (desc->kind == MENU_ITEM_SUBMENU && desc->submenu) {
			HMENU hsub = CreatePopupMenu();
			populate_menu_from_model(hsub, desc->submenu);
			AppendMenuW(hmenu, MF_POPUP, (UINT_PTR)hsub, wlabel);
			continue;
		}

		/* The command id is the model's, so WM_COMMAND can be handed straight
		   back to MenuModel_Invoke() with no table of our own. */
		AppendMenuW(hmenu, MF_STRING, (UINT_PTR)desc->command_id, wlabel);
		EnableMenuItem(hmenu, (UINT)desc->command_id,
		               MF_BYCOMMAND | (desc->enabled ? MF_ENABLED : MF_GRAYED));
	}
}

/*
 *  Restate labels and enable flags on an already-built popup.
 *
 *  Rows are matched by position, which is sound because the same model node
 *  created them; a mismatch in count means the caller should have rebuilt, so
 *  this bails out rather than corrupting the menu.
 *
 *  Arguments:
 *    hmenu: popup previously built from node.
 *    node : model node to read the new labels and flags from.
 */
static void sync_menu_from_model(HMENU hmenu, const MenuNode *node)
{
	WCHAR wlabel[MENU_LABEL_MAX * 2];

	if (GetMenuItemCount(hmenu) != node->item_count)
		return;

	for (int i = 0; i < node->item_count; i++) {
		const MenuItem *desc = &node->items[i];
		if (desc->kind == MENU_ITEM_SEPARATOR)
			continue;

		label_to_wide(desc->label, wlabel, MENU_LABEL_MAX * 2);

		if (desc->kind == MENU_ITEM_SUBMENU && desc->submenu) {
			/* ModifyMenu on a popup row needs the existing submenu handle
			   passed back in, or the row loses its children. */
			HMENU hsub = GetSubMenu(hmenu, i);
			ModifyMenuW(hmenu, (UINT)i, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hsub, wlabel);
			if (hsub)
				sync_menu_from_model(hsub, desc->submenu);
			continue;
		}

		ModifyMenuW(hmenu, (UINT)i, MF_BYPOSITION | MF_STRING,
		            (UINT_PTR)desc->command_id, wlabel);
		EnableMenuItem(hmenu, (UINT)desc->command_id,
		               MF_BYCOMMAND | (desc->enabled ? MF_ENABLED : MF_GRAYED));
	}
}

/*
 *  Create the whole menu bar from the model and attach it to the window,
 *  discarding any bar built from an earlier model revision.
 */
static void rebuild_menu_bar(void)
{
	if (!g_hwnd)
		return;

	const MenuBarModel *model = MenuModel_Get();
	HMENU hbar = CreateMenu();
	WCHAR wlabel[MENU_LABEL_MAX * 2];

	for (int i = 0; i < model->menu_count; i++) {
		const MenuNode *node = &model->menus[i];
		HMENU hmenu = CreatePopupMenu();
		populate_menu_from_model(hmenu, node);
		label_to_wide(node->label, wlabel, MENU_LABEL_MAX * 2);
		AppendMenuW(hbar, MF_POPUP, (UINT_PTR)hmenu, wlabel);
	}

	HMENU old = g_hmenu;
	g_hmenu = hbar;
	SetMenu(g_hwnd, g_hmenu);
	/* SetMenu has already detached the old bar, so destroying it now frees its
	   popups without pulling the rug out from under the window. */
	if (old)
		DestroyMenu(old);

	g_modelRevision = MenuModel_Revision();
}

/*
 *  Window procedure hook that turns a menu click into a model action.
 *
 *  Only ids the model recognises are consumed; anything else falls through to
 *  SDL's own window procedure, which still owns this window.
 */
static LRESULT CALLBACK MenuSubclassWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_COMMAND) {
		int id = LOWORD(wParam);
		if (MenuModel_FindItem(id)) {
			MenuModel_Invoke(id);
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

	/* VideoInit() has already queried the desktop and rebuilt the presets by
	   the time SDL hands us a window, so the Video menu is generated from the
	   real screen size without asking Win32 for it. */
	rebuild_menu_bar();
	AdjustWindowForMenu(g_hwnd);

	MenuBar_UpdateAll();
}

void MenuBar_UpdateAll(void)
{
	if (!g_hwnd || !g_hmenu)
		return;

	/* A true return, or a revision the bar was not built from, means rows
	   changed shape and retitling in place would not be enough. */
	const bool structural = MenuModel_Refresh() || MenuModel_Revision() != g_modelRevision;
	if (structural) {
		rebuild_menu_bar();
	} else {
		const MenuBarModel *model = MenuModel_Get();
		for (int i = 0; i < model->menu_count; i++) {
			HMENU hmenu = GetSubMenu(g_hmenu, i);
			if (hmenu)
				sync_menu_from_model(hmenu, &model->menus[i]);
		}
	}

	DrawMenuBar(g_hwnd);
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

#endif /* WIN32 */
