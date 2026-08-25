/*
 * download.c — Download and reconstruct files from CAS
 *
 * Reads version metadata, verifies all referenced chunks are available
 * in the local CAS, and reassembles them into the target output path.
 */

#include "revfs.h"
#include <time.h>

int revfs_download(const char *filename, int version, const char *output_path)
{
    if (!filename || !output_path) {
        fprintf(stderr, "revfs: download: invalid arguments\n");
        errno = EINVAL;
        return -1;
    }

    revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
    if (!meta) {
        fprintf(stderr, "revfs: download: malloc failed: %s\n", strerror(errno));
        return -1;
    }

    if (revfs_meta_read(filename, version, meta) < 0) {
        fprintf(stderr, "revfs: download: cannot read metadata for \"%s\"", filename);
        if (version > 0) fprintf(stderr, " version %d", version);
        fprintf(stderr, "\n");
        free(meta);
        return -1;
    }

    /* Verify all referenced chunks exist before attempting reassembly */
    for (int i = 0; i < meta->num_chunks; i++) {
        if (!revfs_chunk_exists(meta->chunk_hashes[i])) {
            fprintf(stderr, "revfs: download: missing chunk %d/%d (hash: %.16s...)\n",
                    i + 1, meta->num_chunks, meta->chunk_hashes[i]);
            free(meta);
            errno = ENOENT;
            return -1;
        }
    }

    /* Reassemble chunks into destination file */
    ssize_t written = revfs_chunks_reassemble(
        output_path,
        (const char (*)[REVFS_HASH_HEX_SIZE])meta->chunk_hashes,
        meta->num_chunks
    );

    if (written < 0) {
        fprintf(stderr, "revfs: download: reassembly failed for \"%s\"\n", filename);
        unlink(output_path);
        free(meta);
        return -1;
    }

    if (written != (ssize_t)meta->file_size) {
        fprintf(stderr, "revfs: download: size mismatch! expected %lld, got %zd\n",
                (long long)meta->file_size, written);
        unlink(output_path);
        free(meta);
        errno = EIO;
        return -1;
    }

    printf("Downloaded \"%s\" v%d → \"%s\" (%zd bytes, %d chunks)\n",
           meta->name, meta->version, output_path, written, meta->num_chunks);

    free(meta);
    return 0;
}
