/*
 * services.c - Page 5: systemd service management
 *
 * Layers (all private to this file):
 *   1. SvcItem            - structured model object (no widgets, no raw D-Bus)
 *   2. systemd access     - GDBus calls/signals (system + user scope), sd-journal
 *   3. Page               - list models, filters, sorting, selection
 *   4. Presentation       - rows, detail panel, dialogs
 *
 * Build:  pkg-config --cflags --libs gtk4 libadwaita-1 libsystemd
 * Needs:  GTK >= 4.12, libadwaita >= 1.5
 *
 * Shared process backend (Page 2):
 *   Per-PID CPU/memory/threads come from the application's shared process
 *   backend. If a particular PID is unavailable, the page falls back to
 *   systemd's own cgroup accounting (MemoryCurrent / CPUUsageNSec /
 *   TasksCurrent). No second process scanner exists in this file.
 */

#include "services.h"

#include <adwaita.h>
#include <gio/gio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <systemd/sd-journal.h>

/* ------------------------------------------------------------------ */
/* Shared process backend contract                                     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define SD_BUS_NAME      "org.freedesktop.systemd1"
#define SD_PATH          "/org/freedesktop/systemd1"
#define SD_MANAGER_IFACE "org.freedesktop.systemd1.Manager"
#define SD_UNIT_IFACE    "org.freedesktop.systemd1.Unit"
#define SD_SERVICE_IFACE "org.freedesktop.systemd1.Service"
#define DBUS_PROPS_IFACE "org.freedesktop.DBus.Properties"
#define UNIT_PATH_PREFIX "/org/freedesktop/systemd1/unit/"

#define LOG_CHUNK         200
#define LOG_MAX_LINES     5000
#define SAMPLE_INTERVAL_S 4
#define DISCOVER_DEBOUNCE_MS 600
#define VIEW_DEBOUNCE_MS  250
#define DEP_SHOW_MAX      150

typedef enum { SCOPE_SYSTEM = 0, SCOPE_USER = 1, SCOPE_COUNT } Scope;

typedef enum {
    ST_RUNNING, ST_EXITED, ST_STOPPED, ST_FAILED,
    ST_ACTIVATING, ST_DEACTIVATING, ST_RELOADING, ST_UNKNOWN, ST_COUNT
} SvcState;

typedef enum {
    SU_ENABLED, SU_DISABLED, SU_STATIC, SU_MASKED, SU_GENERATED, SU_UNKNOWN,
    SU_COUNT
} Startup;

typedef enum {
    DEP_REQUIRES, DEP_WANTS, DEP_AFTER, DEP_BEFORE, DEP_CONFLICTS,
    DEP_REQUIRED_BY, DEP_WANTED_BY, DEP_COUNT
} DepKind;

static const char *const DEP_PROP[DEP_COUNT] = {
    "Requires", "Wants", "After", "Before", "Conflicts", "RequiredBy",
    "WantedBy"
};

static const struct { const char *label, *icon, *css; } STATE_META[ST_COUNT] = {
    [ST_RUNNING]      = { "Running",      "media-playback-start-symbolic", "running" },
    [ST_EXITED]       = { "Exited",       "object-select-symbolic",        "exited" },
    [ST_STOPPED]      = { "Stopped",      "media-playback-stop-symbolic",  "stopped" },
    [ST_FAILED]       = { "Failed",       "dialog-error-symbolic",         "failed" },
    [ST_ACTIVATING]   = { "Activating",   "emblem-synchronizing-symbolic", "busy" },
    [ST_DEACTIVATING] = { "Deactivating", "emblem-synchronizing-symbolic", "busy" },
    [ST_RELOADING]    = { "Reloading",    "view-refresh-symbolic",         "busy" },
    [ST_UNKNOWN]      = { "Unknown",      "dialog-question-symbolic",      "unknown" },
};

static const char *const STARTUP_LABEL[SU_COUNT] = {
    "Enabled", "Disabled", "Static", "Masked", "Generated", "Unknown"
};

/* ================================================================== */
/* 1. Model: SvcItem                                                   */
/* ================================================================== */

#define SVC_TYPE_ITEM (svc_item_get_type())
G_DECLARE_FINAL_TYPE(SvcItem, svc_item, SVC, ITEM, GObject)

struct _SvcItem {
    GObject parent_instance;

    Scope  scope;
    gchar *key;        /* "<scope>:<unit>" */
    gchar *name;       /* full unit name, e.g. NetworkManager.service */
    gchar *label;      /* name without ".service" */
    gchar *name_fold;
    gchar *search;     /* casefolded haystack */

    gchar *description, *load_state, *active_state, *sub_state;
    gchar *unit_file_state, *unit_path, *fragment_path;
    gchar *exec_path, *cmdline, *user, *group, *workdir, *result;
    gchar **drop_ins;
    gchar **deps[DEP_COUNT];
    guint   dep_rev;

    SvcState state;
    Startup  startup;
    gboolean masked;

    guint32 main_pid, control_pid;
    guint64 active_enter, active_exit, inactive_enter, exec_start; /* usec */
    gboolean can_reload;

    gboolean has_cpu, has_mem, has_tasks;
    gdouble  cpu;
    guint64  mem, tasks;
    guint64  prev_ns;
    gint64   prev_time;

    gboolean meta_loaded, meta_pending, seen;
};

G_DEFINE_FINAL_TYPE(SvcItem, svc_item, G_TYPE_OBJECT)

static guint item_changed_sig;

static void svc_item_finalize(GObject *o)
{
    SvcItem *i = SVC_ITEM(o);
    g_free(i->key); g_free(i->name); g_free(i->label); g_free(i->name_fold);
    g_free(i->search); g_free(i->description); g_free(i->load_state);
    g_free(i->active_state); g_free(i->sub_state); g_free(i->unit_file_state);
    g_free(i->unit_path); g_free(i->fragment_path); g_free(i->exec_path);
    g_free(i->cmdline); g_free(i->user); g_free(i->group); g_free(i->workdir);
    g_free(i->result);
    g_strfreev(i->drop_ins);
    for (int k = 0; k < DEP_COUNT; k++)
        g_strfreev(i->deps[k]);
    G_OBJECT_CLASS(svc_item_parent_class)->finalize(o);
}

static void svc_item_class_init(SvcItemClass *k)
{
    G_OBJECT_CLASS(k)->finalize = svc_item_finalize;
    item_changed_sig = g_signal_new("changed", G_TYPE_FROM_CLASS(k),
                                    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                    G_TYPE_NONE, 0);
}

static void svc_item_init(SvcItem *i) { (void)i; }

static gboolean set_str(gchar **dst, const gchar *v)
{
    if (!v)
        v = "";
    if (g_strcmp0(*dst, v) == 0)
        return FALSE;
    g_free(*dst);
    *dst = g_strdup(v);
    return TRUE;
}

static Startup startup_classify(const gchar *s)
{
    if (!s || !*s) return SU_UNKNOWN;
    if (g_str_has_prefix(s, "enabled")) return SU_ENABLED;
    if (g_str_has_prefix(s, "masked"))  return SU_MASKED;
    if (g_str_equal(s, "disabled"))     return SU_DISABLED;
    if (g_str_equal(s, "static"))       return SU_STATIC;
    if (g_str_equal(s, "generated"))    return SU_GENERATED;
    return SU_UNKNOWN;
}

static gboolean svc_is_active(SvcState s)
{
    return s == ST_RUNNING || s == ST_EXITED || s == ST_RELOADING ||
           s == ST_ACTIVATING || s == ST_DEACTIVATING;
}

/* "live" == may have processes worth sampling */
static gboolean svc_is_live(SvcState s)
{
    return s == ST_RUNNING || s == ST_RELOADING || s == ST_ACTIVATING ||
           s == ST_DEACTIVATING;
}

static void item_derive(SvcItem *i)
{
    const gchar *a = i->active_state, *s = i->sub_state;

    if (!g_strcmp0(a, "active"))
        i->state = !g_strcmp0(s, "exited") ? ST_EXITED : ST_RUNNING;
    else if (!g_strcmp0(a, "reloading"))    i->state = ST_RELOADING;
    else if (!g_strcmp0(a, "activating"))   i->state = ST_ACTIVATING;
    else if (!g_strcmp0(a, "deactivating")) i->state = ST_DEACTIVATING;
    else if (!g_strcmp0(a, "failed"))       i->state = ST_FAILED;
    else if (!g_strcmp0(a, "inactive"))     i->state = ST_STOPPED;
    else                                    i->state = ST_UNKNOWN;

    i->startup = startup_classify(i->unit_file_state);
    i->masked = !g_strcmp0(i->load_state, "masked") || i->startup == SU_MASKED;

    gchar *pid = i->main_pid ? g_strdup_printf("%u", i->main_pid) : g_strdup("");
    gchar *raw = g_strjoin("\n", i->name, i->description ? i->description : "",
                           i->exec_path ? i->exec_path : "",
                           i->cmdline ? i->cmdline : "", pid, NULL);
    g_free(i->search);
    i->search = g_utf8_casefold(raw, -1);
    g_free(raw);
    g_free(pid);
}

static SvcItem *item_new(Scope scope, const gchar *name)
{
    SvcItem *i = g_object_new(SVC_TYPE_ITEM, NULL);
    i->scope = scope;
    i->name = g_strdup(name);
    i->key = g_strdup_printf("%d:%s", (int)scope, name);
    i->label = g_str_has_suffix(name, ".service")
                   ? g_strndup(name, strlen(name) - 8)
                   : g_strdup(name);
    i->name_fold = g_utf8_casefold(name, -1);
    i->description = g_strdup("");
    i->load_state = g_strdup("");
    i->active_state = g_strdup("inactive");
    i->sub_state = g_strdup("dead");
    i->unit_file_state = g_strdup("");
    item_derive(i);
    return i;
}

static void item_emit(SvcItem *i)
{
    g_signal_emit(i, item_changed_sig, 0);
}

static void item_clear_metrics(SvcItem *i)
{
    i->has_cpu = i->has_mem = i->has_tasks = FALSE;
    i->cpu = 0; i->mem = 0; i->tasks = 0;
    i->prev_ns = 0;
}

/* --- parsing of systemd property dictionaries (a{sv}) --- */

static void item_apply_unit_props(SvcItem *i, GVariant *d)
{
    const gchar *s; guint64 t; gboolean b; gchar **v;

    if (g_variant_lookup(d, "Description", "&s", &s))     set_str(&i->description, s);
    if (g_variant_lookup(d, "LoadState", "&s", &s))       set_str(&i->load_state, s);
    if (g_variant_lookup(d, "ActiveState", "&s", &s))     set_str(&i->active_state, s);
    if (g_variant_lookup(d, "SubState", "&s", &s))        set_str(&i->sub_state, s);
    if (g_variant_lookup(d, "UnitFileState", "&s", &s) && *s)
        set_str(&i->unit_file_state, s);
    if (g_variant_lookup(d, "FragmentPath", "&s", &s))    set_str(&i->fragment_path, s);
    if (g_variant_lookup(d, "ActiveEnterTimestamp", "t", &t))   i->active_enter = t;
    if (g_variant_lookup(d, "ActiveExitTimestamp", "t", &t))    i->active_exit = t;
    if (g_variant_lookup(d, "InactiveEnterTimestamp", "t", &t)) i->inactive_enter = t;
    if (g_variant_lookup(d, "CanReload", "b", &b))        i->can_reload = b;
    if (g_variant_lookup(d, "DropInPaths", "^as", &v)) {
        g_strfreev(i->drop_ins);
        i->drop_ins = v;
    }
    for (int k = 0; k < DEP_COUNT; k++) {
        if (g_variant_lookup(d, DEP_PROP[k], "^as", &v)) {
            g_strfreev(i->deps[k]);
            i->deps[k] = v;
            i->dep_rev++;
        }
    }
    item_derive(i);
}

static gboolean backend_available(void)
{
    return TRUE;
}

static void item_apply_service_props(SvcItem *i, GVariant *d)
{
    guint32 u; guint64 t; const gchar *s;

    if (g_variant_lookup(d, "MainPID", "u", &u))    i->main_pid = u;
    if (g_variant_lookup(d, "ControlPID", "u", &u)) i->control_pid = u;
    if (g_variant_lookup(d, "Result", "&s", &s))    set_str(&i->result, s);
    if (g_variant_lookup(d, "User", "&s", &s))      set_str(&i->user, s);
    if (g_variant_lookup(d, "Group", "&s", &s))     set_str(&i->group, s);
    if (g_variant_lookup(d, "WorkingDirectory", "&s", &s))
        set_str(&i->workdir, s);
    if (g_variant_lookup(d, "ExecMainStartTimestamp", "t", &t))
        i->exec_start = t;

    GVariant *ex = g_variant_lookup_value(d, "ExecStart",
                                          G_VARIANT_TYPE("a(sasbttttuii)"));
    if (ex) {
        if (g_variant_n_children(ex) > 0) {
            GVariant *e = g_variant_get_child_value(ex, 0);
            const gchar *path = NULL; gchar **argv = NULL;
            g_variant_get_child(e, 0, "&s", &path);
            g_variant_get_child(e, 1, "^as", &argv);
            set_str(&i->exec_path, path);
            gchar *cl = argv ? g_strjoinv(" ", argv) : g_strdup("");
            set_str(&i->cmdline, cl);
            g_free(cl);
            g_strfreev(argv);
            g_variant_unref(e);
        }
        g_variant_unref(ex);
    }

    /* cgroup accounting: fallback when the shared backend is unavailable */
    if (!backend_available()) {
        if (g_variant_lookup(d, "MemoryCurrent", "t", &t) && t != G_MAXUINT64) {
            i->mem = t; i->has_mem = TRUE;
        }
        if (g_variant_lookup(d, "TasksCurrent", "t", &t) && t != G_MAXUINT64) {
            i->tasks = t; i->has_tasks = TRUE;
        }
        if (g_variant_lookup(d, "CPUUsageNSec", "t", &t) && t != G_MAXUINT64) {
            gint64 now = g_get_monotonic_time();
            if (i->prev_ns && now > i->prev_time && t >= i->prev_ns) {
                i->cpu = ((gdouble)(t - i->prev_ns) / 1000.0) /
                         (gdouble)(now - i->prev_time) * 100.0;
                i->has_cpu = TRUE;
            }
            i->prev_ns = t;
            i->prev_time = now;
        }
    }
    i->meta_loaded = TRUE;
    item_derive(i);
}

/* ================================================================== */
/* 2/3. Page state                                                     */
/* ================================================================== */

typedef struct Page Page;

typedef struct {
    Page            *page;
    Scope            scope;
    GDBusConnection *conn;
    guint            subs[6];
    guint            n_subs;
    gboolean         discovering, again;
    guint            discover_id;
} ScopeCtx;

typedef struct { GtkWidget *box, *icon, *label; } Chip;

typedef struct { int id; const char *label; } Opt;

typedef struct {
    GtkDropDown *dd;
    const Opt   *opts;
    int          n;
    GArray      *ids;
    guint        mask;
    int          current; /* selected option id */
} FilterDD;

enum { SF_ALL, SF_RUNNING, SF_STOPPED, SF_FAILED, SF_ACTIVATING,
       SF_DEACTIVATING, SF_OTHER };

static const Opt STATE_OPTS[] = {
    { 0, "All states" }, { SF_RUNNING, "Running" }, { SF_STOPPED, "Stopped" },
    { SF_FAILED, "Failed" }, { SF_ACTIVATING, "Activating" },
    { SF_DEACTIVATING, "Deactivating" }, { SF_OTHER, "Other active states" },
};
static const Opt STARTUP_OPTS[] = {
    { 0, "All" }, { SU_ENABLED + 1, "Enabled" }, { SU_DISABLED + 1, "Disabled" },
    { SU_STATIC + 1, "Static" }, { SU_MASKED + 1, "Masked" },
    { SU_GENERATED + 1, "Generated" }, { SU_UNKNOWN + 1, "Unknown" },
};
static const Opt SCOPE_OPTS[] = {
    { 0, "All scopes" }, { SCOPE_SYSTEM + 1, "System" }, { SCOPE_USER + 1, "User" },
};

typedef enum { SORT_NAME, SORT_STATE, SORT_STARTUP, SORT_CPU, SORT_MEM,
               SORT_PID } SortCol;
typedef enum { GROUP_NONE, GROUP_ACTIVE, GROUP_STARTUP, GROUP_SCOPE } GroupMode;

typedef enum { ACT_START, ACT_STOP, ACT_RESTART, ACT_RELOAD, ACT_ENABLE,
               ACT_DISABLE, ACT_MASK, ACT_UNMASK, ACT_COUNT } Action;

static const struct { const char *label, *done, *method; } ACT[ACT_COUNT] = {
    [ACT_START]   = { "Start",   "Started",  "StartUnit" },
    [ACT_STOP]    = { "Stop",    "Stopped",  "StopUnit" },
    [ACT_RESTART] = { "Restart", "Restarted", "RestartUnit" },
    [ACT_RELOAD]  = { "Reload",  "Reloaded", "ReloadUnit" },
    [ACT_ENABLE]  = { "Enable",  "Enabled",  "EnableUnitFiles" },
    [ACT_DISABLE] = { "Disable", "Disabled", "DisableUnitFiles" },
    [ACT_MASK]    = { "Mask",    "Masked",   "MaskUnitFiles" },
    [ACT_UNMASK]  = { "Unmask",  "Unmasked", "UnmaskUnitFiles" },
};

enum {
    R_ACTIVE, R_SUB, R_LOAD, R_FILE, R_ACT_TS, R_DEACT_TS, R_FAILURE,
    R_MAINPID, R_CTRLPID, R_EXEC, R_CMD, R_USER, R_GROUP, R_WORKDIR,
    R_CPU, R_MEM, R_TASKS, R_START, R_RUNTIME,
    R_FRAG, R_DROPIN, R_COUNT
};

static const struct { int id; const char *label; int group; gboolean mono; }
ROW_DEFS[] = {
    { R_ACTIVE, "Active state", 0, 0 }, { R_SUB, "Sub-state", 0, 0 },
    { R_LOAD, "Load state", 0, 0 }, { R_FILE, "Unit file state", 0, 0 },
    { R_ACT_TS, "Activated", 0, 0 }, { R_DEACT_TS, "Deactivated", 0, 0 },
    { R_FAILURE, "Failure", 0, 0 },
    { R_MAINPID, "Main PID", 1, 1 }, { R_CTRLPID, "Control PID", 1, 1 },
    { R_EXEC, "Executable", 1, 1 }, { R_CMD, "Command line", 1, 1 },
    { R_USER, "User", 1, 0 }, { R_GROUP, "Group", 1, 0 },
    { R_WORKDIR, "Working directory", 1, 1 },
    { R_CPU, "CPU", 2, 0 }, { R_MEM, "Memory", 2, 0 },
    { R_TASKS, "Tasks / threads", 2, 0 }, { R_START, "Started", 2, 0 },
    { R_RUNTIME, "Runtime", 2, 0 },
    { R_FRAG, "Fragment path", 3, 1 }, { R_DROPIN, "Drop-ins", 3, 1 },
};

static const char *const DEP_TITLE[DEP_COUNT] = {
    "Requires", "Wants", "After", "Before", "Conflicts", "Required by",
    "Wanted by"
};

struct Page {
    App *app;
    GtkWidget *root;
    AdwToastOverlay *toasts;
    GCancellable *cancel;

    ScopeCtx scopes[SCOPE_COUNT];
    GHashTable *items;     /* key -> SvcItem (owned) */
    GListStore *store;
    GtkFilterListModel *fmodel;
    GtkSortListModel *smodel;
    GtkSingleSelection *sel;
    GtkCustomFilter *filter;
    GtkCustomSorter *col_sorter, *group_sorter;
    GtkWidget *list_view;
    GtkWidget *split;

    gchar **tokens;
    FilterDD f_state, f_startup, f_scope;
    SortCol sort_col;
    GroupMode group_mode;
    gboolean building, syncing;

    guint busy;
    GtkWidget *spinner;
    GtkWidget *stat[6];
    guint view_id, sample_id;
    gboolean view_filter_dirty, view_sort_dirty;

    /* selection & details */
    SvcItem *selected;
    gulong sel_handler;
    GtkWidget *detail_stack;
    GtkWidget *d_title, *d_desc, *d_startup, *d_scope;
    Chip d_chip;
    GtkWidget *btn[ACT_COUNT], *disruptive_box, *action_spinner;
    Action primary_act;
    gboolean action_busy;
    AdwActionRow *rows[R_COUNT];
    AdwExpanderRow *dep_exp[DEP_COUNT];
    GPtrArray *dep_rows[DEP_COUNT];
    SvcItem *dep_item;
    guint dep_rev;

    /* logs */
    AdwExpanderRow *logs_exp;
    GtkWidget *log_view, *log_older, *log_spinner, *log_status;
    GtkTextBuffer *log_buf;
    gchar *log_cursor;
    gboolean log_more, log_loaded, log_busy;
    guint log_gen, log_lines;
};

/* forward declarations */
static void services_select(Page *p, SvcItem *item);
static void services_refresh(Page *p);
static void details_refresh_ui(Page *p);
static void schedule_views(Page *p, gboolean filter_dirty, gboolean sort_dirty);
static void logs_reset(Page *p);
static void logs_request(Page *p, gboolean older);
static void scope_schedule_discover(ScopeCtx *sc);

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static gboolean err_cancelled(GError *e)
{
    return e && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

/* Finish a GDBusConnection call without touching page state (safe after
 * the page is gone and the operation was cancelled). */
static GVariant *call_finish(GAsyncResult *res, GError **err)
{
    GObject *src = g_async_result_get_source_object(res);
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, err);
    g_object_unref(src);
    return r;
}

static void toast(Page *p, const gchar *msg)
{
    adw_toast_overlay_add_toast(p->toasts, adw_toast_new(msg));
}

static gchar *fmt_time(guint64 usec)
{
    if (!usec)
        return NULL;
    GDateTime *dt = g_date_time_new_from_unix_local((gint64)(usec / 1000000));
    if (!dt)
        return NULL;
    gchar *s = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S");
    g_date_time_unref(dt);
    return s;
}

static gchar *fmt_duration(guint64 secs)
{
    guint64 d = secs / 86400, h = (secs % 86400) / 3600,
            m = (secs % 3600) / 60, s = secs % 60;
    if (d) return g_strdup_printf("%" G_GUINT64_FORMAT "d %" G_GUINT64_FORMAT "h %" G_GUINT64_FORMAT "m", d, h, m);
    if (h) return g_strdup_printf("%" G_GUINT64_FORMAT "h %" G_GUINT64_FORMAT "m %" G_GUINT64_FORMAT "s", h, m, s);
    if (m) return g_strdup_printf("%" G_GUINT64_FORMAT "m %" G_GUINT64_FORMAT "s", m, s);
    return g_strdup_printf("%" G_GUINT64_FORMAT "s", s);
}

static gchar *unit_name_from_path(const gchar *path)
{
    if (!g_str_has_prefix(path, UNIT_PATH_PREFIX))
        return NULL;
    const gchar *c = path + strlen(UNIT_PATH_PREFIX);
    GString *s = g_string_new(NULL);
    for (; *c; c++) {
        if (*c == '_' && g_ascii_isxdigit(c[1]) && g_ascii_isxdigit(c[2])) {
            g_string_append_c(s, (gchar)(g_ascii_xdigit_value(c[1]) * 16 +
                                         g_ascii_xdigit_value(c[2])));
            c += 2;
        } else {
            g_string_append_c(s, *c);
        }
    }
    return g_string_free(s, FALSE);
}

static SvcItem *page_lookup(Page *p, Scope sc, const gchar *name)
{
    gchar *key = g_strdup_printf("%d:%s", (int)sc, name);
    SvcItem *i = g_hash_table_lookup(p->items, key);
    g_free(key);
    return i;
}

static void page_busy(Page *p, int delta)
{
    if (delta < 0 && p->busy == 0)
        return;
    p->busy += delta;
    gtk_widget_set_visible(p->spinner, p->busy > 0);
    gtk_spinner_set_spinning(GTK_SPINNER(p->spinner), p->busy > 0);
}

/* ------------------------------------------------------------------ */
/* Sorting / filtering / grouping                                      */
/* ------------------------------------------------------------------ */

static int state_rank(SvcState s)
{
    switch (s) {
    case ST_FAILED: return 0;
    case ST_ACTIVATING: return 1;
    case ST_RELOADING: return 2;
    case ST_DEACTIVATING: return 3;
    case ST_RUNNING: return 4;
    case ST_EXITED: return 5;
    case ST_STOPPED: return 6;
    default: return 7;
    }
}

static int state_filter_id(SvcState s)
{
    switch (s) {
    case ST_RUNNING: return SF_RUNNING;
    case ST_STOPPED: return SF_STOPPED;
    case ST_FAILED: return SF_FAILED;
    case ST_ACTIVATING: return SF_ACTIVATING;
    case ST_DEACTIVATING: return SF_DEACTIVATING;
    case ST_EXITED: case ST_RELOADING: return SF_OTHER;
    default: return 0;
    }
}

static int cmp_u64_desc_missing_last(gboolean ha, guint64 a, gboolean hb, guint64 b)
{
    if (ha != hb) return ha ? -1 : 1;
    if (a > b) return -1;
    if (a < b) return 1;
    return 0;
}

static int cmp_name(const SvcItem *a, const SvcItem *b)
{
    int c = strcmp(a->name_fold, b->name_fold);
    if (c) return c;
    c = (int)a->scope - (int)b->scope;
    return c ? c : strcmp(a->key, b->key);
}

static int col_compare(gconstpointer pa, gconstpointer pb, gpointer data)
{
    Page *p = data;
    const SvcItem *a = pa, *b = pb;
    int c = 0;

    switch (p->sort_col) {
    case SORT_STATE:   c = state_rank(a->state) - state_rank(b->state); break;
    case SORT_STARTUP: c = (int)a->startup - (int)b->startup; break;
    case SORT_CPU:
        c = cmp_u64_desc_missing_last(a->has_cpu, (guint64)(a->cpu * 10.0 + 0.5),
                                      b->has_cpu, (guint64)(b->cpu * 10.0 + 0.5));
        break;
    case SORT_MEM:
        c = cmp_u64_desc_missing_last(a->has_mem, a->mem, b->has_mem, b->mem);
        break;
    case SORT_PID:
        if ((a->main_pid > 0) != (b->main_pid > 0))
            c = a->main_pid > 0 ? -1 : 1;
        else
            c = a->main_pid < b->main_pid ? -1 : a->main_pid > b->main_pid;
        break;
    default: break;
    }
    return c ? c : cmp_name(a, b); /* always a stable name tie-break */
}

static int group_key(Page *p, const SvcItem *i)
{
    switch (p->group_mode) {
    case GROUP_ACTIVE:
        if (i->state == ST_FAILED) return 0;
        if (i->state == ST_RUNNING || i->state == ST_EXITED ||
            i->state == ST_RELOADING) return 1;
        if (i->state == ST_ACTIVATING || i->state == ST_DEACTIVATING) return 2;
        return 3;
    case GROUP_STARTUP: return (int)i->startup;
    case GROUP_SCOPE:   return (int)i->scope;
    default:            return 0;
    }
}

static int group_compare(gconstpointer a, gconstpointer b, gpointer data)
{
    Page *p = data;
    return group_key(p, a) - group_key(p, b);
}

static const gchar *group_title(Page *p, const SvcItem *i)
{
    static const char *const act[] = { "Failed", "Active", "Activating or deactivating", "Inactive" };
    switch (p->group_mode) {
    case GROUP_ACTIVE:  return act[group_key(p, i)];
    case GROUP_STARTUP: return STARTUP_LABEL[i->startup];
    case GROUP_SCOPE:   return i->scope == SCOPE_USER ? "User services" : "System services";
    default:            return "";
    }
}

static gboolean match_func(gpointer obj, gpointer data)
{
    Page *p = data;
    const SvcItem *i = obj;

    if (p->f_scope.current && (int)i->scope + 1 != p->f_scope.current) return FALSE;
    if (p->f_state.current && state_filter_id(i->state) != p->f_state.current) return FALSE;
    if (p->f_startup.current && (int)i->startup + 1 != p->f_startup.current) return FALSE;
    if (p->tokens)
        for (gchar **t = p->tokens; *t; t++)
            if (**t && !strstr(i->search, *t))
                return FALSE;
    return TRUE;
}

static gboolean sort_is_dynamic_metric(Page *p)
{
    return p->sort_col == SORT_CPU || p->sort_col == SORT_MEM ||
           p->sort_col == SORT_PID;
}

/* Debounced: recompute summary, and re-evaluate filter/sorter only when a
 * change could actually affect them. */
static void update_summary(Page *p);
static void rebuild_filter_options(Page *p);

static gboolean views_cb(gpointer d)
{
    Page *p = d;
    p->view_id = 0;
    update_summary(p);
    rebuild_filter_options(p);
    if (p->view_filter_dirty)
        gtk_filter_changed(GTK_FILTER(p->filter), GTK_FILTER_CHANGE_DIFFERENT);
    if (p->view_sort_dirty)
        gtk_sorter_changed(GTK_SORTER(p->col_sorter), GTK_SORTER_CHANGE_DIFFERENT);
    p->view_filter_dirty = p->view_sort_dirty = FALSE;
    return G_SOURCE_REMOVE;
}

static void schedule_views(Page *p, gboolean filter_dirty, gboolean sort_dirty)
{
    p->view_filter_dirty |= filter_dirty;
    p->view_sort_dirty |= sort_dirty;
    if (!p->view_id)
        p->view_id = g_timeout_add(VIEW_DEBOUNCE_MS, views_cb, p);
}

/* ------------------------------------------------------------------ */
/* Summary strip and dynamic filter options                            */
/* ------------------------------------------------------------------ */

static void update_summary(Page *p)
{
    guint total = 0, cnt[ST_COUNT] = { 0 }, en = 0, dis = 0;
    GHashTableIter it; gpointer k, v;

    g_hash_table_iter_init(&it, p->items);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const SvcItem *i = v;
        total++;
        cnt[i->state]++;
        if (i->startup == SU_ENABLED) en++;
        if (i->startup == SU_DISABLED) dis++;
    }
    guint vals[6] = { total, cnt[ST_RUNNING], cnt[ST_STOPPED], cnt[ST_FAILED], en, dis };
    for (int n = 0; n < 6; n++) {
        gchar *s = g_strdup_printf("%u", vals[n]);
        gtk_label_set_text(GTK_LABEL(p->stat[n]), s);
        g_free(s);
    }
}

static void fdd_rebuild(Page *p, FilterDD *f, guint mask)
{
    if (mask == f->mask)
        return;
    f->mask = mask;

    GtkStringList *sl = gtk_string_list_new(NULL);
    guint sel = 0;
    gboolean found = FALSE;
    g_array_set_size(f->ids, 0);
    for (int k = 0; k < f->n; k++) {
        int id = f->opts[k].id;
        if (id == 0 || (mask & (1u << id))) {
            if (id == f->current) { sel = f->ids->len; found = TRUE; }
            gtk_string_list_append(sl, f->opts[k].label);
            g_array_append_val(f->ids, id);
        }
    }
    gboolean reset = !found && f->current != 0;
    if (reset)
        f->current = 0;
    p->building = TRUE;
    gtk_drop_down_set_model(f->dd, G_LIST_MODEL(sl));
    gtk_drop_down_set_selected(f->dd, found ? sel : 0);
    p->building = FALSE;
    g_object_unref(sl);
    if (reset)
        gtk_filter_changed(GTK_FILTER(p->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

/* Only offer filter values that exist in the data actually reported by
 * systemd. */
static void rebuild_filter_options(Page *p)
{
    guint ms = 0, mu = 0, mc = 0;
    GHashTableIter it; gpointer k, v;

    g_hash_table_iter_init(&it, p->items);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const SvcItem *i = v;
        int s = state_filter_id(i->state);
        if (s) ms |= 1u << s;
        mu |= 1u << ((int)i->startup + 1);
        mc |= 1u << ((int)i->scope + 1);
    }
    fdd_rebuild(p, &p->f_state, ms);
    fdd_rebuild(p, &p->f_startup, mu);
    fdd_rebuild(p, &p->f_scope, mc);
}

/* ------------------------------------------------------------------ */
/* systemd discovery (ListUnits + ListUnitFiles)                       */
/* ------------------------------------------------------------------ */

static void scope_apply_listing(Page *p, ScopeCtx *sc, GVariant *units, GVariant *files)
{
    GHashTable *states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GHashTable *paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GPtrArray *added = g_ptr_array_new();
    GHashTableIter it; gpointer k, v;

    if (files) {
        GVariantIter fi;
        const gchar *path, *state;
        g_variant_iter_init(&fi, files);
        while (g_variant_iter_next(&fi, "(&s&s)", &path, &state)) {
            gchar *base = g_path_get_basename(path);
            if (g_str_has_suffix(base, ".service")) {
                g_hash_table_insert(states, g_strdup(base), g_strdup(state));
                g_hash_table_insert(paths, base, g_strdup(path));
            } else {
                g_free(base);
            }
        }
    }

    g_hash_table_iter_init(&it, p->items);
    while (g_hash_table_iter_next(&it, &k, &v))
        if (((SvcItem *)v)->scope == sc->scope)
            ((SvcItem *)v)->seen = FALSE;

    GVariantIter ui;
    const gchar *id, *desc, *load, *act, *sub, *follow, *upath, *jtype, *jpath;
    guint32 jid;
    g_variant_iter_init(&ui, units);
    while (g_variant_iter_next(&ui, "(&s&s&s&s&s&s&ou&s&o)", &id, &desc, &load,
                               &act, &sub, &follow, &upath, &jid, &jtype, &jpath)) {
        if (!g_str_has_suffix(id, ".service") || (follow && *follow))
            continue;
        SvcItem *i = page_lookup(p, sc->scope, id);
        if (!i) {
            i = item_new(sc->scope, id);
            g_hash_table_insert(p->items, g_strdup(i->key), i);
            g_ptr_array_add(added, i);
        }
        i->seen = TRUE;
        set_str(&i->description, desc);
        set_str(&i->load_state, load);
        set_str(&i->active_state, act);
        set_str(&i->sub_state, sub);
        set_str(&i->unit_path, upath);
    }

    /* unit files: startup state, and units that are not currently loaded */
    g_hash_table_iter_init(&it, states);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const gchar *name = k;
        if (g_str_has_suffix(name, "@.service"))
            continue; /* templates cannot be controlled directly */
        SvcItem *i = page_lookup(p, sc->scope, name);
        if (!i) {
            i = item_new(sc->scope, name);
            set_str(&i->load_state, "not-loaded");
            g_hash_table_insert(p->items, g_strdup(i->key), i);
            g_ptr_array_add(added, i);
        }
        i->seen = TRUE;
    }

    GPtrArray *gone = g_ptr_array_new();
    g_hash_table_iter_init(&it, p->items);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        SvcItem *i = v;
        if (i->scope != sc->scope)
            continue;
        if (!i->seen) {
            g_ptr_array_add(gone, i);
            continue;
        }
        const gchar *st = g_hash_table_lookup(states, i->name);
        if (!st) {
            /* instance of a template: inherit the template's state */
            const gchar *at = strchr(i->name, '@');
            if (at) {
                gchar *tmpl = g_strdup_printf("%.*s@.service", (int)(at - i->name), i->name);
                st = g_hash_table_lookup(states, tmpl);
                g_free(tmpl);
            }
        }
        if (st)
            set_str(&i->unit_file_state, st);
        const gchar *fp = g_hash_table_lookup(paths, i->name);
        if (fp && (!i->fragment_path || !*i->fragment_path))
            set_str(&i->fragment_path, fp);
        if (!svc_is_live(i->state) && i->main_pid == 0)
            item_clear_metrics(i);
        item_derive(i);
        item_emit(i);
    }

    /* splice new items in one batch (single list-model notification) */
    if (added->len)
        g_list_store_splice(p->store, g_list_model_get_n_items(G_LIST_MODEL(p->store)),
                            0, added->pdata, added->len);

    for (guint n = 0; n < gone->len; n++) {
        SvcItem *i = g_ptr_array_index(gone, n);
        guint pos;
        gchar *key = g_strdup(i->key);
        g_object_ref(i);
        if (g_list_store_find(p->store, i, &pos))
            g_list_store_remove(p->store, pos);
        g_hash_table_remove(p->items, key);
        if (i == p->selected) {
            gchar *msg = g_strdup_printf("%s no longer exists", i->name);
            services_select(p, NULL);
            adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(p->split), FALSE);
            toast(p, msg);
            g_free(msg);
        }
        g_object_unref(i);
        g_free(key);
    }

    g_ptr_array_free(added, TRUE);
    g_ptr_array_free(gone, TRUE);
    g_hash_table_unref(states);
    g_hash_table_unref(paths);
    schedule_views(p, TRUE, TRUE);
}

/* ---- per-unit service metadata (MainPID, exec, accounting) ---- */

typedef struct { Page *page; SvcItem *item; } MetaOp;

static void on_meta(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    MetaOp *op = data;
    GError *err = NULL;
    GVariant *r = call_finish(res, &err);

    if (err_cancelled(err)) {
        g_clear_error(&err);
        g_object_unref(op->item);
        g_free(op);
        return;
    }
    op->item->meta_pending = FALSE;
    if (r) {
        GVariant *d = g_variant_get_child_value(r, 0);
        item_apply_service_props(op->item, d);
        item_emit(op->item);
        g_variant_unref(d);
        g_variant_unref(r);
        schedule_views(op->page, FALSE, sort_is_dynamic_metric(op->page));
    }
    g_clear_error(&err);
    g_object_unref(op->item);
    g_free(op);
}

static void fetch_meta(Page *p, SvcItem *i)
{
    ScopeCtx *sc = &p->scopes[i->scope];
    if (!sc->conn || !i->unit_path || !*i->unit_path || i->meta_pending)
        return;
    MetaOp *op = g_new0(MetaOp, 1);
    op->page = p;
    op->item = g_object_ref(i);
    i->meta_pending = TRUE;
    g_dbus_connection_call(sc->conn, SD_BUS_NAME, i->unit_path, DBUS_PROPS_IFACE,
                           "GetAll", g_variant_new("(s)", SD_SERVICE_IFACE),
                           G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, -1,
                           p->cancel, on_meta, op);
}

static void fetch_missing_meta(Page *p, Scope scope)
{
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, p->items);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        SvcItem *i = v;
        if (i->scope == scope && svc_is_live(i->state) && !i->meta_loaded)
            fetch_meta(p, i);
    }
}

static void scope_discover(Page *p, ScopeCtx *sc);

typedef struct { Page *page; ScopeCtx *sc; GVariant *units; } DiscoverOp;

static void discover_finish(DiscoverOp *op)
{
    ScopeCtx *sc = op->sc;
    Page *p = op->page;
    if (op->units)
        g_variant_unref(op->units);
    g_free(op);
    sc->discovering = FALSE;
    page_busy(p, -1);
    if (sc->again) {
        sc->again = FALSE;
        scope_discover(p, sc);
    }
}

static void on_list_files(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    DiscoverOp *op = data;
    GError *err = NULL;
    GVariant *r = call_finish(res, &err);

    if (err_cancelled(err)) {
        g_clear_error(&err);
        if (op->units) g_variant_unref(op->units);
        g_free(op);
        return;
    }
    GVariant *files = r ? g_variant_get_child_value(r, 0) : NULL;
    scope_apply_listing(op->page, op->sc, op->units, files);
    fetch_missing_meta(op->page, op->sc->scope);
    if (files) g_variant_unref(files);
    if (r) g_variant_unref(r);
    g_clear_error(&err);
    discover_finish(op);
}

static void on_list_units(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    DiscoverOp *op = data;
    GError *err = NULL;
    GVariant *r = call_finish(res, &err);

    if (err_cancelled(err)) {
        g_clear_error(&err);
        g_free(op);
        return;
    }
    if (!r) {
        g_clear_error(&err);
        discover_finish(op);
        return;
    }
    op->units = g_variant_get_child_value(r, 0);
    g_variant_unref(r);
    g_dbus_connection_call(op->sc->conn, SD_BUS_NAME, SD_PATH, SD_MANAGER_IFACE,
                           "ListUnitFiles", NULL, G_VARIANT_TYPE("(a(ss))"),
                           G_DBUS_CALL_FLAGS_NONE, -1, op->page->cancel,
                           on_list_files, op);
}

static void scope_discover(Page *p, ScopeCtx *sc)
{
    if (!sc->conn)
        return;
    if (sc->discovering) {
        sc->again = TRUE;
        return;
    }
    sc->discovering = TRUE;
    page_busy(p, +1);
    DiscoverOp *op = g_new0(DiscoverOp, 1);
    op->page = p;
    op->sc = sc;
    g_dbus_connection_call(sc->conn, SD_BUS_NAME, SD_PATH, SD_MANAGER_IFACE,
                           "ListUnits", NULL, G_VARIANT_TYPE("(a(ssssssouso))"),
                           G_DBUS_CALL_FLAGS_NONE, -1, p->cancel, on_list_units, op);
}

static void services_refresh(Page *p)
{
    for (int s = 0; s < SCOPE_COUNT; s++)
        scope_discover(p, &p->scopes[s]);
}

static gboolean discover_timeout(gpointer d)
{
    ScopeCtx *sc = d;
    sc->discover_id = 0;
    scope_discover(sc->page, sc);
    return G_SOURCE_REMOVE;
}

static void scope_schedule_discover(ScopeCtx *sc)
{
    if (!sc->discover_id)
        sc->discover_id = g_timeout_add(DISCOVER_DEBOUNCE_MS, discover_timeout, sc);
}

/* ------------------------------------------------------------------ */
/* Change notification (D-Bus signals)                                 */
/* ------------------------------------------------------------------ */

static void on_manager_signal(GDBusConnection *c, const gchar *sender,
                              const gchar *path, const gchar *iface,
                              const gchar *signal, GVariant *params, gpointer data)
{
    (void)c; (void)sender; (void)path; (void)iface;
    ScopeCtx *sc = data;

    if (!g_strcmp0(signal, "Reloading")) {
        gboolean active = FALSE;
        g_variant_get(params, "(b)", &active);
        if (active)
            return; /* wait for the reload to finish */
    }
    scope_schedule_discover(sc);
}

static void on_job_removed(GDBusConnection *c, const gchar *sender,
                           const gchar *path, const gchar *iface,
                           const gchar *signal, GVariant *params, gpointer data)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)signal;
    ScopeCtx *sc = data;
    Page *p = sc->page;
    guint32 id; const gchar *jp, *unit, *result;

    g_variant_get(params, "(u&o&s&s)", &id, &jp, &unit, &result);
    if (!p->selected || p->selected->scope != sc->scope ||
        g_strcmp0(p->selected->name, unit) != 0)
        return;
    if (!g_strcmp0(result, "done") || !g_strcmp0(result, "canceled"))
        return;
    gchar *msg = g_strdup_printf("%s: job finished with result \"%s\"", unit, result);
    toast(p, msg);
    g_free(msg);
}

static void on_props_changed(GDBusConnection *c, const gchar *sender,
                             const gchar *path, const gchar *iface,
                             const gchar *signal, GVariant *params, gpointer data)
{
    (void)c; (void)sender; (void)iface; (void)signal;
    ScopeCtx *sc = data;
    Page *p = sc->page;
    const gchar *ifn = NULL;

    gchar *name = unit_name_from_path(path);
    if (!name)
        return;
    SvcItem *i = g_str_has_suffix(name, ".service") ? page_lookup(p, sc->scope, name) : NULL;
    g_free(name);
    if (!i)
        return;

    g_variant_get_child(params, 0, "&s", &ifn);
    GVariant *changed = g_variant_get_child_value(params, 1);
    SvcState before = i->state;
    Startup sb = i->startup;

    if (!g_strcmp0(ifn, SD_UNIT_IFACE)) {
        item_apply_unit_props(i, changed);
    } else if (!g_strcmp0(ifn, SD_SERVICE_IFACE)) {
        item_apply_service_props(i, changed);
    } else {
        g_variant_unref(changed);
        return;
    }
    g_variant_unref(changed);

    if (!svc_is_live(i->state) && before != i->state) {
        i->main_pid = 0;
        item_clear_metrics(i);
        item_derive(i);
    }
    item_emit(i);
    schedule_views(p, before != i->state || sb != i->startup,
                   before != i->state || sb != i->startup || sort_is_dynamic_metric(p));
}

static void scope_subscribe(ScopeCtx *sc)
{
    GDBusConnection *c = sc->conn;
    const gchar *mgr[] = { "UnitNew", "UnitRemoved", "UnitFilesChanged", "Reloading" };

    for (guint k = 0; k < G_N_ELEMENTS(mgr); k++)
        sc->subs[sc->n_subs++] = g_dbus_connection_signal_subscribe(
            c, SD_BUS_NAME, SD_MANAGER_IFACE, mgr[k], SD_PATH, NULL,
            G_DBUS_SIGNAL_FLAGS_NONE, on_manager_signal, sc, NULL);
    sc->subs[sc->n_subs++] = g_dbus_connection_signal_subscribe(
        c, SD_BUS_NAME, SD_MANAGER_IFACE, "JobRemoved", SD_PATH, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_job_removed, sc, NULL);
    sc->subs[sc->n_subs++] = g_dbus_connection_signal_subscribe(
        c, SD_BUS_NAME, DBUS_PROPS_IFACE, "PropertiesChanged", NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_props_changed, sc, NULL);

    /* Ask systemd to emit unit signals to us. Failure is not fatal. */
    g_dbus_connection_call(c, SD_BUS_NAME, SD_PATH, SD_MANAGER_IFACE, "Subscribe",
                           NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                           sc->page->cancel, NULL, NULL);
}

static void on_bus_ready(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    ScopeCtx *sc = data;
    GError *err = NULL;
    GDBusConnection *c = g_bus_get_finish(res, &err);

    if (err_cancelled(err)) {
        g_clear_error(&err);
        return;
    }
    if (!c) {
        g_clear_error(&err);
        return;
    }
    sc->conn = c;
    scope_subscribe(sc);
    scope_discover(sc->page, sc);
}

/* ------------------------------------------------------------------ */
/* Resource sampling (shared backend; cgroup accounting as fallback)   */
/* ------------------------------------------------------------------ */

static gboolean backend_sample(Page *p, SvcItem *i)
{
    ProcessSample s;
    memset(&s, 0, sizeof s);
    if (!app_process_sample_pid(p->app, (pid_t)i->main_pid, &s))
        return FALSE;
    i->cpu = s.cpu_percent;
    i->has_cpu = TRUE;
    i->mem = s.rss_bytes;
    i->has_mem = TRUE;
    if (s.n_threads) {
        i->tasks = s.n_threads;
        i->has_tasks = TRUE;
    }
    return TRUE;
}

static gboolean sample_cb(gpointer d)
{
    Page *p = d;
    GHashTableIter it; gpointer k, v;
    gboolean dirty = FALSE;
    guint budget = 24;

    if (!gtk_widget_get_mapped(p->root))
        return G_SOURCE_CONTINUE;

    g_hash_table_iter_init(&it, p->items);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        SvcItem *i = v;
        if (!svc_is_live(i->state)) {
            if (i->has_cpu || i->has_mem || i->has_tasks) {
                item_clear_metrics(i);
                item_emit(i);
                dirty = TRUE;
            }
            continue;
        }
        if (!i->meta_loaded) {
            if (budget) { fetch_meta(p, i); budget--; }
            continue;
        }
        if (backend_available()) {
            if (i->main_pid > 0 && backend_sample(p, i)) {
                item_emit(i);
                dirty = TRUE;
            }
        } else if (budget && !i->meta_pending) {
            fetch_meta(p, i);
            budget--;
        }
    }
    if (dirty)
        schedule_views(p, FALSE, sort_is_dynamic_metric(p));
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Selected-unit details                                               */
/* ------------------------------------------------------------------ */

typedef struct { Page *page; SvcItem *item; int pending; } DetailOp;

static void detail_op_done(DetailOp *op)
{
    if (--op->pending > 0)
        return;
    g_object_unref(op->item);
    g_free(op);
}

static void on_detail_props(GObject *src, GAsyncResult *res, gpointer data,
                            gboolean is_unit)
{
    (void)src;
    DetailOp *op = data;
    GError *err = NULL;
    GVariant *r = call_finish(res, &err);

    if (err_cancelled(err)) {
        g_clear_error(&err);
        detail_op_done(op);
        return;
    }
    if (r) {
        GVariant *d = g_variant_get_child_value(r, 0);
        if (is_unit) item_apply_unit_props(op->item, d);
        else         item_apply_service_props(op->item, d);
        g_variant_unref(d);
        g_variant_unref(r);
        item_emit(op->item);
        schedule_views(op->page, TRUE, FALSE);
    }
    g_clear_error(&err);
    detail_op_done(op);
}

static void on_detail_unit(GObject *s, GAsyncResult *r, gpointer d) { on_detail_props(s, r, d, TRUE); }
static void on_detail_service(GObject *s, GAsyncResult *r, gpointer d) { on_detail_props(s, r, d, FALSE); }

static void details_have_path(DetailOp *op)
{
    Page *p = op->page;
    ScopeCtx *sc = &p->scopes[op->item->scope];
    const gchar *path = op->item->unit_path;

    op->pending = 2;
    g_dbus_connection_call(sc->conn, SD_BUS_NAME, path, DBUS_PROPS_IFACE, "GetAll",
                           g_variant_new("(s)", SD_UNIT_IFACE), G_VARIANT_TYPE("(a{sv})"),
                           G_DBUS_CALL_FLAGS_NONE, -1, p->cancel, on_detail_unit, op);
    g_dbus_connection_call(sc->conn, SD_BUS_NAME, path, DBUS_PROPS_IFACE, "GetAll",
                           g_variant_new("(s)", SD_SERVICE_IFACE), G_VARIANT_TYPE("(a{sv})"),
                           G_DBUS_CALL_FLAGS_NONE, -1, p->cancel, on_detail_service, op);
}

static void on_load_unit(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    DetailOp *op = data;
    GError *err = NULL;
    GVariant *r = call_finish(res, &err);

    if (err_cancelled(err)) {
        g_clear_error(&err);
        g_object_unref(op->item);
        g_free(op);
        return;
    }
    if (!r) {
        g_clear_error(&err);
        g_object_unref(op->item);
        g_free(op);
        return;
    }
    const gchar *path = NULL;
    g_variant_get(r, "(&o)", &path);
    set_str(&op->item->unit_path, path);
    g_variant_unref(r);
    details_have_path(op);
}

static void details_fetch(Page *p, SvcItem *item)
{
    ScopeCtx *sc = &p->scopes[item->scope];
    if (!sc->conn)
        return;
    DetailOp *op = g_new0(DetailOp, 1);
    op->page = p;
    op->item = g_object_ref(item);
    if (item->unit_path && *item->unit_path) {
        details_have_path(op);
    } else {
        op->pending = 1;
        g_dbus_connection_call(sc->conn, SD_BUS_NAME, SD_PATH, SD_MANAGER_IFACE,
                               "LoadUnit", g_variant_new("(s)", item->name),
                               G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1,
                               p->cancel, on_load_unit, op);
    }
}

static void on_selected_changed(SvcItem *i, gpointer data)
{
    (void)i;
    details_refresh_ui(data);
}

static void services_select(Page *p, SvcItem *item)
{
    if (item == p->selected)
        return;
    if (p->selected) {
        g_signal_handler_disconnect(p->selected, p->sel_handler);
        g_object_unref(p->selected);
    }
    p->selected = item ? g_object_ref(item) : NULL;
    logs_reset(p);
    if (item) {
        p->sel_handler = g_signal_connect(item, "changed",
                                          G_CALLBACK(on_selected_changed), p);
        details_fetch(p, item);
        if (item->state == ST_FAILED)
            adw_expander_row_set_expanded(p->logs_exp, TRUE);
    }
    details_refresh_ui(p);
}

/* ------------------------------------------------------------------ */
/* Actions (D-Bus + Polkit)                                            */
/* ------------------------------------------------------------------ */

typedef struct { Page *page; ScopeCtx *sc; gchar *name; Action act; } ActionOp;

static void set_action_busy(Page *p, gboolean busy)
{
    p->action_busy = busy;
    gtk_widget_set_visible(p->action_spinner, busy);
    gtk_spinner_set_spinning(GTK_SPINNER(p->action_spinner), busy);
    if (p->selected)
        details_refresh_ui(p);
}

static gchar *action_error_text(GError *e)
{
    gchar *remote = g_dbus_error_get_remote_error(e);
    gchar *out;
    if (remote && (g_str_has_suffix(remote, "AccessDenied") ||
                   g_str_has_suffix(remote, "InteractiveAuthorizationRequired")))
        out = g_strdup("Authorization was denied, or no authentication agent is available.");
    else {
        g_dbus_error_strip_remote_error(e);
        out = g_strdup(e->message);
    }
    g_free(remote);
    return out;
}

static void action_op_free(ActionOp *op)
{
    g_free(op->name);
    g_free(op);
}

static void on_reload_after_files(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    ActionOp *op = data;
    GError *err = NULL;
    GVariant *r = call_finish(res, &err);

    if (err_cancelled(err)) {
        g_clear_error(&err);
        action_op_free(op);
        return;
    }
    Page *p = op->page;
    if (r) g_variant_unref(r);
    if (err) {
        gchar *t = action_error_text(err);
        gchar *m = g_strdup_printf("Could not reload systemd after %s: %s",
                                   ACT[op->act].label, t);
        toast(p, m);
        g_free(m); g_free(t);
        g_clear_error(&err);
    }
    scope_schedule_discover(op->sc);
    SvcItem *i = page_lookup(p, op->sc->scope, op->name);
    if (i) details_fetch(p, i);
    set_action_busy(p, FALSE);
    action_op_free(op);
}

static void on_action_done(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    ActionOp *op = data;
    GError *err = NULL;
    GVariant *r = call_finish(res, &err);

    if (err_cancelled(err)) {
        g_clear_error(&err);
        action_op_free(op);
        return;
    }
    Page *p = op->page;
    if (!r) {
        gchar *t = action_error_text(err);
        gchar *m = g_strdup_printf("%s failed for %s: %s", ACT[op->act].label, op->name, t);
        toast(p, m);
        g_free(m); g_free(t);
        g_clear_error(&err);
        /* re-sync with the real system state */
        scope_schedule_discover(op->sc);
        SvcItem *i = page_lookup(p, op->sc->scope, op->name);
        if (i) details_fetch(p, i);
        set_action_busy(p, FALSE);
        action_op_free(op);
        return;
    }
    g_variant_unref(r);

    gboolean files_op = op->act == ACT_ENABLE || op->act == ACT_DISABLE ||
                        op->act == ACT_MASK || op->act == ACT_UNMASK;
    if (files_op) {
        gchar *m = g_strdup_printf("%s %s", ACT[op->act].done, op->name);
        toast(p, m);
        g_free(m);
        /* unit-file changes only take effect after a manager reload */
        g_dbus_connection_call(op->sc->conn, SD_BUS_NAME, SD_PATH, SD_MANAGER_IFACE,
                               "Reload", NULL, NULL,
                               G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                               -1, p->cancel, on_reload_after_files, op);
        return;
    }
    gchar *m = g_strdup_printf("%s requested: %s", ACT[op->act].label, op->name);
    toast(p, m);
    g_free(m);
    set_action_busy(p, FALSE);
    action_op_free(op);
}

static void run_action(Page *p, SvcItem *item, Action act)
{
    ScopeCtx *sc = &p->scopes[item->scope];
    if (!sc->conn || p->action_busy)
        return;

    ActionOp *op = g_new0(ActionOp, 1);
    op->page = p; op->sc = sc; op->name = g_strdup(item->name); op->act = act;

    GVariant *params;
    const gchar *names[] = { item->name, NULL };
    switch (act) {
    case ACT_START: case ACT_STOP: case ACT_RESTART: case ACT_RELOAD:
        params = g_variant_new("(ss)", item->name, "replace"); break;
    case ACT_ENABLE: params = g_variant_new("(^asbb)", names, FALSE, FALSE); break;
    case ACT_DISABLE: params = g_variant_new("(^asb)", names, FALSE); break;
    case ACT_MASK: params = g_variant_new("(^asbb)", names, FALSE, FALSE); break;
    default: params = g_variant_new("(^asb)", names, FALSE); break;
    }
    set_action_busy(p, TRUE);
    /* ALLOW_INTERACTIVE_AUTHORIZATION lets polkit prompt through the
     * session's authentication agent; this process never sees a password. */
    g_dbus_connection_call(sc->conn, SD_BUS_NAME, SD_PATH, SD_MANAGER_IFACE,
                           ACT[act].method, params, NULL,
                           G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                           -1, p->cancel, on_action_done, op);
}

static gboolean name_has(const SvcItem *i, const char *const *needles)
{
    for (; *needles; needles++)
        if (strstr(i->name_fold, *needles))
            return TRUE;
    return FALSE;
}

static gchar *consequence_text(const SvcItem *i, Action act)
{
    static const char *const net[] = { "networkmanager", "systemd-networkd", "systemd-resolved",
        "wpa_supplicant", "iwd", "dhcpcd", "sshd", "openvpn", "wireguard", NULL };
    static const char *const dm[] = { "gdm", "sddm", "lightdm", "greetd", "ly.service", NULL };
    static const char *const core[] = { "dbus", "systemd-logind", "polkit", "systemd-journald",
        "systemd-udevd", NULL };
    GString *s = g_string_new(NULL);

    if (act == ACT_MASK) {
        g_string_append(s, "A masked service cannot be started manually or by other units until it is unmasked. Masking does not stop it if it is running now.");
    } else if (name_has(i, net)) {
        g_string_append_printf(s, "%s is network-related. Network connectivity, including remote sessions, may be interrupted while it is %s.",
                               i->name, act == ACT_STOP ? "stopped" : "restarting");
    } else if (name_has(i, dm)) {
        g_string_append(s, "This is a display or login manager. Your graphical session and open applications may be closed.");
    } else if (name_has(i, core)) {
        g_string_append(s, "Other services and your session depend on this component. Interrupting it can destabilize the system.");
    } else if (act == ACT_STOP) {
        g_string_append(s, "The service stops running until it is started again.");
    } else {
        g_string_append(s, "The service is briefly unavailable and active connections it handles may be interrupted.");
    }
    guint rb = i->deps[DEP_REQUIRED_BY] ? g_strv_length(i->deps[DEP_REQUIRED_BY]) : 0;
    if (rb)
        g_string_append_printf(s, "\n\n%u other unit%s declare%s a requirement on it.",
                               rb, rb == 1 ? "" : "s", rb == 1 ? "s" : "");
    g_string_append(s, i->scope == SCOPE_USER
                           ? "\n\nThis is a user service in your session."
                           : "\n\nThis is a system service; you may be asked to authenticate.");
    return g_string_free(s, FALSE);
}

typedef struct { Page *page; SvcItem *item; Action act; } ConfirmCtx;

static void on_confirm(GObject *src, GAsyncResult *res, gpointer data)
{
    ConfirmCtx *c = data;
    const gchar *r = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(src), res);
    if (!g_strcmp0(r, "confirm"))
        run_action(c->page, c->item, c->act);
    g_object_unref(c->item);
    g_free(c);
}

static gboolean action_needs_confirm(const SvcItem *i, Action act)
{
    if (act == ACT_MASK) return TRUE;
    if (act == ACT_STOP) return TRUE;
    if (act == ACT_RESTART) return svc_is_active(i->state); /* failed/stopped: nothing to disrupt */
    return FALSE;
}

static void services_do_action(Page *p, Action act)
{
    SvcItem *i = p->selected;
    if (!i || p->action_busy)
        return;
    if (!action_needs_confirm(i, act)) {
        run_action(p, i, act);
        return;
    }
    gchar *heading = g_strdup_printf("%s %s?", ACT[act].label, i->name);
    gchar *body = consequence_text(i, act);
    AdwDialog *dlg = adw_alert_dialog_new(heading, body);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg), "cancel", "Cancel",
                                   "confirm", ACT[act].label, NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "confirm",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "cancel");
    ConfirmCtx *c = g_new0(ConfirmCtx, 1);
    c->page = p; c->item = g_object_ref(i); c->act = act;
    adw_alert_dialog_choose(ADW_ALERT_DIALOG(dlg), p->root, NULL, on_confirm, c);
    g_free(heading);
    g_free(body);
}

/* thin named wrappers matching the module's action vocabulary */
static void services_start(Page *p)   { services_do_action(p, ACT_START); }
static void services_stop(Page *p)    { services_do_action(p, ACT_STOP); }
static void services_restart(Page *p) { services_do_action(p, ACT_RESTART); }
static void services_reload(Page *p)  { services_do_action(p, ACT_RELOAD); }
static void services_enable(Page *p)  { services_do_action(p, ACT_ENABLE); }
static void services_disable(Page *p) { services_do_action(p, ACT_DISABLE); }
static void services_mask(Page *p)    { services_do_action(p, ACT_MASK); }
static void services_unmask(Page *p)  { services_do_action(p, ACT_UNMASK); }

static void on_action_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    Page *p = g_object_get_data(G_OBJECT(b), "page");
    Action a = GPOINTER_TO_INT(data);
    if (a == ACT_START && p->primary_act != ACT_START)
        a = p->primary_act; /* primary button is Start or Restart */
    switch (a) {
    case ACT_START: services_start(p); break;
    case ACT_STOP: services_stop(p); break;
    case ACT_RESTART: services_restart(p); break;
    case ACT_RELOAD: services_reload(p); break;
    case ACT_ENABLE: services_enable(p); break;
    case ACT_DISABLE: services_disable(p); break;
    case ACT_MASK: services_mask(p); break;
    case ACT_UNMASK: services_unmask(p); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* Journal (sd-journal in a worker thread)                             */
/* ------------------------------------------------------------------ */

typedef struct { guint64 usec; gint prio; gchar *msg; } LogLine;
typedef struct { gchar *unit; Scope scope; gchar *cursor; guint limit; } LogReq;
typedef struct { GPtrArray *lines; gchar *oldest; gboolean more; } LogRes;
typedef struct { Page *page; guint gen; gboolean older; } LogCtx;

static void log_line_free(gpointer l) { g_free(((LogLine *)l)->msg); g_free(l); }
static void log_req_free(gpointer d) { LogReq *r = d; g_free(r->unit); g_free(r->cursor); g_free(r); }
static void log_res_free(gpointer d)
{
    LogRes *r = d;
    g_ptr_array_free(r->lines, TRUE);
    g_free(r->oldest);
    g_free(r);
}

static void log_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src;
    LogReq *rq = data;
    sd_journal *j = NULL;
    int r = sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY);

    if (r < 0) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Cannot open the journal: %s", g_strerror(-r));
        return;
    }
    gboolean sys = rq->scope == SCOPE_SYSTEM;
    gchar *m1 = g_strdup_printf(sys ? "_SYSTEMD_UNIT=%s" : "_SYSTEMD_USER_UNIT=%s", rq->unit);
    gchar *m2 = g_strdup_printf(sys ? "UNIT=%s" : "USER_UNIT=%s", rq->unit);
    sd_journal_add_match(j, m1, 0);
    sd_journal_add_disjunction(j);
    sd_journal_add_match(j, m2, 0);
    g_free(m1); g_free(m2);

    if (rq->cursor) sd_journal_seek_cursor(j, rq->cursor);
    else            sd_journal_seek_tail(j);

    LogRes *res = g_new0(LogRes, 1);
    res->lines = g_ptr_array_new_with_free_func(log_line_free);
    gboolean skip = rq->cursor != NULL;

    r = sd_journal_previous(j);
    while (r > 0 && !g_cancellable_is_cancelled(c)) {
        if (skip) {
            skip = FALSE;
            if (sd_journal_test_cursor(j, rq->cursor) > 0) {
                r = sd_journal_previous(j);
                continue;
            }
        }
        if (res->lines->len >= rq->limit) {
            res->more = TRUE;
            break;
        }
        LogLine *l = g_new0(LogLine, 1);
        uint64_t us = 0;
        const char *d; size_t len;
        sd_journal_get_realtime_usec(j, &us);
        l->usec = us;
        l->prio = 6;
        if (sd_journal_get_data(j, "PRIORITY", (const void **)&d, &len) >= 0 && len > 9)
            l->prio = d[9] - '0';
        if (sd_journal_get_data(j, "MESSAGE", (const void **)&d, &len) >= 0 && len > 8)
            l->msg = g_utf8_make_valid(d + 8, (gssize)(len - 8));
        else
            l->msg = g_strdup("");
        g_ptr_array_add(res->lines, l);
        g_free(res->oldest);
        res->oldest = NULL;
        char *cur = NULL;
        if (sd_journal_get_cursor(j, &cur) >= 0) {
            res->oldest = g_strdup(cur);
            free(cur);
        }
        r = sd_journal_previous(j);
    }
    sd_journal_close(j);

    /* collected newest -> oldest; present oldest -> newest */
    guint n = res->lines->len;
    for (guint a = 0; a + 1 < n - a; a++) {
        gpointer t = res->lines->pdata[a];
        res->lines->pdata[a] = res->lines->pdata[n - 1 - a];
        res->lines->pdata[n - 1 - a] = t;
    }
    g_task_return_pointer(task, res, log_res_free);
}

static void logs_reset(Page *p)
{
    p->log_gen++;
    g_clear_pointer(&p->log_cursor, g_free);
    p->log_more = FALSE;
    p->log_loaded = FALSE;
    p->log_busy = FALSE;
    p->log_lines = 0;
    gtk_text_buffer_set_text(p->log_buf, "", 0);
    gtk_widget_set_sensitive(p->log_older, FALSE);
    gtk_widget_set_visible(p->log_spinner, FALSE);
    gtk_label_set_text(GTK_LABEL(p->log_status), "");
    if (p->selected && adw_expander_row_get_expanded(p->logs_exp))
        logs_request(p, FALSE);
}

static void logs_insert(Page *p, LogRes *r, gboolean older)
{
    GtkTextIter it;
    if (older) gtk_text_buffer_get_start_iter(p->log_buf, &it);
    else       gtk_text_buffer_get_end_iter(p->log_buf, &it);

    for (guint n = 0; n < r->lines->len; n++) {
        LogLine *l = g_ptr_array_index(r->lines, n);
        GDateTime *dt = g_date_time_new_from_unix_local((gint64)(l->usec / 1000000));
        gchar *ts = dt ? g_date_time_format(dt, "%b %d %H:%M:%S  ") : g_strdup("");
        gchar *msg = g_strdup_printf("%s\n", l->msg);
        gtk_text_buffer_insert_with_tags_by_name(p->log_buf, &it, ts, -1, "ts", NULL);
        if (l->prio <= 3)
            gtk_text_buffer_insert_with_tags_by_name(p->log_buf, &it, msg, -1, "err", NULL);
        else
            gtk_text_buffer_insert(p->log_buf, &it, msg, -1);
        g_free(ts); g_free(msg);
        if (dt) g_date_time_unref(dt);
    }
    if (!older) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(p->log_buf, &end);
        GtkTextMark *m = gtk_text_buffer_get_mark(p->log_buf, "end");
        gtk_text_buffer_move_mark(p->log_buf, m, &end);
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(p->log_view), m, 0.0, TRUE, 0.0, 1.0);
    }
}

static void on_logs_done(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    LogCtx *c = data;
    GError *err = NULL;
    LogRes *r = g_task_propagate_pointer(G_TASK(res), &err);

    if (err_cancelled(err)) {
        g_clear_error(&err);
        g_free(c);
        return;
    }
    Page *p = c->page;
    if (c->gen != p->log_gen) { /* selection changed meanwhile */
        if (r) log_res_free(r);
        g_clear_error(&err);
        g_free(c);
        return;
    }
    p->log_busy = FALSE;
    p->log_loaded = TRUE;
    gtk_widget_set_visible(p->log_spinner, FALSE);
    gtk_spinner_set_spinning(GTK_SPINNER(p->log_spinner), FALSE);

    if (!r) {
        gtk_label_set_text(GTK_LABEL(p->log_status), err ? err->message : "Journal unavailable.");
    } else {
        logs_insert(p, r, c->older);
        p->log_lines += r->lines->len;
        if (r->oldest) {
            g_free(p->log_cursor);
            p->log_cursor = g_strdup(r->oldest);
        }
        p->log_more = r->more && p->log_lines < LOG_MAX_LINES;
        if (p->log_lines == 0)
            gtk_label_set_text(GTK_LABEL(p->log_status),
                               "No journal entries visible for this unit. Reading system logs may require membership in the systemd-journal or adm group.");
        else if (r->more && p->log_lines >= LOG_MAX_LINES)
            gtk_label_set_text(GTK_LABEL(p->log_status), "Display limit reached.");
        else
            gtk_label_set_text(GTK_LABEL(p->log_status), "");
        log_res_free(r);
    }
    gtk_widget_set_sensitive(p->log_older, p->log_more);
    g_clear_error(&err);
    g_free(c);
}

static void logs_request(Page *p, gboolean older)
{
    if (!p->selected || p->log_busy)
        return;
    LogReq *rq = g_new0(LogReq, 1);
    rq->unit = g_strdup(p->selected->name);
    rq->scope = p->selected->scope;
    rq->limit = LOG_CHUNK;
    if (older && p->log_cursor)
        rq->cursor = g_strdup(p->log_cursor);

    LogCtx *c = g_new0(LogCtx, 1);
    c->page = p; c->gen = p->log_gen; c->older = older;
    p->log_busy = TRUE;
    gtk_widget_set_visible(p->log_spinner, TRUE);
    gtk_spinner_set_spinning(GTK_SPINNER(p->log_spinner), TRUE);

    GTask *t = g_task_new(NULL, p->cancel, on_logs_done, c);
    g_task_set_task_data(t, rq, log_req_free);
    g_task_run_in_thread(t, log_thread);
    g_object_unref(t);
}

static void on_logs_expanded(GObject *o, GParamSpec *ps, gpointer data)
{
    (void)ps;
    Page *p = data;
    if (adw_expander_row_get_expanded(ADW_EXPANDER_ROW(o)) && !p->log_loaded)
        logs_request(p, FALSE);
}

static void on_logs_older(GtkButton *b, gpointer data)
{
    (void)b;
    logs_request(data, TRUE);
}

static void on_logs_copy(GtkButton *b, gpointer data)
{
    (void)b;
    Page *p = data;
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(p->log_buf, &s, &e);
    gchar *t = gtk_text_buffer_get_text(p->log_buf, &s, &e, FALSE);
    gdk_clipboard_set_text(gtk_widget_get_clipboard(p->root), t);
    g_free(t);
    toast(p, "Log copied");
}

/* ------------------------------------------------------------------ */
/* Presentation: chips and list rows                                   */
/* ------------------------------------------------------------------ */

static void install_css(void)
{
    static gboolean done;
    if (done)
        return;
    done = TRUE;
    GtkCssProvider *cp = gtk_css_provider_new();
    gtk_css_provider_load_from_string(cp,
        ".svc-mono .subtitle { font-family: monospace; }"
        ".svc-chip { padding: 1px 8px 1px 6px; border-radius: 99px; font-size: 0.82em; font-weight: 700; }"
        ".svc-chip.running { background: alpha(@success_color, .16); color: @success_color; }"
        ".svc-chip.exited  { background: alpha(@accent_color, .14); color: @accent_color; }"
        ".svc-chip.stopped { background: alpha(currentColor, .10); }"
        ".svc-chip.failed  { background: alpha(@error_color, .18); color: @error_color; }"
        ".svc-chip.busy    { background: alpha(@warning_color, .20); color: @warning_color; }"
        ".svc-chip.unknown { background: alpha(currentColor, .10); }"
        ".svc-row-failed { border-left: 3px solid @error_color; padding-left: 7px; }"
        ".svc-log { font-family: monospace; font-size: 0.9em; }"
        ".svc-stat-num { font-weight: 800; font-size: 1.25em; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(cp), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(cp);
}

static Chip chip_new(void)
{
    Chip c;
    c.box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    c.icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(c.icon), 12);
    c.label = gtk_label_new("");
    gtk_box_append(GTK_BOX(c.box), c.icon);
    gtk_box_append(GTK_BOX(c.box), c.label);
    gtk_widget_add_css_class(c.box, "svc-chip");
    gtk_widget_set_valign(c.box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(c.box, GTK_ALIGN_END);
    return c;
}

static void chip_set(Chip *c, SvcState s)
{
    for (int k = 0; k < ST_COUNT; k++)
        gtk_widget_remove_css_class(c->box, STATE_META[k].css);
    gtk_widget_add_css_class(c->box, STATE_META[s].css);
    gtk_image_set_from_icon_name(GTK_IMAGE(c->icon), STATE_META[s].icon);
    gtk_label_set_text(GTK_LABEL(c->label), STATE_META[s].label);
}

typedef struct {
    GtkWidget *outer, *name, *scope, *startup, *desc, *metrics;
    Chip chip;
    SvcItem *item;
    gulong handler;
} RowW;

static void row_refresh(RowW *rw)
{
    SvcItem *i = rw->item;
    if (!i)
        return;
    gtk_label_set_text(GTK_LABEL(rw->name), i->label);
    gtk_widget_set_tooltip_text(rw->name, i->name);
    gtk_label_set_text(GTK_LABEL(rw->desc), i->description ? i->description : "");
    gtk_widget_set_visible(rw->scope, i->scope == SCOPE_USER);
    gtk_label_set_text(GTK_LABEL(rw->startup), STARTUP_LABEL[i->startup]);
    chip_set(&rw->chip, i->state);
    if (i->state == ST_FAILED) gtk_widget_add_css_class(rw->outer, "svc-row-failed");
    else                       gtk_widget_remove_css_class(rw->outer, "svc-row-failed");

    GString *m = g_string_new(NULL);
    if (i->main_pid)
        g_string_append_printf(m, "PID %u", i->main_pid);
    if (i->has_cpu)
        g_string_append_printf(m, "%s%.1f%% CPU", m->len ? "  ·  " : "", i->cpu);
    if (i->has_mem) {
        gchar *sz = g_format_size(i->mem);
        g_string_append_printf(m, "%s%s", m->len ? "  ·  " : "", sz);
        g_free(sz);
    }
    gtk_label_set_text(GTK_LABEL(rw->metrics), m->str);
    gtk_widget_set_visible(rw->metrics, m->len > 0);
    g_string_free(m, TRUE);
}

static void row_item_changed(SvcItem *i, gpointer data)
{
    (void)i;
    row_refresh(data);
}

static void row_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f; (void)d;
    RowW *rw = g_new0(RowW, 1);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *l1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *l2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    gtk_widget_set_margin_top(outer, 7);
    gtk_widget_set_margin_bottom(outer, 7);
    gtk_widget_set_margin_start(outer, 10);
    gtk_widget_set_margin_end(outer, 10);

    rw->outer = outer;
    rw->name = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(rw->name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(rw->name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(rw->name, TRUE);
    gtk_widget_add_css_class(rw->name, "heading");

    rw->scope = gtk_label_new("user");
    gtk_widget_add_css_class(rw->scope, "caption");
    gtk_widget_add_css_class(rw->scope, "dim-label");
    rw->startup = gtk_label_new("");
    gtk_widget_add_css_class(rw->startup, "caption");
    gtk_widget_add_css_class(rw->startup, "dim-label");
    rw->chip = chip_new();

    rw->desc = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(rw->desc), 0);
    gtk_label_set_ellipsize(GTK_LABEL(rw->desc), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(rw->desc, TRUE);
    gtk_widget_add_css_class(rw->desc, "caption");
    gtk_widget_add_css_class(rw->desc, "dim-label");
    rw->metrics = gtk_label_new("");
    gtk_widget_add_css_class(rw->metrics, "caption");
    gtk_widget_add_css_class(rw->metrics, "numeric");

    gtk_box_append(GTK_BOX(l1), rw->name);
    gtk_box_append(GTK_BOX(l1), rw->scope);
    gtk_box_append(GTK_BOX(l1), rw->startup);
    gtk_box_append(GTK_BOX(l1), rw->chip.box);
    gtk_box_append(GTK_BOX(l2), rw->desc);
    gtk_box_append(GTK_BOX(l2), rw->metrics);
    gtk_box_append(GTK_BOX(outer), l1);
    gtk_box_append(GTK_BOX(outer), l2);

    gtk_list_item_set_child(li, outer);
    g_object_set_data_full(G_OBJECT(outer), "rw", rw, g_free);
}

static void row_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f; (void)d;
    RowW *rw = g_object_get_data(G_OBJECT(gtk_list_item_get_child(li)), "rw");
    rw->item = g_object_ref(gtk_list_item_get_item(li));
    rw->handler = g_signal_connect(rw->item, "changed", G_CALLBACK(row_item_changed), rw);
    row_refresh(rw);
}

static void row_unbind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f; (void)d;
    RowW *rw = g_object_get_data(G_OBJECT(gtk_list_item_get_child(li)), "rw");
    if (rw->item) {
        g_signal_handler_disconnect(rw->item, rw->handler);
        g_clear_object(&rw->item);
    }
}

static void header_setup(GtkSignalListItemFactory *f, GtkListHeader *h, gpointer d)
{
    (void)f; (void)d;
    GtkWidget *l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_add_css_class(l, "heading");
    gtk_widget_add_css_class(l, "dim-label");
    gtk_widget_set_margin_top(l, 10);
    gtk_widget_set_margin_bottom(l, 4);
    gtk_widget_set_margin_start(l, 10);
    gtk_list_header_set_child(h, l);
}

static void header_bind(GtkSignalListItemFactory *f, GtkListHeader *h, gpointer d)
{
    (void)f;
    Page *p = d;
    SvcItem *i = gtk_list_header_get_item(h);
    if (i)
        gtk_label_set_text(GTK_LABEL(gtk_list_header_get_child(h)), group_title(p, i));
}

/* ------------------------------------------------------------------ */
/* Presentation: detail panel updates                                  */
/* ------------------------------------------------------------------ */

static void row_set(AdwActionRow *r, const gchar *text)
{
    gboolean show = text && *text;
    gtk_widget_set_visible(GTK_WIDGET(r), show);
    if (show)
        adw_action_row_set_subtitle(r, text);
}

static void row_set_take(AdwActionRow *r, gchar *text)
{
    row_set(r, text);
    g_free(text);
}

static void dep_activated(AdwActionRow *row, gpointer data)
{
    Page *p = data;
    const gchar *key = g_object_get_data(G_OBJECT(row), "key");
    SvcItem *i = key ? g_hash_table_lookup(p->items, key) : NULL;
    if (!i)
        return;

    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->sel));
    for (guint k = 0; k < n; k++) {
        SvcItem *c = g_list_model_get_item(G_LIST_MODEL(p->sel), k);
        gboolean hit = (c == i);
        g_object_unref(c);
        if (hit) {
            gtk_single_selection_set_selected(p->sel, k);
            gtk_list_view_scroll_to(GTK_LIST_VIEW(p->list_view), k, GTK_LIST_SCROLL_SELECT, NULL);
            return;
        }
    }
    /* not visible under the current filters: inspect it anyway */
    p->syncing = TRUE;
    gtk_single_selection_set_selected(p->sel, GTK_INVALID_LIST_POSITION);
    p->syncing = FALSE;
    services_select(p, i);
    toast(p, "That service is hidden by the current filters");
}

static void deps_rebuild(Page *p, SvcItem *i)
{
    for (int k = 0; k < DEP_COUNT; k++) {
        for (guint n = 0; n < p->dep_rows[k]->len; n++)
            adw_expander_row_remove(p->dep_exp[k], g_ptr_array_index(p->dep_rows[k], n));
        g_ptr_array_set_size(p->dep_rows[k], 0);

        guint total = i->deps[k] ? g_strv_length(i->deps[k]) : 0;
        gchar *sub = g_strdup_printf("%u", total);
        adw_expander_row_set_subtitle(p->dep_exp[k], sub);
        g_free(sub);
        gtk_widget_set_sensitive(GTK_WIDGET(p->dep_exp[k]), total > 0);

        for (guint n = 0; n < total && n < DEP_SHOW_MAX; n++) {
            const gchar *dn = i->deps[k][n];
            AdwActionRow *r = ADW_ACTION_ROW(adw_action_row_new());
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), dn);
            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(r), FALSE);
            gtk_widget_add_css_class(GTK_WIDGET(r), "svc-mono");
            SvcItem *t = page_lookup(p, i->scope, dn);
            if (t) {
                g_object_set_data_full(G_OBJECT(r), "key", g_strdup(t->key), g_free);
                adw_action_row_add_suffix(r, gtk_image_new_from_icon_name("go-next-symbolic"));
                gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r), TRUE);
                g_signal_connect(r, "activated", G_CALLBACK(dep_activated), p);
            }
            adw_expander_row_add_row(p->dep_exp[k], GTK_WIDGET(r));
            g_ptr_array_add(p->dep_rows[k], r);
        }
    }
    p->dep_item = i;
    p->dep_rev = i->dep_rev;
}

static void actions_update(Page *p, SvcItem *i)
{
    gboolean act = svc_is_active(i->state);
    gboolean idle = !p->action_busy;

    p->primary_act = (act || i->state == ST_FAILED) ? ACT_RESTART : ACT_START;
    gtk_button_set_label(GTK_BUTTON(p->btn[ACT_START]), ACT[p->primary_act].label);
    gtk_widget_set_visible(p->btn[ACT_START], !i->masked);
    gtk_widget_set_visible(p->btn[ACT_RELOAD], !i->masked && i->state == ST_RUNNING && i->can_reload);
    gtk_widget_set_visible(p->btn[ACT_ENABLE], !i->masked && i->startup == SU_DISABLED);
    gtk_widget_set_visible(p->btn[ACT_DISABLE], i->startup == SU_ENABLED);
    gtk_widget_set_visible(p->btn[ACT_UNMASK], i->masked);
    gtk_widget_set_visible(p->btn[ACT_STOP], act);
    gtk_widget_set_visible(p->btn[ACT_MASK], !i->masked);
    gtk_widget_set_visible(p->btn[ACT_RESTART], FALSE);
    gtk_widget_set_visible(p->disruptive_box, act || !i->masked);
    for (int k = 0; k < ACT_COUNT; k++)
        gtk_widget_set_sensitive(p->btn[k], idle);
}

static void details_refresh_ui(Page *p)
{
    SvcItem *i = p->selected;
    if (!i) {
        gtk_stack_set_visible_child_name(GTK_STACK(p->detail_stack), "empty");
        return;
    }
    gtk_stack_set_visible_child_name(GTK_STACK(p->detail_stack), "details");

    gtk_label_set_text(GTK_LABEL(p->d_title), i->name);
    gtk_label_set_text(GTK_LABEL(p->d_desc), i->description && *i->description ? i->description : "No description available");
    chip_set(&p->d_chip, i->state);
    gchar *su = g_strdup_printf("Startup: %s", STARTUP_LABEL[i->startup]);
    gtk_label_set_text(GTK_LABEL(p->d_startup), su);
    g_free(su);
    gtk_label_set_text(GTK_LABEL(p->d_scope), i->scope == SCOPE_USER ? "User service" : "System service");

    /* status */
    row_set(p->rows[R_ACTIVE], i->active_state);
    row_set(p->rows[R_SUB], i->sub_state);
    row_set(p->rows[R_LOAD], i->load_state);
    row_set(p->rows[R_FILE], i->unit_file_state && *i->unit_file_state ? i->unit_file_state : "unknown");
    row_set_take(p->rows[R_ACT_TS], fmt_time(i->active_enter));
    row_set_take(p->rows[R_DEACT_TS], i->state == ST_STOPPED || i->state == ST_FAILED ? fmt_time(i->active_exit) : NULL);
    if (i->state == ST_FAILED) {
        gchar *f = g_strdup_printf("%s%s%s", i->sub_state ? i->sub_state : "failed",
                                   i->result && *i->result ? " — result: " : "",
                                   i->result ? i->result : "");
        gchar *when = fmt_time(i->inactive_enter);
        if (when) {
            gchar *f2 = g_strdup_printf("%s (at %s)", f, when);
            g_free(f); f = f2;
        }
        g_free(when);
        row_set_take(p->rows[R_FAILURE], f);
    } else {
        row_set(p->rows[R_FAILURE], NULL);
    }

    /* execution */
    gchar *s;
    row_set_take(p->rows[R_MAINPID], i->main_pid ? g_strdup_printf("%u", i->main_pid) : NULL);
    row_set_take(p->rows[R_CTRLPID], i->control_pid ? g_strdup_printf("%u", i->control_pid) : NULL);
    row_set(p->rows[R_EXEC], i->exec_path);
    row_set(p->rows[R_CMD], i->cmdline);
    row_set(p->rows[R_USER], i->user && *i->user ? i->user
                               : (i->scope == SCOPE_USER ? g_get_user_name() : "root (default)"));
    row_set(p->rows[R_GROUP], i->group);
    row_set(p->rows[R_WORKDIR], i->workdir);

    /* resources */
    row_set_take(p->rows[R_CPU], i->has_cpu ? g_strdup_printf("%.1f %%", i->cpu) : NULL);
    row_set_take(p->rows[R_MEM], i->has_mem ? g_format_size(i->mem) : NULL);
    row_set_take(p->rows[R_TASKS], i->has_tasks ? g_strdup_printf("%" G_GUINT64_FORMAT, i->tasks) : NULL);
    row_set_take(p->rows[R_START], svc_is_live(i->state) ? fmt_time(i->exec_start) : NULL);
    s = NULL;
    if (svc_is_live(i->state) && i->active_enter) {
        gint64 now = g_get_real_time();
        if (now > (gint64)i->active_enter)
            s = fmt_duration((guint64)(now - (gint64)i->active_enter) / 1000000);
    }
    row_set_take(p->rows[R_RUNTIME], s);

    /* unit */
    row_set(p->rows[R_FRAG], i->fragment_path);
    s = (i->drop_ins && *i->drop_ins) ? g_strjoinv("\n", i->drop_ins) : NULL;
    row_set_take(p->rows[R_DROPIN], s);

    if (p->dep_item != i || p->dep_rev != i->dep_rev)
        deps_rebuild(p, i);
    actions_update(p, i);
}

/* ------------------------------------------------------------------ */
/* UI construction                                                     */
/* ------------------------------------------------------------------ */

static GtkWidget *mk_label(const char *t, const char *c1, const char *c2)
{
    GtkWidget *l = gtk_label_new(t);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (c1) gtk_widget_add_css_class(l, c1);
    if (c2) gtk_widget_add_css_class(l, c2);
    return l;
}

static GtkWidget *services_create_header(Page *p, GtkWidget **search_out)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");

    gtk_box_append(GTK_BOX(titles), mk_label("Services", "title-1", NULL));
    gtk_box_append(GTK_BOX(titles), mk_label("Inspect and control systemd services for the system and your user session.", "dim-label", NULL));

    GtkWidget *search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), "Search name, description, PID or executable");
    gtk_widget_set_hexpand(search, TRUE);
    *search_out = search;

    p->spinner = gtk_spinner_new();
    gtk_widget_set_visible(p->spinner, FALSE);
    gtk_widget_set_tooltip_text(refresh, "Refresh services");
    gtk_widget_add_css_class(refresh, "flat");
    g_signal_connect_swapped(refresh, "clicked", G_CALLBACK(services_refresh), p);

    gtk_box_append(GTK_BOX(row), search);
    gtk_box_append(GTK_BOX(row), p->spinner);
    gtk_box_append(GTK_BOX(row), refresh);
    gtk_box_append(GTK_BOX(box), titles);
    gtk_box_append(GTK_BOX(box), row);
    return box;
}

static GtkWidget *services_create_summary(Page *p)
{
    static const char *const names[6] = { "Total", "Running", "Stopped", "Failed", "Enabled", "Disabled" };
    GtkWidget *box = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(box), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(box), 6);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(box), 24);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(box), FALSE);
    for (int n = 0; n < 6; n++) {
        GtkWidget *c = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        p->stat[n] = mk_label("0", "svc-stat-num", "numeric");
        if (n == 3) gtk_widget_add_css_class(p->stat[n], "error");
        gtk_box_append(GTK_BOX(c), p->stat[n]);
        gtk_box_append(GTK_BOX(c), mk_label(names[n], "dim-label", "caption"));
        gtk_flow_box_append(GTK_FLOW_BOX(box), c);
    }
    return box;
}

static void on_filter_selected(GObject *o, GParamSpec *ps, gpointer data)
{
    (void)ps;
    Page *p = data;
    FilterDD *f = g_object_get_data(o, "fdd");
    if (p->building || !f->ids->len)
        return;
    guint s = gtk_drop_down_get_selected(f->dd);
    if (s >= f->ids->len)
        return;
    f->current = g_array_index(f->ids, int, s);
    gtk_filter_changed(GTK_FILTER(p->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static void fdd_init(Page *p, FilterDD *f, const Opt *opts, int n)
{
    f->opts = opts; f->n = n;
    f->ids = g_array_new(FALSE, FALSE, sizeof(int));
    f->mask = G_MAXUINT;
    f->dd = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
    g_object_set_data(G_OBJECT(f->dd), "fdd", f);
    g_signal_connect(f->dd, "notify::selected", G_CALLBACK(on_filter_selected), p);
}

static void on_sort_selected(GObject *o, GParamSpec *ps, gpointer data)
{
    (void)ps;
    Page *p = data;
    p->sort_col = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    gtk_sorter_changed(GTK_SORTER(p->col_sorter), GTK_SORTER_CHANGE_DIFFERENT);
}

static void on_group_selected(GObject *o, GParamSpec *ps, gpointer data)
{
    (void)ps;
    Page *p = data;
    p->group_mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    gtk_sorter_changed(GTK_SORTER(p->group_sorter), GTK_SORTER_CHANGE_DIFFERENT);
}

static GtkWidget *captioned(const char *cap, GtkWidget *w)
{
    GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(b), mk_label(cap, "caption", "dim-label"));
    gtk_box_append(GTK_BOX(b), w);
    return b;
}

static void on_search_changed(GtkSearchEntry *e, gpointer data)
{
    Page *p = data;
    const gchar *q = gtk_editable_get_text(GTK_EDITABLE(e));
    g_strfreev(p->tokens);
    p->tokens = NULL;
    if (q && *q) {
        gchar *f = g_utf8_casefold(q, -1);
        p->tokens = g_strsplit(f, " ", -1);
        g_free(f);
    }
    gtk_filter_changed(GTK_FILTER(p->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static GtkWidget *services_create_filters(Page *p)
{
    GtkWidget *fb = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(fb), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(fb), 5);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(fb), 8);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(fb), FALSE);

    fdd_init(p, &p->f_state, STATE_OPTS, G_N_ELEMENTS(STATE_OPTS));
    fdd_init(p, &p->f_startup, STARTUP_OPTS, G_N_ELEMENTS(STARTUP_OPTS));
    fdd_init(p, &p->f_scope, SCOPE_OPTS, G_N_ELEMENTS(SCOPE_OPTS));

    const char *const sorts[] = { "Name", "State", "Startup state", "CPU usage", "Memory usage", "PID", NULL };
    const char *const groups[] = { "None", "Active state", "Startup state", "Scope", NULL };
    GtkWidget *sd = gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(sorts)), NULL);
    GtkWidget *gd = gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(groups)), NULL);
    g_signal_connect(sd, "notify::selected", G_CALLBACK(on_sort_selected), p);
    g_signal_connect(gd, "notify::selected", G_CALLBACK(on_group_selected), p);

    gtk_flow_box_append(GTK_FLOW_BOX(fb), captioned("State", GTK_WIDGET(p->f_state.dd)));
    gtk_flow_box_append(GTK_FLOW_BOX(fb), captioned("Startup", GTK_WIDGET(p->f_startup.dd)));
    gtk_flow_box_append(GTK_FLOW_BOX(fb), captioned("Scope", GTK_WIDGET(p->f_scope.dd)));
    gtk_flow_box_append(GTK_FLOW_BOX(fb), captioned("Sort by", sd));
    gtk_flow_box_append(GTK_FLOW_BOX(fb), captioned("Group by", gd));
    return fb;
}

static void on_back_clicked(Page *p)
{
    adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(p->split), FALSE);
}

static void on_list_activate(GtkListView *lv, guint pos, gpointer data)
{
    (void)lv;
    Page *p = data;
    SvcItem *i = g_list_model_get_item(G_LIST_MODEL(p->sel), pos);
    if (i) {
        services_select(p, i);
        g_object_unref(i);
    }
    adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(p->split), TRUE);
}

static void on_selection_changed(GtkSelectionModel *m, guint pos, guint n, gpointer data)
{
    (void)m; (void)pos; (void)n;
    Page *p = data;
    if (p->syncing)
        return;
    SvcItem *i = gtk_single_selection_get_selected_item(p->sel);
    if (i)
        services_select(p, i);
}

static GtkWidget *services_create_list(Page *p)
{
    p->store = g_list_store_new(SVC_TYPE_ITEM);
    p->filter = gtk_custom_filter_new(match_func, p, NULL);
    p->col_sorter = gtk_custom_sorter_new(col_compare, p, NULL);
    p->group_sorter = gtk_custom_sorter_new(group_compare, p, NULL);

    p->fmodel = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(p->store)),
                                          GTK_FILTER(g_object_ref(p->filter)));
    GtkMultiSorter *ms = gtk_multi_sorter_new();
    gtk_multi_sorter_append(ms, GTK_SORTER(g_object_ref(p->group_sorter)));
    gtk_multi_sorter_append(ms, GTK_SORTER(g_object_ref(p->col_sorter)));
    p->smodel = gtk_sort_list_model_new(G_LIST_MODEL(g_object_ref(p->fmodel)), GTK_SORTER(ms));
    gtk_sort_list_model_set_section_sorter(p->smodel, GTK_SORTER(p->group_sorter));
    p->sel = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(p->smodel)));
    gtk_single_selection_set_autoselect(p->sel, FALSE);
    gtk_single_selection_set_can_unselect(p->sel, TRUE);
    gtk_single_selection_set_selected(p->sel, GTK_INVALID_LIST_POSITION);

    GtkListItemFactory *f = gtk_signal_list_item_factory_new();
    g_signal_connect(f, "setup", G_CALLBACK(row_setup), p);
    g_signal_connect(f, "bind", G_CALLBACK(row_bind), p);
    g_signal_connect(f, "unbind", G_CALLBACK(row_unbind), p);
    GtkListItemFactory *hf = gtk_signal_list_item_factory_new();
    g_signal_connect(hf, "setup", G_CALLBACK(header_setup), p);
    g_signal_connect(hf, "bind", G_CALLBACK(header_bind), p);

    p->list_view = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(p->sel)), f);
    gtk_list_view_set_header_factory(GTK_LIST_VIEW(p->list_view), hf);
    g_object_unref(hf);
    gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(p->list_view), TRUE);
    gtk_widget_add_css_class(p->list_view, "navigation-sidebar");
    g_signal_connect(p->list_view, "activate", G_CALLBACK(on_list_activate), p);
    g_signal_connect(p->sel, "selection-changed", G_CALLBACK(on_selection_changed), p);

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), p->list_view);
    gtk_widget_set_vexpand(sw, TRUE);
    return sw;
}

static AdwActionRow *mk_info_row(const char *label, gboolean mono)
{
    AdwActionRow *r = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), label);
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(r), FALSE);
    adw_action_row_set_subtitle_selectable(r, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(r), "property");
    if (mono)
        gtk_widget_add_css_class(GTK_WIDGET(r), "svc-mono");
    gtk_widget_set_visible(GTK_WIDGET(r), FALSE);
    return r;
}

static GtkWidget *mk_action_button(Page *p, Action a, const char *css)
{
    GtkWidget *b = gtk_button_new_with_label(ACT[a].label);
    if (css) gtk_widget_add_css_class(b, css);
    g_object_set_data(G_OBJECT(b), "page", p);
    g_signal_connect(b, "clicked", G_CALLBACK(on_action_clicked), GINT_TO_POINTER(a));
    p->btn[a] = b;
    return b;
}

static GtkWidget *services_create_actions(Page *p)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *ord = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    p->disruptive_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    gtk_box_append(GTK_BOX(ord), mk_action_button(p, ACT_START, "suggested-action"));
    gtk_box_append(GTK_BOX(ord), mk_action_button(p, ACT_RELOAD, NULL));
    gtk_box_append(GTK_BOX(ord), mk_action_button(p, ACT_ENABLE, NULL));
    gtk_box_append(GTK_BOX(ord), mk_action_button(p, ACT_DISABLE, NULL));
    gtk_box_append(GTK_BOX(ord), mk_action_button(p, ACT_UNMASK, NULL));
    p->action_spinner = gtk_spinner_new();
    gtk_widget_set_visible(p->action_spinner, FALSE);
    gtk_box_append(GTK_BOX(ord), p->action_spinner);

    gtk_box_append(GTK_BOX(p->disruptive_box), mk_label("Disruptive", "caption", "dim-label"));
    gtk_box_append(GTK_BOX(p->disruptive_box), mk_action_button(p, ACT_STOP, "destructive-action"));
    gtk_box_append(GTK_BOX(p->disruptive_box), mk_action_button(p, ACT_MASK, "destructive-action"));
    /* ACT_RESTART has no dedicated button: the primary button doubles as it */
    p->btn[ACT_RESTART] = gtk_button_new();
    g_object_ref_sink(p->btn[ACT_RESTART]);

    GtkWidget *wrap = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(wrap), GTK_SELECTION_NONE);
    gtk_flow_box_append(GTK_FLOW_BOX(wrap), ord);
    gtk_box_append(GTK_BOX(box), wrap);
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box), p->disruptive_box);
    return box;
}

static GtkWidget *services_create_logs_section(Page *p)
{
    p->logs_exp = ADW_EXPANDER_ROW(adw_expander_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->logs_exp), "Logs");
    adw_expander_row_set_subtitle(p->logs_exp, "Recent journal entries for this unit");

    p->log_buf = gtk_text_buffer_new(NULL);
    gtk_text_buffer_create_tag(p->log_buf, "ts", "foreground", "gray", "scale", 0.92, NULL);
    gtk_text_buffer_create_tag(p->log_buf, "err", "weight", PANGO_WEIGHT_BOLD, NULL);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(p->log_buf, &end);
    gtk_text_buffer_create_mark(p->log_buf, "end", &end, FALSE);

    p->log_view = gtk_text_view_new_with_buffer(p->log_buf);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(p->log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(p->log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(p->log_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(p->log_view), GTK_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(p->log_view, "svc-log");

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), p->log_view);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 240);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), 420);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    p->log_older = gtk_button_new_with_label("Load older entries");
    GtkWidget *copy = gtk_button_new_with_label("Copy all");
    p->log_spinner = gtk_spinner_new();
    gtk_widget_set_visible(p->log_spinner, FALSE);
    p->log_status = mk_label("", "caption", "dim-label");
    gtk_label_set_wrap(GTK_LABEL(p->log_status), TRUE);
    gtk_widget_set_hexpand(p->log_status, TRUE);
    gtk_widget_set_sensitive(p->log_older, FALSE);
    gtk_box_append(GTK_BOX(bar), p->log_older);
    gtk_box_append(GTK_BOX(bar), copy);
    gtk_box_append(GTK_BOX(bar), p->log_spinner);
    gtk_box_append(GTK_BOX(bar), p->log_status);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
    gtk_widget_set_margin_start(bar, 12);
    gtk_widget_set_margin_end(bar, 12);

    adw_expander_row_add_row(p->logs_exp, sw);
    adw_expander_row_add_row(p->logs_exp, bar);
    g_signal_connect(p->log_older, "clicked", G_CALLBACK(on_logs_older), p);
    g_signal_connect(copy, "clicked", G_CALLBACK(on_logs_copy), p);
    g_signal_connect(p->logs_exp, "notify::expanded", G_CALLBACK(on_logs_expanded), p);
    return GTK_WIDGET(p->logs_exp);
}

static GtkWidget *services_create_dependency_section(Page *p)
{
    AdwExpanderRow *top = ADW_EXPANDER_ROW(adw_expander_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(top), "Dependencies");
    adw_expander_row_set_subtitle(top, "Requires, wants, ordering and conflicts");
    for (int k = 0; k < DEP_COUNT; k++) {
        p->dep_exp[k] = ADW_EXPANDER_ROW(adw_expander_row_new());
        p->dep_rows[k] = g_ptr_array_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->dep_exp[k]), DEP_TITLE[k]);
        adw_expander_row_add_row(top, GTK_WIDGET(p->dep_exp[k]));
    }
    return GTK_WIDGET(top);
}

static GtkWidget *services_create_details(Page *p)
{
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *chips = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    AdwPreferencesGroup *groups[4];
    static const char *const gtitle[3] = { "Status", "Execution", "Resources" };

    gtk_widget_set_margin_top(content, 18);
    gtk_widget_set_margin_bottom(content, 24);
    gtk_widget_set_margin_start(content, 14);
    gtk_widget_set_margin_end(content, 14);

    p->d_title = mk_label("", "title-2", NULL);
    gtk_label_set_wrap(GTK_LABEL(p->d_title), TRUE);
    gtk_label_set_selectable(GTK_LABEL(p->d_title), TRUE);
    p->d_desc = mk_label("", "dim-label", NULL);
    gtk_label_set_wrap(GTK_LABEL(p->d_desc), TRUE);
    p->d_chip = chip_new();
    p->d_startup = mk_label("", "dim-label", NULL);
    p->d_scope = mk_label("", "dim-label", NULL);
    gtk_widget_set_halign(p->d_chip.box, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(chips), p->d_chip.box);
    gtk_box_append(GTK_BOX(chips), p->d_startup);
    gtk_box_append(GTK_BOX(chips), p->d_scope);
    gtk_box_append(GTK_BOX(head), p->d_title);
    gtk_box_append(GTK_BOX(head), p->d_desc);
    gtk_box_append(GTK_BOX(head), chips);
    gtk_box_append(GTK_BOX(head), services_create_actions(p));
    gtk_box_append(GTK_BOX(content), head);

    for (int g = 0; g < 4; g++) {
        groups[g] = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        if (g < 3)
            adw_preferences_group_set_title(groups[g], gtitle[g]);
    }
    AdwExpanderRow *unit = ADW_EXPANDER_ROW(adw_expander_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(unit), "Unit files");
    adw_expander_row_set_subtitle(unit, "Fragment path and drop-in overrides");

    for (guint k = 0; k < G_N_ELEMENTS(ROW_DEFS); k++) {
        AdwActionRow *r = mk_info_row(ROW_DEFS[k].label, ROW_DEFS[k].mono);
        p->rows[ROW_DEFS[k].id] = r;
        if (ROW_DEFS[k].group < 3) adw_preferences_group_add(groups[ROW_DEFS[k].group], GTK_WIDGET(r));
        else                       adw_expander_row_add_row(unit, GTK_WIDGET(r));
    }
    adw_preferences_group_set_title(groups[3], "Advanced");
    adw_preferences_group_add(groups[3], GTK_WIDGET(unit));
    adw_preferences_group_add(groups[3], services_create_dependency_section(p));
    adw_preferences_group_add(groups[3], services_create_logs_section(p));

    for (int g = 0; g < 4; g++)
        gtk_box_append(GTK_BOX(content), GTK_WIDGET(groups[g]));

    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 760);
    adw_clamp_set_child(ADW_CLAMP(clamp), content);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), clamp);

    GtkWidget *empty = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "system-run-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(empty), "No Service Selected");
    adw_status_page_set_description(ADW_STATUS_PAGE(empty), "Select a service to see its state, resources and actions.");

    p->detail_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(p->detail_stack), empty, "empty");
    gtk_stack_add_named(GTK_STACK(p->detail_stack), sw, "details");
    gtk_stack_set_visible_child_name(GTK_STACK(p->detail_stack), "empty");
    return p->detail_stack;
}

/* ------------------------------------------------------------------ */
/* Page lifetime                                                       */
/* ------------------------------------------------------------------ */

static void page_free(gpointer data)
{
    Page *p = data;

    g_cancellable_cancel(p->cancel);
    if (p->view_id) g_source_remove(p->view_id);
    if (p->sample_id) g_source_remove(p->sample_id);
    for (int s = 0; s < SCOPE_COUNT; s++) {
        ScopeCtx *sc = &p->scopes[s];
        if (sc->discover_id) g_source_remove(sc->discover_id);
        for (guint k = 0; k < sc->n_subs; k++)
            g_dbus_connection_signal_unsubscribe(sc->conn, sc->subs[k]);
        g_clear_object(&sc->conn);
    }
    if (p->selected) {
        g_signal_handler_disconnect(p->selected, p->sel_handler);
        g_clear_object(&p->selected);
    }
    for (int k = 0; k < DEP_COUNT; k++)
        if (p->dep_rows[k]) g_ptr_array_free(p->dep_rows[k], TRUE);
    if (p->btn[ACT_RESTART]) g_object_unref(p->btn[ACT_RESTART]);
    g_array_free(p->f_state.ids, TRUE);
    g_array_free(p->f_startup.ids, TRUE);
    g_array_free(p->f_scope.ids, TRUE);
    g_strfreev(p->tokens);
    g_free(p->log_cursor);
    g_clear_object(&p->sel);
    g_clear_object(&p->smodel);
    g_clear_object(&p->fmodel);
    g_clear_object(&p->store);
    g_clear_object(&p->filter);
    g_clear_object(&p->col_sorter);
    g_clear_object(&p->group_sorter);
    g_clear_object(&p->log_buf);
    g_hash_table_unref(p->items);
    g_object_unref(p->cancel);
    g_free(p);
}

GtkWidget *services_page_create(App *app)
{
    install_css();

    Page *p = g_new0(Page, 1);
    p->app = app;
    p->cancel = g_cancellable_new();
    p->items = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    for (int s = 0; s < SCOPE_COUNT; s++) {
        p->scopes[s].page = p;
        p->scopes[s].scope = s;
    }

    GtkWidget *search;
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(top, 14);
    gtk_widget_set_margin_bottom(top, 8);
    gtk_widget_set_margin_start(top, 14);
    gtk_widget_set_margin_end(top, 14);
    gtk_box_append(GTK_BOX(top), services_create_header(p, &search));
    gtk_box_append(GTK_BOX(top), services_create_filters(p));
    gtk_box_append(GTK_BOX(top), services_create_summary(p));
    g_signal_connect(search, "search-changed", G_CALLBACK(on_search_changed), p);

    /* list pane */
    GtkWidget *list = services_create_list(p);
    AdwNavigationPage *side = adw_navigation_page_new(list, "Services");

    /* detail pane with an explicit back button shown only when collapsed */
    GtkWidget *dbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *back = gtk_button_new_with_label("Services");
    gtk_button_set_icon_name(GTK_BUTTON(back), "go-previous-symbolic");
    gtk_widget_add_css_class(back, "flat");
    gtk_widget_set_halign(back, GTK_ALIGN_START);
    gtk_widget_set_margin_start(back, 8);
    gtk_widget_set_margin_top(back, 4);
    GtkWidget *details = services_create_details(p);
    gtk_widget_set_vexpand(details, TRUE);
    gtk_box_append(GTK_BOX(dbox), back);
    gtk_box_append(GTK_BOX(dbox), details);
    AdwNavigationPage *main = adw_navigation_page_new(dbox, "Service");

    p->split = adw_navigation_split_view_new();
    AdwNavigationSplitView *sv = ADW_NAVIGATION_SPLIT_VIEW(p->split);
    adw_navigation_split_view_set_sidebar(sv, side);
    adw_navigation_split_view_set_content(sv, main);
    adw_navigation_split_view_set_min_sidebar_width(sv, 340);
    adw_navigation_split_view_set_max_sidebar_width(sv, 520);
    adw_navigation_split_view_set_sidebar_width_fraction(sv, 0.42);
    g_object_bind_property(p->split, "collapsed", back, "visible", G_BINDING_SYNC_CREATE);
    g_signal_connect_swapped(back, "clicked", G_CALLBACK(on_back_clicked), p);

    /* the split view collapses based on this page's own width */
    GtkWidget *bin = adw_breakpoint_bin_new();
    gtk_widget_set_size_request(bin, 360, 360);
    adw_breakpoint_bin_set_child(ADW_BREAKPOINT_BIN(bin), p->split);
    AdwBreakpoint *bp = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 820sp"));
    adw_breakpoint_add_setters(bp, G_OBJECT(p->split), "collapsed", TRUE, NULL);
    adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(bin), bp);
    gtk_widget_set_vexpand(bin, TRUE);

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(page), top);
    gtk_box_append(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(page), bin);

    p->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(p->toasts, page);
    p->root = GTK_WIDGET(p->toasts);
    g_object_set_data_full(G_OBJECT(p->root), "services-page", p, page_free);

    update_summary(p);
    rebuild_filter_options(p);

    /* connect to systemd (system + per-user manager) */
    g_bus_get(G_BUS_TYPE_SYSTEM, p->cancel, on_bus_ready, &p->scopes[SCOPE_SYSTEM]);
    g_bus_get(G_BUS_TYPE_SESSION, p->cancel, on_bus_ready, &p->scopes[SCOPE_USER]);
    p->sample_id = g_timeout_add_seconds(SAMPLE_INTERVAL_S, sample_cb, p);

    return p->root;
}
