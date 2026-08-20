/*
 * RevFS — Day 7 Tests
 *
 * Exercises version restore: basic restore, version number correctness,
 * content integrity, error handling, edge cases.
 *
 * Compile separately:
 *   clang -Wall -Wextra -std=c11 -g -Iinclude -o test_restore \
 *         tests/test_restore.c src/restore.c src/version.c src/upload.c \
 *         src/download.c src/chunk.c src/file.c
 *
 * Run:
 *   ./test_restore
 */

#include "revfs.h"
#include <assert.h>
#include <dirent.h>
#include <time.h>

#define TEST_DIR        "data/test_restore_tmp"
#define TEST_SRC_FILE   TEST_DIR "/restore_test.txt"
#define TEST_OUT_FILE   TEST_DIR "/restored_output.txt"

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

/* ------- Recursive cleanup ------- */
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
    recursive_rm("data/meta/restore_test.txt");
}

/* ------- Test content ------- */
static const char *CONTENT_V1 = "Restore test version 1 — original content.";
static const char *CONTENT_V2 = "Restore test version 2 — updated content with changes.";
static const char *CONTENT_V3 = "Restore test version 3 — final revision before restore.";

/* ======================================================= */
/*  Test 1: Basic restore — restore v1 after uploading 3   */
/*          versions. Should create v4.                     */
/* ======================================================= */
static int test_restore_basic(void)
{
    /* Upload 3 versions */
    if (create_test_file(TEST_SRC_FILE, CONTENT_V1, strlen(CONTENT_V1)) < 0)
        return 0;
    if (revfs_upload(TEST_SRC_FILE) < 1) return 0;

    if (create_test_file(TEST_SRC_FILE, CONTENT_V2, strlen(CONTENT_V2)) < 0)
        return 0;
    if (revfs_upload(TEST_SRC_FILE) < 1) return 0;

    if (create_test_file(TEST_SRC_FILE, CONTENT_V3, strlen(CONTENT_V3)) < 0)
        return 0;
    if (revfs_upload(TEST_SRC_FILE) < 1) return 0;

    /* Restore v1 → should create v4 */
    int new_ver = revfs_restore("restore_test.txt", 1);
    return (new_ver == 4);
}

/* ======================================================= */
/*  Test 2: Restored version has correct metadata          */
/* ======================================================= */
static int test_restore_metadata(void)
{
    /* v4 should have the same size as v1 */
    revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
    if (!meta) return 0;

    if (revfs_meta_read("restore_test.txt", 4, meta) < 0) {
        free(meta);
        return 0;
    }

    int ok = 1;
    if (meta->version != 4) ok = 0;
    if (meta->file_size != (off_t)strlen(CONTENT_V1)) ok = 0;
    if (meta->num_chunks < 1) ok = 0;

    free(meta);
    return ok;
}

/* ======================================================= */
/*  Test 3: Restored version shares chunk hashes with      */
/*          the source version (content dedup).             */
/* ======================================================= */
static int test_restore_chunk_hashes(void)
{
    revfs_meta_t *v1 = calloc(1, sizeof(revfs_meta_t));
    revfs_meta_t *v4 = calloc(1, sizeof(revfs_meta_t));
    if (!v1 || !v4) { free(v1); free(v4); return 0; }

    if (revfs_meta_read("restore_test.txt", 1, v1) < 0 ||
        revfs_meta_read("restore_test.txt", 4, v4) < 0) {
        free(v1); free(v4);
        return 0;
    }

    int ok = 1;
    if (v1->num_chunks != v4->num_chunks) ok = 0;

    if (ok) {
        for (int i = 0; i < v1->num_chunks; i++) {
            if (strcmp(v1->chunk_hashes[i], v4->chunk_hashes[i]) != 0)
                ok = 0;
        }
    }

    free(v1);
    free(v4);
    return ok;
}

/* ======================================================= */
/*  Test 4: Download the restored version and verify       */
/*          the content matches v1.                         */
/* ======================================================= */
static int test_restore_download_verify(void)
{
    /* Download v4 (restored from v1) */
    if (revfs_download("restore_test.txt", 4, TEST_OUT_FILE) < 0)
        return 0;

    /* Read the output file and compare */
    int fd = revfs_file_open(TEST_OUT_FILE, O_RDONLY, 0);
    if (fd < 0) return 0;

    off_t sz = revfs_file_size(fd);
    if (sz != (off_t)strlen(CONTENT_V1)) {
        revfs_file_close(fd);
        return 0;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { revfs_file_close(fd); return 0; }

    ssize_t r = revfs_file_read_all(fd, buf, (size_t)sz);
    revfs_file_close(fd);
    if (r != (ssize_t)sz) { free(buf); return 0; }
    buf[sz] = '\0';

    int ok = (strcmp(buf, CONTENT_V1) == 0);
    free(buf);
    unlink(TEST_OUT_FILE);
    return ok;
}

/* ======================================================= */
/*  Test 5: Version count increments after restore         */
/* ======================================================= */
static int test_restore_version_count(void)
{
    int count = revfs_version_count("restore_test.txt");
    return (count == 4);
}

/* ======================================================= */
/*  Test 6: Restore v2 — should create v5                  */
/* ======================================================= */
static int test_restore_another_version(void)
{
    int new_ver = revfs_restore("restore_test.txt", 2);
    if (new_ver != 5) return 0;

    /* Verify v5 has v2's content size */
    revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
    if (!meta) return 0;

    if (revfs_meta_read("restore_test.txt", 5, meta) < 0) {
        free(meta);
        return 0;
    }

    int ok = (meta->file_size == (off_t)strlen(CONTENT_V2));
    free(meta);
    return ok;
}

/* ======================================================= */
/*  Test 7: Restore latest version → error (already latest)*/
/* ======================================================= */
static int test_restore_latest_error(void)
{
    /* v5 is the latest — restoring it should fail */
    int rc = revfs_restore("restore_test.txt", 5);
    return (rc == -1);
}

/* ======================================================= */
/*  Test 8: Restore with NULL filename → error             */
/* ======================================================= */
static int test_restore_null_filename(void)
{
    return (revfs_restore(NULL, 1) == -1);
}

/* ======================================================= */
/*  Test 9: Restore with invalid version (0) → error       */
/* ======================================================= */
static int test_restore_invalid_version(void)
{
    return (revfs_restore("restore_test.txt", 0) == -1);
}

/* ======================================================= */
/*  Test 10: Restore non-existent file → error             */
/* ======================================================= */
static int test_restore_nonexistent(void)
{
    return (revfs_restore("completely_fake_file.txt", 1) == -1);
}

/* ------- main ------- */
int main(void)
{
    printf("\n━━━ RevFS Day 7 — Version Restore Tests ━━━\n\n");

    /* Clean up any leftovers from previous runs */
    cleanup();

    RUN(test_restore_basic);
    RUN(test_restore_metadata);
    RUN(test_restore_chunk_hashes);
    RUN(test_restore_download_verify);
    RUN(test_restore_version_count);
    RUN(test_restore_another_version);
    RUN(test_restore_latest_error);
    RUN(test_restore_null_filename);
    RUN(test_restore_invalid_version);
    RUN(test_restore_nonexistent);

    printf("\n━━━ Results: %d / %d passed ━━━\n\n", tests_passed, tests_run);

    cleanup();

    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
