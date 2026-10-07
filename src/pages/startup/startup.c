/*
 * startup.c — Page 4: Startup Applications
 *
 * Inspects and manages XDG autostart entries for the current graphical session.
 *
 * Requirements: GTK >= 4.12, libadwaita >= 1.5, GLib/GIO >= 2.76.
 *
 * Design notes
 * ------------
 *  - Discovery, parsing and validation run on a worker thread and produce plain
 *    StartupData structs. The GTK main thread only merges them into a GListStore.
 *  - System files are NEVER modified. Disabling/editing a system entry creates
 *    a per-user override in $XDG_CONFIG_HOME/autostart (XDG autostart spec).
 *  - All writes (toggle/add/edit/remove) run on worker threads; toggles are applied
 *    optimistically and reverted by a refresh if the write fails.
 *  - Refresh is incremental (merge by desktop-file ID); the page is never rebuilt.
 *  - Running state comes from the application's shared process backend.
 */

#include "startup.h"

#include <adwaita.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#define KF_GROUP      "Desktop Entry"
#define KEY_OVERRIDE  "X-TaskManager-Override"
#define KEY_GNOME_EN  "X-GNOME-Autostart-enabled"
#define PAGE_KEY      "startup-page"
#define ROW_KEY       "startup-row"
#define RAW_LIMIT     (64 * 1024)
#define DEBOUNCE_MS   300
#define RUNNING_SECS  5

/* ------------------------------------------------------------------------ *
 * Shared process backend
 *
 * Running-state checks use the application's single process backend.  The
 * page does not maintain a second process scanner.
 * ------------------------------------------------------------------------ */

#define STARTUP_BACKEND_IS_RUNNING app_process_is_running

static gboolean
startup_backend_available(void)
{
  return TRUE;
}

/* ------------------------------------------------------------------------ *
 * Data model (no GTK types)
 * ------------------------------------------------------------------------ */

typedef enum {
  STARTUP_SRC_USER = 0,     /* only exists in the user's autostart dir          */
  STARTUP_SRC_OVERRIDE,     /* user file that overrides a system entry          */
  STARTUP_SRC_SYSTEM        /* system entry, no user override                   */
} StartupSource;

typedef enum {
  STARTUP_ST_VALID = 0,
  STARTUP_ST_DISABLED,
  STARTUP_ST_MISSING_EXEC,
  STARTUP_ST_MALFORMED,
  STARTUP_ST_UNAVAILABLE
} StartupStatus;

typedef struct {
  char *id;              /* desktop-file ID, e.g. "foo.desktop"            */
  char *path;            /* effective file                                  */
  char *dir;             /* directory containing the effective file         */
  char *system_path;     /* shadowed system file, or NULL                   */
  StartupSource source;

  char *name, *generic_name, *comment, *icon, *exec, *try_exec;
  char *cmd, *args, *work_dir, *categories, *delay, *condition;
  char *match_name;      /* program name handed to the process backend      */
  char *raw;             /* file text (UTF-8 sanitised, size-limited)       */
  char *problem;         /* syntax/required-field problem, or NULL          */
  char *unavail_reason;  /* why the entry is not applicable here            */
  char *message;         /* final human-readable explanation                */
  char *haystack;        /* lower-cased search text                         */

  gboolean kf_ok, has_type, type_ok, has_name, has_exec, dbus, bad_value;
  gboolean hidden, gnome_disabled, no_display, startup_notify, terminal;
  gboolean applicable, exec_found, try_exec_ok, workdir_ok, icon_ok;
  gboolean override_marker, is_stub, system_enabled;

  gboolean enabled, valid;
  StartupStatus status;
  gboolean running;
} StartupData;

static void
startup_data_free(StartupData *d)
{
  if (!d)
    return;
  g_free(d->id); g_free(d->path); g_free(d->dir); g_free(d->system_path);
  g_free(d->name); g_free(d->generic_name); g_free(d->comment); g_free(d->icon);
  g_free(d->exec); g_free(d->try_exec); g_free(d->cmd); g_free(d->args);
  g_free(d->work_dir); g_free(d->categories); g_free(d->delay); g_free(d->condition);
  g_free(d->match_name); g_free(d->raw); g_free(d->problem); g_free(d->unavail_reason);
  g_free(d->message); g_free(d->haystack);
  g_free(d);
}

/* ------------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------------ */

static char *
startup_tilde(const char *path)
{
  const char *home = g_get_home_dir();
  gsize n = home ? strlen(home) : 0;
  if (n > 1 && g_str_has_prefix(path, home) && (path[n] == '/' || path[n] == '\0'))
    return g_strconcat("~", path + n, NULL);
  return g_strdup(path);
}

static char *
startup_user_dir(void)
{
  return g_build_filename(g_get_user_config_dir(), "autostart", NULL);
}

/* Autostart search order per the XDG spec: user dir first, then $XDG_CONFIG_DIRS. */
static GPtrArray *
startup_dirs(void)
{
  GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(dirs, startup_user_dir());
  const char * const *sys = g_get_system_config_dirs();
  for (guint i = 0; sys && sys[i]; i++)
    g_ptr_array_add(dirs, g_build_filename(sys[i], "autostart", NULL));
  return dirs;
}

static gboolean
startup_is_safe_token(const char *s)
{
  if (!*s)
    return FALSE;
  for (; *s; s++) {
    if (!(g_ascii_isalnum(*s) || strchr("-_./+:,@=%~", *s)))
      return FALSE;
  }
  return TRUE;
}

/* Quote one Exec argument (double quotes; escape " ` $ \ per the Desktop Entry spec). */
static char *
startup_quote_token(const char *tok)
{
  if (startup_is_safe_token(tok))
    return g_strdup(tok);
  GString *s = g_string_new("\"");
  for (; *tok; tok++) {
    if (strchr("\"`$\\", *tok))
      g_string_append_c(s, '\\');
    g_string_append_c(s, *tok);
  }
  g_string_append_c(s, '"');
  return g_string_free(s, FALSE);
}

static gboolean
startup_has_control_chars(const char *s)
{
  for (; s && *s; s++)
    if ((guchar)*s < 0x20 || *s == 0x7f)
      return TRUE;
  return FALSE;
}

static gboolean
startup_program_exists(const char *prog)
{
  if (!prog || !*prog)
    return FALSE;
  if (strchr(prog, '/'))
    return g_path_is_absolute(prog) &&
           g_file_test(prog, G_FILE_TEST_IS_EXECUTABLE) &&
           !g_file_test(prog, G_FILE_TEST_IS_DIR);
  char *found = g_find_program_in_path(prog);
  gboolean ok = found != NULL;
  g_free(found);
  return ok;
}

static char *
startup_str_or_null(char *s)
{
  if (s && !*s) { g_free(s); return NULL; }
  return s;
}

/* ------------------------------------------------------------------------ *
 * Parsing
 * ------------------------------------------------------------------------ */

static char *kf_str(GKeyFile *kf, const char *key)
{ return startup_str_or_null(g_key_file_get_string(kf, KF_GROUP, key, NULL)); }

static char *kf_locale(GKeyFile *kf, const char *key)
{ return startup_str_or_null(g_key_file_get_locale_string(kf, KF_GROUP, key, NULL, NULL)); }

static gboolean
kf_bool(GKeyFile *kf, const char *key, gboolean def, gboolean *bad)
{
  if (!g_key_file_has_key(kf, KF_GROUP, key, NULL))
    return def;
  GError *e = NULL;
  gboolean v = g_key_file_get_boolean(kf, KF_GROUP, key, &e);
  if (e) {
    if (bad) *bad = TRUE;
    g_error_free(e);
    return def;
  }
  return v;
}

static gboolean
startup_list_has(char **list, char **desktops)
{
  for (guint i = 0; list && list[i]; i++)
    for (guint j = 0; desktops && desktops[j]; j++)
      if (g_ascii_strcasecmp(list[i], desktops[j]) == 0)
        return TRUE;
  return FALSE;
}

/* OnlyShowIn / NotShowIn against XDG_CURRENT_DESKTOP. Unknown desktop => don't filter. */
static gboolean
startup_session_applicable(GKeyFile *kf, char **reason)
{
  const char *cur = g_getenv("XDG_CURRENT_DESKTOP");
  if (!cur || !*cur)
    return TRUE;
  char **desk = g_strsplit(cur, ":", -1);
  char **only = g_key_file_get_string_list(kf, KF_GROUP, "OnlyShowIn", NULL, NULL);
  char **notin = g_key_file_get_string_list(kf, KF_GROUP, "NotShowIn", NULL, NULL);
  gboolean ok = TRUE;

  if (only && only[0] && !startup_list_has(only, desk)) {
    ok = FALSE;
    char *l = g_strjoinv(", ", only);
    *reason = g_strdup_printf("Only starts in: %s (current desktop: %s)", l, cur);
    g_free(l);
  } else if (notin && startup_list_has(notin, desk)) {
    ok = FALSE;
    *reason = g_strdup_printf("Excluded from the current desktop (%s)", cur);
  }
  g_strfreev(desk); g_strfreev(only); g_strfreev(notin);
  return ok;
}

static StartupData *
startup_load_entry(const char *dir, const char *id, StartupSource src)
{
  StartupData *d = g_new0(StartupData, 1);
  d->id = g_strdup(id);
  d->dir = g_strdup(dir);
  d->path = g_build_filename(dir, id, NULL);
  d->source = src;
  d->applicable = TRUE;
  d->try_exec_ok = TRUE;
  d->workdir_ok = TRUE;
  d->icon_ok = TRUE;

  char *text = NULL;
  gsize len = 0;
  GError *err = NULL;

  if (!g_file_get_contents(d->path, &text, &len, &err)) {
    d->problem = g_strdup_printf("The file cannot be read: %s", err->message);
    g_clear_error(&err);
    return d;
  }
  if (len > RAW_LIMIT) {
    d->problem = g_strdup("The file is unreasonably large for a desktop entry");
    g_free(text);
    return d;
  }
  if (!g_utf8_validate(text, len, NULL)) {
    d->problem = g_strdup("The file is not valid UTF-8");
    d->raw = g_utf8_make_valid(text, len);
    g_free(text);
    return d;
  }
  d->raw = g_strdup(text);

  GKeyFile *kf = g_key_file_new();
  if (!g_key_file_load_from_data(kf, text, len, G_KEY_FILE_NONE, &err)) {
    d->problem = g_strdup_printf("Invalid desktop-entry syntax: %s", err->message);
    g_clear_error(&err);
  } else if (!g_key_file_has_group(kf, KF_GROUP)) {
    d->problem = g_strdup("Missing the [Desktop Entry] group");
  } else {
    gboolean bad = FALSE;
    char *type = kf_str(kf, "Type");
    d->kf_ok = TRUE;
    d->has_type = type != NULL;
    d->type_ok = g_strcmp0(type, "Application") == 0;
    g_free(type);

    d->has_name = g_key_file_has_key(kf, KF_GROUP, "Name", NULL);
    d->name = kf_locale(kf, "Name");
    d->generic_name = kf_locale(kf, "GenericName");
    d->comment = kf_locale(kf, "Comment");
    d->icon = kf_str(kf, "Icon");
    d->exec = kf_str(kf, "Exec");
    d->has_exec = d->exec != NULL;
    d->try_exec = kf_str(kf, "TryExec");
    d->work_dir = kf_str(kf, "Path");
    d->categories = kf_str(kf, "Categories");
    d->delay = kf_str(kf, "X-GNOME-Autostart-Delay");
    d->condition = kf_str(kf, "AutostartCondition");

    d->hidden = kf_bool(kf, "Hidden", FALSE, &bad);
    d->no_display = kf_bool(kf, "NoDisplay", FALSE, &bad);
    d->startup_notify = kf_bool(kf, "StartupNotify", FALSE, &bad);
    d->terminal = kf_bool(kf, "Terminal", FALSE, &bad);
    d->dbus = kf_bool(kf, "DBusActivatable", FALSE, &bad);
    if (g_key_file_has_key(kf, KF_GROUP, KEY_GNOME_EN, NULL))
      d->gnome_disabled = !kf_bool(kf, KEY_GNOME_EN, TRUE, &bad);
    d->override_marker = kf_bool(kf, KEY_OVERRIDE, FALSE, NULL);
    d->bad_value = bad;
    d->applicable = startup_session_applicable(kf, &d->unavail_reason);
  }
  g_key_file_free(kf);
  g_free(text);
  return d;
}

/* A user file with no Exec that overrides a system entry is an XDG "stub":
 * display data is taken from the system entry underneath. */
static void
startup_fill_from_system(StartupData *d, const StartupData *s)
{
#define FILL(f) if (!d->f && s->f) d->f = g_strdup(s->f)
  FILL(name); FILL(generic_name); FILL(comment); FILL(icon); FILL(exec);
  FILL(try_exec); FILL(work_dir); FILL(categories); FILL(delay); FILL(condition);
#undef FILL
  if (!d->has_name && s->name) {          /* name was only an ID-derived fallback */
    g_free(d->name);
    d->name = g_strdup(s->name);
  }
  d->has_name = d->has_name || s->has_name;
  d->has_exec = s->has_exec;
  g_clear_pointer(&d->problem, g_free);   /* "missing Exec" was provisional; re-validated by caller */
  d->has_type = TRUE;
  d->type_ok = TRUE;
  d->dbus = s->dbus;
  d->startup_notify = s->startup_notify;
  d->terminal = s->terminal;
  d->no_display = s->no_display;
  d->applicable = s->applicable;
  g_free(d->unavail_reason);
  d->unavail_reason = g_strdup(s->unavail_reason);
  d->is_stub = TRUE;
}

static char *
startup_source_label(const StartupData *d, gboolean long_form)
{
  char *dir = startup_tilde(d->dir);
  char *out;
  switch (d->source) {
  case STARTUP_SRC_USER:
    out = long_form ? g_strdup_printf("User entry · %s", dir) : g_strdup("User");
    break;
  case STARTUP_SRC_OVERRIDE:
    out = long_form ? g_strdup_printf("System entry, overridden by you · %s", dir)
                    : g_strdup("System · overridden by you");
    break;
  default:
    out = long_form ? g_strdup_printf("System entry · %s", dir) : g_strdup("System");
    break;
  }
  g_free(dir);
  return out;
}

/* Pure state computation from already-collected facts: no I/O. */
static void
startup_get_effective_state(StartupData *d)
{
  g_clear_pointer(&d->message, g_free);
  d->enabled = !d->hidden && !d->gnome_disabled;

  if (!d->kf_ok || d->problem) {
    d->status = STARTUP_ST_MALFORMED;
    d->message = g_strdup(d->problem ? d->problem : "The desktop entry could not be parsed");
  } else if (d->has_exec && !d->exec_found) {
    d->status = STARTUP_ST_MISSING_EXEC;
    d->message = g_strdup_printf("The program “%s” was not found or is not executable",
                                 d->cmd ? d->cmd : "?");
  } else if (!d->workdir_ok) {
    d->status = STARTUP_ST_MALFORMED;
    d->message = g_strdup_printf("The working directory “%s” does not exist", d->work_dir);
  } else if (!d->enabled) {
    d->status = STARTUP_ST_DISABLED;
    d->message = g_strdup(d->hidden ? "Disabled for your account (Hidden=true)"
                                    : "Disabled for your account");
  } else if (!d->try_exec_ok) {
    d->status = STARTUP_ST_UNAVAILABLE;
    d->message = g_strdup_printf("TryExec target “%s” is not installed; the session skips this entry",
                                 d->try_exec);
  } else if (!d->applicable) {
    d->status = STARTUP_ST_UNAVAILABLE;
    d->message = g_strdup(d->unavail_reason ? d->unavail_reason : "Not applicable in this session");
  } else {
    d->status = STARTUP_ST_VALID;
    d->message = g_strdup("Starts automatically when you log in");
  }
  d->valid = d->status != STARTUP_ST_MALFORMED && d->status != STARTUP_ST_MISSING_EXEC;
}

/* Collects syntax problems, performs filesystem checks, then derives state. */
static void
startup_validate_entry(StartupData *d)
{
  g_clear_pointer(&d->cmd, g_free);
  g_clear_pointer(&d->args, g_free);
  g_clear_pointer(&d->match_name, g_free);
  d->exec_found = d->try_exec_ok = d->workdir_ok = d->icon_ok = TRUE;

  if (d->kf_ok && !d->problem) {
    if (!d->has_type)
      d->problem = g_strdup("Missing required key: Type");
    else if (!d->type_ok)
      d->problem = g_strdup("Unsupported Type (only Type=Application can be autostarted)");
    else if (!d->has_name)
      d->problem = g_strdup("Missing required key: Name");
    else if (!d->has_exec && !d->dbus)
      d->problem = g_strdup("Missing required key: Exec");
    else if (d->bad_value)
      d->problem = g_strdup("Contains a malformed boolean value");
  }
  if (!d->name) {
    d->name = g_strdup(d->id);
    char *dot = strrchr(d->name, '.');
    if (dot) *dot = '\0';
  }

  if (d->kf_ok && !d->problem && d->has_exec) {
    int argc = 0;
    char **argv = NULL;
    GError *e = NULL;
    if (!g_shell_parse_argv(d->exec, &argc, &argv, &e)) {
      d->problem = g_strdup_printf("The Exec line cannot be parsed: %s", e->message);
      g_clear_error(&e);
    } else if (argc < 1 || !*argv[0]) {
      d->problem = g_strdup("The Exec line is empty");
    } else {
      d->cmd = g_strdup(argv[0]);
      GString *a = g_string_new(NULL);
      for (int i = 1; i < argc; i++) {
        char *q = startup_quote_token(argv[i]);
        if (a->len) g_string_append_c(a, ' ');
        g_string_append(a, q);
        g_free(q);
      }
      d->args = g_string_free(a, FALSE);

      /* program name for the process backend; skip `env VAR=x` wrappers */
      int k = 0;
      char *base = g_path_get_basename(argv[0]);
      if (g_strcmp0(base, "env") == 0) {
        for (k = 1; k < argc && (argv[k][0] == '-' || strchr(argv[k], '=')); k++)
          ;
        if (k < argc) { g_free(base); base = g_path_get_basename(argv[k]); }
      }
      d->match_name = base;
      d->exec_found = startup_program_exists(argv[0]);
    }
    g_strfreev(argv);
  }

  if (d->try_exec)
    d->try_exec_ok = startup_program_exists(d->try_exec);
  if (d->work_dir)
    d->workdir_ok = g_path_is_absolute(d->work_dir) && g_file_test(d->work_dir, G_FILE_TEST_IS_DIR);
  if (d->icon && g_path_is_absolute(d->icon))
    d->icon_ok = g_file_test(d->icon, G_FILE_TEST_EXISTS);

  startup_get_effective_state(d);

  /* search text */
  char *src = startup_source_label(d, FALSE);
  char *joined = g_strjoin(" ", d->name, d->generic_name ? d->generic_name : "",
                           d->comment ? d->comment : "", d->exec ? d->exec : "",
                           d->id, src, NULL);
  g_free(d->haystack);
  d->haystack = g_utf8_strdown(joined, -1);
  g_free(joined);
  g_free(src);
}

typedef struct {
  GPtrArray *entries;       /* StartupData* */
  gboolean   user_writable;
} StartupScan;

static void
startup_scan_free(StartupScan *s)
{
  if (!s) return;
  if (s->entries) g_ptr_array_unref(s->entries);
  g_free(s);
}

static int
startup_strcmp_ptr(gconstpointer a, gconstpointer b)
{
  return strcmp(*(char * const *)a, *(char * const *)b);
}

static StartupScan *
startup_discover_entries(void)
{
  StartupScan *scan = g_new0(StartupScan, 1);
  scan->entries = g_ptr_array_new_with_free_func((GDestroyNotify)startup_data_free);
  GPtrArray *dirs = startup_dirs();
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);   /* id -> StartupData* */

  {
    const char *udir = dirs->pdata[0];
    if (g_file_test(udir, G_FILE_TEST_IS_DIR))
      scan->user_writable = g_access(udir, W_OK) == 0;
    else
      scan->user_writable = g_access(g_get_user_config_dir(), W_OK) == 0 ||
                            !g_file_test(g_get_user_config_dir(), G_FILE_TEST_EXISTS);
  }

  for (guint i = 0; i < dirs->len; i++) {
    const char *dir = dirs->pdata[i];
    gboolean is_user = (i == 0);
    GDir *gd = g_dir_open(dir, 0, NULL);
    if (!gd)
      continue;

    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const char *n;
    while ((n = g_dir_read_name(gd)))
      if (g_str_has_suffix(n, ".desktop"))
        g_ptr_array_add(names, g_strdup(n));
    g_dir_close(gd);
    g_ptr_array_sort(names, startup_strcmp_ptr);

    for (guint k = 0; k < names->len; k++) {
      const char *id = names->pdata[k];
      char *path = g_build_filename(dir, id, NULL);
      if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        g_free(path);
        continue;
      }
      g_free(path);

      StartupData *prev = g_hash_table_lookup(seen, id);
      if (prev) {
        /* shadowed by a higher-priority file; remember the first system file under a user file */
        if (!is_user && prev->source == STARTUP_SRC_USER && !prev->system_path) {
          StartupData *s = startup_load_entry(dir, id, STARTUP_SRC_SYSTEM);
          startup_validate_entry(s);
          prev->source = STARTUP_SRC_OVERRIDE;
          prev->system_path = g_strdup(s->path);
          if (prev->kf_ok && !prev->has_exec && !prev->dbus)
            startup_fill_from_system(prev, s);
          prev->system_enabled = s->enabled;
          startup_validate_entry(prev);
          startup_data_free(s);
        }
        continue;
      }

      StartupData *d = startup_load_entry(dir, id, is_user ? STARTUP_SRC_USER : STARTUP_SRC_SYSTEM);
      startup_validate_entry(d);
      d->system_enabled = d->enabled;
      g_ptr_array_add(scan->entries, d);
      g_hash_table_insert(seen, d->id, d);
    }
    g_ptr_array_unref(names);
  }

  g_hash_table_unref(seen);
  g_ptr_array_unref(dirs);
  return scan;
}

static gboolean
startup_data_state_equal(const StartupData *a, const StartupData *b)
{
  return a->enabled == b->enabled && a->status == b->status && a->valid == b->valid;
}

static gboolean
startup_data_equal(const StartupData *a, const StartupData *b)
{
  return startup_data_state_equal(a, b) && a->source == b->source &&
         a->system_enabled == b->system_enabled && a->exec_found == b->exec_found &&
         g_strcmp0(a->path, b->path) == 0 && g_strcmp0(a->system_path, b->system_path) == 0 &&
         g_strcmp0(a->raw, b->raw) == 0 && g_strcmp0(a->message, b->message) == 0 &&
         g_strcmp0(a->name, b->name) == 0 && g_strcmp0(a->comment, b->comment) == 0 &&
         g_strcmp0(a->icon, b->icon) == 0 && g_strcmp0(a->exec, b->exec) == 0;
}

/* ------------------------------------------------------------------------ *
 * Form fields + write operations (all run on worker threads)
 * ------------------------------------------------------------------------ */

typedef struct {
  char *name, *comment, *command, *args, *workdir, *icon;
  gboolean notify, terminal;
} StartupFields;

static void
startup_fields_clear(StartupFields *f)
{
  g_free(f->name); g_free(f->comment); g_free(f->command);
  g_free(f->args); g_free(f->workdir); g_free(f->icon);
  memset(f, 0, sizeof *f);
}

static void
startup_fields_copy(StartupFields *dst, const StartupFields *src)
{
  dst->name = g_strdup(src->name);       dst->comment = g_strdup(src->comment);
  dst->command = g_strdup(src->command); dst->args = g_strdup(src->args);
  dst->workdir = g_strdup(src->workdir); dst->icon = g_strdup(src->icon);
  dst->notify = src->notify;             dst->terminal = src->terminal;
}

static void
startup_set_invalid(GError **err, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void
startup_set_invalid(GError **err, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  char *m = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, m);
  g_free(m);
}

static gboolean
startup_fields_validate(StartupFields *f, GError **err)
{
  if (f->name)    g_strstrip(f->name);
  if (f->comment) g_strstrip(f->comment);
  if (f->command) g_strstrip(f->command);
  if (f->args)    g_strstrip(f->args);
  if (f->workdir) g_strstrip(f->workdir);
  if (f->icon)    g_strstrip(f->icon);

  if (!f->name || !*f->name) { startup_set_invalid(err, "Enter a name for the application."); return FALSE; }
  if (strlen(f->name) > 255) { startup_set_invalid(err, "The name is too long."); return FALSE; }
  if (!f->command || !*f->command) { startup_set_invalid(err, "Enter the command to run."); return FALSE; }

  const char *fields[] = { f->name, f->comment, f->command, f->args, f->workdir, f->icon };
  for (guint i = 0; i < G_N_ELEMENTS(fields); i++)
    if (!g_utf8_validate(fields[i] ? fields[i] : "", -1, NULL) || startup_has_control_chars(fields[i])) {
      startup_set_invalid(err, "Fields must not contain control characters or invalid text.");
      return FALSE;
    }

  /* command: a single program. An existing absolute path with spaces is accepted as-is. */
  if (strchr(f->command, '%')) {
    startup_set_invalid(err, "The command must not contain “%%”. Put field codes in Arguments.");
    return FALSE;
  }
  gboolean literal = g_path_is_absolute(f->command) && g_file_test(f->command, G_FILE_TEST_IS_EXECUTABLE);
  if (!literal) {
    int argc = 0;
    char **argv = NULL;
    GError *e = NULL;
    if (!g_shell_parse_argv(f->command, &argc, &argv, &e)) {
      startup_set_invalid(err, "The command is malformed: %s", e->message);
      g_clear_error(&e);
      return FALSE;
    }
    if (argc != 1) {
      g_strfreev(argv);
      startup_set_invalid(err, "The command must be a single program. Put arguments in the Arguments field.");
      return FALSE;
    }
    char *clean = g_strdup(argv[0]);
    g_strfreev(argv);
    g_free(f->command);
    f->command = clean;
  }
  if (!startup_program_exists(f->command)) {
    startup_set_invalid(err, "“%s” was not found or is not executable.", f->command);
    return FALSE;
  }
  if (f->args && *f->args) {
    int argc = 0;
    char **argv = NULL;
    GError *e = NULL;
    if (!g_shell_parse_argv(f->args, &argc, &argv, &e)) {
      startup_set_invalid(err, "The arguments are malformed: %s", e->message);
      g_clear_error(&e);
      return FALSE;
    }
    g_strfreev(argv);
  }
  if (f->workdir && *f->workdir) {
    if (f->workdir[0] == '~' && (f->workdir[1] == '/' || f->workdir[1] == '\0')) {
      char *x = g_build_filename(g_get_home_dir(), f->workdir + 1, NULL);
      g_free(f->workdir);
      f->workdir = x;
    }
    if (!g_path_is_absolute(f->workdir) || !g_file_test(f->workdir, G_FILE_TEST_IS_DIR)) {
      startup_set_invalid(err, "The working directory must be an existing absolute path.");
      return FALSE;
    }
  }
  if (f->icon && *f->icon) {
    if (strchr(f->icon, '/')) {
      if (!g_path_is_absolute(f->icon) || !g_file_test(f->icon, G_FILE_TEST_IS_REGULAR)) {
        startup_set_invalid(err, "The icon file does not exist. Use an icon name or an absolute path.");
        return FALSE;
      }
    } else if (strpbrk(f->icon, " \t")) {
      startup_set_invalid(err, "Icon names cannot contain spaces.");
      return FALSE;
    }
  }
  return TRUE;
}

static char *
startup_build_exec(const StartupFields *f)
{
  char *c = startup_quote_token(f->command);
  if (f->args && *f->args) {
    char *r = g_strconcat(c, " ", f->args, NULL);
    g_free(c);
    return r;
  }
  return c;
}

static GKeyFile *
kf_load(const char *path, GError **err)
{
  GKeyFile *kf = g_key_file_new();
  if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, err)) {
    g_key_file_free(kf);
    return NULL;
  }
  return kf;
}

static gboolean
kf_save(GKeyFile *kf, const char *path, GError **err)
{
  gsize len = 0;
  char *data = g_key_file_to_data(kf, &len, err);
  if (!data)
    return FALSE;
  gboolean ok = g_file_set_contents(path, data, (gssize)len, err);   /* atomic rename */
  g_free(data);
  return ok;
}

static void
kf_drop_localized(GKeyFile *kf, const char *key)
{
  gsize n = 0;
  char **keys = g_key_file_get_keys(kf, KF_GROUP, &n, NULL);
  char *prefix = g_strconcat(key, "[", NULL);
  for (gsize i = 0; keys && i < n; i++)
    if (g_str_has_prefix(keys[i], prefix))
      g_key_file_remove_key(kf, KF_GROUP, keys[i], NULL);
  g_free(prefix);
  g_strfreev(keys);
}

static void
kf_set_opt(GKeyFile *kf, const char *key, const char *val)
{
  if (val && *val)
    g_key_file_set_string(kf, KF_GROUP, key, val);
  else
    g_key_file_remove_key(kf, KF_GROUP, key, NULL);
}

static void
kf_set_localizable(GKeyFile *kf, const char *key, const char *val)
{
  char *old = g_key_file_get_string(kf, KF_GROUP, key, NULL);
  if (g_strcmp0(old, val && *val ? val : NULL) != 0)
    kf_drop_localized(kf, key);
  g_free(old);
  kf_set_opt(kf, key, val);
}

static void
kf_mark_enabled(GKeyFile *kf, gboolean enable)
{
  gboolean has_gnome = g_key_file_has_key(kf, KF_GROUP, KEY_GNOME_EN, NULL);
  if (enable) {
    g_key_file_remove_key(kf, KF_GROUP, "Hidden", NULL);
    if (has_gnome)
      g_key_file_set_boolean(kf, KF_GROUP, KEY_GNOME_EN, TRUE);
  } else {
    g_key_file_set_boolean(kf, KF_GROUP, "Hidden", TRUE);
    if (has_gnome)
      g_key_file_set_boolean(kf, KF_GROUP, KEY_GNOME_EN, FALSE);
  }
}

static void
startup_apply_fields(GKeyFile *kf, const StartupFields *f)
{
  char *exec = startup_build_exec(f);
  if (!g_key_file_has_key(kf, KF_GROUP, "Type", NULL))
    g_key_file_set_string(kf, KF_GROUP, "Type", "Application");
  kf_set_localizable(kf, "Name", f->name);
  kf_set_localizable(kf, "Comment", f->comment);
  g_key_file_set_string(kf, KF_GROUP, "Exec", exec);
  kf_set_opt(kf, "Path", f->workdir);
  kf_set_opt(kf, "Icon", f->icon);
  g_key_file_set_boolean(kf, KF_GROUP, "StartupNotify", f->notify);
  g_key_file_set_boolean(kf, KF_GROUP, "Terminal", f->terminal);
  g_free(exec);
}

static gboolean
startup_ensure_user_dir(const char *dir, GError **err)
{
  if (g_mkdir_with_parents(dir, 0755) != 0 && errno != EEXIST) {
    g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Cannot create %s: %s", dir, g_strerror(errno));
    return FALSE;
  }
  if (g_access(dir, W_OK) != 0) {
    g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_ACCES,
                "You do not have permission to write to %s", dir);
    return FALSE;
  }
  return TRUE;
}

static char *
startup_unique_id(const char *name)
{
  GString *slug = g_string_new(NULL);
  char *down = g_utf8_strdown(name, -1);
  gboolean dash = TRUE;
  for (const char *p = down; *p; p++) {
    if (g_ascii_isalnum(*p)) { g_string_append_c(slug, *p); dash = FALSE; }
    else if (!dash) { g_string_append_c(slug, '-'); dash = TRUE; }
  }
  g_free(down);
  while (slug->len && slug->str[slug->len - 1] == '-')
    g_string_truncate(slug, slug->len - 1);
  if (!slug->len) g_string_append(slug, "startup-app");
  if (slug->len > 80) g_string_truncate(slug, 80);

  GPtrArray *dirs = startup_dirs();
  char *id = NULL;
  for (int n = 1; n < 1000; n++) {
    g_free(id);
    id = n == 1 ? g_strdup_printf("%s.desktop", slug->str)
                : g_strdup_printf("%s-%d.desktop", slug->str, n);
    gboolean taken = FALSE;
    for (guint i = 0; i < dirs->len && !taken; i++) {
      char *p = g_build_filename(dirs->pdata[i], id, NULL);
      taken = g_file_test(p, G_FILE_TEST_EXISTS);
      g_free(p);
    }
    if (!taken) break;
  }
  g_ptr_array_unref(dirs);
  g_string_free(slug, TRUE);
  return id;
}

typedef enum { OP_TOGGLE, OP_ADD, OP_EDIT, OP_REMOVE } StartupOpKind;

typedef struct {
  StartupOpKind kind;
  char *id, *path, *system_path, *user_dir, *name, *created_id;
  StartupSource source;
  gboolean enable, system_enabled, is_stub, was_enabled;
  StartupFields f;
  GtkWidget *dialog;          /* strong ref, may be NULL */
} StartupOp;

static void
startup_op_free(StartupOp *op)
{
  if (!op) return;
  g_free(op->id); g_free(op->path); g_free(op->system_path); g_free(op->user_dir);
  g_free(op->name); g_free(op->created_id);
  startup_fields_clear(&op->f);
  g_clear_object(&op->dialog);
  g_free(op);
}

static StartupOp *
startup_op_new(StartupOpKind kind, const StartupData *d)
{
  StartupOp *op = g_new0(StartupOp, 1);
  op->kind = kind;
  op->user_dir = startup_user_dir();
  if (d) {
    op->id = g_strdup(d->id);
    op->path = g_strdup(d->path);
    op->system_path = g_strdup(d->system_path);
    op->name = g_strdup(d->name);
    op->source = d->source;
    op->system_enabled = d->system_enabled;
    op->is_stub = d->is_stub;
    op->was_enabled = d->enabled;
  }
  return op;
}

static gboolean
startup_op_run(StartupOp *op, GError **err)
{
  /* Hard safety rule: only ever write inside the user's autostart directory. */
  if (op->id && (strchr(op->id, '/') || !g_str_has_suffix(op->id, ".desktop"))) {
    startup_set_invalid(err, "Invalid desktop-file name.");
    return FALSE;
  }
  char *target = op->id ? g_build_filename(op->user_dir, op->id, NULL) : NULL;
  gboolean ok = FALSE;
  GKeyFile *kf = NULL;

  switch (op->kind) {
  case OP_TOGGLE:
    if (!startup_ensure_user_dir(op->user_dir, err))
      break;
    if (op->source == STARTUP_SRC_SYSTEM) {
      if (!op->enable) {            /* minimal XDG override: Hidden=true */
        kf = g_key_file_new();
        g_key_file_set_string(kf, KF_GROUP, "Type", "Application");
        g_key_file_set_string(kf, KF_GROUP, "Name", op->name ? op->name : op->id);
        g_key_file_set_boolean(kf, KF_GROUP, "Hidden", TRUE);
      } else {                      /* vendor disabled it: personal enabled copy */
        kf = kf_load(op->path, err);
        if (!kf) break;
        kf_mark_enabled(kf, TRUE);
      }
      g_key_file_set_boolean(kf, KF_GROUP, KEY_OVERRIDE, TRUE);
      ok = kf_save(kf, target, err);
    } else if (op->is_stub && op->enable) {
      if (op->system_enabled) {     /* drop the override: system default is enabled */
        ok = g_unlink(target) == 0 || errno == ENOENT;
        if (!ok)
          g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(errno),
                      "Cannot remove the override: %s", g_strerror(errno));
      } else {
        kf = kf_load(op->system_path, err);
        if (!kf) break;
        kf_mark_enabled(kf, TRUE);
        g_key_file_set_boolean(kf, KF_GROUP, KEY_OVERRIDE, TRUE);
        ok = kf_save(kf, target, err);
      }
    } else {
      kf = kf_load(target, err);
      if (!kf) break;
      kf_mark_enabled(kf, op->enable);
      ok = kf_save(kf, target, err);
    }
    break;

  case OP_ADD: {
    if (!startup_fields_validate(&op->f, err))
      break;
    if (!startup_ensure_user_dir(op->user_dir, err))
      break;
    op->created_id = startup_unique_id(op->f.name);
    g_free(target);
    target = g_build_filename(op->user_dir, op->created_id, NULL);
    kf = g_key_file_new();
    g_key_file_set_string(kf, KF_GROUP, "Version", "1.0");
    startup_apply_fields(kf, &op->f);
    g_key_file_set_boolean(kf, KF_GROUP, KEY_GNOME_EN, TRUE);
    ok = kf_save(kf, target, err);
    break;
  }

  case OP_EDIT:
    if (!startup_fields_validate(&op->f, err))
      break;
    if (!startup_ensure_user_dir(op->user_dir, err))
      break;
    if (op->source == STARTUP_SRC_USER || (op->source == STARTUP_SRC_OVERRIDE && !op->is_stub)) {
      kf = kf_load(target, err);
      if (!kf) break;
    } else {                        /* create/replace override from the vendor entry */
      kf = kf_load(op->source == STARTUP_SRC_SYSTEM ? op->path : op->system_path, err);
      if (!kf) break;
      if (op->source == STARTUP_SRC_OVERRIDE)
        kf_mark_enabled(kf, op->was_enabled);
      g_key_file_set_boolean(kf, KF_GROUP, KEY_OVERRIDE, TRUE);
    }
    startup_apply_fields(kf, &op->f);
    ok = kf_save(kf, target, err);
    break;

  case OP_REMOVE:
    if (op->source == STARTUP_SRC_SYSTEM) {
      startup_set_invalid(err, "System entries cannot be removed. Disable the entry instead.");
      break;
    }
    ok = g_unlink(target) == 0;
    if (!ok)
      g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(errno),
                  "Cannot remove the file: %s", g_strerror(errno));
    break;
  }

  if (kf) g_key_file_free(kf);
  g_free(target);
  return ok;
}

/* ------------------------------------------------------------------------ *
 * GObject wrapper so entries can live in a GListStore
 * ------------------------------------------------------------------------ */

typedef struct {
  GObject parent_instance;
  StartupData *d;
} StartupEntry;

typedef struct {
  GObjectClass parent_class;
} StartupEntryClass;

static GType startup_entry_get_type(void);
G_DEFINE_TYPE(StartupEntry, startup_entry, G_TYPE_OBJECT)
#define STARTUP_TYPE_ENTRY (startup_entry_get_type())
#define ENTRY(o) ((StartupEntry *)(o))

static guint entry_changed_signal;

static void
startup_entry_finalize(GObject *o)
{
  startup_data_free(ENTRY(o)->d);
  G_OBJECT_CLASS(startup_entry_parent_class)->finalize(o);
}

static void
startup_entry_class_init(StartupEntryClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = startup_entry_finalize;
  entry_changed_signal = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                      0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void startup_entry_init(StartupEntry *self) { (void)self; }

static StartupEntry *
startup_entry_new(StartupData *d)
{
  StartupEntry *e = g_object_new(STARTUP_TYPE_ENTRY, NULL);
  e->d = d;
  return e;
}

static void
startup_entry_emit_changed(StartupEntry *e)
{
  g_signal_emit(e, entry_changed_signal, 0);
}

/* ------------------------------------------------------------------------ *
 * Page state
 * ------------------------------------------------------------------------ */

typedef enum {
  FILTER_ALL, FILTER_ENABLED, FILTER_DISABLED, FILTER_USER, FILTER_SYSTEM,
  FILTER_VALID, FILTER_INVALID, FILTER_RUNNING, FILTER_N
} StartupFilterMode;

typedef enum {
  SORT_NAME, SORT_ENABLED, SORT_SOURCE, SORT_VALIDITY, SORT_RUNNING
} StartupSortMode;

enum {
  DR_GENERIC, DR_DESCRIPTION, DR_FILENAME, DR_PATH, DR_SOURCE, DR_ENABLED, DR_EXEC,
  DR_WORKDIR, DR_CATEGORIES, DR_NOTIFY, DR_TERMINAL, DR_VISIBILITY, DR_DELAY,
  DR_VALID, DR_EXECEXISTS, DR_RUNNING, DR_ORIGIN, DR_N
};

typedef struct {
  App *app;
  GtkWidget *root;                 /* AdwToastOverlay: the page widget            */
  GtkWidget *split;
  GtkWidget *search, *filter_dd, *sort_dd, *desc_label;
  GtkWidget *lbl_total, *lbl_enabled, *lbl_disabled;
  GtkWidget *view_stack, *list_view, *empty_page;

  GListStore *store;
  GtkFilterListModel *filtered;
  GtkSortListModel *sorted;
  GtkSingleSelection *sel;
  GtkFilter *filter;
  GtkSorter *sorter;
  GHashTable *by_id;               /* id -> StartupEntry* (borrowed)              */

  StartupFilterMode filter_modes[FILTER_N];
  guint n_filter_modes;
  StartupFilterMode filter_mode;
  StartupSortMode sort_mode;
  char *search_text;               /* lower-cased                                 */

  /* details panel */
  GtkWidget *det_stack, *det_icon, *det_title, *det_sub, *det_pill;
  GtkWidget *dr[DR_N];
  GtkWidget *btn_edit, *btn_remove, *raw_label;
  StartupEntry *selected;          /* strong ref                                  */
  gulong selected_changed_id;

  /* lifecycle / refresh */
  GPtrArray *monitors;
  GHashTable *monitored;
  guint debounce_id, running_id;
  gboolean refresh_busy, refresh_pending, loaded, destroyed, user_writable;
  gint64 last_refresh_us;
  guint pending_ops;
  char *select_after_id;
} StartupPage;

static void startup_refresh(StartupPage *p);
static void startup_select(StartupPage *p, StartupEntry *e);
static void startup_create_edit_dialog(StartupPage *p, StartupEntry *e);
static void startup_create_add_dialog(StartupPage *p);

static StartupPage *
startup_page_of(gpointer widget)
{
  return g_object_get_data(G_OBJECT(widget), PAGE_KEY);
}

static void
startup_toast(StartupPage *p, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void
startup_toast(StartupPage *p, const char *fmt, ...)
{
  if (p->destroyed) return;
  va_list ap;
  va_start(ap, fmt);
  char *m = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  AdwToast *t = adw_toast_new(m);
  adw_toast_set_timeout(t, 4);
  adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(p->root), t);
  g_free(m);
}

/* ------------------------------------------------------------------------ *
 * Presentation helpers
 * ------------------------------------------------------------------------ */

static const char *
startup_pill_text(const StartupData *d)
{
  switch (d->status) {
  case STARTUP_ST_VALID:        return "Enabled";
  case STARTUP_ST_DISABLED:     return "Disabled";
  case STARTUP_ST_MISSING_EXEC: return "Missing executable";
  case STARTUP_ST_MALFORMED:    return "Malformed";
  default:                      return "Unavailable";
  }
}

static void
startup_style_pill(GtkWidget *pill, const StartupData *d)
{
  const char *tone = "ok";
  switch (d->status) {
  case STARTUP_ST_VALID:        tone = "ok";   break;
  case STARTUP_ST_DISABLED:     tone = "off";  break;
  case STARTUP_ST_UNAVAILABLE:  tone = "warn"; break;
  default:                      tone = "bad";  break;
  }
  const char *cls[] = { "startup-pill", tone, NULL };
  gtk_widget_set_css_classes(pill, cls);
  gtk_label_set_text(GTK_LABEL(pill), startup_pill_text(d));
}

static void
startup_set_icon(GtkImage *img, const StartupData *d)
{
  GIcon *gi = NULL;
  if (d->icon && d->icon_ok) {
    GError *e = NULL;
    gi = g_icon_new_for_string(d->icon, &e);
    g_clear_error(&e);
  }
  if (!gi)
    gi = g_themed_icon_new("application-x-executable");
  gtk_image_set_from_gicon(img, gi);
  g_object_unref(gi);
}

static void
startup_install_css(void)
{
  static gsize once = 0;
  if (!g_once_init_enter(&once))
    return;
  GdkDisplay *disp = gdk_display_get_default();
  if (disp) {
    GtkCssProvider *pr = gtk_css_provider_new();
    gtk_css_provider_load_from_string(pr,
      ".startup-pill { border-radius: 999px; padding: 1px 9px; font-size: 0.8em; font-weight: 700; }"
      ".startup-pill.ok   { color: @success_color; background: alpha(@success_color, 0.14); }"
      ".startup-pill.off  { color: alpha(currentColor, 0.75); background: alpha(currentColor, 0.10); }"
      ".startup-pill.warn { color: @warning_color; background: alpha(@warning_color, 0.16); }"
      ".startup-pill.bad  { color: @error_color;   background: alpha(@error_color, 0.14); }"
      ".startup-chip { background: alpha(currentColor, 0.06); border-radius: 12px; padding: 8px 16px; }"
      ".startup-problem { color: @warning_color; }");
    gtk_style_context_add_provider_for_display(disp, GTK_STYLE_PROVIDER(pr),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(pr);
  }
  g_once_init_leave(&once, 1);
}

/* ------------------------------------------------------------------------ *
 * Filtering / sorting
 * ------------------------------------------------------------------------ */

static const char *const filter_labels[FILTER_N] = {
  "All", "Enabled", "Disabled", "User entries", "System entries", "Valid", "Invalid", "Running"
};

static gboolean
startup_filter(gpointer item, gpointer user_data)
{
  StartupPage *p = user_data;
  const StartupData *d = ENTRY(item)->d;

  switch (p->filter_mode) {
  case FILTER_ENABLED:  if (!d->enabled) return FALSE; break;
  case FILTER_DISABLED: if (d->enabled) return FALSE; break;
  case FILTER_USER:     if (d->source != STARTUP_SRC_USER) return FALSE; break;
  case FILTER_SYSTEM:   if (d->source == STARTUP_SRC_USER) return FALSE; break;
  case FILTER_VALID:    if (!d->valid) return FALSE; break;
  case FILTER_INVALID:  if (d->valid) return FALSE; break;
  case FILTER_RUNNING:  if (!d->running) return FALSE; break;
  default: break;
  }
  if (p->search_text && *p->search_text && !strstr(d->haystack, p->search_text))
    return FALSE;
  return TRUE;
}

static int
startup_source_rank(const StartupData *d)
{
  return d->source == STARTUP_SRC_USER ? 0 : d->source == STARTUP_SRC_OVERRIDE ? 1 : 2;
}

/* Primary key by mode, then name, then ID: a total order, so rows never jitter. */
static int
startup_sort(gconstpointer a, gconstpointer b, gpointer user_data)
{
  StartupPage *p = user_data;
  const StartupData *x = ENTRY(a)->d, *y = ENTRY(b)->d;
  int r = 0;
  switch (p->sort_mode) {
  case SORT_ENABLED:  r = (int)y->enabled - (int)x->enabled; break;
  case SORT_SOURCE:   r = startup_source_rank(x) - startup_source_rank(y); break;
  case SORT_VALIDITY: r = (int)x->valid - (int)y->valid; break;   /* invalid first */
  case SORT_RUNNING:  r = (int)y->running - (int)x->running; break;
  default: break;
  }
  if (r) return r < 0 ? -1 : 1;
  r = g_utf8_collate(x->name, y->name);
  if (r) return r < 0 ? -1 : 1;
  r = strcmp(x->id, y->id);
  return r < 0 ? -1 : r > 0 ? 1 : 0;
}

static void
startup_reevaluate(StartupPage *p)
{
  gtk_filter_changed(p->filter, GTK_FILTER_CHANGE_DIFFERENT);
  gtk_sorter_changed(p->sorter, GTK_SORTER_CHANGE_DIFFERENT);
}

/* ------------------------------------------------------------------------ *
 * Summary / empty-state
 * ------------------------------------------------------------------------ */

static void
startup_update_summary(StartupPage *p)
{
  guint n = g_list_model_get_n_items(G_LIST_MODEL(p->store)), en = 0;
  for (guint i = 0; i < n; i++) {
    StartupEntry *e = g_list_model_get_item(G_LIST_MODEL(p->store), i);
    if (ENTRY(e)->d->enabled) en++;
    g_object_unref(e);
  }
  char buf[32];
  g_snprintf(buf, sizeof buf, "%u", n);      gtk_label_set_text(GTK_LABEL(p->lbl_total), buf);
  g_snprintf(buf, sizeof buf, "%u", en);     gtk_label_set_text(GTK_LABEL(p->lbl_enabled), buf);
  g_snprintf(buf, sizeof buf, "%u", n - en); gtk_label_set_text(GTK_LABEL(p->lbl_disabled), buf);
}

static void
startup_update_view(StartupPage *p)
{
  if (p->destroyed) return;
  guint shown = g_list_model_get_n_items(G_LIST_MODEL(p->sorted));
  guint total = g_list_model_get_n_items(G_LIST_MODEL(p->store));
  if (!p->loaded) {
    gtk_stack_set_visible_child_name(GTK_STACK(p->view_stack), "loading");
  } else if (shown == 0) {
    adw_status_page_set_title(ADW_STATUS_PAGE(p->empty_page),
                              total ? "No Matching Entries" : "No Startup Applications");
    adw_status_page_set_description(ADW_STATUS_PAGE(p->empty_page),
                              total ? "Try a different search or filter."
                                    : "Nothing starts automatically yet. Use Add to create an entry.");
    gtk_stack_set_visible_child_name(GTK_STACK(p->view_stack), "empty");
  } else {
    gtk_stack_set_visible_child_name(GTK_STACK(p->view_stack), "list");
  }
}

static void
on_sorted_items_changed(GListModel *m, guint pos, guint rem, guint add, gpointer pg)
{
  (void)m; (void)pos; (void)rem; (void)add;
  startup_update_view(pg);
}

/* ------------------------------------------------------------------------ *
 * Operations (UI side)
 * ------------------------------------------------------------------------ */

static void
startup_op_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
  (void)src; (void)c;
  GError *err = NULL;
  if (startup_op_run(data, &err))
    g_task_return_boolean(task, TRUE);
  else
    g_task_return_error(task, err);
}

static void
startup_select_by_id(StartupPage *p, const char *id)
{
  guint n = g_list_model_get_n_items(G_LIST_MODEL(p->sorted));
  for (guint i = 0; i < n; i++) {
    StartupEntry *e = g_list_model_get_item(G_LIST_MODEL(p->sorted), i);
    gboolean hit = g_strcmp0(ENTRY(e)->d->id, id) == 0;
    g_object_unref(e);
    if (hit) {
      gtk_single_selection_set_selected(p->sel, i);
      gtk_list_view_scroll_to(GTK_LIST_VIEW(p->list_view), i, GTK_LIST_SCROLL_NONE, NULL);
      return;
    }
  }
}

static void
startup_op_done(GObject *src, GAsyncResult *res, gpointer unused)
{
  (void)unused;
  StartupPage *p = startup_page_of(src);
  StartupOp *op = g_task_get_task_data(G_TASK(res));
  GError *err = NULL;
  g_task_propagate_boolean(G_TASK(res), &err);

  if (p->pending_ops) p->pending_ops--;
  if (p->destroyed) {
    g_clear_error(&err);
    return;
  }

  if (err) {
    switch (op->kind) {
    case OP_TOGGLE:
      startup_toast(p, "Couldn’t %s “%s”: %s", op->enable ? "enable" : "disable",
                    op->name ? op->name : op->id, err->message);
      break;
    case OP_REMOVE:
      startup_toast(p, "Couldn’t remove “%s”: %s", op->name ? op->name : op->id, err->message);
      break;
    default:
      if (op->dialog) {
        GtkWidget *banner = g_object_get_data(G_OBJECT(op->dialog), "banner");
        GtkWidget *content = g_object_get_data(G_OBJECT(op->dialog), "content");
        adw_banner_set_title(ADW_BANNER(banner), err->message);
        adw_banner_set_revealed(ADW_BANNER(banner), TRUE);
        gtk_widget_set_sensitive(content, TRUE);
      } else {
        startup_toast(p, "%s", err->message);
      }
    }
    g_clear_error(&err);
  } else {
    switch (op->kind) {
    case OP_ADD:
      g_free(p->select_after_id);
      p->select_after_id = g_strdup(op->created_id);
      startup_toast(p, "Added “%s” to startup applications", op->f.name);
      break;
    case OP_EDIT:
      startup_toast(p, "Saved changes to “%s”", op->f.name);
      break;
    case OP_REMOVE:
      startup_toast(p, op->source == STARTUP_SRC_OVERRIDE ? "Restored system default for “%s”"
                                                          : "Removed “%s”", op->name);
      break;
    default:
      break;
    }
    if (op->dialog)
      adw_dialog_close(ADW_DIALOG(op->dialog));
  }
  startup_refresh(p);   /* reconcile with disk (also reverts failed optimistic toggles) */
}

static void
startup_op_start(StartupPage *p, StartupOp *op)
{
  p->pending_ops++;
  GTask *t = g_task_new(p->root, NULL, startup_op_done, NULL);
  g_task_set_task_data(t, op, (GDestroyNotify)startup_op_free);
  g_task_run_in_thread(t, startup_op_thread);
  g_object_unref(t);
}

static void
startup_toggle(StartupPage *p, StartupEntry *e, gboolean enable)
{
  StartupData *d = e->d;
  if (d->enabled == enable)
    return;

  StartupOp *op = startup_op_new(OP_TOGGLE, d);
  op->enable = enable;

  /* optimistic update: the row reflects the new state immediately */
  d->hidden = !enable;
  if (enable) d->gnome_disabled = FALSE;
  startup_get_effective_state(d);
  startup_entry_emit_changed(e);
  startup_update_summary(p);

  startup_op_start(p, op);
}

static void startup_enable_entry(StartupPage *p, StartupEntry *e)  { startup_toggle(p, e, TRUE); }
static void startup_disable_entry(StartupPage *p, StartupEntry *e) { startup_toggle(p, e, FALSE); }

static void
startup_add_entry(StartupPage *p, const StartupFields *f, GtkWidget *dialog)
{
  StartupOp *op = startup_op_new(OP_ADD, NULL);
  startup_fields_copy(&op->f, f);
  op->dialog = g_object_ref(dialog);
  startup_op_start(p, op);
}

static void
startup_edit_entry(StartupPage *p, StartupEntry *e, const StartupFields *f, GtkWidget *dialog)
{
  StartupOp *op = startup_op_new(OP_EDIT, e->d);
  startup_fields_copy(&op->f, f);
  op->dialog = g_object_ref(dialog);
  startup_op_start(p, op);
}

static void
startup_remove_entry(StartupPage *p, StartupEntry *e)
{
  StartupData *d = e->d;
  if (d->source == STARTUP_SRC_SYSTEM)
    return;                                  /* never offered; guarded again in the worker */
  StartupOp *op = startup_op_new(OP_REMOVE, d);

  if (d->source == STARTUP_SRC_USER) {       /* user-only entry: drop the row right away */
    guint pos;
    g_object_ref(e);
    if (g_list_store_find(p->store, e, &pos)) {
      g_hash_table_remove(p->by_id, d->id);
      g_list_store_remove(p->store, pos);
    }
    g_object_unref(e);
    startup_update_summary(p);
  }
  startup_op_start(p, op);
}

/* ---- confirmation ---- */

typedef struct { StartupPage *page; StartupEntry *entry; } ConfirmCtx;

static void
on_confirm_done(GObject *src, GAsyncResult *res, gpointer data)
{
  ConfirmCtx *c = data;
  const char *r = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(src), res);
  if (!c->page->destroyed && g_strcmp0(r, "confirm") == 0)
    startup_remove_entry(c->page, c->entry);
  g_object_unref(c->entry);
  g_free(c);
}

static void
startup_confirm_remove(StartupPage *p, StartupEntry *e)
{
  const StartupData *d = e->d;
  if (d->source == STARTUP_SRC_SYSTEM)
    return;
  gboolean reset = d->source == STARTUP_SRC_OVERRIDE;
  char *body = reset
    ? g_strdup_printf("Your personal override of “%s” will be deleted and the system entry will apply again. "
                      "The system file is not touched.", d->name)
    : g_strdup_printf("“%s” will no longer start automatically and its file will be deleted from "
                      "your autostart folder.", d->name);
  AdwDialog *dlg = adw_alert_dialog_new(reset ? "Restore System Default?" : "Remove Startup Entry?", body);
  g_free(body);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg), "cancel", "Cancel",
                                 "confirm", reset ? "Restore" : "Remove", NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "confirm", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "cancel");

  ConfirmCtx *c = g_new0(ConfirmCtx, 1);
  c->page = p;
  c->entry = g_object_ref(e);
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(dlg), p->root, NULL, on_confirm_done, c);
}

/* ------------------------------------------------------------------------ *
 * Add / edit dialog
 * ------------------------------------------------------------------------ */

typedef struct {
  StartupPage *page;
  StartupEntry *entry;          /* NULL when adding */
  GtkWidget *dialog, *banner, *content;
  GtkWidget *e_name, *e_desc, *e_cmd, *e_args, *e_dir, *e_icon, *sw_notify, *sw_term;
} FormCtx;

static void
form_free(gpointer data)
{
  FormCtx *c = data;
  g_clear_object(&c->entry);
  g_free(c);
}

static GtkWidget *
form_row(GtkWidget *group, const char *title, const char *text)
{
  GtkWidget *r = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
  if (text)
    gtk_editable_set_text(GTK_EDITABLE(r), text);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), r);
  return r;
}

static void
form_save(FormCtx *c)
{
  StartupFields f = {0};
  f.name = g_strdup(gtk_editable_get_text(GTK_EDITABLE(c->e_name)));
  f.comment = g_strdup(gtk_editable_get_text(GTK_EDITABLE(c->e_desc)));
  f.command = g_strdup(gtk_editable_get_text(GTK_EDITABLE(c->e_cmd)));
  f.args = g_strdup(gtk_editable_get_text(GTK_EDITABLE(c->e_args)));
  f.workdir = g_strdup(gtk_editable_get_text(GTK_EDITABLE(c->e_dir)));
  f.icon = g_strdup(gtk_editable_get_text(GTK_EDITABLE(c->e_icon)));
  f.notify = adw_switch_row_get_active(ADW_SWITCH_ROW(c->sw_notify));
  f.terminal = adw_switch_row_get_active(ADW_SWITCH_ROW(c->sw_term));

  adw_banner_set_revealed(ADW_BANNER(c->banner), FALSE);
  gtk_widget_set_sensitive(c->content, FALSE);     /* validation + write happen off-thread */
  if (c->entry)
    startup_edit_entry(c->page, c->entry, &f, c->dialog);
  else
    startup_add_entry(c->page, &f, c->dialog);
  startup_fields_clear(&f);
}

static void on_form_save_clicked(GtkButton *b, gpointer c) { (void)b; form_save(c); }
static void on_form_entry_activated(AdwEntryRow *r, gpointer c) { (void)r; form_save(c); }
static void on_form_cancel_clicked(GtkButton *b, gpointer dlg) { (void)b; adw_dialog_close(ADW_DIALOG(dlg)); }

static void
startup_build_form(StartupPage *p, StartupEntry *edit)
{
  const StartupData *d = edit ? edit->d : NULL;
  FormCtx *c = g_new0(FormCtx, 1);
  c->page = p;
  c->entry = edit ? g_object_ref(edit) : NULL;

  AdwDialog *dlg = adw_dialog_new();
  c->dialog = GTK_WIDGET(dlg);
  adw_dialog_set_title(dlg, edit ? "Edit Startup Application" : "Add Startup Application");
  adw_dialog_set_content_width(dlg, 480);
  adw_dialog_set_content_height(dlg, 620);
  g_object_set_data_full(G_OBJECT(dlg), "form", c, form_free);

  GtkWidget *tv = adw_toolbar_view_new();
  GtkWidget *hb = adw_header_bar_new();
  GtkWidget *cancel = gtk_button_new_with_label("Cancel");
  GtkWidget *save = gtk_button_new_with_label(edit ? "Save" : "Add");
  gtk_widget_add_css_class(save, "suggested-action");
  g_signal_connect(cancel, "clicked", G_CALLBACK(on_form_cancel_clicked), dlg);
  g_signal_connect(save, "clicked", G_CALLBACK(on_form_save_clicked), c);
  adw_header_bar_pack_start(ADW_HEADER_BAR(hb), cancel);
  adw_header_bar_pack_end(ADW_HEADER_BAR(hb), save);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), hb);

  c->banner = adw_banner_new("");
  adw_banner_set_revealed(ADW_BANNER(c->banner), FALSE);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), c->banner);

  GtkWidget *page = adw_preferences_page_new();
  c->content = page;
  GtkWidget *g1 = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g1), "Application");
  if (d && d->source != STARTUP_SRC_USER)
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g1),
      "This is a system entry. Your changes are saved as a personal override in your autostart "
      "folder; the system file is never modified.");

  c->e_name = form_row(g1, "Name", d ? d->name : NULL);
  c->e_desc = form_row(g1, "Description (optional)", d ? d->comment : NULL);
  c->e_cmd  = form_row(g1, "Command", d ? d->cmd : NULL);
  c->e_args = form_row(g1, "Arguments (optional)", d ? d->args : NULL);
  c->e_dir  = form_row(g1, "Working directory (optional)", d ? d->work_dir : NULL);
  c->e_icon = form_row(g1, "Icon name or path (optional)", d ? d->icon : NULL);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g1));

  GtkWidget *g2 = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g2), "Behavior");
  c->sw_notify = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(c->sw_notify), "Startup notification");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(c->sw_notify), "Show a launch indicator while the application starts");
  adw_switch_row_set_active(ADW_SWITCH_ROW(c->sw_notify), d ? d->startup_notify : FALSE);
  c->sw_term = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(c->sw_term), "Run in terminal");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(c->sw_term), "Only for command-line programs");
  adw_switch_row_set_active(ADW_SWITCH_ROW(c->sw_term), d ? d->terminal : FALSE);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), c->sw_notify);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), c->sw_term);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g2));

  GtkWidget *entries[] = { c->e_name, c->e_desc, c->e_cmd, c->e_args, c->e_dir, c->e_icon };
  for (guint i = 0; i < G_N_ELEMENTS(entries); i++)
    g_signal_connect(entries[i], "entry-activated", G_CALLBACK(on_form_entry_activated), c);

  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), page);
  adw_dialog_set_child(dlg, tv);
  g_object_set_data(G_OBJECT(dlg), "banner", c->banner);
  g_object_set_data(G_OBJECT(dlg), "content", c->content);
  adw_dialog_set_focus(dlg, c->e_name);
  adw_dialog_present(dlg, p->root);
}

static void startup_create_add_dialog(StartupPage *p)            { startup_build_form(p, NULL); }
static void startup_create_edit_dialog(StartupPage *p, StartupEntry *e) { startup_build_form(p, e); }

/* ------------------------------------------------------------------------ *
 * Row widget
 * ------------------------------------------------------------------------ */

typedef struct {
  StartupPage *page;
  StartupEntry *entry;            /* borrowed while bound */
  gulong changed_id;
  GtkWidget *icon, *name, *pill, *desc, *meta_icon, *meta, *sw, *menu, *info;
} StartupRow;

static void
row_menu_add(GMenu *m, const char *label, const char *action, const char *id)
{
  GMenuItem *it = g_menu_item_new(label, NULL);
  g_menu_item_set_action_and_target(it, action, "s", id);
  g_menu_append_item(m, it);
  g_object_unref(it);
}

static void
row_update(StartupRow *r)
{
  if (!r->entry) return;
  const StartupData *d = r->entry->d;

  startup_set_icon(GTK_IMAGE(r->icon), d);
  gtk_label_set_text(GTK_LABEL(r->name), d->name);
  startup_style_pill(r->pill, d);

  const char *line = d->comment ? d->comment : d->generic_name ? d->generic_name : d->exec ? d->exec : "";
  if (!d->valid) {
    gtk_label_set_text(GTK_LABEL(r->desc), d->message);
    gtk_widget_set_tooltip_text(r->desc, d->message);
    gtk_widget_add_css_class(r->desc, "startup-problem");
    gtk_widget_remove_css_class(r->desc, "dim-label");
  } else {
    gtk_label_set_text(GTK_LABEL(r->desc), line);
    gtk_widget_set_tooltip_text(r->desc, NULL);
    gtk_widget_remove_css_class(r->desc, "startup-problem");
    gtk_widget_add_css_class(r->desc, "dim-label");
  }

  const char *icon = d->source == STARTUP_SRC_USER ? "user-home-symbolic"
                   : d->source == STARTUP_SRC_OVERRIDE ? "document-edit-symbolic" : "computer-symbolic";
  gtk_image_set_from_icon_name(GTK_IMAGE(r->meta_icon), icon);
  GString *m = g_string_new(NULL);
  char *src = startup_source_label(d, FALSE);
  g_string_append(m, src);
  g_free(src);
  if (d->delay) g_string_append_printf(m, " · Delay %s s", d->delay);
  if (d->condition) g_string_append(m, " · Conditional start");
  if (d->running) g_string_append(m, " · Running");
  gtk_label_set_text(GTK_LABEL(r->meta), m->str);
  g_string_free(m, TRUE);

  gtk_widget_set_opacity(r->info, d->enabled ? 1.0 : 0.65);

  g_signal_handlers_block_matched(r->sw, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, r);
  gtk_switch_set_active(GTK_SWITCH(r->sw), d->enabled);
  g_signal_handlers_unblock_matched(r->sw, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, r);
  gtk_widget_set_sensitive(r->sw, r->page->user_writable);
  gtk_widget_set_tooltip_text(r->sw, r->page->user_writable
      ? (d->source == STARTUP_SRC_SYSTEM ? "Enable or disable for your account only" : "Enable or disable")
      : "Your autostart folder is not writable");

  GMenu *menu = g_menu_new();
  row_menu_add(menu, "Show Details", "startup.details", d->id);
  row_menu_add(menu, d->source == STARTUP_SRC_USER ? "Edit…" : "Edit as Personal Override…", "startup.edit", d->id);
  row_menu_add(menu, "Copy Command", "startup.copy", d->id);
  if (d->source != STARTUP_SRC_SYSTEM) {
    GMenu *sec = g_menu_new();
    row_menu_add(sec, d->source == STARTUP_SRC_USER ? "Remove…" : "Restore System Default…", "startup.remove", d->id);
    g_menu_append_section(menu, NULL, G_MENU_MODEL(sec));
    g_object_unref(sec);
  }
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(r->menu), G_MENU_MODEL(menu));
  g_object_unref(menu);
}

static void on_row_entry_changed(StartupEntry *e, gpointer r) { (void)e; row_update(r); }

static void
on_row_switch_active(GObject *sw, GParamSpec *ps, gpointer data)
{
  (void)ps;
  StartupRow *r = data;
  if (!r->entry) return;
  gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
  if (on) startup_enable_entry(r->page, r->entry);
  else    startup_disable_entry(r->page, r->entry);
}

static GtkWidget *
startup_create_row(StartupPage *p)
{
  StartupRow *r = g_new0(StartupRow, 1);
  r->page = p;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(box, 10);    gtk_widget_set_margin_bottom(box, 10);
  gtk_widget_set_margin_start(box, 12);  gtk_widget_set_margin_end(box, 8);

  r->icon = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(r->icon), 40);
  gtk_widget_set_valign(r->icon, GTK_ALIGN_CENTER);

  r->info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(r->info, TRUE);
  gtk_widget_set_valign(r->info, GTK_ALIGN_CENTER);

  GtkWidget *l1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  r->name = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(r->name), 0);
  gtk_label_set_ellipsize(GTK_LABEL(r->name), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(r->name, "heading");
  r->pill = gtk_label_new(NULL);
  gtk_widget_set_valign(r->pill, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(l1), r->name);
  gtk_box_append(GTK_BOX(l1), r->pill);

  r->desc = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(r->desc), 0);
  gtk_label_set_ellipsize(GTK_LABEL(r->desc), PANGO_ELLIPSIZE_END);

  GtkWidget *l3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  r->meta_icon = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(r->meta_icon), 12);
  gtk_widget_add_css_class(r->meta_icon, "dim-label");
  r->meta = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(r->meta), 0);
  gtk_label_set_ellipsize(GTK_LABEL(r->meta), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(r->meta, "caption");
  gtk_widget_add_css_class(r->meta, "dim-label");
  gtk_box_append(GTK_BOX(l3), r->meta_icon);
  gtk_box_append(GTK_BOX(l3), r->meta);

  gtk_box_append(GTK_BOX(r->info), l1);
  gtk_box_append(GTK_BOX(r->info), r->desc);
  gtk_box_append(GTK_BOX(r->info), l3);

  r->sw = gtk_switch_new();
  gtk_widget_set_valign(r->sw, GTK_ALIGN_CENTER);
  g_signal_connect(r->sw, "notify::active", G_CALLBACK(on_row_switch_active), r);

  r->menu = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(r->menu), "view-more-symbolic");
  gtk_widget_add_css_class(r->menu, "flat");
  gtk_widget_add_css_class(r->menu, "circular");
  gtk_widget_set_valign(r->menu, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(r->menu, "More actions");

  gtk_box_append(GTK_BOX(box), r->icon);
  gtk_box_append(GTK_BOX(box), r->info);
  gtk_box_append(GTK_BOX(box), r->sw);
  gtk_box_append(GTK_BOX(box), r->menu);

  g_object_set_data_full(G_OBJECT(box), ROW_KEY, r, g_free);
  return box;
}

static void on_factory_setup(GtkSignalListItemFactory *f, GtkListItem *item, gpointer pg)
{ (void)f; gtk_list_item_set_child(item, startup_create_row(pg)); }

static void
on_factory_bind(GtkSignalListItemFactory *f, GtkListItem *item, gpointer pg)
{
  (void)f; (void)pg;
  StartupRow *r = g_object_get_data(G_OBJECT(gtk_list_item_get_child(item)), ROW_KEY);
  r->entry = gtk_list_item_get_item(item);
  r->changed_id = g_signal_connect(r->entry, "changed", G_CALLBACK(on_row_entry_changed), r);
  row_update(r);
}

static void
on_factory_unbind(GtkSignalListItemFactory *f, GtkListItem *item, gpointer pg)
{
  (void)f; (void)pg;
  StartupRow *r = g_object_get_data(G_OBJECT(gtk_list_item_get_child(item)), ROW_KEY);
  if (r->entry && r->changed_id)
    g_signal_handler_disconnect(r->entry, r->changed_id);
  r->changed_id = 0;
  r->entry = NULL;
}

/* ------------------------------------------------------------------------ *
 * Details panel
 * ------------------------------------------------------------------------ */

static GtkWidget *
detail_row(GtkWidget *group, const char *title)
{
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
  gtk_widget_add_css_class(row, "property");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
  return row;
}

static void
drow_set(GtkWidget *row, const char *val)
{
  gtk_widget_set_visible(row, val && *val);
  if (val && *val)
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), val);
}

static void
startup_details_update(StartupPage *p)
{
  if (p->destroyed) return;
  if (!p->selected) {
    gtk_stack_set_visible_child_name(GTK_STACK(p->det_stack), "empty");
    return;
  }
  const StartupData *d = p->selected->d;
  gtk_stack_set_visible_child_name(GTK_STACK(p->det_stack), "content");

  startup_set_icon(GTK_IMAGE(p->det_icon), d);
  gtk_label_set_text(GTK_LABEL(p->det_title), d->name);
  gtk_label_set_text(GTK_LABEL(p->det_sub), d->id);
  startup_style_pill(p->det_pill, d);

  drow_set(p->dr[DR_GENERIC], d->generic_name);
  drow_set(p->dr[DR_DESCRIPTION], d->comment);
  drow_set(p->dr[DR_FILENAME], d->id);
  drow_set(p->dr[DR_PATH], d->path);

  char *s = startup_source_label(d, TRUE);
  drow_set(p->dr[DR_SOURCE], s);
  g_free(s);

  drow_set(p->dr[DR_ENABLED], d->enabled ? "Enabled" : "Disabled");
  drow_set(p->dr[DR_EXEC], d->exec ? d->exec : "(D-Bus activation)");
  drow_set(p->dr[DR_WORKDIR], d->work_dir);
  drow_set(p->dr[DR_CATEGORIES], d->categories);
  drow_set(p->dr[DR_NOTIFY], d->startup_notify ? "Yes" : "No");
  drow_set(p->dr[DR_TERMINAL], d->terminal ? "Runs in a terminal" : "No terminal");

  char *vis = g_strdup_printf("%s%s", d->no_display ? "Hidden from application menus (NoDisplay)" : "Visible in application menus",
                              d->hidden ? " · marked deleted (Hidden=true)" : "");
  drow_set(p->dr[DR_VISIBILITY], vis);
  g_free(vis);

  char *delay = NULL;
  if (d->delay && d->condition) delay = g_strdup_printf("Delay %s s · Condition: %s", d->delay, d->condition);
  else if (d->delay)            delay = g_strdup_printf("Delay %s s", d->delay);
  else if (d->condition)        delay = g_strdup_printf("Condition: %s", d->condition);
  drow_set(p->dr[DR_DELAY], delay);
  g_free(delay);

  char *valid = g_strdup_printf("%s — %s", d->valid ? "Valid" : "Invalid", d->message);
  drow_set(p->dr[DR_VALID], valid);
  g_free(valid);

  char *ex = !d->has_exec ? g_strdup("Not applicable")
           : d->exec_found ? g_strdup_printf("Found: %s", d->cmd)
                           : g_strdup_printf("Not found: %s", d->cmd ? d->cmd : "?");
  drow_set(p->dr[DR_EXECEXISTS], ex);
  g_free(ex);

  drow_set(p->dr[DR_RUNNING], !startup_backend_available() ? "Unknown (process backend unavailable)"
                              : d->running ? "Running now" : "Not running");

  const char *origin;
  switch (d->source) {
  case STARTUP_SRC_USER:
    origin = "Created or owned by you. Fully editable.";
    break;
  case STARTUP_SRC_OVERRIDE:
    origin = "Provided by the system and modified by you through a personal override. "
             "The system file is untouched; “Restore System Default” removes your override.";
    break;
  default:
    origin = "Provided by the system and read-only here. Disabling or editing creates a personal "
             "override in your autostart folder. Changing the system file itself needs administrator "
             "rights and is intentionally not offered.";
  }
  drow_set(p->dr[DR_ORIGIN], origin);

  gtk_button_set_label(GTK_BUTTON(p->btn_edit), d->source == STARTUP_SRC_USER ? "Edit…" : "Edit as Override…");
  gtk_widget_set_sensitive(p->btn_edit, p->user_writable);
  gtk_button_set_label(GTK_BUTTON(p->btn_remove), d->source == STARTUP_SRC_OVERRIDE ? "Restore Default…" : "Remove…");
  gtk_widget_set_sensitive(p->btn_remove, d->source != STARTUP_SRC_SYSTEM && p->user_writable);
  gtk_widget_set_tooltip_text(p->btn_remove, d->source == STARTUP_SRC_SYSTEM
      ? "System entries cannot be removed. Use the switch to disable it for your account." : NULL);

  gtk_label_set_text(GTK_LABEL(p->raw_label), d->raw ? d->raw : "");
}

static void on_selected_entry_changed(StartupEntry *e, gpointer p) { (void)e; startup_details_update(p); }

static void
startup_select(StartupPage *p, StartupEntry *e)
{
  if (p->selected == e)
    return;
  if (p->selected) {
    if (p->selected_changed_id)
      g_signal_handler_disconnect(p->selected, p->selected_changed_id);
    p->selected_changed_id = 0;
    g_clear_object(&p->selected);
  }
  if (e) {
    p->selected = g_object_ref(e);
    p->selected_changed_id = g_signal_connect(e, "changed", G_CALLBACK(on_selected_entry_changed), p);
  }
  startup_details_update(p);
}

static void
on_selection_changed(GObject *sel, GParamSpec *ps, gpointer pg)
{
  (void)ps;
  startup_select(pg, gtk_single_selection_get_selected_item(GTK_SINGLE_SELECTION(sel)));
}

static void
on_list_activate(GtkListView *lv, guint pos, gpointer pg)
{
  (void)lv; (void)pos;
  StartupPage *p = pg;
  if (adw_overlay_split_view_get_collapsed(ADW_OVERLAY_SPLIT_VIEW(p->split)))
    adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(p->split), TRUE);
}

static void
on_collapsed_changed(GObject *split, GParamSpec *ps, gpointer pg)
{
  (void)ps; (void)pg;
  AdwOverlaySplitView *s = ADW_OVERLAY_SPLIT_VIEW(split);
  adw_overlay_split_view_set_show_sidebar(s, !adw_overlay_split_view_get_collapsed(s));
}

static void
on_details_close(GtkButton *b, gpointer pg)
{
  (void)b;
  StartupPage *p = pg;
  adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(p->split), FALSE);
}

static void on_details_edit(GtkButton *b, gpointer pg)
{ (void)b; StartupPage *p = pg; if (p->selected) startup_create_edit_dialog(p, p->selected); }
static void on_details_remove(GtkButton *b, gpointer pg)
{ (void)b; StartupPage *p = pg; if (p->selected) startup_confirm_remove(p, p->selected); }

static GtkWidget *
startup_create_details(StartupPage *p)
{
  p->det_stack = gtk_stack_new();

  GtkWidget *empty = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "system-run-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(empty), "No Entry Selected");
  adw_status_page_set_description(ADW_STATUS_PAGE(empty), "Select a startup application to see its details.");
  gtk_stack_add_named(GTK_STACK(p->det_stack), empty, "empty");

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *close = gtk_button_new_from_icon_name("go-next-symbolic");
  gtk_widget_set_halign(close, GTK_ALIGN_START);
  gtk_widget_set_margin_start(close, 6); gtk_widget_set_margin_top(close, 6);
  gtk_widget_add_css_class(close, "flat");
  gtk_widget_set_tooltip_text(close, "Hide details");
  g_signal_connect(close, "clicked", G_CALLBACK(on_details_close), p);
  g_object_bind_property(p->split, "collapsed", close, "visible", G_BINDING_SYNC_CREATE);
  gtk_box_append(GTK_BOX(outer), close);

  GtkWidget *sw = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(sw, TRUE);
  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 520);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 380);
  GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_top(col, 12);    gtk_widget_set_margin_bottom(col, 18);
  gtk_widget_set_margin_start(col, 12);  gtk_widget_set_margin_end(col, 12);

  /* identity header */
  GtkWidget *head = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_halign(head, GTK_ALIGN_CENTER);
  p->det_icon = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(p->det_icon), 64);
  p->det_title = gtk_label_new(NULL);
  gtk_widget_add_css_class(p->det_title, "title-2");
  gtk_label_set_wrap(GTK_LABEL(p->det_title), TRUE);
  gtk_label_set_justify(GTK_LABEL(p->det_title), GTK_JUSTIFY_CENTER);
  p->det_sub = gtk_label_new(NULL);
  gtk_widget_add_css_class(p->det_sub, "dim-label");
  gtk_widget_add_css_class(p->det_sub, "caption");
  gtk_label_set_selectable(GTK_LABEL(p->det_sub), TRUE);
  p->det_pill = gtk_label_new(NULL);
  gtk_widget_set_halign(p->det_pill, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(head), p->det_icon);
  gtk_box_append(GTK_BOX(head), p->det_title);
  gtk_box_append(GTK_BOX(head), p->det_sub);
  gtk_box_append(GTK_BOX(head), p->det_pill);
  gtk_box_append(GTK_BOX(col), head);

  /* actions */
  GtkWidget *acts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(acts, GTK_ALIGN_CENTER);
  p->btn_edit = gtk_button_new_with_label("Edit…");
  p->btn_remove = gtk_button_new_with_label("Remove…");
  gtk_widget_add_css_class(p->btn_remove, "destructive-action");
  g_signal_connect(p->btn_edit, "clicked", G_CALLBACK(on_details_edit), p);
  g_signal_connect(p->btn_remove, "clicked", G_CALLBACK(on_details_remove), p);
  gtk_box_append(GTK_BOX(acts), p->btn_edit);
  gtk_box_append(GTK_BOX(acts), p->btn_remove);
  gtk_box_append(GTK_BOX(col), acts);

  /* sections */
  GtkWidget *g;
  g = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), "Identity");
  p->dr[DR_GENERIC]     = detail_row(g, "Generic name");
  p->dr[DR_DESCRIPTION] = detail_row(g, "Description");
  gtk_box_append(GTK_BOX(col), g);

  g = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), "Startup");
  p->dr[DR_FILENAME]   = detail_row(g, "Desktop file");
  p->dr[DR_PATH]       = detail_row(g, "Full path");
  p->dr[DR_SOURCE]     = detail_row(g, "Source");
  p->dr[DR_ENABLED]    = detail_row(g, "State");
  p->dr[DR_EXEC]       = detail_row(g, "Command (Exec)");
  p->dr[DR_WORKDIR]    = detail_row(g, "Working directory");
  p->dr[DR_CATEGORIES] = detail_row(g, "Categories");
  p->dr[DR_DELAY]      = detail_row(g, "Delay / condition");
  p->dr[DR_NOTIFY]     = detail_row(g, "Startup notification");
  p->dr[DR_TERMINAL]   = detail_row(g, "Terminal");
  p->dr[DR_VISIBILITY] = detail_row(g, "Visibility");
  gtk_box_append(GTK_BOX(col), g);

  g = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), "Behavior");
  p->dr[DR_VALID]      = detail_row(g, "Validation");
  p->dr[DR_EXECEXISTS] = detail_row(g, "Executable");
  p->dr[DR_RUNNING]    = detail_row(g, "Running");
  p->dr[DR_ORIGIN]     = detail_row(g, "Origin");
  gtk_box_append(GTK_BOX(col), g);

  g = adw_preferences_group_new();
  GtkWidget *exp = adw_expander_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(exp), "Raw desktop entry");
  GtkWidget *rsw = gtk_scrolled_window_new();
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(rsw), 140);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(rsw), 260);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(rsw), TRUE);
  p->raw_label = gtk_label_new(NULL);
  gtk_label_set_selectable(GTK_LABEL(p->raw_label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(p->raw_label), 0);
  gtk_label_set_wrap(GTK_LABEL(p->raw_label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(p->raw_label), PANGO_WRAP_CHAR);
  gtk_widget_add_css_class(p->raw_label, "monospace");
  gtk_widget_set_margin_top(p->raw_label, 8);   gtk_widget_set_margin_bottom(p->raw_label, 8);
  gtk_widget_set_margin_start(p->raw_label, 12); gtk_widget_set_margin_end(p->raw_label, 12);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(rsw), p->raw_label);
  adw_expander_row_add_row(ADW_EXPANDER_ROW(exp), rsw);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g), exp);
  gtk_box_append(GTK_BOX(col), g);

  adw_clamp_set_child(ADW_CLAMP(clamp), col);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), clamp);
  gtk_box_append(GTK_BOX(outer), sw);
  gtk_stack_add_named(GTK_STACK(p->det_stack), outer, "content");
  gtk_stack_set_visible_child_name(GTK_STACK(p->det_stack), "empty");
  return p->det_stack;
}

/* ------------------------------------------------------------------------ *
 * Header, summary, search, filters, list
 * ------------------------------------------------------------------------ */

static void
on_search_changed(GtkSearchEntry *e, gpointer pg)
{
  StartupPage *p = pg;
  g_free(p->search_text);
  p->search_text = g_utf8_strdown(gtk_editable_get_text(GTK_EDITABLE(e)), -1);
  gtk_filter_changed(p->filter, GTK_FILTER_CHANGE_DIFFERENT);
}

static GtkWidget *
startup_create_search(StartupPage *p)
{
  p->search = gtk_search_entry_new();
  gtk_widget_set_hexpand(p->search, TRUE);
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(p->search),
                                        "Search name, command, file or source");
  g_signal_connect(p->search, "search-changed", G_CALLBACK(on_search_changed), p);
  return p->search;
}

static void
on_filter_selected(GObject *dd, GParamSpec *ps, gpointer pg)
{
  (void)ps;
  StartupPage *p = pg;
  guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
  p->filter_mode = i < p->n_filter_modes ? p->filter_modes[i] : FILTER_ALL;
  gtk_filter_changed(p->filter, GTK_FILTER_CHANGE_DIFFERENT);
}

static void
on_sort_selected(GObject *dd, GParamSpec *ps, gpointer pg)
{
  (void)ps;
  StartupPage *p = pg;
  p->sort_mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
  gtk_sorter_changed(p->sorter, GTK_SORTER_CHANGE_DIFFERENT);
}

static GtkWidget *
startup_create_filters(StartupPage *p)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  const char *labels[FILTER_N + 1];
  p->n_filter_modes = 0;
  for (int m = 0; m < FILTER_N; m++) {
    if (m == FILTER_RUNNING && !startup_backend_available())
      continue;                                   /* only offered when detectable */
    p->filter_modes[p->n_filter_modes] = m;
    labels[p->n_filter_modes++] = filter_labels[m];
  }
  labels[p->n_filter_modes] = NULL;
  p->filter_dd = gtk_drop_down_new_from_strings(labels);
  gtk_widget_set_tooltip_text(p->filter_dd, "Filter entries");
  g_signal_connect(p->filter_dd, "notify::selected", G_CALLBACK(on_filter_selected), p);

  const char *sorts[] = { "Name", "Enabled state", "Source", "Validity", "Running state", NULL };
  p->sort_dd = gtk_drop_down_new_from_strings(sorts);
  gtk_widget_set_tooltip_text(p->sort_dd, "Sort by");
  g_signal_connect(p->sort_dd, "notify::selected", G_CALLBACK(on_sort_selected), p);

  gtk_box_append(GTK_BOX(box), p->filter_dd);
  gtk_box_append(GTK_BOX(box), p->sort_dd);
  return box;
}

static GtkWidget *
startup_create_header(StartupPage *p)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

  GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(titles, TRUE);
  GtkWidget *title = gtk_label_new("Startup Applications");
  gtk_widget_add_css_class(title, "title-1");
  gtk_label_set_xalign(GTK_LABEL(title), 0);
  p->desc_label = gtk_label_new("These entries are launched automatically when your desktop session starts.");
  gtk_widget_add_css_class(p->desc_label, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(p->desc_label), 0);
  gtk_label_set_wrap(GTK_LABEL(p->desc_label), TRUE);
  gtk_box_append(GTK_BOX(titles), title);
  gtk_box_append(GTK_BOX(titles), p->desc_label);

  GtkWidget *add = gtk_button_new();
  GtkWidget *content = adw_button_content_new();
  adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), "list-add-symbolic");
  adw_button_content_set_label(ADW_BUTTON_CONTENT(content), "Add");
  gtk_button_set_child(GTK_BUTTON(add), content);
  gtk_widget_add_css_class(add, "suggested-action");
  gtk_widget_set_valign(add, GTK_ALIGN_START);
  gtk_widget_set_tooltip_text(add, "Add a startup application");
  g_signal_connect_swapped(add, "clicked", G_CALLBACK(startup_create_add_dialog), p);

  gtk_box_append(GTK_BOX(top), titles);
  gtk_box_append(GTK_BOX(top), add);

  GtkWidget *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(tools), startup_create_search(p));
  gtk_box_append(GTK_BOX(tools), startup_create_filters(p));

  gtk_box_append(GTK_BOX(box), top);
  gtk_box_append(GTK_BOX(box), tools);
  return box;
}

static GtkWidget *
summary_chip(const char *caption, GtkWidget **number)
{
  GtkWidget *chip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(chip, "startup-chip");
  *number = gtk_label_new("–");
  gtk_widget_add_css_class(*number, "title-3");
  GtkWidget *cap = gtk_label_new(caption);
  gtk_widget_add_css_class(cap, "dim-label");
  gtk_widget_set_valign(cap, GTK_ALIGN_BASELINE);
  gtk_box_append(GTK_BOX(chip), *number);
  gtk_box_append(GTK_BOX(chip), cap);
  return chip;
}

static GtkWidget *
startup_create_summary(StartupPage *p)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(box), summary_chip("Total", &p->lbl_total));
  gtk_box_append(GTK_BOX(box), summary_chip("Enabled", &p->lbl_enabled));
  gtk_box_append(GTK_BOX(box), summary_chip("Disabled", &p->lbl_disabled));
  return box;
}

static GtkWidget *
startup_create_list(StartupPage *p)
{
  p->store = g_list_store_new(STARTUP_TYPE_ENTRY);
  p->filter = GTK_FILTER(gtk_custom_filter_new(startup_filter, p, NULL));
  p->sorter = GTK_SORTER(gtk_custom_sorter_new(startup_sort, p, NULL));
  p->filtered = gtk_filter_list_model_new(g_object_ref(G_LIST_MODEL(p->store)), g_object_ref(p->filter));
  p->sorted = gtk_sort_list_model_new(g_object_ref(G_LIST_MODEL(p->filtered)), g_object_ref(p->sorter));
  p->sel = gtk_single_selection_new(g_object_ref(G_LIST_MODEL(p->sorted)));
  gtk_single_selection_set_autoselect(p->sel, FALSE);
  gtk_single_selection_set_can_unselect(p->sel, TRUE);
  gtk_single_selection_set_selected(p->sel, GTK_INVALID_LIST_POSITION);

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(on_factory_setup), p);
  g_signal_connect(factory, "bind", G_CALLBACK(on_factory_bind), p);
  g_signal_connect(factory, "unbind", G_CALLBACK(on_factory_unbind), p);

  p->list_view = gtk_list_view_new(g_object_ref(GTK_SELECTION_MODEL(p->sel)), factory);
  gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(p->list_view), TRUE);
  gtk_widget_add_css_class(p->list_view, "rich-list");
  gtk_widget_set_name(p->list_view, "startup-list");

  GtkWidget *sw = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), p->list_view);
  gtk_widget_add_css_class(sw, "card");

  p->empty_page = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(p->empty_page), "system-run-symbolic");

  GtkWidget *loading = gtk_spinner_new();
  gtk_spinner_set_spinning(GTK_SPINNER(loading), TRUE);
  gtk_widget_set_size_request(loading, 32, 32);
  gtk_widget_set_halign(loading, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(loading, GTK_ALIGN_CENTER);

  p->view_stack = gtk_stack_new();
  gtk_widget_set_vexpand(p->view_stack, TRUE);
  gtk_stack_add_named(GTK_STACK(p->view_stack), loading, "loading");
  gtk_stack_add_named(GTK_STACK(p->view_stack), sw, "list");
  gtk_stack_add_named(GTK_STACK(p->view_stack), p->empty_page, "empty");
  gtk_stack_set_visible_child_name(GTK_STACK(p->view_stack), "loading");

  g_signal_connect(p->sorted, "items-changed", G_CALLBACK(on_sorted_items_changed), p);
  g_signal_connect(p->list_view, "activate", G_CALLBACK(on_list_activate), p);
  g_signal_connect(p->sel, "notify::selected-item", G_CALLBACK(on_selection_changed), p);
  return p->view_stack;
}

/* ------------------------------------------------------------------------ *
 * Refresh: discovery (worker) -> merge (main thread)
 * ------------------------------------------------------------------------ */

static void
startup_update_running(StartupPage *p)
{
  if (!startup_backend_available() || p->destroyed)
    return;
  gboolean any = FALSE;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(p->store));
  for (guint i = 0; i < n; i++) {
    StartupEntry *e = g_list_model_get_item(G_LIST_MODEL(p->store), i);
    StartupData *d = e->d;
    gboolean r = d->match_name && STARTUP_BACKEND_IS_RUNNING(p->app, d->match_name);
    if (r != d->running) {
      d->running = r;
      any = TRUE;
      startup_entry_emit_changed(e);
    }
    g_object_unref(e);
  }
  /* Only the Running filter must react live; sort order is left alone to avoid row jumping. */
  if (any && p->filter_mode == FILTER_RUNNING)
    gtk_filter_changed(p->filter, GTK_FILTER_CHANGE_DIFFERENT);
}

static void
startup_merge(StartupPage *p, StartupScan *scan)
{
  GPtrArray *fresh = scan->entries;
  GHashTable *ids = g_hash_table_new(g_str_hash, g_str_equal);
  GPtrArray *adds = g_ptr_array_new();
  gboolean reeval = FALSE;

  p->user_writable = scan->user_writable;

  for (guint i = 0; i < fresh->len; i++) {
    StartupData *d = fresh->pdata[i];
    StartupEntry *e = g_hash_table_lookup(p->by_id, d->id);
    if (!e) {
      e = startup_entry_new(d);
      fresh->pdata[i] = NULL;                     /* ownership moved to the entry */
      g_ptr_array_add(adds, e);
      g_hash_table_insert(p->by_id, g_strdup(d->id), e);
      g_hash_table_add(ids, e->d->id);
    } else {
      g_hash_table_add(ids, e->d->id);
      if (!startup_data_equal(e->d, d)) {
        if (!startup_data_state_equal(e->d, d))
          reeval = TRUE;
        d->running = e->d->running;
        startup_data_free(e->d);
        e->d = d;
        fresh->pdata[i] = NULL;
        startup_entry_emit_changed(e);
      }
    }
  }

  /* drop entries whose files are gone */
  for (guint i = g_list_model_get_n_items(G_LIST_MODEL(p->store)); i-- > 0;) {
    StartupEntry *e = g_list_model_get_item(G_LIST_MODEL(p->store), i);
    if (!g_hash_table_contains(ids, e->d->id)) {
      g_hash_table_remove(p->by_id, e->d->id);
      g_list_store_remove(p->store, i);
    }
    g_object_unref(e);
  }

  if (adds->len) {
    g_list_store_splice(p->store, g_list_model_get_n_items(G_LIST_MODEL(p->store)), 0,
                        adds->pdata, adds->len);
    for (guint i = 0; i < adds->len; i++)
      g_object_unref(adds->pdata[i]);
  }
  g_ptr_array_unref(adds);
  g_hash_table_unref(ids);

  if (reeval)
    startup_reevaluate(p);

  startup_update_running(p);
  startup_update_summary(p);
  startup_details_update(p);      /* writability may have changed */
  startup_update_view(p);

  if (p->select_after_id) {
    startup_select_by_id(p, p->select_after_id);
    g_clear_pointer(&p->select_after_id, g_free);
  }
}

static void
startup_scan_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
  (void)src; (void)data; (void)c;
  g_task_return_pointer(task, startup_discover_entries(), (GDestroyNotify)startup_scan_free);
}

static void startup_watch_changes(StartupPage *p);

static void
on_scan_done(GObject *src, GAsyncResult *res, gpointer unused)
{
  (void)unused;
  StartupPage *p = startup_page_of(src);
  StartupScan *scan = g_task_propagate_pointer(G_TASK(res), NULL);
  p->refresh_busy = FALSE;

  if (!p->destroyed && scan) {
    if (p->pending_ops > 0) {
      p->refresh_pending = TRUE;      /* a write is in flight; its completion refreshes again */
    } else {
      p->loaded = TRUE;
      startup_merge(p, scan);
      p->last_refresh_us = g_get_monotonic_time();
      startup_watch_changes(p);
    }
  }
  startup_scan_free(scan);

  if (!p->destroyed && p->refresh_pending) {
    p->refresh_pending = FALSE;
    if (p->pending_ops == 0)
      startup_refresh(p);
  }
}

static void
startup_refresh(StartupPage *p)
{
  if (p->destroyed)
    return;
  if (p->refresh_busy) {
    p->refresh_pending = TRUE;
    return;
  }
  p->refresh_busy = TRUE;
  GTask *t = g_task_new(p->root, NULL, on_scan_done, NULL);
  g_task_run_in_thread(t, startup_scan_thread);
  g_object_unref(t);
}

/* ---- filesystem monitoring ---- */

static gboolean
on_debounce(gpointer pg)
{
  StartupPage *p = pg;
  p->debounce_id = 0;
  startup_refresh(p);
  return G_SOURCE_REMOVE;
}

static void
on_fs_event(GFileMonitor *m, GFile *f, GFile *other, GFileMonitorEvent ev, gpointer pg)
{
  (void)m; (void)f; (void)other; (void)ev;
  StartupPage *p = pg;
  if (!p->debounce_id)
    p->debounce_id = g_timeout_add(DEBOUNCE_MS, on_debounce, p);
}

static void
startup_watch_changes(StartupPage *p)
{
  GPtrArray *dirs = startup_dirs();
  for (guint i = 0; i < dirs->len; i++) {
    const char *dir = dirs->pdata[i];
    if (g_hash_table_contains(p->monitored, dir) || !g_file_test(dir, G_FILE_TEST_IS_DIR))
      continue;
    GFile *f = g_file_new_for_path(dir);
    GFileMonitor *mon = g_file_monitor_directory(f, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    g_object_unref(f);
    if (!mon)
      continue;
    g_signal_connect(mon, "changed", G_CALLBACK(on_fs_event), p);
    g_ptr_array_add(p->monitors, mon);
    g_hash_table_add(p->monitored, g_strdup(dir));
  }
  g_ptr_array_unref(dirs);
}

/* ---- visibility ---- */

static gboolean
on_running_tick(gpointer pg)
{
  startup_update_running(pg);
  return G_SOURCE_CONTINUE;
}

static void
on_page_map(GtkWidget *w, gpointer pg)
{
  (void)w;
  StartupPage *p = pg;
  if (g_get_monotonic_time() - p->last_refresh_us > G_USEC_PER_SEC)
    startup_refresh(p);
  if (startup_backend_available() && !p->running_id)
    p->running_id = g_timeout_add_seconds(RUNNING_SECS, on_running_tick, p);
}

static void
on_page_unmap(GtkWidget *w, gpointer pg)
{
  (void)w;
  StartupPage *p = pg;
  if (p->running_id) {
    g_source_remove(p->running_id);
    p->running_id = 0;
  }
}

/* ------------------------------------------------------------------------ *
 * Row / details actions (GAction with the desktop ID as target)
 * ------------------------------------------------------------------------ */

static StartupEntry *
action_entry(StartupPage *p, GVariant *param)
{
  return param ? g_hash_table_lookup(p->by_id, g_variant_get_string(param, NULL)) : NULL;
}

static void
on_act_details(GSimpleAction *a, GVariant *param, gpointer pg)
{
  (void)a;
  StartupPage *p = pg;
  StartupEntry *e = action_entry(p, param);
  if (!e) return;
  startup_select_by_id(p, e->d->id);
  startup_select(p, e);
  if (adw_overlay_split_view_get_collapsed(ADW_OVERLAY_SPLIT_VIEW(p->split)))
    adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(p->split), TRUE);
}

static void on_act_edit(GSimpleAction *a, GVariant *param, gpointer pg)
{ (void)a; StartupEntry *e = action_entry(pg, param); if (e) startup_create_edit_dialog(pg, e); }

static void on_act_remove(GSimpleAction *a, GVariant *param, gpointer pg)
{ (void)a; StartupEntry *e = action_entry(pg, param); if (e) startup_confirm_remove(pg, e); }

static void
on_act_copy(GSimpleAction *a, GVariant *param, gpointer pg)
{
  (void)a;
  StartupPage *p = pg;
  StartupEntry *e = action_entry(p, param);
  if (!e || !e->d->exec) return;
  gdk_clipboard_set_text(gtk_widget_get_clipboard(p->root), e->d->exec);
  startup_toast(p, "Command copied");
}

static void
startup_install_actions(StartupPage *p)
{
  static const struct { const char *name; void (*cb)(GSimpleAction *, GVariant *, gpointer); } acts[] = {
    { "details", on_act_details }, { "edit", on_act_edit },
    { "remove", on_act_remove },   { "copy", on_act_copy },
  };
  GSimpleActionGroup *grp = g_simple_action_group_new();
  for (guint i = 0; i < G_N_ELEMENTS(acts); i++) {
    GSimpleAction *a = g_simple_action_new(acts[i].name, G_VARIANT_TYPE_STRING);
    g_signal_connect(a, "activate", G_CALLBACK(acts[i].cb), p);
    g_action_map_add_action(G_ACTION_MAP(grp), G_ACTION(a));
    g_object_unref(a);
  }
  gtk_widget_insert_action_group(p->root, "startup", G_ACTION_GROUP(grp));
  g_object_unref(grp);
}

/* ------------------------------------------------------------------------ *
 * Page construction / teardown
 * ------------------------------------------------------------------------ */

static void
add_bool_setter(AdwBreakpoint *bp, gpointer obj, const char *prop, gboolean v)
{
  GValue val = G_VALUE_INIT;
  g_value_init(&val, G_TYPE_BOOLEAN);
  g_value_set_boolean(&val, v);
  adw_breakpoint_add_setter(bp, G_OBJECT(obj), prop, &val);
  g_value_unset(&val);
}

static void
startup_page_free(gpointer data)
{
  StartupPage *p = data;
  g_clear_object(&p->selected);
  g_clear_object(&p->store);
  g_clear_object(&p->filtered);
  g_clear_object(&p->sorted);
  g_clear_object(&p->sel);
  g_clear_object(&p->filter);
  g_clear_object(&p->sorter);
  g_clear_pointer(&p->by_id, g_hash_table_unref);
  g_clear_pointer(&p->monitors, g_ptr_array_unref);
  g_clear_pointer(&p->monitored, g_hash_table_unref);
  g_free(p->search_text);
  g_free(p->select_after_id);
  g_free(p);
}

static void
on_page_destroy(GtkWidget *w, gpointer pg)
{
  (void)w;
  StartupPage *p = pg;
  p->destroyed = TRUE;
  if (p->debounce_id) { g_source_remove(p->debounce_id); p->debounce_id = 0; }
  if (p->running_id)  { g_source_remove(p->running_id);  p->running_id = 0; }
  for (guint i = 0; p->monitors && i < p->monitors->len; i++) {
    g_signal_handlers_disconnect_by_data(p->monitors->pdata[i], p);
    g_file_monitor_cancel(p->monitors->pdata[i]);
  }
  if (p->selected && p->selected_changed_id) {
    g_signal_handler_disconnect(p->selected, p->selected_changed_id);
    p->selected_changed_id = 0;
  }
}

GtkWidget *
startup_page_create(App *app)
{
  startup_install_css();

  StartupPage *p = g_new0(StartupPage, 1);
  p->app = app;
  p->filter_mode = FILTER_ALL;
  p->sort_mode = SORT_NAME;
  p->user_writable = TRUE;
  p->by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  p->monitors = g_ptr_array_new_with_free_func(g_object_unref);
  p->monitored = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  p->root = adw_toast_overlay_new();
  g_object_set_data_full(G_OBJECT(p->root), PAGE_KEY, p, startup_page_free);
  g_signal_connect(p->root, "destroy", G_CALLBACK(on_page_destroy), p);

  p->split = adw_overlay_split_view_new();

  /* main column */
  GtkWidget *main_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
  gtk_widget_set_margin_top(main_col, 18);    gtk_widget_set_margin_bottom(main_col, 18);
  gtk_widget_set_margin_start(main_col, 18);  gtk_widget_set_margin_end(main_col, 12);
  gtk_box_append(GTK_BOX(main_col), startup_create_header(p));
  gtk_box_append(GTK_BOX(main_col), startup_create_summary(p));
  gtk_box_append(GTK_BOX(main_col), startup_create_list(p));

  adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(p->split), main_col);
  adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(p->split), startup_create_details(p));
  adw_overlay_split_view_set_sidebar_position(ADW_OVERLAY_SPLIT_VIEW(p->split), GTK_PACK_END);
  adw_overlay_split_view_set_min_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(p->split), 340);
  adw_overlay_split_view_set_max_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(p->split), 460);
  adw_overlay_split_view_set_sidebar_width_fraction(ADW_OVERLAY_SPLIT_VIEW(p->split), 0.38);
  adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(p->split), TRUE);
  g_signal_connect(p->split, "notify::collapsed", G_CALLBACK(on_collapsed_changed), p);

  /* responsive behaviour: collapse details on narrow windows, trim header on very narrow */
  GtkWidget *bin = adw_breakpoint_bin_new();
  gtk_widget_set_size_request(bin, 360, 360);
  adw_breakpoint_bin_set_child(ADW_BREAKPOINT_BIN(bin), p->split);

  AdwBreakpoint *narrow = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 840sp"));
  add_bool_setter(narrow, p->split, "collapsed", TRUE);
  adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(bin), narrow);

  AdwBreakpoint *tiny = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 560sp"));
  add_bool_setter(tiny, p->sort_dd, "visible", FALSE);
  add_bool_setter(tiny, p->desc_label, "visible", FALSE);
  adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(bin), tiny);

  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(p->root), bin);

  startup_install_actions(p);
  startup_update_summary(p);

  g_signal_connect(p->root, "map", G_CALLBACK(on_page_map), p);
  g_signal_connect(p->root, "unmap", G_CALLBACK(on_page_unmap), p);

  startup_refresh(p);                     /* initial load (off the main thread) */
  return p->root;
}
