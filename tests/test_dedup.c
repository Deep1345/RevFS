/*
 * RevFS — Day 11 Tests
 *
 * Exercises Deduplication and Storage Statistics:
 * - Empty storage stats
 * - Single file upload stats
 * - Multi-version duplicate content deduplication savings
 * - Multi-file cross-file deduplication
 * - Partial chunk modification stats
 * - Restore deduplication efficiency
 * - Content-addressed storage directory traversal
 * - Remote STATS command over TCP
 * - Invalid arguments & boundary conditions
 * - Pretty-printing of statistics
 *
 * Compile:
 *   make test_dedup
 *
 * Run:
 *   ./test_dedup
 */

#include "revfs.h"
#include <assert.h>
#include <dirent.h>
#include <sys/wait.h>
#include <signal.h>

#define TEST_DIR        "data/test_dedup_tmp"
#define TEST_FILE_A     TEST_DIR "/dedup_a.txt"
#define TEST_FILE_B     TEST_DIR "/dedup_b.txt"
#define TEST_FILE_LARGE TEST_DIR "/dedup_large.bin"

static int tests_run    = 0;
static int tests_passed = 0;

#define RUN(name) do {                                    \
    fflush(stdout);                                       \
    tests_run++;                                          \
    printf("  [%d] %-48s ", tests_run, #name);           \
    fflush(stdout);                                       \
    if (name()) { tests_passed++; printf("✅ PASS\n"); }  \
    else        { printf("❌ FAIL\n"); }                   \
    fflush(stdout);                                       \
} while (0)

/* Helper: Recursive cleanup */
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

static void cleanup_test_env(void)
{
    recursive_rm(TEST_DIR);
    recursive_rm("data/meta/dedup_a.txt");
    recursive_rm("data/meta/dedup_b.txt");
    recursive_rm("data/meta/dedup_large.bin");
}

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

/* Helper: spawn server in background process */
static pid_t spawn_server(int *out_port)
{
    int port = 0;
    int listen_fd = revfs_server_create(0, &port);
    if (listen_fd < 0) return -1;

    *out_port = port;

    pid_t pid = fork();
    if (pid < 0) {
        revfs_file_close(listen_fd);
        return -1;
    }

    if (pid == 0) {
        struct pollfd pfd;
        pfd.fd = listen_fd;
        pfd.events = POLLIN;

        while (1) {
            int ret = poll(&pfd, 1, 1000);
            if (ret > 0 && (pfd.revents & POLLIN)) {
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
                if (client_fd >= 0) {
                    revfs_server_handle_client(client_fd);
                }
            }
        }
        exit(0);
    }

    revfs_file_close(listen_fd);
    usleep(50000); /* 50ms warm-up */
    return pid;
}

static void stop_server(pid_t pid)
{
    if (pid > 0) {
        kill(pid, SIGTERM);
        int status;
        waitpid(pid, &status, 0);
    }
}

/* ======================================================= */
/*  Test 1: Empty storage statistics                        */
/* ======================================================= */
static int test_dedup_empty_storage(void)
{
    cleanup_test_env();

    revfs_stats_t stats;
    if (revfs_stats_calculate(&stats) < 0) return 0;

    /* In empty storage (or if previous test leftovers existed in meta), verify non-crash and proper zero bounds */
    if (stats.logical_bytes == 0) {
        if (stats.dedup_ratio != 1.0) return 0;
        if (stats.savings_bytes != 0) return 0;
        if (stats.savings_percent != 0.0) return 0;
    }

    return 1;
}

/* ======================================================= */
/*  Test 2: Single file upload statistics                   */
/* ======================================================= */
static int test_dedup_single_file(void)
{
    cleanup_test_env();

    const char *content = "RevFS Day 11 Deduplication Test Content Alpha";
    size_t len = strlen(content);
    if (create_test_file(TEST_FILE_A, content, len) < 0) return 0;

    int v1 = revfs_upload(TEST_FILE_A);
    if (v1 != 1) return 0;

    revfs_stats_t stats;
    if (revfs_stats_calculate(&stats) < 0) return 0;

    if (stats.total_files < 1) return 0;
    if (stats.total_versions < 1) return 0;
    if (stats.logical_bytes < (off_t)len) return 0;
    if (stats.physical_bytes < (off_t)len) return 0;
    if (stats.unique_chunks < 1) return 0;
    if (stats.referenced_chunks < 1) return 0;

    return 1;
}

/* ======================================================= */
/*  Test 3: Identical multi-version deduplication savings   */
/* ======================================================= */
static int test_dedup_identical_multi_version(void)
{
    cleanup_test_env();

    const char *content = "Unique repeatable chunk data for version duplication 1234567890";
    size_t len = strlen(content);
    if (create_test_file(TEST_FILE_A, content, len) < 0) return 0;

    /* Upload version 1 */
    revfs_upload(TEST_FILE_A);

    revfs_stats_t s1;
    revfs_stats_calculate(&s1);

    /* Upload version 2 (exact same content) */
    revfs_upload(TEST_FILE_A);

    /* Upload version 3 (exact same content) */
    revfs_upload(TEST_FILE_A);

    revfs_stats_t s3;
    if (revfs_stats_calculate(&s3) < 0) return 0;

    /* Version count for this file is 3 */
    if (revfs_version_count("dedup_a.txt") != 3) return 0;

    /* Logical bytes grew by ~3x, physical bytes stayed same for this file */
    if (s3.referenced_chunks < s1.referenced_chunks + 2) return 0;
    if (s3.unique_chunks != s1.unique_chunks) return 0;
    if (s3.dedup_ratio < 1.5) return 0; /* Dedup ratio is greater than 1.5x */
    if (s3.savings_bytes <= 0) return 0;
    if (s3.savings_percent <= 0.0) return 0;

    return 1;
}

/* ======================================================= */
/*  Test 4: Cross-file content deduplication                */
/* ======================================================= */
static int test_dedup_multi_file_shared_content(void)
{
    cleanup_test_env();

    const char *shared_data = "Cross file shared content block: abcdefgh12345678";
    size_t len = strlen(shared_data);

    if (create_test_file(TEST_FILE_A, shared_data, len) < 0) return 0;
    if (create_test_file(TEST_FILE_B, shared_data, len) < 0) return 0;

    revfs_upload(TEST_FILE_A);
    revfs_stats_t s1;
    revfs_stats_calculate(&s1);

    revfs_upload(TEST_FILE_B);
    revfs_stats_t s2;
    revfs_stats_calculate(&s2);

    /* Unique chunks should not have increased */
    if (s2.unique_chunks != s1.unique_chunks) return 0;
    if (s2.referenced_chunks <= s1.referenced_chunks) return 0;
    if (s2.total_files <= s1.total_files) return 0;

    return 1;
}

/* ======================================================= */
/*  Test 5: Partial chunk modification statistics           */
/* ======================================================= */
static int test_dedup_partial_chunk_modification(void)
{
    cleanup_test_env();

    /* Create a 2-chunk file (~4MB + 100KB) */
    size_t chunk1_sz = REVFS_CHUNK_SIZE;
    size_t chunk2_sz = 100 * 1024;
    size_t total_sz = chunk1_sz + chunk2_sz;

    char *large_buf = malloc(total_sz);
    if (!large_buf) return 0;
    memset(large_buf, 'A', chunk1_sz);
    memset(large_buf + chunk1_sz, 'B', chunk2_sz);

    if (create_test_file(TEST_FILE_LARGE, large_buf, total_sz) < 0) {
        free(large_buf);
        return 0;
    }

    int v1 = revfs_upload(TEST_FILE_LARGE);
    if (v1 != 1) { free(large_buf); return 0; }

    revfs_stats_t s1;
    revfs_stats_calculate(&s1);

    /* Modify only chunk 2 and upload v2 */
    memset(large_buf + chunk1_sz, 'C', chunk2_sz);
    create_test_file(TEST_FILE_LARGE, large_buf, total_sz);
    free(large_buf);

    int v2 = revfs_upload(TEST_FILE_LARGE);
    if (v2 != 2) return 0;

    revfs_stats_t s2;
    revfs_stats_calculate(&s2);

    /* Referenced chunks grew by 2 (from 2 to 4), but unique chunks only grew by 1 (from 2 to 3) */
    if (s2.referenced_chunks - s1.referenced_chunks != 2) return 0;
    if (s2.unique_chunks - s1.unique_chunks != 1) return 0;
    if (s2.dedup_ratio <= 1.0) return 0;

    return 1;
}

/* ======================================================= */
/*  Test 6: Deduplication after version restore             */
/* ======================================================= */
static int test_dedup_after_restore(void)
{
    cleanup_test_env();

    const char *v1_data = "Version 1 text for restore test";
    const char *v2_data = "Version 2 text for restore test (different)";

    create_test_file(TEST_FILE_A, v1_data, strlen(v1_data));
    revfs_upload(TEST_FILE_A);

    create_test_file(TEST_FILE_A, v2_data, strlen(v2_data));
    revfs_upload(TEST_FILE_A);

    revfs_stats_t s_before;
    revfs_stats_calculate(&s_before);

    /* Restore version 1 -> creates version 3 referencing version 1's chunk */
    int v3 = revfs_restore("dedup_a.txt", 1);
    if (v3 != 3) return 0;

    revfs_stats_t s_after;
    revfs_stats_calculate(&s_after);

    /* Total versions grew by 1, unique chunks stayed unchanged */
    if (s_after.total_versions != s_before.total_versions + 1) return 0;
    if (s_after.unique_chunks != s_before.unique_chunks) return 0;
    if (s_after.referenced_chunks != s_before.referenced_chunks + 1) return 0;

    return 1;
}

/* ======================================================= */
/*  Test 7: Direct chunk directory traversal info           */
/* ======================================================= */
static int test_dedup_chunks_info_traversal(void)
{
    int u_chunks = 0;
    off_t p_bytes = 0;
    if (revfs_stats_chunks_info(&u_chunks, &p_bytes) < 0) return 0;

    if (u_chunks < 0 || p_bytes < 0) return 0;
    return 1;
}

/* ======================================================= */
/*  Test 8: Remote STATS command over TCP                   */
/* ======================================================= */
static int test_dedup_remote_stats_tcp(void)
{
    cleanup_test_env();

    const char *msg = "Stats over TCP test message content";
    create_test_file(TEST_FILE_A, msg, strlen(msg));
    revfs_upload(TEST_FILE_A);

    int port = 0;
    pid_t pid = spawn_server(&port);
    if (pid < 0) return 0;

    int sock = revfs_client_connect("127.0.0.1", port);
    if (sock < 0) {
        stop_server(pid);
        return 0;
    }

    revfs_stats_t stats;
    if (revfs_client_get_stats(sock, &stats) < 0) {
        revfs_client_disconnect(sock);
        stop_server(pid);
        return 0;
    }
    revfs_client_disconnect(sock);

    if (stats.total_files < 1) {
        stop_server(pid);
        return 0;
    }
    if (stats.total_versions < 1) {
        stop_server(pid);
        return 0;
    }
    if (stats.logical_bytes < (off_t)strlen(msg)) {
        stop_server(pid);
        return 0;
    }

    /* Test high-level client_stats call */
    if (revfs_client_stats("127.0.0.1", port) < 0) {
        stop_server(pid);
        return 0;
    }

    stop_server(pid);
    return 1;
}

/* ======================================================= */
/*  Test 9: Invalid arguments & error handling              */
/* ======================================================= */
static int test_dedup_invalid_arguments(void)
{
    if (revfs_stats_calculate(NULL) == 0) return 0;
    if (revfs_stats_print(NULL) == 0) return 0;
    if (revfs_stats_chunks_info(NULL, NULL) == 0) return 0;
    if (revfs_client_get_stats(-1, NULL) == 0) return 0;
    if (revfs_client_stats("127.0.0.1", -5) == 0) return 0;

    return 1;
}

/* ======================================================= */
/*  Test 10: Formatting output & execution                  */
/* ======================================================= */
static int test_dedup_formatting_output(void)
{
    revfs_stats_t dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.total_files = 10;
    dummy.total_versions = 25;
    dummy.referenced_chunks = 50;
    dummy.unique_chunks = 20;
    dummy.logical_bytes = 200 * 1024 * 1024;
    dummy.physical_bytes = 80 * 1024 * 1024;
    dummy.dedup_ratio = 2.5;
    dummy.savings_bytes = 120 * 1024 * 1024;
    dummy.savings_percent = 60.0;

    if (revfs_stats_print(&dummy) < 0) return 0;
    if (revfs_stats() < 0) return 0;

    cleanup_test_env();
    return 1;
}

int main(void)
{
    printf("\n━━━ RevFS Day 11 — Deduplication & Storage Stats Tests ━━━\n\n");

    RUN(test_dedup_empty_storage);
    RUN(test_dedup_single_file);
    RUN(test_dedup_identical_multi_version);
    RUN(test_dedup_multi_file_shared_content);
    RUN(test_dedup_partial_chunk_modification);
    RUN(test_dedup_after_restore);
    RUN(test_dedup_chunks_info_traversal);
    RUN(test_dedup_remote_stats_tcp);
    RUN(test_dedup_invalid_arguments);
    RUN(test_dedup_formatting_output);

    printf("\n━━━ Results: %d / %d passed ━━━\n\n", tests_passed, tests_run);

    cleanup_test_env();
    return (tests_passed == tests_run) ? 0 : 1;
}
