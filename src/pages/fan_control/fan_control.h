#ifndef FAN_CONTROL_H
#define FAN_CONTROL_H

#include <gtk/gtk.h>

typedef struct _App App;

GtkWidget *fan_control_page_create(App *app);

#endif /* FAN_CONTROL_H */
