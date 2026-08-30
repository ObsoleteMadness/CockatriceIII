/*
 *  menu_bar.cpp - OS Agnostic Menu Bar and Action Handlers
 *
 *  Cockatrice III
 *
 *  All MenuAction_* functions run on the UI thread.  They build a MenuCmd and
 *  call MenuQueue_Post(), which enqueues it in a lock-free SPSC ring buffer and
 *  calls TriggerInterrupt() to wake the emulation thread.
 *
 *  MenuQueue_Drain() is called by the emulation thread inside M68K_EMUL_OP_IRQ,
 *  where it is safe to touch all 68k emulator state (ADB, Sony, SCSI, XPRAM…).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "adb.h"
#include "sony.h"
#include "scsi.h"
#include "xpram.h"
#include "prefs.h"
#include "user_strings.h"
#include "menu_bar.h"

#define DEBUG 0
#include "debug.h"

/* =========================================================================
 *  Lock-free single-producer / single-consumer ring buffer
 *  UI thread writes (head), CPU thread reads (tail).
 *  Size must be a power of two.
 * ====================================================================== */

#define MENU_QUEUE_SIZE 16   /* must be power of 2 */
#define MENU_QUEUE_MASK (MENU_QUEUE_SIZE - 1)

static MenuCmd       s_queue[MENU_QUEUE_SIZE];
static volatile int  s_head = 0;   /* written by UI thread  */
static volatile int  s_tail = 0;   /* written by CPU thread */

/*
 *  MenuQueue_Post  [UI thread]
 *  Enqueue a command and trigger a CPU interrupt so the emulation thread
 *  wakes up to drain it.  Drops silently if the queue is full (should never
 *  happen at human interaction rates).
 */
void MenuQueue_Post(const MenuCmd *cmd)
{
    int next = (s_head + 1) & MENU_QUEUE_MASK;
    if (next == s_tail) {
        printf("MenuQueue: queue full, dropping command %d\n", cmd->type);
        fflush(stdout);
        return;
    }
    s_queue[s_head] = *cmd;
    /* Compiler barrier: ensure the write is visible before advancing head */
    __sync_synchronize();
    s_head = next;
    /* Wake the CPU thread */
    TriggerInterrupt();
}

/*
 *  MenuQueue_Drain  [CPU / emulation thread — called from M68K_EMUL_OP_IRQ]
 *  Execute all pending commands.  May call any emulation API safely.
 */
void MenuQueue_Drain(void)
{
    while (s_tail != s_head) {
        MenuCmd cmd = s_queue[s_tail];
        __sync_synchronize();
        s_tail = (s_tail + 1) & MENU_QUEUE_MASK;

        switch (cmd.type) {

        case MENU_CMD_SAVE_CONFIG:
            printf("MenuQueue: Saving Configuration\n");
            fflush(stdout);
            SavePrefs();
            break;

        case MENU_CMD_ZAP_PRAM:
            printf("MenuQueue: Zapping PRAM\n");
            fflush(stdout);
            memset(XPRAM, 0, sizeof(XPRAM));
            ZapPRAM();
            break;

        case MENU_CMD_RESET:
            printf("MenuQueue: Resetting Machine\n");
            fflush(stdout);
            Reset680x0();
            break;

        case MENU_CMD_SHUTDOWN:
            printf("MenuQueue: Requesting OS Shutdown\n");
            fflush(stdout);
            ADBKeyDown(0x7f);
            ADBKeyUp(0x7f);
            break;

        case MENU_CMD_ADD_FLOPPY:
            printf("MenuQueue: Inserting floppy: %s\n", cmd.path);
            fflush(stdout);
            if (!SonyInsertFloppy(cmd.path))
                ErrorAlert("Failed to insert floppy disk image.");
            MenuBar_UpdateAll();
            break;

        case MENU_CMD_ATTACH_SCSI:
            printf("MenuQueue: Attaching SCSI %d: %s\n", cmd.param, cmd.path);
            fflush(stdout);
            if (!SCSI_Attach(cmd.param, cmd.path)) {
                char err[128];
                sprintf(err, "Failed to attach image to SCSI ID %d.", cmd.param);
                ErrorAlert(err);
            }
            MenuBar_UpdateAll();
            break;

        case MENU_CMD_DETACH_SCSI:
            printf("MenuQueue: Detaching SCSI %d\n", cmd.param);
            fflush(stdout);
            SCSI_Detach(cmd.param);
            MenuBar_UpdateAll();
            break;

        default:
            break;
        }
    }
}

void MenuQueue_Reset(void)
{
    s_tail = s_head;
}

/* =========================================================================
 *  MenuAction_*  [UI thread — build and post commands]
 * ====================================================================== */

void MenuAction_SaveConfig(void)
{
    MenuCmd cmd = {MENU_CMD_SAVE_CONFIG, 0, ""};
    MenuQueue_Post(&cmd);
}

void MenuAction_ZapPRAM(void)
{
    MenuCmd cmd = {MENU_CMD_ZAP_PRAM, 0, ""};
    MenuQueue_Post(&cmd);
}

void MenuAction_ResetMachine(void)
{
    MenuCmd cmd = {MENU_CMD_RESET, 0, ""};
    MenuQueue_Post(&cmd);
}

void MenuAction_Shutdown(void)
{
    MenuCmd cmd = {MENU_CMD_SHUTDOWN, 0, ""};
    MenuQueue_Post(&cmd);
}

/*
 *  Force Poweroff: calls exit() immediately — inherently safe from any thread
 *  because it does not touch any 68k state.
 */
void MenuAction_ForcePoweroff(void)
{
    printf("MenuAction: Forced Poweroff and Exit\n");
    fflush(stdout);
    QuitEmulator();
}

void MenuAction_AddFloppy(void)
{
    char path[MENU_CMD_PATH_MAX] = {0};
    if (!MenuBar_ShowOpenFileDialog("Select Floppy Disk Image",
                                   "Floppy Disk Images",
                                   "*.dsk;*.img;*.image;*.dmg;*.flp;*",
                                   path, sizeof(path)))
        return;
    if (path[0] == '\0')
        return;

    MenuCmd cmd;
    cmd.type  = MENU_CMD_ADD_FLOPPY;
    cmd.param = 0;
    strncpy(cmd.path, path, MENU_CMD_PATH_MAX - 1);
    cmd.path[MENU_CMD_PATH_MAX - 1] = '\0';
    MenuQueue_Post(&cmd);
}

void MenuAction_AttachSCSI(int id)
{
    if (id < 0 || id > 6)
        return;

    char title[64];
    sprintf(title, "Select Image for SCSI %d", id);

    char path[MENU_CMD_PATH_MAX] = {0};
    if (!MenuBar_ShowOpenFileDialog(title,
                                   "Disk and CD Images",
                                   "*.dsk;*.img;*.iso;*.toast;*.cdr;*.cue;*.hda;*.dmg;*",
                                   path, sizeof(path)))
        return;
    if (path[0] == '\0')
        return;

    MenuCmd cmd;
    cmd.type  = MENU_CMD_ATTACH_SCSI;
    cmd.param = id;
    strncpy(cmd.path, path, MENU_CMD_PATH_MAX - 1);
    cmd.path[MENU_CMD_PATH_MAX - 1] = '\0';
    MenuQueue_Post(&cmd);
}

void MenuAction_DetachSCSI(int id)
{
    if (id < 0 || id > 6)
        return;
    MenuCmd cmd = {MENU_CMD_DETACH_SCSI, id, ""};
    MenuQueue_Post(&cmd);
}
