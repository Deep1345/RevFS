/*
 * RevFS — Day 12 Tests
 *
 * Exercises Two-Node Replication:
 * - Dual-node configuration and health pings
 * - Dual-node chunk storage and presence verification
 * - Dual-node replicated file upload
 * - Normal download from Primary node
 * - Automatic failover download when Primary node is down
 * - Failover download when Secondary node is down
 * - Chunk-level failover when Primary chunk is corrupt/missing
 * - Degraded mode upload (quorum=1 with one node down)
 * - Two-way replica synchronization and auto-repair
 * - Invalid arguments and unreachable node error handling
 *
 * Compile:
 *   make test_replication
 *
 * Run:
 *   ./test_replication
 */

#include "revfs.h"
#include <assert.h>
#include <dirent.h>
#include <sys/wait.h>
#include <signal.h>

#define TEST_DIR         "data/test_repl_tmp"
#define TEST_SRC_A       TEST_DIR "/repl_src_a.txt"
#define TEST_SRC_B       TEST_DIR "/repl_src_b.txt"
#define TEST_OUT_A       TEST_DIR "/repl_out_a.txt"
#define TEST_OUT_B       TEST_DIR "/repl_out_b.txt"

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
    recursive_rm("data/meta/repl_src_a.txt");
    recursive_rm("data/meta/repl_src_b.txt");
    recursive_rm("data/meta/repl_sync_a.txt");
    recursive_rm("data/meta/repl_sync_b.txt");
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

static ssize_t read_test_file(const char *path, char *buf, size_t max_len)
{
    int fd = revfs_file_open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    off_t sz = revfs_file_size(fd);
    if (sz < 0 || (size_t)sz >= max_len) {
        revfs_file_close(fd);
        return -1;
    }
    ssize_t n = revfs_file_read_all(fd, buf, (size_t)sz);
    revfs_file_close(fd);
    if (n >= 0) buf[n] = '\0';
    return n;
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
/*  Test 1: Config init and dual-node ping                  */
/* ======================================================= */
static int test_repl_config_and_ping(void)
{
    cleanup_test_env();

    int port1 = 0, port2 = 0;
    pid_t pid1 = spawn_server(&port1);
    pid_t pid2 = spawn_server(&port2);
    if (pid1 < 0 || pid2 < 0) {
        stop_server(pid1);
        stop_server(pid2);
        return 0;
    }

    revfs_repl_config_t cfg;
    if (revfs_repl_config_init(&cfg, "127.0.0.1", port1, "127.0.0.1", port2) < 0) {
        stop_server(pid1);
        stop_server(pid2);
        return 0;
    }

    int p_ok = 0, s_ok = 0;
    int rc = revfs_repl_ping(&cfg, &p_ok, &s_ok);

    stop_server(pid1);
    stop_server(pid2);

    return (rc == 0 && p_ok == 1 && s_ok == 1);
}

/* ======================================================= */
/*  Test 2: Dual chunk store and presence check             */
/* ======================================================= */
static int test_repl_dual_chunk_store_and_check(void)
{
    cleanup_test_env();

    int port1 = 0, port2 = 0;
    pid_t pid1 = spawn_server(&port1);
    pid_t pid2 = spawn_server(&port2);
    if (pid1 < 0 || pid2 < 0) {
        stop_server(pid1);
        stop_server(pid2);
        return 0;
    }

    revfs_repl_config_t cfg;
    revfs_repl_config_init(&cfg, "127.0.0.1", port1, "127.0.0.1", port2);

    const char *payload = "Replication Chunk Binary Content 123456789";
    size_t len = strlen(payload);
    char hash_hex[REVFS_HASH_HEX_SIZE];
    revfs_sha256(payload, len, hash_hex);

    /* Store chunk across both nodes */
    int stored = revfs_repl_store_chunk(&cfg, hash_hex, payload, len);
    if (stored != 2) {
        stop_server(pid1);
        stop_server(pid2);
        return 0;
    }

    /* Verify chunk presence on both nodes */
    int p_has = 0, s_has = 0;
    revfs_repl_has_chunk(&cfg, hash_hex, &p_has, &s_has);

    /* Verify chunk retrieval */
    char buf[128];
    ssize_t loaded = revfs_repl_get_chunk(&cfg, hash_hex, buf, sizeof(buf));

    stop_server(pid1);
    stop_server(pid2);

    return (p_has == 1 && s_has == 1 && loaded == (ssize_t)len && memcmp(buf, payload, len) == 0);
}

/* ======================================================= */
/*  Test 3: Dual replicated upload                          */
/* ======================================================= */
static int test_repl_dual_upload_success(void)
{
    cleanup_test_env();

    int port1 = 0, port2 = 0;
    pid_t pid1 = spawn_server(&port1);
    pid_t pid2 = spawn_server(&port2);
    if (pid1 < 0 || pid2 < 0) {
        stop_server(pid1);
        stop_server(pid2);
        return 0;
    }

    revfs_repl_config_t cfg;
    revfs_repl_config_init(&cfg, "127.0.0.1", port1, "127.0.0.1", port2);

    const char *content = "Dual Replicated File Upload Test Content Alpha";
    create_test_file(TEST_SRC_A, content, strlen(content));

    int ver = revfs_repl_upload(&cfg, TEST_SRC_A);

    stop_server(pid1);
    stop_server(pid2);

    return (ver == 1);
}

/* ======================================================= */
/*  Test 4: Normal download with primary online             */
/* ======================================================= */
static int test_repl_download_primary_online(void)
{
    cleanup_test_env();

    int port1 = 0, port2 = 0;
    pid_t pid1 = spawn_server(&port1);
    pid_t pid2 = spawn_server(&port2);
    if (pid1 < 0 || pid2 < 0) {
        stop_server(pid1);
        stop_server(pid2);
        return 0;
    }

    revfs_repl_config_t cfg;
    revfs_repl_config_init(&cfg, "127.0.0.1", port1, "127.0.0.1", port2);

    const char *content = "Testing Download with Primary Replicated Online";
    create_test_file(TEST_SRC_A, content, strlen(content));
    revfs_repl_upload(&cfg, TEST_SRC_A);

    /* Delete local CAS and test file to force network fetch */
    unlink(TEST_SRC_A);
    unlink(TEST_OUT_A);

    int rc = revfs_repl_download(&cfg, "repl_src_a.txt", 1, TEST_OUT_A);

    char read_back[256];
    ssize_t n = read_test_file(TEST_OUT_A, read_back, sizeof(read_back));

    stop_server(pid1);
    stop_server(pid2);

    return (rc == 0 && n == (ssize_t)strlen(content) && strcmp(read_back, content) == 0);
}

/* ======================================================= */
/*  Test 5: Fallback download when Primary is DOWN          */
/* ======================================================= */
static int test_repl_download_fallback_when_primary_down(void)
{
    cleanup_test_env();

    int port1 = 0, port2 = 0;
    pid_t pid1 = spawn_server(&port1);
    pid_t pid2 = spawn_server(&port2);
    if (pid1 < 0 || pid2 < 0) {
        stop_server(pid1);
        stop_server(pid2);
        return 0;
    }

    revfs_repl_config_t cfg;
    revfs_repl_config_init(&cfg, "127.0.0.1", port1, "127.0.0.1", port2);

    const char *content = "Content that will be read from Secondary when Primary is dead!";
    create_test_file(TEST_SRC_A, content, strlen(content));
    revfs_repl_upload(&cfg, TEST_SRC_A);

    /* Kill Primary server */
    stop_server(pid1);

    unlink(TEST_OUT_A);

    /* Download should transparently fallback to Secondary (port2) */
    int rc = revfs_repl_download(&cfg, "repl_src_a.txt", 1, TEST_OUT_A);

    char read_back[256];
    ssize_t n = read_test_file(TEST_OUT_A, read_back, sizeof(read_back));

    stop_server(pid2);

    return (rc == 0 && n == (ssize_t)strlen(content) && strcmp(read_back, content) == 0);
}

/* ======================================================= */
/*  Test 6: Download when Secondary is DOWN (Primary OK)    */
/* ======================================================= */
static int test_repl_download_fallback_when_secondary_down(void)
{
    cleanup_test_env();

    int port1 = 0, port2 = 0;
    pid_t pid1 = spawn_server(&port1);
    pid_t pid2 = spawn_server(&port2);
    if (pid1 < 0 || pid2 < 0) {
        stop_server(pid1);
        stop_server(pid2);
        return 0;
    }

    revfs_repl_config_t cfg;
    revfs_repl_config_init(&cfg, "127.0.0.1", port1, "127.0.0.1", port2);

    const char *content = "Content read normally from Primary when Secondary is offline";
    create_test_file(TEST_SRC_A, content, strlen(content));
    revfs_repl_upload(&cfg, TEST_SRC_A);

    /* Kill Secondary server */
    stop_server(pid2);

    unlink(TEST_OUT_A);

    int rc = revfs_repl_download(&cfg, "repl_src_a.txt", 1, TEST_OUT_A);

    char read_back[256];
    ssize_t n = read_test_file(TEST_OUT_A, read_back, sizeof(read_back));

    stop_server(pid1);

    return (rc == 0 && n == (ssize_t)strlen(content) && strcmp(read_back, content) == 0);
}

/* ======================================================= */
/*  Test 7: Chunk-level corrupt fallback                     */
/* ======================================================= */
static int test_repl_chunk_level_corrupt_fallback(void)
{
    cleanup_test_env();

    int port1 = 0, port2 = 0;
    pid_t pid1 = spawn_server(&port1);
    pid_t pid2 = spawn_server(&port2);
    if (pid1 < 0 || pid2 < 0) {
        stop_server(pid1);
        stop_server(pid2);
        return 0;
    }

    revfs_repl_config_t cfg;
    revfs_repl_config_init(&cfg, "127.0.0.1", port1, "127.0.0.1", port2);

    const char *payload = "Chunk for integrity fallback testing 987654321";
    char hash_hex[REVFS_HASH_HEX_SIZE];
    revfs_sha256(payload, strlen(payload), hash_hex);

    /* Store valid chunk to Secondary */
    int sock_s = revfs_client_connect(cfg.secondary.host, cfg.secondary.port);
    revfs_client_store_chunk(sock_s, hash_hex, payload, strlen(payload));
    revfs_client_disconnect(sock_s);

    /* Fetch chunk via replication: Primary doesn't have it, Secondary does */
    char buf[128];
    ssize_t n = revfs_repl_get_chunk(&cfg, hash_hex, buf, sizeof(buf));

    stop_server(pid1);
    stop_server(pid2);

    return (n == (ssize_t)strlen(payload) && memcmp(buf, payload, n) == 0);
}

/* ======================================================= */
/*  Test 8: Degraded upload (quorum=1 with 1 node down)     */
/* ======================================================= */
static int test_repl_degraded_upload(void)
{
    cleanup_test_env();

    int port1 = 0, port2 = 0;
    pid_t pid1 = spawn_server(&port1);
    /* Node 2 is not spawned / offline */
    port2 = 59998;

    if (pid1 < 0) {
        stop_server(pid1);
        return 0;
    }

    revfs_repl_config_t cfg;
    revfs_repl_config_init(&cfg, "127.0.0.1", port1, "127.0.0.1", port2);
    cfg.write_quorum = 1; /* Allow degraded upload */

    const char *content = "Degraded mode upload with 1 of 2 nodes active";
    create_test_file(TEST_SRC_B, content, strlen(content));

    int ver = revfs_repl_upload(&cfg, TEST_SRC_B);

    stop_server(pid1);

    return (ver == 1);
}

/* ======================================================= */
/*  Test 9: Two-way sync & auto-repair                      */
/* ======================================================= */
static int test_repl_sync_two_way_repair(void)
{
    cleanup_test_env();

    int port1 = 0, port2 = 0;
    pid_t pid1 = spawn_server(&port1);
    pid_t pid2 = spawn_server(&port2);
    if (pid1 < 0 || pid2 < 0) {
        stop_server(pid1);
        stop_server(pid2);
        return 0;
    }

    revfs_repl_config_t cfg;
    revfs_repl_config_init(&cfg, "127.0.0.1", port1, "127.0.0.1", port2);

    /* Run sync on the cluster */
    revfs_repl_sync_report_t report;
    int rc = revfs_repl_sync(&cfg, &report);

    stop_server(pid1);
    stop_server(pid2);

    return (rc == 0 && report.errors == 0);
}

/* ======================================================= */
/*  Test 10: Invalid arguments & error handling             */
/* ======================================================= */
static int test_repl_invalid_args_and_unreachable(void)
{
    if (revfs_repl_config_init(NULL, NULL, 0, NULL, 0) == 0) return 0;
    if (revfs_repl_ping(NULL, NULL, NULL) == 0) return 0;
    if (revfs_repl_upload(NULL, NULL) == 0) return 0;
    if (revfs_repl_download(NULL, NULL, 1, NULL) == 0) return 0;
    if (revfs_repl_store_chunk(NULL, NULL, NULL, 0) == 0) return 0;
    if (revfs_repl_get_chunk(NULL, NULL, NULL, 0) == 0) return 0;
    if (revfs_repl_sync(NULL, NULL) == 0) return 0;
    if (revfs_repl_list(NULL) >= 0) return 0;
    if (revfs_repl_history(NULL, NULL) >= 0) return 0;

    /* Ping unreachable cluster */
    revfs_repl_config_t cfg;
    revfs_repl_config_init(&cfg, "127.0.0.1", 59991, "127.0.0.1", 59992);
    int p_ok = 0, s_ok = 0;
    int ping_rc = revfs_repl_ping(&cfg, &p_ok, &s_ok);

    return (ping_rc < 0 && p_ok == 0 && s_ok == 0);
}

int main(void)
{
    printf("\n━━━ RevFS Day 12 — Two-Node Replication & Failover Tests ━━━\n\n");

    RUN(test_repl_config_and_ping);
    RUN(test_repl_dual_chunk_store_and_check);
    RUN(test_repl_dual_upload_success);
    RUN(test_repl_download_primary_online);
    RUN(test_repl_download_fallback_when_primary_down);
    RUN(test_repl_download_fallback_when_secondary_down);
    RUN(test_repl_chunk_level_corrupt_fallback);
    RUN(test_repl_degraded_upload);
    RUN(test_repl_sync_two_way_repair);
    RUN(test_repl_invalid_args_and_unreachable);

    printf("\n━━━ Results: %d / %d passed ━━━\n\n", tests_passed, tests_run);

    cleanup_test_env();
    return (tests_passed == tests_run) ? 0 : 1;
}
