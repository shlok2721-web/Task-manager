/*
 * Page 7: Users & Sessions
 * Native GTK4/libadwaita implementation for Arch Linux.
 *
 * The module is deliberately self-contained.  Only users_page_create() is
 * exported.  Account data comes from the standard NSS account interface,
 * sessions come from systemd-logind over D-Bus, and resource accounting comes
 * from the application's shared process backend.
 *
 * No shell commands are used for routine discovery or updates.
 */

#include "users.h"
#include "../../app/app.h"

#include <adwaita.h>
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define LOGIND_BUS        "org.freedesktop.login1"
#define LOGIND_PATH       "/org/freedesktop/login1"
#define LOGIND_MANAGER    "org.freedesktop.login1.Manager"
#define LOGIND_SESSION    "org.freedesktop.login1.Session"
#define SAMPLE_INTERVAL   1000
#define DISCOVERY_INTERVAL 5000
#define MAX_USER_ROWS      4096

/* ------------------------------------------------------------------------- */
/* Structured model                                                          */
/* ------------------------------------------------------------------------- */

typedef enum {
    USER_FILTER_ALL,
    USER_FILTER_ACTIVE,
    USER_FILTER_INACTIVE,
    USER_FILTER_HUMAN,
    USER_FILTER_SYSTEM
} UserFilter;

typedef enum {
    USER_SORT_NAME,
    USER_SORT_LOGIN,
    USER_SORT_UID,
    USER_SORT_ACTIVE,
    USER_SORT_SESSIONS,
    USER_SORT_PROCESSES,
    USER_SORT_CPU,
    USER_SORT_MEMORY
} UserSort;

typedef struct {
    gchar *id;
    gchar *object_path;
    guint uid;
    gchar *username;
    gchar *seat;
    gchar *tty;
    gchar *display;
    gchar *remote_host;
    gchar *type;
    gchar *state;
    pid_t leader_pid;
    gint64 login_usec;
    gboolean remote;
    gboolean idle;
} SessionInfo;

typedef struct {
    gchar *login;
    gchar *display_name;
    uid_t uid;
    gid_t gid;
    gchar *home;
    gchar *shell;
    gchar *primary_group;
    gchar *classification;
    gchar *account_state;
    gchar *source;
    gchar *last_login;
    gboolean human;
    gboolean active;
    guint session_count;
    guint process_count;
    gdouble cpu_percent;
    guint64 memory_bytes;
    GPtrArray *sessions; /* SessionInfo*; non-owning pointers into page session model */
} UserInfo;

typedef struct {
    guint uid;
    guint process_count;
    guint64 memory_bytes;
    gdouble cpu_percent;
} ResourceUsage;

typedef struct {
    gchar *login;
    guint uid;
    guint process_count;
    guint64 memory_bytes;
    gdouble cpu_percent;
} ResourceResult;

typedef struct {
    gchar *session_id;
    guint uid;
    gchar *username;
    gchar *seat;
    gchar *tty;
    gchar *display;
    gchar *type;
    gchar *state;
    gboolean remote;
    gchar *remote_host;
    pid_t leader_pid;
    gint64 login_usec;
    gboolean idle;
} RawSession;

typedef struct _UsersPage UsersPage;

typedef struct {
    UsersPage *page;
    GObject *keepalive;
    GPtrArray *users;    /* UserInfo* */
    GPtrArray *sessions; /* SessionInfo* */
    guint generation;
    gboolean include_system;
} DiscoveryResult;

struct _UsersPage {
    App *app;

    GtkWidget *root;
    GtkWidget *search;
    GtkWidget *filter;
    GtkWidget *sort;
    GtkWidget *include_system_toggle;
    GtkWidget *status;
    GtkWidget *activity;
    GtkWidget *user_list;
    GtkWidget *details;
    GtkWidget *details_scroller;
    GtkWidget *session_list;
    GtkWidget *session_details;
    GtkWidget *terminate_button;
    GtkWidget *user_count;
    GtkWidget *active_count;
    GtkWidget *session_count;
    GtkWidget *graphical_count;
    GtkWidget *remote_count;
    GtkWidget *identity_group;
    GtkWidget *activity_group;
    GtkWidget *sessions_group;
    GtkWidget *selected_avatar;
    GtkWidget *selected_name;
    GtkWidget *selected_login;
    GtkWidget *selected_uid;
    GtkWidget *selected_gid;
    GtkWidget *selected_home;
    GtkWidget *selected_shell;
    GtkWidget *selected_class;
    GtkWidget *selected_state;
    GtkWidget *selected_source;
    GtkWidget *selected_last_login;
    GtkWidget *selected_processes;
    GtkWidget *selected_cpu;
    GtkWidget *selected_memory;
    GtkWidget *selected_session_count;
    GtkWidget *selected_session_header;

    GPtrArray *users;    /* UserInfo* */
    GPtrArray *sessions; /* SessionInfo* */
    UserInfo *selected_user;
    SessionInfo *selected_session;

    UserFilter filter_mode;
    UserSort sort_mode;
    gboolean include_system;

    GDBusConnection *bus;
    guint manager_signal_id;
    guint session_signal_id;
    guint sample_source;
    guint discovery_source;
    GMutex state_lock;
    gboolean worker_running;
    gboolean disposed;
    guint generation;
};

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static gchar *human_bytes(guint64 bytes)
{
    static const gchar *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    gdouble value = (gdouble)bytes;
    guint i = 0;
    while (value >= 1024.0 && i < G_N_ELEMENTS(units) - 1) {
        value /= 1024.0;
        ++i;
    }
    if (i == 0)
        return g_strdup_printf("%" G_GUINT64_FORMAT " B", bytes);
    return g_strdup_printf("%.1f %s", value, units[i]);
}

static gchar *human_cpu(gdouble value)
{
    if (!isfinite(value) || value < 0.0)
        value = 0.0;
    return g_strdup_printf("%.1f%%", value);
}

static gchar *human_time(gint64 usec)
{
    if (usec <= 0)
        return g_strdup("Unknown");

    time_t sec = (time_t)(usec / G_USEC_PER_SEC);
    struct tm tmv;
    if (!localtime_r(&sec, &tmv))
        return g_strdup("Unknown");

    gchar buf[64];
    if (strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv) == 0)
        return g_strdup("Unknown");
    return g_strdup(buf);
}

static const gchar *nonempty(const gchar *s)
{
    return (s && *s) ? s : "—";
}

static void set_label(GtkWidget *widget, const gchar *text)
{
    if (widget)
        gtk_label_set_text(GTK_LABEL(widget), text ? text : "—");
}

static GtkWidget *make_value_row(const gchar *key, GtkWidget **value_out)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(row, 4);
    gtk_widget_set_margin_bottom(row, 4);

    GtkWidget *k = gtk_label_new(key);
    gtk_label_set_xalign(GTK_LABEL(k), 0.0);
    gtk_widget_set_size_request(k, 125, -1);
    gtk_widget_add_css_class(k, "dim-label");

    GtkWidget *v = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(v), 0.0);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_label_set_wrap(GTK_LABEL(v), TRUE);

    gtk_box_append(GTK_BOX(row), k);
    gtk_box_append(GTK_BOX(row), v);
    if (value_out)
        *value_out = v;
    return row;
}

static GtkWidget *make_group(const gchar *title, GtkWidget **content_out)
{
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(frame, "card");
    gtk_widget_set_margin_top(frame, 6);
    gtk_widget_set_margin_bottom(frame, 6);

    GtkWidget *heading = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
    gtk_widget_add_css_class(heading, "heading");
    gtk_widget_set_margin_start(heading, 14);
    gtk_widget_set_margin_end(heading, 14);
    gtk_widget_set_margin_top(heading, 12);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(content, 14);
    gtk_widget_set_margin_end(content, 14);
    gtk_widget_set_margin_bottom(content, 12);

    gtk_box_append(GTK_BOX(frame), heading);
    gtk_box_append(GTK_BOX(frame), content);
    if (content_out)
        *content_out = content;
    return frame;
}

static GtkWidget *make_stat_card(const gchar *title, GtkWidget **value_out)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_set_margin_start(card, 4);
    gtk_widget_set_margin_end(card, 4);
    gtk_widget_set_margin_top(card, 4);
    gtk_widget_set_margin_bottom(card, 4);

    GtkWidget *value = gtk_label_new("0");
    gtk_label_set_xalign(GTK_LABEL(value), 0.0);
    gtk_widget_add_css_class(value, "title-2");
    gtk_widget_set_margin_start(value, 12);
    gtk_widget_set_margin_top(value, 10);

    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_bottom(label, 10);

    gtk_box_append(GTK_BOX(card), value);
    gtk_box_append(GTK_BOX(card), label);
    if (value_out)
        *value_out = value;
    return card;
}

static gchar *initials_for_user(const UserInfo *u)
{
    const gchar *name = (u && u->display_name && *u->display_name)
                      ? u->display_name : (u ? u->login : "?");
    gchar **parts = g_strsplit_set(name, " \t", -1);
    gchar *result = NULL;
    if (parts && parts[0] && parts[0][0]) {
        if (parts[1] && parts[1][0])
            result = g_strdup_printf("%c%c", parts[0][0], parts[1][0]);
        else
            result = g_strdup_printf("%c", parts[0][0]);
    } else {
        result = g_strdup("?");
    }
    g_strfreev(parts);
    return result;
}

/* ------------------------------------------------------------------------- */
/* Model lifetime                                                            */
/* ------------------------------------------------------------------------- */

static void session_free(SessionInfo *s)
{
    if (!s) return;
    g_free(s->id); g_free(s->object_path); g_free(s->username); g_free(s->seat); g_free(s->tty);
    g_free(s->display); g_free(s->remote_host); g_free(s->type); g_free(s->state);
    g_free(s);
}

static void user_free(UserInfo *u)
{
    if (!u) return;
    g_free(u->login); g_free(u->display_name); g_free(u->home); g_free(u->shell);
    g_free(u->primary_group); g_free(u->classification); g_free(u->account_state);
    g_free(u->source); g_free(u->last_login);
    if (u->sessions) g_ptr_array_free(u->sessions, TRUE);
    g_free(u);
}

static void discovery_result_free(DiscoveryResult *r)
{
    if (!r) return;
    if (r->keepalive) g_object_unref(r->keepalive);
    if (r->users) g_ptr_array_unref(r->users);
    if (r->sessions) g_ptr_array_unref(r->sessions);
    g_free(r);
}

/* ------------------------------------------------------------------------- */
/* Account discovery                                                         */
/* ------------------------------------------------------------------------- */

static gboolean passwd_name_is_system(const struct passwd *pw)
{
    if (!pw || !pw->pw_name)
        return TRUE;

    /* UID ranges are only a default project classification, never identity. */
    if (pw->pw_uid < 1000)
        return TRUE;

    if (!pw->pw_shell || !*pw->pw_shell)
        return TRUE;

    if (g_str_has_suffix(pw->pw_shell, "/nologin") ||
        g_str_has_suffix(pw->pw_shell, "/false"))
        return TRUE;

    return FALSE;
}

static gchar *passwd_display_name(const struct passwd *pw)
{
    if (!pw) return g_strdup("Unknown");
    if (pw->pw_gecos && *pw->pw_gecos) {
        gchar *comma = strchr(pw->pw_gecos, ',');
        if (comma)
            return g_strndup(pw->pw_gecos, (gsize)(comma - pw->pw_gecos));
        return g_strdup(pw->pw_gecos);
    }
    return g_strdup(pw->pw_name ? pw->pw_name : "Unknown");
}

static gchar *group_name_from_gid(gid_t gid)
{
    struct group *gr = getgrgid(gid);
    return gr && gr->gr_name ? g_strdup(gr->gr_name) : g_strdup("Unknown");
}

static UserInfo *user_from_passwd(const struct passwd *pw)
{
    UserInfo *u = g_new0(UserInfo, 1);
    u->login = g_strdup(pw->pw_name ? pw->pw_name : "unknown");
    u->display_name = passwd_display_name(pw);
    u->uid = pw->pw_uid;
    u->gid = pw->pw_gid;
    u->home = g_strdup(pw->pw_dir ? pw->pw_dir : "");
    u->shell = g_strdup(pw->pw_shell ? pw->pw_shell : "");
    u->primary_group = group_name_from_gid(pw->pw_gid);
    u->human = !passwd_name_is_system(pw);
    u->classification = g_strdup(u->human ? "Human user" : "System account");
    u->account_state = g_strdup("Present");
    u->source = g_strdup("NSS / passwd");
    u->last_login = g_strdup("Unavailable");
    u->sessions = g_ptr_array_new();
    return u;
}

static GPtrArray *discover_accounts(gboolean include_system)
{
    GPtrArray *users = g_ptr_array_new_with_free_func((GDestroyNotify)user_free);
    struct passwd *pw;

    errno = 0;
    setpwent();
    while ((pw = getpwent()) != NULL) {
        gboolean human = !passwd_name_is_system(pw);
        if (!include_system && !human)
            continue;
        if (users->len >= MAX_USER_ROWS)
            break;
        g_ptr_array_add(users, user_from_passwd(pw));
    }
    endpwent();
    return users;
}

static UserInfo *find_user(GPtrArray *users, guint uid)
{
    for (guint i = 0; i < users->len; ++i) {
        UserInfo *u = g_ptr_array_index(users, i);
        if (u->uid == uid)
            return u;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* logind session discovery                                                  */
/* ------------------------------------------------------------------------- */

static SessionInfo *session_from_variant(GVariant *tuple)
{
    /* ListSessions entry: (susso) => id, uid, user, seat, object path. */
    const gchar *id = NULL, *username = NULL, *seat = NULL, *path = NULL;
    guint uid = 0;

    if (!g_variant_is_of_type(tuple, G_VARIANT_TYPE("(susso)")))
        return NULL;
    g_variant_get(tuple, "(&su&s&s&o)", &id, &uid, &username, &seat, &path);

    SessionInfo *s = g_new0(SessionInfo, 1);
    s->id = g_strdup(id);
    s->object_path = g_strdup(path);
    s->uid = uid;
    s->username = g_strdup(username);
    s->seat = g_strdup(seat ? seat : "");
    return s;
}

static void session_read_properties(GDBusConnection *bus, SessionInfo *s)
{
    if (!bus || !s || !s->id)
        return;

    gchar *object_path = g_strdup(s->object_path);
    if (!object_path)
        return;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        bus, LOGIND_BUS, object_path,
        "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new("(s)", LOGIND_SESSION),
        G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);

    g_free(object_path);
    if (!reply) {
        g_clear_error(&error);
        return;
    }

    GVariant *dict = NULL;
    g_variant_get(reply, "(@a{sv})", &dict);
    GVariantIter iter;
    const gchar *key;
    GVariant *value;
    g_variant_iter_init(&iter, dict);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
        if (g_str_equal(key, "Type")) {
            g_free(s->type); s->type = g_variant_dup_string(value, NULL);
        } else if (g_str_equal(key, "State")) {
            g_free(s->state); s->state = g_variant_dup_string(value, NULL);
        } else if (g_str_equal(key, "TTY")) {
            g_free(s->tty); s->tty = g_variant_dup_string(value, NULL);
        } else if (g_str_equal(key, "Remote")) {
            s->remote = g_variant_get_boolean(value);
        } else if (g_str_equal(key, "RemoteHost")) {
            g_free(s->remote_host); s->remote_host = g_variant_dup_string(value, NULL);
        } else if (g_str_equal(key, "Leader")) {
            s->leader_pid = (pid_t)g_variant_get_uint32(value);
        } else if (g_str_equal(key, "Timestamp")) {
            s->login_usec = (gint64)g_variant_get_uint64(value);
        } else if (g_str_equal(key, "IdleHint")) {
            s->idle = g_variant_get_boolean(value);
        } else if (g_str_equal(key, "Display")) {
            g_free(s->display); s->display = g_variant_dup_string(value, NULL);
        }
        g_variant_unref(value);
    }
    g_variant_unref(dict);
    g_variant_unref(reply);
}

static GPtrArray *discover_sessions_sync(GDBusConnection *bus)
{
    GPtrArray *sessions = g_ptr_array_new_with_free_func((GDestroyNotify)session_free);
    if (!bus)
        return sessions;

    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        bus, LOGIND_BUS, LOGIND_PATH, LOGIND_MANAGER, "ListSessions",
        NULL, G_VARIANT_TYPE("(a(susso))"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);

    if (!reply) {
        g_clear_error(&error);
        return sessions;
    }

    GVariant *array = NULL;
    g_variant_get(reply, "(@a(susso))", &array);
    GVariantIter iter;
    GVariant *entry;
    g_variant_iter_init(&iter, array);
    while ((entry = g_variant_iter_next_value(&iter)) != NULL) {
        SessionInfo *s = session_from_variant(entry);
        g_variant_unref(entry);
        if (!s)
            continue;
        session_read_properties(bus, s);
        if (!s->type) s->type = g_strdup("unknown");
        if (!s->state) s->state = g_strdup("unknown");
        g_ptr_array_add(sessions, s);
    }
    g_variant_unref(array);
    g_variant_unref(reply);
    return sessions;
}

static void attach_sessions(GPtrArray *users, GPtrArray *sessions)
{
    for (guint i = 0; i < sessions->len; ++i) {
        SessionInfo *s = g_ptr_array_index(sessions, i);
        UserInfo *u = find_user(users, s->uid);
        if (!u && s->username) {
            for (guint j = 0; j < users->len; ++j) {
                UserInfo *candidate = g_ptr_array_index(users, j);
                if (g_strcmp0(candidate->login, s->username) == 0) {
                    u = candidate;
                    break;
                }
            }
        }
        if (!u)
            continue;
        g_ptr_array_add(u->sessions, s);
        u->session_count++;
        u->active = TRUE;
    }
}

/* ------------------------------------------------------------------------- */
/* Shared process-backend resource aggregation                              */
/* ------------------------------------------------------------------------- */

static GPtrArray *collect_resources(UsersPage *page)
{
    GHashTable *agg = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    GPtrArray *processes = app_get_processes(page->app);
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count <= 0) cpu_count = 1;

    if (processes) {
        for (guint i = 0; i < processes->len; ++i) {
            const ProcessInfo *info = g_ptr_array_index(processes, i);
            if (!info)
                continue;

            ResourceResult *rr = g_hash_table_lookup(agg, GUINT_TO_POINTER(info->uid));
            if (!rr) {
                rr = g_new0(ResourceResult, 1);
                rr->uid = info->uid;
                g_hash_table_insert(agg, GUINT_TO_POINTER(info->uid), rr);
            }
            rr->process_count++;
            rr->memory_bytes += info->rss_bytes;
            rr->cpu_percent += info->cpu_percent / (gdouble)cpu_count;
        }
    }

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, agg);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        ResourceResult *src = value;
        ResourceResult *copy = g_new0(ResourceResult, 1);
        copy->uid = src->uid;
        copy->process_count = src->process_count;
        copy->memory_bytes = src->memory_bytes;
        copy->cpu_percent = src->cpu_percent;
        g_ptr_array_add(out, copy);
    }
    g_hash_table_destroy(agg);
    return out;
}

static void apply_resources(GPtrArray *users, GPtrArray *resources)
{
    for (guint i = 0; i < resources->len; ++i) {
        ResourceResult *r = g_ptr_array_index(resources, i);
        UserInfo *u = find_user(users, r->uid);
        if (!u) continue;
        u->process_count = r->process_count;
        u->memory_bytes = r->memory_bytes;
        u->cpu_percent = r->cpu_percent;
    }
}

/* ------------------------------------------------------------------------- */
/* Worker discovery                                                          */
/* ------------------------------------------------------------------------- */

typedef struct {
    UsersPage *page;
    guint generation;
    GObject *keepalive;
} DiscoveryTask;

static gboolean discovery_apply_cb(gpointer data);

static gpointer discovery_worker(gpointer data)
{
    DiscoveryTask *task = data;
    UsersPage *page = task->page;

    DiscoveryResult *result = g_new0(DiscoveryResult, 1);
    result->page = page;
    result->keepalive = task->keepalive;
    result->generation = task->generation;
    result->users = discover_accounts(page->include_system || page->filter_mode == USER_FILTER_SYSTEM);
    result->sessions = discover_sessions_sync(page->bus);
    attach_sessions(result->users, result->sessions);
    g_main_context_invoke(NULL, discovery_apply_cb, result);
    g_free(task);
    return NULL;
}

static void start_discovery(UsersPage *page)
{
    if (!page || page->disposed)
        return;

    g_mutex_lock(&page->state_lock);
    if (page->worker_running) {
        g_mutex_unlock(&page->state_lock);
        return;
    }
    page->worker_running = TRUE;
    guint generation = ++page->generation;
    g_mutex_unlock(&page->state_lock);

    DiscoveryTask *task = g_new0(DiscoveryTask, 1);
    task->page = page;
    task->generation = generation;
    g_object_ref(page->root);
    task->keepalive = G_OBJECT(page->root);
    GThread *thread = g_thread_new("users-discovery", discovery_worker, task);
    g_thread_unref(thread);
}

/* ------------------------------------------------------------------------- */
/* UI model helpers                                                          */
/* ------------------------------------------------------------------------- */

static gboolean user_matches(UsersPage *page, UserInfo *u)
{
    if (!u) return FALSE;
    if (!page->include_system && !u->human && page->filter_mode != USER_FILTER_SYSTEM)
        return FALSE;

    switch (page->filter_mode) {
    case USER_FILTER_ACTIVE: return u->active;
    case USER_FILTER_INACTIVE: return !u->active;
    case USER_FILTER_HUMAN: return u->human;
    case USER_FILTER_SYSTEM: return !u->human;
    case USER_FILTER_ALL: default: return TRUE;
    }
}

static gboolean user_search_matches(UsersPage *page, UserInfo *u)
{
    const gchar *query = gtk_editable_get_text(GTK_EDITABLE(page->search));
    if (!query || !*query)
        return TRUE;

    gchar *needle = g_utf8_casefold(query, -1);
    gchar *uid = g_strdup_printf("%u", u->uid);
    gchar *hay[] = {
        g_utf8_casefold(nonempty(u->display_name), -1),
        g_utf8_casefold(nonempty(u->login), -1),
        g_utf8_casefold(uid, -1),
        g_utf8_casefold(nonempty(u->home), -1),
        NULL
    };
    gboolean found = FALSE;
    for (guint i = 0; hay[i]; ++i) {
        if (g_strstr_len(hay[i], -1, needle)) {
            found = TRUE;
            break;
        }
    }
    for (guint i = 0; hay[i]; ++i) g_free(hay[i]);
    g_free(uid);
    g_free(needle);

    if (found) return TRUE;
    for (guint i = 0; i < u->sessions->len; ++i) {
        SessionInfo *s = g_ptr_array_index(u->sessions, i);
        gchar *fields = g_strdup_printf("%s %s %s %s %s",
                                        nonempty(s->id), nonempty(s->tty),
                                        nonempty(s->remote_host), nonempty(s->seat),
                                        nonempty(s->type));
        gchar *folded = g_utf8_casefold(fields, -1);
        gchar *n = g_utf8_casefold(query, -1);
        if (g_strstr_len(folded, -1, n)) {
            found = TRUE;
            g_free(folded); g_free(n); g_free(fields);
            break;
        }
        g_free(folded); g_free(n); g_free(fields);
    }
    return found;
}

static gint user_compare(gconstpointer a, gconstpointer b, gpointer data)
{
    UsersPage *page = data;
    const UserInfo *x = *(UserInfo * const *)a;
    const UserInfo *y = *(UserInfo * const *)b;
    gint c = 0;
    switch (page->sort_mode) {
    case USER_SORT_LOGIN: c = g_ascii_strcasecmp(nonempty(x->login), nonempty(y->login)); break;
    case USER_SORT_UID: c = x->uid < y->uid ? -1 : x->uid > y->uid; break;
    case USER_SORT_ACTIVE: c = (gint)y->active - (gint)x->active; break;
    case USER_SORT_SESSIONS: c = y->session_count - x->session_count; break;
    case USER_SORT_PROCESSES: c = y->process_count - x->process_count; break;
    case USER_SORT_CPU: c = y->cpu_percent > x->cpu_percent ? 1 : y->cpu_percent < x->cpu_percent ? -1 : 0; break;
    case USER_SORT_MEMORY: c = y->memory_bytes > x->memory_bytes ? 1 : y->memory_bytes < x->memory_bytes ? -1 : 0; break;
    case USER_SORT_NAME: default: c = g_ascii_strcasecmp(nonempty(x->display_name), nonempty(y->display_name)); break;
    }
    if (c == 0) c = g_ascii_strcasecmp(nonempty(x->login), nonempty(y->login));
    if (c == 0) c = x->uid < y->uid ? -1 : x->uid > y->uid;
    return c;
}

static void clear_container(GtkWidget *box)
{
    GtkWidget *child = gtk_widget_get_first_child(box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(box), child);
        child = next;
    }
}

static void select_user(UsersPage *page, UserInfo *user);
static void select_session(UsersPage *page, SessionInfo *session);

static GtkWidget *create_user_row(UsersPage *page, UserInfo *u)
{
    GtkWidget *button = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
    gtk_widget_set_hexpand(button, TRUE);
    gtk_widget_add_css_class(button, "flat");
    g_object_set_data(G_OBJECT(button), "user-info", u);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(row, 8); gtk_widget_set_margin_end(row, 8);
    gtk_widget_set_margin_top(row, 7); gtk_widget_set_margin_bottom(row, 7);

    GtkWidget *avatar = gtk_label_new(NULL);
    gchar *initials = initials_for_user(u);
    gtk_label_set_text(GTK_LABEL(avatar), initials);
    g_free(initials);
    gtk_widget_set_size_request(avatar, 38, 38);
    gtk_widget_add_css_class(avatar, "avatar");

    GtkWidget *identity = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(identity, TRUE);
    GtkWidget *name = gtk_label_new(nonempty(u->display_name));
    gtk_label_set_xalign(GTK_LABEL(name), 0.0);
    gtk_widget_add_css_class(name, "heading");
    GtkWidget *login = gtk_label_new(NULL);
    gchar *login_text = g_strdup_printf("%s · UID %u", nonempty(u->login), u->uid);
    gtk_label_set_text(GTK_LABEL(login), login_text);
    g_free(login_text);
    gtk_label_set_xalign(GTK_LABEL(login), 0.0);
    gtk_widget_add_css_class(login, "dim-label");
    gtk_box_append(GTK_BOX(identity), name);
    gtk_box_append(GTK_BOX(identity), login);

    GtkWidget *metrics = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_halign(metrics, GTK_ALIGN_END);
    gchar *cpu = human_cpu(u->cpu_percent);
    gchar *mem = human_bytes(u->memory_bytes);
    gchar *metric_text = g_strdup_printf("%u proc · %s · %s", u->process_count, cpu, mem);
    GtkWidget *metric = gtk_label_new(metric_text);
    g_free(metric_text); g_free(cpu); g_free(mem);
    gtk_label_set_xalign(GTK_LABEL(metric), 1.0);
    GtkWidget *sessions = gtk_label_new(NULL);
    gchar *session_text = g_strdup_printf("%u session%s · %s",
                                          u->session_count,
                                          u->session_count == 1 ? "" : "s",
                                          u->active ? "Online" : "Offline");
    gtk_label_set_text(GTK_LABEL(sessions), session_text);
    g_free(session_text);
    gtk_label_set_xalign(GTK_LABEL(sessions), 1.0);
    gtk_widget_add_css_class(sessions, u->active ? "success" : "dim-label");
    gtk_box_append(GTK_BOX(metrics), metric);
    gtk_box_append(GTK_BOX(metrics), sessions);

    gtk_box_append(GTK_BOX(row), avatar);
    gtk_box_append(GTK_BOX(row), identity);
    gtk_box_append(GTK_BOX(row), metrics);
    gtk_button_set_child(GTK_BUTTON(button), row);
    g_signal_connect_swapped(button, "clicked", G_CALLBACK(select_user), page);
    return button;
}

static void rebuild_user_list(UsersPage *page)
{
    clear_container(GTK_WIDGET(page->user_list));

    GPtrArray *visible = g_ptr_array_new();
    for (guint i = 0; i < page->users->len; ++i) {
        UserInfo *u = g_ptr_array_index(page->users, i);
        if (user_matches(page, u) && user_search_matches(page, u))
            g_ptr_array_add(visible, u);
    }
    g_ptr_array_sort_with_data(visible, user_compare, page);

    for (guint i = 0; i < visible->len; ++i) {
        UserInfo *u = g_ptr_array_index(visible, i);
        GtkWidget *row = create_user_row(page, u);
        if (u == page->selected_user)
            gtk_widget_add_css_class(row, "selected");
        gtk_box_append(GTK_BOX(page->user_list), row);
    }
    g_ptr_array_free(visible, TRUE);
}

/* ------------------------------------------------------------------------- */
/* Details                                                                   */
/* ------------------------------------------------------------------------- */

static void update_selected_details(UsersPage *page)
{
    UserInfo *u = page->selected_user;
    if (!u) {
        set_label(page->selected_avatar, "?");
        set_label(page->selected_name, "No user selected");
        set_label(page->selected_login, "—");
        set_label(page->selected_uid, "—");
        set_label(page->selected_gid, "—");
        set_label(page->selected_home, "—");
        set_label(page->selected_shell, "—");
        set_label(page->selected_class, "—");
        set_label(page->selected_state, "—");
        set_label(page->selected_source, "—");
        set_label(page->selected_last_login, "—");
        set_label(page->selected_processes, "—");
        set_label(page->selected_cpu, "—");
        set_label(page->selected_memory, "—");
        set_label(page->selected_session_count, "—");
        clear_container(GTK_WIDGET(page->session_list));
        return;
    }

    gchar *uid = g_strdup_printf("%u", u->uid);
    gchar *gid = g_strdup_printf("%u (%s)", u->gid, nonempty(u->primary_group));
    gchar *proc = g_strdup_printf("%u", u->process_count);
    gchar *cpu = human_cpu(u->cpu_percent);
    gchar *mem = human_bytes(u->memory_bytes);
    gchar *sessions = g_strdup_printf("%u", u->session_count);

    set_label(page->selected_name, nonempty(u->display_name));
    set_label(page->selected_login, nonempty(u->login));
    gchar *initials = initials_for_user(u);
    set_label(page->selected_avatar, initials);
    g_free(initials);
    set_label(page->selected_uid, uid);
    set_label(page->selected_gid, gid);
    set_label(page->selected_home, nonempty(u->home));
    set_label(page->selected_shell, nonempty(u->shell));
    set_label(page->selected_class, nonempty(u->classification));
    set_label(page->selected_state, nonempty(u->account_state));
    set_label(page->selected_source, nonempty(u->source));
    set_label(page->selected_last_login, nonempty(u->last_login));
    set_label(page->selected_processes, proc);
    set_label(page->selected_cpu, cpu);
    set_label(page->selected_memory, mem);
    set_label(page->selected_session_count, sessions);

    g_free(uid); g_free(gid); g_free(proc); g_free(cpu); g_free(mem); g_free(sessions);

    gchar *title = g_strdup_printf("Sessions · %u", u->session_count);
    set_label(page->selected_session_header, title);
    g_free(title);

    clear_container(GTK_WIDGET(page->session_list));
    for (guint i = 0; i < u->sessions->len; ++i) {
        SessionInfo *s = g_ptr_array_index(u->sessions, i);
        GtkWidget *button = gtk_button_new();
        gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
        gtk_widget_set_hexpand(button, TRUE);
        gtk_widget_add_css_class(button, "flat");
        g_object_set_data(G_OBJECT(button), "session-info", s);

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(row, 8); gtk_widget_set_margin_end(row, 8);
        gtk_widget_set_margin_top(row, 6); gtk_widget_set_margin_bottom(row, 6);

        GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        gtk_widget_set_hexpand(left, TRUE);
        gchar *main = g_strdup_printf("%s · %s", nonempty(s->id), nonempty(s->type));
        GtkWidget *main_l = gtk_label_new(main); g_free(main);
        gtk_label_set_xalign(GTK_LABEL(main_l), 0.0);
        gtk_widget_add_css_class(main_l, "heading");
        gchar *sub = g_strdup_printf("%s · %s · %s",
                                     nonempty(s->seat), nonempty(s->tty),
                                     s->remote ? "Remote" : "Local");
        GtkWidget *sub_l = gtk_label_new(sub); g_free(sub);
        gtk_label_set_xalign(GTK_LABEL(sub_l), 0.0);
        gtk_widget_add_css_class(sub_l, "dim-label");
        gtk_box_append(GTK_BOX(left), main_l);
        gtk_box_append(GTK_BOX(left), sub_l);

        gchar *when = human_time(s->login_usec);
        GtkWidget *time_l = gtk_label_new(when); g_free(when);
        gtk_label_set_xalign(GTK_LABEL(time_l), 1.0);
        gtk_widget_add_css_class(time_l, "dim-label");

        gtk_box_append(GTK_BOX(row), left);
        gtk_box_append(GTK_BOX(row), time_l);
        gtk_button_set_child(GTK_BUTTON(button), row);
        g_signal_connect_swapped(button, "clicked", G_CALLBACK(select_session), page);
        gtk_box_append(GTK_BOX(page->session_list), button);
    }

    page->selected_session = NULL;
    gtk_widget_set_sensitive(page->terminate_button, FALSE);
}

static void update_session_details(UsersPage *page)
{
    clear_container(GTK_WIDGET(page->session_details));
    SessionInfo *s = page->selected_session;
    if (!s) {
        GtkWidget *label = gtk_label_new("Select a session to view technical details.");
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        gtk_widget_add_css_class(label, "dim-label");
        gtk_box_append(GTK_BOX(page->session_details), label);
        gtk_widget_set_sensitive(page->terminate_button, FALSE);
        return;
    }

    GtkWidget *v;
    GtkWidget *r;
    gchar *uid = g_strdup_printf("%u", s->uid);
    gchar *leader = g_strdup_printf("%d", (int)s->leader_pid);
    gchar *remote = g_strdup_printf("%s%s%s", s->remote ? "Remote" : "Local",
                                    s->remote && s->remote_host && *s->remote_host ? " · " : "",
                                    s->remote && s->remote_host && *s->remote_host ? s->remote_host : "");
    gchar *login = human_time(s->login_usec);
    gchar *idle = g_strdup(s->idle ? "Idle" : "Active");

    r = make_value_row("Session ID", &v); set_label(v, nonempty(s->id)); gtk_box_append(GTK_BOX(page->session_details), r);
    r = make_value_row("User", &v); set_label(v, nonempty(s->username)); gtk_box_append(GTK_BOX(page->session_details), r);
    r = make_value_row("UID", &v); set_label(v, uid); gtk_box_append(GTK_BOX(page->session_details), r);
    r = make_value_row("Seat", &v); set_label(v, nonempty(s->seat)); gtk_box_append(GTK_BOX(page->session_details), r);
    r = make_value_row("Display", &v); set_label(v, nonempty(s->display)); gtk_box_append(GTK_BOX(page->session_details), r);
    r = make_value_row("Type", &v); set_label(v, nonempty(s->type)); gtk_box_append(GTK_BOX(page->session_details), r);
    r = make_value_row("TTY", &v); set_label(v, nonempty(s->tty)); gtk_box_append(GTK_BOX(page->session_details), r);
    r = make_value_row("State", &v); set_label(v, nonempty(s->state)); gtk_box_append(GTK_BOX(page->session_details), r);
    r = make_value_row("Location", &v); set_label(v, remote); gtk_box_append(GTK_BOX(page->session_details), r);
    r = make_value_row("Leader PID", &v); set_label(v, leader); gtk_box_append(GTK_BOX(page->session_details), r);
    r = make_value_row("Login time", &v); set_label(v, login); gtk_box_append(GTK_BOX(page->session_details), r);
    r = make_value_row("Idle", &v); set_label(v, idle); gtk_box_append(GTK_BOX(page->session_details), r);

    g_free(uid); g_free(leader); g_free(remote); g_free(login); g_free(idle);
    gtk_widget_set_sensitive(page->terminate_button, TRUE);
}

static void select_user(UsersPage *page, UserInfo *user)
{
    if (!page) return;
    page->selected_user = user;
    page->selected_session = NULL;
    update_selected_details(page);
    update_session_details(page);
    rebuild_user_list(page);
}

static void select_session(UsersPage *page, SessionInfo *session)
{
    if (!page) return;
    page->selected_session = session;
    update_session_details(page);
}

/* ------------------------------------------------------------------------- */
/* Summary                                                                   */
/* ------------------------------------------------------------------------- */

static void update_summary(UsersPage *page)
{
    guint active_users = 0, active_sessions = 0, graphical = 0, remote = 0;
    for (guint i = 0; i < page->users->len; ++i) {
        UserInfo *u = g_ptr_array_index(page->users, i);
        if (u->active) active_users++;
    }
    for (guint i = 0; i < page->sessions->len; ++i) {
        SessionInfo *s = g_ptr_array_index(page->sessions, i);
        active_sessions++;
        if (g_strcmp0(s->type, "x11") == 0 || g_strcmp0(s->type, "wayland") == 0 ||
            g_strcmp0(s->type, "mir") == 0 || g_strcmp0(s->type, "desktop") == 0)
            graphical++;
        if (s->remote) remote++;
    }
    gchar *n;
    n = g_strdup_printf("%u", page->users->len); set_label(page->user_count, n); g_free(n);
    n = g_strdup_printf("%u", active_users); set_label(page->active_count, n); g_free(n);
    n = g_strdup_printf("%u", active_sessions); set_label(page->session_count, n); g_free(n);
    n = g_strdup_printf("%u", graphical); set_label(page->graphical_count, n); g_free(n);
    n = g_strdup_printf("%u", remote); set_label(page->remote_count, n); g_free(n);
}

/* ------------------------------------------------------------------------- */
/* Discovery result application                                              */
/* ------------------------------------------------------------------------- */

static gboolean discovery_apply_cb(gpointer data)
{
    DiscoveryResult *result = data;
    UsersPage *page = result->page;
    if (!page || page->disposed) {
        discovery_result_free(result);
        return G_SOURCE_REMOVE;
    }

    g_mutex_lock(&page->state_lock);
    page->worker_running = FALSE;
    gboolean current = result->generation == page->generation;
    g_mutex_unlock(&page->state_lock);
    if (!current) {
        discovery_result_free(result);
        return G_SOURCE_REMOVE;
    }

    gchar *selected_login = page->selected_user ? g_strdup(page->selected_user->login) : NULL;
    gchar *selected_session_id = page->selected_session ? g_strdup(page->selected_session->id) : NULL;

    if (page->users) g_ptr_array_unref(page->users);
    if (page->sessions) g_ptr_array_unref(page->sessions);
    page->users = g_ptr_array_ref(result->users);
    page->sessions = g_ptr_array_ref(result->sessions);

    GPtrArray *resources = collect_resources(page);
    apply_resources(page->users, resources);
    g_ptr_array_free(resources, TRUE);

    page->selected_user = NULL;
    page->selected_session = NULL;
    if (selected_login) {
        for (guint i = 0; i < page->users->len; ++i) {
            UserInfo *u = g_ptr_array_index(page->users, i);
            if (g_strcmp0(u->login, selected_login) == 0) {
                page->selected_user = u;
                break;
            }
        }
    }
    if (!page->selected_user && page->users->len > 0)
        page->selected_user = g_ptr_array_index(page->users, 0);
    if (selected_session_id && page->selected_user) {
        for (guint i = 0; i < page->selected_user->sessions->len; ++i) {
            SessionInfo *s = g_ptr_array_index(page->selected_user->sessions, i);
            if (g_strcmp0(s->id, selected_session_id) == 0) {
                page->selected_session = s;
                break;
            }
        }
    }
    g_free(selected_login);
    g_free(selected_session_id);

    rebuild_user_list(page);
    update_summary(page);
    update_selected_details(page);
    update_session_details(page);
    set_label(page->status, "Updated");
    gtk_spinner_stop(GTK_SPINNER(page->activity));
    discovery_result_free(result);
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------------- */
/* logind event monitoring                                                   */
/* ------------------------------------------------------------------------- */

static void logind_signal_cb(GDBusConnection *connection,
                             const gchar *sender_name,
                             const gchar *object_path,
                             const gchar *interface_name,
                             const gchar *signal_name,
                             GVariant *parameters,
                             gpointer user_data)
{
    (void)connection; (void)sender_name; (void)object_path;
    (void)interface_name; (void)parameters;
    UsersPage *page = user_data;
    if (!page || page->disposed)
        return;

    if (g_str_equal(signal_name, "SessionNew") ||
        g_str_equal(signal_name, "SessionRemoved") ||
        g_str_equal(signal_name, "UserNew") ||
        g_str_equal(signal_name, "UserRemoved") ||
        g_str_equal(signal_name, "PropertiesChanged")) {
        gtk_spinner_start(GTK_SPINNER(page->activity));
        set_label(page->status, "Session activity detected…");
        start_discovery(page);
    }
}

static void logind_bus_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)source;
    UsersPage *page = user_data;
    if (!page)
        return;

    GError *error = NULL;
    page->bus = g_bus_get_finish(result, &error);
    if (!page->bus) {
        if (!page->disposed)
            set_label(page->status, "logind unavailable");
        g_clear_error(&error);
        g_object_unref(page->root);
        return;
    }

    page->manager_signal_id = g_dbus_connection_signal_subscribe(
        page->bus, LOGIND_BUS, LOGIND_MANAGER, NULL, LOGIND_PATH,
        NULL, G_DBUS_SIGNAL_FLAGS_NONE, logind_signal_cb, page, NULL);

    /* Session properties such as State/IdleHint can change without a
     * SessionNew/SessionRemoved event. Filter PropertiesChanged to logind's
     * Session interface instead of polling the bus with shell commands. */
    page->session_signal_id = g_dbus_connection_signal_subscribe(
        page->bus, LOGIND_BUS, "org.freedesktop.DBus.Properties", "PropertiesChanged", NULL,
        "org.freedesktop.login1.Session", G_DBUS_SIGNAL_FLAGS_NONE,
        logind_signal_cb, page, NULL);

    if (!page->disposed) {
        set_label(page->status, "logind connected");
        start_discovery(page);
    }
    g_object_unref(page->root);
}

static void connect_logind(UsersPage *page)
{
    g_object_ref(page->root);
    g_bus_get(G_BUS_TYPE_SYSTEM, NULL, logind_bus_ready, page);
}

/* ------------------------------------------------------------------------- */
/* Session termination                                                       */
/* ------------------------------------------------------------------------- */

static void terminate_call_done(GObject *source, GAsyncResult *res, gpointer data)
{
    UsersPage *page = data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (!reply) {
        GtkAlertDialog *dialog = gtk_alert_dialog_new("Could not terminate the session: %s",
                                                      error ? error->message : "Unknown error");
        gtk_alert_dialog_show(dialog, GTK_WINDOW(gtk_widget_get_root(page->root)));
        g_object_unref(dialog);
        g_clear_error(&error);
    } else {
        g_variant_unref(reply);
        set_label(page->status, "Session termination requested");
        start_discovery(page);
    }
}

static void terminate_session_confirmed(UsersPage *page)
{
    SessionInfo *s = page->selected_session;
    if (!s || !page->bus || !s->id)
        return;

    g_dbus_connection_call(page->bus, LOGIND_BUS, LOGIND_PATH,
                           LOGIND_MANAGER, "TerminateSession",
                           g_variant_new("(s)", s->id), NULL,
                           G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                           terminate_call_done, page);
}

static void terminate_response(GObject *source, GAsyncResult *result, gpointer data)
{
    UsersPage *page = data;
    GtkAlertDialog *dialog = GTK_ALERT_DIALOG(source);
    GError *error = NULL;
    int response = gtk_alert_dialog_choose_finish(dialog, result, &error);
    if (error) {
        g_clear_error(&error);
        return;
    }
    if (response == 1)
        terminate_session_confirmed(page);
}

static void terminate_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    UsersPage *page = data;
    SessionInfo *s = page->selected_session;
    if (!s) return;

    const gchar *name = page->selected_user ? nonempty(page->selected_user->display_name) : nonempty(s->username);
    gchar *message = g_strdup_printf("Terminate session %s for %s?\n\nThis will log the user out of this session.",
                                     nonempty(s->id), name);
    const gchar *labels[] = {"Cancel", "Terminate", NULL};
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", message);
    gtk_alert_dialog_set_detail(dialog, "The action applies only to the selected session.");
    gtk_alert_dialog_set_buttons(dialog, labels);
    gtk_alert_dialog_set_default_button(dialog, 0);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    g_free(message);
    gtk_alert_dialog_choose(dialog, GTK_WINDOW(gtk_widget_get_root(page->root)), NULL,
                            terminate_response, page);
    g_object_unref(dialog);
}

/* ------------------------------------------------------------------------- */
/* UI construction                                                           */
/* ------------------------------------------------------------------------- */

static void filter_changed(GObject *object, GParamSpec *pspec, gpointer data);
static void sort_changed(GObject *object, GParamSpec *pspec, gpointer data);
static void include_system_toggled(GtkCheckButton *button, gpointer data);

static GtkWidget *users_create_header(UsersPage *page)
{
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(header, 14);
    gtk_widget_set_margin_end(header, 14);
    gtk_widget_set_margin_top(header, 12);
    gtk_widget_set_margin_bottom(header, 8);

    GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(titles, TRUE);
    GtkWidget *title = gtk_label_new("Users & Sessions");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_widget_add_css_class(title, "title-2");
    GtkWidget *description = gtk_label_new("Local accounts, active login sessions, and per-user resource usage.");
    gtk_label_set_xalign(GTK_LABEL(description), 0.0);
    gtk_widget_add_css_class(description, "dim-label");
    gtk_box_append(GTK_BOX(titles), title);
    gtk_box_append(GTK_BOX(titles), description);
    gtk_box_append(GTK_BOX(header), titles);

    page->search = gtk_search_entry_new();
    gtk_widget_set_size_request(page->search, 220, -1);
    gtk_search_entry_set_placeholder_text(
        GTK_SEARCH_ENTRY(page->search),
        "Search users or sessions");
    g_signal_connect_swapped(page->search, "search-changed", G_CALLBACK(rebuild_user_list), page);
    gtk_box_append(GTK_BOX(header), page->search);

    page->filter = gtk_drop_down_new_from_strings((const gchar *[]){"All", "Active", "Inactive", "Human", "System", NULL});
    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->filter), 0);
    g_signal_connect(page->filter, "notify::selected", G_CALLBACK(filter_changed), page);
    gtk_box_append(GTK_BOX(header), page->filter);

    page->sort = gtk_drop_down_new_from_strings((const gchar *[]){"Name", "Login", "UID", "Active", "Sessions", "Processes", "CPU", "Memory", NULL});
    g_signal_connect(page->sort, "notify::selected", G_CALLBACK(sort_changed), page);
    gtk_box_append(GTK_BOX(header), page->sort);

    page->include_system_toggle = gtk_check_button_new_with_label("System accounts");
    gtk_widget_set_tooltip_text(page->include_system_toggle,
                                "Include system/service accounts in the user model");
    g_signal_connect(page->include_system_toggle, "toggled",
                     G_CALLBACK(include_system_toggled), page);
    gtk_box_append(GTK_BOX(header), page->include_system_toggle);

    GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, "Refresh users and sessions");
    g_signal_connect_swapped(refresh, "clicked", G_CALLBACK(start_discovery), page);
    gtk_box_append(GTK_BOX(header), refresh);

    page->activity = gtk_spinner_new();
    page->status = gtk_label_new("Starting…");
    gtk_widget_add_css_class(page->status, "dim-label");
    gtk_box_append(GTK_BOX(header), page->activity);
    gtk_box_append(GTK_BOX(header), page->status);
    return header;
}

static GtkWidget *users_create_summary(UsersPage *page)
{
    GtkWidget *summary = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(summary), TRUE);
    gtk_widget_set_margin_start(summary, 10);
    gtk_widget_set_margin_end(summary, 10);

    GtkWidget *c1 = make_stat_card("Total users", &page->user_count);
    GtkWidget *c2 = make_stat_card("Active users", &page->active_count);
    GtkWidget *c3 = make_stat_card("Active sessions", &page->session_count);
    GtkWidget *c4 = make_stat_card("Graphical sessions", &page->graphical_count);
    GtkWidget *c5 = make_stat_card("Remote sessions", &page->remote_count);
    gtk_grid_attach(GTK_GRID(summary), c1, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(summary), c2, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(summary), c3, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(summary), c4, 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(summary), c5, 4, 0, 1, 1);
    return summary;
}

static GtkWidget *users_create_list(UsersPage *page)
{
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(frame, "card");
    gtk_widget_set_margin_start(frame, 10);
    gtk_widget_set_margin_end(frame, 5);
    gtk_widget_set_margin_top(frame, 6);
    gtk_widget_set_margin_bottom(frame, 10);

    GtkWidget *heading = gtk_label_new("Users");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
    gtk_widget_add_css_class(heading, "heading");
    gtk_widget_set_margin_start(heading, 14); gtk_widget_set_margin_top(heading, 12); gtk_widget_set_margin_bottom(heading, 6);
    gtk_box_append(GTK_BOX(frame), heading);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    page->user_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), page->user_list);
    gtk_box_append(GTK_BOX(frame), scroll);
    return frame;
}

static GtkWidget *users_create_details(UsersPage *page)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(outer, 5);
    gtk_widget_set_margin_end(outer, 10);
    gtk_widget_set_margin_top(outer, 6);
    gtk_widget_set_margin_bottom(outer, 10);

    GtkWidget *identity_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *avatar = gtk_label_new("?");
    page->selected_avatar = avatar;
    gtk_widget_set_size_request(avatar, 54, 54);
    gtk_widget_add_css_class(avatar, "avatar");
    gtk_box_append(GTK_BOX(identity_header), avatar);

    GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(heading, TRUE);
    page->selected_name = gtk_label_new("No user selected");
    gtk_label_set_xalign(GTK_LABEL(page->selected_name), 0.0);
    gtk_widget_add_css_class(page->selected_name, "title-3");
    page->selected_login = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(page->selected_login), 0.0);
    gtk_widget_add_css_class(page->selected_login, "dim-label");
    gtk_box_append(GTK_BOX(heading), page->selected_name);
    gtk_box_append(GTK_BOX(heading), page->selected_login);
    gtk_box_append(GTK_BOX(identity_header), heading);
    gtk_box_append(GTK_BOX(outer), identity_header);

    GtkWidget *id_content;
    GtkWidget *identity = make_group("Identity", &id_content);
    gtk_box_append(GTK_BOX(id_content), make_value_row("UID", &page->selected_uid));
    gtk_box_append(GTK_BOX(id_content), make_value_row("Primary group", &page->selected_gid));
    gtk_box_append(GTK_BOX(id_content), make_value_row("Home", &page->selected_home));
    gtk_box_append(GTK_BOX(id_content), make_value_row("Shell", &page->selected_shell));
    gtk_box_append(GTK_BOX(outer), identity);
    page->identity_group = identity;

    GtkWidget *account_content;
    GtkWidget *account = make_group("Account", &account_content);
    gtk_box_append(GTK_BOX(account_content), make_value_row("Classification", &page->selected_class));
    gtk_box_append(GTK_BOX(account_content), make_value_row("State", &page->selected_state));
    gtk_box_append(GTK_BOX(account_content), make_value_row("Source", &page->selected_source));
    gtk_box_append(GTK_BOX(account_content), make_value_row("Last login", &page->selected_last_login));
    gtk_box_append(GTK_BOX(outer), account);

    GtkWidget *activity_content;
    GtkWidget *activity = make_group("Activity", &activity_content);
    gtk_box_append(GTK_BOX(activity_content), make_value_row("Processes", &page->selected_processes));
    gtk_box_append(GTK_BOX(activity_content), make_value_row("CPU", &page->selected_cpu));
    gtk_box_append(GTK_BOX(activity_content), make_value_row("Memory", &page->selected_memory));
    gtk_box_append(GTK_BOX(activity_content), make_value_row("Sessions", &page->selected_session_count));
    gtk_box_append(GTK_BOX(outer), activity);
    page->activity_group = activity;

    GtkWidget *sessions_content;
    GtkWidget *sessions = make_group("Sessions", &sessions_content);
    page->selected_session_header = gtk_label_new("Sessions · 0");
    gtk_label_set_xalign(GTK_LABEL(page->selected_session_header), 0.0);
    gtk_widget_add_css_class(page->selected_session_header, "dim-label");
    gtk_box_append(GTK_BOX(sessions_content), page->selected_session_header);
    page->session_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_box_append(GTK_BOX(sessions_content), page->session_list);
    page->sessions_group = sessions;
    gtk_box_append(GTK_BOX(outer), sessions);

    GtkWidget *technical_content;
    GtkWidget *technical = make_group("Session details", &technical_content);
    page->session_details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(technical_content), page->session_details);
    gtk_box_append(GTK_BOX(outer), technical);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    page->terminate_button = gtk_button_new_with_label("Terminate selected session");
    gtk_widget_add_css_class(page->terminate_button, "destructive-action");
    gtk_widget_set_sensitive(page->terminate_button, FALSE);
    g_signal_connect(page->terminate_button, "clicked", G_CALLBACK(terminate_clicked), page);
    gtk_box_append(GTK_BOX(actions), page->terminate_button);
    gtk_box_append(GTK_BOX(outer), actions);

    return outer;
}

/* ------------------------------------------------------------------------- */
/* Filter/sort callbacks                                                     */
/* ------------------------------------------------------------------------- */

static void filter_changed(GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    UsersPage *page = data;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    page->filter_mode = (UserFilter)selected;
    if (page->filter_mode == USER_FILTER_SYSTEM && !page->include_system)
        start_discovery(page);
    rebuild_user_list(page);
}

static void sort_changed(GObject *object, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    UsersPage *page = data;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    page->sort_mode = (UserSort)selected;
    rebuild_user_list(page);
}

static void include_system_toggled(GtkCheckButton *button, gpointer data)
{
    UsersPage *page = data;
    page->include_system = gtk_check_button_get_active(button);
    start_discovery(page);
}

static gboolean periodic_refresh(gpointer data)
{
    UsersPage *page = data;
    if (!page || page->disposed)
        return G_SOURCE_REMOVE;
    start_discovery(page);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

static void users_dispose(gpointer data)
{
    UsersPage *page = data;
    if (!page) return;

    page->disposed = TRUE;
    if (page->sample_source) g_source_remove(page->sample_source);
    if (page->discovery_source) g_source_remove(page->discovery_source);
    if (page->manager_signal_id && page->bus)
        g_dbus_connection_signal_unsubscribe(page->bus, page->manager_signal_id);
    if (page->session_signal_id && page->bus)
        g_dbus_connection_signal_unsubscribe(page->bus, page->session_signal_id);
    page->manager_signal_id = 0;
    page->session_signal_id = 0;

    if (page->bus) {
        g_object_unref(page->bus);
        page->bus = NULL;
    }

    if (page->users) g_ptr_array_unref(page->users);
    if (page->sessions) g_ptr_array_unref(page->sessions);
    g_mutex_clear(&page->state_lock);
    g_free(page);
}

GtkWidget *users_page_create(App *app)
{
    UsersPage *page = g_new0(UsersPage, 1);
    page->app = app;
    page->include_system = FALSE;
    page->filter_mode = USER_FILTER_ALL;
    page->sort_mode = USER_SORT_NAME;
    page->users = g_ptr_array_new_with_free_func((GDestroyNotify)user_free);
    page->sessions = g_ptr_array_new_with_free_func((GDestroyNotify)session_free);
    g_mutex_init(&page->state_lock);

    page->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(page->root, "users-page");
    g_object_set_data_full(G_OBJECT(page->root), "users-page-state", page, users_dispose);

    gtk_box_append(GTK_BOX(page->root), users_create_header(page));
    gtk_box_append(GTK_BOX(page->root), users_create_summary(page));

    /* AdwNavigationSplitView supplies the requested wide/narrow behavior:
     * sidebar = users, content = selected user details. */
    AdwNavigationSplitView *split = ADW_NAVIGATION_SPLIT_VIEW(adw_navigation_split_view_new());
    adw_navigation_split_view_set_min_sidebar_width(split, 340.0);
    adw_navigation_split_view_set_max_sidebar_width(split, 520.0);
    adw_navigation_split_view_set_sidebar_width_fraction(split, 0.38);

    GtkWidget *sidebar = users_create_list(page);
    AdwNavigationPage *sidebar_page = adw_navigation_page_new(sidebar, "Users");
    adw_navigation_split_view_set_sidebar(split, sidebar_page);

    GtkWidget *detail_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(detail_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(detail_scroll, TRUE);
    GtkWidget *detail = users_create_details(page);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(detail_scroll), detail);
    page->details_scroller = detail_scroll;
    page->details = detail;
    AdwNavigationPage *detail_page = adw_navigation_page_new(detail_scroll, "User");
    adw_navigation_split_view_set_content(split, detail_page);

    gtk_widget_set_vexpand(GTK_WIDGET(split), TRUE);
    gtk_box_append(GTK_BOX(page->root), GTK_WIDGET(split));

    connect_logind(page);
    page->discovery_source = g_timeout_add_seconds(5, periodic_refresh, page);
    start_discovery(page);
    return page->root;
}
