/*
 * RevFS — Versioned Distributed File Storage System
 *
 * Day 6: File Versioning
 *
 * This module implements version history and file listing:
 *
 *   revfs_history()       — Display all versions of a file with metadata
 *   revfs_version_list()  — Get structured version info for a file
 *   revfs_version_count() — Count how many versions exist for a file
 *   revfs_list_files()    — List all files stored in RevFS
 *
 * All functions build on the metadata infrastructure from Day 4
 * (revfs_meta_read, revfs_meta_next_version, revfs_meta_list_files).
 */

#include "revfs.h"
#include <time.h>
#include <dirent.h>

/* ------------------------------------------------------------------ */
/*  revfs_version_count                                                */
/*                                                                     */
/*  Returns the number of versions that exist for a given filename.    */
/*  Returns 0 if the file has never been uploaded, -1 on error.        */
/* ------------------------------------------------------------------ */
int revfs_version_count(const char *filename)
{
    if (!filename) {
        errno = EINVAL;
        return -1;
    }

    int next = revfs_meta_next_version(filename);
    if (next < 0)
        return -1;

    /* next_version returns 1 if no versions exist → count = 0 */
    return next - 1;
}

/* ------------------------------------------------------------------ */
/*  revfs_version_list                                                 */
/*                                                                     */
/*  Reads all version metadata for a file into a caller-supplied       */
/*  array of revfs_meta_t.  Returns the number of versions read,       */
/*  or -1 on error.                                                    */
/*                                                                     */
/*  `versions_out` must point to an array of at least `max_versions`   */
/*  elements.  Each element is heap-sized (~260 KB), so the caller     */
/*  should allocate on the heap.                                       */
/* ------------------------------------------------------------------ */
int revfs_version_list(const char *filename,
                       revfs_meta_t *versions_out, int max_versions)
{
    if (!filename || !versions_out || max_versions <= 0) {
        errno = EINVAL;
        return -1;
    }

    int total = revfs_version_count(filename);
    if (total <= 0)
        return total;  /* 0 = no versions, -1 = error */

    int count = 0;
    for (int v = 1; v <= total && count < max_versions; v++) {
        if (revfs_meta_read(filename, v, &versions_out[count]) == 0)
            count++;
        /* If a version file is missing/corrupt, skip it silently */
    }

    return count;
}

/* ------------------------------------------------------------------ */
/*  revfs_history                                                      */
/*                                                                     */
/*  Prints the full version history for a file to stdout.              */
/*  Each version shows: version number, size, chunk count, timestamp.  */
/*                                                                     */
/*  Returns the number of versions printed, -1 on error.               */
/* ------------------------------------------------------------------ */
int revfs_history(const char *filename)
{
    if (!filename) {
        fprintf(stderr, "revfs: history: invalid arguments\n");
        errno = EINVAL;
        return -1;
    }

    /* 1. Count versions */
    int total = revfs_version_count(filename);
    if (total < 0)
        return -1;

    if (total == 0) {
        fprintf(stderr, "revfs: history: no versions found for \"%s\"\n",
                filename);
        errno = ENOENT;
        return -1;
    }

    /* 2. Print header */
    printf("\n");
    printf("History for \"%s\" — %d version%s\n",
           filename, total, total == 1 ? "" : "s");
    printf("─────────────────────────────────────────────────\n");

    /* 3. Read and display each version */
    revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
    if (!meta)
        return -1;

    int printed = 0;
    for (int v = 1; v <= total; v++) {
        if (revfs_meta_read(filename, v, meta) < 0)
            continue;   /* skip corrupt/missing versions */

        /* Format the timestamp */
        char time_buf[64];
        time_t ts = (time_t)meta->timestamp;
        struct tm *tm_info = localtime(&ts);
        if (tm_info) {
            strftime(time_buf, sizeof(time_buf),
                     "%Y-%m-%d %H:%M:%S", tm_info);
        } else {
            snprintf(time_buf, sizeof(time_buf), "%ld", meta->timestamp);
        }

        /* Format file size in human-readable form */
        char size_buf[32];
        if (meta->file_size >= 1024 * 1024) {
            snprintf(size_buf, sizeof(size_buf), "%.1f MB",
                     (double)meta->file_size / (1024.0 * 1024.0));
        } else if (meta->file_size >= 1024) {
            snprintf(size_buf, sizeof(size_buf), "%.1f KB",
                     (double)meta->file_size / 1024.0);
        } else {
            snprintf(size_buf, sizeof(size_buf), "%lld B",
                     (long long)meta->file_size);
        }

        printf("  v%-4d  %8s  %3d chunk%s  %s",
               meta->version,
               size_buf,
               meta->num_chunks,
               meta->num_chunks == 1 ? " " : "s",
               time_buf);

        /* Mark the latest version */
        if (v == total)
            printf("  ← latest");

        printf("\n");
        printed++;
    }

    printf("─────────────────────────────────────────────────\n\n");

    free(meta);
    return printed;
}

/* ------------------------------------------------------------------ */
/*  revfs_list_files                                                   */
/*                                                                     */
/*  Lists all files stored in RevFS with version counts.               */
/*  Prints a summary to stdout.                                        */
/*                                                                     */
/*  Returns the number of files found, -1 on error.                    */
/* ------------------------------------------------------------------ */
int revfs_list_files(void)
{
    /* 1. Get the list of all files */
    char (*names)[REVFS_MAX_FILENAME] =
        malloc(1024 * REVFS_MAX_FILENAME);
    if (!names)
        return -1;

    int count = revfs_meta_list_files(names, 1024);
    if (count < 0) {
        free(names);
        return -1;
    }

    if (count == 0) {
        printf("\nNo files stored in RevFS.\n\n");
        free(names);
        return 0;
    }

    /* 2. Print header */
    printf("\n");
    printf("Files stored in RevFS — %d file%s\n",
           count, count == 1 ? "" : "s");
    printf("─────────────────────────────────────────────────\n");

    /* 3. For each file, show version count and latest version info */
    revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
    if (!meta) {
        free(names);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        int versions = revfs_version_count(names[i]);

        /* Try to read the latest metadata for additional info */
        if (revfs_meta_read(names[i], -1, meta) == 0) {
            /* Format file size */
            char size_buf[32];
            if (meta->file_size >= 1024 * 1024) {
                snprintf(size_buf, sizeof(size_buf), "%.1f MB",
                         (double)meta->file_size / (1024.0 * 1024.0));
            } else if (meta->file_size >= 1024) {
                snprintf(size_buf, sizeof(size_buf), "%.1f KB",
                         (double)meta->file_size / 1024.0);
            } else {
                snprintf(size_buf, sizeof(size_buf), "%lld B",
                         (long long)meta->file_size);
            }

            printf("  %-30s  %d version%s  latest: %s\n",
                   names[i],
                   versions, versions == 1 ? " " : "s",
                   size_buf);
        } else {
            printf("  %-30s  %d version%s\n",
                   names[i],
                   versions, versions == 1 ? " " : "s");
        }
    }

    printf("─────────────────────────────────────────────────\n\n");

    free(meta);
    free(names);
    return count;
}
