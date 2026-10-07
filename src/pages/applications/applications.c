/*
 * applications.c - Page 3: Applications
 *
 * A self-contained application-library page: installed graphical applications
 * (XDG desktop entries), their live running state aggregated from the shared
 * process backend, and launch / focus / quit actions.
 *
 * Layering inside this file:
 *
 *   BACKEND CONTRACT   what we need from app.h (nothing else is touched)
 *   AppsEntry          GObject: one installed application + live state
 *   Discovery          GIO desktop-entry scan (worker thread) + merge
 *   Matching           process -> application association and aggregation
 *   Models             GListStore -> filter -> sort -> single selection
 *   Presentation       header, grid, list, detail panel
 *   Actions            launch, focus, terminate
 *   Page lifecycle     applications_page_create()
 *
 * Requires: GTK >= 4.12, libadwaita >= 1.5, GLib >= 2.70.
 */

#include "applications.h"
#include "../../graphs/graph.h"

#include <adwaita.h>
#include <gio/gdesktopappinfo.h>

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <string.h>

/* ========================================================================== *
 * BACKEND CONTRACT
 *
 * This page does NOT read /proc and keeps no poller of its own.  It consumes
 * the shared backend through the following interface, expected from app.h:
 *
 *   typedef struct {
 *       pid_t              pid, ppid;
 *       const char        *name;          // comm
 *       const char        *exe;           // resolved exe path, may be NULL
 *       const char *const *argv;          // NULL-terminated, may be NULL
 *       const char        *cgroup;        // e.g. "0::/user.slice/.../app-gnome-firefox-123.scope", may be NULL
 *       const char        *user;          // owner name
 *       double             cpu_percent;   // per-process, 100 == one core
 *       guint64            rss_bytes;
 *       gint64             start_time_us; // unix epoch microseconds, 0 if unknown
 *   } ProcessInfo;
 *
 *   // Borrowed array of ProcessInfo*, valid until the next backend refresh.
 *   GPtrArray *app_get_processes (App *app);
 *
 *   // Called on the GTK main thread after every backend refresh.
 *   gulong app_connect_processes_updated    (App *app, void (*cb)(App *, gpointer), gpointer data);
 *   void   app_disconnect_processes_updated (App *app, gulong id);
 *
 *   // Shared graph/history widget used by the other pages.
 *   GtkWidget *graph_new   (const char *title);
 *   void       graph_clear (GtkWidget *graph);
 *   void       graph_push  (GtkWidget *graph, double value);
 *
 * If the real names differ, adjust the four thin wrappers below; nothing else
 * in this file depends on them.
 * ========================================================================== */

typedef ProcessInfo ApplicationsProc;

static GPtrArray *
backend_processes (App *app)
{
  return app_get_processes (app);
}

static gulong
backend_connect (App *app, void (*cb) (App *, gpointer), gpointer data)
{
  return app_connect_processes_updated (app, cb, data);
}

static void
backend_disconnect (App *app, gulong id)
{
  if (id != 0)
    app_disconnect_processes_updated (app, id);
}

static GtkWidget *
backend_graph_new (const char *title)
{
  return graph_new (title);
}

/* ========================================================================== */

#define HIST_LEN        120          /* samples kept per application           */
#define CPU_EPS         0.05         /* ignore CPU changes below this (%)      */
#define MEM_EPS         (256 * 1024) /* ignore RSS changes below this          */
#define SORT_CPU_STEP   2.0          /* re-sort only when CPU moved this much  */
#define SORT_MEM_STEP   (16 * 1024 * 1024)

typedef enum { VIEW_GRID, VIEW_LIST } ViewMode;

typedef enum {
  SORT_NAME, SORT_RUNNING, SORT_CPU, SORT_MEM, SORT_PROCS
} SortMode;

typedef enum {
  FILTER_ALL, FILTER_RUNNING, FILTER_STOPPED,
  FILTER_DEV, FILTER_NET, FILTER_MEDIA, FILTER_GFX, FILTER_SYS, FILTER_UTIL
} FilterMode;

enum {
  CAT_DEV   = 1 << 0,
  CAT_NET   = 1 << 1,
  CAT_MEDIA = 1 << 2,
  CAT_GFX   = 1 << 3,
  CAT_SYS   = 1 << 4,
  CAT_UTIL  = 1 << 5,
};

static const struct { FilterMode mode; const char *label; } filter_items[] = {
  { FILTER_ALL,     "All applications" },
  { FILTER_RUNNING, "Running" },
  { FILTER_STOPPED, "Not running" },
  { FILTER_DEV,     "Development" },
  { FILTER_NET,     "Internet" },
  { FILTER_MEDIA,   "Multimedia" },
  { FILTER_GFX,     "Graphics" },
  { FILTER_SYS,     "System" },
  { FILTER_UTIL,    "Utilities" },
};

static const char *const sort_labels[] = {
  "Name", "Running status", "CPU usage", "Memory usage", "Process count", NULL
};

/* ========================================================================== *
 * AppsEntry: one installed application
 * ========================================================================== */

#define APPS_TYPE_ENTRY (apps_entry_get_type ())
G_DECLARE_FINAL_TYPE (AppsEntry, apps_entry, APPS, ENTRY, GObject)

struct _AppsEntry {
  GObject parent_instance;

  /* ---- static metadata (from the desktop entry) ---- */
  GDesktopAppInfo *dinfo;
  char     *id;            /* "org.gnome.Nautilus.desktop"            */
  char     *id_stem;       /* lower-case id without ".desktop"        */
  char     *name;
  char     *generic;
  char     *comment;
  char     *desktop_path;
  char     *categories;    /* raw "A;B;C;"                            */
  char     *exec_line;
  char    **exec_argv;     /* wrappers and field codes removed        */
  char     *exec_key;      /* lower-case matching key, may be NULL    */
  char     *wm_class;      /* lower-case StartupWMClass, may be NULL  */
  char     *haystack;      /* casefolded search text                  */
  char     *name_key;      /* locale collation key                    */
  GIcon    *icon;
  guint     cat_mask;
  gboolean  terminal;
  gboolean  startup_notify;
  gboolean  dbus_activatable;
  guint     seen_gen;

  /* ---- live state (committed values shown in the UI) ---- */
  gboolean  running;
  guint     nprocs;
  double    cpu;
  guint64   mem;
  gint64    start_time_us;
  char     *user;
  GArray   *pids;          /* pid_t */

  /* ---- sort keys, updated with hysteresis for a stable order ---- */
  gboolean  skey_running;
  guint     skey_procs;
  double    skey_cpu;
  guint64   skey_mem;

  /* ---- per-refresh accumulators ---- */
  guint     acc_n;
  double    acc_cpu;
  guint64   acc_mem;
  gint64    acc_start;
  const char *acc_user;
  GArray   *acc_pids;

  /* ---- history: kept after exit, reset on next start ---- */
  GArray   *cpu_hist;      /* double */
  GArray   *mem_hist;      /* double, MiB */
};

G_DEFINE_FINAL_TYPE (AppsEntry, apps_entry, G_TYPE_OBJECT)

enum { SIGNAL_UPDATED, N_ENTRY_SIGNALS };
static guint entry_signals[N_ENTRY_SIGNALS];

static void
entry_clear_static (AppsEntry *e)
{
  g_clear_pointer (&e->id, g_free);
  g_clear_pointer (&e->id_stem, g_free);
  g_clear_pointer (&e->name, g_free);
  g_clear_pointer (&e->generic, g_free);
  g_clear_pointer (&e->comment, g_free);
  g_clear_pointer (&e->desktop_path, g_free);
  g_clear_pointer (&e->categories, g_free);
  g_clear_pointer (&e->exec_line, g_free);
  g_clear_pointer (&e->exec_argv, g_strfreev);
  g_clear_pointer (&e->exec_key, g_free);
  g_clear_pointer (&e->wm_class, g_free);
  g_clear_pointer (&e->haystack, g_free);
  g_clear_pointer (&e->name_key, g_free);
  g_clear_object (&e->icon);
  g_clear_object (&e->dinfo);
}

static void
apps_entry_finalize (GObject *obj)
{
  AppsEntry *e = APPS_ENTRY (obj);

  entry_clear_static (e);
  g_free (e->user);
  g_clear_pointer (&e->pids, g_array_unref);
  g_clear_pointer (&e->acc_pids, g_array_unref);
  g_clear_pointer (&e->cpu_hist, g_array_unref);
  g_clear_pointer (&e->mem_hist, g_array_unref);

  G_OBJECT_CLASS (apps_entry_parent_class)->finalize (obj);
}

static void
apps_entry_class_init (AppsEntryClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = apps_entry_finalize;
  entry_signals[SIGNAL_UPDATED] =
    g_signal_new ("updated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
apps_entry_init (AppsEntry *e)
{
  e->pids     = g_array_new (FALSE, FALSE, sizeof (pid_t));
  e->acc_pids = g_array_new (FALSE, FALSE, sizeof (pid_t));
  e->cpu_hist = g_array_new (FALSE, FALSE, sizeof (double));
  e->mem_hist = g_array_new (FALSE, FALSE, sizeof (double));
}

static void
entry_emit_updated (AppsEntry *e)
{
  g_signal_emit (e, entry_signals[SIGNAL_UPDATED], 0);
}

/* ========================================================================== *
 * Desktop-entry parsing helpers
 * ========================================================================== */

static gboolean
is_interpreter (const char *base)
{
  static const char *const names[] = {
    "python", "python2", "python3", "java", "node", "nodejs", "perl", "ruby",
    "sh", "bash", "dash", "zsh", "env", "mono", "dotnet", "wine", "wine64", NULL
  };

  if (g_str_has_prefix (base, "python"))
    return TRUE;
  return g_strv_contains (names, base);
}

/* Parse an Exec line into a clean argv and a lower-case matching key. */
static void
parse_exec (const char *cmd, char ***argv_out, char **key_out)
{
  g_auto (GStrv) raw = NULL;
  GPtrArray *clean;
  guint i = 0;
  char *key = NULL;

  *argv_out = NULL;
  *key_out = NULL;
  if (cmd == NULL || !g_shell_parse_argv (cmd, NULL, &raw, NULL))
    return;

  clean = g_ptr_array_new_with_free_func (g_free);
  for (guint k = 0; raw[k] != NULL; k++)
    {
      /* drop field codes (%U, %f, ...) and flatpak "@@u" markers */
      if ((raw[k][0] == '%' && raw[k][1] != '\0') ||
          g_str_has_prefix (raw[k], "@@"))
        continue;
      g_ptr_array_add (clean, g_strdup (raw[k]));
    }

  /* skip wrappers: env [-opts] [VAR=val]... */
  while (i < clean->len)
    {
      const char *tok = g_ptr_array_index (clean, i);
      g_autofree char *base = g_path_get_basename (tok);

      if (strcmp (base, "env") == 0)
        { i++; continue; }
      if (i > 0 && (tok[0] == '-' || strchr (tok, '=') != NULL) &&
          strchr (tok, '/') == NULL)
        { i++; continue; }
      if (strchr (tok, '=') != NULL && tok[0] != '/' && tok[0] != '-')
        { i++; continue; }
      break;
    }

  if (i < clean->len)
    {
      const char *first = g_ptr_array_index (clean, i);
      g_autofree char *base = g_path_get_basename (first);

      if (strcmp (base, "flatpak") == 0 || strcmp (base, "snap") == 0)
        {
          /* "flatpak run [opts] APP-ID args..." */
          guint j = i + 1;
          if (j < clean->len && strcmp (g_ptr_array_index (clean, j), "run") == 0)
            {
              j++;
              while (j < clean->len &&
                     ((const char *) g_ptr_array_index (clean, j))[0] == '-')
                j++;
              if (j < clean->len)
                {
                  key = g_utf8_strdown (g_ptr_array_index (clean, j), -1);
                  i = j;
                }
            }
        }
      else if (is_interpreter (base))
        {
          /* script launchers: key on the script, not the interpreter */
          guint j = i + 1;
          while (j < clean->len &&
                 ((const char *) g_ptr_array_index (clean, j))[0] == '-')
            j++;
          if (j < clean->len && strcmp (base, "sh") != 0 &&
              strcmp (base, "bash") != 0 && strcmp (base, "dash") != 0)
            {
              g_autofree char *sb = g_path_get_basename (g_ptr_array_index (clean, j));
              char *dot = strrchr (sb, '.');
              if (dot != NULL && dot != sb)
                *dot = '\0';
              key = g_utf8_strdown (sb, -1);
            }
        }
      else
        {
          g_autofree char *lower = g_utf8_strdown (base, -1);
          if (g_str_has_suffix (lower, "-bin") && strlen (lower) > 4)
            lower[strlen (lower) - 4] = '\0';
          key = g_strdup (lower);
        }
    }

  {
    GPtrArray *out = g_ptr_array_new ();
    for (guint k = i; k < clean->len; k++)
      g_ptr_array_add (out, g_strdup (g_ptr_array_index (clean, k)));
    g_ptr_array_add (out, NULL);
    *argv_out = (char **) g_ptr_array_free (out, FALSE);
  }
  *key_out = key;
  g_ptr_array_unref (clean);
}

static guint
categories_to_mask (const char *cats)
{
  guint mask = 0;
  g_auto (GStrv) v = NULL;

  if (cats == NULL)
    return 0;
  v = g_strsplit (cats, ";", -1);
  for (guint i = 0; v[i] != NULL; i++)
    {
      /* Only registered main categories; nothing is invented. */
      if (strcmp (v[i], "Development") == 0)       mask |= CAT_DEV;
      else if (strcmp (v[i], "Network") == 0)      mask |= CAT_NET;
      else if (strcmp (v[i], "AudioVideo") == 0 ||
               strcmp (v[i], "Audio") == 0 ||
               strcmp (v[i], "Video") == 0)        mask |= CAT_MEDIA;
      else if (strcmp (v[i], "Graphics") == 0)     mask |= CAT_GFX;
      else if (strcmp (v[i], "System") == 0)       mask |= CAT_SYS;
      else if (strcmp (v[i], "Utility") == 0)      mask |= CAT_UTIL;
    }
  return mask;
}

static GIcon *
resolve_icon (GAppInfo *info)
{
  GIcon *icon = g_app_info_get_icon (info);
  GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());

  if (icon != NULL && G_IS_THEMED_ICON (icon) &&
      !gtk_icon_theme_has_gicon (theme, icon))
    icon = NULL;
  if (icon != NULL && G_IS_FILE_ICON (icon))
    {
      GFile *f = g_file_icon_get_file (G_FILE_ICON (icon));
      if (f == NULL || !g_file_query_exists (f, NULL))
        icon = NULL;
    }
  if (icon != NULL)
    return g_object_ref (icon);
  return g_themed_icon_new ("application-x-executable");
}

static void
entry_set_info (AppsEntry *e, GDesktopAppInfo *d)
{
  GAppInfo *ai = G_APP_INFO (d);
  const char *s;
  g_autofree char *stem = NULL;
  g_autoptr (GString) hay = g_string_new (NULL);
  g_autofree char *folded = NULL;

  entry_clear_static (e);

  e->dinfo = g_object_ref (d);
  e->id = g_strdup (g_app_info_get_id (ai));
  stem = g_strdup (e->id != NULL ? e->id : "");
  if (g_str_has_suffix (stem, ".desktop"))
    stem[strlen (stem) - 8] = '\0';
  e->id_stem = g_utf8_strdown (stem, -1);

  s = g_app_info_get_display_name (ai);
  e->name = g_strdup (s != NULL && *s ? s : (e->id != NULL ? e->id : "Unnamed"));
  e->generic = g_strdup (g_desktop_app_info_get_generic_name (d));
  e->comment = g_strdup (g_app_info_get_description (ai));
  e->desktop_path = g_strdup (g_desktop_app_info_get_filename (d));
  e->categories = g_strdup (g_desktop_app_info_get_categories (d));
  e->exec_line = g_strdup (g_app_info_get_commandline (ai));
  parse_exec (e->exec_line, &e->exec_argv, &e->exec_key);

  s = g_desktop_app_info_get_startup_wm_class (d);
  e->wm_class = s != NULL ? g_utf8_strdown (s, -1) : NULL;

  e->terminal = g_desktop_app_info_get_boolean (d, "Terminal");
  e->startup_notify = g_desktop_app_info_get_boolean (d, "StartupNotify");
  e->dbus_activatable = g_desktop_app_info_get_boolean (d, "DBusActivatable");
  e->cat_mask = categories_to_mask (e->categories);
  e->icon = resolve_icon (ai);
  e->name_key = g_utf8_collate_key (e->name, -1);

  g_string_append_printf (hay, "%s\n%s\n%s\n%s\n%s",
                          e->name,
                          e->generic ? e->generic : "",
                          e->comment ? e->comment : "",
                          e->categories ? e->categories : "",
                          e->exec_key ? e->exec_key : "");
  folded = g_utf8_casefold (hay->str, -1);
  e->haystack = g_steal_pointer (&folded);
}

static AppsEntry *
entry_new (GDesktopAppInfo *d)
{
  AppsEntry *e = g_object_new (APPS_TYPE_ENTRY, NULL);
  entry_set_info (e, d);
  return e;
}

/* ========================================================================== *
 * Page state (private)
 * ========================================================================== */

typedef struct {
  App        *app;
  GtkWidget  *root;               /* AdwToastOverlay, owned by the caller   */
  gulong      refresh_id;
  gboolean    loaded;
  gboolean    discovering;
  guint       generation;

  /* data */
  GPtrArray  *entries;            /* AppsEntry*, owned                      */
  GHashTable *by_id;              /* id -> AppsEntry* (unowned)             */
  GHashTable *by_key;             /* key -> GPtrArray<AppsEntry*> (unowned) */
  GAppInfoMonitor *monitor;
  guint       rediscover_id;
  GCancellable *cancel;

  /* models */
  GListStore         *store;
  GtkFilterListModel *filtered;
  GtkSortListModel   *sorted;
  GtkSingleSelection *sel;
  GtkCustomFilter    *filter;
  GtkCustomSorter    *sorter;

  /* view state */
  FilterMode  filter_mode;
  SortMode    sort_mode;
  GStrv       tokens;             /* casefolded search words                */
  gboolean    syncing;

  /* widgets */
  GtkWidget  *search;
  GtkWidget  *counts;
  GtkWidget  *filter_drop;
  GtkStringList *filter_list;
  GtkWidget  *sort_drop;
  GtkWidget  *btn_grid;
  GtkWidget  *btn_list;
  GtkWidget  *view_stack;         /* grid | list                            */
  GtkWidget  *outer_stack;        /* loading | results | empty              */
  GtkWidget  *split;

  /* detail panel */
  GtkWidget  *d_icon, *d_name, *d_desc;
  GtkWidget  *d_row_file, *d_row_exec, *d_row_cats;
  GtkWidget  *d_row_state, *d_row_procs, *d_row_cpu, *d_row_mem,
             *d_row_start, *d_row_user;
  GtkWidget  *d_cpu_graph, *d_mem_graph;
  GtkWidget  *d_btn_launch, *d_btn_focus, *d_btn_quit;
  AppsEntry  *d_graph_entry;      /* entry whose history the graphs show    */
} PageState;

static PageState *
state_of (GtkWidget *w)
{
  return g_object_get_data (G_OBJECT (w), "apps-page");
}

/* ========================================================================== *
 * Formatting
 * ========================================================================== */

static char *
fmt_cpu (double v)
{
  return g_strdup_printf ("%.1f%%", v);
}

static char *
fmt_mem (guint64 b)
{
  return g_format_size_full (b, G_FORMAT_SIZE_DEFAULT);
}

static char *
fmt_state (AppsEntry *e)
{
  if (!e->running)
    return g_strdup ("Not running");
  if (e->nprocs > 1)
    return g_strdup_printf ("Running · %u processes", e->nprocs);
  return g_strdup ("Running");
}

static const char *
short_description (AppsEntry *e)
{
  if (e->comment != NULL && *e->comment)
    return e->comment;
  if (e->generic != NULL && *e->generic)
    return e->generic;
  return "";
}

/* ========================================================================== *
 * Discovery (worker thread) and merge (main thread)
 * ========================================================================== */

static void
discover_thread (GTask *task, gpointer src, gpointer data, GCancellable *c)
{
  GList *all = g_app_info_get_all ();
  GPtrArray *out = g_ptr_array_new_with_free_func (g_object_unref);

  for (GList *l = all; l != NULL; l = l->next)
    {
      GAppInfo *ai = l->data;

      if (!G_IS_DESKTOP_APP_INFO (ai))
        continue;
      /* NoDisplay, Hidden, OnlyShowIn/NotShowIn for the current desktop */
      if (!g_app_info_should_show (ai))
        continue;
      if (g_app_info_get_id (ai) == NULL)
        continue;
      g_ptr_array_add (out, g_object_ref (ai));
    }
  g_list_free_full (all, g_object_unref);

  g_task_return_pointer (task, out, (GDestroyNotify) g_ptr_array_unref);
}

static void index_add (PageState *st, const char *key, AppsEntry *e);
static void update_outer_stack (PageState *st);

static void
rebuild_index (PageState *st)
{
  g_hash_table_remove_all (st->by_key);
  for (guint i = 0; i < st->entries->len; i++)
    {
      AppsEntry *e = g_ptr_array_index (st->entries, i);
      index_add (st, e->exec_key, e);
      index_add (st, e->id_stem, e);
      index_add (st, e->wm_class, e);
    }
}

static void
index_add (PageState *st, const char *key, AppsEntry *e)
{
  GPtrArray *arr;

  if (key == NULL || *key == '\0')
    return;
  arr = g_hash_table_lookup (st->by_key, key);
  if (arr == NULL)
    {
      arr = g_ptr_array_new ();
      g_hash_table_insert (st->by_key, g_strdup (key), arr);
    }
  if (!g_ptr_array_find (arr, e, NULL))
    g_ptr_array_add (arr, e);
}

static void applications_update_resources (PageState *st);
static void
merge_discovered (PageState *st, GPtrArray *infos)
{
  GPtrArray *added = g_ptr_array_new ();
  guint gen = ++st->generation;

  for (guint i = 0; i < infos->len; i++)
    {
      GDesktopAppInfo *d = g_ptr_array_index (infos, i);
      const char *id = g_app_info_get_id (G_APP_INFO (d));
      AppsEntry *e = g_hash_table_lookup (st->by_id, id);

      if (e == NULL)
        {
          e = entry_new (d);
          g_ptr_array_add (st->entries, e);       /* owns the reference */
          g_hash_table_insert (st->by_id, g_strdup (e->id), e);
          g_ptr_array_add (added, g_object_ref (e));
        }
      else
        {
          entry_set_info (e, d);                  /* metadata changed on disk */
        }
      e->seen_gen = gen;
    }

  /* applications that disappeared from the desktop metadata */
  for (guint i = st->entries->len; i > 0; i--)
    {
      AppsEntry *e = g_ptr_array_index (st->entries, i - 1);
      guint pos;

      if (e->seen_gen == gen)
        continue;
      if (g_list_store_find (st->store, e, &pos))
        g_list_store_remove (st->store, pos);
      if (st->d_graph_entry == e)
        st->d_graph_entry = NULL;
      g_hash_table_remove (st->by_id, e->id);
      g_ptr_array_remove_index (st->entries, i - 1);
    }

  if (added->len > 0)
    g_list_store_splice (st->store, g_list_model_get_n_items (G_LIST_MODEL (st->store)),
                         0, added->pdata, added->len);
  g_ptr_array_set_free_func (added, g_object_unref);
  g_ptr_array_unref (added);

  rebuild_index (st);

  /* metadata may have changed: refresh sorting and search */
  gtk_filter_changed (GTK_FILTER (st->filter), GTK_FILTER_CHANGE_DIFFERENT);
  gtk_sorter_changed (GTK_SORTER (st->sorter), GTK_SORTER_CHANGE_DIFFERENT);

  applications_update_resources (st);
}

static void
discover_done (GObject *source, GAsyncResult *res, gpointer unused)
{
  PageState *st = state_of (GTK_WIDGET (source));
  GPtrArray *infos;
  g_autoptr (GError) error = NULL;

  infos = g_task_propagate_pointer (G_TASK (res), &error);
  st->discovering = FALSE;

  if (infos == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      return;
    }

  merge_discovered (st, infos);
  g_ptr_array_unref (infos);
  st->loaded = TRUE;
  update_outer_stack (st);
}

/* Scan desktop entries off the main thread. */
static void
applications_discover (PageState *st)
{
  g_autoptr (GTask) task = NULL;

  if (st->discovering)
    return;
  st->discovering = TRUE;

  /* The task holds a reference on the page root, so `st` outlives it. */
  task = g_task_new (st->root, st->cancel, discover_done, NULL);
  g_task_set_return_on_cancel (task, FALSE);
  g_task_run_in_thread (task, discover_thread);
}

static gboolean
rediscover_timeout (gpointer data)
{
  PageState *st = data;

  st->rediscover_id = 0;
  applications_discover (st);
  return G_SOURCE_REMOVE;
}

static void
on_apps_changed (GAppInfoMonitor *monitor, gpointer data)
{
  PageState *st = data;

  /* installs often touch many files; coalesce */
  if (st->rediscover_id == 0)
    st->rediscover_id = g_timeout_add (600, rediscover_timeout, st);
}

/* ========================================================================== *
 * Matching processes to applications
 * ========================================================================== */

static char *
base_key (const char *path)
{
  g_autofree char *b = NULL;

  if (path == NULL || *path == '\0')
    return NULL;
  b = g_path_get_basename (path);
  return g_utf8_strdown (b, -1);
}

static AppsEntry *
lookup_best (PageState *st, const char *key, const ApplicationsProc *p)
{
  GPtrArray *cands = g_hash_table_lookup (st->by_key, key);
  AppsEntry *best = NULL;
  int best_score = -1;

  if (cands == NULL)
    return NULL;

  for (guint i = 0; i < cands->len; i++)
    {
      AppsEntry *e = g_ptr_array_index (cands, i);
      int score = 1;

      /* Several desktop entries can share one executable
       * (e.g. "firefox" and "firefox --private-window"): prefer the entry
       * whose own arguments agree with the process's arguments. */
      if (e->exec_argv != NULL && e->exec_argv[0] != NULL)
        {
          if (p->exe != NULL && strcmp (e->exec_argv[0], p->exe) == 0)
            score += 10;
          if (p->argv != NULL && p->argv[0] != NULL)
            {
              guint k = 1;
              while (e->exec_argv[k] != NULL && p->argv[k] != NULL &&
                     strcmp (e->exec_argv[k], p->argv[k]) == 0)
                { score++; k++; }
              if (e->exec_argv[k] != NULL && k == 1 && p->argv[1] == NULL &&
                  e->exec_argv[1] != NULL)
                score--;      /* entry demands arguments the process lacks */
            }
        }
      if (e->id_stem != NULL && strcmp (e->id_stem, key) == 0)
        score += 5;

      if (score > best_score)
        {
          best_score = score;
          best = e;
        }
    }
  return best;
}

/* systemd/GNOME/Flatpak place each application in "app-<launcher>-<id>-<n>.scope" */
static AppsEntry *
match_cgroup (PageState *st, const ApplicationsProc *p)
{
  static const char *const launchers[] = {
    "", "gnome-", "kde-", "flatpak-", "snap-", "xdg-", "uwsm-", "dbus-", NULL
  };
  const char *last;
  g_autofree char *s = NULL;
  g_auto (GStrv) parts = NULL;
  g_autofree char *joined = NULL;
  AppsEntry *hit = NULL;

  if (p->cgroup == NULL)
    return NULL;
  last = strrchr (p->cgroup, '/');
  last = last != NULL ? last + 1 : p->cgroup;
  if (!g_str_has_prefix (last, "app-"))
    return NULL;

  s = g_strdup (last + 4);
  if (g_str_has_suffix (s, ".scope"))
    s[strlen (s) - 6] = '\0';
  else if (g_str_has_suffix (s, ".service"))
    s[strlen (s) - 8] = '\0';
  {
    char *at = strchr (s, '@');
    if (at != NULL)
      *at = '\0';
  }

  parts = g_strsplit (s, "\\x2d", -1);      /* systemd escapes '-' */
  {
    g_autofree char *tmp = g_strjoinv ("-", parts);
    joined = g_utf8_strdown (tmp, -1);
  }

  for (guint i = 0; launchers[i] != NULL && hit == NULL; i++)
    {
      const char *cand = joined;
      g_autofree char *c = NULL;
      char *dash;

      if (*launchers[i] != '\0')
        {
          if (!g_str_has_prefix (joined, launchers[i]))
            continue;
          cand = joined + strlen (launchers[i]);
        }
      c = g_strdup (cand);
      for (;;)
        {
          GPtrArray *arr = g_hash_table_lookup (st->by_key, c);
          if (arr != NULL && arr->len > 0)
            { hit = g_ptr_array_index (arr, 0); break; }
          dash = strrchr (c, '-');            /* strip "-<pid>" suffixes */
          if (dash == NULL || dash == c)
            break;
          *dash = '\0';
        }
    }
  return hit;
}

static gboolean
key_is_generic (const char *k)
{
  return k == NULL || is_interpreter (k);
}

static AppsEntry *
match_one (PageState *st, const ApplicationsProc *p)
{
  AppsEntry *e = match_cgroup (st, p);
  g_autofree char *k_exe = NULL, *k_arg0 = NULL, *k_name = NULL;
  const char *keys[4];
  guint n = 0;

  if (e != NULL)
    return e;

  k_exe = base_key (p->exe);
  k_arg0 = (p->argv != NULL) ? base_key (p->argv[0]) : NULL;
  k_name = p->name != NULL ? g_utf8_strdown (p->name, -1) : NULL;

  if (k_exe != NULL && !key_is_generic (k_exe))
    keys[n++] = k_exe;
  if (k_arg0 != NULL && !key_is_generic (k_arg0))
    keys[n++] = k_arg0;
  if (k_name != NULL && !key_is_generic (k_name))
    keys[n++] = k_name;

  /* interpreter-hosted apps: python3 /usr/bin/foo.py */
  g_autofree char *k_script = NULL;
  if (k_arg0 != NULL && is_interpreter (k_arg0) && p->argv != NULL)
    {
      guint j = 1;
      while (p->argv[j] != NULL && p->argv[j][0] == '-')
        j++;
      if (p->argv[j] != NULL)
        {
          g_autofree char *b = g_path_get_basename (p->argv[j]);
          char *dot = strrchr (b, '.');
          if (dot != NULL && dot != b)
            *dot = '\0';
          k_script = g_utf8_strdown (b, -1);
          keys[n++] = k_script;
        }
    }

  for (guint i = 0; i < n; i++)
    {
      e = lookup_best (st, keys[i], p);
      if (e != NULL)
        return e;
      /* "firefox-bin" etc. */
      if (g_str_has_suffix (keys[i], "-bin"))
        {
          g_autofree char *trim = g_strndup (keys[i], strlen (keys[i]) - 4);
          e = lookup_best (st, trim, p);
          if (e != NULL)
            return e;
        }
    }
  return NULL;
}

/* Fill each entry's accumulators from the backend's current process list. */
static void
applications_match_processes (PageState *st, GPtrArray *procs)
{
  GHashTable *by_pid = g_hash_table_new (g_direct_hash, g_direct_equal);
  GHashTable *owner  = g_hash_table_new (g_direct_hash, g_direct_equal);

  for (guint i = 0; i < st->entries->len; i++)
    {
      AppsEntry *e = g_ptr_array_index (st->entries, i);
      e->acc_n = 0;
      e->acc_cpu = 0;
      e->acc_mem = 0;
      e->acc_start = 0;
      e->acc_user = NULL;
      g_array_set_size (e->acc_pids, 0);
    }

  for (guint i = 0; i < procs->len; i++)
    {
      const ApplicationsProc *p = g_ptr_array_index (procs, i);
      g_hash_table_insert (by_pid, GINT_TO_POINTER (p->pid), (gpointer) p);
    }

  /* pass 1: direct matches */
  for (guint i = 0; i < procs->len; i++)
    {
      const ApplicationsProc *p = g_ptr_array_index (procs, i);
      AppsEntry *e = match_one (st, p);
      if (e != NULL)
        g_hash_table_insert (owner, GINT_TO_POINTER (p->pid), e);
    }

  /* pass 2: helper processes inherit from the nearest matched ancestor */
  for (guint i = 0; i < procs->len; i++)
    {
      const ApplicationsProc *p = g_ptr_array_index (procs, i);
      const ApplicationsProc *cur = p;
      AppsEntry *e = g_hash_table_lookup (owner, GINT_TO_POINTER (p->pid));

      for (guint depth = 0; e == NULL && depth < 16; depth++)
        {
          const ApplicationsProc *parent =
            g_hash_table_lookup (by_pid, GINT_TO_POINTER (cur->ppid));
          if (parent == NULL || parent->pid <= 1)
            break;
          e = g_hash_table_lookup (owner, GINT_TO_POINTER (parent->pid));
          cur = parent;
        }
      if (e == NULL)
        continue;

      e->acc_n++;
      e->acc_cpu += p->cpu_percent;
      e->acc_mem += p->rss_bytes;
      if (p->start_time_us > 0 &&
          (e->acc_start == 0 || p->start_time_us < e->acc_start))
        {
          e->acc_start = p->start_time_us;
          e->acc_user = p->user;
        }
      else if (e->acc_user == NULL)
        e->acc_user = p->user;
      {
        pid_t pid = p->pid;
        g_array_append_val (e->acc_pids, pid);
      }
    }

  g_hash_table_unref (owner);
  g_hash_table_unref (by_pid);
}

/* ========================================================================== *
 * Running state, resources, history
 * ========================================================================== */

static void
history_push (GArray *a, double v)
{
  if (a->len >= HIST_LEN)
    g_array_remove_index (a, 0);
  g_array_append_val (a, v);
}

static gboolean
sort_keys_stale (AppsEntry *e)
{
  double mem_step = MAX ((double) SORT_MEM_STEP, e->skey_mem * 0.10);

  return e->skey_running != e->running ||
         e->skey_procs != e->nprocs ||
         fabs (e->cpu - e->skey_cpu) >= SORT_CPU_STEP ||
         fabs ((double) e->mem - (double) e->skey_mem) >= mem_step;
}

static void applications_select (PageState *st);
static void detail_refresh (PageState *st, AppsEntry *e, gboolean push_history);

static void
update_counts (PageState *st)
{
  guint running = 0, multi = 0;
  g_autofree char *text = NULL;

  for (guint i = 0; i < st->entries->len; i++)
    {
      AppsEntry *e = g_ptr_array_index (st->entries, i);
      if (e->running)
        {
          running++;
          if (e->nprocs > 1)
            multi++;
        }
    }

  if (multi > 0)
    text = g_strdup_printf ("%u installed · %u running · %u with several processes",
                            st->entries->len, running, multi);
  else
    text = g_strdup_printf ("%u installed · %u running", st->entries->len, running);

  if (g_strcmp0 (gtk_label_get_text (GTK_LABEL (st->counts)), text) != 0)
    gtk_label_set_text (GTK_LABEL (st->counts), text);
}

/* Commit accumulators to the visible state; returns TRUE if widgets must update. */
static gboolean
commit_entry (AppsEntry *e, gboolean *need_refilter, gboolean *need_resort)
{
  gboolean running = e->acc_n > 0;
  gboolean changed = FALSE;

  if (running != e->running)
    {
      e->running = running;
      changed = TRUE;
      *need_refilter = TRUE;
      if (running)
        {
          /* history policy: reset when the application starts again */
          g_array_set_size (e->cpu_hist, 0);
          g_array_set_size (e->mem_hist, 0);
        }
      else
        {
          /* exited: drop stale process associations, keep history */
          g_array_set_size (e->pids, 0);
        }
    }

  if (running)
    {
      if (e->nprocs != e->acc_n ||
          fabs (e->cpu - e->acc_cpu) >= CPU_EPS ||
          (e->mem > e->acc_mem ? e->mem - e->acc_mem : e->acc_mem - e->mem) >= MEM_EPS ||
          g_strcmp0 (e->user, e->acc_user) != 0 ||
          e->start_time_us != e->acc_start)
        changed = TRUE;

      e->nprocs = e->acc_n;
      e->cpu = e->acc_cpu;
      e->mem = e->acc_mem;
      e->start_time_us = e->acc_start;
      if (g_strcmp0 (e->user, e->acc_user) != 0)
        {
          g_free (e->user);
          e->user = g_strdup (e->acc_user);
        }
      g_array_set_size (e->pids, 0);
      g_array_append_vals (e->pids, e->acc_pids->data, e->acc_pids->len);

      history_push (e->cpu_hist, e->cpu);
      history_push (e->mem_hist, (double) e->mem / (1024.0 * 1024.0));
    }
  else if (changed)
    {
      e->nprocs = 0;
      e->cpu = 0;
      e->mem = 0;
    }

  if (changed && sort_keys_stale (e))
    {
      e->skey_running = e->running;
      e->skey_procs = e->nprocs;
      e->skey_cpu = e->cpu;
      e->skey_mem = e->mem;
      *need_resort = TRUE;
    }
  return changed;
}

/* applications_update_running_state() + applications_update_resources() */
static void
applications_update_resources (PageState *st)
{
  GPtrArray *procs = backend_processes (st->app);
  gboolean need_refilter = FALSE, need_resort = FALSE;
  AppsEntry *selected;

  if (procs == NULL)
    return;

  applications_match_processes (st, procs);

  for (guint i = 0; i < st->entries->len; i++)
    {
      AppsEntry *e = g_ptr_array_index (st->entries, i);

      if (commit_entry (e, &need_refilter, &need_resort))
        entry_emit_updated (e);               /* only bound widgets redraw */
    }

  if (need_refilter &&
      (st->filter_mode == FILTER_RUNNING || st->filter_mode == FILTER_STOPPED))
    gtk_filter_changed (GTK_FILTER (st->filter), GTK_FILTER_CHANGE_DIFFERENT);
  if (need_resort && st->sort_mode != SORT_NAME)
    gtk_sorter_changed (GTK_SORTER (st->sorter), GTK_SORTER_CHANGE_DIFFERENT);

  update_counts (st);

  selected = gtk_single_selection_get_selected_item (st->sel);
  if (selected != NULL)
    detail_refresh (st, selected, TRUE);
}

static void
on_backend_refresh (App *app, gpointer data)
{
  applications_update_resources (data);
}

static void
applications_start_monitoring (PageState *st)
{
  st->refresh_id = backend_connect (st->app, on_backend_refresh, st);
  st->monitor = g_app_info_monitor_get ();
  g_signal_connect (st->monitor, "changed", G_CALLBACK (on_apps_changed), st);
}

/* ========================================================================== *
 * Search / filter / sort
 * ========================================================================== */

static gboolean
filter_func (gpointer item, gpointer data)
{
  PageState *st = data;
  AppsEntry *e = item;

  switch (st->filter_mode)
    {
    case FILTER_ALL:     break;
    case FILTER_RUNNING: if (!e->running) return FALSE; break;
    case FILTER_STOPPED: if (e->running) return FALSE; break;
    case FILTER_DEV:     if (!(e->cat_mask & CAT_DEV)) return FALSE; break;
    case FILTER_NET:     if (!(e->cat_mask & CAT_NET)) return FALSE; break;
    case FILTER_MEDIA:   if (!(e->cat_mask & CAT_MEDIA)) return FALSE; break;
    case FILTER_GFX:     if (!(e->cat_mask & CAT_GFX)) return FALSE; break;
    case FILTER_SYS:     if (!(e->cat_mask & CAT_SYS)) return FALSE; break;
    case FILTER_UTIL:    if (!(e->cat_mask & CAT_UTIL)) return FALSE; break;
    }

  if (st->tokens != NULL)
    for (guint i = 0; st->tokens[i] != NULL; i++)
      if (strstr (e->haystack, st->tokens[i]) == NULL)
        return FALSE;
  return TRUE;
}

static int
cmp_double_desc (double a, double b)
{
  return (a < b) - (a > b);
}

static int
sort_func (gconstpointer pa, gconstpointer pb, gpointer data)
{
  PageState *st = data;
  const AppsEntry *a = pa, *b = pb;
  int r = 0;

  switch (st->sort_mode)
    {
    case SORT_RUNNING: r = (int) b->skey_running - (int) a->skey_running; break;
    case SORT_CPU:     r = cmp_double_desc (a->skey_cpu, b->skey_cpu); break;
    case SORT_MEM:     r = (a->skey_mem < b->skey_mem) - (a->skey_mem > b->skey_mem); break;
    case SORT_PROCS:   r = (int) b->skey_procs - (int) a->skey_procs; break;
    case SORT_NAME:    break;
    }
  /* total, deterministic tie-breakers keep the order stable */
  if (r == 0)
    r = strcmp (a->name_key, b->name_key);
  if (r == 0)
    r = strcmp (a->id, b->id);
  return r;
}

/* applications_search(): called with the raw text of the search entry */
static void
applications_search (PageState *st, const char *text)
{
  g_autofree char *folded = g_utf8_casefold (text != NULL ? text : "", -1);

  g_strstrip (folded);
  g_strfreev (st->tokens);
  st->tokens = (*folded != '\0') ? g_strsplit_set (folded, " \t", -1) : NULL;
  if (st->tokens != NULL)
    {
      /* remove empty tokens produced by repeated spaces */
      guint w = 0;
      for (guint r = 0; st->tokens[r] != NULL; r++)
        {
          if (*st->tokens[r] == '\0')
            g_free (st->tokens[r]);
          else
            st->tokens[w++] = st->tokens[r];
        }
      st->tokens[w] = NULL;
    }
  gtk_filter_changed (GTK_FILTER (st->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static void
applications_filter (PageState *st, FilterMode mode)
{
  st->filter_mode = mode;
  gtk_filter_changed (GTK_FILTER (st->filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static void
applications_sort (PageState *st, SortMode mode)
{
  st->sort_mode = mode;
  gtk_sorter_changed (GTK_SORTER (st->sorter), GTK_SORTER_CHANGE_DIFFERENT);
}

static FilterMode
filter_mode_for_index (PageState *st, guint idx)
{
  (void)st;
  static const FilterMode modes[] = {
    FILTER_ALL, FILTER_RUNNING, FILTER_STOPPED, FILTER_DEV,
    FILTER_NET, FILTER_MEDIA, FILTER_GFX, FILTER_SYS, FILTER_UTIL
  };
  return idx < G_N_ELEMENTS(modes) ? modes[idx] : FILTER_ALL;
}

static guint
filter_index_for_mode (PageState *st, FilterMode mode)
{
  (void)st;
  static const FilterMode modes[] = {
    FILTER_ALL, FILTER_RUNNING, FILTER_STOPPED, FILTER_DEV,
    FILTER_NET, FILTER_MEDIA, FILTER_GFX, FILTER_SYS, FILTER_UTIL
  };
  for (guint i = 0; i < G_N_ELEMENTS(modes); ++i)
    if (modes[i] == mode) return i;
  return 0;
}

/* ========================================================================== *
 * Actions: launch / focus / terminate
 * ========================================================================== */

typedef struct {
  GtkWidget *root;     /* strong ref */
  AppsEntry *entry;    /* strong ref */
} ActionData;

static void
action_data_free (ActionData *d)
{
  g_object_unref (d->root);
  g_object_unref (d->entry);
  g_free (d);
}

static void
toast (GtkWidget *root, const char *msg)
{
  AdwToast *t = adw_toast_new (msg);

  adw_toast_set_timeout (t, 5);
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (root), t);
}

static void
launch_done (GObject *src, GAsyncResult *res, gpointer data)
{
  ActionData *d = data;
  g_autoptr (GError) error = NULL;

  if (!g_app_info_launch_uris_finish (G_APP_INFO (src), res, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_autofree char *msg =
            g_strdup_printf ("Couldn't start %s: %s", d->entry->name, error->message);
          toast (d->root, msg);
        }
    }
  else
    {
    }
  action_data_free (d);
}

/* Goes through GIO / the desktop launch machinery: no shell, honours
 * Terminal=, Path=, StartupNotify= and DBusActivatable= (which also makes
 * a second launch focus the existing instance). */
static void
applications_launch (PageState *st, AppsEntry *e)
{
  GdkAppLaunchContext *ctx;
  ActionData *d;

  if (e->dinfo == NULL)
    return;

  ctx = gdk_display_get_app_launch_context (gtk_widget_get_display (st->root));
  d = g_new0 (ActionData, 1);
  d->root = g_object_ref (st->root);
  d->entry = g_object_ref (e);

  g_app_info_launch_uris_async (G_APP_INFO (e->dinfo), NULL,
                                G_APP_LAUNCH_CONTEXT (ctx),
                                st->cancel, launch_done, d);
  g_object_unref (ctx);
}

static void
terminate_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
  ActionData *d = data;

  if (strcmp (response, "terminate") == 0)
    {
      guint failed = 0;

      for (guint i = 0; i < d->entry->pids->len; i++)
        {
          pid_t pid = g_array_index (d->entry->pids, pid_t, i);
          if (kill (pid, SIGTERM) != 0 && errno != ESRCH)
            failed++;
        }
      if (failed > 0)
        {
          g_autofree char *msg =
            g_strdup_printf ("Couldn't signal %u process%s of %s",
                             failed, failed == 1 ? "" : "es", d->entry->name);
          toast (d->root, msg);
        }
    }
  action_data_free (d);
}

static void
applications_terminate (PageState *st, AppsEntry *e)
{
  AdwDialog *dlg;
  ActionData *d;
  g_autofree char *heading = g_strdup_printf ("Quit %s?", e->name);
  g_autofree char *body =
    g_strdup_printf ("A termination request will be sent to %u process%s. "
                     "Unsaved work may be lost.",
                     e->pids->len, e->pids->len == 1 ? "" : "es");

  if (!e->running || e->pids->len == 0)
    return;

  dlg = adw_alert_dialog_new (heading, body);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                  "cancel", "Cancel",
                                  "terminate", "Quit Application", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg), "terminate",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "cancel");

  d = g_new0 (ActionData, 1);
  d->root = g_object_ref (st->root);
  d->entry = g_object_ref (e);
  g_signal_connect (dlg, "response", G_CALLBACK (terminate_response), d);
  adw_dialog_present (dlg, st->root);
}

/* ========================================================================== *
 * Cell helpers (GtkListItem, and GtkColumnViewCell where it exists)
 * ========================================================================== */

static gpointer
cell_get_item (GObject *cell)
{
#if GTK_CHECK_VERSION (4, 12, 0)
  if (GTK_IS_COLUMN_VIEW_CELL (cell))
    return gtk_column_view_cell_get_item (GTK_COLUMN_VIEW_CELL (cell));
#endif
  return gtk_list_item_get_item (GTK_LIST_ITEM (cell));
}

static void
cell_set_child (GObject *cell, GtkWidget *child)
{
#if GTK_CHECK_VERSION (4, 12, 0)
  if (GTK_IS_COLUMN_VIEW_CELL (cell))
    {
      gtk_column_view_cell_set_child (GTK_COLUMN_VIEW_CELL (cell), child);
      return;
    }
#endif
  gtk_list_item_set_child (GTK_LIST_ITEM (cell), child);
}

static GtkWidget *
cell_get_child (GObject *cell)
{
#if GTK_CHECK_VERSION (4, 12, 0)
  if (GTK_IS_COLUMN_VIEW_CELL (cell))
    return gtk_column_view_cell_get_child (GTK_COLUMN_VIEW_CELL (cell));
#endif
  return gtk_list_item_get_child (GTK_LIST_ITEM (cell));
}

/* ========================================================================== *
 * Grid cards
 * ========================================================================== */

typedef struct {
  GtkWidget *icon, *name, *desc, *state_row, *state_icon, *state, *res;
} CardWidgets;

static void
card_refresh (AppsEntry *e, gpointer box)
{
  CardWidgets *w = g_object_get_data (G_OBJECT (box), "cw");
  g_autofree char *state = fmt_state (e);
  const char *desc = short_description (e);

  gtk_image_set_from_gicon (GTK_IMAGE (w->icon), e->icon);
  gtk_label_set_text (GTK_LABEL (w->name), e->name);
  gtk_label_set_text (GTK_LABEL (w->desc), desc);
  gtk_widget_set_tooltip_text (box, *desc ? desc : e->name);
  gtk_label_set_text (GTK_LABEL (w->state), state);
  gtk_widget_set_visible (w->state_icon, e->running);

  if (e->running)
    {
      g_autofree char *cpu = fmt_cpu (e->cpu);
      g_autofree char *mem = fmt_mem (e->mem);
      g_autofree char *res = g_strdup_printf ("CPU %s · %s", cpu, mem);

      gtk_label_set_text (GTK_LABEL (w->res), res);
      gtk_widget_add_css_class (box, "apps-running");
      gtk_widget_remove_css_class (w->state, "dim-label");
    }
  else
    {
      gtk_label_set_text (GTK_LABEL (w->res), "");
      gtk_widget_remove_css_class (box, "apps-running");
      gtk_widget_add_css_class (w->state, "dim-label");
    }
}

static void
card_setup (GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  CardWidgets *w = g_new0 (CardWidgets, 1);

  gtk_widget_add_css_class (box, "apps-card");
  gtk_widget_set_size_request (box, 150, -1);

  w->icon = gtk_image_new ();
  gtk_image_set_pixel_size (GTK_IMAGE (w->icon), 64);
  gtk_widget_set_margin_bottom (w->icon, 6);

  w->name = gtk_label_new (NULL);
  gtk_widget_add_css_class (w->name, "apps-name");
  gtk_label_set_ellipsize (GTK_LABEL (w->name), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (w->name), 18);

  w->desc = gtk_label_new (NULL);
  gtk_widget_add_css_class (w->desc, "caption");
  gtk_widget_add_css_class (w->desc, "dim-label");
  gtk_label_set_ellipsize (GTK_LABEL (w->desc), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (w->desc), 22);
  gtk_widget_set_size_request (w->desc, -1, 16);

  w->state_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_halign (w->state_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (w->state_row, 4);
  w->state_icon = gtk_image_new_from_icon_name ("media-playback-start-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (w->state_icon), 12);
  w->state = gtk_label_new (NULL);
  gtk_widget_add_css_class (w->state, "caption-heading");
  gtk_label_set_ellipsize (GTK_LABEL (w->state), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (w->state_row), w->state_icon);
  gtk_box_append (GTK_BOX (w->state_row), w->state);

  w->res = gtk_label_new (NULL);
  gtk_widget_add_css_class (w->res, "caption");
  gtk_widget_add_css_class (w->res, "apps-res");
  gtk_label_set_ellipsize (GTK_LABEL (w->res), PANGO_ELLIPSIZE_END);
  gtk_widget_set_size_request (w->res, -1, 16);

  gtk_box_append (GTK_BOX (box), w->icon);
  gtk_box_append (GTK_BOX (box), w->name);
  gtk_box_append (GTK_BOX (box), w->desc);
  gtk_box_append (GTK_BOX (box), w->state_row);
  gtk_box_append (GTK_BOX (box), w->res);

  g_object_set_data_full (G_OBJECT (box), "cw", w, g_free);
  cell_set_child (G_OBJECT (item), box);
}

static void
card_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
  AppsEntry *e = gtk_list_item_get_item (item);
  GtkWidget *box = gtk_list_item_get_child (item);

  card_refresh (e, box);
  g_signal_connect (e, "updated", G_CALLBACK (card_refresh), box);
}

static void
card_unbind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
  AppsEntry *e = gtk_list_item_get_item (item);
  GtkWidget *box = gtk_list_item_get_child (item);

  if (e != NULL)
    g_signal_handlers_disconnect_by_data (e, box);
}

/* ========================================================================== *
 * List rows (GtkColumnView cells)
 * ========================================================================== */

typedef enum {
  COL_APP, COL_STATUS, COL_CPU, COL_MEM, COL_PROCS, COL_USER, COL_EXEC
} ColKind;

typedef struct {
  ColKind    kind;
  GtkWidget *icon;   /* COL_APP only */
  GtkWidget *label;
} RowCell;

static void
row_refresh (AppsEntry *e, gpointer widget)
{
  RowCell *c = g_object_get_data (G_OBJECT (widget), "rc");
  g_autofree char *text = NULL;

  switch (c->kind)
    {
    case COL_APP:
      gtk_image_set_from_gicon (GTK_IMAGE (c->icon), e->icon);
      text = g_strdup (e->name);
      break;
    case COL_STATUS:
      text = g_strdup (e->running ? "Running" : "Stopped");
      if (e->running)
        gtk_widget_remove_css_class (c->label, "dim-label");
      else
        gtk_widget_add_css_class (c->label, "dim-label");
      break;
    case COL_CPU:   text = e->running ? fmt_cpu (e->cpu) : g_strdup ("—"); break;
    case COL_MEM:   text = e->running ? fmt_mem (e->mem) : g_strdup ("—"); break;
    case COL_PROCS: text = e->running ? g_strdup_printf ("%u", e->nprocs) : g_strdup ("—"); break;
    case COL_USER:  text = g_strdup (e->running && e->user ? e->user : "—"); break;
    case COL_EXEC:
      text = g_strdup (e->exec_argv && e->exec_argv[0] ? e->exec_argv[0] : "—");
      break;
    }
  gtk_label_set_text (GTK_LABEL (c->label), text);
  if (c->kind == COL_EXEC)
    gtk_widget_set_tooltip_text (c->label, e->exec_line);
}

static void
row_setup (GtkSignalListItemFactory *f, GObject *cell, gpointer data)
{
  RowCell *c = g_new0 (RowCell, 1);
  GtkWidget *w;

  c->kind = GPOINTER_TO_INT (data);
  c->label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (c->label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (c->label), PANGO_ELLIPSIZE_END);

  if (c->kind == COL_APP)
    {
      w = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
      c->icon = gtk_image_new ();
      gtk_image_set_pixel_size (GTK_IMAGE (c->icon), 32);
      gtk_box_append (GTK_BOX (w), c->icon);
      gtk_widget_set_hexpand (c->label, TRUE);
      gtk_box_append (GTK_BOX (w), c->label);
      gtk_widget_set_margin_top (w, 4);
      gtk_widget_set_margin_bottom (w, 4);
    }
  else
    {
      w = c->label;
      gtk_widget_set_margin_top (w, 8);
      gtk_widget_set_margin_bottom (w, 8);
      if (c->kind == COL_CPU || c->kind == COL_MEM || c->kind == COL_PROCS)
        {
          gtk_label_set_xalign (GTK_LABEL (w), 1.0);
          gtk_widget_add_css_class (w, "apps-res");
        }
    }

  g_object_set_data_full (G_OBJECT (w), "rc", c, g_free);
  cell_set_child (cell, w);
}

static void
row_bind (GtkSignalListItemFactory *f, GObject *cell, gpointer data)
{
  AppsEntry *e = cell_get_item (cell);
  GtkWidget *w = cell_get_child (cell);

  row_refresh (e, w);
  g_signal_connect (e, "updated", G_CALLBACK (row_refresh), w);
}

static void
row_unbind (GtkSignalListItemFactory *f, GObject *cell, gpointer data)
{
  AppsEntry *e = cell_get_item (cell);
  GtkWidget *w = cell_get_child (cell);

  if (e != NULL)
    g_signal_handlers_disconnect_by_data (e, w);
}

static GtkColumnViewColumn *
add_column (GtkColumnView *view, const char *title, ColKind kind,
            int fixed_width, gboolean expand)
{
  GtkListItemFactory *f = gtk_signal_list_item_factory_new ();
  GtkColumnViewColumn *col;

  g_signal_connect (f, "setup", G_CALLBACK (row_setup), GINT_TO_POINTER (kind));
  g_signal_connect (f, "bind", G_CALLBACK (row_bind), NULL);
  g_signal_connect (f, "unbind", G_CALLBACK (row_unbind), NULL);

  col = gtk_column_view_column_new (title, f);
  gtk_column_view_column_set_resizable (col, TRUE);
  if (fixed_width > 0)
    gtk_column_view_column_set_fixed_width (col, fixed_width);
  gtk_column_view_column_set_expand (col, expand);
  gtk_column_view_append_column (view, col);
  g_object_unref (col);
  return col;
}

/* ========================================================================== *
 * Detail panel
 * ========================================================================== */

static GtkWidget *
detail_row (GtkWidget *group, const char *title)
{
  GtkWidget *row = adw_action_row_new ();

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), "—");
  adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), TRUE);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);
  return row;
}

static void
set_row (GtkWidget *row, const char *text)
{
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row),
                               (text != NULL && *text) ? text : "—");
}

static void
graphs_replay (PageState *st, AppsEntry *e)
{
  graph_clear (st->d_cpu_graph);
  graph_clear (st->d_mem_graph);
  for (guint i = 0; i < e->cpu_hist->len; i++)
    graph_push (st->d_cpu_graph, g_array_index (e->cpu_hist, double, i));
  for (guint i = 0; i < e->mem_hist->len; i++)
    graph_push (st->d_mem_graph, g_array_index (e->mem_hist, double, i));
  st->d_graph_entry = e;
}

static void
detail_refresh (PageState *st, AppsEntry *e, gboolean push_history)
{
  g_autofree char *cpu = NULL, *mem = NULL, *procs = NULL, *started = NULL;
  g_autofree char *cats = NULL;

  gtk_image_set_from_gicon (GTK_IMAGE (st->d_icon), e->icon);
  gtk_label_set_text (GTK_LABEL (st->d_name), e->name);
  gtk_label_set_text (GTK_LABEL (st->d_desc),
                      *short_description (e) ? short_description (e) : "No description");

  set_row (st->d_row_file, e->desktop_path);
  set_row (st->d_row_exec, e->exec_line);
  if (e->categories != NULL)
    cats = g_strdelimit (g_strdup (e->categories), ";", ',');
  if (cats != NULL)
    {
      size_t n = strlen (cats);
      if (n > 0 && cats[n - 1] == ',')
        cats[n - 1] = '\0';
    }
  set_row (st->d_row_cats, cats);

  set_row (st->d_row_state, e->running ? "Running" : "Not running");
  if (e->running)
    {
      g_autoptr (GDateTime) dt = e->start_time_us > 0
        ? g_date_time_new_from_unix_local (e->start_time_us / G_USEC_PER_SEC) : NULL;

      procs = g_strdup_printf ("%u", e->nprocs);
      cpu = fmt_cpu (e->cpu);
      mem = fmt_mem (e->mem);
      if (dt != NULL)
        started = g_date_time_format (dt, "%e %b, %H:%M");
    }
  set_row (st->d_row_procs, procs);
  set_row (st->d_row_cpu, cpu);
  set_row (st->d_row_mem, mem);
  set_row (st->d_row_start, started);
  set_row (st->d_row_user, e->running ? e->user : NULL);

  gtk_widget_set_sensitive (st->d_btn_quit,
                            e->running && e->pids->len > 0 &&
                            g_strcmp0 (e->user, g_get_user_name ()) == 0);
  gtk_widget_set_sensitive (st->d_btn_focus, e->running && e->dbus_activatable);
  gtk_widget_set_tooltip_text (st->d_btn_focus,
                               e->dbus_activatable
                                 ? "Bring the running instance to the front"
                                 : "This application doesn't support activating an existing instance");

  if (st->d_graph_entry != e)
    graphs_replay (st, e);
  else if (push_history && e->running && e->cpu_hist->len > 0)
    {
      graph_push (st->d_cpu_graph, g_array_index (e->cpu_hist, double, e->cpu_hist->len - 1));
      graph_push (st->d_mem_graph, g_array_index (e->mem_hist, double, e->mem_hist->len - 1));
    }
}

/* applications_select(): react to selection changes */
static void
applications_select (PageState *st)
{
  AppsEntry *e = gtk_single_selection_get_selected_item (st->sel);

  if (e != NULL)
    {
      st->d_graph_entry = NULL;           /* force a history replay */
      detail_refresh (st, e, FALSE);
    }
  else
    st->d_graph_entry = NULL;

  adw_overlay_split_view_set_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (st->split), e != NULL);
}

static void
on_selection_changed (GObject *obj, GParamSpec *pspec, gpointer data)
{
  applications_select (data);
}

static void
on_detail_launch (GtkButton *b, gpointer data)
{
  PageState *st = data;
  AppsEntry *e = gtk_single_selection_get_selected_item (st->sel);

  if (e != NULL)
    applications_launch (st, e);
}

static void
on_detail_quit (GtkButton *b, gpointer data)
{
  PageState *st = data;
  AppsEntry *e = gtk_single_selection_get_selected_item (st->sel);

  if (e != NULL)
    applications_terminate (st, e);
}

static void
on_detail_close (GtkButton *b, gpointer data)
{
  PageState *st = data;

  gtk_selection_model_unselect_all (GTK_SELECTION_MODEL (st->sel));
}

static GtkWidget *
applications_create_details (PageState *st)
{
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  GtkWidget *top = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *close = gtk_button_new_from_icon_name ("window-close-symbolic");
  GtkWidget *g_id, *g_rt, *g_hist, *actions;

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 18);
  gtk_widget_set_margin_start (box, 16);
  gtk_widget_set_margin_end (box, 16);

  gtk_widget_add_css_class (close, "flat");
  gtk_widget_add_css_class (close, "circular");
  gtk_widget_set_halign (close, GTK_ALIGN_END);
  gtk_widget_set_tooltip_text (close, "Close details");
  g_signal_connect (close, "clicked", G_CALLBACK (on_detail_close), st);

  st->d_icon = gtk_image_new ();
  gtk_image_set_pixel_size (GTK_IMAGE (st->d_icon), 96);
  st->d_name = gtk_label_new (NULL);
  gtk_widget_add_css_class (st->d_name, "title-2");
  gtk_label_set_wrap (GTK_LABEL (st->d_name), TRUE);
  gtk_label_set_justify (GTK_LABEL (st->d_name), GTK_JUSTIFY_CENTER);
  st->d_desc = gtk_label_new (NULL);
  gtk_widget_add_css_class (st->d_desc, "dim-label");
  gtk_label_set_wrap (GTK_LABEL (st->d_desc), TRUE);
  gtk_label_set_justify (GTK_LABEL (st->d_desc), GTK_JUSTIFY_CENTER);

  gtk_box_append (GTK_BOX (top), close);
  gtk_box_append (GTK_BOX (top), st->d_icon);
  gtk_box_append (GTK_BOX (top), st->d_name);
  gtk_box_append (GTK_BOX (top), st->d_desc);

  actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (actions, GTK_ALIGN_CENTER);
  st->d_btn_launch = gtk_button_new_with_label ("Launch");
  gtk_widget_add_css_class (st->d_btn_launch, "suggested-action");
  st->d_btn_focus = gtk_button_new_with_label ("Focus");
  st->d_btn_quit = gtk_button_new_with_label ("Quit");
  gtk_widget_add_css_class (st->d_btn_quit, "destructive-action");
  g_signal_connect (st->d_btn_launch, "clicked", G_CALLBACK (on_detail_launch), st);
  g_signal_connect (st->d_btn_focus, "clicked", G_CALLBACK (on_detail_launch), st);
  g_signal_connect (st->d_btn_quit, "clicked", G_CALLBACK (on_detail_quit), st);
  gtk_box_append (GTK_BOX (actions), st->d_btn_launch);
  gtk_box_append (GTK_BOX (actions), st->d_btn_focus);
  gtk_box_append (GTK_BOX (actions), st->d_btn_quit);

  g_id = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (g_id), "Identity");
  st->d_row_file = detail_row (g_id, "Desktop file");
  st->d_row_exec = detail_row (g_id, "Executable");
  st->d_row_cats = detail_row (g_id, "Categories");

  g_rt = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (g_rt), "Runtime");
  st->d_row_state = detail_row (g_rt, "Status");
  st->d_row_procs = detail_row (g_rt, "Processes");
  st->d_row_cpu = detail_row (g_rt, "CPU");
  st->d_row_mem = detail_row (g_rt, "Memory");
  st->d_row_start = detail_row (g_rt, "Started");
  st->d_row_user = detail_row (g_rt, "User");

  g_hist = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (g_hist), "History");
  adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (g_hist),
                                         "Aggregated across all of the application's processes");
  st->d_cpu_graph = backend_graph_new ("CPU");
  st->d_mem_graph = backend_graph_new ("Memory");
  gtk_widget_set_size_request (st->d_cpu_graph, -1, 90);
  gtk_widget_set_size_request (st->d_mem_graph, -1, 90);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (g_hist), st->d_cpu_graph);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (g_hist), st->d_mem_graph);

  gtk_box_append (GTK_BOX (box), top);
  gtk_box_append (GTK_BOX (box), actions);
  gtk_box_append (GTK_BOX (box), g_rt);
  gtk_box_append (GTK_BOX (box), g_hist);
  gtk_box_append (GTK_BOX (box), g_id);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box);
  return scroller;
}

/* ========================================================================== *
 * Header: title, search, view switcher, filter, sort, counts
 * ========================================================================== */

static void
on_search_changed (GtkSearchEntry *entry, gpointer data)
{
  applications_search (data, gtk_editable_get_text (GTK_EDITABLE (entry)));
}

static void
on_filter_selected (GObject *obj, GParamSpec *pspec, gpointer data)
{
  PageState *st = data;

  if (st->syncing)
    return;
  applications_filter (st, filter_mode_for_index (
      st, gtk_drop_down_get_selected (GTK_DROP_DOWN (obj))));
}

static void
on_sort_selected (GObject *obj, GParamSpec *pspec, gpointer data)
{
  applications_sort (data, gtk_drop_down_get_selected (GTK_DROP_DOWN (obj)));
}

static void
on_view_toggled (GtkToggleButton *btn, gpointer data)
{
  PageState *st = data;

  if (!gtk_toggle_button_get_active (btn))
    return;
  /* models (and therefore search, filter, sort and selection) are shared */
  gtk_stack_set_visible_child_name (GTK_STACK (st->view_stack),
                                    GTK_WIDGET (btn) == st->btn_grid ? "grid" : "list");
}

static GtkWidget *
applications_create_search (PageState *st)
{
  st->search = gtk_search_entry_new ();
  gtk_widget_set_hexpand (st->search, TRUE);
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (st->search),
                                         "Search applications");
  gtk_widget_set_size_request (st->search, 120, -1);
  g_signal_connect (st->search, "search-changed", G_CALLBACK (on_search_changed), st);
  return st->search;
}

static GtkWidget *
applications_create_view_switcher (PageState *st)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class (box, "linked");
  st->btn_grid = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (st->btn_grid), "view-grid-symbolic");
  gtk_widget_set_tooltip_text (st->btn_grid, "Grid view");
  st->btn_list = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (st->btn_list), "view-list-symbolic");
  gtk_widget_set_tooltip_text (st->btn_list, "List view");
  gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (st->btn_list),
                               GTK_TOGGLE_BUTTON (st->btn_grid));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (st->btn_grid), TRUE);
  g_signal_connect (st->btn_grid, "toggled", G_CALLBACK (on_view_toggled), st);
  g_signal_connect (st->btn_list, "toggled", G_CALLBACK (on_view_toggled), st);
  gtk_box_append (GTK_BOX (box), st->btn_grid);
  gtk_box_append (GTK_BOX (box), st->btn_list);
  return box;
}

static GtkWidget *
applications_create_filters (PageState *st)
{
  const char *labels[G_N_ELEMENTS (filter_items) + 1];
  guint n = 0;

  for (guint i = 0; i < G_N_ELEMENTS (filter_items); i++)
    labels[n++] = filter_items[i].label;
  labels[n] = NULL;

  st->filter_list = gtk_string_list_new (NULL);
  for (guint i = 0; i < n; i++)
    gtk_string_list_append (st->filter_list, labels[i]);
  /* the drop-down takes ownership of one reference; we keep our own */
  st->filter_drop = gtk_drop_down_new (G_LIST_MODEL (g_object_ref (st->filter_list)), NULL);
  gtk_widget_set_tooltip_text (st->filter_drop, "Filter");
  g_signal_connect (st->filter_drop, "notify::selected",
                    G_CALLBACK (on_filter_selected), st);
  return st->filter_drop;
}

static GtkWidget *
applications_create_sort (PageState *st)
{
  st->sort_drop = gtk_drop_down_new (G_LIST_MODEL (gtk_string_list_new (sort_labels)), NULL);
  gtk_widget_set_tooltip_text (st->sort_drop, "Sort by");
  g_signal_connect (st->sort_drop, "notify::selected",
                    G_CALLBACK (on_sort_selected), st);
  return st->sort_drop;
}

static GtkWidget *
applications_create_header (PageState *st)
{
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  GtkWidget *title_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *title = gtk_label_new ("Applications");

  gtk_widget_set_margin_top (header, 16);
  gtk_widget_set_margin_bottom (header, 8);
  gtk_widget_set_margin_start (header, 18);
  gtk_widget_set_margin_end (header, 18);

  gtk_widget_add_css_class (title, "title-1");
  gtk_widget_set_halign (title, GTK_ALIGN_START);

  st->counts = gtk_label_new ("");
  gtk_widget_add_css_class (st->counts, "dim-label");
  gtk_widget_set_hexpand (st->counts, TRUE);
  gtk_widget_set_halign (st->counts, GTK_ALIGN_END);
  gtk_widget_set_valign (st->counts, GTK_ALIGN_BASELINE);
  gtk_label_set_ellipsize (GTK_LABEL (st->counts), PANGO_ELLIPSIZE_END);

  gtk_box_append (GTK_BOX (title_row), title);
  gtk_box_append (GTK_BOX (title_row), st->counts);

  gtk_box_append (GTK_BOX (controls), applications_create_search (st));
  gtk_box_append (GTK_BOX (controls), applications_create_view_switcher (st));
  gtk_box_append (GTK_BOX (controls), applications_create_filters (st));
  gtk_box_append (GTK_BOX (controls), applications_create_sort (st));

  gtk_box_append (GTK_BOX (header), title_row);
  gtk_box_append (GTK_BOX (header), controls);
  return header;
}

/* ========================================================================== *
 * Grid and list containers
 * ========================================================================== */

static void
on_activate_position (GtkWidget *view, guint position, gpointer data)
{
  PageState *st = data;
  AppsEntry *e = g_list_model_get_item (G_LIST_MODEL (st->sel), position);

  if (e != NULL)
    {
      applications_launch (st, e);
      g_object_unref (e);
    }
}

static GtkWidget *
applications_create_grid (PageState *st)
{
  GtkListItemFactory *f = gtk_signal_list_item_factory_new ();
  GtkWidget *grid, *scroller;

  g_signal_connect (f, "setup", G_CALLBACK (card_setup), NULL);
  g_signal_connect (f, "bind", G_CALLBACK (card_bind), NULL);
  g_signal_connect (f, "unbind", G_CALLBACK (card_unbind), NULL);

  /* GtkGridView lays out as many equally sized columns as fit the width. */
  grid = gtk_grid_view_new (GTK_SELECTION_MODEL (g_object_ref (st->sel)), f);
  gtk_grid_view_set_min_columns (GTK_GRID_VIEW (grid), 1);
  gtk_grid_view_set_max_columns (GTK_GRID_VIEW (grid), 8);
  gtk_grid_view_set_single_click_activate (GTK_GRID_VIEW (grid), FALSE);
  g_signal_connect (grid, "activate", G_CALLBACK (on_activate_position), st);
  gtk_widget_add_css_class (grid, "apps-grid");

  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), grid);
  gtk_widget_set_vexpand (scroller, TRUE);
  return scroller;
}

static GtkWidget *
applications_create_list (PageState *st)
{
  GtkWidget *view = gtk_column_view_new (GTK_SELECTION_MODEL (g_object_ref (st->sel)));
  GtkWidget *scroller = gtk_scrolled_window_new ();

  gtk_column_view_set_reorderable (GTK_COLUMN_VIEW (view), FALSE);
  gtk_column_view_set_show_row_separators (GTK_COLUMN_VIEW (view), FALSE);
  g_signal_connect (view, "activate", G_CALLBACK (on_activate_position), st);

  add_column (GTK_COLUMN_VIEW (view), "Application", COL_APP, 0, TRUE);
  add_column (GTK_COLUMN_VIEW (view), "Status", COL_STATUS, 90, FALSE);
  add_column (GTK_COLUMN_VIEW (view), "CPU", COL_CPU, 80, FALSE);
  add_column (GTK_COLUMN_VIEW (view), "Memory", COL_MEM, 100, FALSE);
  add_column (GTK_COLUMN_VIEW (view), "Processes", COL_PROCS, 90, FALSE);
  add_column (GTK_COLUMN_VIEW (view), "User", COL_USER, 100, FALSE);
  add_column (GTK_COLUMN_VIEW (view), "Executable", COL_EXEC, 0, TRUE);

  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), view);
  gtk_widget_set_vexpand (scroller, TRUE);
  return scroller;
}

static void
update_outer_stack (PageState *st)
{
  const char *name;

  if (!st->loaded)
    name = "loading";
  else if (g_list_model_get_n_items (G_LIST_MODEL (st->sel)) == 0)
    name = "empty";
  else
    name = "results";
  gtk_stack_set_visible_child_name (GTK_STACK (st->outer_stack), name);
}

static void
on_items_changed (GListModel *m, guint p, guint r, guint a, gpointer data)
{
  update_outer_stack (data);
}

/* ========================================================================== *
 * Styling
 * ========================================================================== */

static void
ensure_css (void)
{
  static gsize once = 0;

  if (g_once_init_enter (&once))
    {
      GtkCssProvider *p = gtk_css_provider_new ();

      gtk_css_provider_load_from_string (p,
        ".apps-card { padding: 16px 10px 12px 10px; border-radius: 14px; }"
        ".apps-card .apps-name { font-weight: 700; }"
        ".apps-card.apps-running { background-color: alpha(@accent_bg_color, 0.07); }"
        ".apps-res { font-feature-settings: \"tnum\"; }"
        ".apps-grid { padding: 6px 12px 18px 12px; background: transparent; }"
        ".apps-grid > child { border-radius: 16px; margin: 4px; padding: 0; }");
      gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                                  GTK_STYLE_PROVIDER (p),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      g_object_unref (p);
      g_once_init_leave (&once, 1);
    }
}

/* ========================================================================== *
 * Page lifecycle
 * ========================================================================== */

/* Called when the page widget is disposed.  Must not touch child widgets. */
static void
page_state_free (gpointer data, GObject *where_the_object_was)
{
  PageState *st = data;

  backend_disconnect (st->app, st->refresh_id);
  g_cancellable_cancel (st->cancel);
  if (st->monitor != NULL)
    g_signal_handlers_disconnect_by_data (st->monitor, st);
  g_clear_object (&st->monitor);
  g_clear_handle_id (&st->rediscover_id, g_source_remove);

  g_clear_object (&st->cancel);
  g_clear_pointer (&st->tokens, g_strfreev);
  g_clear_pointer (&st->by_key, g_hash_table_unref);
  g_clear_pointer (&st->by_id, g_hash_table_unref);
  g_clear_pointer (&st->entries, g_ptr_array_unref);
  g_clear_object (&st->filter_list);
  g_clear_object (&st->filter);
  g_clear_object (&st->sorter);
  g_clear_object (&st->store);
  g_clear_object (&st->filtered);
  g_clear_object (&st->sorted);
  g_clear_object (&st->sel);
  g_free (st);
}

GtkWidget *
applications_page_create (App *app)
{
  PageState *st;
  GtkWidget *root, *bin, *content, *header, *grid, *list, *split;
  GtkWidget *results, *loading, *empty, *spinner;
  AdwBreakpoint *bp;
  GValue collapsed = G_VALUE_INIT;

  g_return_val_if_fail (app != NULL, NULL);
  ensure_css ();

  st = g_new0 (PageState, 1);
  st->app = app;
  st->cancel = g_cancellable_new ();
  st->entries = g_ptr_array_new_with_free_func (g_object_unref);
  st->by_id = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  st->by_key = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                      (GDestroyNotify) g_ptr_array_unref);
  st->filter_mode = FILTER_ALL;
  st->sort_mode = SORT_NAME;

  /* models: store -> filter -> sort -> single selection (shared by both views) */
  st->store = g_list_store_new (APPS_TYPE_ENTRY);
  st->filter = gtk_custom_filter_new (filter_func, st, NULL);
  st->filtered = gtk_filter_list_model_new (G_LIST_MODEL (g_object_ref (st->store)),
                                            GTK_FILTER (g_object_ref (st->filter)));
  gtk_filter_list_model_set_incremental (st->filtered, TRUE);   /* chunked in idle */
  st->sorter = gtk_custom_sorter_new (sort_func, st, NULL);
  st->sorted = gtk_sort_list_model_new (G_LIST_MODEL (g_object_ref (st->filtered)),
                                        GTK_SORTER (g_object_ref (st->sorter)));
  gtk_sort_list_model_set_incremental (st->sorted, TRUE);
  st->sel = gtk_single_selection_new (G_LIST_MODEL (g_object_ref (st->sorted)));
  gtk_single_selection_set_autoselect (st->sel, FALSE);
  gtk_single_selection_set_can_unselect (st->sel, TRUE);
  gtk_single_selection_set_selected (st->sel, GTK_INVALID_LIST_POSITION);

  /* root */
  root = adw_toast_overlay_new ();
  st->root = root;
  g_object_set_data (G_OBJECT (root), "apps-page", st);
  g_object_weak_ref (G_OBJECT (root), page_state_free, st);

  header = applications_create_header (st);

  grid = applications_create_grid (st);
  list = applications_create_list (st);
  st->view_stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (st->view_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration (GTK_STACK (st->view_stack), 180);
  gtk_stack_add_named (GTK_STACK (st->view_stack), grid, "grid");
  gtk_stack_add_named (GTK_STACK (st->view_stack), list, "list");

  spinner = gtk_spinner_new ();
  gtk_spinner_set_spinning (GTK_SPINNER (spinner), TRUE);
  gtk_widget_set_size_request (spinner, 32, 32);
  loading = adw_status_page_new ();
  adw_status_page_set_title (ADW_STATUS_PAGE (loading), "Loading Applications…");
  adw_status_page_set_child (ADW_STATUS_PAGE (loading), spinner);

  empty = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty), "system-search-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (empty), "No Applications Found");
  adw_status_page_set_description (ADW_STATUS_PAGE (empty),
                                   "Try a different search or filter.");

  results = st->view_stack;
  st->outer_stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (st->outer_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_named (GTK_STACK (st->outer_stack), loading, "loading");
  gtk_stack_add_named (GTK_STACK (st->outer_stack), results, "results");
  gtk_stack_add_named (GTK_STACK (st->outer_stack), empty, "empty");
  gtk_widget_set_vexpand (st->outer_stack, TRUE);

  /* content + sliding detail sidebar */
  split = adw_overlay_split_view_new ();
  st->split = split;
  adw_overlay_split_view_set_content (ADW_OVERLAY_SPLIT_VIEW (split), st->outer_stack);
  adw_overlay_split_view_set_sidebar (ADW_OVERLAY_SPLIT_VIEW (split),
                                      applications_create_details (st));
  adw_overlay_split_view_set_sidebar_position (ADW_OVERLAY_SPLIT_VIEW (split), GTK_PACK_END);
  adw_overlay_split_view_set_min_sidebar_width (ADW_OVERLAY_SPLIT_VIEW (split), 300);
  adw_overlay_split_view_set_max_sidebar_width (ADW_OVERLAY_SPLIT_VIEW (split), 380);
  adw_overlay_split_view_set_sidebar_width_fraction (ADW_OVERLAY_SPLIT_VIEW (split), 0.32);
  adw_overlay_split_view_set_enable_show_gesture (ADW_OVERLAY_SPLIT_VIEW (split), FALSE);
  adw_overlay_split_view_set_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (split), FALSE);
  gtk_widget_set_vexpand (split, TRUE);

  content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (content), header);
  gtk_box_append (GTK_BOX (content), split);

  /* small windows: the detail panel becomes an overlay instead of a column */
  bin = adw_breakpoint_bin_new ();
  gtk_widget_set_size_request (bin, 360, 300);
  adw_breakpoint_bin_set_child (ADW_BREAKPOINT_BIN (bin), content);
  bp = adw_breakpoint_new (adw_breakpoint_condition_parse ("max-width: 780sp"));
  g_value_init (&collapsed, G_TYPE_BOOLEAN);
  g_value_set_boolean (&collapsed, TRUE);
  adw_breakpoint_add_setter (bp, G_OBJECT (split), "collapsed", &collapsed);
  g_value_unset (&collapsed);
  adw_breakpoint_bin_add_breakpoint (ADW_BREAKPOINT_BIN (bin), bp);

  adw_toast_overlay_set_child (ADW_TOAST_OVERLAY (root), bin);

  g_signal_connect (st->sel, "notify::selected-item",
                    G_CALLBACK (on_selection_changed), st);
  g_signal_connect (st->sel, "items-changed", G_CALLBACK (on_items_changed), st);
  update_outer_stack (st);

  update_counts (st);
  applications_start_monitoring (st);
  applications_discover (st);

  return root;
}
