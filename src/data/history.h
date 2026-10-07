#ifndef TASK_MANAGER_HISTORY_H
#define TASK_MANAGER_HISTORY_H

#include <glib.h>

typedef struct _History History;

History *history_new(guint capacity);
void history_free(History *history);
void history_clear(History *history);
void history_push(History *history, double value);
guint history_length(const History *history);
guint history_capacity(const History *history);
double history_get(const History *history, guint index);
double history_latest(const History *history);

#endif
