/*
 * applications.h - Page 3: Applications
 *
 * The only symbol this module exports.  Everything else (discovery, models,
 * widgets, matching, refresh logic) is private to applications.c.
 */

#ifndef APPLICATIONS_H
#define APPLICATIONS_H

#include <gtk/gtk.h>

#include "../../app/app.h" /* App, shared process/system monitoring backend */

G_BEGIN_DECLS

/*
 * Creates the Applications page.  Returns a floating GtkWidget that the
 * caller inserts into its page stack.  The page owns all of its own state and
 * frees it when the widget is destroyed.
 */
GtkWidget *applications_page_create(App *app);

G_END_DECLS

#endif /* APPLICATIONS_H */
