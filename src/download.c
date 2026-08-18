/*
 * RevFS — Versioned Distributed File Storage System
 *
 * Day 5: Download + File Reconstruction
 *
 * This module implements the download pipeline:
 *   1. Read the metadata for the requested filename and version
 *   2. Verify all chunks referenced in the metadata exist in the store
 *   3. Reassemble the original file from the ordered chunk list
 *   4. Verify the reconstructed file's size matches the metadata
 *
 * The download is the inverse of the upload pipeline (Day 4):
 *   upload:   file → chunks → store → metadata
 *   download: metadata → store → chunks → file
 */

#include "revfs.h"
#include <time.h>

/* ------------------------------------------------------------------ */
/*  revfs_download                                                     */
/*                                                                     */
/*  Downloads (reconstructs) a file from the RevFS store.              */
/*                                                                     */
/*  `filename`    — the basename of the file (as stored in metadata).  */
/*  `version`     — the version to download, or -1 for latest.        */
/*  `output_path` — where to write the reconstructed file.             */
/*                                                                     */
/*  Pipeline:                                                          */
/*    1. Read metadata for filename + version                          */
/*    2. Verify all chunk hashes exist in the store                    */
/*    3. Reassemble chunks → output file                               */
/*    4. Verify reconstructed size matches metadata                    */
/*                                                                     */
/*  Returns 0 on success, -1 on error.                                */
/* ------------------------------------------------------------------ */
int revfs_download(const char *filename, int version, const char *output_path)
{
    if (!filename || !output_path) {
        fprintf(stderr, "revfs: download: invalid arguments\n");
        errno = EINVAL;
        return -1;
    }

    /* 1. Read the metadata */
    revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
    if (!meta) {
        fprintf(stderr, "revfs: download: malloc: %s\n", strerror(errno));
        return -1;
    }

    if (revfs_meta_read(filename, version, meta) < 0) {
        fprintf(stderr, "revfs: download: cannot read metadata for \"%s\"",
                filename);
        if (version > 0)
            fprintf(stderr, " version %d", version);
        fprintf(stderr, "\n");
        free(meta);
        return -1;
    }

    /* 2. Verify all chunks exist in the store */
    for (int i = 0; i < meta->num_chunks; i++) {
        if (!revfs_chunk_exists(meta->chunk_hashes[i])) {
            fprintf(stderr,
                    "revfs: download: missing chunk %d/%d (hash: %.16s...)\n",
                    i + 1, meta->num_chunks, meta->chunk_hashes[i]);
            free(meta);
            errno = ENOENT;
            return -1;
        }
    }

    /* 3. Reassemble the file from chunks */
    ssize_t written = revfs_chunks_reassemble(
        output_path,
        (const char (*)[REVFS_HASH_HEX_SIZE])meta->chunk_hashes,
        meta->num_chunks
    );

    if (written < 0) {
        fprintf(stderr, "revfs: download: reassembly failed for \"%s\"\n",
                filename);
        /* Clean up the partial output file */
        unlink(output_path);
        free(meta);
        return -1;
    }

    /* 4. Verify reconstructed size matches metadata */
    if (written != (ssize_t)meta->file_size) {
        fprintf(stderr,
                "revfs: download: size mismatch! expected %lld, got %zd\n",
                (long long)meta->file_size, written);
        unlink(output_path);
        free(meta);
        errno = EIO;
        return -1;
    }

    /* 5. Print success message */
    printf("Downloaded \"%s\" v%d → \"%s\" (%zd bytes, %d chunks)\n",
           meta->name, meta->version, output_path, written, meta->num_chunks);

    free(meta);
    return 0;
}
