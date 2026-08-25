/*
 * RevFS — Versioned Distributed File Storage System
 *
 * Day 13: Write-Ahead Journaling + Crash Recovery
 *
 * This module provides a Write-Ahead Log (WAL) that records all
 * intended file operations BEFORE they happen.  If RevFS crashes
 * mid-operation, the WAL is replayed on next startup to either
 * finish committed transactions or roll back incomplete ones.
 *
 * WAL record format (line-oriented, text, one record per line):
 *
 *   BEGIN <txn_id>
 *   WRITE <txn_id> <target_path> <size>
 *   COMMIT <txn_id>
 *   ABORT <txn_id>
 *
 * Recovery strategy:
 *   - Scan the WAL from top to bottom
 *   - Any transaction with a COMMIT record → no action needed (already done)
 *   - Any transaction with only BEGIN (no COMMIT/ABORT) → rollback
 *     (delete any partial files listed in WRITE entries)
 *   - ABORT records → same as rollback (clean up WRITE targets)
 *
 * The WAL lives at: data/journal.wal
 * A backup is kept at: data/journal.wal.bak (previous WAL after recovery)
 *
 * Thread-safety: all WAL operations are protected by a pthread mutex.
 */

#include "revfs.h"
#include <time.h>
#include <dirent.h>

/* ------------------------------------------------------------------ */
/*  Internal state                                                     */
/* ------------------------------------------------------------------ */

#define REVFS_WAL_PATH      REVFS_DATA_DIR "/journal.wal"
#define REVFS_WAL_BAK_PATH  REVFS_DATA_DIR "/journal.wal.bak"
#define REVFS_WAL_MAX_LINE  2048
#define REVFS_WAL_MAX_TXNS  64
#define REVFS_WAL_MAX_WRITES 1024
#define REVFS_WAL_TXN_MAX_WRITES 16

/* Per-transaction tracking during recovery */
typedef struct {
    long   txn_id;
    int    committed;           /* 1 if COMMIT seen */
    int    aborted;             /* 1 if ABORT seen */
    int    num_writes;
    char   write_paths[REVFS_WAL_TXN_MAX_WRITES][REVFS_MAX_PATH];
} wal_txn_t;

/* ------------------------------------------------------------------ */
/*  revfs_journal_open                                                 */
/*                                                                     */
/*  Opens (or creates) the WAL file.  If an existing WAL contains      */
/*  uncommitted transactions, runs recovery automatically.             */
/*                                                                     */
/*  Returns a valid revfs_journal_t pointer on success, NULL on error. */
/* ------------------------------------------------------------------ */
revfs_journal_t *revfs_journal_open(void)
{
    /* Ensure data directory exists */
    revfs_mkdir_p(REVFS_DATA_DIR, 0755);

    revfs_journal_t *j = calloc(1, sizeof(revfs_journal_t));
    if (!j) return NULL;

    pthread_mutex_init(&j->lock, NULL);
    j->next_txn_id = 1;
    j->active_txn  = 0;

    /* If a WAL file already exists, run recovery */
    if (revfs_file_exists(REVFS_WAL_PATH)) {
        int recovered = revfs_journal_recover(j);
        if (recovered > 0) {
            fprintf(stderr, "revfs: journal: recovered %d incomplete transaction(s)\n",
                    recovered);
        }
        /* Backup old WAL and start fresh */
        rename(REVFS_WAL_PATH, REVFS_WAL_BAK_PATH);
    }

    /* Open a fresh WAL file */
    j->fd = revfs_file_open(REVFS_WAL_PATH,
                             O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (j->fd < 0) {
        free(j);
        return NULL;
    }

    return j;
}

/* ------------------------------------------------------------------ */
/*  revfs_journal_close                                                */
/*                                                                     */
/*  Syncs and closes the WAL file.  Any active (uncommitted)           */
/*  transaction is aborted first.                                      */
/*                                                                     */
/*  Returns 0 on success, -1 on error.                                */
/* ------------------------------------------------------------------ */
int revfs_journal_close(revfs_journal_t *j)
{
    if (!j) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&j->lock);

    /* Abort any active transaction */
    if (j->active_txn) {
        char line[REVFS_WAL_MAX_LINE];
        int len = snprintf(line, sizeof(line), "ABORT %ld\n", j->current_txn_id);
        if (len > 0)
            revfs_file_write_all(j->fd, line, (size_t)len);
        j->active_txn = 0;
    }

    if (j->fd >= 0) {
        revfs_file_sync(j->fd);
        revfs_file_close(j->fd);
        j->fd = -1;
    }

    pthread_mutex_unlock(&j->lock);
    pthread_mutex_destroy(&j->lock);
    free(j);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_journal_begin                                                */
/*                                                                     */
/*  Starts a new transaction.  Writes a BEGIN record to the WAL.       */
/*                                                                     */
/*  Returns the transaction ID on success, -1 on error.               */
/* ------------------------------------------------------------------ */
long revfs_journal_begin(revfs_journal_t *j)
{
    if (!j) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&j->lock);

    if (j->active_txn) {
        fprintf(stderr, "revfs: journal: transaction %ld already active\n",
                j->current_txn_id);
        pthread_mutex_unlock(&j->lock);
        errno = EBUSY;
        return -1;
    }

    long txn_id = j->next_txn_id++;
    j->current_txn_id = txn_id;
    j->active_txn = 1;
    j->num_writes = 0;

    char line[REVFS_WAL_MAX_LINE];
    int len = snprintf(line, sizeof(line), "BEGIN %ld\n", txn_id);
    if (len < 0 || revfs_file_write_all(j->fd, line, (size_t)len) < 0) {
        j->active_txn = 0;
        pthread_mutex_unlock(&j->lock);
        return -1;
    }
    revfs_file_sync(j->fd);

    pthread_mutex_unlock(&j->lock);
    return txn_id;
}

/* ------------------------------------------------------------------ */
/*  revfs_journal_write                                                */
/*                                                                     */
/*  Records an intended write operation in the WAL within the current  */
/*  active transaction.  This must be called BEFORE the actual write.  */
/*                                                                     */
/*  `target_path` — path of the file that will be written              */
/*  `size`        — number of bytes that will be written               */
/*                                                                     */
/*  Returns 0 on success, -1 on error.                                */
/* ------------------------------------------------------------------ */
int revfs_journal_write(revfs_journal_t *j, const char *target_path,
                         size_t size)
{
    if (!j || !target_path) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&j->lock);

    if (!j->active_txn) {
        fprintf(stderr, "revfs: journal: no active transaction\n");
        pthread_mutex_unlock(&j->lock);
        errno = EINVAL;
        return -1;
    }

    if (j->num_writes >= REVFS_JOURNAL_MAX_WRITES) {
        fprintf(stderr, "revfs: journal: too many writes in transaction\n");
        pthread_mutex_unlock(&j->lock);
        errno = EOVERFLOW;
        return -1;
    }

    /* Record the write target for rollback purposes */
    strncpy(j->write_paths[j->num_writes], target_path, REVFS_MAX_PATH - 1);
    j->write_paths[j->num_writes][REVFS_MAX_PATH - 1] = '\0';
    j->num_writes++;

    char line[REVFS_WAL_MAX_LINE];
    int len = snprintf(line, sizeof(line), "WRITE %ld %s %zu\n",
                       j->current_txn_id, target_path, size);
    if (len < 0 || revfs_file_write_all(j->fd, line, (size_t)len) < 0) {
        pthread_mutex_unlock(&j->lock);
        return -1;
    }
    revfs_file_sync(j->fd);

    pthread_mutex_unlock(&j->lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_journal_commit                                               */
/*                                                                     */
/*  Marks the current transaction as committed.  After this record     */
/*  is fsynced, the transaction's writes are considered durable.       */
/*                                                                     */
/*  Returns 0 on success, -1 on error.                                */
/* ------------------------------------------------------------------ */
int revfs_journal_commit(revfs_journal_t *j)
{
    if (!j) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&j->lock);

    if (!j->active_txn) {
        fprintf(stderr, "revfs: journal: no active transaction to commit\n");
        pthread_mutex_unlock(&j->lock);
        errno = EINVAL;
        return -1;
    }

    char line[REVFS_WAL_MAX_LINE];
    int len = snprintf(line, sizeof(line), "COMMIT %ld\n", j->current_txn_id);
    if (len < 0 || revfs_file_write_all(j->fd, line, (size_t)len) < 0) {
        pthread_mutex_unlock(&j->lock);
        return -1;
    }
    revfs_file_sync(j->fd);

    j->active_txn = 0;
    j->num_writes = 0;

    pthread_mutex_unlock(&j->lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_journal_abort                                                */
/*                                                                     */
/*  Aborts the current transaction.  Writes an ABORT record and        */
/*  removes any files that were created by WRITE entries.              */
/*                                                                     */
/*  Returns 0 on success, -1 on error.                                */
/* ------------------------------------------------------------------ */
int revfs_journal_abort(revfs_journal_t *j)
{
    if (!j) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&j->lock);

    if (!j->active_txn) {
        fprintf(stderr, "revfs: journal: no active transaction to abort\n");
        pthread_mutex_unlock(&j->lock);
        errno = EINVAL;
        return -1;
    }

    /* Write ABORT record */
    char line[REVFS_WAL_MAX_LINE];
    int len = snprintf(line, sizeof(line), "ABORT %ld\n", j->current_txn_id);
    if (len > 0)
        revfs_file_write_all(j->fd, line, (size_t)len);
    revfs_file_sync(j->fd);

    /* Roll back: remove files created by WRITE entries */
    for (int i = 0; i < j->num_writes; i++) {
        if (revfs_file_exists(j->write_paths[i])) {
            unlink(j->write_paths[i]);
        }
    }

    j->active_txn = 0;
    j->num_writes = 0;

    pthread_mutex_unlock(&j->lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_journal_recover                                              */
/*                                                                     */
/*  Scans an existing WAL file and recovers from any incomplete        */
/*  transactions:                                                      */
/*    - Committed transactions → no action (already durable)           */
/*    - Incomplete transactions (BEGIN without COMMIT) → rollback      */
/*      (delete files listed in WRITE entries)                         */
/*    - Aborted transactions → rollback (same cleanup)                 */
/*                                                                     */
/*  Returns the number of rolled-back transactions on success,         */
/*  or -1 on error.                                                    */
/* ------------------------------------------------------------------ */
int revfs_journal_recover(revfs_journal_t *j)
{
    if (!j) {
        errno = EINVAL;
        return -1;
    }

    int fd = revfs_file_open(REVFS_WAL_PATH, O_RDONLY, 0);
    if (fd < 0)
        return 0;  /* No WAL to recover */

    off_t sz = revfs_file_size(fd);
    if (sz <= 0) {
        revfs_file_close(fd);
        return 0;
    }

    /* Read entire WAL into memory */
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        revfs_file_close(fd);
        return -1;
    }

    ssize_t r = revfs_file_read_all(fd, buf, (size_t)sz);
    revfs_file_close(fd);
    if (r < 0) {
        free(buf);
        return -1;
    }
    buf[sz] = '\0';

    /* Parse WAL records into transactions (heap-allocated — each wal_txn_t is ~16 KB) */
    wal_txn_t *txns = calloc(REVFS_WAL_MAX_TXNS, sizeof(wal_txn_t));
    if (!txns) {
        free(buf);
        return -1;
    }
    int num_txns = 0;

    /* Helper: find transaction entry by ID */
    /* (defined as inline lookup to avoid GNU statement expressions) */
    long max_txn_id = 0;
    char *line = buf;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';

        long txn_id = 0;

        if (strncmp(line, "BEGIN ", 6) == 0) {
            txn_id = atol(line + 6);
            if (txn_id > 0 && num_txns < REVFS_WAL_MAX_TXNS) {
                txns[num_txns].txn_id = txn_id;
                num_txns++;
            }
            if (txn_id > max_txn_id) max_txn_id = txn_id;
        } else if (strncmp(line, "WRITE ", 6) == 0) {
            /* Format: WRITE <txn_id> <path> <size> */
            char path[REVFS_MAX_PATH];
            size_t wr_size;
            if (sscanf(line + 6, "%ld %1023s %zu", &txn_id, path, &wr_size) >= 2) {
                wal_txn_t *t = NULL;
                for (int ti = 0; ti < num_txns; ti++) {
                    if (txns[ti].txn_id == txn_id) { t = &txns[ti]; break; }
                }
                if (t && t->num_writes < REVFS_WAL_TXN_MAX_WRITES) {
                    strncpy(t->write_paths[t->num_writes], path, REVFS_MAX_PATH - 1);
                    t->num_writes++;
                }
            }
        } else if (strncmp(line, "COMMIT ", 7) == 0) {
            txn_id = atol(line + 7);
            wal_txn_t *t = NULL;
            for (int ti = 0; ti < num_txns; ti++) {
                if (txns[ti].txn_id == txn_id) { t = &txns[ti]; break; }
            }
            if (t) t->committed = 1;
        } else if (strncmp(line, "ABORT ", 6) == 0) {
            txn_id = atol(line + 6);
            wal_txn_t *t = NULL;
            for (int ti = 0; ti < num_txns; ti++) {
                if (txns[ti].txn_id == txn_id) { t = &txns[ti]; break; }
            }
            if (t) t->aborted = 1;
        }

        line = eol ? eol + 1 : NULL;
    }

    /* Set next txn_id past any existing ones */
    j->next_txn_id = max_txn_id + 1;

    /* Roll back incomplete/aborted transactions */
    int rolled_back = 0;
    for (int i = 0; i < num_txns; i++) {
        if (!txns[i].committed) {
            /* Transaction was not committed — clean up write targets */
            for (int w = 0; w < txns[i].num_writes; w++) {
                if (revfs_file_exists(txns[i].write_paths[w])) {
                    unlink(txns[i].write_paths[w]);
                }
            }
            rolled_back++;
        }
    }

    free(txns);
    free(buf);
    return rolled_back;
}

/* ------------------------------------------------------------------ */
/*  revfs_journal_status                                               */
/*                                                                     */
/*  Returns the current journal status: active transaction info,       */
/*  total transactions processed, etc.                                 */
/*                                                                     */
/*  Returns 0 on success, -1 on error.                                */
/* ------------------------------------------------------------------ */
int revfs_journal_status(const revfs_journal_t *j, int *active_txn_out,
                          long *current_txn_id_out, long *next_txn_id_out)
{
    if (!j) {
        errno = EINVAL;
        return -1;
    }

    if (active_txn_out)     *active_txn_out     = j->active_txn;
    if (current_txn_id_out) *current_txn_id_out = j->current_txn_id;
    if (next_txn_id_out)    *next_txn_id_out    = j->next_txn_id;

    return 0;
}
