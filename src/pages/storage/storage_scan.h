#ifndef TASK_MANAGER_STORAGE_SCAN_H
#define TASK_MANAGER_STORAGE_SCAN_H

/*
 * storage_scan - concurrent "disk usage by file category" scanner.
 *
 * Pure GLib/POSIX: no GTK in here, so it can be unit-tested or reused from a
 * CLI. Only aggregate counters are stored (per category, per extension), never
 * individual file names, so memory stays flat even for millions of files.
 */

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    STORAGE_CAT_SYSTEM,
    STORAGE_CAT_APPLICATIONS,
    STORAGE_CAT_VIDEOS,
    STORAGE_CAT_IMAGES,
    STORAGE_CAT_MUSIC,
    STORAGE_CAT_DOCUMENTS,
    STORAGE_CAT_ARCHIVES,
    STORAGE_CAT_SOURCE_CODE,
    STORAGE_CAT_DATABASES,
    STORAGE_CAT_FONTS,
    STORAGE_CAT_DISK_IMAGES,
    STORAGE_CAT_EXECUTABLES,
    STORAGE_CAT_CONFIG,
    STORAGE_CAT_LOGS,
    STORAGE_CAT_TEMPORARY,
    STORAGE_CAT_VMS,
    STORAGE_CAT_OTHER,
    STORAGE_CAT_COUNT
} StorageCategory;

#define STORAGE_NO_EXTENSION "(no extension)"

typedef struct {
    guint64 count;
    guint64 total_size;
    guint64 largest_size;
} StorageExtStats;

typedef struct {
    StorageCategory id;
    GHashTable *extensions;
    guint64 total_size;
    guint64 total_count;
} StorageCategoryStats;

typedef struct {
    char *root;
    StorageCategoryStats cats[STORAGE_CAT_COUNT];
    guint64 total_files;
    guint64 total_bytes;
    guint64 errors;
    gint64 elapsed_us;
    gboolean cancelled;
} StorageScanResult;

typedef struct {
    guint64 files;
    guint64 bytes;
    guint64 errors;
    double elapsed;
    char current_dir[512];
} StorageScanProgress;

typedef struct _StorageScan StorageScan;

StorageScan *storage_scan_new(const char *root, int workers);
StorageScanResult *storage_scan_run(StorageScan *scan);
void storage_scan_get_progress(StorageScan *scan, StorageScanProgress *out);
void storage_scan_cancel(StorageScan *scan);
void storage_scan_free(StorageScan *scan);
void storage_scan_result_free(StorageScanResult *result);

const char *storage_category_name(StorageCategory cat);
const char *storage_category_color(StorageCategory cat);
void storage_result_sorted_categories(const StorageScanResult *r,
                                      StorageCategory *order);

typedef struct {
    const char *extension;
    const StorageExtStats *stats;
} StorageExtEntry;

GPtrArray *storage_category_sorted_extensions(const StorageCategoryStats *cat);
const char *storage_format_bytes(char *buf, gsize n, guint64 bytes);
void storage_format_duration(char *buf, gsize n, double seconds);
double storage_percentage(guint64 part, guint64 whole);

gboolean storage_result_export_json(const StorageScanResult *r,
                                    const char *path,
                                    GError **error);
gboolean storage_result_export_csv(const StorageScanResult *r,
                                   const char *path,
                                   GError **error);

G_END_DECLS

#endif
