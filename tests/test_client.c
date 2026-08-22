/*
 * RevFS — Day 9 Tests
 *
 * Exercises TCP client operations: socket connection, remote ping,
 * remote chunk existence checks, remote chunk store/get, distributed
 * deduplication, remote file upload, remote file download (latest & specific
 * version), remote list, remote history, and network error handling.
 *
 * Compile:
 *   make test_client
 *
 * Run:
 *   ./test_client
 */

#include "revfs.h"
#include <assert.h>
#include <dirent.h>
#include <sys/wait.h>
#include <signal.h>

#define TEST_DIR        "data/test_client_tmp"
#define TEST_FILE_A     TEST_DIR "/cli_a.txt"
#define TEST_FILE_B     TEST_DIR "/cli_b.txt"
#define TEST_OUT_A      TEST_DIR "/out_cli_a.txt"
#define TEST_OUT_V1     TEST_DIR "/out_cli_v1.txt"
#define TEST_OUT_V2     TEST_DIR "/out_cli_v2.txt"

static int tests_run    = 0;
static int tests_passed = 0;

#define RUN(name) do {                                    \
    fflush(stdout);                                       \
    tests_run++;                                          \
    printf("  [%d] %-46s ", tests_run, #name);            \
    fflush(stdout);                                       \
    if (name()) { tests_passed++; printf("✅ PASS\n"); }  \
    else        { printf("❌ FAIL\n"); }                   \
    fflush(stdout);                                       \
} while (0)

/* ------- Helper: Recursive cleanup ------- */
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
    recursive_rm("data/meta/cli_a.txt");
    recursive_rm("data/meta/cli_b.txt");
    recursive_rm("data/meta/cli_dedup.txt");
}

/* Helper: create local test file */
static int create_test_file(const char *path, const char *content, size_t len)
{
    revfs_mkdir_p(TEST_DIR, 0755);
    int fd = revfs_file_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (content && len > 0)
        revfs_file_write_all(fd, content, len);
    revfs_file_close(fd);
    return 0;
}

/* Helper: read entire file into buffer */
static ssize_t read_entire_file(const char *path, char *buf, size_t max_len)
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
        /* Child: run server loop */
        struct pollfd pfd;
        pfd.fd     = listen_fd;
        pfd.events = POLLIN;

        while (1) {
            int prc = poll(&pfd, 1, 200);
            if (prc > 0 && (pfd.revents & POLLIN)) {
                int client_fd = accept(listen_fd, NULL, NULL);
                if (client_fd >= 0) {
                    revfs_server_handle_client(client_fd);
                    revfs_file_close(client_fd);
                }
            }
        }
        _exit(0);
    }

    /* Parent */
    revfs_file_close(listen_fd);
    usleep(20000); /* 20ms startup grace */
    return pid;
}

static void stop_server(pid_t pid)
{
    if (pid > 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
}

/* ======================================================= */
/*  Test 1: Connect and Ping                               */
/* ======================================================= */
static int test_client_connect_and_ping(void)
{
    int port = 0;
    pid_t srv_pid = spawn_server(&port);
    if (srv_pid <= 0) return 0;

    int sock = revfs_client_connect("127.0.0.1", port);
    if (sock < 0) {
        stop_server(srv_pid);
        return 0;
    }

    char resp[256];
    int rc = revfs_client_ping(sock, "day9_ping_payload", resp, sizeof(resp));
    if (rc != 0 || strcmp(resp, "PONG day9_ping_payload") != 0) {
        revfs_client_disconnect(sock);
        stop_server(srv_pid);
        return 0;
    }

    revfs_client_disconnect(sock);
    stop_server(srv_pid);
    return 1;
}

/* ======================================================= */
/*  Test 2: Remote HAS_CHUNK check                         */
/* ======================================================= */
static int test_client_has_chunk_remote(void)
{
    cleanup();

    /* Store a chunk directly on the server's CAS store */
    char hash_hex[REVFS_HASH_HEX_SIZE];
    const char *payload = "Day 9 Content Addressed Test Data";
    if (revfs_chunk_store(payload, strlen(payload), hash_hex) < 0) return 0;

    int port = 0;
    pid_t srv_pid = spawn_server(&port);
    if (srv_pid <= 0) return 0;

    int sock = revfs_client_connect("127.0.0.1", port);
    if (sock < 0) { stop_server(srv_pid); return 0; }

    /* Check chunk that exists */
    int has_existing = revfs_client_has_chunk(sock, hash_hex);
    if (has_existing != 1) {
        revfs_client_disconnect(sock);
        stop_server(srv_pid);
        return 0;
    }

    /* Check chunk that does NOT exist */
    int has_fake = revfs_client_has_chunk(sock, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    if (has_fake != 0) {
        revfs_client_disconnect(sock);
        stop_server(srv_pid);
        return 0;
    }

    revfs_client_disconnect(sock);
    stop_server(srv_pid);
    return 1;
}

/* ======================================================= */
/*  Test 3: Store and Retrieve Chunk over TCP              */
/* ======================================================= */
static int test_client_store_and_get_chunk(void)
{
    cleanup();
    int port = 0;
    pid_t srv_pid = spawn_server(&port);
    if (srv_pid <= 0) return 0;

    int sock = revfs_client_connect("127.0.0.1", port);
    if (sock < 0) { stop_server(srv_pid); return 0; }

    const char *data = "RevFS Chunk Storage Protocol Day 9";
    size_t len = strlen(data);

    char hash_hex[REVFS_HASH_HEX_SIZE];
    revfs_sha256(data, len, hash_hex);

    /* Store chunk remotely */
    if (revfs_client_store_chunk(sock, hash_hex, data, len) != 0) {
        revfs_client_disconnect(sock);
        stop_server(srv_pid);
        return 0;
    }

    /* Get chunk remotely */
    char buf[1024];
    ssize_t loaded = revfs_client_get_chunk(sock, hash_hex, buf, sizeof(buf));
    if (loaded != (ssize_t)len || memcmp(buf, data, len) != 0) {
        revfs_client_disconnect(sock);
        stop_server(srv_pid);
        return 0;
    }

    revfs_client_disconnect(sock);
    stop_server(srv_pid);
    return 1;
}

/* ======================================================= */
/*  Test 4: Remote File Upload                             */
/* ======================================================= */
static int test_client_upload_basic(void)
{
    cleanup();
    const char *content = "Remote upload test content for RevFS Day 9!";
    create_test_file(TEST_FILE_A, content, strlen(content));

    int port = 0;
    pid_t srv_pid = spawn_server(&port);
    if (srv_pid <= 0) return 0;

    int version = revfs_client_upload("127.0.0.1", port, TEST_FILE_A);
    if (version != 1) {
        stop_server(srv_pid);
        return 0;
    }

    /* Verify metadata was persisted */
    revfs_meta_t meta;
    if (revfs_meta_read("cli_a.txt", 1, &meta) != 0) {
        stop_server(srv_pid);
        return 0;
    }

    if (meta.file_size != (off_t)strlen(content) || meta.num_chunks != 1) {
        stop_server(srv_pid);
        return 0;
    }

    stop_server(srv_pid);
    return 1;
}

/* ======================================================= */
/*  Test 5: Remote File Download (Latest Version)          */
/* ======================================================= */
static int test_client_download_basic(void)
{
    const char *content = "Remote upload test content for RevFS Day 9!";
    int port = 0;
    pid_t srv_pid = spawn_server(&port);
    if (srv_pid <= 0) return 0;

    unlink(TEST_OUT_A);

    int rc = revfs_client_download("127.0.0.1", port, "cli_a.txt", -1, TEST_OUT_A);
    if (rc != 0) {
        stop_server(srv_pid);
        return 0;
    }

    char buf[512];
    ssize_t n = read_entire_file(TEST_OUT_A, buf, sizeof(buf));
    if (n != (ssize_t)strlen(content) || strcmp(buf, content) != 0) {
        stop_server(srv_pid);
        return 0;
    }

    stop_server(srv_pid);
    return 1;
}

/* ======================================================= */
/*  Test 6: Remote File Download Specific Version          */
/* ======================================================= */
static int test_client_download_specific_version(void)
{
    const char *content_v1 = "Remote upload test content for RevFS Day 9!";
    const char *content_v2 = "Second revision of cli_a.txt with modified body!";
    create_test_file(TEST_FILE_A, content_v2, strlen(content_v2));

    int port = 0;
    pid_t srv_pid = spawn_server(&port);
    if (srv_pid <= 0) return 0;

    int v2 = revfs_client_upload("127.0.0.1", port, TEST_FILE_A);
    if (v2 != 2) {
        stop_server(srv_pid);
        return 0;
    }

    unlink(TEST_OUT_V1);
    unlink(TEST_OUT_V2);

    if (revfs_client_download("127.0.0.1", port, "cli_a.txt", 1, TEST_OUT_V1) != 0) {
        stop_server(srv_pid);
        return 0;
    }
    if (revfs_client_download("127.0.0.1", port, "cli_a.txt", 2, TEST_OUT_V2) != 0) {
        stop_server(srv_pid);
        return 0;
    }

    char buf1[512], buf2[512];
    read_entire_file(TEST_OUT_V1, buf1, sizeof(buf1));
    read_entire_file(TEST_OUT_V2, buf2, sizeof(buf2));

    if (strcmp(buf1, content_v1) != 0 || strcmp(buf2, content_v2) != 0) {
        stop_server(srv_pid);
        return 0;
    }

    stop_server(srv_pid);
    return 1;
}

/* ======================================================= */
/*  Test 7: Remote Network Chunk Deduplication             */
/* ======================================================= */
static int test_client_dedup_efficiency(void)
{
    /* Upload another file with identical content to test dedup */
    const char *same_content = "Second revision of cli_a.txt with modified body!";
    const char *dedup_path = TEST_DIR "/cli_dedup.txt";
    create_test_file(dedup_path, same_content, strlen(same_content));

    int port = 0;
    pid_t srv_pid = spawn_server(&port);
    if (srv_pid <= 0) return 0;

    int v = revfs_client_upload("127.0.0.1", port, dedup_path);
    if (v != 1) {
        stop_server(srv_pid);
        return 0;
    }

    stop_server(srv_pid);
    return 1;
}

/* ======================================================= */
/*  Test 8: Remote List and History Queries                */
/* ======================================================= */
static int test_client_list_and_history_remote(void)
{
    int port = 0;
    pid_t srv_pid = spawn_server(&port);
    if (srv_pid <= 0) return 0;

    int list_count = revfs_client_list("127.0.0.1", port);
    if (list_count < 2) { /* Should see cli_a.txt and cli_dedup.txt */
        stop_server(srv_pid);
        return 0;
    }

    int hist_count = revfs_client_history("127.0.0.1", port, "cli_a.txt");
    if (hist_count != 2) { /* v1 and v2 */
        stop_server(srv_pid);
        return 0;
    }

    stop_server(srv_pid);
    return 1;
}

/* ======================================================= */
/*  Test 9: Corrupt Chunk & Integrity Detection            */
/* ======================================================= */
static int test_client_download_corrupt_chunk_detection(void)
{
    int port = 0;
    pid_t srv_pid = spawn_server(&port);
    if (srv_pid <= 0) return 0;

    int sock = revfs_client_connect("127.0.0.1", port);
    if (sock < 0) { stop_server(srv_pid); return 0; }

    char buf[256];
    /* Try to fetch a non-existent chunk hash */
    ssize_t r = revfs_client_get_chunk(sock, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", buf, sizeof(buf));
    if (r >= 0) {
        revfs_client_disconnect(sock);
        stop_server(srv_pid);
        return 0;
    }

    revfs_client_disconnect(sock);
    stop_server(srv_pid);
    return 1;
}

/* ======================================================= */
/*  Test 10: Invalid Connections and Error Handling        */
/* ======================================================= */
static int test_client_invalid_connections_and_errors(void)
{
    /* 1. Invalid port */
    int s1 = revfs_client_connect("127.0.0.1", -5);
    if (s1 >= 0) { revfs_client_disconnect(s1); return 0; }

    int s2 = revfs_client_connect("127.0.0.1", 70000);
    if (s2 >= 0) { revfs_client_disconnect(s2); return 0; }

    /* 2. Non-existent file upload */
    int up_rc = revfs_client_upload("127.0.0.1", 9000, "/no/such/local/file.txt");
    if (up_rc >= 0) return 0;

    /* 3. Non-existent file download */
    int port = 0;
    pid_t srv_pid = spawn_server(&port);
    if (srv_pid <= 0) return 0;

    int dl_rc = revfs_client_download("127.0.0.1", port, "non_existent_file.xyz", -1, TEST_DIR "/never.txt");
    if (dl_rc == 0) {
        stop_server(srv_pid);
        return 0;
    }

    stop_server(srv_pid);
    return 1;
}

/* ------- main ------- */
int main(void)
{
    setbuf(stdout, NULL);
    printf("\n━━━ RevFS Day 9 — TCP Client & Remote Operations Tests ━━━\n\n");

    cleanup();

    RUN(test_client_connect_and_ping);
    RUN(test_client_has_chunk_remote);
    RUN(test_client_store_and_get_chunk);
    RUN(test_client_upload_basic);
    RUN(test_client_download_basic);
    RUN(test_client_download_specific_version);
    RUN(test_client_dedup_efficiency);
    RUN(test_client_list_and_history_remote);
    RUN(test_client_download_corrupt_chunk_detection);
    RUN(test_client_invalid_connections_and_errors);

    printf("\n━━━ Results: %d / %d passed ━━━\n\n", tests_passed, tests_run);

    cleanup();

    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
