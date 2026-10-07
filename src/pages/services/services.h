#ifndef SERVICES_H
#define SERVICES_H

#include <gtk/gtk.h>
#include "../../app/app.h" /* provides the App type */

/* Creates the Services page (systemd service management).
 * The returned widget is a floating reference suitable for AdwViewStack /
 * GtkStack insertion. All state is owned by the widget and released when
 * it is destroyed. */
GtkWidget *services_page_create(App *app);

#endif /* SERVICES_H */
