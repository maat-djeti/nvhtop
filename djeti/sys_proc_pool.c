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

  // PID-indexed record collection. Allocated once at pid_max+1 entries; entry
  // i is the record for pid i (or NULL if that pid has not been seen). This is
  // the PID-keyed working store: a process is always found by its pid, so its
  // CPU% baseline (stored in the record) is stable across scans no matter which
  // display slot it lands in. This is what fixes the flicker.
  struct sys_proc **indexed_collection;
  size_t pid_max;

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

// Tokenize a whitespace-separated line into an array of field pointers.
// Returns the number of fields. `fields` must hold at least `max` entries.
static int tokenize(char *line, char *fields[][2], int max) {
  int count = 0;
  char *p = line;
  while (*p && count < max) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (!*p)
      break;
    fields[count][0] = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;
    fields[count][1] = p; // one past the end (not NUL-terminated yet)
    count++;
  }
  return count;
}

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
    // anchor on the last ')' in the line. Everything after is whitespace-
    // separated fields, indexed by position (1-based per the procps man page).
    char *open = strchr(line, '(');
    char *close = open ? strrchr(open, ')') : NULL;
    if (open && close) {
      // NUL-terminate the tail so tokenize sees a clean string.
      char *p = close + 1;
      while (*p == ' ')
        p++;
      // Tokenize the tail: f3=state, f4=ppid, f14=utime, f15=stime,
      // f16=cutime, f17=cstime, f18=priority, f19=nice, f20=threads,
      // f23=vsize, f24=rss (pages). Index = field_number - 3.
      char *fields[64][2];
      int nf = tokenize(p, fields, 64);
      // Need at least up to f24 => index 21.
      if (nf >= 22) {
        rec->state = fields[0][0][0];           // f3
        rec->ppid = (pid_t)strtol(fields[1][0], NULL, 10); // f4
        rec->utime = strtoull(fields[11][0], NULL, 10);   // f14
        rec->stime = strtoull(fields[12][0], NULL, 10);   // f15
        rec->cutime = strtoull(fields[13][0], NULL, 10);  // f16
        rec->cstime = strtoull(fields[14][0], NULL, 10);  // f17
        rec->priority = (int)strtol(fields[15][0], NULL, 10); // f18
        rec->nice = (int)strtol(fields[16][0], NULL, 10);     // f19
        rec->threads = strtol(fields[17][0], NULL, 10);       // f20
        rec->vsize = strtoull(fields[20][0], NULL, 10);       // f23
        rec->rss = strtoull(fields[21][0], NULL, 10) * (unsigned long long)pool->page_size; // f24
        ok = true;
      }
    }
  }
  fclose(f);
  return ok;
}

// Shared pages come from /proc/<pid>/statm field 3 (not /proc/<pid>/stat).
static void read_shr(pid_t pid, struct sys_proc *rec, long page_size) {
  char path[DJETI_PATH_MAX];
  int w = snprintf(path, sizeof(path), "/proc/%" PRIdMAX "/statm", (intmax_t)pid);
  if (w < 0 || (size_t)w >= sizeof(path))
    return;
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char line[256];
  if (fgets(line, sizeof(line), f)) {
    char *fields[8][2];
    int nf = tokenize(line, fields, 8);
    // statm: size(1) resident(2) shared(3) text(4) lib(5) data(6) dt(7)
    if (nf >= 3) {
      rec->shr = strtoull(fields[2][0], NULL, 10) * (unsigned long long)page_size;
    }
  }
  fclose(f);
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
  case process_shr:
    return (uint64_t)r->shr;
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
  // pid_max is a live kernel tunable (/proc/sys/kernel/pid_max), read at
  // runtime. Fall back to a generous default if the file is unreadable.
  pool->pid_max = 0;
  {
    FILE *pf = fopen("/proc/sys/kernel/pid_max", "r");
    if (pf) {
      if (fscanf(pf, "%zu", &pool->pid_max) != 1)
        pool->pid_max = 0;
      fclose(pf);
    }
  }
  if (pool->pid_max < 1024)
    pool->pid_max = 1024;
  // PID-indexed record collection, zeroed so every entry starts NULL (pid not
  // yet seen). calloc guarantees the NULL-init the scan relies on.
  pool->indexed_collection = calloc(pool->pid_max + 1, sizeof(*pool->indexed_collection));
  if (!pool->indexed_collection)
    abort();
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
  // records[] and sorted[] hold POINTERS into indexed_collection; only the
  // index array itself is freed here, the pointer arrays are just index buffers.
  free(pool->records);
  free(pool->sorted);
  // Free every record the indexed collection owns, then the index itself.
  for (size_t i = 0; i <= pool->pid_max; ++i)
    free(pool->indexed_collection[i]);
  free(pool->indexed_collection);
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

  // GC: walk the previous display frame (sorted[] still holds last scan's
  // records). For each, check /proc/<pid> against the source of truth. If the
  // process is gone, free the record and NULL the indexed_collection slot so
  // a pid reuse starts with a clean baseline. Records pointed to by the
  // current display frame are retained for the next cycle.
  for (size_t i = 0; i < pool->sorted_count; ++i) {
    struct sys_proc *rec = pool->sorted[i];
    char path[DJETI_PATH_MAX];
    int w = snprintf(path, sizeof(path), "/proc/%" PRIdMAX, (intmax_t)rec->pid);
    if (w < 0 || (size_t)w >= sizeof(path))
      continue;
    struct stat st;
    if (stat(path, &st) != 0) {
      pool->indexed_collection[rec->pid] = NULL;
      free(rec);
    }
  }

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

    // Record is addressed by PID via the indexed collection: stable across
    // scans, so the CPU% baseline stored in the record survives slot shifts.
    if (pool->indexed_collection[pid] == NULL)
      pool->indexed_collection[pid] = calloc(1, sizeof(struct sys_proc)); // ONE record (I4)
    struct sys_proc *rec = pool->indexed_collection[pid];
    rec->pid = pid; // DATA, not a key (I2)
    if (!read_stat(pool, pid, rec))
      continue; // vanished mid-scan; slot not counted in h
    read_shr(pid, rec, pool->page_size);
    read_user(pid, rec);
    read_command(pid, rec);

    // CPU% delta against this record's own previous sample (PID-stable).
    nvtop_time now;
    nvtop_get_current_time(&now);
    if (rec->has_prev) {
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
    rec->has_prev = true;
    rec->total_time = (double)(rec->utime + rec->stime) / pool->ticks;
    rec->mem_pct = pool->total_ram > 0. ? 100. * (double)rec->rss / pool->total_ram : 0.;
    rec->fresh = true;

    // Also place this record into the slot-ordered working array for the frame.
    if (h >= pool->record_slots)
      records_append_slot(pool);
    pool->records[h] = rec;
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
