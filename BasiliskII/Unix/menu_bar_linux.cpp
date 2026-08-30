/*
 *  menu_bar_linux.cpp - Linux / Unix Menu Bar and Dialogs
 *
 *  Cockatrice III
 */

#if !defined(__APPLE__) && !defined(WIN32)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sysdeps.h"
#include "menu_bar.h"
#include "scsi.h"

#if ENABLE_GTK
#include <gtk/gtk.h>
#endif

void MenuBar_Init(void *native_window_handle)
{
	MenuBar_UpdateAll();
}

void MenuBar_UpdateAll(void)
{
}

bool MenuBar_ShowOpenFileDialog(const char *title, const char *filter_desc, const char *filter_exts, char *out_path, size_t max_len)
{
#if ENABLE_GTK
	if (gtk_init_check(NULL, NULL)) {
		GtkWidget *dialog = gtk_file_chooser_dialog_new(title ? title : "Open File",
		                                               NULL,
		                                               GTK_FILE_CHOOSER_ACTION_OPEN,
		                                               GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
		                                               GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
		                                               NULL);
		if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
			char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
			if (filename) {
				strncpy(out_path, filename, max_len - 1);
				out_path[max_len - 1] = '\0';
				g_free(filename);
				gtk_widget_destroy(dialog);
				return true;
			}
		}
		gtk_widget_destroy(dialog);
		return false;
	}
#endif

	// Fallback 1: try zenity
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "zenity --file-selection --title=\"%s\" 2>/dev/null", title ? title : "Select File");
	FILE *fp = popen(cmd, "r");
	if (fp) {
		if (fgets(out_path, max_len, fp)) {
			size_t l = strlen(out_path);
			if (l > 0 && out_path[l - 1] == '\n')
				out_path[l - 1] = '\0';
			int status = pclose(fp);
			if (status == 0 && out_path[0] != '\0')
				return true;
		} else {
			pclose(fp);
		}
	}

	// Fallback 2: try kdialog
	snprintf(cmd, sizeof(cmd), "kdialog --getopenfilename . 2>/dev/null");
	fp = popen(cmd, "r");
	if (fp) {
		if (fgets(out_path, max_len, fp)) {
			size_t l = strlen(out_path);
			if (l > 0 && out_path[l - 1] == '\n')
				out_path[l - 1] = '\0';
			int status = pclose(fp);
			if (status == 0 && out_path[0] != '\0')
				return true;
		} else {
			pclose(fp);
		}
	}

	return false;
}

#endif // !defined(__APPLE__) && !defined(WIN32)
