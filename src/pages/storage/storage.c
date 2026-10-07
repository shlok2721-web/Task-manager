#include "storage.h"
#include "storage_scan.h"

#include <adwaita.h>
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

/* ========================================================================= */
/* Category tables                                                           */
/* ========================================================================= */

typedef struct {
    const char *name;
    const char *color;
    const char *extensions;   /* space separated, lowercase, no dot */
} CategoryInfo;

/* Indexed by StorageCategory. Order here is also the "stable" display order. */
static const CategoryInfo CATEGORY_INFO[STORAGE_CAT_COUNT] = {
    [STORAGE_CAT_SYSTEM]       = { "System",              "#8e8e93", "" },
    [STORAGE_CAT_APPLICATIONS] = { "Applications",        "#5e5ce6", "" },
    [STORAGE_CAT_VIDEOS]       = { "Videos",              "#ff453a",
        "mp4 mkv avi mov flv wmv webm m4v mpeg mpg ts" },
    [STORAGE_CAT_IMAGES]       = { "Images",              "#ff9f0a",
        "jpg jpeg png gif bmp tiff tif webp heic svg ico" },
    [STORAGE_CAT_MUSIC]        = { "Music",               "#ff375f",
        "mp3 wav aac ogg flac opus m4a wma" },
    [STORAGE_CAT_DOCUMENTS]    = { "Documents",           "#0a84ff",
        "pdf doc docx xls xlsx ppt pptx txt md odt ods epub rtf csv" },
    [STORAGE_CAT_ARCHIVES]     = { "Archives",            "#bf5af2",
        "zip rar 7z tar gz xz bz2 zst tgz txz" },
    [STORAGE_CAT_SOURCE_CODE]  = { "Source Code",         "#30d158",
        "py c cpp cc h hpp java kt js jsx tsx go rs lua sh bash zsh json yaml yml "
        "xml html htm css sql rb php pl swift m ipynb" },
    [STORAGE_CAT_DATABASES]    = { "Databases",           "#64d2ff",
        "db sqlite sqlite3 mdb accdb" },
    [STORAGE_CAT_FONTS]        = { "Fonts",               "#ac8e68",
        "ttf otf woff woff2 eot" },
    [STORAGE_CAT_DISK_IMAGES]  = { "Disk Images / ISOs",  "#ffd60a",
        "iso img dmg toast nrg" },
    [STORAGE_CAT_EXECUTABLES]  = { "Executables",         "#ff6961",
        "exe dll so bin appimage run deb rpm" },
    [STORAGE_CAT_CONFIG]       = { "Configuration Files", "#a2845e",
        "conf cfg ini toml rc" },
    [STORAGE_CAT_LOGS]         = { "Logs",                "#636366", "log" },
    [STORAGE_CAT_TEMPORARY]    = { "Temporary Files",     "#98989d",
        "tmp temp bak old swp swo cache part crdownload" },
    [STORAGE_CAT_VMS]          = { "Virtual Machines",    "#32d74b",
        "vdi vmdk vhd vhdx qcow2 vbox vmx ova ovf" },
    [STORAGE_CAT_OTHER]        = { "Other",               "#48484a", "" },
};

/* Path rules: checked in this order, per *directory* (not per file). */
static const char *const APPLICATION_PREFIXES[] = {
    "/usr/bin", "/usr/sbin", "/usr/lib", "/usr/lib64", "/usr/share", "/usr/local", NULL
};
static const char *const SYSTEM_PREFIXES[] = {
    "/usr", "/lib", "/lib64", "/etc", "/var", "/boot", "/opt", NULL
};
/* Pseudo / huge / virtual trees that are never worth scanning. */
static const char *const EXCLUDED_DIRS[] = {
    "/proc", "/sys", "/dev", "/run", "/snap",
    "/var/lib/docker", "/var/lib/containers", NULL
};

#define MAX_EXT_LEN 15

const char *storage_category_name(StorageCategory c)
{
    return (c >= 0 && c < STORAGE_CAT_COUNT) ? CATEGORY_INFO[c].name : "Other";
}

const char *storage_category_color(StorageCategory c)
{
    return (c >= 0 && c < STORAGE_CAT_COUNT) ? CATEGORY_INFO[c].color : "#888888";
}

static GHashTable *ext_map;   /* char* -> GINT_TO_POINTER(category) */

static gpointer build_extension_map(gpointer unused)
{
    (void)unused;
    GHashTable *m = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (int c = 0; c < STORAGE_CAT_COUNT; ++c) {
        gchar **parts = g_strsplit(CATEGORY_INFO[c].extensions, " ", -1);
        for (gchar **p = parts; *p; ++p)
            if (**p) g_hash_table_replace(m, g_strdup(*p), GINT_TO_POINTER(c));
        g_strfreev(parts);
    }
    ext_map = m;
    return m;
}

static StorageCategory category_for_extension(const char *ext)
{
    static GOnce once = G_ONCE_INIT;
    g_once(&once, build_extension_map, NULL);   /* read-only afterwards: thread safe */
    gpointer v;
    if (ext[0] && g_hash_table_lookup_extended(ext_map, ext, NULL, &v))
        return (StorageCategory)GPOINTER_TO_INT(v);
    return STORAGE_CAT_OTHER;
}

static gboolean path_under(const char *path, const char *prefix)
{
    gsize n = strlen(prefix);
    return strncmp(path, prefix, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* Returns a category for the directory, or -1 if the extension decides. */
static int directory_category(const char *dir)
{
    for (const char *const *p = APPLICATION_PREFIXES; *p; ++p)
        if (path_under(dir, *p)) return STORAGE_CAT_APPLICATIONS;
    for (const char *const *p = SYSTEM_PREFIXES; *p; ++p)
        if (path_under(dir, *p)) return STORAGE_CAT_SYSTEM;
    return -1;
}

/*
 * Lowercase extension without the dot into out[MAX_EXT_LEN+1].
 * Dotfiles (".bashrc"), trailing dots, absurdly long "extensions" (hash-like
 * names such as "foo.3f9a1c...") and anything that is not plain ASCII
 * alphanumerics produce STORAGE_NO_EXTENSION, which keeps the extension table
 * small and the JSON/CSV output clean.
 */
static gboolean extract_extension(const char *name, char *out)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || dot[1] == '\0') goto none;
    gsize len = strlen(dot + 1);
    if (len > MAX_EXT_LEN) goto none;
    for (gsize i = 0; i < len; ++i) {
        guchar ch = (guchar)dot[1 + i];
        if (!g_ascii_isalnum(ch) && ch != '_' && ch != '-' && ch != '+') goto none;
        out[i] = (char)g_ascii_tolower(ch);
    }
    out[len] = '\0';
    return TRUE;
none:
    out[0] = '\0';
    return FALSE;
}

/* ========================================================================= */
/* Result containers                                                         */
/* ========================================================================= */

static void result_init(StorageScanResult *r, const char *root)
{
    memset(r, 0, sizeof *r);
    r->root = g_strdup(root);
    for (int c = 0; c < STORAGE_CAT_COUNT; ++c) {
        r->cats[c].id = (StorageCategory)c;
        r->cats[c].extensions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    }
}

static StorageScanResult *result_new(const char *root)
{
    StorageScanResult *r = g_new0(StorageScanResult, 1);
    result_init(r, root);
    return r;
}

void storage_scan_result_free(StorageScanResult *r)
{
    if (!r) return;
    for (int c = 0; c < STORAGE_CAT_COUNT; ++c)
        g_clear_pointer(&r->cats[c].extensions, g_hash_table_unref);
    g_free(r->root);
    g_free(r);
}

static StorageExtStats *ext_lookup(StorageCategoryStats *cat, const char *ext)
{
    StorageExtStats *s = g_hash_table_lookup(cat->extensions, ext);
    if (!s) {
        s = g_new0(StorageExtStats, 1);
        g_hash_table_insert(cat->extensions, g_strdup(ext), s);
    }
    return s;
}

static void category_add(StorageCategoryStats *cat, const char *ext, guint64 size)
{
    StorageExtStats *s = ext_lookup(cat, ext);
    s->count++;
    s->total_size += size;
    if (size > s->largest_size) s->largest_size = size;
    cat->total_count++;
    cat->total_size += size;
}

static void result_merge(StorageScanResult *dst, const StorageScanResult *src)
{
    for (int c = 0; c < STORAGE_CAT_COUNT; ++c) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, src->cats[c].extensions);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            const StorageExtStats *from = v;
            StorageExtStats *to = ext_lookup(&dst->cats[c], k);
            to->count += from->count;
            to->total_size += from->total_size;
            if (from->largest_size > to->largest_size) to->largest_size = from->largest_size;
        }
        dst->cats[c].total_count += src->cats[c].total_count;
        dst->cats[c].total_size += src->cats[c].total_size;
    }
    dst->errors += src->errors;
}

/* ========================================================================= */
/* Scanner                                                                   */
/* ========================================================================= */

typedef struct { guint64 dev; guint64 ino; } LinkKey;

static guint link_hash(gconstpointer p)
{
    const LinkKey *k = p;
    return (guint)(k->ino ^ (k->ino >> 32) ^ (k->dev * 2654435761u));
}

static gboolean link_equal(gconstpointer a, gconstpointer b)
{
    const LinkKey *x = a, *y = b;
    return x->dev == y->dev && x->ino == y->ino;
}

struct _StorageScan {
    char *root;
    int nworkers;
    dev_t root_dev;
    gboolean have_root_dev;
    GPtrArray *excluded;          /* active exclusions (those not containing root) */

    /* work queue: all guarded by `lock` */
    GMutex lock;
    GCond cond;
    GQueue queue;                 /* char* directories */
    guint pending;                /* queued + currently being processed */

    gint cancel;                  /* atomic */

    GMutex progress_lock;
    StorageScanProgress prog;
    gint64 start_us;

    /* hard links: count every inode only once */
    GMutex links_lock;
    GHashTable *seen_links;
};

StorageScan *storage_scan_new(const char *root, int workers)
{
    StorageScan *s = g_new0(StorageScan, 1);
    s->root = g_canonicalize_filename(root && *root ? root : "/", NULL);
    if (workers <= 0) workers = (int)MIN(32u, g_get_num_processors() * 4u);
    s->nworkers = MAX(1, workers);

    struct stat st;
    if (lstat(s->root, &st) == 0) { s->root_dev = st.st_dev; s->have_root_dev = TRUE; }

    /* If the user scans *inside* an excluded tree (e.g. /run/media/me/USB),
     * that exclusion must not apply, otherwise the scan would be empty. */
    s->excluded = g_ptr_array_new();
    for (const char *const *p = EXCLUDED_DIRS; *p; ++p)
        if (!path_under(s->root, *p)) g_ptr_array_add(s->excluded, (gpointer)*p);

    g_mutex_init(&s->lock);
    g_cond_init(&s->cond);
    g_queue_init(&s->queue);
    g_mutex_init(&s->progress_lock);
    g_mutex_init(&s->links_lock);
    s->seen_links = g_hash_table_new_full(link_hash, link_equal, g_free, NULL);
    return s;
}

void storage_scan_free(StorageScan *s)
{
    if (!s) return;
    g_queue_clear_full(&s->queue, g_free);
    g_ptr_array_unref(s->excluded);
    g_hash_table_unref(s->seen_links);
    g_mutex_clear(&s->lock);
    g_cond_clear(&s->cond);
    g_mutex_clear(&s->progress_lock);
    g_mutex_clear(&s->links_lock);
    g_free(s->root);
    g_free(s);
}

void storage_scan_cancel(StorageScan *s)
{
    if (!s) return;
    g_atomic_int_set(&s->cancel, 1);
    g_mutex_lock(&s->lock);
    g_cond_broadcast(&s->cond);          /* wake idle workers so they notice */
    g_mutex_unlock(&s->lock);
}

void storage_scan_get_progress(StorageScan *s, StorageScanProgress *out)
{
    g_mutex_lock(&s->progress_lock);
    *out = s->prog;
    g_mutex_unlock(&s->progress_lock);
    out->elapsed = s->start_us ? (g_get_monotonic_time() - s->start_us) / 1e6 : 0.0;
}

static void push_dir(StorageScan *s, char *dir /* ownership taken */)
{
    g_mutex_lock(&s->lock);
    g_queue_push_tail(&s->queue, dir);
    s->pending++;
    g_cond_signal(&s->cond);
    g_mutex_unlock(&s->lock);
}

static gboolean should_skip_dir(StorageScan *s, const char *path, dev_t dev)
{
    for (guint i = 0; i < s->excluded->len; ++i)
        if (path_under(path, g_ptr_array_index(s->excluded, i))) return TRUE;
    /* Never cross into another filesystem: avoids bind-mount double counting,
     * hanging network shares and loop-mounted images. */
    return s->have_root_dev && dev != s->root_dev;
}

/* TRUE the first time an inode with nlink > 1 is seen. */
static gboolean first_sight_of_link(StorageScan *s, const struct stat *st)
{
    LinkKey probe = { (guint64)st->st_dev, (guint64)st->st_ino };
    gboolean first = FALSE;
    g_mutex_lock(&s->links_lock);
    if (!g_hash_table_contains(s->seen_links, &probe)) {
        g_hash_table_add(s->seen_links, g_memdup2(&probe, sizeof probe));
        first = TRUE;
    }
    g_mutex_unlock(&s->links_lock);
    return first;
}

static void process_directory(StorageScan *s, StorageScanResult *local, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) { local->errors++; return; }

    int dfd = dirfd(d);
    int dir_cat = directory_category(dir);
    guint64 files = 0, bytes = 0;
    struct dirent *e;

    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
            continue;
        if (g_atomic_int_get(&s->cancel)) break;

        struct stat st;
        if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            local->errors++;                       /* vanished, EACCES, EIO, ... */
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            char *child = g_build_filename(dir, name, NULL);
            if (should_skip_dir(s, child, st.st_dev)) g_free(child);
            else push_dir(s, child);
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;         /* symlinks, sockets, fifos, devices */
        if (st.st_nlink > 1 && !first_sight_of_link(s, &st)) continue;

        char ext[MAX_EXT_LEN + 1];
        gboolean has_ext = extract_extension(name, ext);
        StorageCategory cat = dir_cat >= 0 ? (StorageCategory)dir_cat
                                           : category_for_extension(ext);
        /* Allocated size (what `du` reports) rather than apparent size, so
         * sparse files such as VM images aren't wildly over-counted. */
        guint64 size = (guint64)st.st_blocks * 512u;
        category_add(&local->cats[cat], has_ext ? ext : STORAGE_NO_EXTENSION, size);
        files++;
        bytes += size;
    }
    closedir(d);

    g_mutex_lock(&s->progress_lock);
    s->prog.files += files;
    s->prog.bytes += bytes;
    s->prog.errors = s->prog.errors;   /* updated from workers' local counters below */
    g_strlcpy(s->prog.current_dir, dir, sizeof s->prog.current_dir);
    g_mutex_unlock(&s->progress_lock);
}

static gpointer worker_main(gpointer data)
{
    StorageScan *s = data;
    StorageScanResult *local = result_new(s->root);
    guint64 reported_errors = 0;

    for (;;) {
        g_mutex_lock(&s->lock);
        while (!g_atomic_int_get(&s->cancel) && g_queue_is_empty(&s->queue) && s->pending > 0)
            g_cond_wait(&s->cond, &s->lock);
        if (g_atomic_int_get(&s->cancel) || g_queue_is_empty(&s->queue)) {
            g_mutex_unlock(&s->lock);
            break;
        }
        char *dir = g_queue_pop_head(&s->queue);
        g_mutex_unlock(&s->lock);

        process_directory(s, local, dir);
        g_free(dir);

        if (local->errors != reported_errors) {
            g_mutex_lock(&s->progress_lock);
            s->prog.errors += local->errors - reported_errors;
            g_mutex_unlock(&s->progress_lock);
            reported_errors = local->errors;
        }

        g_mutex_lock(&s->lock);
        if (--s->pending == 0)
            g_cond_broadcast(&s->cond);      /* everything done: release idle workers */
        g_mutex_unlock(&s->lock);
    }
    return local;
}

StorageScanResult *storage_scan_run(StorageScan *s)
{
    s->start_us = g_get_monotonic_time();
    push_dir(s, g_strdup(s->root));

    GThread **threads = g_new0(GThread *, s->nworkers);
    for (int i = 0; i < s->nworkers; ++i) {
        char tname[24];
        g_snprintf(tname, sizeof tname, "disk-scan-%d", i);
        threads[i] = g_thread_new(tname, worker_main, s);
    }

    StorageScanResult *result = result_new(s->root);
    for (int i = 0; i < s->nworkers; ++i) {
        StorageScanResult *local = g_thread_join(threads[i]);
        result_merge(result, local);
        storage_scan_result_free(local);
    }
    g_free(threads);

    for (int c = 0; c < STORAGE_CAT_COUNT; ++c) {
        result->total_files += result->cats[c].total_count;
        result->total_bytes += result->cats[c].total_size;
    }
    result->cancelled = g_atomic_int_get(&s->cancel) != 0;
    result->elapsed_us = g_get_monotonic_time() - s->start_us;
    return result;
}

/* ========================================================================= */
/* Presentation helpers                                                      */
/* ========================================================================= */

static gint compare_cat_size(gconstpointer a, gconstpointer b, gpointer user_data)
{
    const StorageScanResult *r = user_data;
    guint64 x = r->cats[*(const StorageCategory *)a].total_size;
    guint64 y = r->cats[*(const StorageCategory *)b].total_size;
    return x < y ? 1 : (x > y ? -1 : 0);
}

void storage_result_sorted_categories(const StorageScanResult *r, StorageCategory *order)
{
    for (int i = 0; i < STORAGE_CAT_COUNT; ++i) order[i] = (StorageCategory)i;
    g_qsort_with_data(order, STORAGE_CAT_COUNT, sizeof order[0], compare_cat_size, (gpointer)r);
}

static gint compare_ext_entry(gconstpointer a, gconstpointer b)
{
    const StorageExtEntry *x = *(StorageExtEntry *const *)a;
    const StorageExtEntry *y = *(StorageExtEntry *const *)b;
    if (x->stats->total_size != y->stats->total_size)
        return x->stats->total_size < y->stats->total_size ? 1 : -1;
    return g_strcmp0(x->extension, y->extension);
}

GPtrArray *storage_category_sorted_extensions(const StorageCategoryStats *cat)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, cat->extensions);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        StorageExtEntry *e = g_new(StorageExtEntry, 1);
        e->extension = k;
        e->stats = v;
        g_ptr_array_add(arr, e);
    }
    g_ptr_array_sort(arr, compare_ext_entry);
    return arr;
}

const char *storage_format_bytes(char *buf, gsize n, guint64 bytes)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    double v = (double)bytes;
    guint i = 0;
    while (v >= 1024.0 && i < G_N_ELEMENTS(units) - 1) { v /= 1024.0; i++; }
    if (i == 0)       g_snprintf(buf, n, "%.0f %s", v, units[i]);
    else if (v < 10)  g_snprintf(buf, n, "%.2f %s", v, units[i]);
    else if (v < 100) g_snprintf(buf, n, "%.1f %s", v, units[i]);
    else              g_snprintf(buf, n, "%.0f %s", v, units[i]);
    return buf;
}

void storage_format_duration(char *buf, gsize n, double seconds)
{
    if (!(seconds >= 0.0) || !isfinite(seconds)) { g_strlcpy(buf, "--", n); return; }
    guint64 t = (guint64)seconds;
    guint64 h = t / 3600, m = (t % 3600) / 60, sec = t % 60;
    if (h)      g_snprintf(buf, n, "%" G_GUINT64_FORMAT "h %" G_GUINT64_FORMAT "m %" G_GUINT64_FORMAT "s", h, m, sec);
    else if (m) g_snprintf(buf, n, "%" G_GUINT64_FORMAT "m %" G_GUINT64_FORMAT "s", m, sec);
    else        g_snprintf(buf, n, "%" G_GUINT64_FORMAT "s", sec);
}

double storage_percentage(guint64 part, guint64 whole)
{
    if (whole == 0) return 0.0;
    return CLAMP((double)part * 100.0 / (double)whole, 0.0, 100.0);
}

/* ========================================================================= */
/* Export                                                                    */
/* ========================================================================= */

static void json_string(GString *out, const char *s)
{
    char *valid = g_utf8_make_valid(s ? s : "", -1);
    g_string_append_c(out, '"');
    for (const guchar *p = (const guchar *)valid; *p; ++p) {
        switch (*p) {
        case '"':  g_string_append(out, "\\\""); break;
        case '\\': g_string_append(out, "\\\\"); break;
        case '\n': g_string_append(out, "\\n");  break;
        case '\r': g_string_append(out, "\\r");  break;
        case '\t': g_string_append(out, "\\t");  break;
        default:
            if (*p < 0x20) g_string_append_printf(out, "\\u%04x", *p);
            else g_string_append_c(out, (char)*p);
        }
    }
    g_string_append_c(out, '"');
    g_free(valid);
}

static void append_double(GString *out, double v, int decimals)
{
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    char fmt[8];
    g_snprintf(fmt, sizeof fmt, "%%.%df", decimals);
    g_ascii_formatd(buf, sizeof buf, fmt, v);   /* locale independent */
    g_string_append(out, buf);
}

static char *timestamp_now(void)
{
    GDateTime *now = g_date_time_new_now_local();
    char *s = g_date_time_format_iso8601(now);
    g_date_time_unref(now);
    return s;
}

gboolean storage_result_export_json(const StorageScanResult *r, const char *path, GError **error)
{
    GString *o = g_string_new(NULL);
    char hb[64];
    char *ts = timestamp_now();
    StorageCategory order[STORAGE_CAT_COUNT];
    storage_result_sorted_categories(r, order);

    g_string_append(o, "{\n  \"generated_at\": "); json_string(o, ts);
    g_string_append(o, ",\n  \"scanned_path\": "); json_string(o, r->root);
    g_string_append_printf(o, ",\n  \"total_files\": %" G_GUINT64_FORMAT, r->total_files);
    g_string_append_printf(o, ",\n  \"total_bytes\": %" G_GUINT64_FORMAT, r->total_bytes);
    g_string_append(o, ",\n  \"total_size_human\": "); json_string(o, storage_format_bytes(hb, sizeof hb, r->total_bytes));
    g_string_append_printf(o, ",\n  \"scan_errors_skipped\": %" G_GUINT64_FORMAT, r->errors);
    g_string_append(o, ",\n  \"scan_duration_seconds\": "); append_double(o, r->elapsed_us / 1e6, 2);
    g_string_append(o, ",\n  \"scan_cancelled\": "); g_string_append(o, r->cancelled ? "true" : "false");
    g_string_append(o, ",\n  \"categories\": [");

    gboolean first_cat = TRUE;
    for (int i = 0; i < STORAGE_CAT_COUNT; ++i) {
        const StorageCategoryStats *c = &r->cats[order[i]];
        if (c->total_count == 0) continue;
        g_string_append(o, first_cat ? "\n    {" : ",\n    {");
        first_cat = FALSE;
        g_string_append(o, "\"name\": "); json_string(o, storage_category_name(c->id));
        g_string_append_printf(o, ", \"total_size_bytes\": %" G_GUINT64_FORMAT, c->total_size);
        g_string_append(o, ", \"total_size_human\": "); json_string(o, storage_format_bytes(hb, sizeof hb, c->total_size));
        g_string_append_printf(o, ", \"file_count\": %" G_GUINT64_FORMAT, c->total_count);
        g_string_append(o, ", \"percent_of_scanned\": ");
        append_double(o, storage_percentage(c->total_size, r->total_bytes), 2);
        g_string_append(o, ",\n      \"extensions\": [");

        GPtrArray *exts = storage_category_sorted_extensions(c);
        for (guint j = 0; j < exts->len; ++j) {
            const StorageExtEntry *e = g_ptr_array_index(exts, j);
            g_string_append(o, j ? ",\n        {" : "\n        {");
            g_string_append(o, "\"extension\": "); json_string(o, e->extension);
            g_string_append_printf(o, ", \"file_count\": %" G_GUINT64_FORMAT, e->stats->count);
            g_string_append_printf(o, ", \"total_size_bytes\": %" G_GUINT64_FORMAT, e->stats->total_size);
            g_string_append_printf(o, ", \"average_size_bytes\": %" G_GUINT64_FORMAT,
                                   e->stats->count ? e->stats->total_size / e->stats->count : 0);
            g_string_append_printf(o, ", \"largest_file_bytes\": %" G_GUINT64_FORMAT "}", e->stats->largest_size);
        }
        g_ptr_array_unref(exts);
        g_string_append(o, exts ? "\n      ]}" : "]}");
    }
    g_string_append(o, "\n  ]\n}\n");

    gboolean ok = g_file_set_contents(path, o->str, (gssize)o->len, error);
    g_string_free(o, TRUE);
    g_free(ts);
    return ok;
}

static void csv_field(GString *out, const char *s)
{
    if (strpbrk(s, ",\"\n\r")) {
        g_string_append_c(out, '"');
        for (; *s; ++s) { if (*s == '"') g_string_append_c(out, '"'); g_string_append_c(out, *s); }
        g_string_append_c(out, '"');
    } else {
        g_string_append(out, s);
    }
}

gboolean storage_result_export_csv(const StorageScanResult *r, const char *path, GError **error)
{
    GString *o = g_string_new("category,extension,file_count,total_size_bytes,total_size_human,"
                              "average_size_bytes,largest_file_bytes,percent_of_category\n");
    StorageCategory order[STORAGE_CAT_COUNT];
    storage_result_sorted_categories(r, order);

    for (int i = 0; i < STORAGE_CAT_COUNT; ++i) {
        const StorageCategoryStats *c = &r->cats[order[i]];
        GPtrArray *exts = storage_category_sorted_extensions(c);
        for (guint j = 0; j < exts->len; ++j) {
            const StorageExtEntry *e = g_ptr_array_index(exts, j);
            char hb[64];
            csv_field(o, storage_category_name(c->id)); g_string_append_c(o, ',');
            csv_field(o, e->extension);                 g_string_append_c(o, ',');
            g_string_append_printf(o, "%" G_GUINT64_FORMAT ",%" G_GUINT64_FORMAT ",",
                                   e->stats->count, e->stats->total_size);
            csv_field(o, storage_format_bytes(hb, sizeof hb, e->stats->total_size));
            g_string_append_printf(o, ",%" G_GUINT64_FORMAT ",%" G_GUINT64_FORMAT ",",
                                   e->stats->count ? e->stats->total_size / e->stats->count : 0,
                                   e->stats->largest_size);
            append_double(o, storage_percentage(e->stats->total_size, c->total_size), 2);
            g_string_append_c(o, '\n');
        }
        g_ptr_array_unref(exts);
    }
    gboolean ok = g_file_set_contents(path, o->str, (gssize)o->len, error);
    g_string_free(o, TRUE);
    return ok;
}

/* ========================================================================= */
/* Physical storage page / device monitoring                                 */
/* ========================================================================= */

#define STORAGE_REFRESH_MS 1000
#define DEVICE_REFRESH_MS 5000
#define HISTORY_LEN 60
#define SCAN_PROGRESS_MS 250

typedef struct {
    char *name;
    char *model;
    char *device;
    gboolean removable;
    gboolean rotational;
    guint64 capacity_bytes;
    guint64 read_bytes;
    guint64 write_bytes;
    double read_rate;
    double write_rate;
    double temperature;
    gboolean has_temperature;
    double usage;
    guint64 used_bytes;
    guint64 free_bytes;
    char *mount_point;
    GArray *read_history;
    GArray *write_history;
} StorageDevice;

typedef struct {
    char *device;
    guint64 size_bytes;
    guint64 start_sector;
    char *filesystem;
    char *mount_point;
} StoragePartition;

typedef struct {
    GtkWidget *root;
    GtkWidget *device_list;
    GtkWidget *detail;
    GtkWidget *summary_total;
    GtkWidget *summary_used;
    GtkWidget *summary_free;
    GtkWidget *summary_devices;
    GtkWidget *device_name;
    GtkWidget *device_meta;
    GtkWidget *capacity_bar;
    GtkWidget *capacity_label;
    GtkWidget *read_label;
    GtkWidget *write_label;
    GtkWidget *temp_label;
    GtkWidget *health_label;
    GtkWidget *filesystem_box;
    GtkWidget *partition_box;
    GtkDrawingArea *graph;

    GtkStack *section_stack;
    GtkWidget *partitions_page;
    GtkWidget *usage_list;
    GtkWidget *usage_summary;
    GtkWidget *scan_path_entry;
    GtkWidget *scan_button;
    GtkWidget *scan_cancel_button;
    GtkWidget *scan_progress;
    GtkWidget *scan_status;

    GPtrArray *devices;
    StorageDevice *selected;
    guint refresh_id;
    guint device_refresh_id;
    guint scan_progress_id;
    gint64 last_sample_us;
    App *app;

    StorageScan *scan;
    gboolean scan_running;
} StoragePage;

typedef struct {
    GtkWidget *root;
    StorageScan *scan;
} ScanJob;

static const char *format_bytes(char *buf, gsize n, guint64 bytes)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    double v = (double)bytes;
    guint i = 0;
    while (v >= 1024.0 && i < G_N_ELEMENTS(units) - 1) {
        v /= 1024.0;
        i++;
    }
    if (i == 0) g_snprintf(buf, n, "%.0f %s", v, units[i]);
    else if (v < 10) g_snprintf(buf, n, "%.2f %s", v, units[i]);
    else if (v < 100) g_snprintf(buf, n, "%.1f %s", v, units[i]);
    else g_snprintf(buf, n, "%.0f %s", v, units[i]);
    return buf;
}

static void storage_device_free(gpointer data)
{
    StorageDevice *d = data;
    if (!d) return;
    g_free(d->name);
    g_free(d->model);
    g_free(d->device);
    g_free(d->mount_point);
    g_clear_pointer(&d->read_history, g_array_unref);
    g_clear_pointer(&d->write_history, g_array_unref);
    g_free(d);
}

static void storage_partition_free(gpointer data)
{
    StoragePartition *p = data;
    if (!p) return;
    g_free(p->device);
    g_free(p->filesystem);
    g_free(p->mount_point);
    g_free(p);
}

static char *read_text_file(const char *path)
{
    gchar *contents = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &contents, &len, NULL)) return NULL;
    g_strstrip(contents);
    return contents;
}

static guint64 read_u64_file(const char *path)
{
    char *text = read_text_file(path);
    if (!text) return 0;
    guint64 v = g_ascii_strtoull(text, NULL, 10);
    g_free(text);
    return v;
}

static gboolean read_disk_stat(const char *name, guint64 *read_bytes, guint64 *write_bytes)
{
    char path[512];
    g_snprintf(path, sizeof path, "/sys/block/%s/stat", name);
    char *text = read_text_file(path);
    if (!text) return FALSE;

    guint64 fields[16] = { 0 };
    char *save = NULL;
    char *tok = strtok_r(text, " \t", &save);
    guint i = 0;
    while (tok && i < G_N_ELEMENTS(fields)) {
        fields[i++] = g_ascii_strtoull(tok, NULL, 10);
        tok = strtok_r(NULL, " \t", &save);
    }
    g_free(text);
    if (i < 7) return FALSE;
    *read_bytes = fields[2] * 512ULL;
    *write_bytes = fields[6] * 512ULL;
    return TRUE;
}

static char *unescape_mount_field(const char *s)
{
    GString *out = g_string_new(NULL);
    for (const char *c = s; *c; ++c) {
        if (c[0] == '\\' && g_ascii_isdigit(c[1]) &&
            g_ascii_isdigit(c[2]) && g_ascii_isdigit(c[3])) {
            g_string_append_c(out, (char)(((c[1] - '0') << 6) |
                                          ((c[2] - '0') << 3) |
                                          (c[3] - '0')));
            c += 3;
        } else {
            g_string_append_c(out, *c);
        }
    }
    return g_string_free(out, FALSE);
}

static char *sysfs_to_disk(const char *sysdir, int depth)
{
    if (depth > 8) return NULL;
    char *real = realpath(sysdir, NULL);
    if (!real) return NULL;

    char *result = NULL;
    char *part = g_build_filename(real, "partition", NULL);
    if (g_file_test(part, G_FILE_TEST_EXISTS)) {
        char *parent = g_path_get_dirname(real);
        result = g_path_get_basename(parent);
        g_free(parent);
    } else {
        char *slaves = g_build_filename(real, "slaves", NULL);
        GDir *dir = g_dir_open(slaves, 0, NULL);
        const char *n = dir ? g_dir_read_name(dir) : NULL;
        if (n) {
            char *next = g_build_filename("/sys/class/block", n, NULL);
            result = sysfs_to_disk(next, depth + 1);
            g_free(next);
        } else {
            result = g_path_get_basename(real);
        }
        if (dir) g_dir_close(dir);
        g_free(slaves);
    }
    g_free(part);
    free(real);
    return result;
}

static char *disk_for_dev_t(dev_t dev)
{
    char path[64];
    g_snprintf(path, sizeof path, "/sys/dev/block/%u:%u", major(dev), minor(dev));
    return sysfs_to_disk(path, 0);
}

static char *disk_for_path(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) != 0) return NULL;
    return disk_for_dev_t(st.st_dev);
}

static gboolean path_is_under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    if (!n || strncmp(path, dir, n) != 0) return FALSE;
    return n == 1 || path[n] == '\0' || path[n] == '/';
}

static char *find_mount_for_disk(const char *disk_name)
{
    gchar *mounts = NULL;
    if (!g_file_get_contents("/proc/self/mounts", &mounts, NULL, NULL)) return NULL;

    const char *home = g_get_home_dir();
    char *home_disk = disk_for_path(home);
    gboolean home_here = home_disk && g_strcmp0(home_disk, disk_name) == 0;
    g_free(home_disk);

    char *best = NULL;
    int best_score = -1;
    char *save = NULL;
    for (char *line = strtok_r(mounts, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char src[512], dst_raw[512];
        if (sscanf(line, "%511s %511s", src, dst_raw) != 2) continue;
        if (!g_str_has_prefix(src, "/dev/")) continue;

        struct stat st;
        if (stat(src, &st) != 0 || !S_ISBLK(st.st_mode)) continue;
        char *disk = disk_for_dev_t(st.st_rdev);
        gboolean match = disk && g_strcmp0(disk, disk_name) == 0;
        g_free(disk);
        if (!match) continue;

        char *dst = unescape_mount_field(dst_raw);
        int score = 1;
        if (g_strcmp0(dst, "/") == 0) score = 2;
        if (home_here && home && strlen(dst) > 1 && path_is_under(home, dst)) score = 3;
        if (score > best_score) {
            g_free(best);
            best = dst;
            best_score = score;
        } else {
            g_free(dst);
        }
    }
    g_free(mounts);
    return best;
}

static void update_usage(StorageDevice *d)
{
    d->used_bytes = 0;
    d->free_bytes = 0;
    d->usage = NAN;
    if (!d->mount_point) d->mount_point = find_mount_for_disk(d->name);
    if (!d->mount_point) return;

    struct statvfs st;
    if (statvfs(d->mount_point, &st) != 0 || st.f_blocks == 0) {
        g_clear_pointer(&d->mount_point, g_free);
        return;
    }
    guint64 total = (guint64)st.f_blocks * st.f_frsize;
    guint64 avail = (guint64)st.f_bavail * st.f_frsize;
    guint64 used = total > avail ? total - avail : 0;
    d->used_bytes = used;
    d->free_bytes = avail;
    d->usage = total ? (double)used * 100.0 / (double)total : NAN;
}

static StorageDevice *device_from_sys(const char *name)
{
    StorageDevice *d = g_new0(StorageDevice, 1);
    d->name = g_strdup(name);
    d->device = g_strdup_printf("/dev/%s", name);
    char path[512];

    g_snprintf(path, sizeof path, "/sys/block/%s/device/model", name);
    d->model = read_text_file(path);
    if (!d->model) d->model = g_strdup(name);
    g_snprintf(path, sizeof path, "/sys/block/%s/removable", name);
    d->removable = read_u64_file(path) != 0;
    g_snprintf(path, sizeof path, "/sys/block/%s/queue/rotational", name);
    d->rotational = read_u64_file(path) != 0;
    g_snprintf(path, sizeof path, "/sys/block/%s/size", name);
    d->capacity_bytes = read_u64_file(path) * 512ULL;
    d->temperature = NAN;
    d->read_history = g_array_new(FALSE, FALSE, sizeof(double));
    d->write_history = g_array_new(FALSE, FALSE, sizeof(double));
    update_usage(d);
    return d;
}

static gboolean should_show_block_device(const char *name)
{
    return name && *name && !g_str_has_prefix(name, "loop") &&
           !g_str_has_prefix(name, "ram") && !g_str_has_prefix(name, "dm-") &&
           !g_str_has_prefix(name, "zram");
}

static void preserve_device_state(StorageDevice *old, StorageDevice *fresh)
{
    if (g_strcmp0(old->device, fresh->device) != 0) return;
    g_clear_pointer(&fresh->read_history, g_array_unref);
    g_clear_pointer(&fresh->write_history, g_array_unref);
    fresh->read_history = old->read_history;
    fresh->write_history = old->write_history;
    old->read_history = NULL;
    old->write_history = NULL;
    fresh->read_bytes = old->read_bytes;
    fresh->write_bytes = old->write_bytes;
    fresh->read_rate = old->read_rate;
    fresh->write_rate = old->write_rate;
}

static void discover_devices(StoragePage *p)
{
    GPtrArray *old_devices = p->devices;
    GPtrArray *new_devices = g_ptr_array_new_with_free_func(storage_device_free);
    char *selected_path = p->selected ? g_strdup(p->selected->device) : NULL;

    GDir *dir = g_dir_open("/sys/block", 0, NULL);
    if (!dir) {
        g_ptr_array_unref(new_devices);
        g_free(selected_path);
        return;
    }

    const char *name;
    while ((name = g_dir_read_name(dir))) {
        if (!should_show_block_device(name)) continue;
        StorageDevice *fresh = device_from_sys(name);
        if (fresh->capacity_bytes == 0) {
            storage_device_free(fresh);
            continue;
        }
        for (guint i = 0; i < old_devices->len; ++i) {
            StorageDevice *old = g_ptr_array_index(old_devices, i);
            if (g_strcmp0(old->device, fresh->device) == 0) {
                preserve_device_state(old, fresh);
                break;
            }
        }
        g_ptr_array_add(new_devices, fresh);
    }
    g_dir_close(dir);

    p->devices = new_devices;
    p->selected = NULL;
    if (selected_path) {
        for (guint i = 0; i < p->devices->len; ++i) {
            StorageDevice *d = g_ptr_array_index(p->devices, i);
            if (g_strcmp0(d->device, selected_path) == 0) {
                p->selected = d;
                break;
            }
        }
    }
    g_free(selected_path);
    g_ptr_array_unref(old_devices);
}

static void draw_history(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                         gpointer user_data)
{
    (void)area;
    StoragePage *p = user_data;
    StorageDevice *d = p ? p->selected : NULL;
    if (!d) return;

    cairo_set_source_rgba(cr, 0.03, 0.04, 0.05, 1.0);
    cairo_paint(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    cairo_set_line_width(cr, 1);
    for (int i = 1; i < 4; ++i) {
        double y = height * (double)i / 4.0;
        cairo_move_to(cr, 0, y);
        cairo_line_to(cr, width, y);
        cairo_stroke(cr);
    }

    guint n = MAX(d->read_history->len, d->write_history->len);
    if (n < 2) return;
    double maxv = 1.0;
    for (guint i = 0; i < d->read_history->len; ++i)
        maxv = MAX(maxv, g_array_index(d->read_history, double, i));
    for (guint i = 0; i < d->write_history->len; ++i)
        maxv = MAX(maxv, g_array_index(d->write_history, double, i));

    cairo_set_line_width(cr, 2);
    const double alpha[2] = { 0.9, 0.75 };
    GArray *series[2] = { d->read_history, d->write_history };
    for (int s = 0; s < 2; ++s) {
        cairo_set_source_rgba(cr,
                              s == 0 ? 0.35 : 0.95,
                              s == 0 ? 0.78 : 0.46,
                              s == 0 ? 1.0 : 0.35, alpha[s]);
        for (guint i = 0; i < series[s]->len; ++i) {
            double v = g_array_index(series[s], double, i);
            double x = (double)i / (double)(n - 1) * width;
            double y = height - (v / maxv) * (height - 8) - 4;
            if (i == 0) cairo_move_to(cr, x, y);
            else cairo_line_to(cr, x, y);
        }
        cairo_stroke(cr);
    }
}

static GtkWidget *value_row(const char *label, const char *value)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *a = gtk_label_new(label);
    GtkWidget *b = gtk_label_new(value);
    gtk_label_set_xalign(GTK_LABEL(a), 0);
    gtk_label_set_xalign(GTK_LABEL(b), 1);
    gtk_widget_set_hexpand(a, TRUE);
    gtk_widget_add_css_class(a, "dim-label");
    gtk_label_set_selectable(GTK_LABEL(b), TRUE);
    gtk_box_append(GTK_BOX(row), a);
    gtk_box_append(GTK_BOX(row), b);
    return row;
}

static void clear_box(GtkWidget *box)
{
    GtkWidget *child = gtk_widget_get_first_child(box);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(box), child);
        child = next;
    }
}

static GtkWidget *summary_card(const char *title, GtkWidget **value_out)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(box, "tm-storage-card");
    GtkWidget *t = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_widget_add_css_class(t, "dim-label");
    GtkWidget *v = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(v), 0);
    gtk_widget_add_css_class(v, "tm-storage-value");
    gtk_box_append(GTK_BOX(box), t);
    gtk_box_append(GTK_BOX(box), v);
    *value_out = v;
    return box;
}

static void update_summary(StoragePage *p)
{
    guint64 total = 0, used = 0, free_bytes = 0;
    for (guint i = 0; i < p->devices->len; ++i) {
        StorageDevice *d = g_ptr_array_index(p->devices, i);
        total += d->capacity_bytes;
        used += d->used_bytes;
        free_bytes += d->free_bytes;
    }
    char a[64], b[64], c[64], d[32];
    format_bytes(a, sizeof a, total);
    format_bytes(b, sizeof b, used);
    format_bytes(c, sizeof c, free_bytes);
    g_snprintf(d, sizeof d, "%u", p->devices->len);
    gtk_label_set_text(GTK_LABEL(p->summary_total), a);
    gtk_label_set_text(GTK_LABEL(p->summary_used), b);
    gtk_label_set_text(GTK_LABEL(p->summary_free), c);
    gtk_label_set_text(GTK_LABEL(p->summary_devices), d);
}

static void partition_mount_info(const char *partition, char **mount_out,
                                 char **filesystem_out)
{
    *mount_out = NULL;
    *filesystem_out = NULL;
    gchar *mounts = NULL;
    if (!g_file_get_contents("/proc/self/mounts", &mounts, NULL, NULL)) return;

    char expected[512];
    g_snprintf(expected, sizeof expected, "/dev/%s", partition);
    char *save = NULL;
    for (char *line = strtok_r(mounts, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char src[512], dst_raw[512], fstype[128];
        if (sscanf(line, "%511s %511s %127s", src, dst_raw, fstype) != 3) continue;
        if (g_strcmp0(src, expected) != 0) continue;
        *mount_out = unescape_mount_field(dst_raw);
        *filesystem_out = g_strdup(fstype);
        break;
    }
    g_free(mounts);
}

static GPtrArray *partitions_for_disk(const char *disk_name)
{
    GPtrArray *parts = g_ptr_array_new_with_free_func(storage_partition_free);
    char *base = g_build_filename("/sys/block", disk_name, NULL);
    GDir *dir = g_dir_open(base, 0, NULL);
    if (!dir) {
        g_free(base);
        return parts;
    }

    const char *name;
    while ((name = g_dir_read_name(dir))) {
        char *marker = g_build_filename(base, name, "partition", NULL);
        gboolean is_partition = g_file_test(marker, G_FILE_TEST_EXISTS);
        g_free(marker);
        if (!is_partition) continue;

        char *size_path = g_build_filename(base, name, "size", NULL);
        guint64 sectors = read_u64_file(size_path);
        g_free(size_path);
        if (!sectors) continue;

        char *start_path = g_build_filename(base, name, "start", NULL);
        guint64 start = read_u64_file(start_path);
        g_free(start_path);

        StoragePartition *part = g_new0(StoragePartition, 1);
        part->device = g_strdup_printf("/dev/%s", name);
        part->size_bytes = sectors * 512ULL;
        part->start_sector = start;

        /*
         * partition_mount_info() may replace the filesystem pointer.
         * Therefore do not allocate "Unknown" before calling it.
         */
        part->filesystem = NULL;

        partition_mount_info(
            name,
            &part->mount_point,
            &part->filesystem
        );

        if (!part->filesystem)
            part->filesystem = g_strdup("Unknown");

        g_ptr_array_add(parts, part);
    }

    g_dir_close(dir);
    g_free(base);
    return parts;
}

static void update_partition_view(StoragePage *p)
{
    clear_box(p->partition_box);
    if (!p->selected) {
        GtkWidget *empty = gtk_label_new("Select a physical drive to view its partitions.");
        gtk_label_set_xalign(GTK_LABEL(empty), 0);
        gtk_widget_add_css_class(empty, "dim-label");
        gtk_box_append(GTK_BOX(p->partition_box), empty);
        return;
    }

    GtkWidget *heading = gtk_label_new("Partitions and filesystems");
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_widget_add_css_class(heading, "heading");
    gtk_box_append(GTK_BOX(p->partition_box), heading);

    GPtrArray *parts = partitions_for_disk(p->selected->name);
    if (parts->len == 0) {
        GtkWidget *empty = gtk_label_new("No partition entries were found for this drive.");
        gtk_label_set_xalign(GTK_LABEL(empty), 0);
        gtk_widget_add_css_class(empty, "dim-label");
        gtk_box_append(GTK_BOX(p->partition_box), empty);
        g_ptr_array_unref(parts);
        return;
    }

    for (guint i = 0; i < parts->len; ++i) {
        StoragePartition *part = g_ptr_array_index(parts, i);
        char size[64], start[64];
        format_bytes(size, sizeof size, part->size_bytes);
        g_snprintf(start, sizeof start, "LBA %" G_GUINT64_FORMAT, part->start_sector);

        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_widget_add_css_class(card, "tm-storage-card");
        GtkWidget *name = gtk_label_new(part->device);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_widget_add_css_class(name, "heading");
        gtk_label_set_selectable(GTK_LABEL(name), TRUE);
        gtk_box_append(GTK_BOX(card), name);
        gtk_box_append(GTK_BOX(card), value_row("Capacity", size));
        gtk_box_append(GTK_BOX(card), value_row("Filesystem", part->filesystem));
        gtk_box_append(GTK_BOX(card), value_row("Mount point",
                         part->mount_point ? part->mount_point : "Not mounted"));
        gtk_box_append(GTK_BOX(card), value_row("Start", start));
        gtk_box_append(GTK_BOX(p->partition_box), card);
    }
    g_ptr_array_unref(parts);
}

static void update_details(StoragePage *p)
{
    StorageDevice *d = p->selected;
    if (!d) {
        gtk_label_set_text(GTK_LABEL(p->device_name), "No storage device selected");
        gtk_label_set_text(GTK_LABEL(p->device_meta), "Connect or select a physical drive.");
        gtk_widget_set_visible(p->detail, FALSE);
        update_partition_view(p);
        return;
    }

    gtk_widget_set_visible(p->detail, TRUE);
    gtk_label_set_text(GTK_LABEL(p->device_name), d->model);
    char meta[256];
    g_snprintf(meta, sizeof meta, "%s · %s · %s", d->device,
                d->rotational ? "HDD" : "SSD/NVMe",
                d->removable ? "Removable" : "Internal");
    gtk_label_set_text(GTK_LABEL(p->device_meta), meta);

    char cap[64];
    format_bytes(cap, sizeof cap, d->capacity_bytes);
    if (isfinite(d->usage)) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->capacity_bar),
                                      CLAMP(d->usage / 100.0, 0.0, 1.0));
        char used[64], freeb[64], label[192];
        format_bytes(used, sizeof used, d->used_bytes);
        format_bytes(freeb, sizeof freeb, d->free_bytes);
        g_snprintf(label, sizeof label, "%s used · %s free · %s total",
                    used, freeb, cap);
        gtk_label_set_text(GTK_LABEL(p->capacity_label), label);
    } else {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->capacity_bar), 0.0);
        char label[128];
        g_snprintf(label, sizeof label, "%s total · filesystem not mounted", cap);
        gtk_label_set_text(GTK_LABEL(p->capacity_label), label);
    }

    char read[64], write[64], rb[80], wb[80];
    format_bytes(read, sizeof read, (guint64)MAX(d->read_rate, 0.0));
    format_bytes(write, sizeof write, (guint64)MAX(d->write_rate, 0.0));
    g_snprintf(rb, sizeof rb, "%s/s", read);
    g_snprintf(wb, sizeof wb, "%s/s", write);
    gtk_label_set_text(GTK_LABEL(p->read_label), rb);
    gtk_label_set_text(GTK_LABEL(p->write_label), wb);
    gtk_label_set_text(GTK_LABEL(p->temp_label), d->has_temperature ? "Available" : "Unavailable");
    gtk_label_set_text(GTK_LABEL(p->health_label), "Not queried");

    clear_box(p->filesystem_box);
    GtkWidget *heading = gtk_label_new("Filesystem");
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_widget_add_css_class(heading, "heading");
    gtk_box_append(GTK_BOX(p->filesystem_box), heading);
    gtk_box_append(GTK_BOX(p->filesystem_box), value_row("Mount point",
                     d->mount_point ? d->mount_point : "Not mounted"));
    char *home_disk = disk_for_path(g_get_home_dir());
    if (g_strcmp0(home_disk, d->name) == 0)
        gtk_box_append(GTK_BOX(p->filesystem_box), value_row("Home folder", g_get_home_dir()));
    g_free(home_disk);
}

static void update_device_list(StoragePage *p);

static void select_device_clicked(GtkButton *button, gpointer user_data)
{
    StoragePage *p = user_data;
    StorageDevice *d = g_object_get_data(G_OBJECT(button), "storage-device");
    if (!p || !d) return;
    p->selected = d;
    update_device_list(p);
    update_details(p);
    update_partition_view(p);
    gtk_widget_queue_draw(GTK_WIDGET(p->graph));
}

static void update_device_list(StoragePage *p)
{
    clear_box(p->device_list);
    for (guint i = 0; i < p->devices->len; ++i) {
        StorageDevice *d = g_ptr_array_index(p->devices, i);
        GtkWidget *button = gtk_button_new();
        gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
        gtk_widget_add_css_class(button, "tm-storage-row");

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *icon = gtk_image_new_from_icon_name(
            d->removable ? "drive-removable-media-symbolic" : "drive-harddisk-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 28);
        GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(labels, TRUE);
        GtkWidget *name = gtk_label_new(d->model);
        GtkWidget *meta = gtk_label_new(d->name);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_xalign(GTK_LABEL(meta), 0);
        gtk_widget_add_css_class(meta, "dim-label");
        gtk_box_append(GTK_BOX(labels), name);
        gtk_box_append(GTK_BOX(labels), meta);
        char cap[64];
        format_bytes(cap, sizeof cap, d->capacity_bytes);
        GtkWidget *size = gtk_label_new(cap);
        gtk_label_set_xalign(GTK_LABEL(size), 1);
        gtk_box_append(GTK_BOX(row), icon);
        gtk_box_append(GTK_BOX(row), labels);
        gtk_box_append(GTK_BOX(row), size);
        gtk_button_set_child(GTK_BUTTON(button), row);
        g_object_set_data(G_OBJECT(button), "storage-device", d);
        g_signal_connect(button, "clicked", G_CALLBACK(select_device_clicked), p);
        gtk_box_append(GTK_BOX(p->device_list), button);
        if (d == p->selected) gtk_widget_add_css_class(button, "selected");
    }
}

static void select_first(StoragePage *p)
{
    if (!p->selected && p->devices->len)
        p->selected = g_ptr_array_index(p->devices, 0);
}

static void usage_clear(StoragePage *p)
{
    clear_box(p->usage_list);
}

static void usage_show_result(StoragePage *p, StorageScanResult *r)
{
    usage_clear(p);

    char total[64], elapsed[64], summary[256];
    storage_format_bytes(total, sizeof total, r->total_bytes);
    storage_format_duration(elapsed, sizeof elapsed, r->elapsed_us / 1e6);
    g_snprintf(summary, sizeof summary,
                "%s in %s · %" G_GUINT64_FORMAT " files · %" G_GUINT64_FORMAT " skipped errors",
                total, elapsed, r->total_files, r->errors);
    gtk_label_set_text(GTK_LABEL(p->usage_summary), summary);

    StorageCategory order[STORAGE_CAT_COUNT];
    storage_result_sorted_categories(r, order);

    for (int i = 0; i < STORAGE_CAT_COUNT; ++i) {
        StorageCategoryStats *cat = &r->cats[order[i]];
        if (!cat->total_count) continue;

        char size[64], detail[128];
        storage_format_bytes(size, sizeof size, cat->total_size);
        g_snprintf(detail, sizeof detail, "%s · %" G_GUINT64_FORMAT " files",
                    size, cat->total_count);

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        gtk_widget_add_css_class(row, "tm-storage-card");
        GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name = gtk_label_new(storage_category_name(cat->id));
        GtkWidget *value = gtk_label_new(detail);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_xalign(GTK_LABEL(value), 1);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_box_append(GTK_BOX(top), name);
        gtk_box_append(GTK_BOX(top), value);
        gtk_box_append(GTK_BOX(row), top);

        GtkWidget *bar = gtk_progress_bar_new();
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar),
                                      storage_percentage(cat->total_size, r->total_bytes) / 100.0);
        gtk_box_append(GTK_BOX(row), bar);

        GPtrArray *exts = storage_category_sorted_extensions(cat);
        if (exts->len) {
            GtkWidget *extbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_set_margin_start(extbox, 12);
            for (guint j = 0; j < exts->len; ++j) {
                StorageExtEntry *e = g_ptr_array_index(exts, j);
                char eb[64], eline[160];
                storage_format_bytes(eb, sizeof eb, e->stats->total_size);
                g_snprintf(eline, sizeof eline, ".%s — %s — %" G_GUINT64_FORMAT " files",
                            e->extension, eb, e->stats->count);
                GtkWidget *label = gtk_label_new(eline);
                gtk_label_set_xalign(GTK_LABEL(label), 0);
                gtk_widget_add_css_class(label, "dim-label");
                gtk_box_append(GTK_BOX(extbox), label);
                if (j >= 7) break;
            }
            gtk_box_append(GTK_BOX(row), extbox);
        }
        g_ptr_array_unref(exts);
        gtk_box_append(GTK_BOX(p->usage_list), row);
    }

    if (r->cancelled) gtk_label_set_text(GTK_LABEL(p->scan_status), "Scan cancelled");
    else gtk_label_set_text(GTK_LABEL(p->scan_status), "Scan complete");
}

static void scan_job_free(gpointer data)
{
    ScanJob *job = data;
    if (!job) return;
    g_clear_pointer(&job->root, g_object_unref);
    g_free(job);
}

static void scan_worker(GTask *task, gpointer source_object, gpointer task_data,
                        GCancellable *cancellable)
{
    (void)source_object;
    (void)cancellable;
    ScanJob *job = task_data;
    StorageScanResult *result = storage_scan_run(job->scan);
    storage_scan_free(job->scan);
    job->scan = NULL;
    g_task_return_pointer(task, result, (GDestroyNotify)storage_scan_result_free);
}

static gboolean scan_progress_tick(gpointer data)
{
    StoragePage *p = data;
    if (!p->scan_running || !p->scan) return G_SOURCE_REMOVE;

    StorageScanProgress progress;
    storage_scan_get_progress(p->scan, &progress);
    char done[64], elapsed[64], status[320];
    storage_format_bytes(done, sizeof done, progress.bytes);
    storage_format_duration(elapsed, sizeof elapsed, progress.elapsed);
    g_snprintf(status, sizeof status, "Scanning · %s · %" G_GUINT64_FORMAT
                " files · %s", done, progress.files, elapsed);
    gtk_label_set_text(GTK_LABEL(p->scan_status), status);
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(p->scan_progress));
    return G_SOURCE_CONTINUE;
}

static void scan_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)source;
    StoragePage *p = user_data;
    GError *error = NULL;
    StorageScanResult *scan_result = g_task_propagate_pointer(G_TASK(result), &error);

    if (p->scan_progress_id) {
        g_source_remove(p->scan_progress_id);
        p->scan_progress_id = 0;
    }

    p->scan = NULL;
    p->scan_running = FALSE;
    gtk_widget_set_sensitive(p->scan_button, TRUE);
    gtk_widget_set_sensitive(p->scan_cancel_button, FALSE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->scan_progress), 0.0);

    if (error) {
        gtk_label_set_text(GTK_LABEL(p->scan_status), error->message);
        g_clear_error(&error);
        return;
    }

    usage_show_result(p, scan_result);
    storage_scan_result_free(scan_result);
}

static void start_scan_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    StoragePage *p = user_data;
    if (p->scan_running) return;

    const char *text = gtk_editable_get_text(GTK_EDITABLE(p->scan_path_entry));
    char *path = g_canonicalize_filename(text && *text ? text : "/", NULL);
    if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
        gtk_label_set_text(GTK_LABEL(p->scan_status), "Scan location is not a directory");
        g_free(path);
        return;
    }

    ScanJob *job = g_new0(ScanJob, 1);
    job->root = g_object_ref(p->root);
    job->scan = storage_scan_new(path, 0);
    g_free(path);

    p->scan = job->scan;
    p->scan_running = TRUE;
    gtk_widget_set_sensitive(p->scan_button, FALSE);
    gtk_widget_set_sensitive(p->scan_cancel_button, TRUE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(p->scan_progress), 0.0);
    gtk_label_set_text(GTK_LABEL(p->scan_status), "Starting scan…");

    GTask *task = g_task_new(NULL, NULL, scan_finished, p);
    g_task_set_task_data(task, job, scan_job_free);
    g_task_run_in_thread(task, scan_worker);
    g_object_unref(task);

    if (!p->scan_progress_id)
        p->scan_progress_id = g_timeout_add(SCAN_PROGRESS_MS, scan_progress_tick, p);
}

static void cancel_scan_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    StoragePage *p = user_data;
    if (!p->scan_running || !p->scan) return;
    gtk_widget_set_sensitive(p->scan_cancel_button, FALSE);
    gtk_label_set_text(GTK_LABEL(p->scan_status), "Cancelling scan…");
    storage_scan_cancel(p->scan);
}

static void quick_scan_clicked(GtkButton *button, gpointer user_data)
{
    const char *path = g_object_get_data(G_OBJECT(button), "scan-path");
    StoragePage *p = user_data;
    if (path) gtk_editable_set_text(GTK_EDITABLE(p->scan_path_entry), path);
    start_scan_clicked(NULL, p);
}

static void select_section(GtkButton *button, gpointer user_data)
{
    StoragePage *p = user_data;
    const char *name = g_object_get_data(G_OBJECT(button), "section-name");
    if (name) gtk_stack_set_visible_child_name(p->section_stack, name);
}

static GtkWidget *section_button(const char *label, const char *name, StoragePage *p)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(button, "flat");
    g_object_set_data(G_OBJECT(button), "section-name", (gpointer)name);
    g_signal_connect(button, "clicked", G_CALLBACK(select_section), p);
    return button;
}

static gboolean sample_tick(gpointer data)
{
    StoragePage *p = data;
    gint64 now = g_get_monotonic_time();
    double dt = p->last_sample_us > 0 ? (now - p->last_sample_us) / 1000000.0 : 1.0;
    if (dt <= 0.0) dt = 1.0;
    p->last_sample_us = now;

    for (guint i = 0; i < p->devices->len; ++i) {
        StorageDevice *d = g_ptr_array_index(p->devices, i);
        guint64 r = 0, w = 0;
        if (read_disk_stat(d->name, &r, &w)) {
            if (d->read_bytes && r >= d->read_bytes)
                d->read_rate = (double)(r - d->read_bytes) / dt;
            if (d->write_bytes && w >= d->write_bytes)
                d->write_rate = (double)(w - d->write_bytes) / dt;
            d->read_bytes = r;
            d->write_bytes = w;
            double rv = d->read_rate, wv = d->write_rate;
            g_array_append_val(d->read_history, rv);
            g_array_append_val(d->write_history, wv);
            while (d->read_history->len > HISTORY_LEN) g_array_remove_index(d->read_history, 0);
            while (d->write_history->len > HISTORY_LEN) g_array_remove_index(d->write_history, 0);
        }
        update_usage(d);
    }
    update_summary(p);
    update_details(p);
    gtk_widget_queue_draw(GTK_WIDGET(p->graph));
    return G_SOURCE_CONTINUE;
}

static gboolean device_refresh_tick(gpointer data)
{
    StoragePage *p = data;
    discover_devices(p);
    select_first(p);
    update_device_list(p);
    update_summary(p);
    update_details(p);
    update_partition_view(p);
    return G_SOURCE_CONTINUE;
}

static void storage_page_free(gpointer data)
{
    StoragePage *p = data;
    if (p->refresh_id) g_source_remove(p->refresh_id);
    if (p->device_refresh_id) g_source_remove(p->device_refresh_id);
    if (p->scan_progress_id) g_source_remove(p->scan_progress_id);
    if (p->scan) storage_scan_cancel(p->scan);
    g_clear_pointer(&p->devices, g_ptr_array_unref);
    g_free(p);
}

GtkWidget *storage_page_create(App *app)
{
    StoragePage *p = g_new0(StoragePage, 1);
    p->app = app;
    p->devices = g_ptr_array_new_with_free_func(storage_device_free);

    p->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(p->root, 16);
    gtk_widget_set_margin_end(p->root, 16);
    gtk_widget_set_margin_top(p->root, 14);
    gtk_widget_set_margin_bottom(p->root, 16);
    gtk_widget_add_css_class(p->root, "tm-storage-page");
    g_object_set_data_full(G_OBJECT(p->root), "storage-page-state", p, storage_page_free);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *title = gtk_label_new("Storage");
    GtkWidget *subtitle = gtk_label_new("Physical drives, capacity, activity, partitions and storage usage.");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0);
    gtk_widget_add_css_class(title, "title-2");
    gtk_widget_add_css_class(subtitle, "dim-label");
    gtk_box_append(GTK_BOX(header), title);
    gtk_box_append(GTK_BOX(header), subtitle);
    gtk_box_append(GTK_BOX(p->root), header);

    GtkWidget *summary = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(summary), 10);
    gtk_grid_set_row_spacing(GTK_GRID(summary), 10);
    gtk_widget_set_hexpand(summary, TRUE);
    gtk_grid_attach(GTK_GRID(summary), summary_card("Total capacity", &p->summary_total), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(summary), summary_card("Used", &p->summary_used), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(summary), summary_card("Free", &p->summary_free), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(summary), summary_card("Devices", &p->summary_devices), 3, 0, 1, 1);
    gtk_box_append(GTK_BOX(p->root), summary);

    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(nav, "linked");
    gtk_box_append(GTK_BOX(nav), section_button("Overview", "overview", p));
    gtk_box_append(GTK_BOX(nav), section_button("Partitions", "partitions", p));
    gtk_box_append(GTK_BOX(nav), section_button("Storage usage", "usage", p));
    gtk_box_append(GTK_BOX(p->root), nav);

    p->section_stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(p->section_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(p->section_stack, 120);
    gtk_widget_set_vexpand(GTK_WIDGET(p->section_stack), TRUE);

    GtkWidget *overview = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(overview), 340);
    gtk_widget_set_vexpand(overview, TRUE);

    GtkWidget *left_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(left_frame, "tm-storage-panel");
    GtkWidget *left_title = gtk_label_new("Physical drives");
    gtk_label_set_xalign(GTK_LABEL(left_title), 0);
    gtk_widget_add_css_class(left_title, "heading");
    gtk_box_append(GTK_BOX(left_frame), left_title);
    GtkWidget *left_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(left_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(left_scroll, TRUE);
    p->device_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(left_scroll), p->device_list);
    gtk_box_append(GTK_BOX(left_frame), left_scroll);

    GtkWidget *right_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(right_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(right_scroll, TRUE);
    gtk_widget_set_vexpand(right_scroll, TRUE);
    p->detail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(p->detail, 18);
    gtk_widget_set_margin_end(p->detail, 8);
    gtk_widget_set_margin_top(p->detail, 4);

    GtkWidget *dh = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    p->device_name = gtk_label_new("No storage device selected");
    p->device_meta = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(p->device_name), 0);
    gtk_label_set_xalign(GTK_LABEL(p->device_meta), 0);
    gtk_widget_add_css_class(p->device_name, "title-3");
    gtk_widget_add_css_class(p->device_meta, "dim-label");
    gtk_box_append(GTK_BOX(dh), p->device_name);
    gtk_box_append(GTK_BOX(dh), p->device_meta);
    gtk_box_append(GTK_BOX(p->detail), dh);

    GtkWidget *capacity_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_add_css_class(capacity_box, "tm-storage-card");
    GtkWidget *cap_title = gtk_label_new("Capacity");
    gtk_label_set_xalign(GTK_LABEL(cap_title), 0);
    gtk_widget_add_css_class(cap_title, "heading");
    p->capacity_bar = gtk_progress_bar_new();
    p->capacity_label = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(p->capacity_label), 0);
    gtk_widget_add_css_class(p->capacity_label, "dim-label");
    gtk_box_append(GTK_BOX(capacity_box), cap_title);
    gtk_box_append(GTK_BOX(capacity_box), p->capacity_bar);
    gtk_box_append(GTK_BOX(capacity_box), p->capacity_label);
    gtk_box_append(GTK_BOX(p->detail), capacity_box);

    GtkWidget *metrics = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(metrics), 18);
    gtk_grid_set_row_spacing(GTK_GRID(metrics), 12);
    gtk_grid_attach(GTK_GRID(metrics), summary_card("Read", &p->read_label), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(metrics), summary_card("Write", &p->write_label), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(metrics), summary_card("Temperature", &p->temp_label), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(metrics), summary_card("Health", &p->health_label), 1, 1, 1, 1);
    gtk_box_append(GTK_BOX(p->detail), metrics);

    GtkWidget *activity = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_add_css_class(activity, "tm-storage-card");
    GtkWidget *activity_title = gtk_label_new("Disk activity");
    gtk_label_set_xalign(GTK_LABEL(activity_title), 0);
    gtk_widget_add_css_class(activity_title, "heading");
    p->graph = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_drawing_area_set_content_height(p->graph, 180);
    gtk_widget_set_hexpand(GTK_WIDGET(p->graph), TRUE);
    gtk_drawing_area_set_draw_func(p->graph, draw_history, p, NULL);
    gtk_box_append(GTK_BOX(activity), activity_title);
    gtk_box_append(GTK_BOX(activity), GTK_WIDGET(p->graph));
    gtk_box_append(GTK_BOX(p->detail), activity);

    p->filesystem_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_add_css_class(p->filesystem_box, "tm-storage-card");
    gtk_box_append(GTK_BOX(p->detail), p->filesystem_box);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(right_scroll), p->detail);
    gtk_paned_set_start_child(GTK_PANED(overview), left_frame);
    gtk_paned_set_end_child(GTK_PANED(overview), right_scroll);
    gtk_stack_add_titled(p->section_stack, overview, "overview", "Overview");

    p->partitions_page = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->partitions_page), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    p->partition_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(p->partition_box, 4);
    gtk_widget_set_margin_end(p->partition_box, 8);
    gtk_widget_set_margin_top(p->partition_box, 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(p->partitions_page), p->partition_box);
    gtk_stack_add_titled(p->section_stack, p->partitions_page, "partitions", "Partitions");

    GtkWidget *usage_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *scan_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *path_label = gtk_label_new("Location");
    p->scan_path_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(p->scan_path_entry), "/");
    gtk_widget_set_hexpand(p->scan_path_entry, TRUE);
    p->scan_button = gtk_button_new_with_label("Scan");
    p->scan_cancel_button = gtk_button_new_with_label("Cancel");
    gtk_widget_set_sensitive(p->scan_cancel_button, FALSE);
    g_signal_connect(p->scan_button, "clicked", G_CALLBACK(start_scan_clicked), p);
    g_signal_connect(p->scan_cancel_button, "clicked", G_CALLBACK(cancel_scan_clicked), p);
    gtk_box_append(GTK_BOX(scan_controls), path_label);
    gtk_box_append(GTK_BOX(scan_controls), p->scan_path_entry);
    gtk_box_append(GTK_BOX(scan_controls), p->scan_button);
    gtk_box_append(GTK_BOX(scan_controls), p->scan_cancel_button);
    gtk_box_append(GTK_BOX(usage_page), scan_controls);

    GtkWidget *quick = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    const char *home = g_get_home_dir();
    GtkWidget *root_btn = gtk_button_new_with_label("/");
    GtkWidget *home_btn = gtk_button_new_with_label("Home");
    g_object_set_data(G_OBJECT(root_btn), "scan-path", "/");
    g_object_set_data_full(G_OBJECT(home_btn), "scan-path", g_strdup(home), g_free);
    g_signal_connect(root_btn, "clicked", G_CALLBACK(quick_scan_clicked), p);
    g_signal_connect(home_btn, "clicked", G_CALLBACK(quick_scan_clicked), p);
    gtk_box_append(GTK_BOX(quick), root_btn);
    gtk_box_append(GTK_BOX(quick), home_btn);
    gtk_box_append(GTK_BOX(usage_page), quick);

    p->scan_progress = gtk_progress_bar_new();
    gtk_widget_set_hexpand(p->scan_progress, TRUE);
    gtk_box_append(GTK_BOX(usage_page), p->scan_progress);
    p->scan_status = gtk_label_new("No storage scan has been run.");
    gtk_label_set_xalign(GTK_LABEL(p->scan_status), 0);
    gtk_widget_add_css_class(p->scan_status, "dim-label");
    gtk_box_append(GTK_BOX(usage_page), p->scan_status);
    p->usage_summary = gtk_label_new("—");
    gtk_label_set_xalign(GTK_LABEL(p->usage_summary), 0);
    gtk_box_append(GTK_BOX(usage_page), p->usage_summary);

    GtkWidget *usage_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(usage_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(usage_scroll, TRUE);
    p->usage_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(usage_scroll), p->usage_list);
    gtk_box_append(GTK_BOX(usage_page), usage_scroll);
    gtk_stack_add_titled(p->section_stack, usage_page, "usage", "Storage usage");

    gtk_box_append(GTK_BOX(p->root), GTK_WIDGET(p->section_stack));

    discover_devices(p);
    select_first(p);
    update_device_list(p);
    update_summary(p);
    update_details(p);
    update_partition_view(p);

    p->refresh_id = g_timeout_add(STORAGE_REFRESH_MS, sample_tick, p);
    p->device_refresh_id = g_timeout_add(DEVICE_REFRESH_MS, device_refresh_tick, p);
    return p->root;
}
