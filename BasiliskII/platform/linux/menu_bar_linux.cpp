/*
 *  menu_bar_linux.cpp - Linux / Unix menu bar and dialogs
 *
 *  Cockatrice III
 *
 *  There is no native menu bar on this host yet: SDL 1.2 owns the only window
 *  and there is no GTK container to hang a GtkMenuBar off, so MenuBar_Init()
 *  and MenuBar_UpdateAll() are stubs and only the file dialog is real.
 *
 *  Adding one is now a translation job rather than a design job. The menus,
 *  their labels, ordering, shortcuts and enable rules are described once in
 *  SDL/menu_model.cpp; walk MenuModel_Get() to build the widgets, call
 *  MenuModel_Invoke() with the row's command_id from the "activate" handler,
 *  and re-walk the model from MenuBar_UpdateAll() after MenuModel_Refresh().
 *  See platform/windows/menu_bar_win32.cpp for a worked example.
 */

#if !defined(__APPLE__) && !defined(WIN32)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sysdeps.h"
#include "menu_bar.h"
#include "menu_model.h"

#if ENABLE_GTK
#include <gtk/gtk.h>
#endif

/*
 *  No native menu bar to build; the model is still consulted so that a future
 *  GTK implementation has a single obvious place to start from.
 */
void MenuBar_Init(void *native_window_handle)
{
	(void)native_window_handle;
	MenuBar_UpdateAll();
}

/*
 *  Keep the shared model's dynamic labels and enable flags current even with no
 *  widgets to show them, so anything else reading the model sees live state.
 */
void MenuBar_UpdateAll(void)
{
	MenuModel_Refresh();
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
