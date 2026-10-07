#include "history.h"
#include <math.h>

struct _History {
    double *values;
    guint capacity;
    guint length;
    guint head;
};

History *history_new(guint capacity)
{
    History *h = g_new0(History, 1);
    h->capacity = capacity ? capacity : 1;
    h->values = g_new0(double, h->capacity);
    return h;
}

void history_free(History *h)
{
    if (!h) return;
    g_free(h->values);
    g_free(h);
}

void history_clear(History *h)
{
    if (!h) return;
    h->length = 0;
    h->head = 0;
}

void history_push(History *h, double value)
{
    if (!h) return;
    h->values[h->head] = value;
    h->head = (h->head + 1) % h->capacity;
    if (h->length < h->capacity)
        h->length++;
}

guint history_length(const History *h) { return h ? h->length : 0; }
guint history_capacity(const History *h) { return h ? h->capacity : 0; }

double history_get(const History *h, guint index)
{
    if (!h || index >= h->length) return NAN;
    guint oldest = (h->head + h->capacity - h->length) % h->capacity;
    return h->values[(oldest + index) % h->capacity];
}

double history_latest(const History *h)
{
    return h && h->length ? history_get(h, h->length - 1) : NAN;
}
