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
    pthread_mutex_t pool_mutex;        // the mutex lock for acquiring pool data
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

static struct list_elem *work_steal(struct thread_pool *pool);

static void *start_routine(void *args) {
    struct thread_pool *pool = (struct thread_pool *)args;

    for (struct list_elem *e = list_begin(&pool->worker_list);
         e != list_end(&pool->worker_list);) {
        struct worker *worker = list_entry(e, struct worker, elem);
        if (worker->threadID == pthread_self()) {
            worker->pool = pool;
            current_worker = worker;
            break;
        } else {
            e = list_next(e);
        }
    }

    for (;;) {
        pthread_mutex_lock(&pool->pool_mutex);
        while (list_empty(&pool->global_queue) && pool->njobs == 0 &&
               pool->destroy == 0) {
            pthread_cond_wait(&pool->pool_cond, &pool->pool_mutex);
        }

        if (pool->destroy) {
            pthread_mutex_unlock(&pool->pool_mutex);
            pthread_exit(NULL);
        }

        struct list_elem *l_elem = NULL;

        //  if the global queue is also empty, then current worker needs to
        //  steal work from other workers' queue
        if (list_empty(&pool->global_queue)) {
            l_elem = work_steal(pool);
        } else {
            l_elem = list_pop_front(&pool->global_queue);
        }

        if (l_elem != NULL) {
            struct future *future = list_entry(l_elem, struct future, elem);
            future->status = 1;
            pool->njobs--;
            pthread_mutex_unlock(&pool->pool_mutex);

            future->result = (future->task)(future->pool, future->args);
            future->status = 0;

            sem_post(&future->completed);
        } else {
            if (pool->destroy == 0) {
                pthread_cond_wait(&pool->pool_cond, &pool->pool_mutex);
            }
            pthread_mutex_unlock(&pool->pool_mutex);
        }
    }
    return NULL;
}

/*Supports work stealing in the thread pool.*/
static struct list_elem *work_steal(struct thread_pool *pool) {
    // The element pointer that will iterate through the list
    struct list_elem *element;

    // Iterate through the entire list
    for (element = list_begin(&pool->worker_list);
         element != list_end(&pool->worker_list);) {
        struct worker *new_worker = list_entry(element, struct worker, elem);
        pthread_mutex_lock(&new_worker->local_mutex);
        // If the list of worker queue is not empty
        if (!list_empty(&new_worker->worker_queue)) {
            // The element is removed from the back of the list
            struct list_elem *new_elem =
                list_pop_back(&new_worker->worker_queue);

            pthread_mutex_unlock(&new_worker->local_mutex);
            return new_elem;
        } else {
            pthread_mutex_unlock(&new_worker->local_mutex);
            // Else returns the next element in the list
            element = list_next(element);
        }
    }

    return NULL;
}

/* Create a new thread pool with no more than n threads. */
struct thread_pool *thread_pool_new(int nthreads) {
    struct thread_pool *pool = malloc(sizeof(struct thread_pool));

    list_init(&pool->global_queue);
    list_init(&pool->worker_list);

    pthread_mutex_init(&pool->pool_mutex, NULL);
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
    if (current_worker == NULL) {
        pthread_mutex_lock(&pool->pool_mutex);
    } else {
        pthread_mutex_lock(&current_worker->local_mutex);
    }

    struct future *future_init = malloc(sizeof(struct future));

    future_init->pool = pool;
    future_init->task = task;
    future_init->args = data;
    future_init->status = 2;
    future_init->result = NULL;

    sem_init(&future_init->completed, 0, 0);

    if (current_worker == NULL) {
        list_push_back(&pool->global_queue, &future_init->elem);
    } else {
        list_push_front(&current_worker->worker_queue, &future_init->elem);
    }
    pool->njobs++;

    pthread_cond_signal(&pool->pool_cond);

    if (current_worker == NULL) {
        pthread_mutex_unlock(&pool->pool_mutex);
    } else {
        pthread_mutex_unlock(&current_worker->local_mutex);
    }
    return future_init;
}

/* Make sure that the thread pool has completed the execution
 * of the fork join task this future represents.
 *
 * 2 = UNSCHEDULE, 1 = EXECUTING, 0 = FINISHED
 *
 * Returns the value returned by this task.
 */
void *future_get(struct future *future) {
    pthread_mutex_lock(&future->pool->pool_mutex);

    if (future->status == 2 && current_worker != NULL &&
        current_worker->pool == future->pool) {
        future->status = 1;
        future->pool->njobs = future->pool->njobs - 1;
        list_remove(&future->elem);
        pthread_mutex_unlock(&future->pool->pool_mutex);
        future->result = (future->task)(future->pool, future->args);
        future->status = 0;
        sem_post(&future->completed);
    } else {
        pthread_mutex_unlock(&future->pool->pool_mutex);
        sem_wait(&future->completed);
    }
    return future->result;
}

/* Deallocate this future.  Must be called after future_get() */
void future_free(struct future *future) { free(future); }
