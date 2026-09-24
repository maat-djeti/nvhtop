/*
 *
 * Copyright (C) 2026 Djeti AI
 *
 * This file is part of Nvtop.
 *
 * Nvtop is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nvtop is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nvtop.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// /proc/<pid> is the ONLY source of truth (I1). records/sorted are disposable
// projections, indexed by POSITION 0..h, never by PID (I2, I3). Each slot
// holds ONE individually malloc'd record (I4); pointer arrays grow ONE slot
// at a time on observed demand (I5, I10). Command is a fixed char[64] in the
// record (I6). Grow-only until pool_free (I7). Two semaphores, strict
// out-of-phase alternation, no mutex (I8). Records are stable-address (I9).
//
// See ~/checkpoints/2026-09-23-djeti-process-pool-final.md.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "sys_proc_pool.h"

#include <dirent.h>
#include <inttypes.h>
#include <pwd.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DJETI_PATH_MAX 1024

struct sys_proc_pool {
  // Anonymous record slots, position-indexed 0..record_slots-1. Slot i holds
  // the i-th live process THIS scan (or NULL if not yet allocated). Grow-only.
  struct sys_proc **records;
  size_t record_slots;

  // Display frame: COPIES of the stable record pointers, sorted.
  struct sys_proc **sorted;
  size_t sorted_slots;
  size_t sorted_count;

  double total_ram; // bytes
  double ticks;     // _SC_CLK_TCK
  long page_size;

  // Out-of-phase handoff, no mutex (I8).
  sem_t frame_ready; // producer -> consumer
  sem_t frame_done;  // consumer -> producer, init 1 so first produce proceeds
};

// Sort parameters for the current produce (read by the qsort_r comparator).
static __thread enum process_field djti_sort_key;
static __thread bool djti_sort_desc;

// ---------------------------------------------------------------------------
// Slot growth: ONE pointer slot at a time, driven by observed demand (I5, I10)
// ---------------------------------------------------------------------------

static void records_append_slot(struct sys_proc_pool *pool) {
  struct sys_proc **nr = realloc(pool->records, (pool->record_slots + 1) * sizeof(*nr));
  if (!nr)
    abort();
  pool->records = nr;
  pool->records[pool->record_slots] = NULL;
  pool->record_slots++;
}

static void sorted_append_slot(struct sys_proc_pool *pool) {
  struct sys_proc **ns = realloc(pool->sorted, (pool->sorted_slots + 1) * sizeof(*ns));
  if (!ns)
    abort();
  pool->sorted = ns;
  pool->sorted_slots++;
}

// ---------------------------------------------------------------------------
// /proc readers
// ---------------------------------------------------------------------------

static bool read_stat(struct sys_proc_pool *pool, pid_t pid, struct sys_proc *rec) {
  char path[DJETI_PATH_MAX];
  int w = snprintf(path, sizeof(path), "/proc/%" PRIdMAX "/stat", (intmax_t)pid);
  if (w < 0 || (size_t)w >= sizeof(path))
    return false;
  FILE *f = fopen(path, "r");
  if (!f)
    return false;
  char line[2048];
  bool ok = false;
  if (fgets(line, sizeof(line), f)) {
    // comm (field 2) is in parentheses and may contain spaces or parens;
    // anchor on the last ')' in the line.
    char *open = strchr(line, '(');
    char *close = open ? strrchr(open, ')') : NULL;
    if (open && close) {
      char *p = close + 1;
      while (*p == ' ')
        p++;
      // f3 state, f4 ppid, f5..f13 skipped, f14 utime, f15 stime,
      // f16 cutime, f17 cstime, f18 priority, f19 nice, f20 threads,
      // f21 f22 skipped, f23 vsize, f24 rss (pages)
      char state_c;
      int ppid, priority, nice;
      long threads;
      unsigned long long utime, stime, cutime, cstime, vsize, rss;
      int n = sscanf(p, "%c %d %*d %*d %*d %*d %*d %*d %*d %*d %*d %llu %llu %lld %lld %d %d %ld %*d %*d %llu %lld",
                     &state_c, &ppid, &utime, &stime, &cutime, &cstime, &priority, &nice,
                     &threads, &vsize, &rss);
      if (n == 11) {
        rec->state = state_c;
        rec->ppid = (pid_t)ppid;
        rec->utime = utime;
        rec->stime = stime;
        rec->cutime = (unsigned long long)cutime;
        rec->cstime = (unsigned long long)cstime;
        rec->priority = priority;
        rec->nice = nice;
        rec->threads = threads;
        rec->vsize = vsize;
        rec->rss = rss * (unsigned long long)pool->page_size;
        ok = true;
      }
    }
  }
  fclose(f);
  return ok;
}

static void read_user(pid_t pid, struct sys_proc *rec) {
  char path[DJETI_PATH_MAX];
  int w = snprintf(path, sizeof(path), "/proc/%" PRIdMAX, (intmax_t)pid);
  if (w < 0 || (size_t)w >= sizeof(path))
    return;
  struct stat st;
  if (stat(path, &st) == -1)
    return;
  rec->uid = (unsigned int)st.st_uid;
  struct passwd *pw = getpwuid(st.st_uid);
  if (pw && pw->pw_name)
    snprintf(rec->user, sizeof(rec->user), "%s", pw->pw_name);
  else
    snprintf(rec->user, sizeof(rec->user), "%u", (unsigned)st.st_uid);
}

// cmdline: NUL-separated args; read directly into rec->command, join with
// spaces in place (I6).
static void read_command(pid_t pid, struct sys_proc *rec) {
  char path[DJETI_PATH_MAX];
  int w = snprintf(path, sizeof(path), "/proc/%" PRIdMAX "/cmdline", (intmax_t)pid);
  if (w < 0 || (size_t)w >= sizeof(path))
    return;
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  size_t total = fread(rec->command, 1, sizeof(rec->command) - 1, f);
  fclose(f);
  if (total == 0) {
    // Kernel threads (and some others) have an empty cmdline; fall back to
    // the task name in /proc/<pid>/comm, which is always present.
    int w2 = snprintf(path, sizeof(path), "/proc/%" PRIdMAX "/comm", (intmax_t)pid);
    if (w2 >= 0 && (size_t)w2 < sizeof(path)) {
      FILE *cf = fopen(path, "r");
      if (cf) {
        size_t cl = fread(rec->command, 1, sizeof(rec->command) - 1, cf);
        fclose(cf);
        rec->command[cl] = '\0';
        while (cl > 0 && (rec->command[cl - 1] == '\n' || rec->command[cl - 1] == '\r'))
          cl--;
        rec->command[cl] = '\0';
        rec->cmd_from_comm = true;
        return;
      }
    }
    rec->command[0] = '\0';
    rec->cmd_from_comm = false;
    return;
  }
  rec->command[total] = '\0';
  for (size_t i = 0; i < total; ++i)
    if (rec->command[i] == '\0')
      rec->command[i] = ' ';
  while (total > 0 && rec->command[total - 1] == ' ')
    total--;
  rec->command[total] = '\0';
  rec->cmd_from_comm = false;
}

// ---------------------------------------------------------------------------
// Sorting
// ---------------------------------------------------------------------------

static uint64_t sort_value(const struct sys_proc *r, enum process_field key) {
  switch (key) {
  case process_pid:
    return (uint64_t)(uint32_t)r->pid;
  case process_ppid:
    return (uint64_t)(uint32_t)r->ppid;
  case process_priority:
    return (uint64_t)(int64_t)r->priority;
  case process_nice:
    return (uint64_t)(int64_t)r->nice;
  case process_threads:
    return (uint64_t)r->threads;
  case process_virt:
    return (uint64_t)r->vsize;
  case process_res:
    return (uint64_t)r->rss;
  case process_cpu_pct:
    return (uint64_t)(r->cpu_pct * 100.0);
  case process_memory:
    return (uint64_t)(r->mem_pct * 100.0);
  case process_time:
    return (uint64_t)(r->utime + r->stime + r->cutime + r->cstime);
  default:
    return (uint64_t)(uint32_t)r->pid;
  }
}

static int sorted_compare(const void *a, const void *b, void *arg) {
  (void)arg;
  const struct sys_proc *const *pa = a;
  const struct sys_proc *const *pb = b;
  const struct sys_proc *ra = *pa, *rb = *pb;
  int c;
  if (djti_sort_key == process_command) {
    c = strcmp(ra->command, rb->command);
  } else {
    uint64_t va = sort_value(ra, djti_sort_key), vb = sort_value(rb, djti_sort_key);
    c = (va < vb) ? -1 : (va > vb ? 1 : 0);
  }
  if (c == 0)
    c = (int)((uint32_t)ra->pid - (uint32_t)rb->pid); // tiebreak: pid
  return djti_sort_desc ? -c : c;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

struct sys_proc_pool *sys_proc_pool_new(void) {
  struct sys_proc_pool *pool = calloc(1, sizeof(*pool));
  if (!pool)
    abort();
  pool->total_ram = (double)sysconf(_SC_PHYS_PAGES) * (double)sysconf(_SC_PAGESIZE);
  pool->ticks = (double)sysconf(_SC_CLK_TCK);
  pool->page_size = sysconf(_SC_PAGESIZE);
  sem_init(&pool->frame_ready, 0, 0);
  sem_init(&pool->frame_done, 0, 1);
  return pool;
}

void sys_proc_pool_free(struct sys_proc_pool *pool) {
  if (!pool)
    return;
  // Drain a pending frame so the consumer is not left blocked (I7A). The
  // producer thread must already be stopped before this call.
  while (sem_trywait(&pool->frame_ready) == 0)
    sem_post(&pool->frame_done);
  for (size_t i = 0; i < pool->record_slots; ++i)
    free(pool->records[i]);
  free(pool->records);
  free(pool->sorted);
  sem_destroy(&pool->frame_ready);
  sem_destroy(&pool->frame_done);
  free(pool);
}

unsigned sys_proc_pool_count(const struct sys_proc_pool *pool) {
  return (unsigned)pool->sorted_count;
}

void sys_proc_pool_drain(struct sys_proc_pool *pool) {
  if (!pool)
    return;
  while (sem_trywait(&pool->frame_ready) == 0)
    sem_post(&pool->frame_done);
}

void sys_proc_pool_produce(struct sys_proc_pool *pool, enum process_field sort_key, bool sort_desc) {
  // Wait for the consumer to finish with sorted[] (first call passes:
  // frame_done starts at 1).
  sem_wait(&pool->frame_done);

  // 1. Walk /proc in scan order; slot h is the h-th live process THIS scan.
  size_t h = 0;
  DIR *proc = opendir("/proc");
  if (!proc) {
    sem_post(&pool->frame_done); // keep the handoff balanced
    return;
  }
  struct dirent *ent;
  while ((ent = readdir(proc)) != NULL) {
    if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
      continue;
    char *endp;
    long v = strtol(ent->d_name, &endp, 10);
    if (endp == ent->d_name || *endp != '\0' || v <= 0)
      continue;
    pid_t pid = (pid_t)v;

    if (h >= pool->record_slots)
      records_append_slot(pool);
    if (pool->records[h] == NULL)
      pool->records[h] = calloc(1, sizeof(struct sys_proc)); // ONE record (I4)
    struct sys_proc *rec = pool->records[h];
    rec->pid = pid; // DATA, not a key (I2)
    if (!read_stat(pool, pid, rec))
      continue; // vanished mid-scan; slot not counted in h
    read_user(pid, rec);
    read_command(pid, rec);

    // CPU% deltas — valid only if this slot held the SAME pid last scan.
    nvtop_time now;
    nvtop_get_current_time(&now);
    if (rec->has_prev && rec->prev_pid == pid) {
      double dt = nvtop_difftime(rec->prev_time, now);
      if (dt > 0.) {
        double dutime = (double)(rec->utime - rec->prev_utime) / pool->ticks;
        double dstime = (double)(rec->stime - rec->prev_stime) / pool->ticks;
        rec->cpu_user_pct = 100. * dutime / dt;
        rec->cpu_sys_pct = 100. * dstime / dt;
        rec->cpu_pct = rec->cpu_user_pct + rec->cpu_sys_pct;
      } else {
        rec->cpu_user_pct = rec->cpu_sys_pct = rec->cpu_pct = 0.;
      }
    } else {
      rec->cpu_user_pct = rec->cpu_sys_pct = rec->cpu_pct = 0.;
    }
    rec->prev_utime = rec->utime;
    rec->prev_stime = rec->stime;
    rec->prev_time = now;
    rec->prev_pid = pid;
    rec->has_prev = true;
    rec->total_time = (double)(rec->utime + rec->stime) / pool->ticks;
    rec->mem_pct = pool->total_ram > 0. ? 100. * (double)rec->rss / pool->total_ram : 0.;
    rec->fresh = true;
    h++;
  }
  closedir(proc);

  // 2. Build the sorted frame: copy the live record POINTERS (stable, I9),
  //    appending ONE pointer slot at a time (I10), then qsort.
  while (pool->sorted_slots < h)
    sorted_append_slot(pool);
  for (size_t i = 0; i < h; ++i)
    pool->sorted[i] = pool->records[i];

  djti_sort_key = sort_key;
  djti_sort_desc = sort_desc;
  qsort_r(pool->sorted, h, sizeof(*pool->sorted),
          (int (*)(const void *, const void *, void *))sorted_compare, NULL);
  pool->sorted_count = h;

  // 3. Hand off to the consumer.
  sem_post(&pool->frame_ready);
}

// Block until a frame is ready, then return the sorted array.
// Caller must invoke sys_proc_pool_frame_done after rendering.
const struct sys_proc **sys_proc_pool_frame_begin(struct sys_proc_pool *pool, unsigned *count) {
  // Non-blocking: if the producer has posted a frame, take it. If not, return
  // NULL so the caller skips rendering this pass and the main loop stays
  // responsive to keystrokes. A block here means the producer is too slow.
  if (sem_trywait(&pool->frame_ready) != 0)
    return NULL;
  *count = (unsigned)pool->sorted_count;
  return (const struct sys_proc **)pool->sorted;
}

void sys_proc_pool_frame_done(struct sys_proc_pool *pool) {
  for (size_t i = 0; i < pool->sorted_count; ++i)
    pool->sorted[i]->fresh = false;
  sem_post(&pool->frame_done);
}
