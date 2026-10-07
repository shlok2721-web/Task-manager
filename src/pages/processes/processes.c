/*
 * processes.c - "Processes" page for the Linux task manager.
 *
 * Requires: GTK 4.12+, libadwaita 1.4+, GLib 2.74+. Linux only.
 *
 * Data flow:
 *   shared process backend  performs the single /proc scan for the application.
 *   main thread             merges the borrowed ProcessInfo view into a GListStore.
 *                           Items are updated in place; only changed rows repaint.
 *   GtkFilterListModel      search + category/state filter (one shared GtkFilter).
 *   GtkSortListModel        column-view sorter (GtkTreeListRowSorter in tree mode).
 *   GtkColumnView           rows. Each bound cell listens to its item's "stats-changed".
 *
 * Identity: every process is keyed by (pid, start time in clock ticks). A reused
 * PID has a different start time, so it never inherits another process's history.
 */

#include "processes.h"

#include <adwaita.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SEARCH_DEBOUNCE_MS  150
#define SYSTEM_UID_MAX      999
#define STATE_KEY           "processes-page-state"
#define DASH                "\xe2\x80\x94" /* em dash: value not available */

/* ------------------------------------------------------------------------- */
/* Types and constants                                                       */
/* ------------------------------------------------------------------------- */

typedef enum {
  COL_PROCESS, COL_PID, COL_USER, COL_CPU, COL_MEMORY, COL_DISK_READ,
  COL_DISK_WRITE, COL_NETWORK, COL_THREADS, COL_STATUS, COL_START, COL_COUNT
} ColumnId;

typedef enum {
  FILTER_ALL, FILTER_USER, FILTER_SYSTEM, FILTER_RUNNING,
  FILTER_SLEEPING, FILTER_STOPPED, FILTER_OTHER
} FilterMode;

typedef enum {
  DETAIL_NAME, DETAIL_PID, DETAIL_PPID, DETAIL_USER, DETAIL_EXE, DETAIL_CMDLINE,
  DETAIL_STATE, DETAIL_STARTED, DETAIL_UPTIME, DETAIL_PRIORITY, DETAIL_NICE,
  DETAIL_AFFINITY, DETAIL_LAST_CPU, DETAIL_CPU_USAGE, DETAIL_CPU_TIME,
  DETAIL_RESIDENT, DETAIL_VIRTUAL, DETAIL_SHARED, DETAIL_DISK_IO,
  DETAIL_OPEN_FILES, DETAIL_THREADS, DETAIL_CGROUP, DETAIL_COUNT
} DetailId;

typedef struct { const char *group; const char *title; } DetailDef;

static const DetailDef detail_defs[DETAIL_COUNT] = {
  [DETAIL_NAME]       = { "Identity",  "Name" },
  [DETAIL_PID]        = { NULL,        "Process ID" },
  [DETAIL_PPID]       = { NULL,        "Parent PID" },
  [DETAIL_USER]       = { NULL,        "User" },
  [DETAIL_EXE]        = { NULL,        "Executable" },
  [DETAIL_CMDLINE]    = { NULL,        "Command line" },
  [DETAIL_STATE]      = { "State",     "Current state" },
  [DETAIL_STARTED]    = { NULL,        "Start time" },
  [DETAIL_UPTIME]     = { NULL,        "Uptime" },
  [DETAIL_PRIORITY]   = { NULL,        "Priority" },
  [DETAIL_NICE]       = { NULL,        "Nice value" },
  [DETAIL_AFFINITY]   = { NULL,        "CPU affinity" },
  [DETAIL_LAST_CPU]   = { NULL,        "Last CPU core" },
  [DETAIL_CPU_USAGE]  = { "Resources", "CPU usage" },
  [DETAIL_CPU_TIME]   = { NULL,        "CPU time" },
  [DETAIL_RESIDENT]   = { NULL,        "Resident memory" },
  [DETAIL_VIRTUAL]    = { NULL,        "Virtual memory" },
  [DETAIL_SHARED]     = { NULL,        "Shared memory" },
  [DETAIL_DISK_IO]    = { NULL,        "Disk I/O" },
  [DETAIL_OPEN_FILES] = { NULL,        "Open files" },
  [DETAIL_THREADS]    = { NULL,        "Threads" },
  [DETAIL_CGROUP]     = { "System",    "Cgroup" },
};

static const char *const column_titles[COL_COUNT + 1] = {
  "Process", "PID", "User", "CPU", "Memory", "Disk read", "Disk write",
  "Network", "Threads", "Status", "Start time", NULL
};

static const int column_min_width[COL_COUNT] = {
  220, 56, 90, 64, 88, 96, 96, 96, 64, 88, 116
};

static const char *const filter_labels[] = {
  "All processes", "User processes", "System processes", "Running",
  "Sleeping", "Stopped", "Other states", NULL
};

typedef struct {
  const char *label;
  int sig;
  gboolean confirm;
} ActionDef;

static const ActionDef process_actions[] = {
  { "Terminate", SIGTERM, TRUE  },
  { "Kill",      SIGKILL, TRUE  },
  { "Stop",      SIGSTOP, TRUE  },
  { "Continue",  SIGCONT, FALSE },
};

/* Identity of one process incarnation. */
typedef struct {
  pid_t pid;
  guint64 start;          /* starttime in clock ticks since boot */
} ProcKey;

/* The shared backend owns all live process samples.  This page only keeps
 * presentation state and copies the fields it needs into ProcItem objects. */

/* ------------------------------------------------------------------------- */
/* Process item: a GObject row in the model                                  */
/* ------------------------------------------------------------------------- */

G_DECLARE_FINAL_TYPE(ProcItem, proc_item, PROC, ITEM, GObject)

struct _ProcItem {
  GObject parent_instance;
  ProcKey key;
  pid_t ppid;
  guint uid;
  char state;
  int nice, priority, threads, last_cpu, fd_count;
  gboolean io_ok, parent_known;
  guint generation;
  guint64 cpu_ticks, rss, vsize, shared, read_bytes, write_bytes, start_epoch;
  double cpu_pct;
  double read_rate, write_rate;   /* bytes/s; -1 means unavailable */
  char *name, *cmdline, *exe, *user, *cgroup, *affinity, *search_blob;
};

G_DEFINE_FINAL_TYPE(ProcItem, proc_item, G_TYPE_OBJECT)

enum { STATS_CHANGED, N_ITEM_SIGNALS };
static guint item_signals[N_ITEM_SIGNALS];

static void
proc_item_finalize(GObject *obj)
{
  ProcItem *it = PROC_ITEM(obj);
  g_free(it->name);
  g_free(it->cmdline);
  g_free(it->exe);
  g_free(it->user);
  g_free(it->cgroup);
  g_free(it->affinity);
  g_free(it->search_blob);
  G_OBJECT_CLASS(proc_item_parent_class)->finalize(obj);
}

static void
proc_item_class_init(ProcItemClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = proc_item_finalize;
  item_signals[STATS_CHANGED] = g_signal_new("stats-changed",
                                             G_TYPE_FROM_CLASS(klass),
                                             G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                             NULL, G_TYPE_NONE, 0);
}

static void
proc_item_init(ProcItem *it)
{
  it->fd_count = -1;
  it->read_rate = -1.0;
  it->write_rate = -1.0;
}

static ProcItem *
proc_item_new(void)
{
  return PROC_ITEM(g_object_new(proc_item_get_type(), NULL));
}

/* ------------------------------------------------------------------------- */
/* Page state (private to this module)                                       */
/* ------------------------------------------------------------------------- */

#define N_ACTION_WIDGETS 5

typedef struct {
  App *app;                               /* borrowed application shell */
  GtkWidget *page;                       /* root widget, not owned */
  GtkWidget *view;
  GtkColumnViewColumn *columns[COL_COUNT];
  GtkWidget *status_label;
  GtkWidget *search;
  GtkWidget *sort_dd;
  GtkWidget *sort_dir_btn;
  GtkWidget *split;
  GtkWidget *details_stack;
  GtkWidget *rows[DETAIL_COUNT];
  GtkWidget *action_status;
  GtkWidget *action_widgets[N_ACTION_WIDGETS];
  guint n_action_widgets;
  GtkWidget *priority_spin;
  GtkWidget *priority_popover;
  GtkSingleSelection *sel;               /* strong ref, owned by the page */

  GListStore *store;                     /* all ProcItem, unfiltered */
  GHashTable *items;                     /* ProcKey * -> ProcItem * (non-owning) */
  GtkFilter *match_filter;               /* search + category, shared */
  GtkFilter *root_filter;                /* tree roots: parent unknown */
  GCancellable *cancellable;

  ProcItem *selected;                    /* strong ref */
  gboolean selected_exited;
  gboolean tree_mode;
  gboolean sort_desc;
  FilterMode filter_mode;
  char *search_text;                     /* lowercase; NULL when empty */
  guint generation;
  long clk_tck;
  gulong backend_handler;
  guint debounce_source;
} PageState;

typedef struct {
  GtkWidget *page;
  GtkAlertDialog *dialog;
  ProcKey key;
  int sig;
  const char *label;
} ConfirmCtx;

typedef struct {
  GtkWidget *page;
  GSubprocess *proc;
  char *desc;
} PrivCtx;

static void start_refresh(PageState *s);
static void processes_refresh_model(PageState *s);
static void processes_select(GtkSingleSelection *sel, GParamSpec *p, gpointer data);
static void processes_sort(GtkDropDown *dd, GParamSpec *p, gpointer data);
static void processes_filter(GtkDropDown *dd, GParamSpec *p, gpointer data);
static void update_details(PageState *s);
static void set_status(PageState *s, const char *text);
static void apply_sort(PageState *s, guint col);

/* ------------------------------------------------------------------------- */
/* Formatting                                                                */
/* ------------------------------------------------------------------------- */

static char *
fmt_size(double bytes)
{
  static const char *const units[] = { "B", "KB", "MB", "GB", "TB" };
  guint i = 0;

  while (bytes >= 1024.0 && i < G_N_ELEMENTS(units) - 1) {
    bytes /= 1024.0;
    i++;
  }
  if (i == 0)
    return g_strdup_printf("%.0f B", bytes);
  if (bytes < 10.0)
    return g_strdup_printf("%.2f %s", bytes, units[i]);
  if (bytes < 100.0)
    return g_strdup_printf("%.1f %s", bytes, units[i]);
  return g_strdup_printf("%.0f %s", bytes, units[i]);
}

static char *
fmt_rate(double bytes_per_s)
{
  if (bytes_per_s < 0.0)
    return g_strdup(DASH);
  g_autofree char *size = fmt_size(bytes_per_s);
  return g_strconcat(size, "/s", NULL);
}

static char *
fmt_percent(double pct)
{
  return g_strdup_printf("%.1f%%", pct);
}

static char *
fmt_duration(gint64 secs)
{
  int d, h, m, s;

  if (secs < 0)
    secs = 0;
  d = (int)(secs / 86400);
  h = (int)(secs / 3600 % 24);
  m = (int)(secs / 60 % 60);
  s = (int)(secs % 60);
  if (d > 0)
    return g_strdup_printf("%dd %02dh %02dm", d, h, m);
  return g_strdup_printf("%02d:%02d:%02d", h, m, s);
}

static char *
fmt_time(guint64 epoch, const char *format)
{
  GDateTime *dt;
  char *out;

  if (epoch == 0)
    return g_strdup(DASH);
  dt = g_date_time_new_from_unix_local((gint64)epoch);
  if (!dt)
    return g_strdup(DASH);
  out = g_date_time_format(dt, format);
  g_date_time_unref(dt);
  return out;
}

static const char *
state_name(char c)
{
  switch (c) {
  case 'R': return "Running";
  case 'S': return "Sleeping";
  case 'D': return "Disk sleep";
  case 'I': return "Idle";
  case 'T': return "Stopped";
  case 't': return "Tracing stop";
  case 'Z': return "Zombie";
  case 'X':
  case 'x': return "Dead";
  case 'W': return "Waking";
  case 'P': return "Parked";
  default:  return "Other";
  }
}

/* ------------------------------------------------------------------------- */
/* Shared process backend adapter                                            */

static gboolean
process_identity_matches(PageState *s, ProcKey key)
{
  GPtrArray *processes = app_get_processes(s->app);
  if (!processes)
    return FALSE;

  for (guint i = 0; i < processes->len; ++i) {
    const ProcessInfo *info = g_ptr_array_index(processes, i);
    if (info && info->pid == key.pid && info->start_ticks == key.start)
      return TRUE;
  }
  return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Model: merge snapshots into ProcItem objects                              */
/* ------------------------------------------------------------------------- */

static gboolean
set_str(char **dst, const char *src)
{
  if (g_strcmp0(*dst, src) == 0)
    return FALSE;
  g_free(*dst);
  *dst = g_strdup(src);
  return TRUE;
}

static void
rebuild_search_blob(ProcItem *it)
{
  g_autofree char *raw = g_strdup_printf("%d %s %s %s %s",
                                         it->key.pid,
                                         it->name ? it->name : "",
                                         it->user ? it->user : "",
                                         it->exe ? it->exe : "",
                                         it->cmdline ? it->cmdline : "");
  g_free(it->search_blob);
  it->search_blob = g_ascii_strdown(raw, -1);
}

static void
item_update(ProcItem *it, const ProcessInfo *info)
{
  gboolean text = FALSE;

  text |= set_str(&it->name, info->name);
  text |= set_str(&it->cmdline, info->cmdline);
  text |= set_str(&it->exe, info->exe);
  text |= set_str(&it->user, info->user);
  text |= set_str(&it->cgroup, info->cgroup);
  text |= set_str(&it->affinity, info->affinity);
  if (text || !it->search_blob)
    rebuild_search_blob(it);

  it->ppid = info->ppid;
  it->uid = info->uid;
  it->state = info->state;
  it->nice = info->nice;
  it->priority = info->priority;
  it->threads = info->threads;
  it->last_cpu = info->last_cpu;
  it->fd_count = info->fd_count;
  it->rss = info->rss_bytes;
  it->vsize = info->vsize_bytes;
  it->shared = info->shared_bytes;
  it->cpu_ticks = info->cpu_ticks;
  it->cpu_pct = info->cpu_percent;
  it->read_bytes = info->read_bytes;
  it->write_bytes = info->write_bytes;
  it->read_rate = info->read_rate;
  it->write_rate = info->write_rate;
  it->io_ok = info->io_ok;
  it->start_epoch = info->start_time_us > 0
    ? (guint64)(info->start_time_us / G_USEC_PER_SEC) : 0;
}

static guint
key_hash(gconstpointer p)
{
  const ProcKey *k = p;
  return (guint)k->pid * 2654435761u ^ (guint)(k->start ^ (k->start >> 32));
}

static gboolean
key_equal(gconstpointer a, gconstpointer b)
{
  const ProcKey *x = a, *y = b;
  return x->pid == y->pid && x->start == y->start;
}

/*
 * A process is a tree root when its parent is not in the list. The child
 * must also have started no earlier than the parent, which protects against
 * a reused PID being treated as the parent.
 */
static void
update_parent_links(PageState *s)
{
  GHashTable *by_pid = g_hash_table_new(g_direct_hash, g_direct_equal);
  GHashTableIter iter;
  gpointer v;
  gboolean changed = FALSE;

  g_hash_table_iter_init(&iter, s->items);
  while (g_hash_table_iter_next(&iter, NULL, &v)) {
    ProcItem *it = v;
    ProcItem *prev = g_hash_table_lookup(by_pid, GINT_TO_POINTER(it->key.pid));
    if (!prev || prev->key.start < it->key.start)
      g_hash_table_insert(by_pid, GINT_TO_POINTER(it->key.pid), it);
  }

  g_hash_table_iter_init(&iter, s->items);
  while (g_hash_table_iter_next(&iter, NULL, &v)) {
    ProcItem *it = v;
    ProcItem *parent = g_hash_table_lookup(by_pid, GINT_TO_POINTER(it->ppid));
    gboolean known = parent && parent != it && parent->key.start <= it->key.start;

    if (known != it->parent_known) {
      it->parent_known = known;
      changed = TRUE;
    }
  }
  g_hash_table_destroy(by_pid);

  if (changed)
    gtk_filter_changed(s->root_filter, GTK_FILTER_CHANGE_DIFFERENT);
}

static void
processes_update(PageState *s, GPtrArray *processes)
{
  guint n;
  GPtrArray *added = g_ptr_array_new();

  if (!processes) {
    set_status(s, "Could not read the process list.");
    g_ptr_array_free(added, TRUE);
    return;
  }

  s->generation++;

  /* 1. Update existing items in place; collect new ones. */
  for (guint i = 0; i < processes->len; ++i) {
    const ProcessInfo *info = g_ptr_array_index(processes, i);
    if (!info)
      continue;

    ProcKey key = { info->pid, info->start_ticks };
    ProcItem *it = g_hash_table_lookup(s->items, &key);
    gboolean fresh = (it == NULL);

    if (fresh) {
      it = proc_item_new();
      it->key = key;
      g_hash_table_insert(s->items, &it->key, it);
      g_ptr_array_add(added, it);
    } else if (it->generation == s->generation) {
      continue;
    }

    it->generation = s->generation;
    item_update(it, info);
    if (!fresh)
      g_signal_emit(it, item_signals[STATS_CHANGED], 0);
  }

  /* 2. Remove exited processes. Walk backwards so indices stay valid. */
  n = g_list_model_get_n_items(G_LIST_MODEL(s->store));
  for (guint i = n; i-- > 0;) {
    ProcItem *it = g_list_model_get_item(G_LIST_MODEL(s->store), i);

    if (it->generation != s->generation) {
      if (it == s->selected) {
        g_clear_object(&s->selected);
        s->selected_exited = TRUE;
      }
      g_hash_table_remove(s->items, &it->key);
      g_list_store_remove(s->store, i);
    }
    g_object_unref(it);
  }

  /* 3. Append new processes in one splice (one items-changed signal). */
  if (added->len > 0) {
    g_list_store_splice(s->store,
                        g_list_model_get_n_items(G_LIST_MODEL(s->store)),
                        0, added->pdata, added->len);
    for (guint i = 0; i < added->len; ++i)
      g_object_unref(g_ptr_array_index(added, i));
  }
  g_ptr_array_free(added, TRUE);

  update_parent_links(s);

  gtk_sorter_changed(gtk_column_view_get_sorter(GTK_COLUMN_VIEW(s->view)),
                     GTK_SORTER_CHANGE_DIFFERENT);

  {
    g_autofree char *now = fmt_time((guint64)(g_get_real_time() / G_USEC_PER_SEC),
                                    "%H:%M:%S");
    g_autofree char *text = g_strdup_printf(
      "Updated %s \xc2\xb7 %u processes \xc2\xb7 shared backend",
      now, g_list_model_get_n_items(G_LIST_MODEL(s->store)));
    gtk_label_set_text(GTK_LABEL(s->status_label), text);
  }

  update_details(s);
}

/* ------------------------------------------------------------------------- */
/* Filtering and sorting                                                     */
/* ------------------------------------------------------------------------- */

static gboolean
match_func(gpointer obj, gpointer data)
{
  PageState *s = data;
  const ProcItem *it = obj;

  if (s->search_text && !strstr(it->search_blob, s->search_text))
    return FALSE;

  switch (s->filter_mode) {
  case FILTER_USER:     return it->uid > SYSTEM_UID_MAX;
  case FILTER_SYSTEM:   return it->uid <= SYSTEM_UID_MAX;
  case FILTER_RUNNING:  return it->state == 'R';
  case FILTER_SLEEPING: return strchr("SDI", it->state) != NULL;
  case FILTER_STOPPED:  return it->state == 'T' || it->state == 't';
  case FILTER_OTHER:    return strchr("RSDITt", it->state) == NULL;
  case FILTER_ALL:
  default:              return TRUE;
  }
}

static gboolean
root_func(gpointer obj, gpointer data)
{
  (void)data;
  return !((const ProcItem *)obj)->parent_known;
}

static gboolean
child_func(gpointer obj, gpointer data)
{
  const ProcKey *parent = data;
  const ProcItem *it = obj;

  return it->ppid == parent->pid && it->key.start >= parent->start;
}

/* Children of a node are a live filter over the shared store. */
static GListModel *
tree_create_children(gpointer item, gpointer data)
{
    PageState *s = data;
    const ProcItem *parent = item;
    ProcKey *key = g_new(ProcKey, 1);
    GtkFilter *child;
    GtkMultiFilter *every;
    GtkFilterListModel *model;

    *key = parent->key;

    child = GTK_FILTER(
        gtk_custom_filter_new(
            child_func,
            key,
            g_free));

    every = GTK_MULTI_FILTER(
        gtk_every_filter_new());

    /*
     * gtk_multi_filter_append() takes ownership of the filters
     * appended to it.
     */
    gtk_multi_filter_append(
        every,
        g_object_ref(s->match_filter));

    gtk_multi_filter_append(
        every,
        child);

    /*
     * 'every' now owns child.
     * Do not unref child here.
     *
     * gtk_filter_list_model_new() takes ownership of both
     * the model and filter passed to it.
     *
     * s->store is persistent and must therefore be explicitly
     * referenced before ownership is transferred.
     */
    model = gtk_filter_list_model_new(
        G_LIST_MODEL(g_object_ref(s->store)),
        GTK_FILTER(every));

    /*
     * Do NOT unref 'every'.
     * Ownership was transferred to model.
     */

    return G_LIST_MODEL(model);
}

static GtkOrdering
cmp_u64(guint64 a, guint64 b)
{
  return a < b ? GTK_ORDERING_SMALLER : a > b ? GTK_ORDERING_LARGER : GTK_ORDERING_EQUAL;
}

static GtkOrdering
cmp_str(const char *a, const char *b)
{
  int r = g_strcmp0(a ? a : "", b ? b : "");
  return r < 0 ? GTK_ORDERING_SMALLER : r > 0 ? GTK_ORDERING_LARGER : GTK_ORDERING_EQUAL;
}

/* Column sorter. Ties fall back to PID, so equal rows never swap places. */
static GtkOrdering
compare_items(gconstpointer a, gconstpointer b, gpointer data)
{
  const ProcItem *x = a, *y = b;
  GtkOrdering o = GTK_ORDERING_EQUAL;

  switch (GPOINTER_TO_INT(data)) {
  case COL_PROCESS:    o = cmp_str(x->name, y->name); break;
  case COL_PID:        o = cmp_u64((guint64)x->key.pid, (guint64)y->key.pid); break;
  case COL_USER:       o = cmp_str(x->user, y->user); break;
  /* Compare CPU at 0.1% resolution so jitter below the display precision does not reorder rows. */
  case COL_CPU:        o = cmp_u64((guint64)(x->cpu_pct * 10.0), (guint64)(y->cpu_pct * 10.0)); break;
  case COL_MEMORY:     o = cmp_u64(x->rss, y->rss); break;
  case COL_DISK_READ:  o = cmp_u64((guint64)(x->read_rate + 1.0), (guint64)(y->read_rate + 1.0)); break;
  case COL_DISK_WRITE: o = cmp_u64((guint64)(x->write_rate + 1.0), (guint64)(y->write_rate + 1.0)); break;
  case COL_NETWORK:    break;            /* not exposed per process: all equal */
  case COL_THREADS:    o = cmp_u64((guint64)x->threads, (guint64)y->threads); break;
  case COL_STATUS:     o = cmp_u64((guchar)x->state, (guchar)y->state); break;
  case COL_START:      o = cmp_u64(x->key.start, y->key.start); break;
  default:             break;
  }
  if (o == GTK_ORDERING_EQUAL)
    o = cmp_u64((guint64)x->key.pid, (guint64)y->key.pid);
  return o;
}

static void
processes_filter(GtkDropDown *dd, GParamSpec *pspec, gpointer data)
{
  PageState *s = data;

  (void)pspec;
  s->filter_mode = (FilterMode)gtk_drop_down_get_selected(dd);
  gtk_filter_changed(s->match_filter, GTK_FILTER_CHANGE_DIFFERENT);
}

static gboolean
on_search_debounced(gpointer data)
{
  PageState *s = data;
  const char *text = gtk_editable_get_text(GTK_EDITABLE(s->search));

  s->debounce_source = 0;
  g_free(s->search_text);
  s->search_text = (text && *text) ? g_ascii_strdown(text, -1) : NULL;
  gtk_filter_changed(s->match_filter, GTK_FILTER_CHANGE_DIFFERENT);
  return G_SOURCE_REMOVE;
}

static void
on_search_changed(GtkSearchEntry *entry, gpointer data)
{
  PageState *s = data;

  (void)entry;
  g_clear_handle_id(&s->debounce_source, g_source_remove);
  s->debounce_source = g_timeout_add(SEARCH_DEBOUNCE_MS, on_search_debounced, s);
}

static void
apply_sort(PageState *s, guint col)
{
  if (!s->view || col >= COL_COUNT || !s->columns[col])
    return;
  gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(s->view), s->columns[col],
                                 s->sort_desc ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
}

static void
processes_sort(GtkDropDown *dd, GParamSpec *pspec, gpointer data)
{
  PageState *s = data;

  (void)pspec;
  apply_sort(s, gtk_drop_down_get_selected(dd));
}

static void
on_sort_direction(GtkButton *btn, gpointer data)
{
  PageState *s = data;

  s->sort_desc = !s->sort_desc;
  gtk_button_set_icon_name(btn, s->sort_desc ? "view-sort-descending-symbolic"
                                             : "view-sort-ascending-symbolic");
  apply_sort(s, gtk_drop_down_get_selected(GTK_DROP_DOWN(s->sort_dd)));
}

static void
on_tree_toggled(GtkToggleButton *btn, gpointer data)
{
  PageState *s = data;

  s->tree_mode = gtk_toggle_button_get_active(btn);
  processes_refresh_model(s);
}

/*
 * Rebuilds the view's model chain. Only used on mode switch, not on refresh.
 * Flat:  store -> filter -> sort -> selection
 * Tree:  store -> roots filter -> TreeListModel -> tree row sort -> selection
 *
 * Ownership (GTK 4):
 *   gtk_filter_list_model_new()      refs its model and filter
 *   gtk_multi_filter_append()        refs the appended filter
 *   gtk_tree_list_model_new()        TAKES OWNERSHIP of its root model
 *   gtk_sort_list_model_new()        refs its source and sorter
 *   gtk_single_selection_new()       refs its model
 *   gtk_column_view_set_model()      refs the selection model
 */
static void
processes_refresh_model(PageState *s)
{
    GtkSorter *column_sorter;
    GListModel *source = NULL;
    GtkSorter *sorter = NULL;
    GtkSortListModel *sorted;
    GtkSingleSelection *sel;

    if (!s->view)
        return;

    column_sorter =
        gtk_column_view_get_sorter(GTK_COLUMN_VIEW(s->view));

    if (s->tree_mode) {
        GtkMultiFilter *roots;
        GtkFilterListModel *root_model;
        GtkTreeListModel *tree_model;

        roots = GTK_MULTI_FILTER(
            gtk_every_filter_new());

        gtk_multi_filter_append(
            roots,
            g_object_ref(s->match_filter));

        gtk_multi_filter_append(
            roots,
            g_object_ref(s->root_filter));

        /*
         * gtk_filter_list_model_new() takes ownership of
         * both arguments.
         *
         * s->store and s->root_filter are persistent page
         * objects, so give the model its own references.
         */
        root_model = gtk_filter_list_model_new(
            G_LIST_MODEL(g_object_ref(s->store)),
            GTK_FILTER(g_object_ref(roots)));

        /*
         * gtk_tree_list_model_new() takes ownership of
         * root_model.
         */
        tree_model = gtk_tree_list_model_new(
            G_LIST_MODEL(root_model),
            FALSE,
            TRUE,
            tree_create_children,
            s,
            NULL);

        /*
         * tree_model is now owned by source.
         */
        source = G_LIST_MODEL(tree_model);

        /*
         * gtk_tree_list_row_sorter_new() takes ownership
         * of the child sorter.
         *
         * column_sorter belongs to GtkColumnView, so give
         * the row sorter its own reference.
         */
        if (column_sorter != NULL) {
            sorter = GTK_SORTER(
                gtk_tree_list_row_sorter_new(
                    g_object_ref(column_sorter)));
        }

        /*
         * roots was referenced explicitly for root_model
         * above. The root_model owns its filter reference.
         * Release our temporary reference.
         */
        g_object_unref(roots);

    } else {
        GtkFilterListModel *filter_model;

        /*
         * gtk_filter_list_model_new() takes ownership of
         * the model and filter.
         *
         * Both objects belong to PageState, so explicitly
         * reference them first.
         */
        filter_model = gtk_filter_list_model_new(
            G_LIST_MODEL(g_object_ref(s->store)),
            GTK_FILTER(g_object_ref(s->match_filter)));

        source = G_LIST_MODEL(filter_model);

        if (column_sorter != NULL) {
            sorter = GTK_SORTER(
                g_object_ref(column_sorter));
        }
    }

    /*
     * gtk_sort_list_model_new() takes ownership of source
     * and sorter.
     */
    sorted = gtk_sort_list_model_new(
        source,
        sorter);

    source = NULL;
    sorter = NULL;

    if (!sorted || !G_IS_LIST_MODEL(sorted)) {

        if (sorted)
            g_object_unref(sorted);

        return;
    }

    /*
     * gtk_single_selection_new() takes ownership of sorted.
     *
     * DO NOT unref sorted after this call.
     */
    sel = gtk_single_selection_new(
        G_LIST_MODEL(sorted));

    sorted = NULL;

    if (!sel) {
        return;
    }

    gtk_single_selection_set_autoselect(
        sel,
        FALSE);

    gtk_single_selection_set_can_unselect(
        sel,
        TRUE);

    g_signal_connect(
        sel,
        "notify::selected-item",
        G_CALLBACK(processes_select),
        s);

    /*
     * GtkColumnView retains its selection model.
     */
    gtk_column_view_set_model(
        GTK_COLUMN_VIEW(s->view),
        GTK_SELECTION_MODEL(sel));

    g_object_unref(sel);

    g_clear_object(&s->selected);
    s->selected_exited = FALSE;

    update_details(s);
}

/* ------------------------------------------------------------------------- */
/* Cells                                                                     */
/* ------------------------------------------------------------------------- */

static ProcItem *
item_from_obj(gpointer obj)
{
  if (GTK_IS_TREE_LIST_ROW(obj)) {
    /* get_item() returns a new ref; the row keeps the item alive, so drop it. */
    gpointer item = gtk_tree_list_row_get_item(GTK_TREE_LIST_ROW(obj));
    if (item)
      g_object_unref(item);
    obj = item;
  }
  return (obj && PROC_IS_ITEM(obj)) ? PROC_ITEM(obj) : NULL;
}

static gboolean
is_numeric_column(ColumnId col)
{
  return col == COL_PID || col == COL_CPU || col == COL_MEMORY ||
         col == COL_DISK_READ || col == COL_DISK_WRITE ||
         col == COL_NETWORK || col == COL_THREADS;
}

static void
set_label_text(GtkLabel *label, const char *text)
{
  if (g_strcmp0(gtk_label_get_text(label), text) != 0)
    gtk_label_set_text(label, text);
}

static char *
cell_text(ColumnId col, const ProcItem *it)
{
  switch (col) {
  case COL_PID:        return g_strdup_printf("%d", it->key.pid);
  case COL_USER:       return g_strdup(it->user ? it->user : DASH);
  case COL_CPU:        return fmt_percent(it->cpu_pct);
  case COL_MEMORY:     return fmt_size((double)it->rss);
  case COL_DISK_READ:  return fmt_rate(it->read_rate);
  case COL_DISK_WRITE: return fmt_rate(it->write_rate);
  case COL_THREADS:    return g_strdup_printf("%d", it->threads);
  case COL_STATUS:     return g_strdup(state_name(it->state));
  case COL_START:      return fmt_time(it->start_epoch, "%b %d %H:%M");
  case COL_NETWORK:
  default:             return g_strdup(DASH);
  }
}

/* Writes a cell from the item. Process cells update two labels. */
static void
refresh_widget(GtkWidget *w, ProcItem *it)
{
  ColumnId col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "col"));

  if (col == COL_PROCESS) {
    GtkWidget *name = g_object_get_data(G_OBJECT(w), "name-label");
    GtkWidget *sub = g_object_get_data(G_OBJECT(w), "sub-label");

    set_label_text(GTK_LABEL(name), it->name ? it->name : DASH);
    set_label_text(GTK_LABEL(sub), it->cmdline ? it->cmdline : (it->exe ? it->exe : ""));
  } else {
    g_autofree char *text = cell_text(col, it);
    set_label_text(GTK_LABEL(w), text);
  }
}

static void
on_stats_changed(ProcItem *it, gpointer widget)
{
  refresh_widget(GTK_WIDGET(widget), it);
}

/* Subscribes a widget to its item. The subscription ends on unbind or finalize. */
static void
bind_item_widget(GtkWidget *w, ProcItem *it)
{
  gulong h;

  refresh_widget(w, it);
  g_object_set_data_full(G_OBJECT(w), "bound-item", g_object_ref(it), g_object_unref);
  h = g_signal_connect_object(it, "stats-changed", G_CALLBACK(on_stats_changed), w, 0);
  g_object_set_data(G_OBJECT(w), "handler", GSIZE_TO_POINTER(h));
}

static void
unbind_item_widget(GtkWidget *w)
{
  ProcItem *it;
  gulong h;

  if (!w)
    return;
  it = g_object_get_data(G_OBJECT(w), "bound-item");
  if (!it)
    return;
  h = (gulong)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(w), "handler"));
  if (h)
    g_signal_handler_disconnect(it, h);
  g_object_set_data(G_OBJECT(w), "bound-item", NULL);   /* drops our ref */
}

static void
on_cell_setup(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
  ColumnId col = GPOINTER_TO_INT(data);
  GtkWidget *label = gtk_label_new(NULL);

  (void)factory;
  gtk_label_set_xalign(GTK_LABEL(label), is_numeric_column(col) ? 1.0f : 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_size_request(label, column_min_width[col], -1);
  gtk_widget_add_css_class(label, "numeric");
  g_object_set_data(G_OBJECT(label), "col", GINT_TO_POINTER(col));
  gtk_list_item_set_child(li, label);
}

static void
on_cell_bind(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
  ProcItem *it = item_from_obj(gtk_list_item_get_item(li));

  (void)factory;
  (void)data;
  if (it)
    bind_item_widget(gtk_list_item_get_child(li), it);
}

static void
on_cell_unbind(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
  (void)factory;
  (void)data;
  unbind_item_widget(gtk_list_item_get_child(li));
}

/* The Process column hosts a tree expander so tree mode gets disclosure triangles. */
static void
on_process_setup(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
  GtkWidget *exp = gtk_tree_expander_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *icon = gtk_image_new_from_icon_name("application-x-executable");
  GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
  GtkWidget *name = gtk_label_new(NULL);
  GtkWidget *sub = gtk_label_new(NULL);

  (void)factory;
  (void)data;

  gtk_image_set_pixel_size(GTK_IMAGE(icon), 24);
  gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(sub), 64);
  gtk_widget_add_css_class(sub, "dim-label");
  gtk_widget_add_css_class(sub, "caption");
  gtk_widget_set_size_request(texts, column_min_width[COL_PROCESS] - 40, -1);

  gtk_box_append(GTK_BOX(texts), name);
  gtk_box_append(GTK_BOX(texts), sub);
  gtk_box_append(GTK_BOX(box), icon);
  gtk_box_append(GTK_BOX(box), texts);
  gtk_tree_expander_set_child(GTK_TREE_EXPANDER(exp), box);

  g_object_set_data(G_OBJECT(box), "col", GINT_TO_POINTER(COL_PROCESS));
  g_object_set_data(G_OBJECT(box), "name-label", name);
  g_object_set_data(G_OBJECT(box), "sub-label", sub);
  gtk_list_item_set_child(li, exp);
}

static void
on_process_bind(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
  GtkTreeExpander *exp = GTK_TREE_EXPANDER(gtk_list_item_get_child(li));
  gpointer obj = gtk_list_item_get_item(li);
  GtkWidget *box = gtk_tree_expander_get_child(exp);
  ProcItem *it = item_from_obj(obj);

  (void)factory;
  (void)data;
  gtk_tree_expander_set_list_row(exp, GTK_IS_TREE_LIST_ROW(obj) ? GTK_TREE_LIST_ROW(obj) : NULL);
  if (it)
    bind_item_widget(box, it);
}

static void
on_process_unbind(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data)
{
  GtkTreeExpander *exp = GTK_TREE_EXPANDER(gtk_list_item_get_child(li));

  (void)factory;
  (void)data;
  unbind_item_widget(gtk_tree_expander_get_child(exp));
  gtk_tree_expander_set_list_row(exp, NULL);
}

static GtkColumnViewColumn *
make_column(ColumnId id)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  GtkColumnViewColumn *col;
  GtkSorter *sorter;

  if (id == COL_PROCESS) {
    g_signal_connect(factory, "setup", G_CALLBACK(on_process_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(on_process_bind), NULL);
    g_signal_connect(factory, "unbind", G_CALLBACK(on_process_unbind), NULL);
  } else {
    g_signal_connect(factory, "setup", G_CALLBACK(on_cell_setup), GINT_TO_POINTER(id));
    g_signal_connect(factory, "bind", G_CALLBACK(on_cell_bind), NULL);
    g_signal_connect(factory, "unbind", G_CALLBACK(on_cell_unbind), NULL);
  }

  /* gtk_column_view_column_new() takes ownership of the factory. */
  col = gtk_column_view_column_new(column_titles[id], factory);
  gtk_column_view_column_set_resizable(col, TRUE);
  gtk_column_view_column_set_expand(col, id == COL_PROCESS);

  sorter = GTK_SORTER(gtk_custom_sorter_new(compare_items, GINT_TO_POINTER(id), NULL));
  gtk_column_view_column_set_sorter(col, sorter);
  g_object_unref(sorter);
  return col;
}

/* ------------------------------------------------------------------------- */
/* Selection, details and actions                                            */
/* ------------------------------------------------------------------------- */

static void
processes_select(GtkSingleSelection *sel, GParamSpec *pspec, gpointer data)
{
  PageState *s = data;
  ProcItem *it = item_from_obj(gtk_single_selection_get_selected_item(sel));
  ProcItem *old;

  (void)pspec;
  if (it == s->selected)
    return;

  old = s->selected;
  s->selected = it ? g_object_ref(it) : NULL;
  s->selected_exited = FALSE;
  g_clear_object(&old);

  if (s->selected)
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->priority_spin), s->selected->nice);
  update_details(s);

  /* In collapsed layouts, a selection brings the details pane forward. */
  if (s->selected &&
      adw_navigation_split_view_get_collapsed(ADW_NAVIGATION_SPLIT_VIEW(s->split)))
    adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(s->split), FALSE);
}

static void
set_row(PageState *s, DetailId id, const char *text)
{
  adw_action_row_set_subtitle(ADW_ACTION_ROW(s->rows[id]), text ? text : DASH);
}

static void
set_rowf(PageState *s, DetailId id, const char *fmt, ...)
{
  va_list ap;
  char *text;

  va_start(ap, fmt);
  text = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(s->rows[id]), text);
  g_free(text);
}

static void
update_details(PageState *s)
{
  ProcItem *it = s->selected;
  guint64 clk = s->clk_tck > 0 ? (guint64)s->clk_tck : 100;

  gtk_stack_set_visible_child_name(GTK_STACK(s->details_stack),
                                   it ? "details" : (s->selected_exited ? "exited" : "empty"));
  for (guint i = 0; i < s->n_action_widgets; i++)
    gtk_widget_set_sensitive(s->action_widgets[i], it != NULL);
  if (!it)
    return;

  set_row(s, DETAIL_NAME, it->name);
  set_rowf(s, DETAIL_PID, "%d", it->key.pid);
  set_rowf(s, DETAIL_PPID, "%d", it->ppid);
  set_row(s, DETAIL_USER, it->user);
  set_row(s, DETAIL_EXE, it->exe);
  set_row(s, DETAIL_CMDLINE, it->cmdline);
  set_rowf(s, DETAIL_STATE, "%s (%c)", state_name(it->state), it->state);

  {
    g_autofree char *started = fmt_time(it->start_epoch, "%Y-%m-%d %H:%M:%S");
    g_autofree char *uptime = NULL;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;

    set_row(s, DETAIL_STARTED, started);
    if (it->start_epoch)
      uptime = fmt_duration(now - (gint64)it->start_epoch);
    set_row(s, DETAIL_UPTIME, uptime);
  }

  set_rowf(s, DETAIL_PRIORITY, "%d", it->priority);
  set_rowf(s, DETAIL_NICE, "%d", it->nice);
  set_row(s, DETAIL_AFFINITY, it->affinity);
  if (it->last_cpu >= 0)
    set_rowf(s, DETAIL_LAST_CPU, "%d", it->last_cpu);
  else
    set_row(s, DETAIL_LAST_CPU, NULL);

  {
    g_autofree char *cpu = fmt_percent(it->cpu_pct);
    g_autofree char *cpu_time = fmt_duration((gint64)(it->cpu_ticks / clk));
    set_row(s, DETAIL_CPU_USAGE, cpu);
    set_row(s, DETAIL_CPU_TIME, cpu_time);
  }

  {
    g_autofree char *res = fmt_size((double)it->rss);
    g_autofree char *virt = fmt_size((double)it->vsize);
    g_autofree char *shr = fmt_size((double)it->shared);
    set_row(s, DETAIL_RESIDENT, res);
    set_row(s, DETAIL_VIRTUAL, virt);
    set_row(s, DETAIL_SHARED, shr);
  }

  if (it->io_ok) {
    g_autofree char *r = fmt_rate(it->read_rate);
    g_autofree char *w = fmt_rate(it->write_rate);
    g_autofree char *tr = fmt_size((double)it->read_bytes);
    g_autofree char *tw = fmt_size((double)it->write_bytes);
    set_rowf(s, DETAIL_DISK_IO, "Read %s \xc2\xb7 Write %s \xc2\xb7 Total %s read, %s written",
             r, w, tr, tw);
  } else {
    set_row(s, DETAIL_DISK_IO, NULL);
  }

  if (it->fd_count >= 0)
    set_rowf(s, DETAIL_OPEN_FILES, "%d", it->fd_count);
  else
    set_row(s, DETAIL_OPEN_FILES, NULL);
  set_rowf(s, DETAIL_THREADS, "%d", it->threads);
  set_row(s, DETAIL_CGROUP, it->cgroup);
}

static void
set_status(PageState *s, const char *text)
{
  gtk_label_set_text(GTK_LABEL(s->action_status), text);
}

static void
run_privileged_finish(GObject *source, GAsyncResult *res, gpointer data)
{
  PrivCtx *c = data;
  GError *err = NULL;
  gboolean ok;
  PageState *s;

  (void)source;
  ok = g_subprocess_wait_check_finish(c->proc, res, &err);
  s = g_object_get_data(G_OBJECT(c->page), STATE_KEY);
  if (s && !g_cancellable_is_cancelled(s->cancellable)) {
    g_autofree char *msg = ok
      ? g_strdup_printf("%s: done.", c->desc)
      : g_strdup_printf("%s: not authorized or failed.", c->desc);
    set_status(s, msg);
    start_refresh(s);
  }

  g_clear_error(&err);
  g_object_unref(c->proc);
  g_object_unref(c->page);
  g_free(c->desc);
  g_free(c);
}

/* Runs a command through polkit (pkexec) when direct access is denied. */
static void
run_privileged(PageState *s, const char *desc, const char *const *argv)
{
  GError *err = NULL;
  GSubprocess *proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_NONE, &err);
  PrivCtx *c;

  if (!proc) {
    set_status(s, err->message);
    g_error_free(err);
    return;
  }
  c = g_new0(PrivCtx, 1);
  c->page = g_object_ref(s->page);
  c->proc = proc;
  c->desc = g_strdup(desc);
  g_subprocess_wait_check_async(proc, NULL, run_privileged_finish, c);
}

static int
pidfd_open_safe(pid_t pid)
{
#ifdef SYS_pidfd_open
  return (int)syscall(SYS_pidfd_open, pid, 0);
#else
  (void)pid;
  errno = ENOSYS;
  return -1;
#endif
}

static int
pidfd_send_signal_safe(int pidfd, int sig)
{
#ifdef SYS_pidfd_send_signal
  return (int)syscall(SYS_pidfd_send_signal, pidfd, sig, NULL, 0);
#else
  (void)pidfd;
  (void)sig;
  errno = ENOSYS;
  return -1;
#endif
}

static const char *
signal_name(int sig)
{
  switch (sig) {
  case SIGKILL: return "KILL";
  case SIGSTOP: return "STOP";
  case SIGCONT: return "CONT";
  case SIGTERM:
  default:      return "TERM";
  }
}

/*
 * Sends a signal to exactly one process incarnation. A pidfd pins the process
 * at open time, so a recycled PID cannot receive the signal. kill() is the
 * fallback on kernels without pidfd, after the identity check.
 */
static void
perform_signal(PageState *s, ProcKey key, int sig, const char *label)
{
  int pfd = pidfd_open_safe(key.pid);
  int ret, err;

  if (pfd < 0 && errno == ESRCH) {
    set_status(s, "The process has already exited.");
    start_refresh(s);
    return;
  }
  if (!process_identity_matches(s, key)) {
    if (pfd >= 0)
      close(pfd);
    set_status(s, "The process changed or exited. Nothing was sent.");
    start_refresh(s);
    return;
  }

  if (pfd >= 0) {
    ret = pidfd_send_signal_safe(pfd, sig);
    err = errno;
    close(pfd);
  } else {
    ret = kill(key.pid, sig);
    err = errno;
  }

  if (ret == 0) {
    g_autofree char *msg = g_strdup_printf("%s sent to PID %d.", label, key.pid);
    set_status(s, msg);
  } else if (err == EPERM || err == EACCES) {
    g_autofree char *pid_s = g_strdup_printf("%d", key.pid);
    const char *argv[] = { "pkexec", "kill", "-s", signal_name(sig), pid_s, NULL };
    run_privileged(s, label, argv);
    return;                                /* refresh happens on completion */
  } else if (err == ESRCH) {
    set_status(s, "The process has already exited.");
  } else {
    set_status(s, g_strerror(err));
  }
  start_refresh(s);
}

static void
perform_set_nice(PageState *s, ProcKey key, int nice_value)
{
  g_autofree char *nice_s = g_strdup_printf("%d", nice_value);
  g_autofree char *pid_s = g_strdup_printf("%d", key.pid);

  if (!process_identity_matches(s, key)) {
    set_status(s, "The process changed or exited. Priority was not changed.");
    start_refresh(s);
    return;
  }

  if (setpriority(PRIO_PROCESS, (id_t)key.pid, nice_value) == 0) {
    g_autofree char *msg = g_strdup_printf("Nice value set to %d.", nice_value);
    set_status(s, msg);
    start_refresh(s);
  } else if (errno == EACCES || errno == EPERM) {
    const char *argv[] = { "pkexec", "renice", "-n", nice_s, "-p", pid_s, NULL };
    run_privileged(s, "Priority change", argv);
  } else {
    set_status(s, g_strerror(errno));
    start_refresh(s);
  }
}

static void
on_confirm_done(GObject *source, GAsyncResult *res, gpointer data)
{
  ConfirmCtx *c = data;
  GError *err = NULL;
  int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, &err);
  PageState *s = g_object_get_data(G_OBJECT(c->page), STATE_KEY);

  /* choice 1 is the confirm button; -1 or 0 means cancelled. */
  if (choice == 1 && s && !g_cancellable_is_cancelled(s->cancellable))
    perform_signal(s, c->key, c->sig, c->label);

  g_clear_error(&err);
  g_object_unref(c->dialog);
  g_object_unref(c->page);
  g_free(c);
}

static void
request_action(PageState *s, const ActionDef *def)
{
  ProcItem *it = s->selected;
  ProcKey key;
  GtkAlertDialog *dialog;
  ConfirmCtx *c;
  GtkRoot *root;
  const char *buttons[] = { "Cancel", NULL, NULL };
  g_autofree char *heading = NULL;
  g_autofree char *detail = NULL;

  if (!it)
    return;
  key = it->key;

  if (!def->confirm) {
    perform_signal(s, key, def->sig, def->label);
    return;
  }

  heading = g_strdup_printf("%s %s?", def->label, it->name ? it->name : "this process");
  detail = g_strdup_printf("PID %d. Unsaved work in this process may be lost.", key.pid);
  buttons[1] = def->label;

  dialog = gtk_alert_dialog_new("%s", heading);
  gtk_alert_dialog_set_detail(dialog, detail);
  gtk_alert_dialog_set_buttons(dialog, buttons);
  gtk_alert_dialog_set_cancel_button(dialog, 0);
  gtk_alert_dialog_set_default_button(dialog, 0);

  c = g_new0(ConfirmCtx, 1);
  c->page = g_object_ref(s->page);
  c->dialog = dialog;
  c->key = key;
  c->sig = def->sig;
  c->label = def->label;

  root = gtk_widget_get_root(s->page);
  gtk_alert_dialog_choose(dialog, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL,
                          NULL, on_confirm_done, c);
}

static void
on_action_clicked(GtkButton *btn, gpointer data)
{
  PageState *s = data;
  guint idx = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "action-index"));

  if (idx < G_N_ELEMENTS(process_actions))
    request_action(s, &process_actions[idx]);
}

static void
on_priority_apply(GtkButton *btn, gpointer data)
{
  PageState *s = data;

  (void)btn;
  gtk_popover_popdown(GTK_POPOVER(s->priority_popover));
  if (s->selected)
    perform_set_nice(s, s->selected->key,
                     gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s->priority_spin)));
}

/* ------------------------------------------------------------------------- */
/* Refresh loop                                                              */

static void
start_refresh(PageState *s)
{
  if (!s || !s->app)
    return;
  processes_update(s, app_get_processes(s->app));
}

static void
on_backend_processes_updated(App *app, gpointer data)
{
  PageState *s = data;
  if (!s || s->app != app || g_cancellable_is_cancelled(s->cancellable))
    return;
  start_refresh(s);
}

/* Runs when the root widget is finalized, after its children are gone. */
static void
page_state_free(gpointer data)
{
  PageState *s = data;

  if (s->backend_handler && s->app)
    app_disconnect_processes_updated(s->app, s->backend_handler);
  s->backend_handler = 0;
  g_cancellable_cancel(s->cancellable);
  g_object_unref(s->cancellable);
  g_clear_handle_id(&s->debounce_source, g_source_remove);

  if (s->sel) {
    g_signal_handlers_disconnect_by_data(s->sel, s);
    g_clear_object(&s->sel);
  }

  g_clear_object(&s->selected);
  g_hash_table_destroy(s->items);
  g_clear_object(&s->match_filter);
  g_clear_object(&s->root_filter);
  g_clear_object(&s->store);
  g_free(s->search_text);
  g_free(s);
}

/* ------------------------------------------------------------------------- */
/* UI construction                                                           */
/* ------------------------------------------------------------------------- */

static GtkWidget *
processes_create_header(PageState *s)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *title = gtk_label_new("Processes");

  gtk_widget_add_css_class(title, "title-2");
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);

  s->status_label = gtk_label_new("Loading\xe2\x80\xa6");
  gtk_widget_add_css_class(s->status_label, "dim-label");
  gtk_widget_add_css_class(s->status_label, "caption");
  gtk_label_set_xalign(GTK_LABEL(s->status_label), 1.0f);
  gtk_widget_set_hexpand(s->status_label, TRUE);

  gtk_box_append(GTK_BOX(box), title);
  gtk_box_append(GTK_BOX(box), s->status_label);
  return box;
}

static GtkWidget *
processes_create_toolbar(PageState *s)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *filter_dd, *tree_btn, *dir_btn;
  GtkStringList *filters;

  s->search = gtk_search_entry_new();
  gtk_widget_set_hexpand(s->search, TRUE);
  gtk_widget_set_tooltip_text(s->search, "Search name, PID, user, executable or command line");
  g_signal_connect(s->search, "search-changed", G_CALLBACK(on_search_changed), s);

  filters = gtk_string_list_new(filter_labels);
  filter_dd = gtk_drop_down_new(G_LIST_MODEL(filters), NULL);
  g_signal_connect(filter_dd, "notify::selected", G_CALLBACK(processes_filter), s);

  s->sort_dd = gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(column_titles)), NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(s->sort_dd), COL_CPU);   /* before connecting */
  g_signal_connect(s->sort_dd, "notify::selected", G_CALLBACK(processes_sort), s);

  s->sort_desc = TRUE;
  dir_btn = gtk_button_new_from_icon_name("view-sort-descending-symbolic");
  s->sort_dir_btn = dir_btn;
  gtk_widget_set_tooltip_text(dir_btn, "Toggle ascending or descending order");
  g_signal_connect(dir_btn, "clicked", G_CALLBACK(on_sort_direction), s);

  tree_btn = gtk_toggle_button_new_with_label("Tree");
  gtk_widget_set_tooltip_text(tree_btn, "Show parent and child processes");
  g_signal_connect(tree_btn, "toggled", G_CALLBACK(on_tree_toggled), s);

  gtk_box_append(GTK_BOX(box), s->search);
  gtk_box_append(GTK_BOX(box), filter_dd);
  gtk_box_append(GTK_BOX(box), s->sort_dd);
  gtk_box_append(GTK_BOX(box), dir_btn);
  gtk_box_append(GTK_BOX(box), tree_btn);
  return box;
}

static GtkWidget *
processes_create_view(PageState *s)
{
  GtkWidget *scroll = gtk_scrolled_window_new();

  s->view = gtk_column_view_new(NULL);
  gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(s->view), TRUE);
  for (guint i = 0; i < COL_COUNT; i++) {
    s->columns[i] = make_column(i);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(s->view), s->columns[i]);
    g_object_unref(s->columns[i]);      /* the view keeps its own reference */
  }

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), s->view);
  gtk_widget_set_hexpand(scroll, TRUE);
  gtk_widget_set_vexpand(scroll, TRUE);
  return scroll;
}

static void
add_detail_rows(PageState *s, GtkWidget *box)
{
  AdwPreferencesGroup *group = NULL;

  for (guint i = 0; i < DETAIL_COUNT; i++) {
    AdwActionRow *row;

    if (detail_defs[i].group) {
      group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
      adw_preferences_group_set_title(group, detail_defs[i].group);
      gtk_box_append(GTK_BOX(box), GTK_WIDGET(group));
    }
    row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), detail_defs[i].title);
    adw_action_row_set_subtitle_selectable(row, TRUE);
    adw_action_row_set_subtitle_lines(row, 3);
    adw_preferences_group_add(group, GTK_WIDGET(row));
    s->rows[i] = GTK_WIDGET(row);
  }
}

static GtkWidget *
processes_create_actions(PageState *s)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *menu;
  GtkWidget *popover = gtk_popover_new();
  GtkWidget *pop_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *apply;

  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);

  s->action_status = gtk_label_new("");
  gtk_label_set_wrap(GTK_LABEL(s->action_status), TRUE);
  gtk_label_set_xalign(GTK_LABEL(s->action_status), 0.0f);
  gtk_widget_add_css_class(s->action_status, "dim-label");
  gtk_widget_add_css_class(s->action_status, "caption");
  gtk_box_append(GTK_BOX(box), s->action_status);

  for (guint i = 0; i < G_N_ELEMENTS(process_actions); i++) {
    GtkWidget *btn = gtk_button_new_with_label(process_actions[i].label);

    if (process_actions[i].sig == SIGKILL)
      gtk_widget_add_css_class(btn, "destructive-action");
    g_object_set_data(G_OBJECT(btn), "action-index", GUINT_TO_POINTER(i));
    g_signal_connect(btn, "clicked", G_CALLBACK(on_action_clicked), s);
    gtk_widget_set_sensitive(btn, FALSE);
    gtk_box_append(GTK_BOX(row), btn);
    s->action_widgets[s->n_action_widgets++] = btn;
  }

  /* Priority is a popover with a nice-value spin button, not a destructive action. */
  s->priority_spin = gtk_spin_button_new_with_range(-20, 19, 1);
  apply = gtk_button_new_with_label("Apply");
  gtk_widget_add_css_class(apply, "suggested-action");
  g_signal_connect(apply, "clicked", G_CALLBACK(on_priority_apply), s);
  gtk_box_append(GTK_BOX(pop_box), gtk_label_new("Nice value (-20 high, 19 low)"));
  gtk_box_append(GTK_BOX(pop_box), s->priority_spin);
  gtk_box_append(GTK_BOX(pop_box), apply);
  gtk_widget_set_margin_top(pop_box, 8);
  gtk_widget_set_margin_bottom(pop_box, 8);
  gtk_widget_set_margin_start(pop_box, 8);
  gtk_widget_set_margin_end(pop_box, 8);
  gtk_popover_set_child(GTK_POPOVER(popover), pop_box);
  s->priority_popover = popover;

  menu = gtk_menu_button_new();
  gtk_menu_button_set_label(GTK_MENU_BUTTON(menu), "Priority");
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), popover);
  gtk_widget_set_sensitive(menu, FALSE);
  gtk_box_append(GTK_BOX(row), menu);
  s->action_widgets[s->n_action_widgets++] = menu;

  gtk_box_append(GTK_BOX(box), row);
  return box;
}

static GtkWidget *
processes_create_details(PageState *s)
{
  GtkWidget *toolbar = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  GtkWidget *scroll = gtk_scrolled_window_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  GtkWidget *empty, *exited;

  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), gtk_label_new("Details"));
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

  s->details_stack = gtk_stack_new();

  empty = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "view-list-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(empty), "No process selected");
  adw_status_page_set_description(ADW_STATUS_PAGE(empty),
                                  "Select a process to see its details and actions.");
  gtk_stack_add_named(GTK_STACK(s->details_stack), empty, "empty");

  exited = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(exited), "process-stop-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(exited), "Process exited");
  adw_status_page_set_description(ADW_STATUS_PAGE(exited),
                                  "The selected process is no longer running.");
  gtk_stack_add_named(GTK_STACK(s->details_stack), exited, "exited");

  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  add_detail_rows(s, box);

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
  gtk_stack_add_named(GTK_STACK(s->details_stack), scroll, "details");

  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), s->details_stack);
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar), processes_create_actions(s));
  return toolbar;
}

/* ------------------------------------------------------------------------- */
/* Public entry point                                                        */
/* ------------------------------------------------------------------------- */

GtkWidget *
processes_page_create(App *app)
{
  PageState *s;
  GtkWidget *root;
  GtkWidget *content_view;
  AdwNavigationPage *content_page;
  AdwNavigationPage *side_page;
  long tck;

  g_return_val_if_fail(app != NULL, NULL);

  s = g_new0(PageState, 1);
  s->app = app;
  s->store = g_list_store_new(proc_item_get_type());
  s->items = g_hash_table_new(key_hash, key_equal);
  s->match_filter = GTK_FILTER(gtk_custom_filter_new(match_func, s, NULL));
  s->root_filter = GTK_FILTER(gtk_custom_filter_new(root_func, NULL, NULL));
  s->cancellable = g_cancellable_new();
  s->sort_desc = TRUE;
  s->filter_mode = FILTER_ALL;

  tck = sysconf(_SC_CLK_TCK);
  s->clk_tck = tck > 0 ? tck : 100;

  root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(root, 16);
  gtk_widget_set_margin_bottom(root, 16);
  gtk_widget_set_margin_start(root, 16);
  gtk_widget_set_margin_end(root, 16);

  s->page = root;

  g_object_set_data_full(
      G_OBJECT(root),
      STATE_KEY,
      s,
      page_state_free);

  gtk_box_append(
      GTK_BOX(root),
      processes_create_header(s));

  gtk_box_append(
      GTK_BOX(root),
      processes_create_toolbar(s));

  content_view = processes_create_view(s);

  content_page = adw_navigation_page_new(
      content_view,
      "Processes");

  side_page = adw_navigation_page_new(
      processes_create_details(s),
      "Details");

  s->split = adw_navigation_split_view_new();

  adw_navigation_split_view_set_content(
      ADW_NAVIGATION_SPLIT_VIEW(s->split),
      content_page);

  adw_navigation_split_view_set_sidebar(
      ADW_NAVIGATION_SPLIT_VIEW(s->split),
      side_page);

  adw_navigation_split_view_set_min_sidebar_width(
      ADW_NAVIGATION_SPLIT_VIEW(s->split),
      340);

  adw_navigation_split_view_set_max_sidebar_width(
      ADW_NAVIGATION_SPLIT_VIEW(s->split),
      460);

  adw_navigation_split_view_set_sidebar_width_fraction(
      ADW_NAVIGATION_SPLIT_VIEW(s->split),
      0.36);

  gtk_widget_set_vexpand(s->split, TRUE);

  gtk_box_append(
      GTK_BOX(root),
      s->split);

  /*
   * Establish the initial column sorter before constructing
   * the model chain used by GtkSingleSelection.
   */
  apply_sort(s, COL_CPU);
  processes_refresh_model(s);

  s->backend_handler = app_connect_processes_updated(
      s->app,
      on_backend_processes_updated,
      s);

  start_refresh(s);

  return root;
}
