/*
 * RevFS — Day 4 Tests
 *
 * Exercises the upload pipeline and metadata persistence layer.
 * Compile separately:
 *   clang -Wall -Wextra -std=c11 -g -Iinclude -o test_upload \
 *         tests/test_upload.c src/upload.c src/chunk.c src/file.c
 *
 * Run:
 *   ./test_upload
 */

#include "revfs.h"
#include <assert.h>
#include <dirent.h>
#include <time.h>

#define TEST_DIR     "data/test_upload_tmp"
#define TEST_FILE_A  TEST_DIR "/test_a.txt"
#define TEST_FILE_B  TEST_DIR "/test_b.txt"
#define TEST_EMPTY   TEST_DIR "/test_empty.txt"

static int tests_run    = 0;
static int tests_passed = 0;

#define RUN(name) do {                                    \
    tests_run++;                                          \
    printf("  [%d] %-44s ", tests_run, #name);            \
    if (name()) { tests_passed++; printf("✅ PASS\n"); }  \
    else        { printf("❌ FAIL\n"); }                   \
} while (0)

/* ------- Helper: create a test file with content ------- */
static int create_test_file(const char *path, const char *content)
{
    revfs_mkdir_p(TEST_DIR, 0755);
    int fd = revfs_file_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (content && strlen(content) > 0)
        revfs_file_write_all(fd, content, strlen(content));
    revfs_file_close(fd);
    return 0;
}

/* ------- Test 1: Upload a small file ------- */
static int test_upload_basic(void)
{
    const char *content = "Hello, RevFS Day 4! This is a basic upload test.";
    if (create_test_file(TEST_FILE_A, content) < 0) return 0;

    int version = revfs_upload(TEST_FILE_A);
    if (version < 1) return 0;
    if (version != 1) return 0;  /* first upload → version 1 */

    return 1;
}

/* ------- Test 2: Metadata file is created and readable ------- */
static int test_metadata_created(void)
{
    revfs_meta_t meta;
    if (revfs_meta_read("test_a.txt", 1, &meta) < 0) return 0;

    if (strcmp(meta.name, "test_a.txt") != 0) return 0;
    if (meta.version != 1) return 0;
    if (meta.num_chunks < 1) return 0;
    if (meta.file_size <= 0) return 0;
    if (meta.timestamp <= 0) return 0;

    return 1;
}

/* ------- Test 3: Upload same file twice → version increments ------- */
static int test_version_increment(void)
{
    /* test_a.txt was already uploaded as v1 in test_upload_basic */
    int version2 = revfs_upload(TEST_FILE_A);
    if (version2 != 2) return 0;

    /* Upload again → v3 */
    int version3 = revfs_upload(TEST_FILE_A);
    if (version3 != 3) return 0;

    return 1;
}

/* ------- Test 4: Metadata content matches original file ------- */
static int test_metadata_content_match(void)
{
    const char *content = "Metadata content match test data for RevFS.";
    if (create_test_file(TEST_FILE_B, content) < 0) return 0;

    int version = revfs_upload(TEST_FILE_B);
    if (version < 1) return 0;

    revfs_meta_t meta;
    if (revfs_meta_read("test_b.txt", version, &meta) < 0) return 0;

    /* File size must match */
    if (meta.file_size != (off_t)strlen(content)) return 0;

    /* Name must match */
    if (strcmp(meta.name, "test_b.txt") != 0) return 0;

    /* Must have at least 1 chunk hash */
    if (meta.num_chunks < 1) return 0;
    if (strlen(meta.chunk_hashes[0]) != 64) return 0;  /* SHA-256 = 64 hex chars */

    return 1;
}

/* ------- Test 5: Upload non-existent file → error ------- */
static int test_upload_nonexistent(void)
{
    int version = revfs_upload("/no/such/file/exists.txt");
    if (version != -1) return 0;  /* must fail */
    return 1;
}

/* ------- Test 6: Empty file upload ------- */
static int test_upload_empty_file(void)
{
    if (create_test_file(TEST_EMPTY, "") < 0) return 0;

    int version = revfs_upload(TEST_EMPTY);
    if (version < 1) return 0;

    revfs_meta_t meta;
    if (revfs_meta_read("test_empty.txt", version, &meta) < 0) return 0;

    if (meta.file_size != 0) return 0;
    if (meta.num_chunks != 1) return 0;  /* empty file → 1 chunk (empty hash) */

    return 1;
}

/* ------- Test 7: Chunk hashes in metadata exist in store ------- */
static int test_chunk_hashes_exist(void)
{
    revfs_meta_t meta;
    if (revfs_meta_read("test_b.txt", 1, &meta) < 0) return 0;

    for (int i = 0; i < meta.num_chunks; i++) {
        if (!revfs_chunk_exists(meta.chunk_hashes[i])) {
            fprintf(stderr, "    chunk %d hash not found in store\n", i);
            return 0;
        }
    }

    return 1;
}

/* ------- Test 8: Read latest version (version == -1) ------- */
static int test_read_latest_version(void)
{
    /* test_a.txt should have versions 1, 2, 3 from earlier tests */
    revfs_meta_t meta;
    if (revfs_meta_read("test_a.txt", -1, &meta) < 0) return 0;

    /* Latest should be version 3 */
    if (meta.version != 3) {
        fprintf(stderr, "    expected version 3, got %d\n", meta.version);
        return 0;
    }

    return 1;
}

/* ------- Test 9: Next version number ------- */
static int test_next_version(void)
{
    /* test_a.txt has 3 versions, next should be 4 */
    int next = revfs_meta_next_version("test_a.txt");
    if (next != 4) return 0;

    /* Never-uploaded file → next should be 1 */
    int next_new = revfs_meta_next_version("totally_new_file.txt");
    if (next_new != 1) return 0;

    return 1;
}

/* ------- Test 10: List uploaded files ------- */
static int test_list_files(void)
{
    char names[16][REVFS_MAX_FILENAME];
    int count = revfs_meta_list_files(names, 16);
    if (count < 2) return 0;  /* we uploaded test_a.txt and test_b.txt at minimum */

    /* Check that test_a.txt and test_b.txt are in the list */
    int found_a = 0, found_b = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], "test_a.txt") == 0) found_a = 1;
        if (strcmp(names[i], "test_b.txt") == 0) found_b = 1;
    }
    if (!found_a || !found_b) return 0;

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
    recursive_rm("data/meta/test_a.txt");
    recursive_rm("data/meta/test_b.txt");
    recursive_rm("data/meta/test_empty.txt");
}

/* ------- main ------- */
int main(void)
{
    printf("\n━━━ RevFS Day 4 — Upload + Metadata Tests ━━━\n\n");

    /* Clean up any leftovers from previous runs */
    cleanup();

    RUN(test_upload_basic);
    RUN(test_metadata_created);
    RUN(test_version_increment);
    RUN(test_metadata_content_match);
    RUN(test_upload_nonexistent);
    RUN(test_upload_empty_file);
    RUN(test_chunk_hashes_exist);
    RUN(test_read_latest_version);
    RUN(test_next_version);
    RUN(test_list_files);

    printf("\n━━━ Results: %d / %d passed ━━━\n\n", tests_passed, tests_run);

    cleanup();

    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
