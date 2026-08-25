#ifndef REVFS_H
#define REVFS_H

#define REVFS_VERSION "1.0.0"
#define REVFS_NAME    "RevFS"

/* Default chunk size: 4 MB */
#define REVFS_CHUNK_SIZE (4 * 1024 * 1024)

/* Default server port */
#define REVFS_DEFAULT_PORT 9000

/* Default data directory */
#define REVFS_DATA_DIR "data"

/* Standard C headers */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* POSIX system headers */
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/* POSIX networking headers */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <netdb.h>

/* POSIX threads headers */
#include <pthread.h>

/* Concurrency defaults */
#define REVFS_DEFAULT_THREADS    4
#define REVFS_DEFAULT_QUEUE_SIZE 64
#define REVFS_MAX_THREADS        64
#define REVFS_MAX_QUEUE_SIZE     1024

/* ------------------------------------------------------------------ */
/*  POSIX File Abstraction Layer                                      */
/* ------------------------------------------------------------------ */

int      revfs_file_open(const char *path, int flags, mode_t mode);
int      revfs_file_close(int fd);

ssize_t  revfs_file_read(int fd, void *buf, size_t count);
ssize_t  revfs_file_read_all(int fd, void *buf, size_t count);
ssize_t  revfs_file_write(int fd, const void *buf, size_t count);
ssize_t  revfs_file_write_all(int fd, const void *buf, size_t count);

ssize_t  revfs_file_pread(int fd, void *buf, size_t count, off_t offset);
ssize_t  revfs_file_pwrite(int fd, const void *buf, size_t count, off_t offset);

off_t    revfs_file_seek(int fd, off_t offset, int whence);
off_t    revfs_file_size(int fd);
off_t    revfs_file_size_path(const char *path);
int      revfs_file_exists(const char *path);
int      revfs_file_sync(int fd);

ssize_t  revfs_file_append(const char *path, const void *buf, size_t count);
int      revfs_mkdir_p(const char *path, mode_t mode);

/* ------------------------------------------------------------------ */
/*  Content-Addressed Storage & SHA-256 Chunking                     */
/* ------------------------------------------------------------------ */

#define REVFS_HASH_HEX_SIZE  65
#define REVFS_MAX_CHUNKS     1048576

int      revfs_sha256(const void *data, size_t len, char *hex_out);
int      revfs_sha256_fd(int fd, char *hex_out);

int      revfs_chunk_store_path(const char *hash_hex, char *path_out, size_t path_size);
int      revfs_chunk_store(const void *data, size_t len, char *hash_hex_out);
ssize_t  revfs_chunk_load(const char *hash_hex, void *buf, size_t buf_size);
int      revfs_chunk_exists(const char *hash_hex);

int      revfs_file_chunk(const char *filepath,
                          char chunk_hashes[][REVFS_HASH_HEX_SIZE],
                          int max_chunks);
ssize_t  revfs_chunks_reassemble(const char *output_path,
                                 const char chunk_hashes[][REVFS_HASH_HEX_SIZE],
                                 int num_chunks);

/* ------------------------------------------------------------------ */
/*  Metadata & File Versioning                                        */
/* ------------------------------------------------------------------ */

#define REVFS_MAX_FILENAME   256
#define REVFS_MAX_PATH       1024
#define REVFS_META_MAX_CHUNKS  4096

typedef struct {
    char    name[REVFS_MAX_FILENAME];
    int     version;
    int     num_chunks;
    off_t   file_size;
    long    timestamp;
    char    chunk_hashes[REVFS_META_MAX_CHUNKS][REVFS_HASH_HEX_SIZE];
} revfs_meta_t;

int      revfs_upload(const char *filepath);
int      revfs_download(const char *filename, int version, const char *output_path);
int      revfs_restore(const char *filename, int source_version);

int      revfs_meta_write(const revfs_meta_t *meta);
int      revfs_meta_read(const char *filename, int version, revfs_meta_t *meta_out);
int      revfs_meta_next_version(const char *filename);
int      revfs_meta_list_files(char names[][REVFS_MAX_FILENAME], int max_names);

int      revfs_version_count(const char *filename);
int      revfs_version_list(const char *filename, revfs_meta_t *versions_out, int max_versions);
int      revfs_history(const char *filename);
int      revfs_list_files(void);

/* ------------------------------------------------------------------ */
/*  TCP Server & Wire Protocol                                        */
/* ------------------------------------------------------------------ */

#define REVFS_SERVER_BACKLOG   128
#define REVFS_MAX_CMD_LEN      1024
#define REVFS_MAX_RESP_LEN     65536

int      revfs_server_create(int port, int *actual_port);
int      revfs_server_handle_client(int client_fd);
int      revfs_server_process_command(const char *cmd_line, int client_fd);
int      revfs_server_start(int port);
void     revfs_server_stop(void);

/* ------------------------------------------------------------------ */
/*  TCP Client Operations                                             */
/* ------------------------------------------------------------------ */

int      revfs_client_connect(const char *host, int port);
int      revfs_client_disconnect(int sock);
int      revfs_client_ping(int sock, const char *msg, char *resp_out, size_t resp_size);
int      revfs_client_has_chunk(int sock, const char *hash_hex);
int      revfs_client_store_chunk(int sock, const char *hash_hex, const void *data, size_t len);
ssize_t  revfs_client_get_chunk(int sock, const char *hash_hex, void *buf, size_t buf_size);
int      revfs_client_get_meta(int sock, const char *filename, int version, revfs_meta_t *meta_out);
int      revfs_client_upload_meta(int sock, const revfs_meta_t *meta);

int      revfs_client_upload(const char *host, int port, const char *filepath);
int      revfs_client_download(const char *host, int port, const char *filename, int version, const char *output_path);
int      revfs_client_list(const char *host, int port);
int      revfs_client_history(const char *host, int port, const char *filename);

/* ------------------------------------------------------------------ */
/*  Thread Pool & Concurrency                                         */
/* ------------------------------------------------------------------ */

typedef struct revfs_task {
    void (*function)(void *arg);
    void  *arg;
} revfs_task_t;

typedef struct revfs_tpool {
    pthread_t       *threads;
    int              num_threads;
    revfs_task_t    *queue;
    int              queue_size;
    int              queue_head;
    int              queue_tail;
    int              queue_count;
    int              active_tasks;
    int              shutdown;
    pthread_mutex_t  lock;
    pthread_cond_t   notify_not_empty;
    pthread_cond_t   notify_not_full;
    pthread_cond_t   notify_idle;
} revfs_tpool_t;

revfs_tpool_t *revfs_tpool_create(int num_threads, int queue_size);
int            revfs_tpool_submit(revfs_tpool_t *pool, void (*function)(void *), void *arg);
int            revfs_tpool_try_submit(revfs_tpool_t *pool, void (*function)(void *), void *arg);
int            revfs_tpool_wait(revfs_tpool_t *pool);
int            revfs_tpool_destroy(revfs_tpool_t *pool, int wait_for_tasks);
int            revfs_tpool_active_workers(revfs_tpool_t *pool);
int            revfs_tpool_queue_count(revfs_tpool_t *pool);

void           revfs_lock_meta(void);
void           revfs_unlock_meta(void);
int            revfs_server_start_threaded(int port, int num_threads);

/* ------------------------------------------------------------------ */
/*  Storage & Deduplication Statistics                                */
/* ------------------------------------------------------------------ */

typedef struct revfs_stats {
    int     total_files;
    int     total_versions;
    off_t   logical_bytes;
    off_t   physical_bytes;
    int     unique_chunks;
    int     referenced_chunks;
    double  dedup_ratio;
    off_t   savings_bytes;
    double  savings_percent;
} revfs_stats_t;

int      revfs_stats_chunks_info(int *unique_chunks_out, off_t *physical_bytes_out);
int      revfs_stats_calculate(revfs_stats_t *stats_out);
int      revfs_stats_print(const revfs_stats_t *stats);
int      revfs_stats(void);

int      revfs_client_get_stats(int sock, revfs_stats_t *stats_out);
int      revfs_client_stats(const char *host, int port);

/* ------------------------------------------------------------------ */
/*  Two-Node Replication & Failover                                   */
/* ------------------------------------------------------------------ */

typedef struct revfs_node {
    char host[128];
    int  port;
    int  is_alive;
} revfs_node_t;

typedef struct revfs_repl_config {
    revfs_node_t primary;
    revfs_node_t secondary;
    int          write_quorum;
    int          timeout_sec;
} revfs_repl_config_t;

typedef struct revfs_repl_sync_report {
    int chunks_synced_to_primary;
    int chunks_synced_to_secondary;
    int files_synced_to_primary;
    int files_synced_to_secondary;
    int total_chunks_checked;
    int errors;
} revfs_repl_sync_report_t;

int      revfs_repl_config_init(revfs_repl_config_t *cfg,
                                const char *primary_host, int primary_port,
                                const char *secondary_host, int secondary_port);
int      revfs_repl_ping(const revfs_repl_config_t *cfg, int *primary_ok, int *secondary_ok);
int      revfs_repl_upload(const revfs_repl_config_t *cfg, const char *filepath);
int      revfs_repl_download(const revfs_repl_config_t *cfg, const char *filename,
                             int version, const char *output_path);
int      revfs_repl_store_chunk(const revfs_repl_config_t *cfg, const char *hash_hex,
                                const void *data, size_t len);
ssize_t  revfs_repl_get_chunk(const revfs_repl_config_t *cfg, const char *hash_hex,
                              void *buf, size_t buf_size);
int      revfs_repl_has_chunk(const revfs_repl_config_t *cfg, const char *hash_hex,
                              int *primary_has, int *secondary_has);
int      revfs_repl_sync(const revfs_repl_config_t *cfg, revfs_repl_sync_report_t *report_out);
int      revfs_repl_list(const revfs_repl_config_t *cfg);
int      revfs_repl_history(const revfs_repl_config_t *cfg, const char *filename);

/* ------------------------------------------------------------------ */
/*  Write-Ahead Journaling & Crash Recovery                          */
/* ------------------------------------------------------------------ */

#define REVFS_JOURNAL_MAX_WRITES  64

typedef struct revfs_journal {
    int              fd;
    long             next_txn_id;
    long             current_txn_id;
    int              active_txn;
    int              num_writes;
    char             write_paths[REVFS_JOURNAL_MAX_WRITES][REVFS_MAX_PATH];
    pthread_mutex_t  lock;
} revfs_journal_t;

revfs_journal_t *revfs_journal_open(void);
int              revfs_journal_close(revfs_journal_t *j);
long             revfs_journal_begin(revfs_journal_t *j);
int              revfs_journal_write(revfs_journal_t *j, const char *target_path, size_t size);
int              revfs_journal_commit(revfs_journal_t *j);
int              revfs_journal_abort(revfs_journal_t *j);
int              revfs_journal_recover(revfs_journal_t *j);
int              revfs_journal_status(const revfs_journal_t *j,
                                      int *active_txn_out,
                                      long *current_txn_id_out,
                                      long *next_txn_id_out);

#endif /* REVFS_H */
