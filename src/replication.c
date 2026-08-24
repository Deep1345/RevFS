/*
 * RevFS — Versioned Distributed File Storage System
 *
 * Day 12: Two-Node Replication & High Availability
 *
 * This module implements dual-node replication, failover reads,
 * degraded mode operation, and two-way replica synchronization/repair.
 */

#include "revfs.h"
#include <libgen.h>
#include <sys/stat.h>
#include <time.h>

static int safe_basename(const char *path, char *out, size_t out_size)
{
    if (!path || !out || out_size == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t len = strlen(path);
    if (len == 0) {
        errno = EINVAL;
        return -1;
    }

    /* Strip trailing slashes */
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    if (len == 1 && path[0] == '/') {
        errno = EINVAL;
        return -1;
    }

    /* Find last slash */
    const char *last_slash = NULL;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') {
            last_slash = &path[i];
        }
    }

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

/* ------------------------------------------------------------------ */
/*  revfs_repl_config_init                                            */
/*                                                                    */
/*  Initializes the replication cluster configuration for Primary     */
/*  and Secondary node endpoints.                                     */
/* ------------------------------------------------------------------ */
int revfs_repl_config_init(revfs_repl_config_t *cfg,
                           const char *primary_host, int primary_port,
                           const char *secondary_host, int secondary_port)
{
    if (!cfg) {
        errno = EINVAL;
        return -1;
    }

    memset(cfg, 0, sizeof(revfs_repl_config_t));

    const char *h1 = (primary_host && *primary_host) ? primary_host : "127.0.0.1";
    int p1 = (primary_port > 0) ? primary_port : REVFS_DEFAULT_PORT;
    strncpy(cfg->primary.host, h1, sizeof(cfg->primary.host) - 1);
    cfg->primary.port = p1;

    const char *h2 = (secondary_host && *secondary_host) ? secondary_host : "127.0.0.1";
    int p2 = (secondary_port > 0) ? secondary_port : REVFS_DEFAULT_PORT + 1;
    strncpy(cfg->secondary.host, h2, sizeof(cfg->secondary.host) - 1);
    cfg->secondary.port = p2;

    cfg->write_quorum = 1; /* Allow degraded writes by default */
    cfg->timeout_sec  = 5;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_repl_ping                                                   */
/*                                                                    */
/*  Pings both Primary and Secondary nodes and populates health state. */
/* ------------------------------------------------------------------ */
int revfs_repl_ping(const revfs_repl_config_t *cfg, int *primary_ok, int *secondary_ok)
{
    if (!cfg || !primary_ok || !secondary_ok) {
        errno = EINVAL;
        return -1;
    }

    *primary_ok = 0;
    *secondary_ok = 0;

    /* Ping Primary */
    int sock_p = revfs_client_connect(cfg->primary.host, cfg->primary.port);
    if (sock_p >= 0) {
        char resp[256];
        if (revfs_client_ping(sock_p, "repl_check", resp, sizeof(resp)) == 0) {
            *primary_ok = 1;
        }
        revfs_client_disconnect(sock_p);
    }

    /* Ping Secondary */
    int sock_s = revfs_client_connect(cfg->secondary.host, cfg->secondary.port);
    if (sock_s >= 0) {
        char resp[256];
        if (revfs_client_ping(sock_s, "repl_check", resp, sizeof(resp)) == 0) {
            *secondary_ok = 1;
        }
        revfs_client_disconnect(sock_s);
    }

    return (*primary_ok || *secondary_ok) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  revfs_repl_has_chunk                                              */
/*                                                                    */
/*  Checks if a chunk hash is present on Primary and Secondary nodes.  */
/* ------------------------------------------------------------------ */
int revfs_repl_has_chunk(const revfs_repl_config_t *cfg, const char *hash_hex,
                         int *primary_has, int *secondary_has)
{
    if (!cfg || !hash_hex || !primary_has || !secondary_has) {
        errno = EINVAL;
        return -1;
    }

    *primary_has = 0;
    *secondary_has = 0;

    int sock_p = revfs_client_connect(cfg->primary.host, cfg->primary.port);
    if (sock_p >= 0) {
        int has = revfs_client_has_chunk(sock_p, hash_hex);
        if (has > 0) *primary_has = 1;
        revfs_client_disconnect(sock_p);
    }

    int sock_s = revfs_client_connect(cfg->secondary.host, cfg->secondary.port);
    if (sock_s >= 0) {
        int has = revfs_client_has_chunk(sock_s, hash_hex);
        if (has > 0) *secondary_has = 1;
        revfs_client_disconnect(sock_s);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_repl_store_chunk                                            */
/*                                                                    */
/*  Stores chunk binary data to both Primary and Secondary nodes.     */
/*  Returns count of successful replicas stored (0, 1, or 2).         */
/* ------------------------------------------------------------------ */
int revfs_repl_store_chunk(const revfs_repl_config_t *cfg, const char *hash_hex,
                           const void *data, size_t len)
{
    if (!cfg || !hash_hex || (!data && len > 0)) {
        errno = EINVAL;
        return -1;
    }

    int stored_count = 0;

    /* Store to Primary */
    int sock_p = revfs_client_connect(cfg->primary.host, cfg->primary.port);
    if (sock_p >= 0) {
        if (revfs_client_store_chunk(sock_p, hash_hex, data, len) == 0) {
            stored_count++;
        }
        revfs_client_disconnect(sock_p);
    }

    /* Store to Secondary */
    int sock_s = revfs_client_connect(cfg->secondary.host, cfg->secondary.port);
    if (sock_s >= 0) {
        if (revfs_client_store_chunk(sock_s, hash_hex, data, len) == 0) {
            stored_count++;
        }
        revfs_client_disconnect(sock_s);
    }

    if (stored_count < cfg->write_quorum) {
        return -1;
    }

    return stored_count;
}

/* ------------------------------------------------------------------ */
/*  revfs_repl_get_chunk                                              */
/*                                                                    */
/*  Fetches chunk data from Primary with automatic failover to        */
/*  Secondary if Primary is unreachable, fails, or has corrupted data.*/
/* ------------------------------------------------------------------ */
ssize_t revfs_repl_get_chunk(const revfs_repl_config_t *cfg, const char *hash_hex,
                             void *buf, size_t buf_size)
{
    if (!cfg || !hash_hex || !buf || buf_size == 0) {
        errno = EINVAL;
        return -1;
    }

    /* 1. Try Primary */
    int sock_p = revfs_client_connect(cfg->primary.host, cfg->primary.port);
    if (sock_p >= 0) {
        ssize_t n = revfs_client_get_chunk(sock_p, hash_hex, buf, buf_size);
        revfs_client_disconnect(sock_p);
        if (n >= 0) {
            /* Verify integrity */
            char calc_hash[REVFS_HASH_HEX_SIZE];
            if (revfs_sha256(buf, (size_t)n, calc_hash) == 0 &&
                strcasecmp(calc_hash, hash_hex) == 0) {
                return n;
            }
            /* Checksum failed, fallback */
        }
    }

    /* 2. Fallback to Secondary */
    int sock_s = revfs_client_connect(cfg->secondary.host, cfg->secondary.port);
    if (sock_s >= 0) {
        ssize_t n = revfs_client_get_chunk(sock_s, hash_hex, buf, buf_size);
        revfs_client_disconnect(sock_s);
        if (n >= 0) {
            char calc_hash[REVFS_HASH_HEX_SIZE];
            if (revfs_sha256(buf, (size_t)n, calc_hash) == 0 &&
                strcasecmp(calc_hash, hash_hex) == 0) {
                return n;
            }
        }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/*  revfs_repl_upload                                                 */
/*                                                                    */
/*  Uploads a file replicated across Primary and Secondary nodes.     */
/*  Chunks the file, checks remote chunk existence on both nodes,     */
/*  transfers missing chunks, and commits metadata manifests to both. */
/* ------------------------------------------------------------------ */
int revfs_repl_upload(const revfs_repl_config_t *cfg, const char *filepath)
{
    if (!cfg || !filepath) {
        errno = EINVAL;
        return -1;
    }

    if (!revfs_file_exists(filepath)) {
        fprintf(stderr, "revfs_repl: upload: file not found: \"%s\"\n", filepath);
        errno = ENOENT;
        return -1;
    }

    off_t file_size = revfs_file_size_path(filepath);
    if (file_size < 0) return -1;

    char filename[REVFS_MAX_FILENAME];
    if (safe_basename(filepath, filename, sizeof(filename)) < 0) {
        return -1;
    }

    /* Connect to Primary and Secondary */
    int sock_p = revfs_client_connect(cfg->primary.host, cfg->primary.port);
    int sock_s = revfs_client_connect(cfg->secondary.host, cfg->secondary.port);

    int active_nodes = (sock_p >= 0 ? 1 : 0) + (sock_s >= 0 ? 1 : 0);
    if (active_nodes < cfg->write_quorum || active_nodes == 0) {
        fprintf(stderr, "revfs_repl: upload: insufficient nodes reachable (active=%d, quorum=%d)\n",
                active_nodes, cfg->write_quorum);
        if (sock_p >= 0) revfs_client_disconnect(sock_p);
        if (sock_s >= 0) revfs_client_disconnect(sock_s);
        return -1;
    }

    /* Chunk the source file */
    int max_chunks = (int)((file_size / REVFS_CHUNK_SIZE) + 2);
    if (max_chunks > REVFS_META_MAX_CHUNKS) max_chunks = REVFS_META_MAX_CHUNKS;
    if (max_chunks < 1) max_chunks = 1;

    char (*chunk_hashes)[REVFS_HASH_HEX_SIZE] = malloc((size_t)max_chunks * REVFS_HASH_HEX_SIZE);
    if (!chunk_hashes) {
        if (sock_p >= 0) revfs_client_disconnect(sock_p);
        if (sock_s >= 0) revfs_client_disconnect(sock_s);
        return -1;
    }

    int num_chunks = revfs_file_chunk(filepath, chunk_hashes, max_chunks);
    if (num_chunks < 0) {
        free(chunk_hashes);
        if (sock_p >= 0) revfs_client_disconnect(sock_p);
        if (sock_s >= 0) revfs_client_disconnect(sock_s);
        return -1;
    }

    /* Replicate chunks to both nodes */
    int src_fd = revfs_file_open(filepath, O_RDONLY, 0);
    if (src_fd < 0) {
        free(chunk_hashes);
        if (sock_p >= 0) revfs_client_disconnect(sock_p);
        if (sock_s >= 0) revfs_client_disconnect(sock_s);
        return -1;
    }

    void *chunk_buf = malloc(REVFS_CHUNK_SIZE);
    if (!chunk_buf) {
        revfs_file_close(src_fd);
        free(chunk_hashes);
        if (sock_p >= 0) revfs_client_disconnect(sock_p);
        if (sock_s >= 0) revfs_client_disconnect(sock_s);
        return -1;
    }

    off_t offset = 0;
    int upload_failed = 0;

    for (int i = 0; i < num_chunks; i++) {
        size_t to_read = REVFS_CHUNK_SIZE;
        if (offset + (off_t)to_read > file_size) {
            to_read = (size_t)(file_size - offset);
        }

        ssize_t r = revfs_file_pread(src_fd, chunk_buf, to_read, offset);
        if (r < 0 || (size_t)r != to_read) {
            upload_failed = 1;
            break;
        }

        /* Check Primary */
        if (sock_p >= 0) {
            int has_p = revfs_client_has_chunk(sock_p, chunk_hashes[i]);
            if (has_p == 0) {
                if (revfs_client_store_chunk(sock_p, chunk_hashes[i], chunk_buf, to_read) < 0) {
                    revfs_client_disconnect(sock_p);
                    sock_p = -1;
                }
            }
        }

        /* Check Secondary */
        if (sock_s >= 0) {
            int has_s = revfs_client_has_chunk(sock_s, chunk_hashes[i]);
            if (has_s == 0) {
                if (revfs_client_store_chunk(sock_s, chunk_hashes[i], chunk_buf, to_read) < 0) {
                    revfs_client_disconnect(sock_s);
                    sock_s = -1;
                }
            }
        }

        active_nodes = (sock_p >= 0 ? 1 : 0) + (sock_s >= 0 ? 1 : 0);
        if (active_nodes < cfg->write_quorum || active_nodes == 0) {
            upload_failed = 1;
            break;
        }

        offset += to_read;
    }

    free(chunk_buf);
    revfs_file_close(src_fd);

    if (upload_failed) {
        free(chunk_hashes);
        if (sock_p >= 0) revfs_client_disconnect(sock_p);
        if (sock_s >= 0) revfs_client_disconnect(sock_s);
        return -1;
    }

    /* Build metadata manifest */
    revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
    if (!meta) {
        free(chunk_hashes);
        if (sock_p >= 0) revfs_client_disconnect(sock_p);
        if (sock_s >= 0) revfs_client_disconnect(sock_s);
        return -1;
    }

    strncpy(meta->name, filename, sizeof(meta->name) - 1);
    meta->file_size  = file_size;
    meta->num_chunks = num_chunks;
    meta->timestamp  = (long)time(NULL);
    for (int i = 0; i < num_chunks; i++) {
        strncpy(meta->chunk_hashes[i], chunk_hashes[i], REVFS_HASH_HEX_SIZE - 1);
    }
    free(chunk_hashes);

    /* Commit metadata to Primary and Secondary */
    int ver_p = -1;
    int ver_s = -1;

    if (sock_p >= 0) {
        ver_p = revfs_client_upload_meta(sock_p, meta);
        revfs_client_disconnect(sock_p);
    }
    if (sock_s >= 0) {
        ver_s = revfs_client_upload_meta(sock_s, meta);
        revfs_client_disconnect(sock_s);
    }

    free(meta);

    int commit_count = (ver_p > 0 ? 1 : 0) + (ver_s > 0 ? 1 : 0);
    if (commit_count < cfg->write_quorum || commit_count == 0) {
        fprintf(stderr, "revfs_repl: upload: metadata commit quorum failed (committed=%d, quorum=%d)\n",
                commit_count, cfg->write_quorum);
        return -1;
    }

    int final_ver = (ver_p > 0) ? ver_p : ver_s;
    printf("Replicated \"%s\" → version %d (%d chunks, %lld bytes) [replicas: %d/2]\n",
           filename, final_ver, num_chunks, (long long)file_size, commit_count);

    return final_ver;
}

/* ------------------------------------------------------------------ */
/*  revfs_repl_download                                               */
/*                                                                    */
/*  Downloads a file from the replicated cluster with automatic       */
/*  transparent failover for metadata and individual chunk transfers. */
/* ------------------------------------------------------------------ */
int revfs_repl_download(const revfs_repl_config_t *cfg, const char *filename,
                        int version, const char *output_path)
{
    if (!cfg || !filename || !output_path) {
        errno = EINVAL;
        return -1;
    }

    revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
    if (!meta) return -1;

    int meta_ok = 0;

    /* 1. Try metadata from Primary */
    int sock_p = revfs_client_connect(cfg->primary.host, cfg->primary.port);
    if (sock_p >= 0) {
        if (revfs_client_get_meta(sock_p, filename, version, meta) == 0) {
            meta_ok = 1;
        }
        revfs_client_disconnect(sock_p);
    }

    /* 2. Fallback metadata from Secondary */
    if (!meta_ok) {
        int sock_s = revfs_client_connect(cfg->secondary.host, cfg->secondary.port);
        if (sock_s >= 0) {
            if (revfs_client_get_meta(sock_s, filename, version, meta) == 0) {
                meta_ok = 1;
            }
            revfs_client_disconnect(sock_s);
        }
    }

    if (!meta_ok) {
        fprintf(stderr, "revfs_repl: download: metadata not found on any replica\n");
        free(meta);
        return -1;
    }

    /* 3. Fetch each chunk (local CAS -> Primary -> Secondary) */
    void *chunk_buf = malloc(REVFS_CHUNK_SIZE);
    if (!chunk_buf) {
        free(meta);
        return -1;
    }

    for (int i = 0; i < meta->num_chunks; i++) {
        const char *h = meta->chunk_hashes[i];
        if (revfs_chunk_exists(h)) {
            continue; /* already cached locally */
        }

        ssize_t loaded = revfs_repl_get_chunk(cfg, h, chunk_buf, REVFS_CHUNK_SIZE);
        if (loaded < 0) {
            fprintf(stderr, "revfs_repl: download: failed to fetch chunk %s from replicas\n", h);
            free(chunk_buf);
            free(meta);
            return -1;
        }

        /* Store into local CAS */
        revfs_chunk_store(chunk_buf, (size_t)loaded, NULL);
    }

    free(chunk_buf);

    /* 4. Reassemble output file */
    ssize_t reassembled = revfs_chunks_reassemble(output_path, (const char (*)[REVFS_HASH_HEX_SIZE])meta->chunk_hashes, meta->num_chunks);
    if (reassembled < 0 || reassembled != meta->file_size) {
        fprintf(stderr, "revfs_repl: download: reassembly failed\n");
        unlink(output_path);
        free(meta);
        return -1;
    }

    printf("Downloaded \"%s\" v%d → \"%s\" (%lld bytes, %d chunks) [replicated]\n",
           meta->name, meta->version, output_path,
           (long long)meta->file_size, meta->num_chunks);

    free(meta);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_repl_sync                                                   */
/*                                                                    */
/*  Two-way synchronization and auto-repair between Primary and       */
/*  Secondary replicas. Copies any missing chunks or metadata.        */
/* ------------------------------------------------------------------ */
int revfs_repl_sync(const revfs_repl_config_t *cfg, revfs_repl_sync_report_t *report_out)
{
    if (!cfg) {
        errno = EINVAL;
        return -1;
    }

    revfs_repl_sync_report_t report;
    memset(&report, 0, sizeof(report));

    int sock_p = revfs_client_connect(cfg->primary.host, cfg->primary.port);
    int sock_s = revfs_client_connect(cfg->secondary.host, cfg->secondary.port);

    if (sock_p < 0 || sock_s < 0) {
        fprintf(stderr, "revfs_repl: sync: both nodes must be reachable for sync (Primary: %s, Secondary: %s)\n",
                sock_p >= 0 ? "ONLINE" : "OFFLINE",
                sock_s >= 0 ? "ONLINE" : "OFFLINE");
        if (sock_p >= 0) revfs_client_disconnect(sock_p);
        if (sock_s >= 0) revfs_client_disconnect(sock_s);
        return -1;
    }

    void *chunk_buf = malloc(REVFS_CHUNK_SIZE);
    revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));

    if (!chunk_buf || !meta) {
        free(chunk_buf);
        free(meta);
        revfs_client_disconnect(sock_p);
        revfs_client_disconnect(sock_s);
        return -1;
    }

    /* 1. Sync from Primary to Secondary */
    const char *cmd_list = "LIST\n";
    revfs_file_write_all(sock_p, cmd_list, strlen(cmd_list));

    char line[512];
    /* Read LIST response from Primary */
    char p_files[128][REVFS_MAX_FILENAME];
    int p_file_count = 0;

    /* Simple line reader on sock_p */
    while (p_file_count < 128) {
        size_t idx = 0;
        while (idx < sizeof(line) - 1) {
            char ch;
            if (revfs_file_read(sock_p, &ch, 1) <= 0) break;
            if (ch == '\n') break;
            if (ch != '\r') line[idx++] = ch;
        }
        line[idx] = '\0';
        if (idx == 0 && p_file_count > 0) break;
        if (strncmp(line, "OK ", 3) == 0) continue;
        if (strcasecmp(line, "END") == 0) break;

        char fname[REVFS_MAX_FILENAME];
        int vcount = 0;
        long long sz = 0;
        if (sscanf(line, "%255s %d %lld", fname, &vcount, &sz) >= 2) {
            strncpy(p_files[p_file_count++], fname, REVFS_MAX_FILENAME - 1);
        }
    }

    /* Inspect each file from Primary */
    for (int f = 0; f < p_file_count; f++) {
        /* Query versions on Primary via GET_META */
        int v = 1;
        while (1) {
            if (revfs_client_get_meta(sock_p, p_files[f], v, meta) < 0) {
                break;
            }

            /* Ensure all chunks exist on Secondary */
            for (int c = 0; c < meta->num_chunks; c++) {
                report.total_chunks_checked++;
                const char *h = meta->chunk_hashes[c];
                int has_s = revfs_client_has_chunk(sock_s, h);
                if (has_s == 0) {
                    /* Fetch from Primary and Store to Secondary */
                    ssize_t clen = revfs_client_get_chunk(sock_p, h, chunk_buf, REVFS_CHUNK_SIZE);
                    if (clen >= 0) {
                        if (revfs_client_store_chunk(sock_s, h, chunk_buf, (size_t)clen) == 0) {
                            report.chunks_synced_to_secondary++;
                        } else {
                            report.errors++;
                        }
                    } else {
                        report.errors++;
                    }
                }
            }

            /* Ensure metadata exists on Secondary */
            revfs_meta_t meta_s;
            if (revfs_client_get_meta(sock_s, p_files[f], v, &meta_s) < 0) {
                if (revfs_client_upload_meta(sock_s, meta) > 0) {
                    report.files_synced_to_secondary++;
                } else {
                    report.errors++;
                }
            }

            v++;
        }
    }

    /* 2. Sync from Secondary to Primary */
    revfs_file_write_all(sock_s, cmd_list, strlen(cmd_list));
    char s_files[128][REVFS_MAX_FILENAME];
    int s_file_count = 0;

    while (s_file_count < 128) {
        size_t idx = 0;
        while (idx < sizeof(line) - 1) {
            char ch;
            if (revfs_file_read(sock_s, &ch, 1) <= 0) break;
            if (ch == '\n') break;
            if (ch != '\r') line[idx++] = ch;
        }
        line[idx] = '\0';
        if (idx == 0 && s_file_count > 0) break;
        if (strncmp(line, "OK ", 3) == 0) continue;
        if (strcasecmp(line, "END") == 0) break;

        char fname[REVFS_MAX_FILENAME];
        int vcount = 0;
        long long sz = 0;
        if (sscanf(line, "%255s %d %lld", fname, &vcount, &sz) >= 2) {
            strncpy(s_files[s_file_count++], fname, REVFS_MAX_FILENAME - 1);
        }
    }

    for (int f = 0; f < s_file_count; f++) {
        int v = 1;
        while (1) {
            if (revfs_client_get_meta(sock_s, s_files[f], v, meta) < 0) {
                break;
            }

            for (int c = 0; c < meta->num_chunks; c++) {
                report.total_chunks_checked++;
                const char *h = meta->chunk_hashes[c];
                int has_p = revfs_client_has_chunk(sock_p, h);
                if (has_p == 0) {
                    ssize_t clen = revfs_client_get_chunk(sock_s, h, chunk_buf, REVFS_CHUNK_SIZE);
                    if (clen >= 0) {
                        if (revfs_client_store_chunk(sock_p, h, chunk_buf, (size_t)clen) == 0) {
                            report.chunks_synced_to_primary++;
                        } else {
                            report.errors++;
                        }
                    } else {
                        report.errors++;
                    }
                }
            }

            revfs_meta_t meta_p;
            if (revfs_client_get_meta(sock_p, s_files[f], v, &meta_p) < 0) {
                if (revfs_client_upload_meta(sock_p, meta) > 0) {
                    report.files_synced_to_primary++;
                } else {
                    report.errors++;
                }
            }

            v++;
        }
    }

    free(chunk_buf);
    free(meta);
    revfs_client_disconnect(sock_p);
    revfs_client_disconnect(sock_s);

    if (report_out) {
        *report_out = report;
    }

    printf("\nReplication Sync & Repair Summary\n");
    printf("─────────────────────────────────────────────────────────────\n");
    printf("  Chunks checked:               %d\n", report.total_chunks_checked);
    printf("  Chunks repaired to Primary:   %d\n", report.chunks_synced_to_primary);
    printf("  Chunks repaired to Secondary: %d\n", report.chunks_synced_to_secondary);
    printf("  Manifests synced to Primary:  %d\n", report.files_synced_to_primary);
    printf("  Manifests synced to Secondary:%d\n", report.files_synced_to_secondary);
    printf("  Errors encountered:           %d\n", report.errors);
    printf("─────────────────────────────────────────────────────────────\n\n");

    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_repl_list                                                   */
/*                                                                    */
/*  Lists remote files trying Primary first, fallback to Secondary.   */
/* ------------------------------------------------------------------ */
int revfs_repl_list(const revfs_repl_config_t *cfg)
{
    if (!cfg) return -1;
    int rc = revfs_client_list(cfg->primary.host, cfg->primary.port);
    if (rc >= 0) return rc;
    return revfs_client_list(cfg->secondary.host, cfg->secondary.port);
}

/* ------------------------------------------------------------------ */
/*  revfs_repl_history                                                */
/*                                                                    */
/*  Lists version history trying Primary first, fallback to Secondary.*/
/* ------------------------------------------------------------------ */
int revfs_repl_history(const revfs_repl_config_t *cfg, const char *filename)
{
    if (!cfg || !filename) return -1;
    int rc = revfs_client_history(cfg->primary.host, cfg->primary.port, filename);
    if (rc >= 0) return rc;
    return revfs_client_history(cfg->secondary.host, cfg->secondary.port, filename);
}
