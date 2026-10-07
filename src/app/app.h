#ifndef TASK_MANAGER_APP_H
#define TASK_MANAGER_APP_H

#include <adwaita.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <sys/types.h>

G_BEGIN_DECLS

/* Borrowed live process record. Arrays returned by app_get_processes() are
 * owned by the shared backend and remain valid until its next refresh. */
typedef struct {
    pid_t pid;
    pid_t ppid;
    const char *name;
    const char *exe;
    const char *const *argv;
    const char *cgroup;
    const char *user;
    const char *cmdline;
    const char *affinity;
    char state;
    int nice;
    int priority;
    int threads;
    int last_cpu;
    int fd_count;
    guint uid;
    gboolean io_ok;
    double cpu_percent;
    double read_rate;
    double write_rate;
    guint64 cpu_ticks;
    guint64 start_ticks;
    guint64 rss_bytes;
    guint64 vsize_bytes;
    guint64 shared_bytes;
    guint64 read_bytes;
    guint64 write_bytes;
    gint64 start_time_us;
} ProcessInfo;

G_DECLARE_FINAL_TYPE(App, app, TASK, APP, GObject)
typedef struct {
    double cpu_percent;
    guint64 rss_bytes;
    guint n_threads;
} ProcessSample;

typedef void (*AppProcessesUpdatedFunc)(App *app, gpointer user_data);

AdwApplication *app_get_application(App *app);
GtkWindow *app_get_window(App *app);
GtkStack *app_get_stack(App *app);

/* Borrowed GPtrArray of ProcessInfo* records; invalid after the next backend refresh. */
GPtrArray *app_get_processes(App *app);
gulong app_connect_processes_updated(App *app, AppProcessesUpdatedFunc callback, gpointer user_data);
void app_disconnect_processes_updated(App *app, gulong handler_id);
gboolean app_process_is_running(App *app, const char *program_name);
gboolean app_process_sample_pid(App *app, pid_t pid, ProcessSample *out);

void app_activate(GtkApplication *application, gpointer user_data);

G_END_DECLS

#endif
