/*
 * RevFS — Day 8 Tests
 *
 * Exercises TCP server creation, wire protocol, command processing,
 * and end-to-end networking via client/server socket interactions.
 *
 * Compile separately:
 *   clang -Wall -Wextra -std=c11 -g -Iinclude -o test_server \
 *         tests/test_server.c src/server.c src/restore.c src/version.c \
 *         src/upload.c src/download.c src/chunk.c src/file.c
 *
 * Run:
 *   ./test_server
 */

#include "revfs.h"
#include <assert.h>
#include <dirent.h>
#include <sys/wait.h>

#define TEST_DIR        "data/test_server_tmp"
#define TEST_FILE_A     TEST_DIR "/srv_a.txt"
#define TEST_FILE_B     TEST_DIR "/srv_b.txt"

static int tests_run    = 0;
static int tests_passed = 0;

#define RUN(name) do {                                    \
    fflush(stdout);                                       \
    tests_run++;                                          \
    printf("  [%d] %-44s ", tests_run, #name);            \
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
    recursive_rm("data/meta/srv_a.txt");
    recursive_rm("data/meta/srv_b.txt");
}

/* ------- Helper: Create a test file ------- */
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

/* ------- Helper: Connect client socket to port on localhost ------- */
static int client_connect(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family      = AF_INET;
    srv_addr.sin_port        = htons((uint16_t)port);
    srv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        revfs_file_close(sock);
        return -1;
    }
    return sock;
}

/* ------- Helper: Send command and receive full response string ------- */
static int client_send_cmd(int sock, const char *cmd, char *out_buf, size_t out_size)
{
    if (revfs_file_write_all(sock, cmd, strlen(cmd)) != (ssize_t)strlen(cmd)) {
        return -1;
    }

    memset(out_buf, 0, out_size);
    ssize_t total = 0;
    while ((size_t)total < out_size - 1) {
        ssize_t n = revfs_file_read(sock, out_buf + total, out_size - 1 - total);
        if (n <= 0) break;
        total += n;
        out_buf[total] = '\0';
        /* If response ends with \n or END\n, we may have the response */
        if (strstr(out_buf, "\n")) {
            /* For multi-line responses (like LIST/HISTORY), wait until END\n */
            if (strncmp(out_buf, "OK ", 3) == 0 && strchr(out_buf, '\n') &&
                (strstr(out_buf, " files\n") || strstr(out_buf, " versions\n"))) {
                if (strstr(out_buf, "END\n") != NULL) {
                    break;
                }
            } else {
                break;
            }
        }
    }
    return (int)total;
}

/* ======================================================= */
/*  Test 1: Server create on ephemeral port (port 0)       */
/* ======================================================= */
static int test_server_create_ephemeral_port(void)
{
    int port = 0;
    int listen_fd = revfs_server_create(0, &port);
    if (listen_fd < 0) return 0;
    if (port <= 0 || port > 65535) {
        revfs_file_close(listen_fd);
        return 0;
    }
    revfs_file_close(listen_fd);
    return 1;
}

/* ======================================================= */
/*  Test 2: Server create with invalid port numbers        */
/* ======================================================= */
static int test_server_create_invalid_port(void)
{
    int p1 = -1;
    int fd1 = revfs_server_create(-1, &p1);
    if (fd1 >= 0) { revfs_file_close(fd1); return 0; }

    int p2 = 70000;
    int fd2 = revfs_server_create(70000, &p2);
    if (fd2 >= 0) { revfs_file_close(fd2); return 0; }

    return 1;
}

/* ======================================================= */
/*  Test 3: End-to-end PING / PONG command                 */
/* ======================================================= */
static int test_server_e2e_ping(void)
{
    int port = 0;
    int listen_fd = revfs_server_create(0, &port);
    if (listen_fd < 0) return 0;

    pid_t pid = fork();
    if (pid < 0) {
        revfs_file_close(listen_fd);
        return 0;
    }

    if (pid == 0) {
        /* Child: client */
        revfs_file_close(listen_fd);
        int sock = client_connect(port);
        if (sock < 0) _exit(1);

        char resp[512];
        if (client_send_cmd(sock, "PING\n", resp, sizeof(resp)) <= 0) _exit(1);
        if (strcmp(resp, "PONG\n") != 0) _exit(1);

        /* Send QUIT */
        client_send_cmd(sock, "QUIT\n", resp, sizeof(resp));
        revfs_file_close(sock);
        _exit(0);
    }

    /* Parent: server accepts one connection */
    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd >= 0) {
        revfs_server_handle_client(client_fd);
        revfs_file_close(client_fd);
    }
    revfs_file_close(listen_fd);

    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* ======================================================= */
/*  Test 4: End-to-end PING with custom payload argument   */
/* ======================================================= */
static int test_server_e2e_ping_arg(void)
{
    int port = 0;
    int listen_fd = revfs_server_create(0, &port);
    if (listen_fd < 0) return 0;

    pid_t pid = fork();
    if (pid == 0) {
        revfs_file_close(listen_fd);
        int sock = client_connect(port);
        if (sock < 0) _exit(1);

        char resp[512];
        if (client_send_cmd(sock, "PING hello_distributed_world\n", resp, sizeof(resp)) <= 0) _exit(1);
        if (strcmp(resp, "PONG hello_distributed_world\n") != 0) _exit(1);

        client_send_cmd(sock, "QUIT\n", resp, sizeof(resp));
        revfs_file_close(sock);
        _exit(0);
    }

    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd >= 0) {
        revfs_server_handle_client(client_fd);
        revfs_file_close(client_fd);
    }
    revfs_file_close(listen_fd);

    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* ======================================================= */
/*  Test 5: End-to-end INFO command                        */
/* ======================================================= */
static int test_server_e2e_info(void)
{
    int port = 0;
    int listen_fd = revfs_server_create(0, &port);
    if (listen_fd < 0) return 0;

    pid_t pid = fork();
    if (pid == 0) {
        revfs_file_close(listen_fd);
        int sock = client_connect(port);
        if (sock < 0) _exit(1);

        char resp[512];
        if (client_send_cmd(sock, "INFO\n", resp, sizeof(resp)) <= 0) _exit(1);
        if (strstr(resp, "OK RevFS") == NULL || strstr(resp, REVFS_VERSION) == NULL) _exit(1);

        client_send_cmd(sock, "QUIT\n", resp, sizeof(resp));
        revfs_file_close(sock);
        _exit(0);
    }

    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd >= 0) {
        revfs_server_handle_client(client_fd);
        revfs_file_close(client_fd);
    }
    revfs_file_close(listen_fd);

    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* ======================================================= */
/*  Test 6: End-to-end HELP command                        */
/* ======================================================= */
static int test_server_e2e_help(void)
{
    int port = 0;
    int listen_fd = revfs_server_create(0, &port);
    if (listen_fd < 0) return 0;

    pid_t pid = fork();
    if (pid == 0) {
        revfs_file_close(listen_fd);
        int sock = client_connect(port);
        if (sock < 0) _exit(1);

        char resp[512];
        if (client_send_cmd(sock, "HELP\n", resp, sizeof(resp)) <= 0) _exit(1);
        if (strstr(resp, "OK Supported commands") == NULL) _exit(1);

        client_send_cmd(sock, "QUIT\n", resp, sizeof(resp));
        revfs_file_close(sock);
        _exit(0);
    }

    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd >= 0) {
        revfs_server_handle_client(client_fd);
        revfs_file_close(client_fd);
    }
    revfs_file_close(listen_fd);

    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* ======================================================= */
/*  Test 7: End-to-end LIST on empty repository            */
/* ======================================================= */
static int test_server_e2e_list_empty(void)
{
    cleanup();

    int port = 0;
    int listen_fd = revfs_server_create(0, &port);
    if (listen_fd < 0) return 0;

    pid_t pid = fork();
    if (pid == 0) {
        revfs_file_close(listen_fd);
        int sock = client_connect(port);
        if (sock < 0) _exit(1);

        char resp[512];
        if (client_send_cmd(sock, "LIST\n", resp, sizeof(resp)) <= 0) _exit(1);
        if (strcmp(resp, "OK 0 files\nEND\n") != 0) _exit(1);

        client_send_cmd(sock, "QUIT\n", resp, sizeof(resp));
        revfs_file_close(sock);
        _exit(0);
    }

    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd >= 0) {
        revfs_server_handle_client(client_fd);
        revfs_file_close(client_fd);
    }
    revfs_file_close(listen_fd);

    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* ======================================================= */
/*  Test 8: End-to-end LIST with stored files              */
/* ======================================================= */
static int test_server_e2e_list_files(void)
{
    cleanup();
    /* Upload two test files */
    create_test_file(TEST_FILE_A, "Day 8 server test A", 20);
    create_test_file(TEST_FILE_B, "Day 8 server test B content", 27);
    revfs_upload(TEST_FILE_A);
    revfs_upload(TEST_FILE_B);

    int port = 0;
    int listen_fd = revfs_server_create(0, &port);
    if (listen_fd < 0) return 0;

    pid_t pid = fork();
    if (pid == 0) {
        revfs_file_close(listen_fd);
        int sock = client_connect(port);
        if (sock < 0) _exit(1);

        char resp[1024];
        if (client_send_cmd(sock, "LIST\n", resp, sizeof(resp)) <= 0) _exit(1);
        if (strstr(resp, "OK 2 files") == NULL) _exit(1);
        if (strstr(resp, "srv_a.txt") == NULL) _exit(1);
        if (strstr(resp, "srv_b.txt") == NULL) _exit(1);
        if (strstr(resp, "END\n") == NULL) _exit(1);

        client_send_cmd(sock, "QUIT\n", resp, sizeof(resp));
        revfs_file_close(sock);
        _exit(0);
    }

    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd >= 0) {
        revfs_server_handle_client(client_fd);
        revfs_file_close(client_fd);
    }
    revfs_file_close(listen_fd);

    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* ======================================================= */
/*  Test 9: End-to-end HISTORY command                     */
/* ======================================================= */
static int test_server_e2e_history(void)
{
    /* Upload a second version of srv_a.txt */
    create_test_file(TEST_FILE_A, "Day 8 server test A - version 2 updated", 39);
    revfs_upload(TEST_FILE_A);

    int port = 0;
    int listen_fd = revfs_server_create(0, &port);
    if (listen_fd < 0) return 0;

    pid_t pid = fork();
    if (pid == 0) {
        revfs_file_close(listen_fd);
        int sock = client_connect(port);
        if (sock < 0) _exit(1);

        char resp[1024];
        if (client_send_cmd(sock, "HISTORY srv_a.txt\n", resp, sizeof(resp)) <= 0) _exit(1);
        if (strstr(resp, "OK 2 versions") == NULL) _exit(1);
        if (strstr(resp, "v1") == NULL || strstr(resp, "v2") == NULL) _exit(1);
        if (strstr(resp, "END\n") == NULL) _exit(1);

        /* Query non-existent history */
        if (client_send_cmd(sock, "HISTORY not_found.txt\n", resp, sizeof(resp)) <= 0) _exit(1);
        if (strstr(resp, "ERR") == NULL) _exit(1);

        client_send_cmd(sock, "QUIT\n", resp, sizeof(resp));
        revfs_file_close(sock);
        _exit(0);
    }

    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd >= 0) {
        revfs_server_handle_client(client_fd);
        revfs_file_close(client_fd);
    }
    revfs_file_close(listen_fd);

    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* ======================================================= */
/*  Test 10: End-to-end Unknown command and sequential cmds*/
/* ======================================================= */
static int test_server_e2e_unknown_and_pipeline(void)
{
    int port = 0;
    int listen_fd = revfs_server_create(0, &port);
    if (listen_fd < 0) return 0;

    pid_t pid = fork();
    if (pid == 0) {
        revfs_file_close(listen_fd);
        int sock = client_connect(port);
        if (sock < 0) _exit(1);

        char resp[512];
        /* 1. Invalid command */
        if (client_send_cmd(sock, "NONEXISTENT_CMD\n", resp, sizeof(resp)) <= 0) _exit(1);
        if (strstr(resp, "ERR Unknown command: NONEXISTENT_CMD") == NULL) _exit(1);

        /* 2. Pipeline a valid command right after */
        if (client_send_cmd(sock, "PING pipeline_test\n", resp, sizeof(resp)) <= 0) _exit(1);
        if (strcmp(resp, "PONG pipeline_test\n") != 0) _exit(1);

        /* 3. QUIT */
        if (client_send_cmd(sock, "QUIT\n", resp, sizeof(resp)) <= 0) _exit(1);
        if (strcmp(resp, "BYE\n") != 0) _exit(1);

        revfs_file_close(sock);
        _exit(0);
    }

    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd >= 0) {
        revfs_server_handle_client(client_fd);
        revfs_file_close(client_fd);
    }
    revfs_file_close(listen_fd);

    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* ------- main ------- */
int main(void)
{
    setbuf(stdout, NULL);
    printf("\n━━━ RevFS Day 8 — TCP Server Skeleton Tests ━━━\n\n");

    cleanup();

    RUN(test_server_create_ephemeral_port);
    RUN(test_server_create_invalid_port);
    RUN(test_server_e2e_ping);
    RUN(test_server_e2e_ping_arg);
    RUN(test_server_e2e_info);
    RUN(test_server_e2e_help);
    RUN(test_server_e2e_list_empty);
    RUN(test_server_e2e_list_files);
    RUN(test_server_e2e_history);
    RUN(test_server_e2e_unknown_and_pipeline);

    printf("\n━━━ Results: %d / %d passed ━━━\n\n", tests_passed, tests_run);

    cleanup();

    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
