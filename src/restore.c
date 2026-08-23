/*
 * RevFS — Versioned Distributed File Storage System
 *
 * Day 7: Version Restore
 *
 * This module implements non-destructive version restore:
 *
 *   revfs_restore()  — Restore a specific version of a file by creating
 *                      a new version that copies the old version's metadata.
 *
 * "Non-destructive" means the old version is never modified or deleted.
 * Instead, a new version is created that references the same chunks as
 * the source version.  This preserves the full history and leverages
 * the existing content-addressed deduplication — no data is copied.
 *
 * Pipeline:
 *   1. Validate inputs (filename, version number)
 *   2. Read the metadata for the source version
 *   3. Verify all chunks referenced by the source version still exist
 *   4. Determine the next version number
 *   5. Write a new metadata file that copies the source's chunk list
 *   6. Print a success message
 */

#include "revfs.h"
#include <time.h>

/* ------------------------------------------------------------------ */
/*  revfs_restore                                                      */
/*                                                                     */
/*  Restores a file to a specific version by creating a new version    */
/*  that references the same chunks as the source version.             */
/*                                                                     */
/*  `filename`        — basename of the file (as stored in metadata).  */
/*  `source_version`  — the version to restore (must be >= 1).        */
/*                                                                     */
/*  Returns the new version number on success, -1 on error.            */
/* ------------------------------------------------------------------ */
int revfs_restore(const char *filename, int source_version)
{
    if (!filename || source_version < 1) {
        fprintf(stderr, "revfs: restore: invalid arguments\n");
        errno = EINVAL;
        return -1;
    }

    /* 1. Read the source version's metadata */
    revfs_meta_t *source = calloc(1, sizeof(revfs_meta_t));
    if (!source) {
        fprintf(stderr, "revfs: restore: malloc: %s\n", strerror(errno));
        return -1;
    }

    if (revfs_meta_read(filename, source_version, source) < 0) {
        fprintf(stderr,
                "revfs: restore: cannot read metadata for \"%s\" version %d\n",
                filename, source_version);
        free(source);
        return -1;
    }

    /* 2. Verify all chunks still exist in the content-addressed store */
    for (int i = 0; i < source->num_chunks; i++) {
        if (!revfs_chunk_exists(source->chunk_hashes[i])) {
            fprintf(stderr,
                    "revfs: restore: missing chunk %d/%d (hash: %.16s...)\n",
                    i + 1, source->num_chunks, source->chunk_hashes[i]);
            free(source);
            errno = ENOENT;
            return -1;
        }
    }

    /* 3. Determine the next version number under metadata lock */
    revfs_lock_meta();
    int new_version = revfs_meta_next_version(filename);
    if (new_version < 0) {
        revfs_unlock_meta();
        free(source);
        return -1;
    }

    /* 4. Check that we are not restoring to the already-latest version */
    if (source_version == new_version - 1) {
        revfs_unlock_meta();
        fprintf(stderr,
                "revfs: restore: version %d is already the latest version\n",
                source_version);
        free(source);
        errno = EEXIST;
        return -1;
    }

    /* 5. Build the new metadata record — same chunks, new version + timestamp */
    revfs_meta_t *restored = calloc(1, sizeof(revfs_meta_t));
    if (!restored) {
        revfs_unlock_meta();
        free(source);
        return -1;
    }

    strncpy(restored->name, source->name, REVFS_MAX_FILENAME - 1);
    restored->version    = new_version;
    restored->num_chunks = source->num_chunks;
    restored->file_size  = source->file_size;
    restored->timestamp  = (long)time(NULL);

    /* Copy chunk hashes (these reference existing content-addressed chunks) */
    for (int i = 0; i < source->num_chunks; i++) {
        memcpy(restored->chunk_hashes[i], source->chunk_hashes[i],
               REVFS_HASH_HEX_SIZE);
    }

    free(source);

    /* 6. Write the new version's metadata */
    if (revfs_meta_write(restored) < 0) {
        revfs_unlock_meta();
        fprintf(stderr, "revfs: restore: failed to write metadata\n");
        free(restored);
        return -1;
    }
    revfs_unlock_meta();

    /* 7. Print success message */
    printf("Restored \"%s\" v%d → v%d (%d chunks, %lld bytes)\n",
           filename, source_version, new_version,
           restored->num_chunks, (long long)restored->file_size);

    free(restored);
    return new_version;
}
