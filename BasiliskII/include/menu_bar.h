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
    MENU_CMD_ATTACH_SCSI  = 5,  /* id in cmd.param, path in cmd.path */
    MENU_CMD_DETACH_SCSI  = 6,  /* id in cmd.param */
    MENU_CMD_SAVE_CONFIG  = 7,
} MenuCmdType;

#define MENU_CMD_PATH_MAX 1024

typedef struct {
    MenuCmdType type;
    int         param;                   /* scsi id, or 0 */
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
