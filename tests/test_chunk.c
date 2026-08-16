/*
 * RevFS — Day 3 Test
 *
 * Exercises the chunking + SHA-256 content-addressed storage layer.
 * Compile separately:
 *   clang -Wall -Wextra -std=c11 -g -Iinclude -o test_chunk \
 *         tests/test_chunk.c src/chunk.c src/file.c
 *
 * Run:
 *   ./test_chunk
 */

#include "revfs.h"
#include <assert.h>

#define TEST_DIR     "data/test_tmp"
#define TEST_FILE    TEST_DIR "/chunk_test.dat"
#define TEST_OUTPUT  TEST_DIR "/chunk_reassembled.dat"

static int tests_run    = 0;
static int tests_passed = 0;

#define RUN(name) do {                                    \
    tests_run++;                                          \
    printf("  [%d] %-40s ", tests_run, #name);            \
    if (name()) { tests_passed++; printf("✅ PASS\n"); }  \
    else        { printf("❌ FAIL\n"); }                   \
} while (0)

/* ------- Test 1: SHA-256 of known data ------- */
static int test_sha256_known(void)
{
    /* SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 */
    char hash[REVFS_HASH_HEX_SIZE];
    if (revfs_sha256("", 0, hash) < 0) return 0;
    if (strcmp(hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") != 0) {
        fprintf(stderr, "    got: %s\n", hash);
        return 0;
    }
    return 1;
}

/* ------- Test 2: SHA-256 of "hello" ------- */
static int test_sha256_hello(void)
{
    /* SHA-256("hello") = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824 */
    char hash[REVFS_HASH_HEX_SIZE];
    if (revfs_sha256("hello", 5, hash) < 0) return 0;
    if (strcmp(hash, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824") != 0) {
        fprintf(stderr, "    got: %s\n", hash);
        return 0;
    }
    return 1;
}

/* ------- Test 3: SHA-256 via file descriptor ------- */
static int test_sha256_fd(void)
{
    /* Write "hello" to a file, hash it via fd */
    revfs_mkdir_p(TEST_DIR, 0755);
    int fd = revfs_file_open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    revfs_file_write_all(fd, "hello", 5);
    revfs_file_close(fd);

    fd = revfs_file_open(TEST_FILE, O_RDONLY, 0);
    if (fd < 0) return 0;

    char hash[REVFS_HASH_HEX_SIZE];
    if (revfs_sha256_fd(fd, hash) < 0) {
        revfs_file_close(fd);
        return 0;
    }
    revfs_file_close(fd);

    if (strcmp(hash, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824") != 0)
        return 0;
    return 1;
}

/* ------- Test 4: Store and load a chunk ------- */
static int test_chunk_store_load(void)
{
    const char data[] = "This is chunk test data for RevFS Day 3.";
    size_t len = strlen(data);

    char hash[REVFS_HASH_HEX_SIZE];
    int rc = revfs_chunk_store(data, len, hash);
    if (rc < 0) return 0;  /* 0 = new, 1 = dedup */

    /* Verify the chunk exists */
    if (!revfs_chunk_exists(hash)) return 0;

    /* Load it back */
    char buf[256] = {0};
    ssize_t loaded = revfs_chunk_load(hash, buf, sizeof(buf));
    if (loaded != (ssize_t)len) return 0;
    if (memcmp(buf, data, len) != 0) return 0;

    return 1;
}

/* ------- Test 5: Deduplication ------- */
static int test_chunk_dedup(void)
{
    const char data[] = "Duplicate chunk content.";
    size_t len = strlen(data);

    char hash1[REVFS_HASH_HEX_SIZE];
    char hash2[REVFS_HASH_HEX_SIZE];

    int rc1 = revfs_chunk_store(data, len, hash1);
    if (rc1 < 0) return 0;

    int rc2 = revfs_chunk_store(data, len, hash2);
    if (rc2 < 0) return 0;

    /* Same data → same hash */
    if (strcmp(hash1, hash2) != 0) return 0;

    /* Second store should return 1 (already existed) */
    if (rc2 != 1) return 0;

    return 1;
}

/* ------- Test 6: Different data → different hashes ------- */
static int test_chunk_different(void)
{
    char hash1[REVFS_HASH_HEX_SIZE];
    char hash2[REVFS_HASH_HEX_SIZE];

    revfs_chunk_store("data_A", 6, hash1);
    revfs_chunk_store("data_B", 6, hash2);

    if (strcmp(hash1, hash2) == 0) return 0;  /* must differ */
    return 1;
}

/* ------- Test 7: File chunking (small file = 1 chunk) ------- */
static int test_file_chunk_small(void)
{
    revfs_mkdir_p(TEST_DIR, 0755);

    /* Write a small file (< 4 MB → 1 chunk) */
    const char data[] = "Small file for chunking test.";
    int fd = revfs_file_open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    revfs_file_write_all(fd, data, strlen(data));
    revfs_file_close(fd);

    char hashes[4][REVFS_HASH_HEX_SIZE];
    int n = revfs_file_chunk(TEST_FILE, hashes, 4);
    if (n != 1) return 0;       /* small file = 1 chunk */

    /* Verify the chunk hash matches direct SHA-256 */
    char direct_hash[REVFS_HASH_HEX_SIZE];
    revfs_sha256(data, strlen(data), direct_hash);
    if (strcmp(hashes[0], direct_hash) != 0) return 0;

    return 1;
}

/* ------- Test 8: Chunk + reassemble round-trip ------- */
static int test_chunk_reassemble(void)
{
    revfs_mkdir_p(TEST_DIR, 0755);

    /* Create a test file with known content */
    const char *content = "Hello, RevFS! This is a chunking round-trip test. "
                          "The file should be split into chunks and then "
                          "reassembled perfectly.";
    size_t len = strlen(content);

    int fd = revfs_file_open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    revfs_file_write_all(fd, content, len);
    revfs_file_close(fd);

    /* Chunk the file */
    char hashes[4][REVFS_HASH_HEX_SIZE];
    int n = revfs_file_chunk(TEST_FILE, hashes, 4);
    if (n < 1) return 0;

    /* Reassemble to a different file */
    ssize_t total = revfs_chunks_reassemble(TEST_OUTPUT,
                        (const char (*)[REVFS_HASH_HEX_SIZE])hashes, n);
    if (total != (ssize_t)len) return 0;

    /* Read back and compare */
    fd = revfs_file_open(TEST_OUTPUT, O_RDONLY, 0);
    if (fd < 0) return 0;
    char buf[512] = {0};
    ssize_t r = revfs_file_read_all(fd, buf, len);
    revfs_file_close(fd);
    if (r != (ssize_t)len) return 0;
    if (memcmp(buf, content, len) != 0) return 0;

    return 1;
}

/* ------- Test 9: Empty file chunking ------- */
static int test_chunk_empty_file(void)
{
    revfs_mkdir_p(TEST_DIR, 0755);

    /* Create an empty file */
    int fd = revfs_file_open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    revfs_file_close(fd);

    char hashes[4][REVFS_HASH_HEX_SIZE];
    int n = revfs_file_chunk(TEST_FILE, hashes, 4);
    if (n != 1) return 0;   /* empty file still produces 1 chunk */

    /* The hash should be SHA-256("") */
    char empty_hash[REVFS_HASH_HEX_SIZE];
    revfs_sha256("", 0, empty_hash);
    if (strcmp(hashes[0], empty_hash) != 0) return 0;

    return 1;
}

/* ------- Test 10: Chunk store path format ------- */
static int test_chunk_store_path_format(void)
{
    /* Hash starts with "ab" → path should be data/chunks/ab/<hash> */
    const char *fake_hash = "abcdef0123456789abcdef0123456789"
                            "abcdef0123456789abcdef0123456789";
    char path[512];
    if (revfs_chunk_store_path(fake_hash, path, sizeof(path)) < 0) return 0;

    /* Check it contains the expected structure */
    if (strstr(path, "chunks/ab/") == NULL) return 0;
    if (strstr(path, fake_hash) == NULL) return 0;

    return 1;
}

/* ------- Cleanup helper ------- */
static void cleanup(void)
{
    unlink(TEST_FILE);
    unlink(TEST_OUTPUT);
    rmdir(TEST_DIR);

    /* Clean up chunk store — remove test chunks.
     * We leave data/chunks/ intact since it's shared storage. */
}

/* ------- main ------- */
int main(void)
{
    printf("\n━━━ RevFS Day 3 — Chunking + SHA-256 Tests ━━━\n\n");

    RUN(test_sha256_known);
    RUN(test_sha256_hello);
    RUN(test_sha256_fd);
    RUN(test_chunk_store_load);
    RUN(test_chunk_dedup);
    RUN(test_chunk_different);
    RUN(test_file_chunk_small);
    RUN(test_chunk_reassemble);
    RUN(test_chunk_empty_file);
    RUN(test_chunk_store_path_format);

    printf("\n━━━ Results: %d / %d passed ━━━\n\n", tests_passed, tests_run);

    cleanup();

    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
