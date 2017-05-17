/*
Copyright: Boaz Segev, 2016-2017
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/
#include "defer.h"
#include "spnlock.inc"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

/* *****************************************************************************
Compile time settings
***************************************************************************** */

#ifndef DEFER_QUEUE_BUFFER
#define DEFER_QUEUE_BUFFER 1024
#endif
#ifndef DEFER_THROTTLE
#define DEFER_THROTTLE 8388608UL
#endif

/* *****************************************************************************
Data Structures
***************************************************************************** */

typedef struct {
  void (*func)(void *);
  void *arg;
} task_s;

typedef struct task_node_s {
  task_s task;
  struct task_node_s *next;
} task_node_s;

static task_node_s tasks_buffer[DEFER_QUEUE_BUFFER];

static struct {
  task_node_s *first;
  task_node_s **last;
  task_node_s *pool;
  spn_lock_i lock;
  unsigned char initialized;
} deferred = {.first = NULL,
              .last = &deferred.first,
              .pool = NULL,
              .lock = 0,
              .initialized = 0};

/* *****************************************************************************
API
***************************************************************************** */

/** Defer an execution of a function for later. */
int defer(void (*func)(void *), void *arg) {
  if (!func)
    goto call_error;
  task_node_s *task;
  spn_lock(&deferred.lock);
  if (deferred.pool) {
    task = deferred.pool;
    deferred.pool = deferred.pool->next;
  } else if (deferred.initialized) {
    task = malloc(sizeof(task_node_s));
    if (!task)
      goto error;
  } else {
    deferred.initialized = 1;
    task = tasks_buffer;
    deferred.pool = tasks_buffer + 1;
    for (size_t i = 2; i < DEFER_QUEUE_BUFFER; i++) {
      tasks_buffer[i - 1].next = tasks_buffer + i;
    }
  }
  *deferred.last = task;
  deferred.last = &task->next;
  task->task.func = func;
  task->task.arg = arg;
  task->next = NULL;
  spn_unlock(&deferred.lock);
  return 0;
error:
  spn_unlock(&deferred.lock);
call_error:
  return -1;
}

/** Performs all deferred functions until the queue had been depleted. */
void defer_perform(void) {
  task_node_s *tmp;
  task_s task;
restart:
  spn_lock(&deferred.lock);
  tmp = deferred.first;
  if (tmp) {
    deferred.first = tmp->next;
    if (!deferred.first)
      deferred.last = &deferred.first;
    task = tmp->task;
    if (tmp <= tasks_buffer + (DEFER_QUEUE_BUFFER - 1) && tmp >= tasks_buffer) {
      tmp->next = deferred.pool;
      deferred.pool = tmp;
    } else {
      free(tmp);
    }
    spn_unlock(&deferred.lock);
    task.func(task.arg);
    goto restart;
  } else
    spn_unlock(&deferred.lock);
}

/** returns true if there are deferred functions waiting for execution. */
int defer_has_queue(void) { return deferred.first != NULL; }

/* *****************************************************************************
Thread Pool Support
***************************************************************************** */

#if defined(__unix__) || defined(__APPLE__) || defined(__linux__) ||           \
    defined(DEBUG)
#include <pthread.h>

#pragma weak defer_new_thread
void *defer_new_thread(void *(*thread_func)(void *), void *arg) {
  pthread_t *thread = malloc(sizeof(*thread));
  if (thread == NULL || pthread_create(thread, NULL, thread_func, arg))
    goto error;
  return thread;
error:
  free(thread);
  return NULL;
}

#pragma weak defer_new_thread
int defer_join_thread(void *p_thr) {
  if (!p_thr)
    return -1;
  pthread_join(*(pthread_t *)p_thr, NULL);
  free(p_thr);
  return 0;
}

#else /* No pthreads... BYO thread implementation. */

#pragma weak defer_new_thread
void *defer_new_thread(void *(*thread_func)(void *), void *arg) {
  (void)thread_func;
  (void)arg;
  return NULL;
}
#pragma weak defer_new_thread
int defer_join_thread(void *p_thr) {
  (void)p_thr;
  return -1;
}

#endif /* DEBUG || pthread default */

struct defer_pool {
  unsigned int flag;
  unsigned int count;
  void *threads[];
};

static void *defer_worker_thread(void *pool) {
  size_t throttle = (((pool_pt)pool)->count & 127) * DEFER_THROTTLE;
  do {
    throttle_thread(throttle);
    defer_perform();
  } while (((pool_pt)pool)->flag);
  return NULL;
}

void defer_pool_stop(pool_pt pool) { pool->flag = 0; }

int defer_pool_is_active(pool_pt pool) { return pool->flag; }

void defer_pool_wait(pool_pt pool) {
  while (pool->count) {
    pool->count--;
    defer_join_thread(pool->threads[pool->count]);
  }
}

static inline pool_pt defer_pool_initialize(unsigned int thread_count,
                                            pool_pt pool) {
  pool->flag = 1;
  pool->count = 0;
  while (pool->count < thread_count &&
         (pool->threads[pool->count] =
              defer_new_thread(defer_worker_thread, pool)))
    pool->count++;
  if (pool->count == thread_count) {
    SPN_LOCK_THROTTLE = 8388608UL * thread_count;
    return pool;
  }
  defer_pool_stop(pool);
  return NULL;
}

pool_pt defer_pool_start(unsigned int thread_count) {
  if (thread_count == 0)
    return NULL;
  pool_pt pool = malloc(sizeof(*pool) + (thread_count * sizeof(void *)));
  if (!pool)
    return NULL;
  return defer_pool_initialize(thread_count, pool);
}

/* *****************************************************************************
Child Process support (`fork`)
***************************************************************************** */

static pool_pt forked_pool;

static void sig_int_handler(int sig) {
  if (sig != SIGINT)
    return;
  if (!forked_pool)
    return;
  defer_pool_stop(forked_pool);
}

/* *
Zombie Reaping
With thanks to Dr Graham D Shaw.
http://www.microhowto.info/howto/reap_zombie_processes_using_a_sigchld_handler.html
*/

void reap_child_handler(int sig) {
  (void)(sig);
  int old_errno = errno;
  while (waitpid(-1, NULL, WNOHANG) > 0)
    ;
  errno = old_errno;
}

inline static void reap_children(void) {
  struct sigaction sa;
  sa.sa_handler = reap_child_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  if (sigaction(SIGCHLD, &sa, 0) == -1) {
    perror("Child reaping initialization failed");
    exit(1);
  }
}

/**
 * Forks the process, starts up a thread pool and waits for all tasks to run.
 * All existing tasks will run in all processes (multiple times).
 *
 * Returns 0 on success, -1 on error and a positive number if this is a child
 * process that was forked.
 */
int defer_perform_in_fork(unsigned int process_count,
                          unsigned int thread_count) {
  struct sigaction act, old, old_term;
  pid_t *pids = NULL;
  int ret = 0;
  unsigned int pids_count;

  act.sa_handler = sig_int_handler;
  sigemptyset(&act.sa_mask);
  act.sa_flags = SA_RESTART | SA_NOCLDSTOP;

  if (sigaction(SIGINT, &act, &old)) {
    perror("couldn't set signal handler");
    goto finish;
  };
  if (sigaction(SIGTERM, &act, &old_term)) {
    perror("couldn't set signal handler");
    goto finish;
  };
  reap_children();

  if (!process_count)
    process_count = 1;
  --process_count;
  pids = calloc(process_count, sizeof(*pids));
  if (process_count && !pids)
    goto finish;
  for (pids_count = 0; pids_count < process_count; pids_count++) {
    if (!(pids[pids_count] = fork())) {
      forked_pool = defer_pool_start(thread_count);
      defer_pool_wait(forked_pool);
      defer_perform();
      defer_perform();
      return 1;
    }
    if (pids[pids_count] == -1) {
      ret = -1;
      goto finish;
    }
  }
  pids_count++;
  forked_pool = defer_pool_start(thread_count);
  defer_pool_wait(forked_pool);
  forked_pool = NULL;
  defer_perform();
finish:
  if (pids) {
    for (size_t j = 0; j < pids_count; j++) {
      kill(pids[j], SIGINT);
    }
    for (size_t j = 0; j < pids_count; j++) {
      waitpid(pids[j], NULL, 0);
    }
    free(pids);
  }
  sigaction(SIGINT, &old, &act);
  sigaction(SIGTERM, &old_term, &act);
  return ret;
}

/** Returns TRUE (1) if the forked thread pool hadn't been signaled to finish
 * up. */
int defer_fork_is_active(void) { return forked_pool && forked_pool->flag; }

/* *****************************************************************************
Test
***************************************************************************** */
#ifdef DEBUG

#include <stdio.h>

#include <pthread.h>
#define DEFER_TEST_THREAD_COUNT 128

static spn_lock_i i_lock = 0;
static size_t i_count = 0;

static void sample_task(void *unused) {
  (void)(unused);
  spn_lock(&i_lock);
  i_count++;
  spn_unlock(&i_lock);
}

static void sched_sample_task(void *unused) {
  (void)(unused);
  for (size_t i = 0; i < 1024; i++) {
    defer(sample_task, NULL);
  }
}

static void thrd_sched(void *unused) {
  for (size_t i = 0; i < (1024 / DEFER_TEST_THREAD_COUNT); i++) {
    sched_sample_task(unused);
  }
}

static void text_task_text(void *unused) {
  (void)(unused);
  spn_lock(&i_lock);
  fprintf(stderr, "this text should print before defer_perform returns\n");
  spn_unlock(&i_lock);
}

static void text_task(void *_) {
  static const struct timespec tm = {.tv_sec = 2};
  nanosleep(&tm, NULL);
  defer(text_task_text, _);
}

static void pid_task(void *arg) {
  fprintf(stderr, "* %d pid is going to sleep... (%s)\n", getpid(),
          arg ? arg : "unknown");
}

void defer_test(void) {
  time_t start, end;
  fprintf(stderr, "Starting defer testing\n");

  spn_lock(&i_lock);
  i_count = 0;
  spn_unlock(&i_lock);
  start = clock();
  for (size_t i = 0; i < 1024; i++) {
    defer(sched_sample_task, NULL);
  }
  defer_perform();
  end = clock();
  fprintf(stderr, "Defer single thread: %lu cycles with i_count = %lu\n",
          end - start, i_count);

  spn_lock(&i_lock);
  i_count = 0;
  spn_unlock(&i_lock);
  start = clock();
  pool_pt pool = defer_pool_start(DEFER_TEST_THREAD_COUNT);
  if (pool) {
    for (size_t i = 0; i < DEFER_TEST_THREAD_COUNT; i++) {
      defer(thrd_sched, NULL);
    }
    // defer((void (*)(void *))defer_pool_stop, pool);
    defer_pool_stop(pool);
    defer_pool_wait(pool);
    end = clock();
    fprintf(stderr,
            "Defer multi-thread (%d threads): %lu cycles with i_count = %lu\n",
            DEFER_TEST_THREAD_COUNT, end - start, i_count);
  } else
    fprintf(stderr, "Defer multi-thread: FAILED!\n");

  spn_lock(&i_lock);
  i_count = 0;
  spn_unlock(&i_lock);
  start = clock();
  for (size_t i = 0; i < 1024; i++) {
    defer(sched_sample_task, NULL);
  }
  defer_perform();
  end = clock();
  fprintf(stderr, "Defer single thread (2): %lu cycles with i_count = %lu\n",
          end - start, i_count);

  fprintf(stderr, "calling defer_perform.\n");
  defer(text_task, NULL);
  defer_perform();
  fprintf(stderr, "defer_perform returned. i_count = %lu\n", i_count);
  size_t pool_count = 0;
  task_node_s *pos = deferred.pool;
  while (pos) {
    pool_count++;
    pos = pos->next;
  }
  fprintf(stderr, "defer pool count %lu/%d (%s)\n", pool_count,
          DEFER_QUEUE_BUFFER,
          pool_count == DEFER_QUEUE_BUFFER ? "pass" : "FAILED");
  fprintf(stderr, "press ^C to finish PID test\n");
  defer(pid_task, "pid test");
  if (defer_perform_in_fork(4, 64) > 0) {
    fprintf(stderr, "* %d finished\n", getpid());
    exit(0);
  };
  fprintf(stderr, "\nPID test passed?\n");
}

#endif
