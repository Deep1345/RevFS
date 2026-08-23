/*
 * RevFS — Versioned Distributed File Storage System
 *
 * Day 4: Upload Logic + Metadata Persistence
 *
 * This module implements the upload pipeline:
 *   1. Validate the source file exists and is readable
 *   2. Split the file into 4 MB chunks via revfs_file_chunk()
 *   3. Store each chunk in the content-addressed store (Day 3)
 *   4. Determine the next version number for this filename
 *   5. Write a metadata file recording the version info
 *
 * Metadata format (line-oriented key=value, no JSON library needed):
 *
 *   name=myfile.txt
 *   version=1
 *   chunks=3
 *   size=12582912
 *   timestamp=1723886400
 *   hash.0=abcdef0123456789...
 *   hash.1=fedcba9876543210...
 *   hash.2=1234567890abcdef...
 *
 * Metadata lives at:  data/meta/<filename>/v<N>.meta
 */

#include "revfs.h"
#include <time.h>
#include <dirent.h>
#include <libgen.h>

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/*
 * Extract the basename from a path.  We copy into a caller-supplied
 * buffer because POSIX basename() may modify its argument.
 */
static int safe_basename(const char *path, char *out, size_t out_size)
{
    if (!path || !out || out_size == 0) {
        errno = EINVAL;
        return -1;
    }

    /* Work on a mutable copy (basename may modify the string) */
    size_t len = strlen(path);
    char *tmp = malloc(len + 1);
    if (!tmp) return -1;
    memcpy(tmp, path, len + 1);

    char *base = basename(tmp);
    if (!base || strlen(base) == 0 || strcmp(base, "/") == 0) {
        free(tmp);
        errno = EINVAL;
        return -1;
    }

    if (strlen(base) >= out_size) {
        free(tmp);
        errno = ENAMETOOLONG;
        return -1;
    }

    strncpy(out, base, out_size - 1);
    out[out_size - 1] = '\0';
    free(tmp);
    return 0;
}

/*
 * Build the metadata directory path: data/meta/<filename>/
 */
static int meta_dir_path(const char *filename, char *out, size_t out_size)
{
    int n = snprintf(out, out_size, "%s/meta/%s", REVFS_DATA_DIR, filename);
    if (n < 0 || (size_t)n >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/*
 * Build the metadata file path: data/meta/<filename>/v<version>.meta
 */
static int meta_file_path(const char *filename, int version,
                          char *out, size_t out_size)
{
    int n = snprintf(out, out_size, "%s/meta/%s/v%d.meta",
                     REVFS_DATA_DIR, filename, version);
    if (n < 0 || (size_t)n >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_meta_next_version                                           */
/*                                                                    */
/*  Scans data/meta/<filename>/ for existing version files            */
/*  (v1.meta, v2.meta, …) and returns the next version number.        */
/*  Returns 1 if no versions exist yet.                               */
/* ------------------------------------------------------------------ */
int revfs_meta_next_version(const char *filename)
{
    if (!filename) {
        errno = EINVAL;
        return -1;
    }

    char dir[REVFS_MAX_PATH];
    if (meta_dir_path(filename, dir, sizeof(dir)) < 0)
        return -1;

    /* If the directory doesn't exist, this is version 1 */
    if (!revfs_file_exists(dir))
        return 1;

    DIR *dp = opendir(dir);
    if (!dp) {
        /* Can't open → treat as version 1 */
        return 1;
    }

    int max_version = 0;
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        int v;
        /* Match files named "v<N>.meta" */
        if (sscanf(entry->d_name, "v%d.meta", &v) == 1) {
            if (v > max_version)
                max_version = v;
        }
    }
    closedir(dp);

    return max_version + 1;
}

/* ------------------------------------------------------------------ */
/*  revfs_meta_write                                                  */
/*                                                                    */
/*  Writes a metadata file for a single version to disk.              */
/*  Creates the directory data/meta/<name>/ if needed.                */
/*                                                                    */
/*  Returns 0 on success, -1 on error.                                */
/* ------------------------------------------------------------------ */
int revfs_meta_write(const revfs_meta_t *meta)
{
    if (!meta || meta->version < 1 || meta->num_chunks < 0) {
        errno = EINVAL;
        return -1;
    }

    /* 1. Ensure the metadata directory exists */
    char dir[REVFS_MAX_PATH];
    if (meta_dir_path(meta->name, dir, sizeof(dir)) < 0)
        return -1;
    if (revfs_mkdir_p(dir, 0755) < 0)
        return -1;

    /* 2. Build the metadata file path */
    char path[REVFS_MAX_PATH];
    if (meta_file_path(meta->name, meta->version, path, sizeof(path)) < 0)
        return -1;

    /* 3. Write to a temp file first, then rename (atomic) */
    char tmp_path[REVFS_MAX_PATH + 64];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld_%p", path, (long)getpid(), (void *)pthread_self());

    int fd = revfs_file_open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;

    /* 4. Write the header fields */
    char line[512];
    int len;

    len = snprintf(line, sizeof(line), "name=%s\n", meta->name);
    if (revfs_file_write_all(fd, line, (size_t)len) < 0) goto fail;

    len = snprintf(line, sizeof(line), "version=%d\n", meta->version);
    if (revfs_file_write_all(fd, line, (size_t)len) < 0) goto fail;

    len = snprintf(line, sizeof(line), "chunks=%d\n", meta->num_chunks);
    if (revfs_file_write_all(fd, line, (size_t)len) < 0) goto fail;

    len = snprintf(line, sizeof(line), "size=%lld\n", (long long)meta->file_size);
    if (revfs_file_write_all(fd, line, (size_t)len) < 0) goto fail;

    len = snprintf(line, sizeof(line), "timestamp=%ld\n", meta->timestamp);
    if (revfs_file_write_all(fd, line, (size_t)len) < 0) goto fail;

    /* 5. Write each chunk hash */
    for (int i = 0; i < meta->num_chunks; i++) {
        len = snprintf(line, sizeof(line), "hash.%d=%s\n",
                       i, meta->chunk_hashes[i]);
        if (revfs_file_write_all(fd, line, (size_t)len) < 0) goto fail;
    }

    /* 6. Sync and close */
    if (revfs_file_sync(fd) < 0) goto fail;
    revfs_file_close(fd);

    /* 7. Atomic rename */
    if (rename(tmp_path, path) < 0) {
        fprintf(stderr, "revfs: rename(\"%s\" → \"%s\"): %s\n",
                tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return -1;
    }

    return 0;

fail:
    revfs_file_close(fd);
    unlink(tmp_path);
    return -1;
}

/* ------------------------------------------------------------------ */
/*  revfs_meta_read                                                   */
/*                                                                    */
/*  Reads a metadata file for a given filename and version.           */
/*  If version == -1, reads the latest version.                       */
/*                                                                    */
/*  Returns 0 on success, -1 on error.                                */
/* ------------------------------------------------------------------ */
int revfs_meta_read(const char *filename, int version,
                    revfs_meta_t *meta_out)
{
    if (!filename || !meta_out) {
        errno = EINVAL;
        return -1;
    }

    /* If version is -1, find the latest */
    if (version == -1) {
        int next = revfs_meta_next_version(filename);
        if (next < 0) return -1;
        if (next == 1) {
            /* No versions exist */
            fprintf(stderr, "revfs: no versions found for \"%s\"\n", filename);
            errno = ENOENT;
            return -1;
        }
        version = next - 1;    /* latest = next - 1 */
    }

    /* Build the path */
    char path[REVFS_MAX_PATH];
    if (meta_file_path(filename, version, path, sizeof(path)) < 0)
        return -1;

    /* Open and read the file */
    int fd = revfs_file_open(path, O_RDONLY, 0);
    if (fd < 0)
        return -1;

    off_t sz = revfs_file_size(fd);
    if (sz < 0 || sz > 1024 * 1024) {   /* sanity: meta should be < 1 MB */
        revfs_file_close(fd);
        errno = EFBIG;
        return -1;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        revfs_file_close(fd);
        return -1;
    }

    ssize_t r = revfs_file_read_all(fd, buf, (size_t)sz);
    revfs_file_close(fd);
    if (r < 0) {
        free(buf);
        return -1;
    }
    buf[sz] = '\0';

    /* Zero out the output struct */
    memset(meta_out, 0, sizeof(*meta_out));

    /* Parse line by line */
    char *line = buf;
    while (line && *line) {
        /* Find end of line */
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';

        /* Parse key=value */
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            const char *key = line;
            const char *val = eq + 1;

            if (strcmp(key, "name") == 0) {
                strncpy(meta_out->name, val, REVFS_MAX_FILENAME - 1);
            } else if (strcmp(key, "version") == 0) {
                meta_out->version = atoi(val);
            } else if (strcmp(key, "chunks") == 0) {
                meta_out->num_chunks = atoi(val);
            } else if (strcmp(key, "size") == 0) {
                meta_out->file_size = (off_t)atoll(val);
            } else if (strcmp(key, "timestamp") == 0) {
                meta_out->timestamp = atol(val);
            } else if (strncmp(key, "hash.", 5) == 0) {
                int idx = atoi(key + 5);
                if (idx >= 0 && idx < REVFS_META_MAX_CHUNKS) {
                    strncpy(meta_out->chunk_hashes[idx], val,
                            REVFS_HASH_HEX_SIZE - 1);
                }
            }
        }

        line = eol ? eol + 1 : NULL;
    }

    free(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_meta_list_files                                             */
/*                                                                    */
/*  Lists all files that have been uploaded by scanning               */
/*  data/meta/ for subdirectories.                                    */
/*                                                                    */
/*  Returns the number of files found, -1 on error.                  */
/* ------------------------------------------------------------------ */
int revfs_meta_list_files(char names[][REVFS_MAX_FILENAME], int max_names)
{
    if (!names || max_names <= 0) {
        errno = EINVAL;
        return -1;
    }

    char meta_dir[REVFS_MAX_PATH];
    snprintf(meta_dir, sizeof(meta_dir), "%s/meta", REVFS_DATA_DIR);

    if (!revfs_file_exists(meta_dir))
        return 0;

    DIR *dp = opendir(meta_dir);
    if (!dp)
        return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL && count < max_names) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        /* Only include directories (each dir = one file) */
        char check_path[REVFS_MAX_PATH];
        snprintf(check_path, sizeof(check_path), "%s/%s",
                 meta_dir, entry->d_name);

        struct stat st;
        if (stat(check_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            strncpy(names[count], entry->d_name, REVFS_MAX_FILENAME - 1);
            names[count][REVFS_MAX_FILENAME - 1] = '\0';
            count++;
        }
    }
    closedir(dp);

    return count;
}

/* ------------------------------------------------------------------ */
/*  revfs_upload                                                      */
/*                                                                    */
/*  The main upload entry point.  Given a file path:                  */
/*    1. Validate the file exists                                     */
/*    2. Chunk it (split + hash + store)                              */
/*    3. Determine the next version number                            */
/*    4. Write metadata                                               */
/*                                                                    */
/*  Returns the version number on success, -1 on error.              */
/* ------------------------------------------------------------------ */
int revfs_upload(const char *filepath)
{
    if (!filepath) {
        errno = EINVAL;
        return -1;
    }

    /* 1. Validate the file exists */
    if (!revfs_file_exists(filepath)) {
        fprintf(stderr, "revfs: upload: file not found: \"%s\"\n", filepath);
        errno = ENOENT;
        return -1;
    }

    /* 2. Get the file size */
    off_t file_size = revfs_file_size_path(filepath);
    if (file_size < 0)
        return -1;

    /* 3. Extract the basename */
    char filename[REVFS_MAX_FILENAME];
    if (safe_basename(filepath, filename, sizeof(filename)) < 0) {
        fprintf(stderr, "revfs: upload: invalid filename: \"%s\"\n", filepath);
        return -1;
    }

    /* 4. Chunk the file — split into 4 MB pieces, hash, and store */
    /*    We allocate chunk_hashes on the heap since REVFS_MAX_CHUNKS
     *    is large (1M entries × 65 bytes ≈ 65 MB).  For normal files
     *    this will only use a few entries. */
    int max_chunks_needed = (int)((file_size / REVFS_CHUNK_SIZE) + 2);
    if (max_chunks_needed > REVFS_MAX_CHUNKS)
        max_chunks_needed = REVFS_MAX_CHUNKS;
    /* Minimum 1 for empty files */
    if (max_chunks_needed < 1)
        max_chunks_needed = 1;

    char (*chunk_hashes)[REVFS_HASH_HEX_SIZE] =
        malloc((size_t)max_chunks_needed * REVFS_HASH_HEX_SIZE);
    if (!chunk_hashes) {
        fprintf(stderr, "revfs: upload: malloc for chunk hashes: %s\n",
                strerror(errno));
        return -1;
    }

    int num_chunks = revfs_file_chunk(filepath, chunk_hashes, max_chunks_needed);
    if (num_chunks < 0) {
        fprintf(stderr, "revfs: upload: chunking failed for \"%s\"\n", filepath);
        free(chunk_hashes);
        return -1;
    }

    /* 5. Determine the next version number under metadata lock */
    revfs_lock_meta();
    int version = revfs_meta_next_version(filename);
    if (version < 0) {
        revfs_unlock_meta();
        free(chunk_hashes);
        return -1;
    }

    /* 6. Build and write the metadata */
    revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
    if (!meta) {
        revfs_unlock_meta();
        free(chunk_hashes);
        return -1;
    }

    strncpy(meta->name, filename, REVFS_MAX_FILENAME - 1);
    meta->version    = version;
    meta->num_chunks = num_chunks;
    meta->file_size  = file_size;
    meta->timestamp  = (long)time(NULL);

    /* Copy chunk hashes into the metadata struct */
    for (int i = 0; i < num_chunks; i++) {
        memcpy(meta->chunk_hashes[i], chunk_hashes[i], REVFS_HASH_HEX_SIZE);
    }

    free(chunk_hashes);

    if (revfs_meta_write(meta) < 0) {
        revfs_unlock_meta();
        fprintf(stderr, "revfs: upload: failed to write metadata\n");
        free(meta);
        return -1;
    }
    revfs_unlock_meta();

    /* 7. Print success message */
    printf("Uploaded \"%s\" → version %d (%d chunks, %lld bytes)\n",
           filename, version, num_chunks, (long long)file_size);

    free(meta);
    return version;
}
