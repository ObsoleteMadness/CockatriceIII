/*
 *  menu_model.cpp - The one definition of the Cockatrice III host menu bar
 *
 *  Cockatrice III
 *
 *  Everything a menu bar is made of -- which menus exist, their order, their
 *  titles, their keyboard equivalents, which rows gray out and what each row
 *  does -- is decided here and nowhere else. platform/darwin, platform/windows
 *  and platform/linux only translate this description into native objects.
 *
 *  The model lives in a fixed static pool rather than the heap: it is small,
 *  bounded, built once, and refreshed in place, so hosts can hold MenuNode and
 *  MenuItem pointers across a refresh without worrying about reallocation.
 *  Only MenuModel_SetHostScreenSize() with a new screen size can change how
 *  many rows exist, and that is reported through MenuModel_Refresh() and
 *  MenuModel_Revision() so a host knows to repopulate.
 */

#include <stdio.h>
#include <string.h>

#include "sysdeps.h"
#include "scsi.h"
#include "video.h"
#include "menu_bar.h"
#include "menu_model.h"

#define DEBUG 0
#include "debug.h"

/* SCSI ids Cockatrice exposes; id 7 is the host adapter and is never shown. */
#define MENU_SCSI_ID_COUNT 7

/* =========================================================================
 *  Static storage pool
 *
 *  s_menus[i] owns s_items[i][]; each submenu owns a slice of s_subitems.
 *  Nothing here is freed, and the addresses never move once built.
 * ====================================================================== */

static MenuNode     s_menus[MENU_MAX_MENUS];
static MenuItem     s_items[MENU_MAX_MENUS][MENU_MAX_ITEMS];
static MenuNode     s_submenus[MENU_MAX_SUBMENUS];
static MenuItem     s_subitems[MENU_MAX_SUBMENUS][MENU_MAX_ITEMS];
static int          s_submenu_count = 0;

static MenuBarModel s_model = { 0, s_menus };
static bool         s_built = false;
static unsigned     s_revision = 0;
static int          s_next_command_id = MENU_COMMAND_ID_BASE;

/* Host desktop size, if a host told us one before the first build. Zero means
   "use whatever presets video.cpp already has", which is the case on hosts
   that build the menu bar after VideoInit() has queried SDL. */
static int s_host_screen_width = 0;
static int s_host_screen_height = 0;

/* Direct index to the rows whose text or enable state changes at runtime, so a
   refresh does not have to search the tree for them. */
static MenuItem *s_scsi_parent_item[MENU_SCSI_ID_COUNT];   /* "SCSI n: image" */
static MenuItem *s_scsi_detach_item[MENU_SCSI_ID_COUNT];   /* grayed when empty */
static MenuNode *s_video_menu = NULL;

/* =========================================================================
 *  Model construction helpers
 * ====================================================================== */

/*
 *  Copy a UTF-8 label into a fixed model buffer, always NUL terminating.
 *
 *  Arguments:
 *    dst: MENU_LABEL_MAX byte destination inside the model.
 *    src: source text; NULL is treated as the empty string.
 */
static void set_label(char *dst, const char *src)
{
	if (!src)
		src = "";
	strncpy(dst, src, MENU_LABEL_MAX - 1);
	dst[MENU_LABEL_MAX - 1] = '\0';
}

/*
 *  Start a new top-level menu in the pool.
 *
 *  Arguments:
 *    title: menu bar caption, UTF-8.
 *
 *  Returns the new (empty) menu, or NULL if MENU_MAX_MENUS is exhausted.
 */
static MenuNode *begin_menu(const char *title)
{
	if (s_model.menu_count >= MENU_MAX_MENUS) {
		printf("MenuModel: MENU_MAX_MENUS (%d) exceeded, dropping menu \"%s\"\n",
		       MENU_MAX_MENUS, title ? title : "");
		return NULL;
	}
	MenuNode *menu = &s_menus[s_model.menu_count];
	set_label(menu->label, title);
	menu->items = s_items[s_model.menu_count];
	menu->item_count = 0;
	s_model.menu_count++;
	return menu;
}

/*
 *  Append a blank row to a menu and return it, or NULL when the menu is full.
 *  Callers fill in the fields they care about; everything else is zeroed here
 *  so no row is ever left with stale data from a previous build.
 */
static MenuItem *append_row(MenuNode *menu)
{
	if (!menu)
		return NULL;
	if (menu->item_count >= MENU_MAX_ITEMS) {
		printf("MenuModel: MENU_MAX_ITEMS (%d) exceeded in menu \"%s\"\n",
		       MENU_MAX_ITEMS, menu->label);
		return NULL;
	}
	MenuItem *item = &menu->items[menu->item_count++];
	memset(item, 0, sizeof(*item));
	item->enabled = true;
	return item;
}

/*
 *  Append a clickable row bound to an action.
 *
 *  Arguments:
 *    menu    : menu to append to.
 *    title   : UTF-8 label.
 *    action  : what the row does when invoked.
 *    arg     : the action's argument (SCSI id, packed video mode, or 0).
 *    shortcut: lowercase ASCII key equivalent, or 0 for none.
 *
 *  Returns the row so the caller can record it for later refreshes.
 */
static MenuItem *append_command(MenuNode *menu, const char *title,
                                MenuActionId action, int arg, char shortcut)
{
	MenuItem *item = append_row(menu);
	if (!item)
		return NULL;
	item->kind       = MENU_ITEM_COMMAND;
	item->action     = action;
	item->arg        = arg;
	item->command_id = s_next_command_id++;
	item->shortcut   = shortcut;
	set_label(item->label, title);
	return item;
}

/*
 *  Append a separator rule.
 */
static void append_separator(MenuNode *menu)
{
	MenuItem *item = append_row(menu);
	if (item)
		item->kind = MENU_ITEM_SEPARATOR;
}

/*
 *  Append a row that opens a submenu, allocating the submenu from the pool.
 *
 *  Arguments:
 *    menu : parent menu.
 *    title: label shown on the parent row and used as the submenu's own title.
 *
 *  Returns the parent row (whose ->submenu is the new menu), or NULL if the
 *  pool is exhausted; the caller must check before appending to ->submenu.
 */
static MenuItem *append_submenu(MenuNode *menu, const char *title)
{
	if (s_submenu_count >= MENU_MAX_SUBMENUS) {
		printf("MenuModel: MENU_MAX_SUBMENUS (%d) exceeded, dropping \"%s\"\n",
		       MENU_MAX_SUBMENUS, title ? title : "");
		return NULL;
	}
	MenuItem *item = append_row(menu);
	if (!item)
		return NULL;

	MenuNode *sub = &s_submenus[s_submenu_count];
	set_label(sub->label, title);
	sub->items = s_subitems[s_submenu_count];
	sub->item_count = 0;
	s_submenu_count++;

	item->kind    = MENU_ITEM_SUBMENU;
	item->submenu = sub;
	set_label(item->label, title);
	return item;
}

/* =========================================================================
 *  Dynamic label and state computation
 * ====================================================================== */

/*
 *  Format the caption for one SCSI submenu from the drive's current state.
 *
 *  Arguments:
 *    id      : SCSI id 0..6.
 *    out     : destination buffer.
 *    out_size: its size in bytes.
 *    occupied: set to true when a device is attached, so the caller can decide
 *              whether "Detach" should be selectable.
 */
static void format_scsi_label(int id, char *out, size_t out_size, bool *occupied)
{
	bool present = false, cdrom = false;
	char path[1024] = {0};
	SCSI_GetDeviceInfo(id, &present, &cdrom, path, sizeof(path));

	if (present && path[0] != '\0') {
		/* Show the leaf file name only; check both separators so a Windows
		   path pasted into a prefs file on any host still shortens. */
		const char *filename = strrchr(path, '/');
		const char *backslash = strrchr(path, '\\');
		if (backslash && (!filename || backslash > filename))
			filename = backslash;
		filename = filename ? filename + 1 : path;
		snprintf(out, out_size, "SCSI %d: %s (%s)", id, filename, cdrom ? "CD-ROM" : "HDD");
		if (occupied)
			*occupied = true;
	} else {
		snprintf(out, out_size, "SCSI %d (Empty)", id);
		if (occupied)
			*occupied = false;
	}
}

/*
 *  Rewrite the SCSI submenu captions and Detach enable flags from the live
 *  drive table.
 *
 *  Returns true if any label or flag actually changed, so a host can skip a
 *  redraw when a refresh was a no-op.
 */
static bool refresh_scsi(void)
{
	bool changed = false;
	for (int i = 0; i < MENU_SCSI_ID_COUNT; i++) {
		char label[MENU_LABEL_MAX];
		bool occupied = false;
		format_scsi_label(i, label, sizeof(label), &occupied);

		MenuItem *parent = s_scsi_parent_item[i];
		if (parent) {
			if (strcmp(parent->label, label) != 0) {
				set_label(parent->label, label);
				/* The submenu carries the same title, so hosts that name the
				   native submenu rather than the parent row stay in step. */
				if (parent->submenu)
					set_label(parent->submenu->label, label);
				changed = true;
			}
		}
		MenuItem *detach = s_scsi_detach_item[i];
		if (detach && detach->enabled != occupied) {
			detach->enabled = occupied;
			changed = true;
		}
	}
	return changed;
}

/*
 *  Fill the Video menu with one row per video preset that fits the host screen.
 *
 *  video.cpp owns the preset list; this only turns it into menu rows. Returns
 *  true when the number of rows changed, which means hosts have to repopulate
 *  their native submenu rather than just retitle rows.
 */
static bool build_video_items(void)
{
	if (!s_video_menu)
		return false;

	const int previous_count = s_video_menu->item_count;
	char previous_labels[MENU_MAX_ITEMS][MENU_LABEL_MAX];
	for (int i = 0; i < previous_count && i < MENU_MAX_ITEMS; i++)
		memcpy(previous_labels[i], s_video_menu->items[i].label, MENU_LABEL_MAX);

	/* Rebuilding reuses the same command ids: the Video menu is the only part
	   of the bar that is rebuilt after startup, and rewinding the counter keeps
	   ids dense and stable for hosts that cached them for other menus. */
	int reclaim_from = 0;
	if (previous_count > 0)
		reclaim_from = s_video_menu->items[0].command_id;

	s_video_menu->item_count = 0;
	if (reclaim_from)
		s_next_command_id = reclaim_from;

	const int count = Menu_VideoPresetCount();
	for (int i = 0; i < count; i++) {
		int width = 0, height = 0;
		if (!Menu_VideoPresetAt(i, &width, &height))
			continue;
		char title[MENU_LABEL_MAX];
		/* U+00D7 MULTIPLICATION SIGN; every host renders the model's labels as
		   UTF-8, so this does not need an ASCII fallback. */
		snprintf(title, sizeof(title), "%d \xc3\x97 %d", width, height);
		/* Pack the mode into the single action argument so a host tag or a
		   WM_COMMAND id is all the context the callback needs. */
		append_command(s_video_menu, title, MENU_ACTION_SET_VIDEO_MODE,
		               ((width & 0xffff) << 16) | (height & 0xffff), 0);
	}

	if (s_video_menu->item_count != previous_count)
		return true;
	for (int i = 0; i < previous_count && i < MENU_MAX_ITEMS; i++) {
		if (strcmp(previous_labels[i], s_video_menu->items[i].label) != 0)
			return true;
	}
	return false;
}

/* =========================================================================
 *  Build
 * ====================================================================== */

/*
 *  Construct the whole menu bar. Called once, lazily, from MenuModel_Get().
 *
 *  This function is the menu bar: adding an item anywhere in Cockatrice means
 *  adding a line here and nothing else.
 */
static void build_model(void)
{
	s_model.menu_count = 0;
	s_submenu_count = 0;
	s_next_command_id = MENU_COMMAND_ID_BASE;
	memset(s_scsi_parent_item, 0, sizeof(s_scsi_parent_item));
	memset(s_scsi_detach_item, 0, sizeof(s_scsi_detach_item));
	s_video_menu = NULL;

	/* ---- File ---------------------------------------------------------- */
	MenuNode *file = begin_menu("File");
	append_command(file, "Save Configuration", MENU_ACTION_SAVE_CONFIG, 0, 's');
	append_separator(file);
	append_command(file, "Zap PRAM", MENU_ACTION_ZAP_PRAM, 0, 0);
	append_separator(file);
	append_command(file, "Reset Machine", MENU_ACTION_RESET_MACHINE, 0, 'r');
	append_command(file, "Shutdown", MENU_ACTION_SHUTDOWN, 0, 0);
	append_command(file, "Forced Poweroff and Exit", MENU_ACTION_FORCE_POWEROFF, 0, 0);

	/* ---- Disk ---------------------------------------------------------- */
	MenuNode *disk = begin_menu("Disk");
	append_command(disk, "Add Floppy...", MENU_ACTION_ADD_FLOPPY, 0, 0);
	append_separator(disk);
	for (int i = 0; i < MENU_SCSI_ID_COUNT; i++) {
		char title[MENU_LABEL_MAX];
		format_scsi_label(i, title, sizeof(title), NULL);
		MenuItem *parent = append_submenu(disk, title);
		if (!parent)
			break;
		s_scsi_parent_item[i] = parent;
		append_command(parent->submenu, "Attach...", MENU_ACTION_ATTACH_SCSI, i, 0);
		s_scsi_detach_item[i] = append_command(parent->submenu, "Detach",
		                                       MENU_ACTION_DETACH_SCSI, i, 0);
	}

	/* ---- Video --------------------------------------------------------- */
	/* The Video menu must stay last: it is the only menu rebuilt after startup,
	   and build_video_items() reissues its command ids by rewinding the id
	   counter, which is only safe while nothing was allocated after it. */
	s_video_menu = begin_menu("Video");
	build_video_items();

	s_built = true;

	/* Enable flags for the rows built above still have to reflect the drives
	   that prefs attached before the menu bar existed. */
	refresh_scsi();
}

/* =========================================================================
 *  Public entry points
 * ====================================================================== */

void MenuModel_SetHostScreenSize(int width, int height)
{
	if (width <= 0 || height <= 0)
		return;
	if (width == s_host_screen_width && height == s_host_screen_height)
		return;
	s_host_screen_width = width;
	s_host_screen_height = height;

	/* video.cpp decides which presets fit; the Video menu is just its output. */
	Video_BuildPresets(width, height);

	if (s_built && build_video_items())
		s_revision++;
}

const MenuBarModel *MenuModel_Get(void)
{
	if (!s_built)
		build_model();
	return &s_model;
}

bool MenuModel_Refresh(void)
{
	if (!s_built)
		build_model();

	const bool structural = build_video_items();
	refresh_scsi();
	if (structural)
		s_revision++;
	return structural;
}

unsigned MenuModel_Revision(void)
{
	return s_revision;
}

const MenuItem *MenuModel_FindItem(int command_id)
{
	if (command_id < MENU_COMMAND_ID_BASE)
		return NULL;
	if (!s_built)
		build_model();

	for (int m = 0; m < s_model.menu_count; m++) {
		const MenuNode *menu = &s_model.menus[m];
		for (int i = 0; i < menu->item_count; i++) {
			const MenuItem *item = &menu->items[i];
			if (item->kind == MENU_ITEM_COMMAND && item->command_id == command_id)
				return item;
			if (item->kind == MENU_ITEM_SUBMENU && item->submenu) {
				const MenuNode *sub = item->submenu;
				for (int j = 0; j < sub->item_count; j++) {
					if (sub->items[j].kind == MENU_ITEM_COMMAND &&
					    sub->items[j].command_id == command_id)
						return &sub->items[j];
				}
			}
		}
	}
	return NULL;
}

void MenuModel_Invoke(int command_id)
{
	const MenuItem *item = MenuModel_FindItem(command_id);
	if (!item)
		return;

	switch (item->action) {
	case MENU_ACTION_SAVE_CONFIG:
		MenuAction_SaveConfig();
		break;
	case MENU_ACTION_ZAP_PRAM:
		MenuAction_ZapPRAM();
		break;
	case MENU_ACTION_RESET_MACHINE:
		MenuAction_ResetMachine();
		break;
	case MENU_ACTION_SHUTDOWN:
		MenuAction_Shutdown();
		break;
	case MENU_ACTION_FORCE_POWEROFF:
		MenuAction_ForcePoweroff();
		break;
	case MENU_ACTION_ADD_FLOPPY:
		MenuAction_AddFloppy();
		break;
	case MENU_ACTION_ATTACH_SCSI:
		MenuAction_AttachSCSI(item->arg);
		break;
	case MENU_ACTION_DETACH_SCSI:
		MenuAction_DetachSCSI(item->arg);
		break;
	case MENU_ACTION_SET_VIDEO_MODE:
		/* Unpack the mode the model folded into the single action argument. */
		MenuAction_SetVideoMode((item->arg >> 16) & 0xffff, item->arg & 0xffff);
		break;
	case MENU_ACTION_NONE:
	default:
		break;
	}
}
