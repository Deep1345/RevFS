/*
 * RevFS — Day 5 Tests
 *
 * Exercises the download pipeline and file reconstruction.
 * Compile separately:
 *   clang -Wall -Wextra -std=c11 -g -Iinclude -o test_download \
 *         tests/test_download.c src/download.c src/upload.c src/chunk.c src/file.c
 *
 * Run:
 *   ./test_download
 */

#include "revfs.h"
#include <assert.h>
#include <dirent.h>
#include <time.h>

#define TEST_DIR       "data/test_download_tmp"
#define TEST_SRC_A     TEST_DIR "/src_a.txt"
#define TEST_SRC_B     TEST_DIR "/src_b.txt"
#define TEST_SRC_EMPTY TEST_DIR "/src_empty.txt"
#define TEST_OUT_A     TEST_DIR "/out_a.txt"
#define TEST_OUT_B     TEST_DIR "/out_b.txt"
#define TEST_OUT_EMPTY TEST_DIR "/out_empty.txt"
#define TEST_OUT_V1    TEST_DIR "/out_v1.txt"
#define TEST_OUT_V2    TEST_DIR "/out_v2.txt"
#define TEST_OUT_LATEST TEST_DIR "/out_latest.txt"

static int tests_run    = 0;
static int tests_passed = 0;

#define RUN(name) do {                                    \
    tests_run++;                                          \
    printf("  [%d] %-44s ", tests_run, #name);            \
    if (name()) { tests_passed++; printf("✅ PASS\n"); }  \
    else        { printf("❌ FAIL\n"); }                   \
} while (0)

/* ------- Helper: create a test file with content ------- */
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

/* ------- Helper: read file content and compare ------- */
static int file_content_equals(const char *path, const char *expected, size_t expected_len)
{
    int fd = revfs_file_open(path, O_RDONLY, 0);
    if (fd < 0) return 0;

    off_t sz = revfs_file_size(fd);
    if (sz < 0 || (size_t)sz != expected_len) {
        revfs_file_close(fd);
        return 0;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        revfs_file_close(fd);
        return 0;
    }

    ssize_t r = revfs_file_read_all(fd, buf, (size_t)sz);
    revfs_file_close(fd);

    if (r < 0 || (size_t)r != expected_len) {
        free(buf);
        return 0;
    }

    int match = (memcmp(buf, expected, expected_len) == 0);
    free(buf);
    return match;
}

/* ======================================================= */
/*  Test 1: Basic download — upload then download          */
/* ======================================================= */
static const char *CONTENT_A = "Hello, RevFS Day 5! This is a basic download test.";

static int test_download_basic(void)
{
    if (create_test_file(TEST_SRC_A, CONTENT_A, strlen(CONTENT_A)) < 0)
        return 0;

    int version = revfs_upload(TEST_SRC_A);
    if (version < 1) return 0;

    int rc = revfs_download("src_a.txt", version, TEST_OUT_A);
    if (rc < 0) return 0;

    /* Verify the output file matches */
    if (!file_content_equals(TEST_OUT_A, CONTENT_A, strlen(CONTENT_A)))
        return 0;

    return 1;
}

/* ======================================================= */
/*  Test 2: Downloaded file size matches original          */
/* ======================================================= */
static int test_download_size_match(void)
{
    off_t orig_size = revfs_file_size_path(TEST_SRC_A);
    off_t down_size = revfs_file_size_path(TEST_OUT_A);

    if (orig_size < 0 || down_size < 0) return 0;
    if (orig_size != down_size) return 0;

    return 1;
}

/* ======================================================= */
/*  Test 3: Download latest version (version == -1)        */
/* ======================================================= */
static const char *CONTENT_B = "Version 2 content — updated file for RevFS download test.";

static int test_download_latest(void)
{
    /* Upload a second version of src_a.txt with different content */
    if (create_test_file(TEST_SRC_A, CONTENT_B, strlen(CONTENT_B)) < 0)
        return 0;

    int version2 = revfs_upload(TEST_SRC_A);
    if (version2 < 2) return 0;

    /* Download latest (should be version 2) */
    int rc = revfs_download("src_a.txt", -1, TEST_OUT_LATEST);
    if (rc < 0) return 0;

    /* Content should match the updated file */
    if (!file_content_equals(TEST_OUT_LATEST, CONTENT_B, strlen(CONTENT_B)))
        return 0;

    return 1;
}

/* ======================================================= */
/*  Test 4: Download specific older version                */
/* ======================================================= */
static int test_download_specific_version(void)
{
    /* Download version 1 (original content) */
    int rc = revfs_download("src_a.txt", 1, TEST_OUT_V1);
    if (rc < 0) return 0;

    if (!file_content_equals(TEST_OUT_V1, CONTENT_A, strlen(CONTENT_A)))
        return 0;

    /* Download version 2 (updated content) */
    rc = revfs_download("src_a.txt", 2, TEST_OUT_V2);
    if (rc < 0) return 0;

    if (!file_content_equals(TEST_OUT_V2, CONTENT_B, strlen(CONTENT_B)))
        return 0;

    return 1;
}

/* ======================================================= */
/*  Test 5: Download non-existent file → error             */
/* ======================================================= */
static int test_download_nonexistent(void)
{
    int rc = revfs_download("no_such_file.txt", 1, TEST_DIR "/out_nope.txt");
    if (rc != -1) return 0;  /* must fail */
    return 1;
}

/* ======================================================= */
/*  Test 6: Download non-existent version → error          */
/* ======================================================= */
static int test_download_bad_version(void)
{
    int rc = revfs_download("src_a.txt", 9999, TEST_DIR "/out_nope.txt");
    if (rc != -1) return 0;  /* must fail */
    return 1;
}

/* ======================================================= */
/*  Test 7: Download empty file                            */
/* ======================================================= */
static int test_download_empty_file(void)
{
    if (create_test_file(TEST_SRC_EMPTY, "", 0) < 0)
        return 0;

    int version = revfs_upload(TEST_SRC_EMPTY);
    if (version < 1) return 0;

    int rc = revfs_download("src_empty.txt", version, TEST_OUT_EMPTY);
    if (rc < 0) return 0;

    /* Empty file should have size 0 */
    off_t sz = revfs_file_size_path(TEST_OUT_EMPTY);
    if (sz != 0) return 0;

    return 1;
}

/* ======================================================= */
/*  Test 8: Download with NULL arguments → error           */
/* ======================================================= */
static int test_download_null_args(void)
{
    if (revfs_download(NULL, 1, "/tmp/out.txt") != -1) return 0;
    if (revfs_download("src_a.txt", 1, NULL) != -1) return 0;
    return 1;
}

/* ======================================================= */
/*  Test 9: Round-trip — upload + download yields same     */
/*          content for a multi-pattern file               */
/* ======================================================= */
static int test_roundtrip_binary_pattern(void)
{
    /* Create a file with varied byte patterns */
    const size_t pattern_size = 8192;
    unsigned char *pattern = malloc(pattern_size);
    if (!pattern) return 0;

    for (size_t i = 0; i < pattern_size; i++)
        pattern[i] = (unsigned char)(i % 256);

    const char *src_path = TEST_DIR "/src_binary.bin";
    const char *out_path = TEST_DIR "/out_binary.bin";

    if (create_test_file(src_path, (const char *)pattern, pattern_size) < 0) {
        free(pattern);
        return 0;
    }

    int version = revfs_upload(src_path);
    if (version < 1) {
        free(pattern);
        return 0;
    }

    int rc = revfs_download("src_binary.bin", version, out_path);
    if (rc < 0) {
        free(pattern);
        return 0;
    }

    int match = file_content_equals(out_path, (const char *)pattern, pattern_size);
    free(pattern);
    return match;
}

/* ======================================================= */
/*  Test 10: Download doesn't leave partial file on error  */
/* ======================================================= */
static int test_download_no_partial_on_error(void)
{
    const char *bad_out = TEST_DIR "/out_partial_test.txt";

    /* Attempt to download a non-existent file */
    revfs_download("definitely_not_here.txt", 1, bad_out);

    /* The output file should NOT exist */
    if (revfs_file_exists(bad_out)) return 0;

    return 1;
}

/* ------- Cleanup helper ------- */
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
    /* Clean up test files */
    recursive_rm(TEST_DIR);

    /* Clean up metadata created by tests */
    recursive_rm("data/meta/src_a.txt");
    recursive_rm("data/meta/src_b.txt");
    recursive_rm("data/meta/src_empty.txt");
    recursive_rm("data/meta/src_binary.bin");
}

/* ------- main ------- */
int main(void)
{
    printf("\n━━━ RevFS Day 5 — Download + Reconstruction Tests ━━━\n\n");

    /* Clean up any leftovers from previous runs */
    cleanup();

    RUN(test_download_basic);
    RUN(test_download_size_match);
    RUN(test_download_latest);
    RUN(test_download_specific_version);
    RUN(test_download_nonexistent);
    RUN(test_download_bad_version);
    RUN(test_download_empty_file);
    RUN(test_download_null_args);
    RUN(test_roundtrip_binary_pattern);
    RUN(test_download_no_partial_on_error);

    printf("\n━━━ Results: %d / %d passed ━━━\n\n", tests_passed, tests_run);

    cleanup();

    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
