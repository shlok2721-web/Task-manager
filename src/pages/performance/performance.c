/*
 * performance.c - Performance page (CPU / GPU / Memory / Disk / Network / Temperature)
 *
 * Layers (kept strictly separate):
 *   1. Series / Graph   - rolling time-series storage and Cairo rendering. Never reads hardware.
 *   2. Inventory/Sampler - hardware discovery (once) and sampling (/proc, /sys, NVML via dlopen).
 *                          Runs on a worker thread through GTask; never touches GTK.
 *   3. Page / Resource  - GTK widgets, layout, state, selection and view modes.
 *                          Receives immutable Sample snapshots on the main thread.
 *
 * Build: pkg-config --cflags --libs gtk4 libadwaita-1   (+ -lm -ldl on older glibc)
 */
#define _GNU_SOURCE
#include "performance.h"

#include <adwaita.h>
#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/* ------------------------------------------------------------------------ */

#define PERF_SAMPLE_INTERVAL_MS 1000 /* change here (or Page.interval) to retune */
#define HIST_LEN 60                  /* samples kept per series */
#define MAX_SER 6
#define MAX_CORES 512
#define MAX_GPU 8
#define MAX_DISK 16
#define MAX_NET 16
#define MAX_SENS 48
#define MAX_STATS 24
#define MAX_GRAPHS 4

#define KIB 1024.0
#define MIB (1024.0 * 1024.0)
#define GIB (1024.0 * 1024.0 * 1024.0)

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

static gboolean read_str(char *buf, gsize len, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return FALSE;
    gsize n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ' || buf[n - 1] == '\t'))
        buf[--n] = '\0';
    return n > 0;
}

static gboolean read_num(double *out, const char *path)
{
    char b[64];
    if (!read_str(b, sizeof b, path))
        return FALSE;
    char *end;
    double v = g_ascii_strtod(b, &end);
    if (end == b)
        return FALSE;
    *out = v;
    return TRUE;
}

G_GNUC_PRINTF(2, 3)
static gboolean read_num_f(double *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *p = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gboolean ok = read_num(out, p);
    g_free(p);
    return ok;
}

G_GNUC_PRINTF(3, 4)
static gboolean read_str_f(char *buf, gsize len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *p = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gboolean ok = read_str(buf, len, p);
    g_free(p);
    return ok;
}

/* First directory entry of `dir` whose name starts with `prefix` (full path, caller frees). */
static char *first_match(const char *dir, const char *prefix)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d)
        return NULL;
    char *best = NULL;
    const char *n;
    while ((n = g_dir_read_name(d))) {
        if (g_str_has_prefix(n, prefix) && (!best || strcmp(n, best) < 0)) {
            g_free(best);
            best = g_strdup(n);
        }
    }
    g_dir_close(d);
    char *full = best ? g_build_filename(dir, best, NULL) : NULL;
    g_free(best);
    return full;
}

/* hwmon directory below a device path (handles both <dev>/hwmon/hwmonN and <dev>/hwmonN). */
static char *find_hwmon(const char *dev)
{
    char *d = g_build_filename(dev, "hwmon", NULL);
    char *r = first_match(d, "hwmon");
    g_free(d);
    if (!r)
        r = first_match(dev, "hwmon");
    return r;
}

static void str_replace_owned(char **dst, char *val)
{
    g_free(*dst);
    *dst = val;
}

/* ------------------------------------------------------------------------ */
/* Value formatting                                                         */
/* ------------------------------------------------------------------------ */

typedef enum { UNIT_PCT, UNIT_BYTES, UNIT_BPS, UNIT_MHZ, UNIT_C, UNIT_W, UNIT_PPS } Unit;

static void fmt_bytes(char *b, gsize n, double v, const char *suffix)
{
    static const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    int i = 0;
    while (v >= 1024.0 && i < 5) {
        v /= 1024.0;
        i++;
    }
    if (i == 0)
        g_snprintf(b, n, "%.0f %s%s", v, u[i], suffix);
    else
        g_snprintf(b, n, v < 10 ? "%.2f %s%s" : (v < 100 ? "%.1f %s%s" : "%.0f %s%s"), v, u[i], suffix);
}

static void fmt_value(char *b, gsize n, Unit u, double v)
{
    if (isnan(v) || isinf(v)) {
        g_snprintf(b, n, "\xE2\x80\x94"); /* em dash */
        return;
    }
    switch (u) {
    case UNIT_PCT:   g_snprintf(b, n, "%.0f%%", v); break;
    case UNIT_BYTES: fmt_bytes(b, n, v, ""); break;
    case UNIT_BPS:   fmt_bytes(b, n, v, "/s"); break;
    case UNIT_MHZ:   if (v >= 1000) g_snprintf(b, n, "%.2f GHz", v / 1000.0); else g_snprintf(b, n, "%.0f MHz", v); break;
    case UNIT_C:     g_snprintf(b, n, "%.0f\xC2\xB0""C", v); break;
    case UNIT_W:     g_snprintf(b, n, v < 10 ? "%.1f W" : "%.0f W", v); break;
    case UNIT_PPS:   g_snprintf(b, n, "%.0f pkt/s", v); break;
    }
}

static void fmt_uptime(char *b, gsize n, double secs)
{
    if (isnan(secs)) { g_snprintf(b, n, "\xE2\x80\x94"); return; }
    guint64 s = (guint64)secs;
    guint64 d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60;
    if (d)
        g_snprintf(b, n, "%" G_GUINT64_FORMAT "d %02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT,
                   d, h, m, s % 60);
    else
        g_snprintf(b, n, "%02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT, h, m, s % 60);
}

static void set_text(GtkLabel *l, const char *t)
{
    if (l)
        gtk_label_set_text(l, t); /* GtkLabel ignores identical text */
}

static void set_value(GtkLabel *l, Unit u, double v)
{
    char b[48];
    fmt_value(b, sizeof b, u, v);
    set_text(l, b);
}

static void set_textf(GtkLabel *l, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void set_textf(GtkLabel *l, const char *fmt, ...)
{
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    g_vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    set_text(l, b);
}

/* ------------------------------------------------------------------------ */
/* Time series (ring buffer)                                                */
/* ------------------------------------------------------------------------ */

typedef struct {
    double v[HIST_LEN];
    int head;  /* next write index */
    int count; /* valid samples */
} Series;

static void series_push(Series *s, double v)
{
    s->v[s->head] = v;
    s->head = (s->head + 1) % HIST_LEN;
    if (s->count < HIST_LEN)
        s->count++;
}

static double series_at(const Series *s, int i) /* 0 = oldest */
{
    return s->v[(s->head - s->count + i + 2 * HIST_LEN) % HIST_LEN];
}

/* ------------------------------------------------------------------------ */
/* Graph widget (GtkDrawingArea + Cairo). Receives data only via Series.     */
/* ------------------------------------------------------------------------ */

typedef enum { GSTYLE_AREA, GSTYLE_LINE } GStyle;

typedef struct {
    GtkWidget *area;
    Unit unit;
    GStyle style;
    gboolean compact;
    double fixed_max; /* >0: fixed scale */
    double floor_max; /* minimum auto-scale ceiling */
    double cur_max;   /* eased axis maximum */
    double span_s;
    int n;
    const Series *ser[MAX_SER];
    const char *name[MAX_SER];
    GdkRGBA col[MAX_SER];
} Graph;

static const GdkRGBA COL_CPU   = { 0.15f, 0.70f, 0.80f, 1 };
static const GdkRGBA COL_GPU   = { 0.62f, 0.45f, 0.86f, 1 };
static const GdkRGBA COL_MEM   = { 0.36f, 0.50f, 0.92f, 1 };
static const GdkRGBA COL_SWAP  = { 0.80f, 0.55f, 0.90f, 1 };
static const GdkRGBA COL_READ  = { 0.25f, 0.76f, 0.47f, 1 };
static const GdkRGBA COL_WRITE = { 0.93f, 0.62f, 0.22f, 1 };
static const GdkRGBA COL_DOWN  = { 0.20f, 0.65f, 0.95f, 1 };
static const GdkRGBA COL_UP    = { 0.92f, 0.38f, 0.58f, 1 };
static const GdkRGBA COL_TEMP  = { 0.93f, 0.40f, 0.31f, 1 };
static const GdkRGBA COL_POWER = { 0.95f, 0.78f, 0.25f, 1 };
static const GdkRGBA COL_CACHE = { 0.55f, 0.70f, 0.95f, 1 };
static const GdkRGBA COL_SENS[MAX_SER] = {
    { 0.93f, 0.40f, 0.31f, 1 }, { 0.62f, 0.45f, 0.86f, 1 }, { 0.25f, 0.76f, 0.47f, 1 },
    { 0.93f, 0.62f, 0.22f, 1 }, { 0.20f, 0.65f, 0.95f, 1 }, { 0.92f, 0.38f, 0.58f, 1 },
};

static double unit_floor(Unit u)
{
    switch (u) {
    case UNIT_PCT:   return 100;
    case UNIT_BYTES: return MIB;
    case UNIT_BPS:   return 100 * KIB;
    case UNIT_MHZ:   return 1000;
    case UNIT_C:     return 100;
    case UNIT_W:     return 10;
    case UNIT_PPS:   return 100;
    }
    return 1;
}

static double nice_ceil(double v)
{
    if (v <= 0)
        return 1;
    double e = pow(10, floor(log10(v)));
    double m = v / e;
    double n = m <= 1 ? 1 : m <= 2 ? 2 : m <= 2.5 ? 2.5 : m <= 5 ? 5 : 10;
    return n * e;
}

static double nice_ceil_bin(double v) /* 1,2,4,8 * 2^k so byte axes read cleanly */
{
    if (v <= 0)
        return 1;
    return pow(2, ceil(log2(v)));
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    r = MIN(r, MIN(w, h) / 2);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static void draw_text(GtkWidget *w, cairo_t *cr, const char *txt, double x, double y, int anchor,
                      const GdkRGBA *c, double alpha, gboolean bold)
{
    PangoLayout *l = gtk_widget_create_pango_layout(w, txt);
    PangoAttrList *al = pango_attr_list_new();
    pango_attr_list_insert(al, pango_attr_scale_new(0.85));
    if (bold)
        pango_attr_list_insert(al, pango_attr_weight_new(PANGO_WEIGHT_SEMIBOLD));
    pango_layout_set_attributes(l, al);
    pango_attr_list_unref(al);
    int tw, th;
    pango_layout_get_pixel_size(l, &tw, &th);
    double ox = anchor == 1 ? x - tw : (anchor == 2 ? x - tw / 2.0 : x);
    cairo_set_source_rgba(cr, c->red, c->green, c->blue, alpha);
    cairo_move_to(cr, ox, y);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

/* Smooth path through points i..j (horizontal-tangent cubic segments, no overshoot). */
static void trace_run(cairo_t *cr, const double *xs, const double *ys, int i, int j)
{
    cairo_move_to(cr, xs[i], ys[i]);
    for (int k = i + 1; k <= j; k++) {
        double mx = (xs[k - 1] + xs[k]) / 2;
        cairo_curve_to(cr, mx, ys[k - 1], mx, ys[k], xs[k], ys[k]);
    }
}

static void graph_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
    Graph *g = data;
    GtkWidget *wd = GTK_WIDGET(area);
    GdkRGBA fg;
    gtk_widget_get_color(wd, &fg);

    const double axis_h = g->compact ? 0 : 18;
    const double pw = w - 1, ph = h - 1 - axis_h, rad = g->compact ? 5 : 8;
    const double top = 3.5, bot = ph - 3.0;
    if (pw < 8 || ph < 8)
        return;

    /* plot background + border */
    rounded_rect(cr, 0.5, 0.5, pw, ph, rad);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.045);
    cairo_fill(cr);

    cairo_save(cr);
    rounded_rect(cr, 0.5, 0.5, pw, ph, rad);
    cairo_clip(cr);

    /* grid */
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, g->compact ? 0.06 : 0.09);
    int hl = g->compact ? 1 : 3, vl = g->compact ? 0 : 5;
    for (int k = 1; k <= hl; k++) {
        double y = floor(0.5 + ph * k / (hl + 1)) + 0.5;
        cairo_move_to(cr, 0, y);
        cairo_line_to(cr, pw, y);
    }
    for (int k = 1; k <= vl; k++) {
        double x = floor(0.5 + pw * k / (vl + 1)) + 0.5;
        cairo_move_to(cr, x, 0);
        cairo_line_to(cr, x, ph);
    }
    cairo_stroke(cr);

    /* series (series 0 drawn last = on top) */
    const double maxv = g->cur_max > 0 ? g->cur_max : 1;
    double lastx = 0, lasty = 0, lastv = NAN;
    for (int si = g->n - 1; si >= 0; si--) {
        const Series *s = g->ser[si];
        int cnt = s ? s->count : 0;
        if (cnt == 0)
            continue;
        double xs[HIST_LEN], ys[HIST_LEN];
        gboolean ok[HIST_LEN];
        for (int i = 0; i < cnt; i++) {
            double v = series_at(s, i);
            ok[i] = !isnan(v) && !isinf(v);
            xs[i] = 0.5 + pw * (double)(HIST_LEN - cnt + i) / (HIST_LEN - 1);
            ys[i] = ok[i] ? bot - (bot - top) * CLAMP(v / maxv, 0.0, 1.0) : bot;
        }
        const GdkRGBA *c = &g->col[si];
        for (int i = 0; i < cnt;) {
            if (!ok[i]) { i++; continue; }
            int j = i;
            while (j + 1 < cnt && ok[j + 1])
                j++;
            if (i == j) {
                cairo_arc(cr, xs[i], ys[i], 1.5, 0, 2 * G_PI);
                cairo_set_source_rgba(cr, c->red, c->green, c->blue, 1);
                cairo_fill(cr);
            } else {
                if (g->style == GSTYLE_AREA) {
                    double a = si == 0 ? (g->n > 1 ? 0.34 : 0.42) : 0.22;
                    cairo_pattern_t *pat = cairo_pattern_create_linear(0, top, 0, bot);
                    cairo_pattern_add_color_stop_rgba(pat, 0, c->red, c->green, c->blue, a);
                    cairo_pattern_add_color_stop_rgba(pat, 1, c->red, c->green, c->blue, a * 0.12);
                    trace_run(cr, xs, ys, i, j);
                    cairo_line_to(cr, xs[j], bot + 4);
                    cairo_line_to(cr, xs[i], bot + 4);
                    cairo_close_path(cr);
                    cairo_set_source(cr, pat);
                    cairo_fill(cr);
                    cairo_pattern_destroy(pat);
                }
                trace_run(cr, xs, ys, i, j);
                cairo_set_source_rgba(cr, c->red, c->green, c->blue, 1);
                cairo_set_line_width(cr, g->compact ? 1.2 : 1.8);
                cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                cairo_stroke(cr);
            }
            i = j + 1;
        }
        if (si == 0 && ok[cnt - 1]) {
            lastx = xs[cnt - 1];
            lasty = ys[cnt - 1];
            lastv = series_at(s, cnt - 1);
        }
    }
    cairo_restore(cr);

    /* border */
    rounded_rect(cr, 0.5, 0.5, pw, ph, rad);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.14);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    if (g->compact)
        return;

    char b[48];

    /* axis maximum */
    fmt_value(b, sizeof b, g->unit, maxv);
    draw_text(wd, cr, b, pw - 8, 6, 1, &fg, 0.55, FALSE);

    /* legend */
    if (g->n > 1) {
        double x = 10;
        for (int i = 0; i < g->n; i++) {
            if (!g->name[i])
                continue;
            cairo_arc(cr, x + 4, 14, 4, 0, 2 * G_PI);
            cairo_set_source_rgba(cr, g->col[i].red, g->col[i].green, g->col[i].blue, 1);
            cairo_fill(cr);
            PangoLayout *l = gtk_widget_create_pango_layout(wd, g->name[i]);
            int tw, th;
            pango_layout_get_pixel_size(l, &tw, &th);
            g_object_unref(l);
            draw_text(wd, cr, g->name[i], x + 12, 6, 0, &fg, 0.75, FALSE);
            x += 12 + tw * 0.85 + 14;
        }
    }

    /* time axis */
    if (g->span_s >= 120)
        g_snprintf(b, sizeof b, "%.0f min", g->span_s / 60);
    else
        g_snprintf(b, sizeof b, "%.0f s", g->span_s);
    draw_text(wd, cr, b, 4, ph + 4, 0, &fg, 0.55, FALSE);
    draw_text(wd, cr, "now", pw - 2, ph + 4, 1, &fg, 0.55, FALSE);

    /* current-value indicator */
    if (!isnan(lastv)) {
        const GdkRGBA *c = &g->col[0];
        cairo_arc(cr, lastx - 1, lasty, 6, 0, 2 * G_PI);
        cairo_set_source_rgba(cr, c->red, c->green, c->blue, 0.25);
        cairo_fill(cr);
        cairo_arc(cr, lastx - 1, lasty, 3.2, 0, 2 * G_PI);
        cairo_set_source_rgba(cr, c->red, c->green, c->blue, 1);
        cairo_fill(cr);
        fmt_value(b, sizeof b, g->unit, lastv);
        double ty = lasty < 32 ? lasty + 9 : lasty - 20;
        draw_text(wd, cr, b, lastx - 10, ty, 1, &fg, 0.95, TRUE);
    }
}

static Graph *graph_new(Unit unit, GStyle style, double fixed_max, gboolean compact)
{
    Graph *g = g_new0(Graph, 1);
    g->unit = unit;
    g->style = style;
    g->compact = compact;
    g->fixed_max = fixed_max;
    g->floor_max = unit_floor(unit);
    g->cur_max = fixed_max > 0 ? fixed_max : g->floor_max;
    g->span_s = HIST_LEN * PERF_SAMPLE_INTERVAL_MS / 1000.0;
    g->area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(g->area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g->area), graph_draw, g, NULL);
    g_object_set_data_full(G_OBJECT(g->area), "perf-graph", g, g_free);
    return g;
}

static void graph_add(Graph *g, const Series *s, const char *name, const GdkRGBA *c)
{
    if (g->n >= MAX_SER)
        return;
    g->ser[g->n] = s;
    g->name[g->n] = name;
    g->col[g->n] = *c;
    g->n++;
}

static void graph_set_fixed_max(Graph *g, double m)
{
    g->fixed_max = m;
    g->cur_max = m;
}

/* Recompute the (eased) axis maximum and schedule a redraw. */
static void graph_refresh(Graph *g)
{
    double target = g->fixed_max;
    if (target <= 0) {
        double m = 0;
        for (int i = 0; i < g->n; i++)
            for (int k = 0; k < g->ser[i]->count; k++) {
                double v = series_at(g->ser[i], k);
                if (!isnan(v) && !isinf(v) && v > m)
                    m = v;
            }
        m = MAX(m * 1.15, g->floor_max);
        target = (g->unit == UNIT_BYTES || g->unit == UNIT_BPS) ? nice_ceil_bin(m) : nice_ceil(m);
    }
    if (target > g->cur_max)
        g->cur_max = target;
    else
        g->cur_max += (target - g->cur_max) * 0.25;
    if (fabs(g->cur_max - target) < target * 0.005)
        g->cur_max = target;
    gtk_widget_queue_draw(g->area);
}

/* ------------------------------------------------------------------------ */
/* Per-core bars widget                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
    double v[MAX_CORES];
    int n;
} Bars;

static void bars_draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
    Bars *b = data;
    if (b->n <= 0)
        return;
    GdkRGBA fg;
    gtk_widget_get_color(GTK_WIDGET(area), &fg);
    const double gap = 3;
    double bw = (w - gap * (b->n - 1)) / b->n;
    if (bw < 2)
        bw = 2;
    gboolean labels = bw >= 16;
    double bh = h - (labels ? 16 : 0);
    for (int i = 0; i < b->n; i++) {
        double x = i * (bw + gap);
        double v = isnan(b->v[i]) ? 0 : CLAMP(b->v[i], 0, 100);
        rounded_rect(cr, x, 0, bw, bh, MIN(4, bw / 2));
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.07);
        cairo_fill(cr);
        double fh = bh * v / 100.0;
        if (fh > 0.5) {
            cairo_save(cr);
            rounded_rect(cr, x, 0, bw, bh, MIN(4, bw / 2));
            cairo_clip(cr);
            cairo_pattern_t *pat = cairo_pattern_create_linear(0, bh - fh, 0, bh);
            cairo_pattern_add_color_stop_rgba(pat, 0, COL_CPU.red, COL_CPU.green, COL_CPU.blue, 0.95);
            cairo_pattern_add_color_stop_rgba(pat, 1, COL_CPU.red, COL_CPU.green, COL_CPU.blue, 0.55);
            cairo_rectangle(cr, x, bh - fh, bw, fh);
            cairo_set_source(cr, pat);
            cairo_fill(cr);
            cairo_pattern_destroy(pat);
            cairo_restore(cr);
        }
        if (labels) {
            char t[16];
            g_snprintf(t, sizeof t, "%d", i);
            draw_text(GTK_WIDGET(area), cr, t, x + bw / 2, bh + 2, 2, &fg, 0.55, FALSE);
        }
    }
}

static GtkWidget *bars_new(void)
{
    GtkWidget *a = gtk_drawing_area_new();
    Bars *b = g_new0(Bars, 1);
    gtk_widget_set_size_request(a, -1, 92);
    gtk_widget_set_hexpand(a, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(a), bars_draw, b, NULL);
    g_object_set_data_full(G_OBJECT(a), "perf-bars", b, g_free);
    return a;
}

static void bars_set(GtkWidget *a, const double *v, int n)
{
    Bars *b = g_object_get_data(G_OBJECT(a), "perf-bars");
    b->n = MIN(n, MAX_CORES);
    memcpy(b->v, v, sizeof(double) * b->n);
    gtk_widget_queue_draw(a);
}

/* ------------------------------------------------------------------------ */
/* Inventory: static hardware description, discovered once                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    char *slot, *name, *vendor, *driver, *pci_id, *link, *sysdev;
    char *busy_path, *vram_used_path, *vram_total_path, *freq_path, *temp_path, *power_path;
    double freq_scale, power_scale, vram_total;
    gboolean nvidia;
    int nvml_idx;
    gboolean has_util, has_vram, has_temp, has_power, has_clock;
} GpuInfo;

typedef struct {
    char *name, *model, *type, *bus, *hwmon;
    double capacity;
    GPtrArray *parts;
} DiskInfo;

typedef struct {
    char *name, *title, *kind, *mac, *driver;
} NetInfo;

typedef struct {
    char *label, *chip, *source, *path;
    int rank; /* 0 CPU, 1 GPU, 2 NVMe, 3 system */
    int nvml_gpu;
} SensInfo;

typedef struct {
    char *model;
    int sockets, cores, logical;
    double fmin, fmax;
    char *cache[4]; /* L1d, L1i, L2, L3 */
    gboolean has_cpufreq;
} CpuInfo;

typedef struct {
    CpuInfo cpu;
    int cpu_sens;
    double mem_total, swap_total;
    GpuInfo gpu[MAX_GPU];
    int ngpu;
    DiskInfo disk[MAX_DISK];
    int ndisk;
    NetInfo net[MAX_NET];
    int nnet;
    SensInfo sens[MAX_SENS];
    int nsens;
} Inventory;

static int cmp_strp(gconstpointer a, gconstpointer b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static GPtrArray *list_dir_sorted(const char *dir)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d) {
        const char *n;
        while ((n = g_dir_read_name(d)))
            g_ptr_array_add(a, g_strdup(n));
        g_dir_close(d);
    }
    g_ptr_array_sort(a, cmp_strp);
    return a;
}

static char *base_of_link(const char *path)
{
    char *t = g_file_read_link(path, NULL);
    if (!t)
        return NULL;
    char *b = g_path_get_basename(t);
    g_free(t);
    return b;
}

static char *cache_size_str(const char *sz)
{
    double n = atof(sz);
    double kb = strchr(sz, 'M') ? n * 1024 : n;
    if (kb >= 1024)
        return g_strdup_printf("%g MiB", kb / 1024);
    return g_strdup_printf("%g KiB", kb);
}

static void discover_cpu(Inventory *inv)
{
    CpuInfo *c = &inv->cpu;
    GHashTable *cores = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *pids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char pid[32] = "0";
    gchar *txt;
    if (g_file_get_contents("/proc/cpuinfo", &txt, NULL, NULL)) {
        char *save = NULL;
        for (char *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char *colon = strchr(line, ':');
            if (!colon)
                continue;
            *colon = '\0';
            char *key = g_strstrip(line), *val = g_strstrip(colon + 1);
            if (!strcmp(key, "processor"))
                c->logical++;
            else if (!c->model && (!strcmp(key, "model name") || !strcmp(key, "Hardware") || !strcmp(key, "cpu model")))
                c->model = g_strdup(val);
            else if (!strcmp(key, "physical id"))
                g_strlcpy(pid, val, sizeof pid);
            else if (!strcmp(key, "core id")) {
                g_hash_table_add(cores, g_strdup_printf("%s:%s", pid, val));
                g_hash_table_add(pids, g_strdup(pid));
            }
        }
        g_free(txt);
    }
    if (c->logical <= 0)
        c->logical = g_get_num_processors();
    c->cores = g_hash_table_size(cores) ? (int)g_hash_table_size(cores) : c->logical;
    c->sockets = g_hash_table_size(pids) ? (int)g_hash_table_size(pids) : 1;
    g_hash_table_unref(cores);
    g_hash_table_unref(pids);
    if (!c->model)
        c->model = g_strdup("Processor");

    c->has_cpufreq = g_file_test("/sys/devices/system/cpu/cpu0/cpufreq", G_FILE_TEST_IS_DIR);
    double v;
    c->fmin = c->fmax = NAN;
    if (read_num_f(&v, "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq"))
        c->fmin = v / 1000.0;
    if (read_num_f(&v, "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq"))
        c->fmax = v / 1000.0;

    for (int i = 0; i < 8; i++) {
        char lv[8], ty[24], sz[16];
        if (!read_str_f(lv, sizeof lv, "/sys/devices/system/cpu/cpu0/cache/index%d/level", i))
            break;
        read_str_f(ty, sizeof ty, "/sys/devices/system/cpu/cpu0/cache/index%d/type", i);
        if (!read_str_f(sz, sizeof sz, "/sys/devices/system/cpu/cpu0/cache/index%d/size", i))
            continue;
        int l = atoi(lv);
        int slot = l == 1 ? (strcmp(ty, "Instruction") == 0 ? 1 : 0) : (l == 2 ? 2 : (l == 3 ? 3 : -1));
        if (slot >= 0 && !c->cache[slot])
            c->cache[slot] = cache_size_str(sz);
    }

    gchar *mi;
    if (g_file_get_contents("/proc/meminfo", &mi, NULL, NULL)) {
        char *p;
        if ((p = strstr(mi, "MemTotal:")))
            inv->mem_total = atof(p + 9) * 1024;
        if ((p = strstr(mi, "SwapTotal:")))
            inv->swap_total = atof(p + 10) * 1024;
        g_free(mi);
    }
}

static char *pci_ids_lookup(const char *vend, const char *dev)
{
    static const char *paths[] = { "/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", "/usr/share/pci.ids", NULL };
    char *result = NULL;
    for (int i = 0; paths[i] && !result; i++) {
        gchar *txt;
        if (!g_file_get_contents(paths[i], &txt, NULL, NULL))
            continue;
        char *vk = g_strdup_printf("\n%s  ", vend), *dk = g_strdup_printf("\t%s  ", dev);
        char *p = strstr(txt, vk);
        if (p) {
            p = strchr(p + 1, '\n');
            while (p && p[1] == '\t') {
                p++;
                if (g_str_has_prefix(p, dk)) {
                    char *name = p + strlen(dk), *eol = strchr(name, '\n');
                    char *s = eol ? g_strndup(name, eol - name) : g_strdup(name);
                    char *lb = strchr(s, '['), *rb = lb ? strrchr(s, ']') : NULL;
                    result = (lb && rb && rb > lb) ? g_strndup(lb + 1, rb - lb - 1) : g_strdup(s);
                    g_free(s);
                    break;
                }
                p = strchr(p, '\n');
            }
        }
        g_free(vk);
        g_free(dk);
        g_free(txt);
    }
    return result;
}

static char *existing_path(const char *dir, const char *file)
{
    if (!dir)
        return NULL;
    char *p = g_build_filename(dir, file, NULL);
    if (g_file_test(p, G_FILE_TEST_EXISTS))
        return p;
    g_free(p);
    return NULL;
}

static void discover_gpus(Inventory *inv)
{
    GPtrArray *devs = list_dir_sorted("/sys/bus/pci/devices");
    int nvidia_seen = 0;
    for (guint i = 0; i < devs->len && inv->ngpu < MAX_GPU; i++) {
        const char *slot = devs->pdata[i];
        char *sys = g_build_filename("/sys/bus/pci/devices", slot, NULL);
        char cls[16] = "", vend[16] = "", dev[16] = "";
        read_str_f(cls, sizeof cls, "%s/class", sys);
        if (strncmp(cls, "0x0300", 6) && strncmp(cls, "0x0302", 6) && strncmp(cls, "0x0380", 6)) {
            g_free(sys);
            continue;
        }
        read_str_f(vend, sizeof vend, "%s/vendor", sys);
        read_str_f(dev, sizeof dev, "%s/device", sys);
        const char *v4 = g_str_has_prefix(vend, "0x") ? vend + 2 : vend;
        const char *d4 = g_str_has_prefix(dev, "0x") ? dev + 2 : dev;

        GpuInfo *g = &inv->gpu[inv->ngpu++];
        g->sysdev = sys;
        g->slot = g_strdup(slot);
        g->pci_id = g_strdup_printf("%s:%s", v4, d4);
        g->vendor = g_strdup(!g_ascii_strcasecmp(v4, "10de") ? "NVIDIA" : !g_ascii_strcasecmp(v4, "1002") ? "AMD"
                             : !g_ascii_strcasecmp(v4, "8086") ? "Intel" : v4);
        g->nvidia = !g_ascii_strcasecmp(v4, "10de");
        g->nvml_idx = g->nvidia ? nvidia_seen++ : -1;
        char *nm = pci_ids_lookup(v4, d4);
        g->name = nm ? g_strdup_printf("%s %s", g->vendor, nm) : g_strdup_printf("%s GPU", g->vendor);
        g_free(nm);
        char *drvp = g_build_filename(sys, "driver", NULL);
        g->driver = base_of_link(drvp);
        g_free(drvp);
        if (g->nvidia) {
            char ver[64];
            if (read_str(ver, sizeof ver, "/sys/module/nvidia/version")) {
                char *d = g_strdup_printf("%s %s", g->driver ? g->driver : "nvidia", ver);
                str_replace_owned(&g->driver, d);
            }
        }
        char spd[32] = "", wid[8] = "";
        read_str_f(spd, sizeof spd, "%s/current_link_speed", sys);
        read_str_f(wid, sizeof wid, "%s/current_link_width", sys);
        if (*spd)
            g->link = g_strdup_printf("%s x%s", spd, *wid ? wid : "?");

        g->busy_path = existing_path(sys, "gpu_busy_percent");
        g->vram_used_path = existing_path(sys, "mem_info_vram_used");
        g->vram_total_path = existing_path(sys, "mem_info_vram_total");
        char *hw = find_hwmon(sys);
        g->temp_path = existing_path(hw, "temp1_input");
        g->power_path = existing_path(hw, "power1_average");
        if (!g->power_path)
            g->power_path = existing_path(hw, "power1_input");
        g->power_scale = 1e-6;
        g->freq_path = existing_path(hw, "freq1_input");
        g->freq_scale = 1e-6;
        if (!g->freq_path) {
            char *drm = g_build_filename(sys, "drm", NULL);
            char *card = first_match(drm, "card");
            g->freq_path = existing_path(card, "gt_cur_freq_mhz");
            g_free(card);
            g_free(drm);
            g->freq_scale = 1;
        }
        if (!g->freq_path) {
            char *x = g_build_filename(sys, "tile0", "gt0", "freq0", NULL);
            g->freq_path = existing_path(x, "cur_freq");
            g_free(x);
        }
        g_free(hw);
        double v;
        if (g->vram_total_path && read_num(&v, g->vram_total_path))
            g->vram_total = v;
        g->has_util = g->busy_path || g->nvidia;
        g->has_vram = g->vram_total_path || g->nvidia;
        g->has_temp = g->temp_path || g->nvidia;
        g->has_power = g->power_path || g->nvidia;
        g->has_clock = g->freq_path || g->nvidia;
    }
    g_ptr_array_unref(devs);
}

static void discover_disks(Inventory *inv)
{
    GPtrArray *devs = list_dir_sorted("/sys/block");
    static const char *skip[] = { "loop", "ram", "zram", "dm-", "md", "sr", "fd", "nbd", NULL };
    for (guint i = 0; i < devs->len && inv->ndisk < MAX_DISK; i++) {
        const char *n = devs->pdata[i];
        gboolean bad = FALSE;
        for (int k = 0; skip[k]; k++)
            if (g_str_has_prefix(n, skip[k]))
                bad = TRUE;
        char *base = g_build_filename("/sys/block", n, NULL);
        char *devp = g_build_filename(base, "device", NULL);
        if (bad || !g_file_test(devp, G_FILE_TEST_EXISTS)) {
            g_free(base);
            g_free(devp);
            continue;
        }
        DiskInfo *d = &inv->disk[inv->ndisk++];
        d->name = g_strdup(n);
        char m[96] = "", b[32];
        read_str_f(m, sizeof m, "%s/model", devp);
        if (!*m)
            read_str_f(m, sizeof m, "%s/name", devp);
        d->model = g_strdup(*m ? g_strstrip(m) : n);
        double sectors = 0, rot = 0;
        read_num_f(&sectors, "%s/size", base);
        read_num_f(&rot, "%s/queue/rotational", base);
        d->capacity = sectors * 512.0;
        char *rp = realpath(base, NULL);
        const char *r = rp ? rp : "";
        d->bus = g_strdup(g_str_has_prefix(n, "nvme") ? "NVMe" : strstr(r, "/usb") ? "USB" : g_str_has_prefix(n, "mmc") ? "MMC/SD"
                          : g_str_has_prefix(n, "vd") ? "VirtIO" : "SATA/SCSI");
        free(rp);
        d->type = g_strdup(g_str_has_prefix(n, "nvme") ? "NVMe SSD" : g_str_has_prefix(n, "mmc") ? "Flash" : rot > 0 ? "HDD" : "SSD");
        (void)b;
        d->hwmon = find_hwmon(devp);
        d->parts = g_ptr_array_new_with_free_func(g_free);
        GPtrArray *kids = list_dir_sorted(base);
        for (guint k = 0; k < kids->len; k++) {
            const char *kn = kids->pdata[k];
            if (!g_str_has_prefix(kn, n))
                continue;
            char *pp = g_strdup_printf("%s/%s/partition", base, kn);
            if (g_file_test(pp, G_FILE_TEST_EXISTS))
                g_ptr_array_add(d->parts, g_strdup(kn));
            g_free(pp);
        }
        g_ptr_array_unref(kids);
        g_free(base);
        g_free(devp);
    }
    g_ptr_array_unref(devs);
}

static void discover_nets(Inventory *inv)
{
    GPtrArray *devs = list_dir_sorted("/sys/class/net");
    for (guint i = 0; i < devs->len && inv->nnet < MAX_NET; i++) {
        const char *n = devs->pdata[i];
        if (!strcmp(n, "lo"))
            continue;
        char *base = g_build_filename("/sys/class/net", n, NULL);
        char *rp = realpath(base, NULL);
        gboolean virt = rp && strstr(rp, "/virtual/");
        free(rp);
        if (virt) {
            g_free(base);
            continue;
        }
        NetInfo *ni = &inv->net[inv->nnet++];
        char *wl = g_build_filename(base, "wireless", NULL);
        gboolean wifi = g_file_test(wl, G_FILE_TEST_EXISTS);
        g_free(wl);
        ni->name = g_strdup(n);
        ni->kind = g_strdup(wifi ? "Wi-Fi" : g_str_has_prefix(n, "ww") ? "Mobile" : g_str_has_prefix(n, "en") || g_str_has_prefix(n, "eth") ? "Ethernet" : "Network");
        char mac[32] = "";
        read_str_f(mac, sizeof mac, "%s/address", base);
        ni->mac = g_strdup(mac);
        char *dl = g_strdup_printf("%s/device/driver", base);
        ni->driver = base_of_link(dl);
        g_free(dl);
        g_free(base);
    }
    g_ptr_array_unref(devs);
    for (int i = 0; i < inv->nnet; i++) { /* disambiguate duplicate kinds */
        int dup = 0;
        for (int k = 0; k < inv->nnet; k++)
            dup += !strcmp(inv->net[i].kind, inv->net[k].kind);
        inv->net[i].title = dup > 1 ? g_strdup_printf("%s (%s)", inv->net[i].kind, inv->net[i].name) : g_strdup(inv->net[i].kind);
    }
}

static int classify_chip(const char *chip)
{
    static const char *cpu[] = { "coretemp", "k10temp", "zenpower", "cpu_thermal", "cpu", NULL };
    static const char *gpu[] = { "amdgpu", "radeon", "nouveau", "nvidia", "i915", NULL };
    for (int i = 0; cpu[i]; i++)
        if (g_str_has_prefix(chip, cpu[i]))
            return 0;
    for (int i = 0; gpu[i]; i++)
        if (g_str_has_prefix(chip, gpu[i]))
            return 1;
    return g_str_has_prefix(chip, "nvme") ? 2 : 3;
}

static void discover_sensors(Inventory *inv)
{
    GPtrArray *hws = list_dir_sorted("/sys/class/hwmon");
    for (guint i = 0; i < hws->len && inv->nsens < MAX_SENS; i++) {
        char *dir = g_build_filename("/sys/class/hwmon", hws->pdata[i], NULL);
        char chip[48] = "";
        if (!read_str_f(chip, sizeof chip, "%s/name", dir))
            g_strlcpy(chip, hws->pdata[i], sizeof chip);
        GPtrArray *files = list_dir_sorted(dir);
        for (guint k = 0; k < files->len && inv->nsens < MAX_SENS; k++) {
            const char *f = files->pdata[k];
            if (!g_str_has_prefix(f, "temp") || !g_str_has_suffix(f, "_input"))
                continue;
            char *path = g_build_filename(dir, f, NULL);
            double v;
            if (!read_num(&v, path)) {
                g_free(path);
                continue;
            }
            char stem[16];
            g_strlcpy(stem, f, sizeof stem);
            *strchr(stem, '_') = '\0';
            char lab[64] = "";
            read_str_f(lab, sizeof lab, "%s/%s_label", dir, stem);
            SensInfo *s = &inv->sens[inv->nsens++];
            s->path = path;
            s->chip = g_strdup(chip);
            s->label = *lab ? g_strdup(lab) : g_strdup_printf("%s %s", chip, stem);
            s->source = g_strdup_printf("%s \xC2\xB7 %s", chip, (const char *)hws->pdata[i]);
            s->rank = classify_chip(chip);
            s->nvml_gpu = -1;
        }
        g_ptr_array_unref(files);
        g_free(dir);
    }
    g_ptr_array_unref(hws);

    if (inv->nsens == 0) {
        GPtrArray *tz = list_dir_sorted("/sys/class/thermal");
        for (guint i = 0; i < tz->len && inv->nsens < MAX_SENS; i++) {
            if (!g_str_has_prefix(tz->pdata[i], "thermal_zone"))
                continue;
            char *path = g_strdup_printf("/sys/class/thermal/%s/temp", (char *)tz->pdata[i]);
            double v;
            char type[48] = "zone";
            if (!read_num(&v, path)) { g_free(path); continue; }
            read_str_f(type, sizeof type, "/sys/class/thermal/%s/type", (char *)tz->pdata[i]);
            SensInfo *s = &inv->sens[inv->nsens++];
            s->path = path;
            s->chip = g_strdup(type);
            s->label = g_strdup(type);
            s->source = g_strdup_printf("thermal \xC2\xB7 %s", (char *)tz->pdata[i]);
            s->rank = classify_chip(type);
            s->nvml_gpu = -1;
        }
        g_ptr_array_unref(tz);
    }
    for (int i = 0; i < inv->ngpu && inv->nsens < MAX_SENS; i++) { /* NVML-only temperatures */
        if (!inv->gpu[i].nvidia)
            continue;
        SensInfo *s = &inv->sens[inv->nsens++];
        s->chip = g_strdup("nvml");
        s->label = g_strdup_printf("GPU %d core", i);
        s->source = g_strdup("NVML");
        s->rank = 1;
        s->nvml_gpu = i;
    }
    /* stable sort by priority rank */
    for (int i = 1; i < inv->nsens; i++) {
        SensInfo t = inv->sens[i];
        int j = i - 1;
        while (j >= 0 && inv->sens[j].rank > t.rank) {
            inv->sens[j + 1] = inv->sens[j];
            j--;
        }
        inv->sens[j + 1] = t;
    }
    inv->cpu_sens = -1;
    for (int i = 0; i < inv->nsens && inv->sens[i].rank == 0; i++) {
        const char *l = inv->sens[i].label;
        if (strstr(l, "Package") || strstr(l, "Tctl") || strstr(l, "Tdie")) {
            inv->cpu_sens = i;
            break;
        }
        if (inv->cpu_sens < 0)
            inv->cpu_sens = i;
    }
}

/* ------------------------------------------------------------------------ */
/* NVML (optional, dlopen'ed, used only from the worker thread)             */
/* ------------------------------------------------------------------------ */

typedef struct { unsigned gpu, memory; } NvUtil;
typedef struct { unsigned long long total, free, used; } NvMem;

typedef struct {
    void *lib;
    gboolean tried, ok;
    int count;
    int (*init)(void), (*shutdown)(void);
    int (*get_count)(unsigned *);
    int (*by_index)(unsigned, void **);
    int (*name)(void *, char *, unsigned);
    int (*util)(void *, NvUtil *);
    int (*mem)(void *, NvMem *);
    int (*temp)(void *, int, unsigned *);
    int (*power)(void *, unsigned *);
    int (*clock)(void *, int, unsigned *);
    void *h[MAX_GPU];
} Nvml;

static void nvml_load(Nvml *n)
{
    n->tried = TRUE;
    n->lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
    if (!n->lib)
        return;
#define SYM(f, s) *(void **)&n->f = dlsym(n->lib, s)
    SYM(init, "nvmlInit_v2");
    SYM(shutdown, "nvmlShutdown");
    SYM(get_count, "nvmlDeviceGetCount_v2");
    SYM(by_index, "nvmlDeviceGetHandleByIndex_v2");
    SYM(name, "nvmlDeviceGetName");
    SYM(util, "nvmlDeviceGetUtilizationRates");
    SYM(mem, "nvmlDeviceGetMemoryInfo");
    SYM(temp, "nvmlDeviceGetTemperature");
    SYM(power, "nvmlDeviceGetPowerUsage");
    SYM(clock, "nvmlDeviceGetClockInfo");
#undef SYM
    if (!n->init || !n->get_count || !n->by_index || n->init() != 0) {
        dlclose(n->lib);
        n->lib = NULL;
        return;
    }
    unsigned c = 0;
    n->get_count(&c);
    for (unsigned i = 0; i < c && i < MAX_GPU; i++)
        if (n->by_index(i, &n->h[i]) == 0)
            n->count = i + 1;
    n->ok = n->count > 0;
}

/* ------------------------------------------------------------------------ */
/* Samples (immutable snapshots handed to the UI)                            */
/* ------------------------------------------------------------------------ */

typedef struct { double util, vram_used, vram_total, clock, power, temp; char *name; } GpuSample;
typedef struct {
    double read_bps, write_bps, active, used, total, temp;
    char *fs_text;
} DiskSample;
typedef struct {
    double rx_bps, tx_bps, rx_pps, tx_pps, rx_bytes, tx_bytes, rx_pkts, tx_pkts, rx_err, tx_err, rx_drop, tx_drop, speed;
    char state[24], duplex[16];
    gboolean present;
} NetSample;

typedef struct {
    double cpu_util, core[MAX_CORES], freq_avg, freq_fast, load[3], uptime;
    int ncores, procs, threads;
    char governor[32];
    double mem_total, mem_free, mem_avail, mem_buffers, mem_cached, sreclaim, swap_total, swap_free, swap_cached;
    GpuSample gpu[MAX_GPU];
    DiskSample disk[MAX_DISK];
    NetSample net[MAX_NET];
    double temp[MAX_SENS];
} Sample;

static void sample_free(Sample *s)
{
    if (!s)
        return;
    for (int i = 0; i < MAX_GPU; i++)
        g_free(s->gpu[i].name);
    for (int i = 0; i < MAX_DISK; i++)
        g_free(s->disk[i].fs_text);
    g_free(s);
}

/* ------------------------------------------------------------------------ */
/* Sampler (worker thread only)                                              */
/* ------------------------------------------------------------------------ */

typedef struct { guint64 busy, total; } CpuTick;

typedef struct {
    Inventory inv; /* immutable after sampler_new() */
    gint64 last_us;
    CpuTick prev_cpu[MAX_CORES + 1];
    double p_rd[MAX_DISK], p_wr[MAX_DISK], p_io[MAX_DISK];
    double p_rx[MAX_NET], p_tx[MAX_NET], p_rxp[MAX_NET], p_txp[MAX_NET];
    Nvml nvml;
} Sampler;

static Sampler *sampler_new(void)
{
    Sampler *s = g_new0(Sampler, 1);
    discover_cpu(&s->inv);
    discover_gpus(&s->inv);
    discover_disks(&s->inv);
    discover_nets(&s->inv);
    discover_sensors(&s->inv);
    return s;
}

static void sampler_free(Sampler *s)
{
    if (!s)
        return;
    if (s->nvml.lib) {
        if (s->nvml.shutdown)
            s->nvml.shutdown();
        dlclose(s->nvml.lib);
    }
    Inventory *v = &s->inv;
    g_free(v->cpu.model);
    for (int i = 0; i < 4; i++)
        g_free(v->cpu.cache[i]);
    for (int i = 0; i < v->ngpu; i++) {
        GpuInfo *g = &v->gpu[i];
        g_free(g->slot); g_free(g->name); g_free(g->vendor); g_free(g->driver); g_free(g->pci_id); g_free(g->link);
        g_free(g->sysdev); g_free(g->busy_path); g_free(g->vram_used_path); g_free(g->vram_total_path);
        g_free(g->freq_path); g_free(g->temp_path); g_free(g->power_path);
    }
    for (int i = 0; i < v->ndisk; i++) {
        DiskInfo *d = &v->disk[i];
        g_free(d->name); g_free(d->model); g_free(d->type); g_free(d->bus); g_free(d->hwmon);
        if (d->parts)
            g_ptr_array_unref(d->parts);
    }
    for (int i = 0; i < v->nnet; i++) {
        NetInfo *n = &v->net[i];
        g_free(n->name); g_free(n->title); g_free(n->kind); g_free(n->mac); g_free(n->driver);
    }
    for (int i = 0; i < v->nsens; i++) {
        SensInfo *x = &v->sens[i];
        g_free(x->label); g_free(x->chip); g_free(x->source); g_free(x->path);
    }
    g_free(s);
}

static void sample_cpu(Sampler *sm, Sample *o)
{
    gchar *txt;
    CpuTick cur[MAX_CORES + 1] = { { 0, 0 } };
    gboolean present[MAX_CORES + 1] = { FALSE };
    int maxn = 0;
    if (g_file_get_contents("/proc/stat", &txt, NULL, NULL)) {
        char *save = NULL;
        for (char *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (strncmp(line, "cpu", 3))
                break;
            int idx;
            char *rest;
            if (line[3] == ' ') {
                idx = 0;
                rest = line + 4;
            } else {
                int n = atoi(line + 3);
                if (n >= MAX_CORES)
                    continue;
                idx = n + 1;
                rest = strchr(line, ' ');
                maxn = MAX(maxn, n + 1);
                if (!rest)
                    continue;
            }
            unsigned long long v[8] = { 0 };
            sscanf(rest, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
            guint64 total = 0;
            for (int i = 0; i < 8; i++)
                total += v[i];
            cur[idx].total = total;
            cur[idx].busy = total - v[3] - v[4];
            present[idx] = TRUE;
        }
        g_free(txt);
    }
    o->ncores = maxn;
    o->cpu_util = NAN;
    for (int i = 0; i < MAX_CORES; i++)
        o->core[i] = NAN;
    if (sm->last_us) {
        for (int i = 0; i <= maxn; i++) {
            if (!present[i] || cur[i].total <= sm->prev_cpu[i].total)
                continue;
            double u = 100.0 * (double)(cur[i].busy - sm->prev_cpu[i].busy) / (double)(cur[i].total - sm->prev_cpu[i].total);
            u = CLAMP(u, 0, 100);
            if (i == 0)
                o->cpu_util = u;
            else
                o->core[i - 1] = u;
        }
    }
    memcpy(sm->prev_cpu, cur, sizeof cur);

    /* frequency */
    double sum = 0, fast = 0;
    int nf = 0;
    for (int i = 0; i < maxn; i++) {
        double khz;
        if (read_num_f(&khz, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i)) {
            sum += khz / 1000.0;
            fast = MAX(fast, khz / 1000.0);
            nf++;
        }
    }
    if (!nf && g_file_get_contents("/proc/cpuinfo", &txt, NULL, NULL)) {
        for (char *p = txt; (p = strstr(p, "cpu MHz")); p++) {
            char *c = strchr(p, ':');
            if (!c)
                break;
            double mhz = atof(c + 1);
            sum += mhz;
            fast = MAX(fast, mhz);
            nf++;
        }
        g_free(txt);
    }
    o->freq_avg = nf ? sum / nf : NAN;
    o->freq_fast = nf ? fast : NAN;
    if (!read_str(o->governor, sizeof o->governor, "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"))
        o->governor[0] = '\0';

    /* load, tasks, uptime */
    o->load[0] = o->load[1] = o->load[2] = NAN;
    o->threads = -1;
    if (g_file_get_contents("/proc/loadavg", &txt, NULL, NULL)) {
        int run = 0, ent = 0;
        if (sscanf(txt, "%lf %lf %lf %d/%d", &o->load[0], &o->load[1], &o->load[2], &run, &ent) == 5)
            o->threads = ent;
        g_free(txt);
    }
    o->procs = 0;
    GDir *d = g_dir_open("/proc", 0, NULL);
    if (d) {
        const char *n;
        while ((n = g_dir_read_name(d)))
            if (n[0] >= '1' && n[0] <= '9')
                o->procs++;
        g_dir_close(d);
    }
    o->uptime = NAN;
    if (g_file_get_contents("/proc/uptime", &txt, NULL, NULL)) {
        o->uptime = atof(txt);
        g_free(txt);
    }
}

static void sample_mem(Sample *o)
{
    static const struct { const char *k; gsize off; } t[] = {
        { "MemTotal:", G_STRUCT_OFFSET(Sample, mem_total) },       { "MemFree:", G_STRUCT_OFFSET(Sample, mem_free) },
        { "MemAvailable:", G_STRUCT_OFFSET(Sample, mem_avail) },   { "Buffers:", G_STRUCT_OFFSET(Sample, mem_buffers) },
        { "Cached:", G_STRUCT_OFFSET(Sample, mem_cached) },        { "SReclaimable:", G_STRUCT_OFFSET(Sample, sreclaim) },
        { "SwapTotal:", G_STRUCT_OFFSET(Sample, swap_total) },     { "SwapFree:", G_STRUCT_OFFSET(Sample, swap_free) },
        { "SwapCached:", G_STRUCT_OFFSET(Sample, swap_cached) },
    };
    o->mem_avail = -1;
    gchar *txt;
    if (!g_file_get_contents("/proc/meminfo", &txt, NULL, NULL))
        return;
    char *save = NULL;
    for (char *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        for (gsize i = 0; i < G_N_ELEMENTS(t); i++)
            if (g_str_has_prefix(line, t[i].k)) {
                *(double *)((char *)o + t[i].off) = atof(line + strlen(t[i].k)) * 1024.0;
                break;
            }
    g_free(txt);
    o->mem_cached += o->sreclaim;
    if (o->mem_avail < 0)
        o->mem_avail = o->mem_free + o->mem_buffers + o->mem_cached;
}

static double read_scaled(const char *path, double scale)
{
    double v;
    return path && read_num(&v, path) ? v * scale : NAN;
}

static void sample_gpu(Sampler *sm, Sample *o)
{
    const Inventory *inv = &sm->inv;
    gboolean want_nvml = FALSE;
    for (int i = 0; i < inv->ngpu; i++)
        want_nvml |= inv->gpu[i].nvidia;
    if (want_nvml && !sm->nvml.tried)
        nvml_load(&sm->nvml);

    for (int i = 0; i < inv->ngpu; i++) {
        const GpuInfo *g = &inv->gpu[i];
        GpuSample *s = &o->gpu[i];
        s->util = s->vram_used = s->vram_total = s->clock = s->power = s->temp = NAN;
        if (g->nvidia) {
            Nvml *n = &sm->nvml;
            if (!n->ok || g->nvml_idx >= n->count || !n->h[g->nvml_idx])
                continue;
            void *h = n->h[g->nvml_idx];
            NvUtil u;
            NvMem m;
            unsigned t, p, c;
            char nm[96];
            if (n->util && n->util(h, &u) == 0)
                s->util = u.gpu;
            if (n->mem && n->mem(h, &m) == 0) {
                s->vram_used = (double)m.used;
                s->vram_total = (double)m.total;
            }
            if (n->temp && n->temp(h, 0, &t) == 0)
                s->temp = t;
            if (n->power && n->power(h, &p) == 0)
                s->power = p / 1000.0;
            if (n->clock && n->clock(h, 0, &c) == 0)
                s->clock = c;
            if (n->name && n->name(h, nm, sizeof nm) == 0)
                s->name = g_strdup_printf("NVIDIA %s", g_str_has_prefix(nm, "NVIDIA ") ? nm + 7 : nm);
        } else {
            s->util = read_scaled(g->busy_path, 1);
            s->vram_used = read_scaled(g->vram_used_path, 1);
            s->vram_total = g->vram_total > 0 ? g->vram_total : read_scaled(g->vram_total_path, 1);
            s->clock = read_scaled(g->freq_path, g->freq_scale);
            s->power = read_scaled(g->power_path, g->power_scale);
            s->temp = read_scaled(g->temp_path, 1e-3);
        }
    }
}

static gboolean dev_belongs(const DiskInfo *d, const char *base, int depth)
{
    if (!strcmp(base, d->name))
        return TRUE;
    for (guint i = 0; i < d->parts->len; i++)
        if (!strcmp(base, d->parts->pdata[i]))
            return TRUE;
    if (depth >= 4)
        return FALSE;
    char *dir = g_strdup_printf("/sys/block/%s/slaves", base);
    GDir *g = g_dir_open(dir, 0, NULL);
    g_free(dir);
    gboolean found = FALSE;
    if (g) {
        const char *n;
        while (!found && (n = g_dir_read_name(g)))
            found = dev_belongs(d, n, depth + 1);
        g_dir_close(g);
    }
    return found;
}

static void sample_fs(const DiskInfo *d, DiskSample *o, const char *mounts)
{
    GString *out = g_string_new(NULL);
    GPtrArray *seen = g_ptr_array_new_with_free_func(g_free);
    double used = 0, total = 0;
    char *copy = g_strdup(mounts), *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char dev[256], mp[256], fs[32];
        if (sscanf(line, "%255s %255s %31s", dev, mp, fs) != 3 || !g_str_has_prefix(dev, "/dev/"))
            continue;
        char *rp = realpath(dev, NULL);
        char *base = g_path_get_basename(rp ? rp : dev);
        gboolean mine = dev_belongs(d, base, 0);
        g_free(base);
        if (mine) {
            GString *m = g_string_new(NULL);
            for (char *c = mp; *c; c++) {
                if (c[0] == '\\' && !strncmp(c + 1, "040", 3)) { g_string_append_c(m, ' '); c += 3; }
                else g_string_append_c(m, *c);
            }
            struct statvfs sv;
            if (statvfs(m->str, &sv) == 0 && sv.f_blocks > 0) {
                double ts = (double)sv.f_blocks * sv.f_frsize, us = ts - (double)sv.f_bfree * sv.f_frsize;
                gboolean dup = FALSE;
                for (guint i = 0; i < seen->len; i++)
                    dup |= !strcmp(seen->pdata[i], rp ? rp : dev);
                if (!dup) {
                    g_ptr_array_add(seen, g_strdup(rp ? rp : dev));
                    used += us;
                    total += ts;
                }
                char a[32], b[32];
                fmt_bytes(a, sizeof a, us, "");
                fmt_bytes(b, sizeof b, ts, "");
                g_string_append_printf(out, "%s  \xC2\xB7  %s  \xC2\xB7  %s of %s (%.0f%%)\n", m->str, fs, a, b, 100.0 * us / ts);
            }
            g_string_free(m, TRUE);
        }
        free(rp);
    }
    g_free(copy);
    g_ptr_array_unref(seen);
    o->used = used;
    o->total = total;
    if (out->len) {
        g_string_truncate(out, out->len - 1);
        o->fs_text = g_string_free(out, FALSE);
    } else {
        g_string_free(out, TRUE);
        o->used = NAN;
    }
}

static void sample_disk(Sampler *sm, Sample *o, double dt)
{
    const Inventory *inv = &sm->inv;
    gchar *txt, *mounts = NULL;
    for (int i = 0; i < MAX_DISK; i++)
        o->disk[i].read_bps = o->disk[i].write_bps = o->disk[i].active = o->disk[i].used = o->disk[i].total = o->disk[i].temp = NAN;
    if (g_file_get_contents("/proc/diskstats", &txt, NULL, NULL)) {
        char *save = NULL;
        for (char *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char name[64];
            double rs, ws, io;
            if (sscanf(line, "%*u %*u %63s %*f %*f %lf %*f %*f %*f %lf %*f %*f %lf", name, &rs, &ws, &io) != 4)
                continue;
            for (int i = 0; i < inv->ndisk; i++) {
                if (strcmp(name, inv->disk[i].name))
                    continue;
                DiskSample *d = &o->disk[i];
                if (dt > 0 && sm->p_rd[i] > 0) {
                    d->read_bps = MAX(0, (rs - sm->p_rd[i]) * 512.0 / dt);
                    d->write_bps = MAX(0, (ws - sm->p_wr[i]) * 512.0 / dt);
                    d->active = CLAMP((io - sm->p_io[i]) / (dt * 10.0), 0, 100); /* ms -> % */
                } else if (dt > 0) {
                    d->read_bps = d->write_bps = d->active = 0;
                }
                sm->p_rd[i] = MAX(rs, 1);
                sm->p_wr[i] = ws;
                sm->p_io[i] = io;
            }
        }
        g_free(txt);
    }
    g_file_get_contents("/proc/self/mounts", &mounts, NULL, NULL);
    for (int i = 0; i < inv->ndisk; i++) {
        if (mounts)
            sample_fs(&inv->disk[i], &o->disk[i], mounts);
        if (inv->disk[i].hwmon) {
            char *p = g_build_filename(inv->disk[i].hwmon, "temp1_input", NULL);
            o->disk[i].temp = read_scaled(p, 1e-3);
            g_free(p);
        }
    }
    g_free(mounts);
}

static void sample_net(Sampler *sm, Sample *o, double dt)
{
    const Inventory *inv = &sm->inv;
    gchar *txt;
    for (int i = 0; i < MAX_NET; i++) {
        NetSample *n = &o->net[i];
        n->rx_bps = n->tx_bps = n->rx_pps = n->tx_pps = n->speed = NAN;
    }
    if (!g_file_get_contents("/proc/net/dev", &txt, NULL, NULL))
        return;
    char *save = NULL;
    int ln = 0;
    for (char *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save), ln++) {
        if (ln < 2)
            continue;
        char *c = strchr(line, ':');
        if (!c)
            continue;
        *c = '\0';
        const char *name = g_strstrip(line);
        for (int i = 0; i < inv->nnet; i++) {
            if (strcmp(name, inv->net[i].name))
                continue;
            double rb, rp, re, rd, tb, tp, te, td;
            if (sscanf(c + 1, "%lf %lf %lf %lf %*f %*f %*f %*f %lf %lf %lf %lf", &rb, &rp, &re, &rd, &tb, &tp, &te, &td) != 8)
                continue;
            NetSample *n = &o->net[i];
            n->present = TRUE;
            n->rx_bytes = rb; n->tx_bytes = tb; n->rx_pkts = rp; n->tx_pkts = tp;
            n->rx_err = re; n->tx_err = te; n->rx_drop = rd; n->tx_drop = td;
            if (dt > 0 && sm->p_rx[i] > 0) {
                n->rx_bps = MAX(0, (rb - sm->p_rx[i]) / dt);
                n->tx_bps = MAX(0, (tb - sm->p_tx[i]) / dt);
                n->rx_pps = MAX(0, (rp - sm->p_rxp[i]) / dt);
                n->tx_pps = MAX(0, (tp - sm->p_txp[i]) / dt);
            } else if (dt > 0) {
                n->rx_bps = n->tx_bps = n->rx_pps = n->tx_pps = 0;
            }
            sm->p_rx[i] = MAX(rb, 1); sm->p_tx[i] = tb; sm->p_rxp[i] = rp; sm->p_txp[i] = tp;
            if (!read_str_f(n->state, sizeof n->state, "/sys/class/net/%s/operstate", inv->net[i].name))
                g_strlcpy(n->state, "unknown", sizeof n->state);
            double sp;
            if (read_num_f(&sp, "/sys/class/net/%s/speed", inv->net[i].name) && sp > 0)
                n->speed = sp * 1e6 / 8.0; /* Mbit/s -> bytes/s */
            if (!read_str_f(n->duplex, sizeof n->duplex, "/sys/class/net/%s/duplex", inv->net[i].name))
                n->duplex[0] = '\0';
        }
    }
    g_free(txt);
}

static Sample *sampler_collect(Sampler *sm)
{
    Sample *o = g_new0(Sample, 1);
    gint64 now = g_get_monotonic_time();
    double dt = sm->last_us ? (now - sm->last_us) / 1e6 : 0;
    sample_cpu(sm, o);
    sample_mem(o);
    sample_gpu(sm, o);
    sample_disk(sm, o, dt);
    sample_net(sm, o, dt);
    const Inventory *inv = &sm->inv;
    for (int i = 0; i < MAX_SENS; i++)
        o->temp[i] = NAN;
    for (int i = 0; i < inv->nsens; i++) {
        const SensInfo *s = &inv->sens[i];
        if (s->nvml_gpu >= 0)
            o->temp[i] = o->gpu[s->nvml_gpu].temp;
        else
            o->temp[i] = read_scaled(s->path, 1e-3);
    }
    sm->last_us = now;
    return o;
}

/* ------------------------------------------------------------------------ */
/* Page / Resource model                                                    */
/* ------------------------------------------------------------------------ */

typedef enum { RES_CPU, RES_GPU, RES_MEM, RES_DISK, RES_NET, RES_TEMP } ResKind;

/* stat slots, per resource kind */
enum { CS_PROCS, CS_THREADS, CS_UPTIME, CS_TEMP, CS_FMIN, CS_FMAX, CS_FFAST, CS_GOV, CS_L1, CS_L5, CS_L15,
       CS_SOCK, CS_CORES, CS_LOGI, CS_C1D, CS_C1I, CS_C2, CS_C3 };
enum { GS_TEMP, GS_POWER, GS_CLOCK, GS_VUSED, GS_VTOTAL, GS_VFREE, GS_VENDOR, GS_DRIVER, GS_SLOT, GS_PCIID, GS_LINK };
enum { MS_INUSE, MS_AVAIL, MS_CACHED, MS_TOTAL, MS_FREE, MS_BUF, MS_SWT, MS_SWU, MS_SWF, MS_SWC };
enum { DS_READ, DS_WRITE, DS_TEMP, DS_STATE, DS_NAME, DS_TYPE, DS_BUS, DS_CAP, DS_USED, DS_FREE, DS_FS };
enum { NS_STATE, NS_SPEED, NS_RX, NS_TX, NS_PIN, NS_POUT, NS_ERR, NS_DROP, NS_NAME, NS_TYPE, NS_DRV, NS_MAC, NS_DUP };
/* temperature tiles are indexed by sensor rank (0..3) */

#define UP_ARROW "\xE2\x86\x91"
#define DOWN_ARROW "\xE2\x86\x93"
#define DASH "\xE2\x80\x94"

typedef struct Page Page;

typedef struct {
    Page *page;
    ResKind kind;
    int idx;
    char *id, *title;
    Series ser[MAX_SER];

    /* navigation row */
    GtkWidget *row;
    Graph *spark;
    GtkLabel *r_val, *r_sub, *r_temp, *r_model;

    /* detail page */
    GtkWidget *detail, *gbox, *extra, *tiles, *groups;
    GtkLabel *h_model, *h_primary, *h_secondary;
    Graph *gr[MAX_GRAPHS];
    int ngr;
    Graph *vram_graph;
    gboolean model_set;
    GtkLabel *st[MAX_STATS];
    GPtrArray *exp_only; /* widgets shown only in Expanded view (not owned) */

    /* kind specific */
    GtkWidget *bars;                       /* CPU per-core */
    GtkWidget *bar_ram, *bar_swap;         /* memory meters */
    GtkLabel *bar_ram_l, *bar_swap_l;
    GtkLabel *sens_cur[MAX_SENS], *sens_min[MAX_SENS], *sens_max[MAX_SENS];
    double tmin[MAX_SENS], tmax[MAX_SENS];
} Resource;

struct Page {
    Sampler *sampler;
    const Inventory *inv;
    GtkWidget *root, *list, *sidebar, *stack, *btn_simple, *btn_expanded;
    GPtrArray *res;
    Resource *selected;
    gboolean expanded, busy, live;
    guint timer, interval;
    Sample *last;
};

static void res_free(gpointer data)
{
    Resource *r = data;
    g_free(r->id);
    g_free(r->title);
    g_ptr_array_unref(r->exp_only);
    g_free(r);
}

static void page_free(gpointer data)
{
    Page *p = data;
    if (p->timer)
        g_source_remove(p->timer);
    sample_free(p->last);
    g_ptr_array_unref(p->res);
    sampler_free(p->sampler);
    g_free(p);
}

/* ------------------------------------------------------------------------ */
/* Styling                                                                  */
/* ------------------------------------------------------------------------ */

static void perf_install_css(void)
{
    static gboolean done;
    GdkDisplay *dpy = gdk_display_get_default();
    if (done || !dpy)
        return;
    done = TRUE;
    static const char *css =
        ".perf-card { background-color: alpha(@window_fg_color, 0.045); border: 1px solid alpha(@window_fg_color, 0.08);"
        "  border-radius: 12px; padding: 10px 14px; }"
        ".perf-big { font-size: 30pt; font-weight: 300; font-feature-settings: \"tnum\"; }"
        ".perf-num { font-feature-settings: \"tnum\"; }"
        ".perf-key { opacity: 0.65; }"
        ".perf-side-scroll { background-color: alpha(@window_fg_color, 0.02); border-right: 1px solid alpha(@window_fg_color, 0.08); }"
        "list.perf-sidebar { background: transparent; }"
        "list.perf-sidebar > row { border-radius: 10px; margin: 2px 8px; padding: 6px 8px; transition: background-color 120ms; }"
        "list.perf-sidebar > row:hover { background-color: alpha(@window_fg_color, 0.06); }"
        "list.perf-sidebar > row:selected { background-color: alpha(@accent_bg_color, 0.22); color: inherit; }"
        "list.perf-sidebar > row:selected .perf-rowtitle { color: @accent_color; }";
    GtkCssProvider *prov = gtk_css_provider_new();
#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(prov, css);
#else
    gtk_css_provider_load_from_data(prov, css, -1);
#endif
    gtk_style_context_add_provider_for_display(dpy, GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);
}

/* ------------------------------------------------------------------------ */
/* Widget helpers                                                           */
/* ------------------------------------------------------------------------ */

static void add_classes(GtkWidget *w, const char *classes)
{
    if (!classes)
        return;
    char **v = g_strsplit(classes, " ", -1);
    for (int i = 0; v[i]; i++)
        if (*v[i])
            gtk_widget_add_css_class(w, v[i]);
    g_strfreev(v);
}

static GtkLabel *mk_label(const char *txt, const char *classes, float xalign)
{
    GtkWidget *l = gtk_label_new(txt);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    add_classes(l, classes);
    return GTK_LABEL(l);
}

static GtkWidget *flow_new(guint max_per_line)
{
    GtkWidget *f = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(f), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(f), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(f), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(f), max_per_line);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(f), 10);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(f), 10);
    return f;
}

static GtkWidget *flow_add(GtkWidget *flow, GtkWidget *child)
{
    gtk_flow_box_append(GTK_FLOW_BOX(flow), child);
    GtkWidget *p = gtk_widget_get_parent(child);
    gtk_widget_set_focusable(p, FALSE);
    return p;
}

static void set_temp(GtkLabel *l, double t)
{
    if (isnan(t))
        set_text(l, "");
    else
        set_value(l, UNIT_C, t);
}

static void set_rate(GtkLabel *l, const char *arrow, double v)
{
    char b[48];
    fmt_value(b, sizeof b, UNIT_BPS, v);
    set_textf(l, "%s %s", arrow, b);
}

static void set_bytes(GtkLabel *l, double v)
{
    char b[48];
    fmt_value(b, sizeof b, UNIT_BYTES, v);
    set_text(l, b);
}

/* ------------------------------------------------------------------------ */
/* Resource scaffolding                                                     */
/* ------------------------------------------------------------------------ */

static Resource *res_new(Page *p, ResKind k, int idx, const char *id, const char *title)
{
    Resource *r = g_new0(Resource, 1);
    r->page = p;
    r->kind = k;
    r->idx = idx;
    r->id = g_strdup(id);
    r->title = g_strdup(title);
    r->exp_only = g_ptr_array_new();
    for (int i = 0; i < MAX_SENS; i++)
        r->tmin[i] = r->tmax[i] = NAN;
    g_ptr_array_add(p->res, r);
    return r;
}

/* Navigation row: sparkline + title + primary value + secondary value (+ model in Expanded). */
static void res_row_init(Resource *r, Unit unit, GStyle style, double fixed_max)
{
    Page *p = r->page;
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    r->spark = graph_new(unit, style, fixed_max, TRUE);
    gtk_widget_set_hexpand(r->spark->area, FALSE);
    gtk_widget_set_valign(r->spark->area, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(hb), r->spark->area);

    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(vb, TRUE);
    gtk_widget_set_valign(vb, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(hb), vb);

    GtkWidget *l1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkLabel *title = mk_label(r->title, "heading perf-rowtitle", 0);
    gtk_widget_set_hexpand(GTK_WIDGET(title), TRUE);
    r->r_val = mk_label(DASH, "heading perf-num", 1);
    gtk_box_append(GTK_BOX(l1), GTK_WIDGET(title));
    gtk_box_append(GTK_BOX(l1), GTK_WIDGET(r->r_val));
    gtk_box_append(GTK_BOX(vb), l1);

    GtkWidget *l2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    r->r_sub = mk_label("", "caption dim-label perf-num", 0);
    gtk_label_set_width_chars(r->r_sub, 6);
    gtk_widget_set_hexpand(GTK_WIDGET(r->r_sub), TRUE);
    r->r_temp = mk_label("", "caption dim-label perf-num", 1);
    gtk_box_append(GTK_BOX(l2), GTK_WIDGET(r->r_sub));
    gtk_box_append(GTK_BOX(l2), GTK_WIDGET(r->r_temp));
    gtk_box_append(GTK_BOX(vb), l2);

    r->r_model = mk_label("", "caption dim-label", 0);
    gtk_label_set_width_chars(r->r_model, 6);
    gtk_box_append(GTK_BOX(vb), GTK_WIDGET(r->r_model));
    g_ptr_array_add(r->exp_only, r->r_model);

    gtk_list_box_append(GTK_LIST_BOX(p->list), hb);
    r->row = gtk_widget_get_parent(hb);
    g_object_set_data(G_OBJECT(r->row), "perf-res", r);
}

/* Detail skeleton: header, graph area, tiles, special cards, stat groups. */
static void res_detail_new(Resource *r, const char *caption)
{
    r->detail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start(r->detail, 20);
    gtk_widget_set_margin_end(r->detail, 20);
    gtk_widget_set_margin_top(r->detail, 16);
    gtk_widget_set_margin_bottom(r->detail, 20);

    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    GtkLabel *title = mk_label(r->title, "title-1", 0);
    r->h_model = mk_label("", "dim-label", 1);
    gtk_widget_set_hexpand(GTK_WIDGET(r->h_model), TRUE);
    gtk_widget_set_valign(GTK_WIDGET(r->h_model), GTK_ALIGN_END);
    gtk_label_set_selectable(r->h_model, FALSE);
    gtk_box_append(GTK_BOX(row1), GTK_WIDGET(title));
    gtk_box_append(GTK_BOX(row1), GTK_WIDGET(r->h_model));

    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    r->h_primary = mk_label(DASH, "perf-big", 0);
    GtkLabel *cap = mk_label(caption, "dim-label", 0);
    gtk_widget_set_valign(GTK_WIDGET(cap), GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(GTK_WIDGET(cap), 10);
    r->h_secondary = mk_label("", "title-3 perf-num", 1);
    gtk_widget_set_hexpand(GTK_WIDGET(r->h_secondary), TRUE);
    gtk_widget_set_valign(GTK_WIDGET(r->h_secondary), GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(GTK_WIDGET(r->h_secondary), 10);
    gtk_box_append(GTK_BOX(row2), GTK_WIDGET(r->h_primary));
    gtk_box_append(GTK_BOX(row2), GTK_WIDGET(cap));
    gtk_box_append(GTK_BOX(row2), GTK_WIDGET(r->h_secondary));
    gtk_box_append(GTK_BOX(hdr), row1);
    gtk_box_append(GTK_BOX(hdr), row2);

    r->gbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_vexpand(r->gbox, TRUE);
    r->tiles = flow_new(4);
    r->extra = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    r->groups = flow_new(4);

    gtk_box_append(GTK_BOX(r->detail), hdr);
    gtk_box_append(GTK_BOX(r->detail), r->gbox);
    gtk_box_append(GTK_BOX(r->detail), r->tiles);
    gtk_box_append(GTK_BOX(r->detail), r->extra);
    gtk_box_append(GTK_BOX(r->detail), r->groups);
    gtk_stack_add_named(GTK_STACK(r->page->stack), r->detail, r->id);
}

static void res_add_graph(Resource *r, const char *title, Graph *g, gboolean expanded_only)
{
    if (r->ngr >= MAX_GRAPHS)
        return;
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    add_classes(card, "perf-card");
    gtk_box_append(GTK_BOX(card), GTK_WIDGET(mk_label(title, "caption-heading dim-label", 0)));
    gtk_widget_set_vexpand(g->area, TRUE);
    gtk_box_append(GTK_BOX(card), g->area);
    gtk_widget_set_vexpand(card, r->ngr == 0);
    gtk_box_append(GTK_BOX(r->gbox), card);
    if (expanded_only)
        g_ptr_array_add(r->exp_only, card);
    r->gr[r->ngr++] = g;
}

static void res_tile(Resource *r, const char *caption, int idx)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    add_classes(card, "perf-card");
    r->st[idx] = mk_label(DASH, "title-3 perf-num", 0);
    gtk_box_append(GTK_BOX(card), GTK_WIDGET(mk_label(caption, "caption dim-label", 0)));
    gtk_box_append(GTK_BOX(card), GTK_WIDGET(r->st[idx]));
    flow_add(r->tiles, card);
}

static GtkWidget *res_group(Resource *r, const char *title)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    add_classes(card, "perf-card");
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_box_append(GTK_BOX(card), GTK_WIDGET(mk_label(title, "heading", 0)));
    gtk_box_append(GTK_BOX(card), grid);
    g_ptr_array_add(r->exp_only, flow_add(r->groups, card));
    return grid;
}

static void group_stat(Resource *r, GtkWidget *grid, int row, const char *key, int idx)
{
    GtkLabel *k = mk_label(key, "perf-key", 0);
    gtk_widget_set_hexpand(GTK_WIDGET(k), TRUE);
    GtkLabel *v = mk_label(DASH, "perf-num", 1);
    gtk_label_set_max_width_chars(v, 26);
    gtk_widget_set_halign(GTK_WIDGET(v), GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(k), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(v), 1, row, 1, 1);
    r->st[idx] = v;
}

static GtkWidget *extra_card(Resource *r, const char *title, gboolean expanded_only)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    add_classes(card, "perf-card");
    gtk_box_append(GTK_BOX(card), GTK_WIDGET(mk_label(title, "heading", 0)));
    gtk_box_append(GTK_BOX(r->extra), card);
    if (expanded_only)
        g_ptr_array_add(r->exp_only, card);
    return card;
}

/* ------------------------------------------------------------------------ */
/* Resource views (layout only; values arrive through the update functions)  */
/* ------------------------------------------------------------------------ */

static void performance_create_cpu_view(Page *p, Resource *r)
{
    const CpuInfo *c = &p->inv->cpu;
    res_row_init(r, UNIT_PCT, GSTYLE_AREA, 100);
    graph_add(r->spark, &r->ser[0], NULL, &COL_CPU);
    res_detail_new(r, "utilization");
    set_text(r->h_model, c->model);
    set_text(r->r_model, c->model);

    Graph *g = graph_new(UNIT_PCT, GSTYLE_AREA, 100, FALSE);
    graph_add(g, &r->ser[0], NULL, &COL_CPU);
    res_add_graph(r, "Overall utilization", g, FALSE);

    Graph *gf = graph_new(UNIT_MHZ, GSTYLE_LINE, isnan(c->fmax) ? 0 : c->fmax * 1.05, FALSE);
    graph_add(gf, &r->ser[1], NULL, &COL_CACHE);
    res_add_graph(r, "Frequency", gf, TRUE);

    gboolean has_temp = p->inv->cpu_sens >= 0;
    if (has_temp) {
        Graph *gt = graph_new(UNIT_C, GSTYLE_LINE, 100, FALSE);
        graph_add(gt, &r->ser[2], NULL, &COL_TEMP);
        res_add_graph(r, "Temperature", gt, TRUE);
    }

    res_tile(r, "Processes", CS_PROCS);
    res_tile(r, "Threads", CS_THREADS);
    res_tile(r, "Up time", CS_UPTIME);
    if (has_temp)
        res_tile(r, "Temperature", CS_TEMP);

    GtkWidget *card = extra_card(r, "Per-core utilization", TRUE);
    r->bars = bars_new();
    gtk_box_append(GTK_BOX(card), r->bars);

    GtkWidget *gr = res_group(r, "Frequency");
    group_stat(r, gr, 0, "Minimum", CS_FMIN);
    group_stat(r, gr, 1, "Maximum", CS_FMAX);
    group_stat(r, gr, 2, "Fastest core now", CS_FFAST);
    group_stat(r, gr, 3, "Governor", CS_GOV);
    gr = res_group(r, "Load average");
    group_stat(r, gr, 0, "1 minute", CS_L1);
    group_stat(r, gr, 1, "5 minutes", CS_L5);
    group_stat(r, gr, 2, "15 minutes", CS_L15);
    gr = res_group(r, "Topology");
    group_stat(r, gr, 0, "Sockets", CS_SOCK);
    group_stat(r, gr, 1, "Cores", CS_CORES);
    group_stat(r, gr, 2, "Logical processors", CS_LOGI);
    gr = res_group(r, "Caches");
    group_stat(r, gr, 0, "L1 data", CS_C1D);
    group_stat(r, gr, 1, "L1 instruction", CS_C1I);
    group_stat(r, gr, 2, "L2", CS_C2);
    group_stat(r, gr, 3, "L3", CS_C3);

    set_value(r->st[CS_FMIN], UNIT_MHZ, c->fmin);
    set_value(r->st[CS_FMAX], UNIT_MHZ, c->fmax);
    set_textf(r->st[CS_SOCK], "%d", c->sockets);
    set_textf(r->st[CS_CORES], "%d", c->cores);
    set_textf(r->st[CS_LOGI], "%d", c->logical);
    set_text(r->st[CS_C1D], c->cache[0] ? c->cache[0] : DASH);
    set_text(r->st[CS_C1I], c->cache[1] ? c->cache[1] : DASH);
    set_text(r->st[CS_C2], c->cache[2] ? c->cache[2] : DASH);
    set_text(r->st[CS_C3], c->cache[3] ? c->cache[3] : DASH);
}

static void performance_create_gpu_view(Page *p, Resource *r)
{
    const GpuInfo *g = &p->inv->gpu[r->idx];
    res_row_init(r, g->has_util ? UNIT_PCT : UNIT_MHZ, g->has_util ? GSTYLE_AREA : GSTYLE_LINE, g->has_util ? 100 : 0);
    graph_add(r->spark, g->has_util ? &r->ser[0] : &r->ser[4], NULL, &COL_GPU);
    res_detail_new(r, g->has_util ? "utilization" : "clock");
    set_text(r->h_model, g->name);
    set_text(r->r_model, g->name);

    if (g->has_util) {
        Graph *x = graph_new(UNIT_PCT, GSTYLE_AREA, 100, FALSE);
        graph_add(x, &r->ser[0], NULL, &COL_GPU);
        res_add_graph(r, "GPU utilization", x, FALSE);
    }
    if (g->has_vram) {
        Graph *x = graph_new(UNIT_BYTES, GSTYLE_AREA, g->vram_total, FALSE);
        graph_add(x, &r->ser[1], NULL, &COL_MEM);
        res_add_graph(r, "Video memory", x, r->ngr > 0);
        r->vram_graph = x;
    }
    if (g->has_clock) {
        Graph *x = graph_new(UNIT_MHZ, GSTYLE_LINE, 0, FALSE);
        graph_add(x, &r->ser[4], NULL, &COL_CACHE);
        res_add_graph(r, "Clock", x, r->ngr > 0);
    }
    if (g->has_temp) {
        Graph *x = graph_new(UNIT_C, GSTYLE_LINE, 100, FALSE);
        graph_add(x, &r->ser[2], NULL, &COL_TEMP);
        res_add_graph(r, "Temperature", x, r->ngr > 0);
    }
    if (g->has_power) {
        Graph *x = graph_new(UNIT_W, GSTYLE_LINE, 0, FALSE);
        graph_add(x, &r->ser[3], NULL, &COL_POWER);
        res_add_graph(r, "Power draw", x, r->ngr > 0);
    }

    if (g->has_temp)
        res_tile(r, "Temperature", GS_TEMP);
    if (g->has_power)
        res_tile(r, "Power", GS_POWER);
    if (g->has_clock && g->has_util)
        res_tile(r, "Clock", GS_CLOCK);

    GtkWidget *gr;
    if (g->has_vram) {
        gr = res_group(r, "Video memory");
        group_stat(r, gr, 0, "Used", GS_VUSED);
        group_stat(r, gr, 1, "Free", GS_VFREE);
        group_stat(r, gr, 2, "Total", GS_VTOTAL);
    }
    gr = res_group(r, "Device");
    group_stat(r, gr, 0, "Vendor", GS_VENDOR);
    group_stat(r, gr, 1, "Driver", GS_DRIVER);
    gr = res_group(r, "PCI");
    group_stat(r, gr, 0, "Slot", GS_SLOT);
    group_stat(r, gr, 1, "Device ID", GS_PCIID);
    group_stat(r, gr, 2, "Link", GS_LINK);

    set_text(r->st[GS_VENDOR], g->vendor);
    set_text(r->st[GS_DRIVER], g->driver ? g->driver : DASH);
    set_text(r->st[GS_SLOT], g->slot);
    set_text(r->st[GS_PCIID], g->pci_id);
    set_text(r->st[GS_LINK], g->link ? g->link : DASH);
}

static void performance_create_memory_view(Page *p, Resource *r)
{
    const Inventory *inv = p->inv;
    res_row_init(r, UNIT_PCT, GSTYLE_AREA, 100);
    graph_add(r->spark, &r->ser[3], NULL, &COL_MEM);
    res_detail_new(r, "in use");
    char b[48];
    fmt_value(b, sizeof b, UNIT_BYTES, inv->mem_total);
    set_textf(r->h_model, "%s RAM", b);

    Graph *g = graph_new(UNIT_BYTES, GSTYLE_AREA, inv->mem_total, FALSE);
    graph_add(g, &r->ser[0], "In use", &COL_MEM);
    graph_add(g, &r->ser[1], "Cached", &COL_CACHE);
    res_add_graph(r, "Memory usage", g, FALSE);
    if (inv->swap_total > 0) {
        Graph *sg = graph_new(UNIT_BYTES, GSTYLE_AREA, inv->swap_total, FALSE);
        graph_add(sg, &r->ser[2], NULL, &COL_SWAP);
        res_add_graph(r, "Swap usage", sg, TRUE);
    }

    res_tile(r, "In use", MS_INUSE);
    res_tile(r, "Available", MS_AVAIL);
    res_tile(r, "Cached", MS_CACHED);

    GtkWidget *card = extra_card(r, "Current usage", FALSE);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_box_append(GTK_BOX(card), grid);
    r->bar_ram = gtk_progress_bar_new();
    gtk_widget_set_hexpand(r->bar_ram, TRUE);
    gtk_widget_set_valign(r->bar_ram, GTK_ALIGN_CENTER);
    r->bar_ram_l = mk_label("", "perf-num", 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(mk_label("RAM", "perf-key", 0)), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), r->bar_ram, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(r->bar_ram_l), 2, 0, 1, 1);
    gtk_label_set_width_chars(r->bar_ram_l, 18);
    if (inv->swap_total > 0) {
        r->bar_swap = gtk_progress_bar_new();
        gtk_widget_set_hexpand(r->bar_swap, TRUE);
        gtk_widget_set_valign(r->bar_swap, GTK_ALIGN_CENTER);
        r->bar_swap_l = mk_label("", "perf-num", 1);
        gtk_label_set_width_chars(r->bar_swap_l, 18);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(mk_label("Swap", "perf-key", 0)), 0, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), r->bar_swap, 1, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(r->bar_swap_l), 2, 1, 1, 1);
    }

    GtkWidget *gr = res_group(r, "RAM");
    group_stat(r, gr, 0, "Total", MS_TOTAL);
    group_stat(r, gr, 1, "Free (unused)", MS_FREE);
    group_stat(r, gr, 2, "Buffers", MS_BUF);
    if (inv->swap_total > 0) {
        gr = res_group(r, "Swap");
        group_stat(r, gr, 0, "Total", MS_SWT);
        group_stat(r, gr, 1, "Used", MS_SWU);
        group_stat(r, gr, 2, "Free", MS_SWF);
        group_stat(r, gr, 3, "Cached", MS_SWC);
        set_bytes(r->st[MS_SWT], inv->swap_total);
    }
    set_bytes(r->st[MS_TOTAL], inv->mem_total);
}

static void performance_create_disk_view(Page *p, Resource *r)
{
    const DiskInfo *d = &p->inv->disk[r->idx];
    res_row_init(r, UNIT_BPS, GSTYLE_AREA, 0);
    graph_add(r->spark, &r->ser[0], NULL, &COL_READ);
    graph_add(r->spark, &r->ser[1], NULL, &COL_WRITE);
    res_detail_new(r, "active time");
    char cap[48];
    fmt_value(cap, sizeof cap, UNIT_BYTES, d->capacity);
    set_textf(r->h_model, "%s (%s)", d->model, d->name);
    set_text(r->r_model, d->model);
    set_textf(r->h_secondary, "%s \xC2\xB7 %s", d->type, cap);

    Graph *g = graph_new(UNIT_BPS, GSTYLE_AREA, 0, FALSE);
    graph_add(g, &r->ser[0], "Read", &COL_READ);
    graph_add(g, &r->ser[1], "Write", &COL_WRITE);
    res_add_graph(r, "Disk throughput", g, FALSE);
    Graph *ga = graph_new(UNIT_PCT, GSTYLE_AREA, 100, FALSE);
    graph_add(ga, &r->ser[2], NULL, &COL_CACHE);
    res_add_graph(r, "Active time", ga, TRUE);

    res_tile(r, "Read", DS_READ);
    res_tile(r, "Write", DS_WRITE);
    if (d->hwmon)
        res_tile(r, "Temperature", DS_TEMP);
    res_tile(r, "State", DS_STATE);

    GtkWidget *card = extra_card(r, "Partitions and mounted filesystems", TRUE);
    r->st[DS_FS] = mk_label("", "perf-num", 0);
    gtk_label_set_ellipsize(r->st[DS_FS], PANGO_ELLIPSIZE_NONE);
    gtk_label_set_wrap(r->st[DS_FS], TRUE);
    gtk_label_set_selectable(r->st[DS_FS], TRUE);
    gtk_box_append(GTK_BOX(card), GTK_WIDGET(r->st[DS_FS]));

    GtkWidget *gr = res_group(r, "Device");
    group_stat(r, gr, 0, "Name", DS_NAME);
    group_stat(r, gr, 1, "Type", DS_TYPE);
    group_stat(r, gr, 2, "Interface", DS_BUS);
    group_stat(r, gr, 3, "Capacity", DS_CAP);
    gr = res_group(r, "Mounted space");
    group_stat(r, gr, 0, "Used", DS_USED);
    group_stat(r, gr, 1, "Free", DS_FREE);

    set_textf(r->st[DS_NAME], "/dev/%s", d->name);
    set_text(r->st[DS_TYPE], d->type);
    set_text(r->st[DS_BUS], d->bus);
    set_text(r->st[DS_CAP], cap);
    char state[32] = "";
    if (!read_str_f(state, sizeof state, "/sys/block/%s/device/state", d->name))
        g_strlcpy(state, "online", sizeof state);
    double ro = 0;
    read_num_f(&ro, "/sys/block/%s/ro", d->name);
    set_textf(r->st[DS_STATE], "%s%s", state, ro > 0 ? " (read-only)" : "");
}

static void performance_create_network_view(Page *p, Resource *r)
{
    const NetInfo *n = &p->inv->net[r->idx];
    res_row_init(r, UNIT_BPS, GSTYLE_AREA, 0);
    graph_add(r->spark, &r->ser[0], NULL, &COL_DOWN);
    graph_add(r->spark, &r->ser[1], NULL, &COL_UP);
    res_detail_new(r, "download");
    set_textf(r->h_model, "%s%s%s", n->name, n->driver ? " \xC2\xB7 " : "", n->driver ? n->driver : "");
    set_text(r->r_model, n->name);

    Graph *g = graph_new(UNIT_BPS, GSTYLE_AREA, 0, FALSE);
    graph_add(g, &r->ser[0], "Download", &COL_DOWN);
    graph_add(g, &r->ser[1], "Upload", &COL_UP);
    res_add_graph(r, "Network throughput", g, FALSE);
    Graph *gp = graph_new(UNIT_PPS, GSTYLE_LINE, 0, FALSE);
    graph_add(gp, &r->ser[2], "Received", &COL_DOWN);
    graph_add(gp, &r->ser[3], "Sent", &COL_UP);
    res_add_graph(r, "Packet rate", gp, TRUE);

    res_tile(r, "State", NS_STATE);
    res_tile(r, "Link speed", NS_SPEED);
    res_tile(r, "Total received", NS_RX);
    res_tile(r, "Total sent", NS_TX);

    GtkWidget *gr = res_group(r, "Packets");
    group_stat(r, gr, 0, "Received", NS_PIN);
    group_stat(r, gr, 1, "Sent", NS_POUT);
    group_stat(r, gr, 2, "Errors (in / out)", NS_ERR);
    group_stat(r, gr, 3, "Dropped (in / out)", NS_DROP);
    gr = res_group(r, "Interface");
    group_stat(r, gr, 0, "Name", NS_NAME);
    group_stat(r, gr, 1, "Type", NS_TYPE);
    group_stat(r, gr, 2, "Driver", NS_DRV);
    group_stat(r, gr, 3, "MAC address", NS_MAC);
    group_stat(r, gr, 4, "Duplex", NS_DUP);
    set_text(r->st[NS_NAME], n->name);
    set_text(r->st[NS_TYPE], n->kind);
    set_text(r->st[NS_DRV], n->driver ? n->driver : DASH);
    set_text(r->st[NS_MAC], *n->mac ? n->mac : DASH);
}

static void performance_create_temperature_view(Page *p, Resource *r)
{
    const Inventory *inv = p->inv;
    res_row_init(r, UNIT_C, GSTYLE_LINE, 100);
    for (int i = 0; i < MIN(3, inv->nsens); i++)
        graph_add(r->spark, &r->ser[i], NULL, &COL_SENS[i]);
    res_detail_new(r, "hottest");
    set_textf(r->h_model, "%d sensor%s", inv->nsens, inv->nsens == 1 ? "" : "s");

    Graph *g = graph_new(UNIT_C, GSTYLE_LINE, 100, FALSE);
    for (int i = 0; i < MIN(MAX_SER, inv->nsens); i++)
        graph_add(g, &r->ser[i], inv->sens[i].label, &COL_SENS[i]);
    res_add_graph(r, "Temperature history (CPU, GPU, NVMe and system sensors first)", g, FALSE);

    static const char *cats[] = { "CPU", "GPU", "NVMe", "System" };
    for (int k = 0; k < 4; k++) {
        gboolean any = FALSE;
        for (int i = 0; i < inv->nsens; i++)
            any |= inv->sens[i].rank == k;
        if (any)
            res_tile(r, cats[k], k);
    }

    GtkWidget *card = extra_card(r, "Sensors", FALSE);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_box_append(GTK_BOX(card), grid);
    static const char *heads[] = { "Sensor", "Current", "Min", "Max", "Source" };
    for (int c = 0; c < 5; c++) {
        GtkLabel *h = mk_label(heads[c], "perf-key caption-heading", c >= 1 && c <= 3 ? 1 : 0);
        gtk_widget_set_hexpand(GTK_WIDGET(h), c == 0 || c == 4);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(h), c, 0, 1, 1);
    }
    for (int i = 0; i < inv->nsens; i++) {
        const SensInfo *s = &inv->sens[i];
        GtkLabel *name = mk_label(s->label, "", 0);
        r->sens_cur[i] = mk_label(DASH, "perf-num", 1);
        r->sens_min[i] = mk_label(DASH, "perf-num dim-label", 1);
        r->sens_max[i] = mk_label(DASH, "perf-num dim-label", 1);
        GtkLabel *src = mk_label(s->source, "caption dim-label", 0);
        gtk_label_set_max_width_chars(src, 28);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(name), 0, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(r->sens_cur[i]), 1, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(r->sens_min[i]), 2, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(r->sens_max[i]), 3, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(src), 4, i + 1, 1, 1);
    }
}

/* ------------------------------------------------------------------------ */
/* Updates: push samples into history (always), refresh widgets (when shown) */
/* ------------------------------------------------------------------------ */

static void update_cpu(Page *p, Resource *r, const Sample *s, gboolean push)
{
    const Inventory *inv = p->inv;
    double t = inv->cpu_sens >= 0 ? s->temp[inv->cpu_sens] : NAN;
    if (push) {
        series_push(&r->ser[0], s->cpu_util);
        series_push(&r->ser[1], s->freq_avg);
        series_push(&r->ser[2], t);
    }
    if (!p->live)
        return;
    set_value(r->h_primary, UNIT_PCT, s->cpu_util);
    set_value(r->h_secondary, UNIT_MHZ, s->freq_avg);
    set_value(r->r_val, UNIT_PCT, s->cpu_util);
    set_value(r->r_sub, UNIT_MHZ, s->freq_avg);
    set_temp(r->r_temp, t);
    if (s->procs > 0) set_textf(r->st[CS_PROCS], "%d", s->procs);
    if (s->threads >= 0) set_textf(r->st[CS_THREADS], "%d", s->threads);
    char b[48];
    fmt_uptime(b, sizeof b, s->uptime);
    set_text(r->st[CS_UPTIME], b);
    set_temp(r->st[CS_TEMP], t);
    set_value(r->st[CS_FFAST], UNIT_MHZ, s->freq_fast);
    set_text(r->st[CS_GOV], s->governor[0] ? s->governor : DASH);
    for (int i = 0; i < 3; i++) {
        if (isnan(s->load[i])) continue;
        set_textf(r->st[CS_L1 + i], "%.2f", s->load[i]);
    }
    bars_set(r->bars, s->core, s->ncores > 0 ? s->ncores : inv->cpu.logical);
}

static void update_gpu(Page *p, Resource *r, const Sample *s, gboolean push)
{
    const GpuInfo *g = &p->inv->gpu[r->idx];
    const GpuSample *gs = &s->gpu[r->idx];
    if (push) {
        series_push(&r->ser[0], gs->util);
        series_push(&r->ser[1], gs->vram_used);
        series_push(&r->ser[2], gs->temp);
        series_push(&r->ser[3], gs->power);
        series_push(&r->ser[4], gs->clock);
        if (r->vram_graph && r->vram_graph->fixed_max <= 0 && gs->vram_total > 0)
            graph_set_fixed_max(r->vram_graph, gs->vram_total);
        if (gs->name && !r->model_set) {
            r->model_set = TRUE;
            set_text(r->h_model, gs->name);
            set_text(r->r_model, gs->name);
        }
    }
    if (!p->live)
        return;
    char a[48], b[48];
    if (g->has_util) {
        set_value(r->h_primary, UNIT_PCT, gs->util);
        set_value(r->r_val, UNIT_PCT, gs->util);
    } else {
        set_value(r->h_primary, UNIT_MHZ, gs->clock);
        set_value(r->r_val, UNIT_MHZ, gs->clock);
    }
    if (g->has_vram) {
        fmt_value(a, sizeof a, UNIT_BYTES, gs->vram_used);
        fmt_value(b, sizeof b, UNIT_BYTES, gs->vram_total);
        set_textf(r->h_secondary, "%s / %s VRAM", a, b);
        set_textf(r->r_sub, "%s / %s", a, b);
    } else {
        set_value(r->h_secondary, UNIT_W, gs->power);
        set_value(r->r_sub, UNIT_MHZ, gs->clock);
    }
    set_temp(r->r_temp, gs->temp);
    set_temp(r->st[GS_TEMP], gs->temp);
    set_value(r->st[GS_POWER], UNIT_W, gs->power);
    set_value(r->st[GS_CLOCK], UNIT_MHZ, gs->clock);
    set_bytes(r->st[GS_VUSED], gs->vram_used);
    set_bytes(r->st[GS_VTOTAL], gs->vram_total);
    set_bytes(r->st[GS_VFREE], gs->vram_total - gs->vram_used);
}

static void update_mem(Page *p, Resource *r, const Sample *s, gboolean push)
{
    double total = s->mem_total > 0 ? s->mem_total : p->inv->mem_total;
    double used = total - s->mem_avail;
    double pct = total > 0 ? 100.0 * used / total : NAN;
    double swu = s->swap_total - s->swap_free;
    if (push) {
        series_push(&r->ser[0], used);
        series_push(&r->ser[1], MIN(s->mem_cached + s->mem_buffers, total));
        series_push(&r->ser[2], swu);
        series_push(&r->ser[3], pct);
    }
    if (!p->live)
        return;
    char a[48], b[48];
    set_value(r->h_primary, UNIT_PCT, pct);
    set_value(r->r_val, UNIT_PCT, pct);
    fmt_value(a, sizeof a, UNIT_BYTES, used);
    fmt_value(b, sizeof b, UNIT_BYTES, total);
    set_textf(r->r_sub, "%s / %s", a, b);
    fmt_value(a, sizeof a, UNIT_BYTES, s->mem_avail);
    set_textf(r->h_secondary, "%s available", a);
    set_bytes(r->st[MS_INUSE], used);
    set_bytes(r->st[MS_AVAIL], s->mem_avail);
    set_bytes(r->st[MS_CACHED], s->mem_cached);
    set_bytes(r->st[MS_FREE], s->mem_free);
    set_bytes(r->st[MS_BUF], s->mem_buffers);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(r->bar_ram), total > 0 ? CLAMP(used / total, 0, 1) : 0);
    fmt_value(a, sizeof a, UNIT_BYTES, used);
    set_textf(r->bar_ram_l, "%s / %s", a, b);
    if (r->bar_swap) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(r->bar_swap),
                                      s->swap_total > 0 ? CLAMP(swu / s->swap_total, 0, 1) : 0);
        fmt_value(a, sizeof a, UNIT_BYTES, swu);
        fmt_value(b, sizeof b, UNIT_BYTES, s->swap_total);
        set_textf(r->bar_swap_l, "%s / %s", a, b);
        set_bytes(r->st[MS_SWU], swu);
        set_bytes(r->st[MS_SWF], s->swap_free);
        set_bytes(r->st[MS_SWC], s->swap_cached);
    }
}

static void update_disk(Page *p, Resource *r, const Sample *s, gboolean push)
{
    const DiskSample *d = &s->disk[r->idx];
    if (push) {
        series_push(&r->ser[0], d->read_bps);
        series_push(&r->ser[1], d->write_bps);
        series_push(&r->ser[2], d->active);
    }
    if (!p->live)
        return;
    char a[48], b[48];
    set_value(r->h_primary, UNIT_PCT, d->active);
    set_value(r->r_val, UNIT_PCT, d->active);
    fmt_value(a, sizeof a, UNIT_BPS, d->read_bps);
    fmt_value(b, sizeof b, UNIT_BPS, d->write_bps);
    set_textf(r->r_sub, "R %s \xC2\xB7 W %s", a, b);
    set_temp(r->r_temp, d->temp);
    set_text(r->st[DS_READ], a);
    set_text(r->st[DS_WRITE], b);
    set_temp(r->st[DS_TEMP], d->temp);
    if (isnan(d->used)) {
        set_text(r->st[DS_USED], DASH);
        set_text(r->st[DS_FREE], DASH);
    } else {
        set_bytes(r->st[DS_USED], d->used);
        set_bytes(r->st[DS_FREE], d->total - d->used);
    }
    set_text(r->st[DS_FS], d->fs_text ? d->fs_text : "No mounted filesystems on this disk");
}

static const char *net_state_str(const NetSample *n)
{
    if (!n->present) return "Not present";
    if (!strcmp(n->state, "up")) return "Connected";
    if (!strcmp(n->state, "down")) return "Disconnected";
    if (!strcmp(n->state, "dormant")) return "Connecting";
    return "Active";
}

static void update_net(Page *p, Resource *r, const Sample *s, gboolean push)
{
    const NetSample *n = &s->net[r->idx];
    if (push) {
        series_push(&r->ser[0], n->rx_bps);
        series_push(&r->ser[1], n->tx_bps);
        series_push(&r->ser[2], n->rx_pps);
        series_push(&r->ser[3], n->tx_pps);
    }
    if (!p->live)
        return;
    set_rate(r->h_primary, DOWN_ARROW, n->rx_bps);
    set_rate(r->h_secondary, UP_ARROW, n->tx_bps);
    set_rate(r->r_val, DOWN_ARROW, n->rx_bps);
    if (n->present && !strcmp(n->state, "down"))
        set_text(r->r_sub, "Disconnected");
    else
        set_rate(r->r_sub, UP_ARROW, n->tx_bps);
    set_text(r->st[NS_STATE], net_state_str(n));
    if (isnan(n->speed)) {
        set_text(r->st[NS_SPEED], DASH);
    } else {
        set_textf(r->st[NS_SPEED], n->speed >= 125e6 ? "%.3g Gbit/s" : "%.0f Mbit/s", n->speed >= 125e6 ? n->speed * 8 / 1e9 : n->speed * 8 / 1e6);
    }
    if (n->present) {
        set_bytes(r->st[NS_RX], n->rx_bytes);
        set_bytes(r->st[NS_TX], n->tx_bytes);
        set_textf(r->st[NS_PIN], "%.0f", n->rx_pkts);
        set_textf(r->st[NS_POUT], "%.0f", n->tx_pkts);
        set_textf(r->st[NS_ERR], "%.0f / %.0f", n->rx_err, n->tx_err);
        set_textf(r->st[NS_DROP], "%.0f / %.0f", n->rx_drop, n->tx_drop);
    }
    set_text(r->st[NS_DUP], n->duplex[0] ? n->duplex : DASH);
}

static void update_temp(Page *p, Resource *r, const Sample *s, gboolean push)
{
    const Inventory *inv = p->inv;
    if (push) {
        for (int i = 0; i < inv->nsens; i++) {
            double t = s->temp[i];
            if (i < MAX_SER)
                series_push(&r->ser[i], t);
            if (!isnan(t)) {
                if (isnan(r->tmin[i]) || t < r->tmin[i]) r->tmin[i] = t;
                if (isnan(r->tmax[i]) || t > r->tmax[i]) r->tmax[i] = t;
            }
        }
    }
    if (!p->live)
        return;
    double hi = NAN, cat[4] = { NAN, NAN, NAN, NAN };
    int hi_i = -1;
    for (int i = 0; i < inv->nsens; i++) {
        double t = s->temp[i];
        if (isnan(t))
            continue;
        if (isnan(hi) || t > hi) { hi = t; hi_i = i; }
        int k = inv->sens[i].rank;
        if (isnan(cat[k]) || t > cat[k]) cat[k] = t;
        set_value(r->sens_cur[i], UNIT_C, t);
        set_value(r->sens_min[i], UNIT_C, r->tmin[i]);
        set_value(r->sens_max[i], UNIT_C, r->tmax[i]);
    }
    set_value(r->h_primary, UNIT_C, hi);
    set_value(r->r_val, UNIT_C, hi);
    set_text(r->h_secondary, hi_i >= 0 ? inv->sens[hi_i].label : "");
    set_text(r->r_sub, hi_i >= 0 ? inv->sens[hi_i].label : "");
    for (int k = 0; k < 4; k++)
        if (r->st[k])
            set_value(r->st[k], UNIT_C, cat[k]);
}

static void performance_update_selected_resource(Page *p)
{
    if (!p->selected)
        return;
    for (int i = 0; i < p->selected->ngr; i++)
        graph_refresh(p->selected->gr[i]);
}

static void performance_update(Page *p, const Sample *s, gboolean push)
{
    for (guint i = 0; i < p->res->len; i++) {
        Resource *r = p->res->pdata[i];
        switch (r->kind) {
        case RES_CPU:  update_cpu(p, r, s, push); break;
        case RES_GPU:  update_gpu(p, r, s, push); break;
        case RES_MEM:  update_mem(p, r, s, push); break;
        case RES_DISK: update_disk(p, r, s, push); break;
        case RES_NET:  update_net(p, r, s, push); break;
        case RES_TEMP: update_temp(p, r, s, push); break;
        }
    }
    if (!p->live)
        return;
    for (guint i = 0; i < p->res->len; i++)
        graph_refresh(((Resource *)p->res->pdata[i])->spark);
    performance_update_selected_resource(p);
}

/* ------------------------------------------------------------------------ */
/* Monitoring: timer + worker thread (never blocks the GTK main loop)        */
/* ------------------------------------------------------------------------ */

static void perf_sample_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    Page *p = data;
    g_task_return_pointer(task, sampler_collect(p->sampler), (GDestroyNotify)sample_free);
}

static void perf_sample_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    (void)ud;
    Page *p = g_object_get_data(src, "perf-page");
    Sample *s = g_task_propagate_pointer(G_TASK(res), NULL);
    p->busy = FALSE;
    if (!s)
        return;
    sample_free(p->last);
    p->last = s;
    performance_update(p, s, TRUE);
}

static gboolean perf_tick(gpointer data)
{
    Page *p = data;
    if (p->busy) /* previous sample still running: skip this tick */
        return G_SOURCE_CONTINUE;
    p->busy = TRUE;
    GTask *t = g_task_new(p->root, NULL, perf_sample_done, NULL);
    g_task_set_task_data(t, p, NULL);
    g_task_run_in_thread(t, perf_sample_thread);
    g_object_unref(t);
    return G_SOURCE_CONTINUE;
}

static void performance_start_monitoring(Page *p)
{
    perf_tick(p);
    p->timer = g_timeout_add(p->interval, perf_tick, p);
}

/* ------------------------------------------------------------------------ */
/* View modes and selection                                                 */
/* ------------------------------------------------------------------------ */

static void apply_view(Page *p, gboolean expanded)
{
    p->expanded = expanded;
    gtk_widget_set_size_request(p->sidebar, expanded ? 330 : 240, -1);
    for (guint i = 0; i < p->res->len; i++) {
        Resource *r = p->res->pdata[i];
        gtk_widget_set_size_request(r->spark->area, expanded ? 92 : 60, expanded ? 46 : 34);
        for (guint k = 0; k < r->exp_only->len; k++)
            gtk_widget_set_visible(r->exp_only->pdata[k], expanded);
        for (int k = 0; k < r->ngr; k++)
            gtk_widget_set_size_request(r->gr[k]->area, -1, k == 0 ? (expanded ? 280 : 200) : 130);
    }
}

/* Simple view: compact navigation column, summary graph and key tiles only. */
static void performance_create_simple_view(Page *p) { apply_view(p, FALSE); }

/* Expanded view: wide navigation cards, large graphs, every statistic group. */
static void performance_create_expanded_view(Page *p) { apply_view(p, TRUE); }

static void performance_switch_view(Page *p, gboolean expanded)
{
    if (expanded)
        performance_create_expanded_view(p);
    else
        performance_create_simple_view(p);
    performance_update_selected_resource(p);
}

static void on_view_toggled(GtkToggleButton *b, gpointer data)
{
    Page *p = data;
    if (gtk_toggle_button_get_active(b))
        performance_switch_view(p, GTK_WIDGET(b) == p->btn_expanded);
}

static void on_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    (void)box;
    Page *p = data;
    if (!row)
        return;
    Resource *r = g_object_get_data(G_OBJECT(row), "perf-res");
    if (!r)
        return;
    p->selected = r;
    gtk_stack_set_visible_child_name(GTK_STACK(p->stack), r->id);
    performance_update_selected_resource(p);
}

static void on_root_map(GtkWidget *w, gpointer data)
{
    (void)w;
    Page *p = data;
    p->live = TRUE;
    if (p->last)
        performance_update(p, p->last, FALSE); /* refresh widgets without duplicating history */
}

static void on_root_unmap(GtkWidget *w, gpointer data)
{
    (void)w;
    ((Page *)data)->live = FALSE;
}

static void on_root_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    Page *p = data;
    if (p->timer) {
        g_source_remove(p->timer);
        p->timer = 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Page construction                                                        */
/* ------------------------------------------------------------------------ */

static GtkWidget *performance_create_header(Page *p)
{
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(hb, 18);
    gtk_widget_set_margin_end(hb, 14);
    gtk_widget_set_margin_top(hb, 10);
    gtk_widget_set_margin_bottom(hb, 10);

    GtkLabel *title = mk_label("Performance", "title-2", 0);
    gtk_widget_set_hexpand(GTK_WIDGET(title), TRUE);
    gtk_box_append(GTK_BOX(hb), GTK_WIDGET(title));

    GtkWidget *seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(seg, "linked");
    p->btn_simple = gtk_toggle_button_new_with_label("Simple");
    p->btn_expanded = gtk_toggle_button_new_with_label("Expanded");
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(p->btn_expanded), GTK_TOGGLE_BUTTON(p->btn_simple));
    gtk_widget_set_tooltip_text(p->btn_simple, "Compact overview");
    gtk_widget_set_tooltip_text(p->btn_expanded, "Detailed metrics and history");
    gtk_box_append(GTK_BOX(seg), p->btn_simple);
    gtk_box_append(GTK_BOX(seg), p->btn_expanded);
    gtk_box_append(GTK_BOX(hb), seg);
    return hb;
}

static GtkWidget *performance_create_resource_list(Page *p)
{
    p->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->list), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(p->list, "perf-sidebar");
    gtk_widget_set_margin_top(p->list, 6);
    gtk_widget_set_margin_bottom(p->list, 6);

    p->sidebar = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->sidebar), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(p->sidebar), p->list);
    gtk_widget_add_css_class(p->sidebar, "perf-side-scroll");
    gtk_widget_set_hexpand(p->sidebar, FALSE);
    return p->sidebar;
}

static void performance_create_resources(Page *p)
{
    const Inventory *inv = p->inv;
    Resource *r = res_new(p, RES_CPU, 0, "cpu", "CPU");
    performance_create_cpu_view(p, r);

    for (int i = 0; i < inv->ngpu; i++) {
        char id[16], title[16];
        g_snprintf(id, sizeof id, "gpu%d", i);
        g_snprintf(title, sizeof title, "GPU %d", i);
        performance_create_gpu_view(p, res_new(p, RES_GPU, i, id, title));
    }
    performance_create_memory_view(p, res_new(p, RES_MEM, 0, "mem", "Memory"));
    for (int i = 0; i < inv->ndisk; i++) {
        char id[16], title[16];
        g_snprintf(id, sizeof id, "disk%d", i);
        g_snprintf(title, sizeof title, "Disk %d", i);
        performance_create_disk_view(p, res_new(p, RES_DISK, i, id, title));
    }
    for (int i = 0; i < inv->nnet; i++) {
        char id[16];
        g_snprintf(id, sizeof id, "net%d", i);
        performance_create_network_view(p, res_new(p, RES_NET, i, id, inv->net[i].title));
    }
    if (inv->nsens > 0)
        performance_create_temperature_view(p, res_new(p, RES_TEMP, 0, "temp", "Temperature"));
}

GtkWidget *performance_page_create(App *app)
{
    (void)app;
    perf_install_css();

    Page *p = g_new0(Page, 1);
    p->interval = PERF_SAMPLE_INTERVAL_MS;
    p->sampler = sampler_new();
    p->inv = &p->sampler->inv;
    p->res = g_ptr_array_new_with_free_func(res_free);

    p->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(p->root, TRUE);
    gtk_widget_set_vexpand(p->root, TRUE);
    g_object_set_data_full(G_OBJECT(p->root), "perf-page", p, page_free);

    gtk_box_append(GTK_BOX(p->root), performance_create_header(p));
    gtk_box_append(GTK_BOX(p->root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(body, TRUE);
    gtk_box_append(GTK_BOX(body), performance_create_resource_list(p));

    p->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(p->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(p->stack), 120);
    gtk_stack_set_vhomogeneous(GTK_STACK(p->stack), FALSE);
    gtk_stack_set_hhomogeneous(GTK_STACK(p->stack), FALSE);
    gtk_widget_set_vexpand(p->stack, TRUE);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), p->stack);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(body), scroll);
    gtk_box_append(GTK_BOX(p->root), body);

    performance_create_resources(p);

    g_signal_connect(p->list, "row-selected", G_CALLBACK(on_row_selected), p);
    g_signal_connect(p->btn_simple, "toggled", G_CALLBACK(on_view_toggled), p);
    g_signal_connect(p->btn_expanded, "toggled", G_CALLBACK(on_view_toggled), p);
    g_signal_connect(p->root, "map", G_CALLBACK(on_root_map), p);
    g_signal_connect(p->root, "unmap", G_CALLBACK(on_root_unmap), p);
    g_signal_connect(p->root, "destroy", G_CALLBACK(on_root_destroy), p);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->btn_expanded), TRUE);
    performance_switch_view(p, TRUE);
    gtk_list_box_select_row(GTK_LIST_BOX(p->list), GTK_LIST_BOX_ROW(((Resource *)p->res->pdata[0])->row));

    performance_start_monitoring(p);
    return p->root;
}
