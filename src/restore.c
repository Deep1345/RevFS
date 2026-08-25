/*
 * restore.c — Non-destructive version restore
 *
 * Restores a prior version of a file by creating a new version entry
 * pointing to the exact same chunks. Preserves history without duplicating data.
 */

#include "revfs.h"
#include <time.h>

int revfs_restore(const char *filename, int source_version)
{
    if (!filename || source_version < 1) {
        fprintf(stderr, "revfs: restore: invalid arguments\n");
        errno = EINVAL;
        return -1;
    }

    revfs_meta_t *source = calloc(1, sizeof(revfs_meta_t));
    if (!source) {
        fprintf(stderr, "revfs: restore: malloc failed: %s\n", strerror(errno));
        return -1;
    }

    if (revfs_meta_read(filename, source_version, source) < 0) {
        fprintf(stderr, "revfs: restore: cannot read metadata for \"%s\" version %d\n",
                filename, source_version);
        free(source);
        return -1;
    }

    /* Verify all referenced chunks still exist in CAS */
    for (int i = 0; i < source->num_chunks; i++) {
        if (!revfs_chunk_exists(source->chunk_hashes[i])) {
            fprintf(stderr, "revfs: restore: missing chunk %d/%d (hash: %.16s...)\n",
                    i + 1, source->num_chunks, source->chunk_hashes[i]);
            free(source);
            errno = ENOENT;
            return -1;
        }
    }

    /* Determine next version number under metadata lock */
    revfs_lock_meta();
    int new_version = revfs_meta_next_version(filename);
    if (new_version < 0) {
        revfs_unlock_meta();
        free(source);
        return -1;
    }

    if (source_version == new_version - 1) {
        revfs_unlock_meta();
        fprintf(stderr, "revfs: restore: version %d is already the latest version\n", source_version);
        free(source);
        errno = EEXIST;
        return -1;
    }

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

    for (int i = 0; i < source->num_chunks; i++) {
        memcpy(restored->chunk_hashes[i], source->chunk_hashes[i], REVFS_HASH_HEX_SIZE);
    }
    free(source);

    if (revfs_meta_write(restored) < 0) {
        revfs_unlock_meta();
        fprintf(stderr, "revfs: restore: failed to write metadata\n");
        free(restored);
        return -1;
    }
    revfs_unlock_meta();

    printf("Restored \"%s\" v%d → v%d (%d chunks, %lld bytes)\n",
           filename, source_version, new_version,
           restored->num_chunks, (long long)restored->file_size);

    free(restored);
    return new_version;
}
