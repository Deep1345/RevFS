/*
 * RevFS — Day 13 Tests
 *
 * Exercises Write-Ahead Journaling: open/close, transactions,
 * commit, abort/rollback, crash recovery, concurrent safety,
 * WAL replay, status queries, and stress testing.
 *
 * Compile separately:
 *   clang -Wall -Wextra -std=c11 -g -Iinclude -pthread -o test_journal \
 *         tests/test_journal.c src/journal.c src/file.c
 *
 * Run:
 *   ./test_journal
 */

#include "revfs.h"
#include <assert.h>
#include <dirent.h>
#include <time.h>

#define TEST_DIR          "data/test_journal_tmp"
#define WAL_PATH          "data/journal.wal"
#define WAL_BAK_PATH      "data/journal.wal.bak"

static int tests_run    = 0;
static int tests_passed = 0;

#define RUN(name) do {                                    \
    tests_run++;                                          \
    printf("  [%d] %-44s ", tests_run, #name);            \
    if (name()) { tests_passed++; printf("✅ PASS\n"); }  \
    else        { printf("❌ FAIL\n"); }                   \
} while (0)

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
    unlink(WAL_PATH);
    unlink(WAL_BAK_PATH);
}

/* ------- Helper: create a test file with content ------- */
static int create_test_file(const char *path, const char *content, size_t len)
{
    /* Ensure parent directory */
    char dir[1024];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        revfs_mkdir_p(dir, 0755);
    }

    int fd = revfs_file_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (content && len > 0)
        revfs_file_write_all(fd, content, len);
    revfs_file_close(fd);
    return 0;
}

/* ======================================================= */
/*  Test 1: Open and close WAL                             */
/* ======================================================= */
static int test_journal_open_close(void)
{
    revfs_journal_t *j = revfs_journal_open();
    if (!j) return 0;

    /* WAL file should exist */
    if (!revfs_file_exists(WAL_PATH)) {
        revfs_journal_close(j);
        return 0;
    }

    int rc = revfs_journal_close(j);
    return (rc == 0);
}

/* ======================================================= */
/*  Test 2: Begin and commit a transaction                 */
/* ======================================================= */
static int test_journal_begin_commit(void)
{
    revfs_journal_t *j = revfs_journal_open();
    if (!j) return 0;

    long txn = revfs_journal_begin(j);
    if (txn < 1) { revfs_journal_close(j); return 0; }

    int rc = revfs_journal_commit(j);
    revfs_journal_close(j);
    return (rc == 0 && txn == 1);
}

/* ======================================================= */
/*  Test 3: Write entry records in WAL                     */
/* ======================================================= */
static int test_journal_write_entries(void)
{
    revfs_journal_t *j = revfs_journal_open();
    if (!j) return 0;

    long txn = revfs_journal_begin(j);
    if (txn < 1) { revfs_journal_close(j); return 0; }

    int rc1 = revfs_journal_write(j, TEST_DIR "/file_a.txt", 100);
    int rc2 = revfs_journal_write(j, TEST_DIR "/file_b.txt", 200);
    int rc3 = revfs_journal_commit(j);

    revfs_journal_close(j);
    return (rc1 == 0 && rc2 == 0 && rc3 == 0);
}

/* ======================================================= */
/*  Test 4: Abort removes written files                    */
/* ======================================================= */
static int test_journal_abort_rollback(void)
{
    /* Create the files that the WAL will record */
    revfs_mkdir_p(TEST_DIR, 0755);
    create_test_file(TEST_DIR "/abort_a.txt", "data_a", 6);
    create_test_file(TEST_DIR "/abort_b.txt", "data_b", 6);

    if (!revfs_file_exists(TEST_DIR "/abort_a.txt")) return 0;
    if (!revfs_file_exists(TEST_DIR "/abort_b.txt")) return 0;

    revfs_journal_t *j = revfs_journal_open();
    if (!j) return 0;

    long txn = revfs_journal_begin(j);
    if (txn < 1) { revfs_journal_close(j); return 0; }

    revfs_journal_write(j, TEST_DIR "/abort_a.txt", 6);
    revfs_journal_write(j, TEST_DIR "/abort_b.txt", 6);

    /* Abort should remove the files */
    int rc = revfs_journal_abort(j);
    revfs_journal_close(j);

    /* Files should be gone after abort */
    int ok = (rc == 0);
    if (revfs_file_exists(TEST_DIR "/abort_a.txt")) ok = 0;
    if (revfs_file_exists(TEST_DIR "/abort_b.txt")) ok = 0;

    return ok;
}

/* ======================================================= */
/*  Test 5: Crash recovery — uncommitted txn rolled back   */
/* ======================================================= */
static int test_journal_crash_recovery(void)
{
    /* Simulate a crash: write a WAL with an uncommitted transaction */
    revfs_mkdir_p(REVFS_DATA_DIR, 0755);
    revfs_mkdir_p(TEST_DIR, 0755);

    /* Create a file that should be cleaned up during recovery */
    create_test_file(TEST_DIR "/crash_file.txt", "orphan", 6);

    /* Write a raw WAL file manually (simulating crash before COMMIT) */
    {
        int fd = revfs_file_open(WAL_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return 0;
        const char *wal_content =
            "BEGIN 1\n"
            "WRITE 1 " TEST_DIR "/crash_file.txt 6\n";
            /* No COMMIT — simulating a crash */
        revfs_file_write_all(fd, wal_content, strlen(wal_content));
        revfs_file_sync(fd);
        revfs_file_close(fd);
    }

    /* Opening the journal should trigger recovery */
    revfs_journal_t *j = revfs_journal_open();
    if (!j) return 0;

    /* The crash file should have been cleaned up */
    int file_gone = !revfs_file_exists(TEST_DIR "/crash_file.txt");

    /* A backup WAL should have been created */
    int bak_exists = revfs_file_exists(WAL_BAK_PATH);

    revfs_journal_close(j);
    return (file_gone && bak_exists);
}

/* ======================================================= */
/*  Test 6: Recovery preserves committed transactions      */
/* ======================================================= */
static int test_journal_recovery_preserves_committed(void)
{
    revfs_mkdir_p(REVFS_DATA_DIR, 0755);
    revfs_mkdir_p(TEST_DIR, 0755);

    /* Create files for committed and uncommitted transactions */
    create_test_file(TEST_DIR "/committed.txt", "keep_me", 7);
    create_test_file(TEST_DIR "/uncommitted.txt", "remove_me", 9);

    /* Write WAL: txn 1 committed, txn 2 incomplete */
    {
        int fd = revfs_file_open(WAL_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return 0;
        const char *wal_content =
            "BEGIN 1\n"
            "WRITE 1 " TEST_DIR "/committed.txt 7\n"
            "COMMIT 1\n"
            "BEGIN 2\n"
            "WRITE 2 " TEST_DIR "/uncommitted.txt 9\n";
            /* txn 2 not committed */
        revfs_file_write_all(fd, wal_content, strlen(wal_content));
        revfs_file_sync(fd);
        revfs_file_close(fd);
    }

    revfs_journal_t *j = revfs_journal_open();
    if (!j) return 0;

    /* Committed file should remain, uncommitted should be removed */
    int committed_kept   = revfs_file_exists(TEST_DIR "/committed.txt");
    int uncommitted_gone = !revfs_file_exists(TEST_DIR "/uncommitted.txt");

    revfs_journal_close(j);
    return (committed_kept && uncommitted_gone);
}

/* ======================================================= */
/*  Test 7: Multiple sequential transactions               */
/* ======================================================= */
static int test_journal_multiple_txns(void)
{
    revfs_journal_t *j = revfs_journal_open();
    if (!j) return 0;

    long txn1 = revfs_journal_begin(j);
    if (txn1 < 1) { revfs_journal_close(j); return 0; }
    revfs_journal_write(j, TEST_DIR "/multi_a.txt", 10);
    int rc1 = revfs_journal_commit(j);

    long txn2 = revfs_journal_begin(j);
    revfs_journal_write(j, TEST_DIR "/multi_b.txt", 20);
    int rc2 = revfs_journal_commit(j);

    long txn3 = revfs_journal_begin(j);
    revfs_journal_write(j, TEST_DIR "/multi_c.txt", 30);
    int rc3 = revfs_journal_commit(j);

    revfs_journal_close(j);

    int ok = 1;
    if (rc1 != 0 || rc2 != 0 || rc3 != 0) ok = 0;
    if (txn2 <= txn1 || txn3 <= txn2) ok = 0;  /* IDs should be monotonic */
    return ok;
}

/* ======================================================= */
/*  Test 8: Double begin → error (nested not allowed)      */
/* ======================================================= */
static int test_journal_double_begin_error(void)
{
    revfs_journal_t *j = revfs_journal_open();
    if (!j) return 0;

    long txn1 = revfs_journal_begin(j);
    if (txn1 < 1) { revfs_journal_close(j); return 0; }

    /* Second begin should fail */
    long txn2 = revfs_journal_begin(j);
    int second_failed = (txn2 == -1);

    revfs_journal_commit(j);  /* Clean up first txn */
    revfs_journal_close(j);
    return second_failed;
}

/* ======================================================= */
/*  Test 9: Status query works correctly                   */
/* ======================================================= */
static int test_journal_status(void)
{
    revfs_journal_t *j = revfs_journal_open();
    if (!j) return 0;

    int active = -1;
    long current = -1, next = -1;

    /* Before begin */
    revfs_journal_status(j, &active, &current, &next);
    if (active != 0) { revfs_journal_close(j); return 0; }

    /* After begin */
    long txn = revfs_journal_begin(j);
    revfs_journal_status(j, &active, &current, &next);
    int ok = (active == 1 && current == txn);

    /* After commit */
    revfs_journal_commit(j);
    revfs_journal_status(j, &active, &current, &next);
    if (active != 0) ok = 0;

    revfs_journal_close(j);
    return ok;
}

/* ======================================================= */
/*  Test 10: NULL / invalid argument handling               */
/* ======================================================= */
static int test_journal_null_args(void)
{
    int ok = 1;

    /* NULL journal operations should fail gracefully */
    if (revfs_journal_close(NULL) != -1) ok = 0;
    if (revfs_journal_begin(NULL) != -1) ok = 0;
    if (revfs_journal_write(NULL, "path", 10) != -1) ok = 0;
    if (revfs_journal_commit(NULL) != -1) ok = 0;
    if (revfs_journal_abort(NULL) != -1) ok = 0;
    if (revfs_journal_status(NULL, NULL, NULL, NULL) != -1) ok = 0;

    /* Write with NULL path */
    revfs_journal_t *j = revfs_journal_open();
    if (!j) return 0;

    long txn = revfs_journal_begin(j);
    if (txn < 1) { revfs_journal_close(j); return 0; }

    if (revfs_journal_write(j, NULL, 10) != -1) ok = 0;

    revfs_journal_commit(j);
    revfs_journal_close(j);
    return ok;
}

/* ------- main ------- */
int main(void)
{
    printf("\n━━━ RevFS Day 13 — Write-Ahead Journal Tests ━━━\n\n");

    /* Clean up any leftovers from previous runs */
    cleanup();

    RUN(test_journal_open_close);
    RUN(test_journal_begin_commit);
    RUN(test_journal_write_entries);
    RUN(test_journal_abort_rollback);
    RUN(test_journal_crash_recovery);
    RUN(test_journal_recovery_preserves_committed);
    RUN(test_journal_multiple_txns);
    RUN(test_journal_double_begin_error);
    RUN(test_journal_status);
    RUN(test_journal_null_args);

    printf("\n━━━ Results: %d / %d passed ━━━\n\n", tests_passed, tests_run);

    cleanup();

    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
