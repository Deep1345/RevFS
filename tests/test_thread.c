/*
 * RevFS — Day 10 Tests
 *
 * Exercises the POSIX Thread Pool subsystem, task queueing,
 * worker thread distribution, concurrency race safety, barrier
 * synchronization, and concurrent client interactions with the
 * multi-threaded RevFS TCP server.
 *
 * Compile separately:
 *   clang -Wall -Wextra -std=c11 -g -Iinclude -pthread -o test_thread \
 *         tests/test_thread.c src/thread.c src/server.c src/client.c \
 *         src/restore.c src/version.c src/upload.c src/download.c \
 *         src/chunk.c src/file.c
 *
 * Run:
 *   ./test_thread
 */

#include "revfs.h"
#include <assert.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#define TEST_DIR "data/test_thread_tmp"

static int tests_run    = 0;
static int tests_passed = 0;

#define RUN(name) do {                                    \
    fflush(stdout);                                       \
    tests_run++;                                          \
    printf("  [%d] %-48s ", tests_run, #name);            \
    fflush(stdout);                                       \
    if (name()) { tests_passed++; printf("✅ PASS\n"); }  \
    else        { printf("❌ FAIL\n"); }                   \
    fflush(stdout);                                       \
} while (0)

/* ------- Helper: Recursive directory cleanup ------- */
static void recursive_rm(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) return;

    if (S_ISDIR(st.st_mode)) {
        DIR *dp = opendir(path);
        if (!dp) return;
        struct dirent *entry;
        while ((entry = readdir(dp)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;
            char child[1024];
            snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
            recursive_rm(child);
        }
        closedir(dp);
        rmdir(path);
    } else {
        unlink(path);
    }
}

static void cleanup(void)
{
    recursive_rm(TEST_DIR);
    for (int i = 0; i < 20; i++) {
        char meta_path[256];
        snprintf(meta_path, sizeof(meta_path), "data/meta/thread_file_%d.txt", i);
        recursive_rm(meta_path);
    }
}

/* ------- Helper: Create test file ------- */
static int create_test_file(const char *path, const void *content, size_t len)
{
    revfs_mkdir_p(TEST_DIR, 0755);
    int fd = revfs_file_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (content && len > 0)
        revfs_file_write_all(fd, content, len);
    revfs_file_close(fd);
    return 0;
}

/* ------- Helper: Spawn threaded server in child process ------- */
static pid_t spawn_threaded_server(int *out_port, int num_threads)
{
    int port = 0;
    int temp_fd = revfs_server_create(0, &port);
    if (temp_fd < 0) return -1;
    revfs_file_close(temp_fd);

    *out_port = port;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* Silence server stdout for test harness */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        revfs_server_start_threaded(port, num_threads);
        _exit(0);
    }

    /* Poll connect until server is accepting connections */
    for (int retry = 0; retry < 50; retry++) {
        usleep(10000); /* 10ms */
        int test_sock = revfs_client_connect("127.0.0.1", port);
        if (test_sock >= 0) {
            revfs_client_disconnect(test_sock);
            break;
        }
    }

    return pid;
}

static void stop_server(pid_t pid)
{
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }
}

/* ======================================================= */
/*  Test 1: Thread pool creation and clean destruction     */
/* ======================================================= */
static int test_tpool_create_destroy_clean(void)
{
    revfs_tpool_t *pool = revfs_tpool_create(4, 16);
    if (!pool) return 0;

    if (pool->num_threads != 4) { revfs_tpool_destroy(pool, 1); return 0; }
    if (pool->queue_size != 16) { revfs_tpool_destroy(pool, 1); return 0; }
    if (revfs_tpool_active_workers(pool) != 0) { revfs_tpool_destroy(pool, 1); return 0; }
    if (revfs_tpool_queue_count(pool) != 0) { revfs_tpool_destroy(pool, 1); return 0; }

    if (revfs_tpool_destroy(pool, 1) != 0) return 0;

    /* Test default parameters */
    revfs_tpool_t *pool_def = revfs_tpool_create(0, 0);
    if (!pool_def) return 0;
    if (pool_def->num_threads != REVFS_DEFAULT_THREADS) { revfs_tpool_destroy(pool_def, 1); return 0; }
    if (pool_def->queue_size != REVFS_DEFAULT_QUEUE_SIZE) { revfs_tpool_destroy(pool_def, 1); return 0; }
    if (revfs_tpool_destroy(pool_def, 1) != 0) return 0;

    return 1;
}

/* ======================================================= */
/*  Test 2: Single task execution                          */
/* ======================================================= */
typedef struct {
    int value;
    int executed;
} single_task_ctx_t;

static void single_task_fn(void *arg)
{
    single_task_ctx_t *ctx = (single_task_ctx_t *)arg;
    ctx->value *= 2;
    ctx->executed = 1;
}

static int test_tpool_single_task_execution(void)
{
    revfs_tpool_t *pool = revfs_tpool_create(2, 8);
    if (!pool) return 0;

    single_task_ctx_t ctx = { .value = 21, .executed = 0 };
    if (revfs_tpool_submit(pool, single_task_fn, &ctx) != 0) {
        revfs_tpool_destroy(pool, 1);
        return 0;
    }

    if (revfs_tpool_wait(pool) != 0) {
        revfs_tpool_destroy(pool, 1);
        return 0;
    }

    if (ctx.executed != 1 || ctx.value != 42) {
        revfs_tpool_destroy(pool, 1);
        return 0;
    }

    revfs_tpool_destroy(pool, 1);
    return 1;
}

/* ======================================================= */
/*  Test 3: Concurrent shared counter increments           */
/* ======================================================= */
typedef struct {
    int             *counter;
    pthread_mutex_t *lock;
} counter_task_ctx_t;

static void counter_task_fn(void *arg)
{
    counter_task_ctx_t *ctx = (counter_task_ctx_t *)arg;
    pthread_mutex_lock(ctx->lock);
    (*ctx->counter)++;
    pthread_mutex_unlock(ctx->lock);
}

static int test_tpool_concurrent_counter_increments(void)
{
    const int NUM_TASKS = 2000;
    revfs_tpool_t *pool = revfs_tpool_create(8, 128);
    if (!pool) return 0;

    int counter = 0;
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    counter_task_ctx_t ctx = { .counter = &counter, .lock = &lock };

    for (int i = 0; i < NUM_TASKS; i++) {
        if (revfs_tpool_submit(pool, counter_task_fn, &ctx) != 0) {
            revfs_tpool_destroy(pool, 1);
            pthread_mutex_destroy(&lock);
            return 0;
        }
    }

    revfs_tpool_wait(pool);
    revfs_tpool_destroy(pool, 1);
    pthread_mutex_destroy(&lock);

    return (counter == NUM_TASKS);
}

/* ======================================================= */
/*  Test 4: Batch tasks data calculation                   */
/* ======================================================= */
typedef struct {
    int index;
    int *array;
} batch_item_t;

static void batch_task_fn(void *arg)
{
    batch_item_t *item = (batch_item_t *)arg;
    item->array[item->index] = item->index * item->index;
}

static int test_tpool_multiple_tasks_batch(void)
{
    const int COUNT = 300;
    revfs_tpool_t *pool = revfs_tpool_create(4, 64);
    if (!pool) return 0;

    int *results = calloc((size_t)COUNT, sizeof(int));
    batch_item_t *items = calloc((size_t)COUNT, sizeof(batch_item_t));
    if (!results || !items) {
        free(results);
        free(items);
        revfs_tpool_destroy(pool, 1);
        return 0;
    }

    for (int i = 0; i < COUNT; i++) {
        items[i].index = i;
        items[i].array = results;
        if (revfs_tpool_submit(pool, batch_task_fn, &items[i]) != 0) {
            free(results);
            free(items);
            revfs_tpool_destroy(pool, 1);
            return 0;
        }
    }

    revfs_tpool_wait(pool);

    int ok = 1;
    for (int i = 0; i < COUNT; i++) {
        if (results[i] != i * i) {
            ok = 0;
            break;
        }
    }

    free(results);
    free(items);
    revfs_tpool_destroy(pool, 1);
    return ok;
}

/* ======================================================= */
/*  Test 5: Barrier wait synchronization                   */
/* ======================================================= */
static void sleep_task_fn(void *arg)
{
    int *flag = (int *)arg;
    usleep(5000); /* 5ms */
    *flag = 1;
}

static int test_tpool_wait_barrier(void)
{
    revfs_tpool_t *pool = revfs_tpool_create(4, 16);
    if (!pool) return 0;

    int flags[8] = {0};
    for (int i = 0; i < 8; i++) {
        revfs_tpool_submit(pool, sleep_task_fn, &flags[i]);
    }

    /* revfs_tpool_wait must block until all 8 complete */
    revfs_tpool_wait(pool);

    int ok = 1;
    for (int i = 0; i < 8; i++) {
        if (flags[i] != 1) ok = 0;
    }

    if (revfs_tpool_active_workers(pool) != 0 || revfs_tpool_queue_count(pool) != 0) {
        ok = 0;
    }

    revfs_tpool_destroy(pool, 1);
    return ok;
}

/* ======================================================= */
/*  Test 6: Try submit and queue capacity                  */
/* ======================================================= */
typedef struct {
    pthread_mutex_t block_mutex;
} blocker_ctx_t;

static void blocker_fn(void *arg)
{
    blocker_ctx_t *ctx = (blocker_ctx_t *)arg;
    pthread_mutex_lock(&ctx->block_mutex);
    pthread_mutex_unlock(&ctx->block_mutex);
}

static int test_tpool_try_submit_and_capacity(void)
{
    /* 1 worker, capacity 2 */
    revfs_tpool_t *pool = revfs_tpool_create(1, 2);
    if (!pool) return 0;

    blocker_ctx_t ctx;
    pthread_mutex_init(&ctx.block_mutex, NULL);
    pthread_mutex_lock(&ctx.block_mutex);

    /* 1 task running, 2 tasks queued in ring buffer */
    int r1 = revfs_tpool_try_submit(pool, blocker_fn, &ctx);
    usleep(5000); /* Let worker pick up task 1 */
    int r2 = revfs_tpool_try_submit(pool, blocker_fn, &ctx);
    int r3 = revfs_tpool_try_submit(pool, blocker_fn, &ctx);
    /* 4th submission must fail with EAGAIN */
    int r4 = revfs_tpool_try_submit(pool, blocker_fn, &ctx);

    /* Unblock tasks */
    pthread_mutex_unlock(&ctx.block_mutex);
    revfs_tpool_wait(pool);
    revfs_tpool_destroy(pool, 1);
    pthread_mutex_destroy(&ctx.block_mutex);

    return (r1 == 0 && r2 == 0 && r3 == 0 && r4 == -1 && errno == EAGAIN);
}

/* ======================================================= */
/*  Test 7: Graceful vs immediate shutdown                 */
/* ======================================================= */
static void inc_task_fn(void *arg)
{
    int *val = (int *)arg;
    (*val)++;
}

static int test_tpool_graceful_vs_immediate_shutdown(void)
{
    /* Test graceful drain: wait_for_tasks = 1 */
    revfs_tpool_t *pool1 = revfs_tpool_create(2, 32);
    int val1 = 0;
    for (int i = 0; i < 20; i++) {
        revfs_tpool_submit(pool1, inc_task_fn, &val1);
    }
    revfs_tpool_destroy(pool1, 1);
    if (val1 != 20) return 0;

    /* Test immediate shutdown: wait_for_tasks = 0 */
    revfs_tpool_t *pool2 = revfs_tpool_create(2, 32);
    int val2 = 0;
    for (int i = 0; i < 5; i++) {
        revfs_tpool_submit(pool2, inc_task_fn, &val2);
    }
    revfs_tpool_destroy(pool2, 0);

    return 1;
}

/* ======================================================= */
/*  Test 8: Concurrent client PINGs to threaded server     */
/* ======================================================= */
typedef struct {
    int  port;
    int  client_id;
    int  success;
} ping_client_ctx_t;

static void *ping_client_thread(void *arg)
{
    ping_client_ctx_t *ctx = (ping_client_ctx_t *)arg;
    ctx->success = 0;

    int sock = revfs_client_connect("127.0.0.1", ctx->port);
    if (sock < 0) return NULL;

    char msg[64];
    snprintf(msg, sizeof(msg), "client_%d", ctx->client_id);

    char resp[256];
    if (revfs_client_ping(sock, msg, resp, sizeof(resp)) == 0) {
        char expected[256];
        snprintf(expected, sizeof(expected), "PONG %s", msg);
        if (strcmp(resp, expected) == 0) {
            ctx->success = 1;
        }
    }

    revfs_client_disconnect(sock);
    return NULL;
}

static int test_server_concurrent_pings(void)
{
    int port = 0;
    pid_t srv_pid = spawn_threaded_server(&port, 4);
    if (srv_pid <= 0) return 0;

    const int CLIENT_COUNT = 16;
    pthread_t client_threads[16];
    ping_client_ctx_t client_ctxs[16];

    for (int i = 0; i < CLIENT_COUNT; i++) {
        client_ctxs[i].port      = port;
        client_ctxs[i].client_id = i;
        client_ctxs[i].success   = 0;
        pthread_create(&client_threads[i], NULL, ping_client_thread, &client_ctxs[i]);
    }

    int all_ok = 1;
    for (int i = 0; i < CLIENT_COUNT; i++) {
        pthread_join(client_threads[i], NULL);
        if (!client_ctxs[i].success) {
            all_ok = 0;
        }
    }

    stop_server(srv_pid);
    return all_ok;
}

/* ======================================================= */
/*  Test 9: Concurrent client uploads and downloads over TCP*/
/* ======================================================= */
typedef struct {
    int  port;
    int  worker_id;
    int  success;
} upload_download_worker_ctx_t;

static void *upload_download_worker_thread(void *arg)
{
    upload_download_worker_ctx_t *ctx = (upload_download_worker_ctx_t *)arg;
    ctx->success = 0;

    char src_file[512];
    char out_file[512];
    char filename[128];
    snprintf(filename, sizeof(filename), "thread_file_%d.txt", ctx->worker_id);
    snprintf(src_file, sizeof(src_file), "%s/%s", TEST_DIR, filename);
    snprintf(out_file, sizeof(out_file), "%s/out_%s", TEST_DIR, filename);

    char content[256];
    snprintf(content, sizeof(content),
             "Unique payload content from concurrent worker thread #%d [timestamp=%ld]\n",
             ctx->worker_id, (long)time(NULL));

    if (create_test_file(src_file, content, strlen(content)) < 0) {
        return NULL;
    }

    /* 1. Upload file to remote server */
    int ver = revfs_client_upload("127.0.0.1", ctx->port, src_file);
    if (ver < 1) return NULL;

    /* 2. Download file back from remote server */
    if (revfs_client_download("127.0.0.1", ctx->port, filename, ver, out_file) < 0) {
        return NULL;
    }

    /* 3. Verify content matches */
    int fd = revfs_file_open(out_file, O_RDONLY, 0);
    if (fd < 0) return NULL;

    char buf[512];
    ssize_t n = revfs_file_read_all(fd, buf, strlen(content));
    revfs_file_close(fd);

    if (n == (ssize_t)strlen(content) && memcmp(buf, content, strlen(content)) == 0) {
        ctx->success = 1;
    }

    return NULL;
}

static int test_server_concurrent_uploads_and_downloads(void)
{
    cleanup();
    revfs_mkdir_p(TEST_DIR, 0755);

    int port = 0;
    pid_t srv_pid = spawn_threaded_server(&port, 4);
    if (srv_pid <= 0) return 0;

    const int NUM_CLIENTS = 8;
    pthread_t threads[8];
    upload_download_worker_ctx_t ctxs[8];

    for (int i = 0; i < NUM_CLIENTS; i++) {
        ctxs[i].port      = port;
        ctxs[i].worker_id = i;
        ctxs[i].success   = 0;
        pthread_create(&threads[i], NULL, upload_download_worker_thread, &ctxs[i]);
    }

    int all_ok = 1;
    for (int i = 0; i < NUM_CLIENTS; i++) {
        pthread_join(threads[i], NULL);
        if (!ctxs[i].success) {
            all_ok = 0;
        }
    }

    stop_server(srv_pid);
    cleanup();
    return all_ok;
}

/* ======================================================= */
/*  Test 10: Invalid arguments and error handling          */
/* ======================================================= */
static int test_tpool_invalid_args_and_edge_cases(void)
{
    if (revfs_tpool_submit(NULL, single_task_fn, NULL) != -1) return 0;
    if (revfs_tpool_try_submit(NULL, single_task_fn, NULL) != -1) return 0;
    if (revfs_tpool_wait(NULL) != -1) return 0;
    if (revfs_tpool_destroy(NULL, 1) != -1) return 0;
    if (revfs_tpool_active_workers(NULL) != -1) return 0;
    if (revfs_tpool_queue_count(NULL) != -1) return 0;

    revfs_tpool_t *pool = revfs_tpool_create(2, 4);
    if (!pool) return 0;

    if (revfs_tpool_submit(pool, NULL, NULL) != -1) {
        revfs_tpool_destroy(pool, 1);
        return 0;
    }

    if (revfs_tpool_try_submit(pool, NULL, NULL) != -1) {
        revfs_tpool_destroy(pool, 1);
        return 0;
    }

    if (revfs_tpool_destroy(pool, 1) != 0) return 0;

    return 1;
}

/* ======================================================= */
/*  Main test runner                                       */
/* ======================================================= */
int main(void)
{
    printf("\n━━━ RevFS Day 10 — Concurrent Clients & Thread Pool Tests ━━━\n\n");

    RUN(test_tpool_create_destroy_clean);
    RUN(test_tpool_single_task_execution);
    RUN(test_tpool_concurrent_counter_increments);
    RUN(test_tpool_multiple_tasks_batch);
    RUN(test_tpool_wait_barrier);
    RUN(test_tpool_try_submit_and_capacity);
    RUN(test_tpool_graceful_vs_immediate_shutdown);
    RUN(test_server_concurrent_pings);
    RUN(test_server_concurrent_uploads_and_downloads);
    RUN(test_tpool_invalid_args_and_edge_cases);

    printf("\n━━━ Results: %d / %d passed ━━━\n\n", tests_passed, tests_run);

    cleanup();
    return (tests_passed == tests_run) ? 0 : 1;
}
