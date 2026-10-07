#include "graph.h"
#include "../data/history.h"
#include <math.h>

#define GRAPH_CAPACITY 120

typedef struct {
    char *title;
    History *history;
} GraphState;

static void graph_state_free(gpointer data)
{
    GraphState *g = data;
    if (!g) return;
    history_free(g->history);
    g_free(g->title);
    g_free(g);
}

static void draw_graph(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data)
{
    GraphState *g = user_data;
    (void)area;
    cairo_set_source_rgba(cr, 0.04, 0.05, 0.07, 1.0);
    cairo_paint(cr);

    const double left = 10, right = 10, top = 10, bottom = 10;
    double w = MAX(1, width - left - right);
    double h = MAX(1, height - top - bottom);

    cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    cairo_set_line_width(cr, 1.0);
    for (int i = 1; i < 4; ++i) {
        double y = top + h * i / 4.0;
        cairo_move_to(cr, left, y);
        cairo_line_to(cr, width - right, y);
        cairo_stroke(cr);
    }

    guint n = history_length(g->history);
    if (!n) return;

    double minv = INFINITY, maxv = -INFINITY;
    for (guint i = 0; i < n; ++i) {
        double v = history_get(g->history, i);
        if (isfinite(v)) { minv = MIN(minv, v); maxv = MAX(maxv, v); }
    }
    if (!isfinite(minv) || !isfinite(maxv)) return;
    if (fabs(maxv - minv) < 1e-9) { minv -= 1; maxv += 1; }
    double pad = (maxv - minv) * 0.12;
    minv -= pad; maxv += pad;

    cairo_set_source_rgba(cr, 0.45, 0.78, 1.0, 0.95);
    cairo_set_line_width(cr, 2.0);
    for (guint i = 0; i < n; ++i) {
        double v = history_get(g->history, i);
        double x = left + (n == 1 ? 0.5 : (double)i / (double)(n - 1)) * w;
        double y = top + (1.0 - (v - minv) / (maxv - minv)) * h;
        if (i == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
}

GtkWidget *graph_new(const char *title)
{
    GtkWidget *area = gtk_drawing_area_new();
    GraphState *g = g_new0(GraphState, 1);
    g->title = g_strdup(title ? title : "");
    g->history = history_new(GRAPH_CAPACITY);
    g_object_set_data_full(G_OBJECT(area), "task-manager-graph-state", g, graph_state_free);
    gtk_widget_add_css_class(area, "tm-graph");
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area), 260);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), 100);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_graph, g, NULL);
    gtk_widget_set_tooltip_text(area, g->title);
    return area;
}

void graph_clear(GtkWidget *graph)
{
    GraphState *g = graph ? g_object_get_data(G_OBJECT(graph), "task-manager-graph-state") : NULL;
    if (!g) return;
    history_clear(g->history);
    gtk_widget_queue_draw(graph);
}

void graph_push(GtkWidget *graph, double value)
{
    GraphState *g = graph ? g_object_get_data(G_OBJECT(graph), "task-manager-graph-state") : NULL;
    if (!g) return;
    history_push(g->history, value);
    gtk_widget_queue_draw(graph);
}
