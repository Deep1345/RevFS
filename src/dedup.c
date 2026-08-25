/*
 * dedup.c — Storage metrics & deduplication analytics
 *
 * Scans metadata manifests and the content-addressed store to compute
 * logical vs physical storage usage, unique chunks, and space savings.
 */

#include "revfs.h"
#include <dirent.h>
#include <sys/stat.h>

#define HASH_TABLE_SIZE 4096

typedef struct hash_entry {
    char hash[REVFS_HASH_HEX_SIZE];
    struct hash_entry *next;
} hash_entry_t;

/* DJB2 string hash */
static unsigned int hash_key(const char *str)
{
    unsigned int h = 5381;
    int c;
    while ((c = *str++))
        h = ((h << 5) + h) + (unsigned int)c;
    return h % HASH_TABLE_SIZE;
}

/* Helper: format human-readable byte sizes */
static void format_size(off_t size, char *out, size_t out_len)
{
    if (size < 1024) {
        snprintf(out, out_len, "%lld B", (long long)size);
    } else if (size < 1024 * 1024) {
        snprintf(out, out_len, "%.2f KB", (double)size / 1024.0);
    } else if (size < 1024LL * 1024 * 1024) {
        snprintf(out, out_len, "%.2f MB", (double)size / (1024.0 * 1024.0));
    } else {
        snprintf(out, out_len, "%.2f GB",
                 (double)size / (1024.0 * 1024.0 * 1024.0));
    }
}

/* ------------------------------------------------------------------ */
/*  revfs_stats_chunks_info                                           */
/*                                                                    */
/*  Scans the content-addressed storage directory (data/chunks/)      */
/*  to count the total number of unique chunk files and sum their     */
/*  actual physical disk sizes.                                       */
/* ------------------------------------------------------------------ */
int revfs_stats_chunks_info(int *unique_chunks_out, off_t *physical_bytes_out)
{
    if (!unique_chunks_out || !physical_bytes_out) {
        errno = EINVAL;
        return -1;
    }

    *unique_chunks_out = 0;
    *physical_bytes_out = 0;

    char chunks_dir[REVFS_MAX_PATH];
    snprintf(chunks_dir, sizeof(chunks_dir), "%s/chunks", REVFS_DATA_DIR);

    if (!revfs_file_exists(chunks_dir)) {
        return 0;
    }

    DIR *main_dp = opendir(chunks_dir);
    if (!main_dp) {
        return 0;
    }

    struct dirent *sub_entry;
    while ((sub_entry = readdir(main_dp)) != NULL) {
        /* Skip . and .. */
        if (strcmp(sub_entry->d_name, ".") == 0 ||
            strcmp(sub_entry->d_name, "..") == 0) {
            continue;
        }

        char sub_path[REVFS_MAX_PATH];
        snprintf(sub_path, sizeof(sub_path), "%s/%s", chunks_dir, sub_entry->d_name);

        struct stat st_sub;
        if (stat(sub_path, &st_sub) != 0 || !S_ISDIR(st_sub.st_mode)) {
            continue;
        }

        DIR *sub_dp = opendir(sub_path);
        if (!sub_dp) continue;

        struct dirent *chunk_entry;
        while ((chunk_entry = readdir(sub_dp)) != NULL) {
            /* Skip hidden or temporary files (e.g. .tmp.*) */
            if (chunk_entry->d_name[0] == '.') {
                continue;
            }

            char chunk_path[REVFS_MAX_PATH];
            snprintf(chunk_path, sizeof(chunk_path), "%s/%s",
                     sub_path, chunk_entry->d_name);

            struct stat st_chunk;
            if (stat(chunk_path, &st_chunk) == 0 && S_ISREG(st_chunk.st_mode)) {
                (*unique_chunks_out)++;
                *physical_bytes_out += st_chunk.st_size;
            }
        }
        closedir(sub_dp);
    }
    closedir(main_dp);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_stats_calculate                                             */
/*                                                                    */
/*  Calculates complete storage and deduplication statistics for      */
/*  the local RevFS data repository.                                  */
/* ------------------------------------------------------------------ */
int revfs_stats_calculate(revfs_stats_t *stats_out)
{
    if (!stats_out) {
        errno = EINVAL;
        return -1;
    }

    memset(stats_out, 0, sizeof(revfs_stats_t));

    /* Hash table to track unique chunk hashes across all file versions */
    hash_entry_t *hash_table[HASH_TABLE_SIZE];
    memset(hash_table, 0, sizeof(hash_table));

    /* 1. List all files */
    char names[1024][REVFS_MAX_FILENAME];
    int file_count = revfs_meta_list_files(names, 1024);
    if (file_count < 0) {
        file_count = 0;
    }
    stats_out->total_files = file_count;

    /* 2. Traverse versions for each file */
    revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
    if (!meta) {
        errno = ENOMEM;
        return -1;
    }

    for (int i = 0; i < file_count; i++) {
        int vcount = revfs_version_count(names[i]);
        if (vcount <= 0) continue;

        for (int v = 1; v <= vcount; v++) {
            if (revfs_meta_read(names[i], v, meta) == 0) {
                stats_out->total_versions++;
                stats_out->logical_bytes += meta->file_size;
                stats_out->referenced_chunks += meta->num_chunks;

                /* Track unique chunk hashes referenced */
                for (int c = 0; c < meta->num_chunks; c++) {
                    const char *h = meta->chunk_hashes[c];
                    if (strlen(h) != 64) continue;

                    unsigned int idx = hash_key(h);
                    hash_entry_t *entry = hash_table[idx];
                    int found = 0;
                    while (entry) {
                        if (strcmp(entry->hash, h) == 0) {
                            found = 1;
                            break;
                        }
                        entry = entry->next;
                    }

                    if (!found) {
                        hash_entry_t *new_entry = malloc(sizeof(hash_entry_t));
                        if (new_entry) {
                            strncpy(new_entry->hash, h, REVFS_HASH_HEX_SIZE - 1);
                            new_entry->hash[REVFS_HASH_HEX_SIZE - 1] = '\0';
                            new_entry->next = hash_table[idx];
                            hash_table[idx] = new_entry;

                            stats_out->unique_chunks++;

                            /* Measure physical size of chunk on disk */
                            char chunk_path[REVFS_MAX_PATH];
                            if (revfs_chunk_store_path(h, chunk_path, sizeof(chunk_path)) == 0) {
                                off_t csz = revfs_file_size_path(chunk_path);
                                if (csz > 0) {
                                    stats_out->physical_bytes += csz;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    free(meta);

    /* Free hash table */
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        hash_entry_t *entry = hash_table[i];
        while (entry) {
            hash_entry_t *next = entry->next;
            free(entry);
            entry = next;
        }
    }

    /* 3. Fallback: if no files tracked in meta but chunks exist, report physical */
    if (stats_out->total_files == 0) {
        int u_chunks = 0;
        off_t p_bytes = 0;
        revfs_stats_chunks_info(&u_chunks, &p_bytes);
        stats_out->unique_chunks = u_chunks;
        stats_out->physical_bytes = p_bytes;
    }

    /* 4. Calculate deduplication savings & ratios */
    if (stats_out->logical_bytes > 0 && stats_out->physical_bytes > 0) {
        stats_out->dedup_ratio = (double)stats_out->logical_bytes / (double)stats_out->physical_bytes;
        if (stats_out->logical_bytes >= stats_out->physical_bytes) {
            stats_out->savings_bytes = stats_out->logical_bytes - stats_out->physical_bytes;
        } else {
            stats_out->savings_bytes = 0;
        }
        stats_out->savings_percent = ((double)stats_out->savings_bytes / (double)stats_out->logical_bytes) * 100.0;
    } else {
        /* Empty or 1:1 */
        stats_out->dedup_ratio = 1.0;
        stats_out->savings_bytes = 0;
        stats_out->savings_percent = 0.0;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_stats_print                                                 */
/*                                                                    */
/*  Prints a formatted report of storage & deduplication stats.       */
/* ------------------------------------------------------------------ */
int revfs_stats_print(const revfs_stats_t *stats)
{
    if (!stats) {
        errno = EINVAL;
        return -1;
    }

    char logical_str[64];
    char physical_str[64];
    char savings_str[64];

    format_size(stats->logical_bytes, logical_str, sizeof(logical_str));
    format_size(stats->physical_bytes, physical_str, sizeof(physical_str));
    format_size(stats->savings_bytes, savings_str, sizeof(savings_str));

    int dedup_chunks = stats->referenced_chunks - stats->unique_chunks;
    if (dedup_chunks < 0) dedup_chunks = 0;

    printf("\n");
    printf("RevFS Storage & Deduplication Statistics\n");
    printf("─────────────────────────────────────────────────────────────\n");
    printf("  Stored Files:              %d\n", stats->total_files);
    printf("  Total Versions:            %d\n", stats->total_versions);
    printf("  Total Referenced Chunks:   %d\n", stats->referenced_chunks);
    if (dedup_chunks > 0) {
        printf("  Unique Chunks in CAS:      %d (%d deduplicated)\n",
               stats->unique_chunks, dedup_chunks);
    } else {
        printf("  Unique Chunks in CAS:      %d\n", stats->unique_chunks);
    }
    printf("─────────────────────────────────────────────────────────────\n");
    printf("  Logical Data Stored:       %s\n", logical_str);
    printf("  Physical Disk Usage (CAS): %s\n", physical_str);
    printf("  Space Saved:               %s (%.1f%%)\n",
           savings_str, stats->savings_percent);
    printf("  Deduplication Ratio:       %.2fx\n", stats->dedup_ratio);
    printf("─────────────────────────────────────────────────────────────\n");
    printf("\n");

    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_stats                                                       */
/*                                                                    */
/*  Calculates and displays storage statistics to stdout.             */
/* ------------------------------------------------------------------ */
int revfs_stats(void)
{
    revfs_stats_t stats;
    if (revfs_stats_calculate(&stats) < 0) {
        fprintf(stderr, "revfs: stats: failed to calculate storage statistics\n");
        return -1;
    }

    return revfs_stats_print(&stats);
}
