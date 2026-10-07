#ifndef TASK_MANAGER_UI_H
#define TASK_MANAGER_UI_H

#include <gtk/gtk.h>
#include "../app/app.h"

GtkWidget *ui_create_header(void);
GtkWidget *ui_create_sidebar(GtkStack *stack);
void ui_load_css(void);

#endif
