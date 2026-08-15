/*
 * RevFS — Day 2 Test
 *
 * Exercises the POSIX file abstraction layer.
 * Compile separately:
 *   clang -Wall -Wextra -std=c11 -g -Iinclude -o test_file \
 *         tests/test_file.c src/file.c
 *
 * Run:
 *   ./test_file
 */

#include "revfs.h"
#include <assert.h>

#define TEST_DIR  "data/test_tmp"
#define TEST_FILE TEST_DIR "/test_file.dat"

static int tests_run    = 0;
static int tests_passed = 0;

#define RUN(name) do {                                    \
    tests_run++;                                          \
    printf("  [%d] %-40s ", tests_run, #name);            \
    if (name()) { tests_passed++; printf("✅ PASS\n"); }  \
    else        { printf("❌ FAIL\n"); }                   \
} while (0)

/* ------- Individual tests ------- */

static int test_mkdir_p(void)
{
    if (revfs_mkdir_p(TEST_DIR "/nested/deep", 0755) < 0) return 0;
    if (!revfs_file_exists(TEST_DIR "/nested/deep"))       return 0;
    return 1;
}

static int test_open_create(void)
{
    int fd = revfs_file_open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    revfs_file_close(fd);
    if (!revfs_file_exists(TEST_FILE)) return 0;
    return 1;
}

static int test_write_read(void)
{
    const char msg[] = "Hello, RevFS!";
    size_t len = sizeof(msg) - 1;  /* exclude null terminator */

    /* Write */
    int fd = revfs_file_open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    ssize_t w = revfs_file_write_all(fd, msg, len);
    if (w != (ssize_t)len) { revfs_file_close(fd); return 0; }
    revfs_file_sync(fd);
    revfs_file_close(fd);

    /* Read back */
    fd = revfs_file_open(TEST_FILE, O_RDONLY, 0);
    if (fd < 0) return 0;
    char buf[64] = {0};
    ssize_t r = revfs_file_read_all(fd, buf, len);
    revfs_file_close(fd);
    if (r != (ssize_t)len) return 0;
    if (memcmp(buf, msg, len) != 0) return 0;
    return 1;
}

static int test_file_size(void)
{
    const char data[] = "0123456789";  /* 10 bytes */
    int fd = revfs_file_open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    revfs_file_write_all(fd, data, 10);
    revfs_file_sync(fd);

    off_t sz = revfs_file_size(fd);
    revfs_file_close(fd);
    if (sz != 10) return 0;

    /* Also test the path-based variant */
    sz = revfs_file_size_path(TEST_FILE);
    if (sz != 10) return 0;
    return 1;
}

static int test_seek(void)
{
    const char data[] = "ABCDEFGHIJ";
    int fd = revfs_file_open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    revfs_file_write_all(fd, data, 10);

    /* Seek to offset 5, read "FGHIJ" */
    off_t pos = revfs_file_seek(fd, 5, SEEK_SET);
    if (pos != 5) { revfs_file_close(fd); return 0; }

    char buf[6] = {0};
    revfs_file_read(fd, buf, 5);
    revfs_file_close(fd);
    if (memcmp(buf, "FGHIJ", 5) != 0) return 0;
    return 1;
}

static int test_pread_pwrite(void)
{
    /* Write "AAAAAAAAAA" then pwrite "BB" at offset 3 → "AAABBAAAAA" */
    char data[10];
    memset(data, 'A', 10);

    int fd = revfs_file_open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    revfs_file_write_all(fd, data, 10);

    /* pwrite at offset 3 */
    if (revfs_file_pwrite(fd, "BB", 2, 3) != 2) {
        revfs_file_close(fd);
        return 0;
    }

    /* pread 10 bytes from offset 0 */
    char buf[11] = {0};
    if (revfs_file_pread(fd, buf, 10, 0) != 10) {
        revfs_file_close(fd);
        return 0;
    }
    revfs_file_close(fd);

    if (memcmp(buf, "AAABBAAAAA", 10) != 0) return 0;
    return 1;
}

static int test_append(void)
{
    /* Start with a fresh file */
    int fd = revfs_file_open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    revfs_file_write_all(fd, "first", 5);
    revfs_file_close(fd);

    /* Append */
    ssize_t a = revfs_file_append(TEST_FILE, "_second", 7);
    if (a != 7) return 0;

    /* Verify combined content */
    fd = revfs_file_open(TEST_FILE, O_RDONLY, 0);
    if (fd < 0) return 0;
    char buf[32] = {0};
    revfs_file_read(fd, buf, 12);
    revfs_file_close(fd);
    if (memcmp(buf, "first_second", 12) != 0) return 0;
    return 1;
}

static int test_file_exists(void)
{
    if (!revfs_file_exists(TEST_FILE)) return 0;     /* should exist */
    if (revfs_file_exists("/no/such/path"))  return 0; /* should not */
    return 1;
}

/* ------- Cleanup helper ------- */
static void cleanup(void)
{
    /* Remove test files */
    unlink(TEST_FILE);
    rmdir(TEST_DIR "/nested/deep");
    rmdir(TEST_DIR "/nested");
    rmdir(TEST_DIR);
}

/* ------- main ------- */
int main(void)
{
    printf("\n━━━ RevFS Day 2 — POSIX File Abstraction Tests ━━━\n\n");

    RUN(test_mkdir_p);
    RUN(test_open_create);
    RUN(test_write_read);
    RUN(test_file_size);
    RUN(test_seek);
    RUN(test_pread_pwrite);
    RUN(test_append);
    RUN(test_file_exists);

    printf("\n━━━ Results: %d / %d passed ━━━\n\n", tests_passed, tests_run);

    cleanup();

    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
