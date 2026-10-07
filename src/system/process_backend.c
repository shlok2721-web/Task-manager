#define _GNU_SOURCE
#include "process_backend.h"

#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROC_PATH_MAX 512

typedef struct {
    guint64 start_ticks;
    guint64 cpu_ticks;
    guint64 read_bytes;
    guint64 write_bytes;
    gboolean io_ok;
} PrevProc;

typedef struct {
    ProcessInfo info;
    guint64 start_ticks;
    guint64 cpu_ticks;
    char *name;
    char *exe;
    char **argv;
    char *cmdline;
    char *cgroup;
    char *user;
    char *affinity;
} OwnedProcess;

struct _ProcessBackend {
    GPtrArray *processes;
    GPtrArray *process_views; /* borrowed ProcessInfo* views into processes */
    GHashTable *previous; /* pid -> PrevProc */
    guint64 total_cpu_ticks;
    guint64 boot_time;
    gint64 last_sample_us;
    long clk_tck;
    int cpu_count;
};

static void owned_process_free(gpointer data)
{
    OwnedProcess *p = data;
    if (!p) return;
    g_free(p->name);
    g_free(p->exe);
    g_strfreev(p->argv);
    g_free(p->cmdline);
    g_free(p->cgroup);
    g_free(p->user);
    g_free(p->affinity);
    g_free(p);
}

static void prev_proc_free(gpointer data)
{
    g_free(data);
}

static gboolean read_file_string(const char *path, char **out)
{
    gchar *contents = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &contents, &len, NULL))
        return FALSE;
    *out = contents;
    return TRUE;
}

static char *readlink_dup(const char *path)
{
    char buf[PROC_PATH_MAX];
    ssize_t n = readlink(path, buf, sizeof buf - 1);
    if (n < 0) return NULL;
    buf[n] = '\0';
    return g_strdup(buf);
}

static gboolean parse_proc_stat(const char *text,
                                pid_t *ppid,
                                char *state,
                                int *nice_value,
                                int *priority,
                                guint *threads,
                                int *last_cpu,
                                guint64 *proc_ticks,
                                guint64 *start_ticks,
                                guint64 *vsize,
                                gint64 *rss_pages)
{
    const char *close = strrchr(text, ')');
    if (!close || close[1] != ' ')
        return FALSE;

    char *rest = g_strdup(close + 2);
    char *save = NULL;
    char *tok = strtok_r(rest, " ", &save); /* field 3 */
    if (!tok || strlen(tok) != 1) {
        g_free(rest);
        return FALSE;
    }
    *state = tok[0];

    gint64 fields[64] = {0};
    int field = 4;
    while ((tok = strtok_r(NULL, " ", &save)) != NULL && field <= 64) {
        char *end = NULL;
        errno = 0;
        long long signed_v = strtoll(tok, &end, 10);
        if (errno != 0 || end == tok)
            fields[field++] = 0;
        else
            fields[field++] = (gint64)signed_v;
    }

    if (field <= 39) {
        g_free(rest);
        return FALSE;
    }

    *ppid = (pid_t)fields[4];
    *proc_ticks = fields[14] + fields[15];
    *priority = (int)(gint64)fields[18];
    *nice_value = (int)(gint64)fields[19];
    *threads = (guint)fields[20];
    *start_ticks = fields[22];
    *vsize = fields[23];
    *rss_pages = (gint64)fields[24];
    *last_cpu = (int)(gint64)fields[39];
    g_free(rest);
    return TRUE;
}

static gboolean read_proc_total_cpu(guint64 *out_total)
{
    gchar *contents = NULL;
    if (!read_file_string("/proc/stat", &contents))
        return FALSE;
    if (!g_str_has_prefix(contents, "cpu ")) {
        g_free(contents);
        return FALSE;
    }

    char *save = NULL;
    char *copy = g_strdup(contents + 4);
    char *tok = strtok_r(copy, " \t\r\n", &save);
    guint64 total = 0;
    while (tok) {
        total += g_ascii_strtoull(tok, NULL, 10);
        tok = strtok_r(NULL, " \t\r\n", &save);
    }
    g_free(copy);
    g_free(contents);
    *out_total = total;
    return TRUE;
}

static guint64 read_boot_time(void)
{
    gchar *contents = NULL;
    if (!read_file_string("/proc/stat", &contents))
        return 0;
    guint64 result = 0;
    char *line = contents;
    while (line && *line) {
        if (g_str_has_prefix(line, "btime ")) {
            result = g_ascii_strtoull(line + 6, NULL, 10);
            break;
        }
        char *next = strchr(line, '\n');
        if (!next) break;
        line = next + 1;
    }
    g_free(contents);
    return result;
}

static char **read_cmdline_argv(pid_t pid)
{
    char path[PROC_PATH_MAX];
    g_snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    gchar *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &data, &len, NULL) || len == 0) {
        g_free(data);
        return NULL;
    }

    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    gsize i = 0;
    while (i < len) {
        gsize start = i;
        while (i < len && data[i] != '\0') i++;
        if (i > start)
            g_ptr_array_add(items, g_strndup(data + start, i - start));
        while (i < len && data[i] == '\0') i++;
    }
    g_ptr_array_add(items, NULL);
    char **argv = (char **)g_ptr_array_free(items, FALSE);
    g_free(data);
    return argv;
}

static char *read_cgroup(pid_t pid)
{
    char path[PROC_PATH_MAX];
    g_snprintf(path, sizeof path, "/proc/%d/cgroup", pid);
    gchar *contents = NULL;
    if (!read_file_string(path, &contents)) return NULL;
    g_strchomp(contents);
    return contents;
}

static uid_t read_proc_uid(pid_t pid)
{
    char path[PROC_PATH_MAX];
    g_snprintf(path, sizeof path, "/proc/%d/status", pid);
    gchar *contents = NULL;
    if (!read_file_string(path, &contents)) return (uid_t)-1;
    uid_t uid = (uid_t)-1;
    char *line = contents;
    while (line && *line) {
        if (g_str_has_prefix(line, "Uid:")) {
            uid = (uid_t)g_ascii_strtoull(line + 4, NULL, 10);
            break;
        }
        char *next = strchr(line, '\n');
        if (!next) break;
        line = next + 1;
    }
    g_free(contents);
    return uid;
}

static char *uid_to_user(uid_t uid)
{
    long n = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (n < 1024) n = 16384;
    char *buf = g_malloc((gsize)n);
    struct passwd pwd;
    struct passwd *result = NULL;
    char *name = NULL;
    if (getpwuid_r(uid, &pwd, buf, (size_t)n, &result) == 0 && result)
        name = g_strdup(result->pw_name);
    g_free(buf);
    return name;
}

static guint64 read_status_u64(const char *contents, const char *key, guint64 fallback)
{
    const char *line = contents;
    gsize key_len = strlen(key);
    while (line && *line) {
        if (g_str_has_prefix(line, key) && line[key_len] == ':')
            return g_ascii_strtoull(line + key_len + 1, NULL, 10);
        char *next = strchr(line, '\n');
        if (!next) break;
        line = next + 1;
    }
    return fallback;
}

static char *read_status_field(const char *contents, const char *key)
{
    const char *line = contents;
    gsize key_len = strlen(key);
    while (line && *line) {
        if (g_str_has_prefix(line, key) && line[key_len] == ':') {
            line += key_len + 1;
            while (*line == ' ' || *line == '\t') line++;
            return g_strndup(line, strcspn(line, "\n"));
        }
        char *next = strchr(line, '\n');
        if (!next) break;
        line = next + 1;
    }
    return NULL;
}

static gboolean read_proc_io(pid_t pid, guint64 *read_bytes, guint64 *write_bytes)
{
    char path[PROC_PATH_MAX];
    g_snprintf(path, sizeof path, "/proc/%d/io", pid);
    gchar *contents = NULL;
    if (!read_file_string(path, &contents))
        return FALSE;
    guint64 r = read_status_u64(contents, "read_bytes", G_MAXUINT64);
    guint64 w = read_status_u64(contents, "write_bytes", G_MAXUINT64);
    g_free(contents);
    if (r == G_MAXUINT64 || w == G_MAXUINT64)
        return FALSE;
    *read_bytes = r;
    *write_bytes = w;
    return TRUE;
}

static int count_open_fds(pid_t pid)
{
    char path[PROC_PATH_MAX];
    g_snprintf(path, sizeof path, "/proc/%d/fd", pid);
    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir)
        return -1;
    int count = 0;
    while (g_dir_read_name(dir)) count++;
    g_dir_close(dir);
    return count;
}

static gboolean read_process(pid_t pid, ProcessBackend *b, OwnedProcess **out)
{
    char stat_path[PROC_PATH_MAX];
    g_snprintf(stat_path, sizeof stat_path, "/proc/%d/stat", pid);
    gchar *stat = NULL;
    if (!read_file_string(stat_path, &stat)) return FALSE;

    pid_t ppid = 0;
    char state = '?';
    guint threads = 0;
    int nice_value = 0, priority = 0, last_cpu = -1;
    guint64 proc_ticks = 0, start_ticks = 0, vsize = 0;
    gint64 rss_pages = 0;
    gboolean ok = parse_proc_stat(stat, &ppid, &state, &nice_value, &priority,
                                  &threads, &last_cpu, &proc_ticks,
                                  &start_ticks, &vsize, &rss_pages);
    char *open = strchr(stat, '(');
    char *close = strrchr(stat, ')');
    if (!ok || !open || !close || close <= open) {
        g_free(stat);
        return FALSE;
    }

    OwnedProcess *p = g_new0(OwnedProcess, 1);
    p->name = g_strndup(open + 1, close - open - 1);
    g_free(stat);

    char exe_path[PROC_PATH_MAX];
    g_snprintf(exe_path, sizeof exe_path, "/proc/%d/exe", pid);
    p->exe = readlink_dup(exe_path);
    p->argv = read_cmdline_argv(pid);
    p->cgroup = read_cgroup(pid);
    if (p->argv) {
        GString *cmd = g_string_new(NULL);
        for (guint i = 0; p->argv[i]; ++i) {
            if (i) g_string_append_c(cmd, ' ');
            g_string_append(cmd, p->argv[i]);
        }
        p->cmdline = g_string_free(cmd, FALSE);
    }

    uid_t uid = read_proc_uid(pid);
    p->user = uid != (uid_t)-1 ? uid_to_user(uid) : NULL;
    if (!p->user) p->user = g_strdup("?");

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;

    char status_path[PROC_PATH_MAX];
    g_snprintf(status_path, sizeof status_path, "/proc/%d/status", pid);
    gchar *status = NULL;
    guint64 shared_pages = 0;
    if (read_file_string(status_path, &status)) {
        p->affinity = read_status_field(status, "Cpus_allowed_list");
        shared_pages = read_status_u64(status, "RssFile", 0) +
                       read_status_u64(status, "RssShmem", 0);
        g_free(status);
    }

    char statm_path[PROC_PATH_MAX];
    g_snprintf(statm_path, sizeof statm_path, "/proc/%d/statm", pid);
    gchar *statm = NULL;
    if (read_file_string(statm_path, &statm)) {
        char **v = g_strsplit(g_strstrip(statm), " ", -1);
        if (g_strv_length(v) >= 3)
            shared_pages = g_ascii_strtoull(v[2], NULL, 10);
        g_strfreev(v);
        g_free(statm);
    }

    guint64 read_bytes = 0, write_bytes = 0;
    gboolean io_ok = read_proc_io(pid, &read_bytes, &write_bytes);
    int fd_count = count_open_fds(pid);

    p->start_ticks = start_ticks;
    p->cpu_ticks = proc_ticks;
    p->info.pid = pid;
    p->info.ppid = ppid;
    p->info.name = p->name;
    p->info.exe = p->exe;
    p->info.argv = (const char *const *)p->argv;
    p->info.cgroup = p->cgroup;
    p->info.user = p->user;
    p->info.cmdline = p->cmdline;
    p->info.affinity = p->affinity;
    p->info.state = state;
    p->info.nice = nice_value;
    p->info.priority = priority;
    p->info.threads = (int)threads;
    p->info.last_cpu = last_cpu;
    p->info.fd_count = fd_count;
    p->info.uid = uid;
    p->info.io_ok = io_ok;
    p->info.cpu_ticks = proc_ticks;
    p->info.start_ticks = start_ticks;
    p->info.rss_bytes = rss_pages > 0 ? (guint64)rss_pages * (guint64)page_size : 0;
    p->info.vsize_bytes = vsize;
    p->info.shared_bytes = shared_pages * (guint64)page_size;
    p->info.read_bytes = read_bytes;
    p->info.write_bytes = write_bytes;
    p->info.read_rate = -1.0;
    p->info.write_rate = -1.0;
    p->info.start_time_us = b->boot_time && b->clk_tck > 0
        ? (gint64)b->boot_time * G_USEC_PER_SEC
          + (gint64)((start_ticks * G_USEC_PER_SEC) / (guint64)b->clk_tck)
        : 0;
    p->info.cpu_percent = 0.0;
    *out = p;
    return TRUE;
}

ProcessBackend *process_backend_new(void)
{
    ProcessBackend *b = g_new0(ProcessBackend, 1);
    b->processes = g_ptr_array_new_with_free_func(owned_process_free);
    b->process_views = g_ptr_array_new();
    b->previous = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, prev_proc_free);
    b->clk_tck = sysconf(_SC_CLK_TCK);
    if (b->clk_tck <= 0) b->clk_tck = 100;
    b->cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (b->cpu_count <= 0) b->cpu_count = 1;
    b->boot_time = read_boot_time();
    return b;
}

void process_backend_free(ProcessBackend *b)
{
    if (!b) return;
    g_clear_pointer(&b->processes, g_ptr_array_unref);
    g_clear_pointer(&b->process_views, g_ptr_array_unref);
    g_clear_pointer(&b->previous, g_hash_table_unref);
    g_free(b);
}

gboolean process_backend_refresh(ProcessBackend *b)
{
    if (!b) return FALSE;
    guint64 total_cpu = 0;
    if (!read_proc_total_cpu(&total_cpu)) return FALSE;

    guint64 previous_total = b->total_cpu_ticks;
    guint64 delta_total = total_cpu > previous_total ? total_cpu - previous_total : 0;

    GHashTable *new_prev = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, prev_proc_free);
    GPtrArray *new_items = g_ptr_array_new_with_free_func(owned_process_free);
    GPtrArray *new_views = g_ptr_array_new();
    gint64 now_us = g_get_monotonic_time();
    double dt = (b->last_sample_us > 0 && now_us > b->last_sample_us)
        ? (double)(now_us - b->last_sample_us) / 1e6 : 0.0;
    GDir *dir = g_dir_open("/proc", 0, NULL);
    if (!dir) {
        g_hash_table_unref(new_prev);
        g_ptr_array_unref(new_items);
        return FALSE;
    }

    const char *entry;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (!g_ascii_isdigit(entry[0])) continue;
        char *end = NULL;
        errno = 0;
        long value = strtol(entry, &end, 10);
        if (errno || end == entry || *end || value <= 0) continue;

        OwnedProcess *p = NULL;
        if (!read_process((pid_t)value, b, &p)) continue;

        PrevProc *old = g_hash_table_lookup(b->previous, GINT_TO_POINTER((gint)p->info.pid));
        if (old && old->start_ticks == p->start_ticks && delta_total > 0) {
            guint64 delta_proc = p->info.cpu_ticks >= old->cpu_ticks ? p->info.cpu_ticks - old->cpu_ticks : 0;
            p->info.cpu_percent = ((double)delta_proc / (double)delta_total) * (double)b->cpu_count * 100.0;
            if (p->info.cpu_percent < 0.0) p->info.cpu_percent = 0.0;
            if (p->info.io_ok && old->io_ok && dt > 0.0 &&
                p->info.read_bytes >= old->read_bytes && p->info.write_bytes >= old->write_bytes) {
                p->info.read_rate = (double)(p->info.read_bytes - old->read_bytes) / dt;
                p->info.write_rate = (double)(p->info.write_bytes - old->write_bytes) / dt;
            }
        }

        PrevProc *now = g_new0(PrevProc, 1);
        now->start_ticks = p->info.start_ticks;
        now->cpu_ticks = p->info.cpu_ticks;
        now->read_bytes = p->info.read_bytes;
        now->write_bytes = p->info.write_bytes;
        now->io_ok = p->info.io_ok;
        g_hash_table_insert(new_prev, GINT_TO_POINTER((gint)p->info.pid), now);
        g_ptr_array_add(new_views, &p->info);
        g_ptr_array_add(new_items, p);
    }
    g_dir_close(dir);

    g_ptr_array_unref(b->processes);
    g_ptr_array_unref(b->process_views);
    b->processes = new_items;
    b->process_views = new_views;
    g_hash_table_unref(b->previous);
    b->previous = new_prev;
    b->total_cpu_ticks = total_cpu;
    b->last_sample_us = now_us;
    return TRUE;
}

GPtrArray *process_backend_get_processes(ProcessBackend *backend)
{
    return backend ? backend->process_views : NULL;
}

gboolean process_backend_is_running(ProcessBackend *backend, const char *program_name)
{
    if (!backend || !program_name || !*program_name) return FALSE;
    char *needle = g_path_get_basename(program_name);
    for (guint i = 0; i < backend->processes->len; ++i) {
        OwnedProcess *p = g_ptr_array_index(backend->processes, i);
        gboolean match = g_strcmp0(p->name, needle) == 0;
        if (!match && p->exe) {
            char *base = g_path_get_basename(p->exe);
            match = g_strcmp0(base, needle) == 0;
            g_free(base);
        }
        if (!match && p->argv && p->argv[0]) {
            char *base = g_path_get_basename(p->argv[0]);
            match = g_strcmp0(base, needle) == 0;
            g_free(base);
        }
        if (match) {
            g_free(needle);
            return TRUE;
        }
    }
    g_free(needle);
    return FALSE;
}

gboolean process_backend_sample_pid_for_backend(ProcessBackend *backend, pid_t pid, ProcessSample *out)
{
    if (!backend || !out || pid <= 0) return FALSE;
    for (guint i = 0; i < backend->processes->len; ++i) {
        OwnedProcess *p = g_ptr_array_index(backend->processes, i);
        if (p->info.pid == pid) {
            out->cpu_percent = p->info.cpu_percent;
            out->rss_bytes = p->info.rss_bytes;
            out->n_threads = p->info.threads > 0 ? (guint)p->info.threads : 0;
            return TRUE;
        }
    }
    out->cpu_percent = 0.0;
    out->rss_bytes = 0;
    out->n_threads = 0;
    return FALSE;
}
