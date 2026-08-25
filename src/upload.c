/*
 * upload.c — Upload pipeline + metadata persistence
 *
 * Handles the full upload flow: validate → chunk → store → version → write metadata.
 * Also provides metadata read/write/list functions used across the project.
 *
 * Metadata format (flat key=value, no JSON dependency):
 *   name=myfile.txt
 *   version=1
 *   chunks=3
 *   size=12582912
 *   timestamp=1723886400
 *   hash.0=abcdef0123...
 *   hash.1=fedcba9876...
 *
 * Stored at: data/meta/<filename>/v<N>.meta
 */

#include "revfs.h"
#include <time.h>
#include <dirent.h>
#include <libgen.h>

/* Thread-safe basename extraction (libc basename uses a static buffer). */
static int safe_basename(const char *path, char *out, size_t out_size)
{
    if (!path || !out || out_size == 0) { errno = EINVAL; return -1; }

    size_t len = strlen(path);
    if (len == 0) { errno = EINVAL; return -1; }

    /* Strip trailing slashes */
    while (len > 1 && path[len - 1] == '/') len--;
    if (len == 1 && path[0] == '/') { errno = EINVAL; return -1; }

    /* Find the last slash */
    const char *last_slash = NULL;
    for (size_t i = 0; i < len; i++)
        if (path[i] == '/') last_slash = &path[i];

    const char *start = last_slash ? (last_slash + 1) : path;
    size_t base_len = (size_t)(&path[len] - start);

    if (base_len == 0 || base_len >= out_size) {
        errno = (base_len >= out_size) ? ENAMETOOLONG : EINVAL;
        return -1;
    }

    memcpy(out, start, base_len);
    out[base_len] = '\0';
    return 0;
}

static int meta_dir_path(const char *filename, char *out, size_t out_size)
{
    int n = snprintf(out, out_size, "%s/meta/%s", REVFS_DATA_DIR, filename);
    if (n < 0 || (size_t)n >= out_size) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int meta_file_path(const char *filename, int version, char *out, size_t out_size)
{
    int n = snprintf(out, out_size, "%s/meta/%s/v%d.meta",
                     REVFS_DATA_DIR, filename, version);
    if (n < 0 || (size_t)n >= out_size) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

/*
 * Scan data/meta/<filename>/ for existing version files and return
 * the next version number. Returns 1 if no versions exist yet.
 */
int revfs_meta_next_version(const char *filename)
{
    if (!filename) { errno = EINVAL; return -1; }

    char dir[REVFS_MAX_PATH];
    if (meta_dir_path(filename, dir, sizeof(dir)) < 0) return -1;
    if (!revfs_file_exists(dir)) return 1;

    DIR *dp = opendir(dir);
    if (!dp) return 1;

    int max_version = 0;
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        int v;
        if (sscanf(entry->d_name, "v%d.meta", &v) == 1 && v > max_version)
            max_version = v;
    }
    closedir(dp);

    return max_version + 1;
}

/* Write metadata atomically (temp + fsync + rename). */
int revfs_meta_write(const revfs_meta_t *meta)
{
    if (!meta || meta->version < 1 || meta->num_chunks < 0) {
        errno = EINVAL;
        return -1;
    }

    char dir[REVFS_MAX_PATH];
    if (meta_dir_path(meta->name, dir, sizeof(dir)) < 0) return -1;
    if (revfs_mkdir_p(dir, 0755) < 0) return -1;

    char path[REVFS_MAX_PATH];
    if (meta_file_path(meta->name, meta->version, path, sizeof(path)) < 0) return -1;

    /* Write to temp, then rename for crash safety */
    char tmp_path[REVFS_MAX_PATH + 64];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld_%p",
             path, (long)getpid(), (void *)pthread_self());

    int fd = revfs_file_open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

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

    for (int i = 0; i < meta->num_chunks; i++) {
        len = snprintf(line, sizeof(line), "hash.%d=%s\n", i, meta->chunk_hashes[i]);
        if (revfs_file_write_all(fd, line, (size_t)len) < 0) goto fail;
    }

    if (revfs_file_sync(fd) < 0) goto fail;
    revfs_file_close(fd);

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

/* Read metadata for a filename + version. Pass version=-1 for latest. */
int revfs_meta_read(const char *filename, int version, revfs_meta_t *meta_out)
{
    if (!filename || !meta_out) { errno = EINVAL; return -1; }

    if (version == -1) {
        int next = revfs_meta_next_version(filename);
        if (next < 0) return -1;
        if (next == 1) {
            fprintf(stderr, "revfs: no versions found for \"%s\"\n", filename);
            errno = ENOENT;
            return -1;
        }
        version = next - 1;
    }

    char path[REVFS_MAX_PATH];
    if (meta_file_path(filename, version, path, sizeof(path)) < 0) return -1;

    int fd = revfs_file_open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    off_t sz = revfs_file_size(fd);
    if (sz < 0 || sz > 1024 * 1024) { revfs_file_close(fd); errno = EFBIG; return -1; }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { revfs_file_close(fd); return -1; }

    ssize_t r = revfs_file_read_all(fd, buf, (size_t)sz);
    revfs_file_close(fd);
    if (r < 0) { free(buf); return -1; }
    buf[sz] = '\0';

    memset(meta_out, 0, sizeof(*meta_out));

    /* Parse key=value lines */
    char *line = buf;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';

        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            const char *key = line;
            const char *val = eq + 1;

            if (strcmp(key, "name") == 0)
                strncpy(meta_out->name, val, REVFS_MAX_FILENAME - 1);
            else if (strcmp(key, "version") == 0)
                meta_out->version = atoi(val);
            else if (strcmp(key, "chunks") == 0)
                meta_out->num_chunks = atoi(val);
            else if (strcmp(key, "size") == 0)
                meta_out->file_size = (off_t)atoll(val);
            else if (strcmp(key, "timestamp") == 0)
                meta_out->timestamp = atol(val);
            else if (strncmp(key, "hash.", 5) == 0) {
                int idx = atoi(key + 5);
                if (idx >= 0 && idx < REVFS_META_MAX_CHUNKS)
                    strncpy(meta_out->chunk_hashes[idx], val, REVFS_HASH_HEX_SIZE - 1);
            }
        }

        line = eol ? eol + 1 : NULL;
    }

    free(buf);
    return 0;
}

/* List all uploaded files by scanning data/meta/ subdirectories. */
int revfs_meta_list_files(char names[][REVFS_MAX_FILENAME], int max_names)
{
    if (!names || max_names <= 0) { errno = EINVAL; return -1; }

    char meta_dir[REVFS_MAX_PATH];
    snprintf(meta_dir, sizeof(meta_dir), "%s/meta", REVFS_DATA_DIR);
    if (!revfs_file_exists(meta_dir)) return 0;

    DIR *dp = opendir(meta_dir);
    if (!dp) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL && count < max_names) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char check_path[REVFS_MAX_PATH];
        snprintf(check_path, sizeof(check_path), "%s/%s", meta_dir, entry->d_name);

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

/*
 * Main upload entry point: validate → chunk → determine version → write metadata.
 * Returns the version number on success, -1 on error.
 */
int revfs_upload(const char *filepath)
{
    if (!filepath) { errno = EINVAL; return -1; }

    if (!revfs_file_exists(filepath)) {
        fprintf(stderr, "revfs: upload: file not found: \"%s\"\n", filepath);
        errno = ENOENT;
        return -1;
    }

    off_t file_size = revfs_file_size_path(filepath);
    if (file_size < 0) return -1;

    char filename[REVFS_MAX_FILENAME];
    if (safe_basename(filepath, filename, sizeof(filename)) < 0) {
        fprintf(stderr, "revfs: upload: invalid filename: \"%s\"\n", filepath);
        return -1;
    }

    /* Allocate chunk hash array on the heap (can be large for big files) */
    int max_chunks_needed = (int)((file_size / REVFS_CHUNK_SIZE) + 2);
    if (max_chunks_needed > REVFS_MAX_CHUNKS) max_chunks_needed = REVFS_MAX_CHUNKS;
    if (max_chunks_needed < 1) max_chunks_needed = 1;

    char (*chunk_hashes)[REVFS_HASH_HEX_SIZE] =
        malloc((size_t)max_chunks_needed * REVFS_HASH_HEX_SIZE);
    if (!chunk_hashes) {
        fprintf(stderr, "revfs: upload: malloc for chunk hashes: %s\n", strerror(errno));
        return -1;
    }

    int num_chunks = revfs_file_chunk(filepath, chunk_hashes, max_chunks_needed);
    if (num_chunks < 0) {
        fprintf(stderr, "revfs: upload: chunking failed for \"%s\"\n", filepath);
        free(chunk_hashes);
        return -1;
    }

    /* Lock metadata to get an atomic version number */
    revfs_lock_meta();
    int version = revfs_meta_next_version(filename);
    if (version < 0) { revfs_unlock_meta(); free(chunk_hashes); return -1; }

    revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
    if (!meta) { revfs_unlock_meta(); free(chunk_hashes); return -1; }

    strncpy(meta->name, filename, REVFS_MAX_FILENAME - 1);
    meta->version    = version;
    meta->num_chunks = num_chunks;
    meta->file_size  = file_size;
    meta->timestamp  = (long)time(NULL);

    for (int i = 0; i < num_chunks; i++)
        memcpy(meta->chunk_hashes[i], chunk_hashes[i], REVFS_HASH_HEX_SIZE);
    free(chunk_hashes);

    if (revfs_meta_write(meta) < 0) {
        revfs_unlock_meta();
        fprintf(stderr, "revfs: upload: failed to write metadata\n");
        free(meta);
        return -1;
    }
    revfs_unlock_meta();

    printf("Uploaded \"%s\" → version %d (%d chunks, %lld bytes)\n",
           filename, version, num_chunks, (long long)file_size);

    free(meta);
    return version;
}
