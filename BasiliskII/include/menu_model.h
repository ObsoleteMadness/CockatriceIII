/*
 *  menu_model.h - The host menu bar, described once for every platform
 *
 *  Cockatrice III
 *
 *  Every host used to spell its own menu bar out in native API calls, so the
 *  macOS, Windows and Linux ports drifted apart: macOS grew a Video menu that
 *  Windows never got, the SCSI label logic was written twice, and a new item
 *  meant three edits in three languages.
 *
 *  This header replaces that with a single declarative description. The model
 *  is built once in SDL/menu_model.cpp and is the only place menu titles,
 *  ordering, keyboard equivalents and enable rules are decided. Platform code
 *  does nothing but translate the description into NSMenu / HMENU / GtkMenu
 *  calls and hand the resulting command id back to MenuModel_Invoke().
 *
 *  Threading: the model is built and refreshed on whichever thread calls in
 *  (macOS main thread at startup, emulation thread via MenuBar_UpdateAll()).
 *  It is plain static data with no locking, so a host must not walk it while
 *  another thread refreshes it; in practice refreshes are rare (disk attach or
 *  detach) and every host marshals its own menu work onto its UI thread.
 *
 *  MenuModel_Invoke() only forwards to the MenuAction_* entry points in
 *  menu_bar.h, which are themselves safe to call from any thread.
 */

#ifndef MENU_MODEL_H
#define MENU_MODEL_H

#include "sysdeps.h"

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Longest menu or item label the model will ever produce, in bytes. SCSI
   labels embed a file name, so this has to be comfortably larger than a title
   like "SCSI 0: MacOS8.dsk (HDD)". */
#define MENU_LABEL_MAX 160

/* Storage limits for the static model pool. These are hard ceilings checked at
   build time by menu_model.cpp; raise them if the menu bar grows. */
#define MENU_MAX_MENUS      8   /* top-level menus, excluding host-owned ones */
#define MENU_MAX_ITEMS     32   /* items in any one menu */
#define MENU_MAX_SUBMENUS  16   /* submenus across the whole bar */

/* First command id handed out. Ids are dense from here and are unique across
   the whole menu bar, so a host can use them directly as Win32 WM_COMMAND ids
   or NSMenuItem tags without any mapping table of its own. */
#define MENU_COMMAND_ID_BASE 1000

/*
 *  What a row in a menu is. Separators and submenu parents carry no command
 *  id; only MENU_ITEM_COMMAND rows are ever passed to MenuModel_Invoke().
 */
typedef enum {
	MENU_ITEM_COMMAND = 0,
	MENU_ITEM_SEPARATOR,
	MENU_ITEM_SUBMENU,
} MenuItemKind;

/*
 *  The action a command row performs. MenuModel_Invoke() maps each of these
 *  onto exactly one MenuAction_* call; hosts never call MenuAction_* directly.
 */
typedef enum {
	MENU_ACTION_NONE = 0,
	MENU_ACTION_SAVE_CONFIG,
	MENU_ACTION_ZAP_PRAM,
	MENU_ACTION_RESET_MACHINE,
	MENU_ACTION_SHUTDOWN,
	MENU_ACTION_FORCE_POWEROFF,
	MENU_ACTION_ADD_FLOPPY,
	MENU_ACTION_ATTACH_SCSI,    /* arg = SCSI id 0..6 */
	MENU_ACTION_DETACH_SCSI,    /* arg = SCSI id 0..6 */
	MENU_ACTION_SET_VIDEO_MODE, /* arg = (width << 16) | height */
} MenuActionId;

typedef struct MenuNode MenuNode;

/*
 *  One row of a menu.
 *
 *  label    : UTF-8, already localized/formatted; hosts display it verbatim.
 *  shortcut : lowercase ASCII key to combine with the host's menu modifier
 *             (Command on macOS, Control elsewhere); 0 for no shortcut. A host
 *             that cannot install accelerators simply ignores it.
 *  enabled  : false means the host should gray the row out.
 *  arg      : the MenuActionId's argument, and a convenient host tag.
 */
typedef struct {
	MenuItemKind kind;
	MenuActionId action;
	int          arg;
	int          command_id;          /* non-zero for MENU_ITEM_COMMAND only */
	char         label[MENU_LABEL_MAX];
	char         shortcut;
	bool         enabled;
	MenuNode    *submenu;             /* MENU_ITEM_SUBMENU only, else NULL */
} MenuItem;

/*
 *  A menu: a title plus its rows. Used both for top-level menus (where the
 *  title is the menu bar caption) and for submenus.
 */
struct MenuNode {
	char      label[MENU_LABEL_MAX];
	int       item_count;
	MenuItem *items;
};

/*
 *  The whole Cockatrice-owned menu bar, in display order. Hosts append these
 *  after whatever menus the platform itself owns (the macOS application menu,
 *  for instance) and may add their own afterwards (the macOS Window menu).
 */
typedef struct {
	int       menu_count;
	MenuNode *menus;
} MenuBarModel;

/*
 *  Tell the model the size of the host desktop before it is first walked.
 *
 *  The Video menu is generated from the video presets that fit the screen, and
 *  on macOS the menu bar is built before SDL exists, so the host answers this
 *  from its own API (NSScreen) instead. Calling it after the model has been
 *  built rebuilds the Video menu, which MenuModel_Refresh() will then report as
 *  a structural change.
 *
 *  Arguments:
 *    width, height: host desktop size in window pixels; ignored if <= 0.
 */
void MenuModel_SetHostScreenSize(int width, int height);

/*
 *  Return the menu bar description, building it on first use.
 *  Never NULL. The returned pointers stay valid for the life of the process,
 *  but the contents are rewritten by MenuModel_Refresh().
 */
const MenuBarModel *MenuModel_Get(void);

/*
 *  Recompute every dynamic label and enable flag (SCSI occupancy, video
 *  presets).
 *
 *  Returns true when the shape of the menu bar changed -- a menu gained or lost
 *  rows -- meaning the host must repopulate its native menus rather than just
 *  restate titles. Returns false when only labels and enable flags moved, which
 *  a host can apply in place.
 */
bool MenuModel_Refresh(void);

/*
 *  Counter bumped every time MenuModel_Refresh() (or a late
 *  MenuModel_SetHostScreenSize()) changes the shape of the bar. A host that
 *  caches native menu objects can compare this against the value it last built
 *  from to decide whether to rebuild.
 */
unsigned MenuModel_Revision(void);

/*
 *  Look a command row up by the id the host was given. Returns NULL for an
 *  unknown id, which is how a host tells "this WM_COMMAND is not ours" from
 *  one of the model's own.
 */
const MenuItem *MenuModel_FindItem(int command_id);

/*
 *  Perform the action bound to a command id. Safe to call from any thread:
 *  every action either posts to the menu queue or exits the process. Unknown
 *  ids are ignored.
 */
void MenuModel_Invoke(int command_id);

#ifdef __cplusplus
}
#endif

#endif /* MENU_MODEL_H */
