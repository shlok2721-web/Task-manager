#include "ui.h"
#include <adwaita.h>

#ifndef TASK_MANAGER_DATA_DIR
#define TASK_MANAGER_DATA_DIR "/usr/share/task-manager"
#endif

void ui_load_css(void)
{
    static gboolean loaded = FALSE;
    if (loaded) return;
    loaded = TRUE;
    GtkCssProvider *provider = gtk_css_provider_new();
    gchar *path = g_build_filename(TASK_MANAGER_DATA_DIR, "style.css", NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        path = g_strdup("resources/style.css");
    }
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        gtk_css_provider_load_from_path(provider, path);
    }
    GdkDisplay *display = gdk_display_get_default();
    if (display)
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION - 10);
    g_object_unref(provider);
    g_free(path);
}

GtkWidget *ui_create_header(void)
{
    AdwHeaderBar *bar = ADW_HEADER_BAR(adw_header_bar_new());
    GtkWidget *title = gtk_label_new("Task Manager");
    gtk_widget_add_css_class(title, "tm-window-title");
    adw_header_bar_set_title_widget(bar, title);
    return GTK_WIDGET(bar);
}

GtkWidget *ui_create_sidebar(GtkStack *stack)
{
    GtkWidget *sidebar = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), stack);
    gtk_widget_add_css_class(sidebar, "tm-sidebar");
    gtk_widget_set_size_request(sidebar, 220, -1);
    return sidebar;
}
