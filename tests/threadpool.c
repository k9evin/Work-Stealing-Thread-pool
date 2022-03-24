/**
 * threadpool.c
 *
 * A work-stealing, fork-join thread pool.
 */
#include "threadpool.h"

#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>

#include "list.h"

struct thread_pool {
    struct list global_queue;          // the global queue list
    struct list worker_list;           // the worker list
    int nthreads;                      // number of threads
    int njobs;                         // number of jobs
    pthread_barrier_t worker_barrier;  // The barrier for worker
    pthread_mutex_t pool_mutex;        // the mutex lock for acquiring pool data
    pthread_mutex_t worker_mutex;  // the mutex lock for acquiring worker data
    pthread_cond_t pool_cond;  // conditional variable sends signal to worker
    int destroy;               // 0 = false, 1 = true
};

struct future {
    fork_join_task_t task;     // fork join task
    void *args;                // input data
    void *result;              // the returned result
    int status;                // 2 = UNSCHEDULE, 1 = EXECUTING, 0 = FINISHED
    sem_t completed;           // semaphore for ordering
    struct list_elem elem;     // list element
    struct thread_pool *pool;  // pointer to thread pool
};

struct worker {
    struct thread_pool *pool;     // the pointer for thread pool
    struct list worker_queue;     // the local aka worker queue list
    struct list_elem elem;        // A list of element
    pthread_t threadID;           // the id of pthread
    pthread_mutex_t local_mutex;  // the mutex lock for acquiring local data
};

static __thread struct worker *current_worker;


/* Create a new thread pool with no more than n threads. */
struct thread_pool *thread_pool_new(int nthreads) {
    struct thread_pool *pool = malloc(sizeof(struct thread_pool));

    list_init(&pool->global_queue);
    list_init(&pool->worker_list);

    pthread_mutex_init(&pool->pool_mutex, NULL);
    pthread_mutex_init(&pool->worker_mutex, NULL);
    pthread_cond_init(&pool->pool_cond, NULL);

    pool->destroy = 0;
    pool->njobs = 0;
    pool->nthreads = nthreads;

    pthread_mutex_lock(&pool->pool_mutex);
    struct worker *worker;
    for (int i = 0; i < nthreads; i++) {
        worker = malloc(sizeof(struct worker));
        pthread_mutex_init(&worker->local_mutex, NULL);
        list_init(&worker->worker_queue);
        list_push_front(&pool->worker_list, &worker->elem);
        pthread_create(&worker->threadID, NULL, start_routine, pool);
    }
    current_worker = NULL;

    pthread_mutex_unlock(&pool->pool_mutex);
    // free(worker);
    return pool;
}

/*
 * Shutdown this thread pool in an orderly fashion.
 * Tasks that have been submitted but not executed may or
 * may not be executed.
 *
 * Deallocate the thread pool object before returning.
 */
void thread_pool_shutdown_and_destroy(struct thread_pool *pool) {
    pthread_mutex_lock(&pool->pool_mutex);
    pool->destroy = 1;
    pthread_cond_broadcast(&pool->pool_cond);
    pthread_mutex_unlock(&pool->pool_mutex);

    for (struct list_elem *element = list_begin(&pool->worker_list);
         element != list_end(&pool->worker_list);
         element = list_next(element)) {
        pthread_join(list_entry(element, struct worker, elem)->threadID, NULL);
    }
    for (struct list_elem *element = list_begin(&pool->worker_list);
         element != list_end(&pool->worker_list);) {
        struct worker *worker = list_entry(element, struct worker, elem);
        element = list_remove(element);
        free(worker);
        // element = list_next(element);
    }

    free(pool);
}

/*
 * Submit a fork join task to the thread pool and return a
 * future.  The returned future can be used in future_get()
 * to obtain the result.
 * 'pool' - the pool to which to submit
 * 'task' - the task to be submitted.
 * 'data' - data to be passed to the task's function
 *
 * Returns a future representing this computation.
 */
struct future *thread_pool_submit(struct thread_pool *pool,
                                  fork_join_task_t task, void *data) {
}

/* Make sure that the thread pool has completed the execution
 * of the fork join task this future represents.
 *
 * 2 = UNSCHEDULE, 1 = EXECUTING, 0 = FINISHED
 *
 * Returns the value returned by this task.
 */
void *future_get(struct future *future) {
}

/* Deallocate this future.  Must be called after future_get() */
void future_free(struct future *future) { free(future); }
