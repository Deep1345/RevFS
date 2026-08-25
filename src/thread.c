/*
 * thread.c — POSIX worker thread pool & concurrency primitives
 *
 * Implements a ring-buffer task queue with worker threads, condition
 * variables for synchronization, and a global metadata mutex for atomic
 * version allocations across concurrent requests.
 */

#include "revfs.h"

/* ------------------------------------------------------------------ */
/*  Global Metadata Mutex                                             */
/*                                                                    */
/*  Ensures atomic version allocations and metadata write operations  */
/*  when multiple threads or client connections access metadata       */
/*  simultaneously.                                                   */
/* ------------------------------------------------------------------ */
static pthread_mutex_t g_revfs_meta_mutex = PTHREAD_MUTEX_INITIALIZER;

void revfs_lock_meta(void)
{
    pthread_mutex_lock(&g_revfs_meta_mutex);
}

void revfs_unlock_meta(void)
{
    pthread_mutex_unlock(&g_revfs_meta_mutex);
}

/* ------------------------------------------------------------------ */
/*  Worker Thread Routine                                             */
/*                                                                    */
/*  Worker threads continuously wait for tasks on `notify_not_empty`, */
/*  pop them from the ring buffer, execute the callback, and signal   */
/*  `notify_idle` when all work is finished.                          */
/* ------------------------------------------------------------------ */
static void *revfs_worker_thread(void *arg)
{
    revfs_tpool_t *pool = (revfs_tpool_t *)arg;
    if (!pool) return NULL;

    while (1) {
        pthread_mutex_lock(&pool->lock);

        /* Wait for a task or shutdown */
        while (pool->queue_count == 0 && pool->shutdown == 0) {
            pthread_cond_wait(&pool->notify_not_empty, &pool->lock);
        }

        /* Check shutdown conditions:
         *   shutdown == 2: immediate stop
         *   shutdown == 1: graceful drain, stop when queue is empty
         */
        if (pool->shutdown == 2 || (pool->shutdown == 1 && pool->queue_count == 0)) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        /* Pop task from circular queue */
        revfs_task_t task = pool->queue[pool->queue_tail];
        pool->queue_tail = (pool->queue_tail + 1) % pool->queue_size;
        pool->queue_count--;
        pool->active_tasks++;

        /* Signal producer threads waiting on a full queue */
        pthread_cond_signal(&pool->notify_not_full);

        pthread_mutex_unlock(&pool->lock);

        /* Execute task outside of pool lock */
        if (task.function) {
            task.function(task.arg);
        }

        /* Task complete: update active task count and signal idle if done */
        pthread_mutex_lock(&pool->lock);
        pool->active_tasks--;
        if (pool->queue_count == 0 && pool->active_tasks == 0) {
            pthread_cond_broadcast(&pool->notify_idle);
        }
        pthread_mutex_unlock(&pool->lock);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  revfs_tpool_create                                                */
/*                                                                    */
/*  Creates and initializes a thread pool with `num_threads` workers  */
/*  and a task buffer of capacity `queue_size`.                       */
/* ------------------------------------------------------------------ */
revfs_tpool_t *revfs_tpool_create(int num_threads, int queue_size)
{
    if (num_threads <= 0) {
        num_threads = REVFS_DEFAULT_THREADS;
    }
    if (num_threads > REVFS_MAX_THREADS) {
        num_threads = REVFS_MAX_THREADS;
    }
    if (queue_size <= 0) {
        queue_size = REVFS_DEFAULT_QUEUE_SIZE;
    }
    if (queue_size > REVFS_MAX_QUEUE_SIZE) {
        queue_size = REVFS_MAX_QUEUE_SIZE;
    }

    revfs_tpool_t *pool = (revfs_tpool_t *)calloc(1, sizeof(revfs_tpool_t));
    if (!pool) {
        return NULL;
    }

    pool->num_threads  = num_threads;
    pool->queue_size   = queue_size;
    pool->queue_head   = 0;
    pool->queue_tail   = 0;
    pool->queue_count  = 0;
    pool->active_tasks = 0;
    pool->shutdown     = 0;

    pool->queue = (revfs_task_t *)calloc((size_t)queue_size, sizeof(revfs_task_t));
    if (!pool->queue) {
        free(pool);
        return NULL;
    }

    pool->threads = (pthread_t *)calloc((size_t)num_threads, sizeof(pthread_t));
    if (!pool->threads) {
        free(pool->queue);
        free(pool);
        return NULL;
    }

    if (pthread_mutex_init(&pool->lock, NULL) != 0) {
        free(pool->threads);
        free(pool->queue);
        free(pool);
        return NULL;
    }

    if (pthread_cond_init(&pool->notify_not_empty, NULL) != 0 ||
        pthread_cond_init(&pool->notify_not_full, NULL) != 0 ||
        pthread_cond_init(&pool->notify_idle, NULL) != 0) {
        pthread_mutex_destroy(&pool->lock);
        free(pool->threads);
        free(pool->queue);
        free(pool);
        return NULL;
    }

    /* Spawn worker threads */
    int threads_created = 0;
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, revfs_worker_thread, pool) != 0) {
            /* Thread creation failed: trigger immediate shutdown and join created threads */
            pool->shutdown = 2;
            pthread_cond_broadcast(&pool->notify_not_empty);
            for (int j = 0; j < threads_created; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            pthread_cond_destroy(&pool->notify_not_empty);
            pthread_cond_destroy(&pool->notify_not_full);
            pthread_cond_destroy(&pool->notify_idle);
            pthread_mutex_destroy(&pool->lock);
            free(pool->threads);
            free(pool->queue);
            free(pool);
            return NULL;
        }
        threads_created++;
    }

    return pool;
}

/* ------------------------------------------------------------------ */
/*  revfs_tpool_submit                                                */
/*                                                                    */
/*  Submits a task to the pool. If the queue is full, blocks until    */
/*  space is available. Returns 0 on success, -1 on error/shutdown.   */
/* ------------------------------------------------------------------ */
int revfs_tpool_submit(revfs_tpool_t *pool, void (*function)(void *), void *arg)
{
    if (!pool || !function) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&pool->lock);

    /* Wait for room in queue while running */
    while (pool->queue_count == pool->queue_size && pool->shutdown == 0) {
        pthread_cond_wait(&pool->notify_not_full, &pool->lock);
    }

    if (pool->shutdown != 0) {
        pthread_mutex_unlock(&pool->lock);
        errno = ESHUTDOWN;
        return -1;
    }

    /* Enqueue task */
    pool->queue[pool->queue_head].function = function;
    pool->queue[pool->queue_head].arg      = arg;
    pool->queue_head = (pool->queue_head + 1) % pool->queue_size;
    pool->queue_count++;

    /* Signal a worker thread */
    pthread_cond_signal(&pool->notify_not_empty);

    pthread_mutex_unlock(&pool->lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_tpool_try_submit                                            */
/*                                                                    */
/*  Non-blocking task submission. Returns 0 on success, -1 if full    */
/*  or in shutdown.                                                   */
/* ------------------------------------------------------------------ */
int revfs_tpool_try_submit(revfs_tpool_t *pool, void (*function)(void *), void *arg)
{
    if (!pool || !function) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&pool->lock);

    if (pool->shutdown != 0 || pool->queue_count == pool->queue_size) {
        pthread_mutex_unlock(&pool->lock);
        errno = (pool->shutdown != 0) ? ESHUTDOWN : EAGAIN;
        return -1;
    }

    pool->queue[pool->queue_head].function = function;
    pool->queue[pool->queue_head].arg      = arg;
    pool->queue_head = (pool->queue_head + 1) % pool->queue_size;
    pool->queue_count++;

    pthread_cond_signal(&pool->notify_not_empty);

    pthread_mutex_unlock(&pool->lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_tpool_wait                                                  */
/*                                                                    */
/*  Blocks until all queued and active tasks in the pool complete.    */
/* ------------------------------------------------------------------ */
int revfs_tpool_wait(revfs_tpool_t *pool)
{
    if (!pool) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&pool->lock);
    while (pool->queue_count > 0 || pool->active_tasks > 0) {
        pthread_cond_wait(&pool->notify_idle, &pool->lock);
    }
    pthread_mutex_unlock(&pool->lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_tpool_destroy                                               */
/*                                                                    */
/*  Destroys the thread pool.                                         */
/*  If `wait_for_tasks` is non-zero, drains remaining tasks first.    */
/*  Joins all threads and releases all resources.                     */
/* ------------------------------------------------------------------ */
int revfs_tpool_destroy(revfs_tpool_t *pool, int wait_for_tasks)
{
    if (!pool) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&pool->lock);
    if (pool->shutdown != 0) {
        pthread_mutex_unlock(&pool->lock);
        return -1;
    }

    pool->shutdown = wait_for_tasks ? 1 : 2;

    if (wait_for_tasks) {
        /* Wait for queued and running tasks to complete */
        while (pool->queue_count > 0 || pool->active_tasks > 0) {
            pthread_cond_broadcast(&pool->notify_not_empty);
            pthread_cond_wait(&pool->notify_idle, &pool->lock);
        }
    }

    /* Wake up all workers so they can terminate */
    pthread_cond_broadcast(&pool->notify_not_empty);
    pthread_cond_broadcast(&pool->notify_not_full);
    pthread_mutex_unlock(&pool->lock);

    /* Join worker threads */
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    /* Destroy synchronization objects and free memory */
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->notify_not_empty);
    pthread_cond_destroy(&pool->notify_not_full);
    pthread_cond_destroy(&pool->notify_idle);

    free(pool->threads);
    free(pool->queue);
    free(pool);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_tpool_active_workers                                        */
/*                                                                    */
/*  Returns the count of worker threads currently executing tasks.    */
/* ------------------------------------------------------------------ */
int revfs_tpool_active_workers(revfs_tpool_t *pool)
{
    if (!pool) return -1;
    pthread_mutex_lock(&pool->lock);
    int count = pool->active_tasks;
    pthread_mutex_unlock(&pool->lock);
    return count;
}

/* ------------------------------------------------------------------ */
/*  revfs_tpool_queue_count                                           */
/*                                                                    */
/*  Returns the count of tasks currently queued in the ring buffer.   */
/* ------------------------------------------------------------------ */
int revfs_tpool_queue_count(revfs_tpool_t *pool)
{
    if (!pool) return -1;
    pthread_mutex_lock(&pool->lock);
    int count = pool->queue_count;
    pthread_mutex_unlock(&pool->lock);
    return count;
}
