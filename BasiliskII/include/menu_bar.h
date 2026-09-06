/*
 *  menu_bar.h - OS Agnostic Menu Bar and Actions
 *
 *  Cockatrice III
 *
 *  All MenuAction_* functions are safe to call from any thread (UI, macOS main,
 *  Win32 WM_COMMAND, GTK signal handler, etc.). They enqueue a command and trigger
 *  a CPU-level interrupt. The emulation thread drains the queue inside the IRQ
 *  handler (M68K_EMUL_OP_IRQ) where it is safe to touch 68k state.
 *
 *  The sole exception is MenuAction_ForcePoweroff(), which calls exit() and so
 *  is inherently safe from any thread.
 */

#ifndef MENU_BAR_H
#define MENU_BAR_H

#include "sysdeps.h"

/* -------------------------------------------------------------------------
 *  Menu command types (keep in sync with MenuQueue implementation)
 * ---------------------------------------------------------------------- */
typedef enum {
    MENU_CMD_NONE         = 0,
    MENU_CMD_ZAP_PRAM     = 1,
    MENU_CMD_RESET        = 2,
    MENU_CMD_SHUTDOWN     = 3,
    MENU_CMD_ADD_FLOPPY   = 4,  /* path in cmd.path */
    MENU_CMD_ATTACH_SCSI       = 5,  /* id in cmd.param, path in cmd.path */
    MENU_CMD_DETACH_SCSI       = 6,  /* id in cmd.param */
    MENU_CMD_SAVE_CONFIG       = 7,
    MENU_CMD_GUEST_MENU_SELECT = 8,  /* menuID in cmd.param, itemIndex in cmd.param2 */
    MENU_CMD_GUEST_WINDOW_CLOSE = 9, /* WindowPtr in cmd.param */
    MENU_CMD_GUEST_DIALOG_CLICK = 10, /* WindowPtr in cmd.param, item index in cmd.param2 */
    MENU_CMD_GUEST_WINDOW_SELECT = 11, /* WindowPtr in cmd.param */
    MENU_CMD_GUEST_WINDOW_RESIZE = 12, /* WindowPtr in cmd.param, w/h in param2/param3 */
    MENU_CMD_GUEST_INPUT         = 13, /* WindowPtr in cmd.param, kind/x/y/code in param2..param5 */
    MENU_CMD_GUEST_WINDOW_MOVE   = 14, /* WindowPtr in cmd.param, x/y in param2/param3 */
    MENU_CMD_SET_VIDEO_MODE      = 15, /* width in cmd.param, height in cmd.param2 */
} MenuCmdType;

#define MENU_CMD_PATH_MAX 1024

typedef struct {
    MenuCmdType type;
    int         param;                   /* scsi id, menuID, or WindowPtr */
    int         param2;                  /* itemIndex, width, or input kind */
    int         param3;                  /* height, or input x */
    int         param4;                  /* input y */
    int         param5;                  /* input key code */
    char        path[MENU_CMD_PATH_MAX]; /* file path, or "" */
} MenuCmd;

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 *  Thread-safe queue used to pass menu commands to the emulation thread.
 *  Post() is called by the UI thread; Drain() is called by the CPU thread
 *  inside M68K_EMUL_OP_IRQ.
 * ---------------------------------------------------------------------- */
void MenuQueue_Post(const MenuCmd *cmd);  /* enqueue a command (UI thread) */
void MenuQueue_Drain(void);               /* execute pending commands (CPU thread) */
void MenuQueue_Reset(void);               /* clear queue on machine reset (CPU thread) */

/* -------------------------------------------------------------------------
 *  High-level OS-agnostic menu action entry points (UI thread).
 *  These open native file dialogs if needed, build a MenuCmd, and call
 *  MenuQueue_Post().
 * ---------------------------------------------------------------------- */
void MenuAction_SaveConfig(void);
void MenuAction_ZapPRAM(void);
void MenuAction_ResetMachine(void);
void MenuAction_Shutdown(void);
void MenuAction_ForcePoweroff(void);   /* calls exit() directly — safe from any thread */
void MenuAction_AddFloppy(void);
void MenuAction_AttachSCSI(int id);
void MenuAction_DetachSCSI(int id);
void MenuAction_GuestMenuSelect(int menuID, int itemIndex);
void MenuAction_GuestWindowClose(int windowPtr);   /* ask the guest to close a mirrored window */
void MenuAction_GuestDialogClick(int windowPtr, int itemIndex); /* press a dialog item */
void MenuAction_GuestWindowSelect(int windowPtr);              /* focus follows the host window */
void MenuAction_GuestWindowResize(int windowPtr, int w, int h);/* host resize drives the guest */
void MenuAction_GuestWindowMove(int windowPtr, int x, int y);  /* host drag drives the guest */
void MenuAction_GuestInput(int windowPtr, int kind, int x, int y, int code); /* mouse and keys */
void MenuAction_SetVideoMode(int width, int height); /* host Video menu / preset */

/*
 * Preset accessors for the host Video menu. Forward VideoPresets so platform
 * menu code does not need to include video.h.
 */
int Menu_VideoPresetCount(void);
bool Menu_VideoPresetAt(int index, int *width, int *height);

/* -------------------------------------------------------------------------
 *  Platform-specific menu bar initialization and updating
 * ---------------------------------------------------------------------- */
void MenuBar_Init(void *native_window_handle);
void MenuBar_UpdateAll(void);

/* -------------------------------------------------------------------------
 *  Cross-platform native open file dialog (runs on UI thread)
 * ---------------------------------------------------------------------- */
bool MenuBar_ShowOpenFileDialog(const char *title, const char *filter_desc, const char *filter_exts, char *out_path, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* MENU_BAR_H */
