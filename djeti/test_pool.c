// Smoke test for the djeti process pool (checkpoint 2026-09-23, FINAL spec).
//
// Test 1 (two-thread): producer thread produces frames at ~1 Hz; consumer
//         thread reads the sorted array directly and prints every row.
// Test 2 (sort invariant): produce one frame, read the array, verify order.
//
// Build:
//   gcc -Wall -Wextra -O2 -Iinclude -Idjeti -o /tmp/pool_test djeti/sys_proc_pool.c djeti/test_pool.c -lpthread

#include "sys_proc_pool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Test 1: two-thread smoke test
// ---------------------------------------------------------------------------

static void *producer_thread(void *arg) {
  struct sys_proc_pool *pool = (struct sys_proc_pool *)arg;
  for (int i = 0; i < 3; ++i) {
    sys_proc_pool_produce(pool, process_cpu_pct, true);
    usleep(1000000);
  }
  return NULL;
}

static void *consumer_thread(void *arg) {
  struct sys_proc_pool *pool = (struct sys_proc_pool *)arg;
  int frames = 0;
  for (int i = 0; i < 3; ++i) {
    unsigned count;
    const struct sys_proc **procs = sys_proc_pool_frame_begin(pool, &count);
    if (procs == NULL) {
      // No frame ready yet (producer slower than consumer this pass).
      usleep(100000);
      continue;
    }
    frames++;
    printf("frame %d: %u live processes\n", frames, count);
    for (unsigned j = 0; j < count; ++j) {
      const struct sys_proc *rec = procs[j];
      printf("  pid=%-6d cpu=%6.1f%% mem=%5.2f%% %s %s\n", (int)rec->pid, rec->cpu_pct,
             rec->mem_pct, rec->user, rec->command);
    }
    sys_proc_pool_frame_done(pool);
    printf("\n");
    usleep(1000000);
  }
  return NULL;
}

static int test_two_thread(void) {
  printf("=== Test 1: two-thread smoke test ===\n");
  struct sys_proc_pool *pool = sys_proc_pool_new();
  pthread_t prod, cons;
  pthread_create(&prod, NULL, producer_thread, pool);
  pthread_create(&cons, NULL, consumer_thread, pool);
  pthread_join(cons, NULL);
  pthread_join(prod, NULL);
  sys_proc_pool_free(pool);
  printf("pool freed cleanly\n\n");
  return 0;
}

// ---------------------------------------------------------------------------
// Test 2: sort invariant
// ---------------------------------------------------------------------------

static int64_t key_val(const struct sys_proc *r, enum process_field key) {
  switch (key) {
  case process_pid: return (int64_t)r->pid;
  case process_ppid: return (int64_t)r->ppid;
  case process_priority: return r->priority;
  case process_nice: return r->nice;
  case process_threads: return (int64_t)r->threads;
  case process_virt: return (int64_t)r->vsize;
  case process_res: return (int64_t)r->rss;
  case process_cpu_pct: return (int64_t)(r->cpu_pct * 100.0);
  case process_memory: return (int64_t)(r->mem_pct * 100.0);
  case process_time: return (int64_t)(r->utime + r->stime + r->cutime + r->cstime);
  default: return (int64_t)r->pid;
  }
}

static int test_sort_invariant(void) {
  printf("=== Test 2: sort invariant ===\n");
  struct sys_proc_pool *pool = sys_proc_pool_new();
  enum process_field keys[] = {process_cpu_pct, process_pid, process_res};
  bool descs[] = {true, false, true};
  int rc = 0;
  for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
    sys_proc_pool_produce(pool, keys[k], descs[k]);
    unsigned count;
    const struct sys_proc **procs = sys_proc_pool_frame_begin(pool, &count);
    if (procs == NULL) {
      printf("key=%d: no frame ready (unexpected)\n", (int)keys[k]);
      rc = 1;
      continue;
    }
    int failures = 0;
    for (unsigned i = 1; i < count; ++i) {
      int64_t va = key_val(procs[i - 1], keys[k]), vb = key_val(procs[i], keys[k]);
      int cmp = (va < vb) ? -1 : (va > vb ? 1 : 0);
      if (cmp == 0)
        cmp = (procs[i - 1]->pid < procs[i]->pid) ? -1 : (procs[i - 1]->pid > procs[i]->pid ? 1 : 0);
      if (descs[k])
        cmp = -cmp;
      if (cmp > 0)
        failures++;
    }
    sys_proc_pool_frame_done(pool);
    printf("key=%d desc=%d: %u records, %d order violations\n", (int)keys[k], (int)descs[k], count,
           failures);
    if (failures)
      rc = 1;
  }
  sys_proc_pool_free(pool);
  printf("sort invariant test %s\n\n", rc ? "FAILED" : "complete");
  return rc;
}

int main(void) {
  int rc = 0;
  rc |= test_two_thread();
  rc |= test_sort_invariant();
  printf(rc ? "FAILED\n" : "ALL TESTS PASSED\n");
  return rc;
}
