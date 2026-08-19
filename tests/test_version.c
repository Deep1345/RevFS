/*
 * RevFS — Day 6 Tests
 *
 * Exercises file versioning: version counting, history listing,
 * version enumeration, and file listing.
 *
 * Compile separately:
 *   clang -Wall -Wextra -std=c11 -g -Iinclude -o test_version \
 *         tests/test_version.c src/version.c src/upload.c src/download.c \
 *         src/chunk.c src/file.c
 *
 * Run:
 *   ./test_version
 */

#include "revfs.h"
#include <assert.h>
#include <dirent.h>
#include <time.h>

#define TEST_DIR        "data/test_version_tmp"
#define TEST_SRC_A      TEST_DIR "/src_alpha.txt"
#define TEST_SRC_B      TEST_DIR "/src_beta.txt"
#define TEST_SRC_EMPTY  TEST_DIR "/src_empty.txt"

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
    recursive_rm("data/meta/src_alpha.txt");
    recursive_rm("data/meta/src_beta.txt");
    recursive_rm("data/meta/src_empty.txt");
}

/* ======================================================= */
/*  Test 1: Version count for non-existent file = 0        */
/* ======================================================= */
static int test_version_count_no_file(void)
{
    int count = revfs_version_count("totally_nonexistent_file.txt");
    return (count == 0);
}

/* ======================================================= */
/*  Test 2: Version count after single upload = 1          */
/* ======================================================= */
static const char *CONTENT_V1 = "Alpha file version 1 content.";

static int test_version_count_one(void)
{
    if (create_test_file(TEST_SRC_A, CONTENT_V1, strlen(CONTENT_V1)) < 0)
        return 0;

    int version = revfs_upload(TEST_SRC_A);
    if (version < 1) return 0;

    int count = revfs_version_count("src_alpha.txt");
    return (count == 1);
}

/* ======================================================= */
/*  Test 3: Version count after multiple uploads           */
/* ======================================================= */
static const char *CONTENT_V2 = "Alpha file version 2 — updated content.";
static const char *CONTENT_V3 = "Alpha file version 3 — final revision.";

static int test_version_count_multiple(void)
{
    /* Upload v2 */
    if (create_test_file(TEST_SRC_A, CONTENT_V2, strlen(CONTENT_V2)) < 0)
        return 0;
    if (revfs_upload(TEST_SRC_A) < 1) return 0;

    /* Upload v3 */
    if (create_test_file(TEST_SRC_A, CONTENT_V3, strlen(CONTENT_V3)) < 0)
        return 0;
    if (revfs_upload(TEST_SRC_A) < 1) return 0;

    int count = revfs_version_count("src_alpha.txt");
    return (count == 3);
}

/* ======================================================= */
/*  Test 4: Version count with NULL → error                */
/* ======================================================= */
static int test_version_count_null(void)
{
    return (revfs_version_count(NULL) == -1);
}

/* ======================================================= */
/*  Test 5: History prints correctly (returns count > 0)   */
/* ======================================================= */
static int test_history_basic(void)
{
    /* src_alpha.txt has 3 versions from earlier tests */
    int printed = revfs_history("src_alpha.txt");
    return (printed == 3);
}

/* ======================================================= */
/*  Test 6: History for non-existent file → error          */
/* ======================================================= */
static int test_history_nonexistent(void)
{
    int rc = revfs_history("no_such_file_ever.txt");
    return (rc == -1);
}

/* ======================================================= */
/*  Test 7: History with NULL → error                      */
/* ======================================================= */
static int test_history_null(void)
{
    return (revfs_history(NULL) == -1);
}

/* ======================================================= */
/*  Test 8: Version list reads all metadata correctly      */
/* ======================================================= */
static int test_version_list(void)
{
    /* Allocate space for up to 10 versions */
    revfs_meta_t *versions = calloc(10, sizeof(revfs_meta_t));
    if (!versions) return 0;

    int count = revfs_version_list("src_alpha.txt", versions, 10);
    if (count != 3) {
        free(versions);
        return 0;
    }

    /* Verify version numbers are sequential */
    int ok = 1;
    for (int i = 0; i < count; i++) {
        if (versions[i].version != i + 1)
            ok = 0;
    }

    /* Verify sizes match the content we uploaded */
    if (versions[0].file_size != (off_t)strlen(CONTENT_V1)) ok = 0;
    if (versions[1].file_size != (off_t)strlen(CONTENT_V2)) ok = 0;
    if (versions[2].file_size != (off_t)strlen(CONTENT_V3)) ok = 0;

    free(versions);
    return ok;
}

/* ======================================================= */
/*  Test 9: List files shows all uploaded files            */
/* ======================================================= */
static const char *CONTENT_BETA = "Beta file content for listing test.";

static int test_list_files(void)
{
    /* Upload a second file */
    if (create_test_file(TEST_SRC_B, CONTENT_BETA, strlen(CONTENT_BETA)) < 0)
        return 0;
    if (revfs_upload(TEST_SRC_B) < 1) return 0;

    int count = revfs_list_files();
    /* Should have at least 2 files (src_alpha.txt and src_beta.txt) */
    return (count >= 2);
}

/* ======================================================= */
/*  Test 10: Version list with max_versions < total        */
/* ======================================================= */
static int test_version_list_limited(void)
{
    /* src_alpha.txt has 3 versions, but we only ask for 2 */
    revfs_meta_t *versions = calloc(2, sizeof(revfs_meta_t));
    if (!versions) return 0;

    int count = revfs_version_list("src_alpha.txt", versions, 2);
    int ok = (count == 2);

    /* Should get v1 and v2 */
    if (ok && versions[0].version != 1) ok = 0;
    if (ok && versions[1].version != 2) ok = 0;

    free(versions);
    return ok;
}

/* ------- main ------- */
int main(void)
{
    printf("\n━━━ RevFS Day 6 — File Versioning Tests ━━━\n\n");

    /* Clean up any leftovers from previous runs */
    cleanup();

    RUN(test_version_count_no_file);
    RUN(test_version_count_one);
    RUN(test_version_count_multiple);
    RUN(test_version_count_null);
    RUN(test_history_basic);
    RUN(test_history_nonexistent);
    RUN(test_history_null);
    RUN(test_version_list);
    RUN(test_list_files);
    RUN(test_version_list_limited);

    printf("\n━━━ Results: %d / %d passed ━━━\n\n", tests_passed, tests_run);

    cleanup();

    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
