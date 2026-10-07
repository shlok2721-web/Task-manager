#ifndef STARTUP_H
#define STARTUP_H

#include <gtk/gtk.h>
#include "../../app/app.h"

G_BEGIN_DECLS

/* Page 4: Startup Applications. Returns a floating, ready-to-insert page widget. */
GtkWidget *startup_page_create(App *app);

G_END_DECLS

#endif /* STARTUP_H */
