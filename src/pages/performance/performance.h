#ifndef PERFORMANCE_H
#define PERFORMANCE_H

#include <gtk/gtk.h>
#include "../../app/app.h" /* provides the App type */

/*
 * Creates the Performance page. The returned widget is fully self-contained:
 * it owns its sampling timer, history and widgets, and cleans everything up
 * when it is destroyed. Insert it into the application's GtkStack as-is.
 */
GtkWidget *performance_page_create(App *app);

#endif /* PERFORMANCE_H */
