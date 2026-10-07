/*
 * processes.h - Public interface of the Processes page.
 *
 * This is the only symbol the rest of the application may use.
 * All state, models, widgets and callbacks are private to processes.c.
 */
#ifndef PAGES_PROCESSES_PROCESSES_H
#define PAGES_PROCESSES_PROCESSES_H

#include <gtk/gtk.h>
#include "../../app/app.h"

G_BEGIN_DECLS

/*
 * Creates the Processes page. The returned widget is owned by its parent
 * (the application's page stack). Monitoring starts immediately and stops
 * when the widget is finalized.
 */
GtkWidget *processes_page_create(App *app);

G_END_DECLS

#endif /* PAGES_PROCESSES_PROCESSES_H */
