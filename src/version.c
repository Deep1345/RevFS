/*
 * version.c — Version history and file cataloging
 *
 * Provides inspection functions for tracked files and their version histories.
 */

#include "revfs.h"
#include <time.h>
#include <dirent.h>

int revfs_version_count(const char *filename)
{
    if (!filename) { errno = EINVAL; return -1; }

    int next = revfs_meta_next_version(filename);
    if (next < 0) return -1;
    return next - 1;
}

int revfs_version_list(const char *filename, revfs_meta_t *versions_out, int max_versions)
{
    if (!filename || !versions_out || max_versions <= 0) {
        errno = EINVAL;
        return -1;
    }

    int total = revfs_version_count(filename);
    if (total <= 0) return total;

    int count = 0;
    for (int v = 1; v <= total && count < max_versions; v++) {
        if (revfs_meta_read(filename, v, &versions_out[count]) == 0)
            count++;
    }
    return count;
}

int revfs_history(const char *filename)
{
    if (!filename) {
        fprintf(stderr, "revfs: history: invalid arguments\n");
        errno = EINVAL;
        return -1;
    }

    int total = revfs_version_count(filename);
    if (total < 0) return -1;

    if (total == 0) {
        fprintf(stderr, "revfs: history: no versions found for \"%s\"\n", filename);
        errno = ENOENT;
        return -1;
    }

    printf("\nHistory for \"%s\" — %d version%s\n",
           filename, total, total == 1 ? "" : "s");
    printf("─────────────────────────────────────────────────\n");

    revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
    if (!meta) return -1;

    int printed = 0;
    for (int v = 1; v <= total; v++) {
        if (revfs_meta_read(filename, v, meta) < 0)
            continue;

        char time_buf[64];
        time_t ts = (time_t)meta->timestamp;
        struct tm *tm_info = localtime(&ts);
        if (tm_info) {
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
        } else {
            snprintf(time_buf, sizeof(time_buf), "%ld", meta->timestamp);
        }

        char size_buf[32];
        if (meta->file_size >= 1024 * 1024) {
            snprintf(size_buf, sizeof(size_buf), "%.1f MB",
                     (double)meta->file_size / (1024.0 * 1024.0));
        } else if (meta->file_size >= 1024) {
            snprintf(size_buf, sizeof(size_buf), "%.1f KB",
                     (double)meta->file_size / 1024.0);
        } else {
            snprintf(size_buf, sizeof(size_buf), "%lld B", (long long)meta->file_size);
        }

        printf("  v%-4d  %8s  %3d chunk%s  %s%s\n",
               meta->version,
               size_buf,
               meta->num_chunks,
               meta->num_chunks == 1 ? " " : "s",
               time_buf,
               (v == total) ? "  ← latest" : "");
        printed++;
    }

    printf("─────────────────────────────────────────────────\n\n");
    free(meta);
    return printed;
}

int revfs_list_files(void)
{
    char (*names)[REVFS_MAX_FILENAME] = malloc(1024 * REVFS_MAX_FILENAME);
    if (!names) return -1;

    int count = revfs_meta_list_files(names, 1024);
    if (count < 0) { free(names); return -1; }

    if (count == 0) {
        printf("\nNo files stored in RevFS.\n\n");
        free(names);
        return 0;
    }

    printf("\nFiles stored in RevFS — %d file%s\n", count, count == 1 ? "" : "s");
    printf("─────────────────────────────────────────────────\n");

    revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
    if (!meta) { free(names); return -1; }

    for (int i = 0; i < count; i++) {
        int versions = revfs_version_count(names[i]);

        if (revfs_meta_read(names[i], -1, meta) == 0) {
            char size_buf[32];
            if (meta->file_size >= 1024 * 1024) {
                snprintf(size_buf, sizeof(size_buf), "%.1f MB",
                         (double)meta->file_size / (1024.0 * 1024.0));
            } else if (meta->file_size >= 1024) {
                snprintf(size_buf, sizeof(size_buf), "%.1f KB",
                         (double)meta->file_size / 1024.0);
            } else {
                snprintf(size_buf, sizeof(size_buf), "%lld B", (long long)meta->file_size);
            }

            printf("  %-30s  %d version%s  latest: %s\n",
                   names[i], versions, versions == 1 ? " " : "s", size_buf);
        } else {
            printf("  %-30s  %d version%s\n",
                   names[i], versions, versions == 1 ? " " : "s");
        }
    }

    printf("─────────────────────────────────────────────────\n\n");
    free(meta);
    free(names);
    return count;
}
