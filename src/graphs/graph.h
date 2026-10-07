#ifndef TASK_MANAGER_GRAPH_H
#define TASK_MANAGER_GRAPH_H

#include <gtk/gtk.h>

GtkWidget *graph_new(const char *title);
void graph_clear(GtkWidget *graph);
void graph_push(GtkWidget *graph, double value);

#endif
