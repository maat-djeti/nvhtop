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

#include "nvtop/system_stats.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Previous-tick per-core counters, for CPU% deltas.
static struct {
  unsigned long long idle[SYS_STATS_MAX_CORES];
  unsigned long long total[SYS_STATS_MAX_CORES];
  unsigned long long user[SYS_STATS_MAX_CORES];
  unsigned long long system[SYS_STATS_MAX_CORES];
  unsigned count;
  bool valid;
} prev = {0};

static unsigned long long read_u64(const char *path, const char *key_prefix, const char *key_suffix) {
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  char line[256];
  unsigned long long val = 0;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, key_prefix, strlen(key_prefix)) == 0) {
      if (key_suffix) {
        // e.g. "S:123" -> want the number after "S:"
        char *p = strstr(line, key_suffix);
        if (p)
          val = strtoull(p + strlen(key_suffix), NULL, 10);
      } else {
        val = strtoull(line, NULL, 10);
      }
      break;
    }
  }
  fclose(f);
  return val;
}

static void read_meminfo(struct sys_stats *out) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f)
    return;
  char line[256];
  unsigned long long mem_total = 0, mem_free = 0, mem_avail = 0, swap_total = 0, swap_free = 0;
  unsigned long long buffers = 0, cached = 0, shmem = 0, sreclaimable = 0;
  while (fgets(line, sizeof(line), f)) {
    unsigned long long v;
    if (sscanf(line, "MemTotal: %llu kB", &v) == 1)
      mem_total = v;
    else if (sscanf(line, "MemFree: %llu kB", &v) == 1)
      mem_free = v;
    else if (sscanf(line, "MemAvailable: %llu kB", &v) == 1)
      mem_avail = v;
    else if (sscanf(line, "Buffers: %llu kB", &v) == 1)
      buffers = v;
    else if (sscanf(line, "Cached: %llu kB", &v) == 1)
      cached = v;
    else if (sscanf(line, "Shmem: %llu kB", &v) == 1)
      shmem = v;
    else if (sscanf(line, "SReclaimable: %llu kB", &v) == 1)
      sreclaimable = v;
    else if (sscanf(line, "SwapTotal: %llu kB", &v) == 1)
      swap_total = v;
    else if (sscanf(line, "SwapFree: %llu kB", &v) == 1)
      swap_free = v;
  }
  fclose(f);
  out->mem_total = mem_total;
  out->mem_free = mem_free;
  out->mem_available = mem_avail;
  out->mem_used = mem_total > mem_avail ? mem_total - mem_avail : 0;
  // htop memory classes (linux/LinuxMachine.c). Shmem is part of Cached, so it
  // is subtracted from the cache class and shown separately as "shared".
  out->mem_shared_class = shmem;
  out->mem_buffers_class = buffers;
  out->mem_cache_class = (cached + sreclaimable) >= shmem ? cached + sreclaimable - shmem : 0;
  // App-used = total minus everything reclaimable (free, cache, buffers),
  // matching htop's "used" (buffers shown as their own blue segment).
  unsigned long long used_diff = mem_free + cached + sreclaimable + buffers;
  out->mem_used_class = mem_total >= used_diff ? mem_total - used_diff : (mem_total > mem_free ? mem_total - mem_free : 0);
  out->swap_total = swap_total;
  out->swap_free = swap_free;
  out->swap_used = swap_total > swap_free ? swap_total - swap_free : 0;
}

static void read_loadavg(struct sys_stats *out) {
  FILE *f = fopen("/proc/loadavg", "r");
  if (!f)
    return;
  if (fscanf(f, "%lf %lf %lf", &out->load1, &out->load5, &out->load15) != 3)
    out->load1 = out->load5 = out->load15 = 0;
  fclose(f);
}

static void read_uptime(struct sys_stats *out) {
  FILE *f = fopen("/proc/uptime", "r");
  if (!f)
    return;
  double up, idle;
  if (fscanf(f, "%lf %lf", &up, &idle) == 1)
    out->uptime = up;
  fclose(f);
}

static void read_task_counts(struct sys_stats *out) {
  // Scan /proc/[pid]/stat field 3 for state counts.
  unsigned long long total = 0, running = 0, sleeping = 0, zombie = 0, stopped = 0;
  char path[64];
  // Reuse a simple directory scan.
  DIR *proc = opendir("/proc");
  if (!proc)
    return;
  struct dirent *ent;
  while ((ent = readdir(proc)) != NULL) {
    if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
      continue;
    char *endp;
    long v = strtol(ent->d_name, &endp, 10);
    if (endp == ent->d_name || *endp != '\0' || v <= 0)
      continue;
    snprintf(path, sizeof(path), "/proc/%s/stat", ent->d_name);
    FILE *sf = fopen(path, "r");
    if (!sf)
      continue;
    char line[512];
    if (fgets(line, sizeof(line), sf)) {
      char *open = strchr(line, '(');
      char *close = open ? strrchr(open, ')') : NULL;
      if (open && close) {
        total++;
        char st = *(close + 2); // state is right after ") "
        switch (st) {
        case 'R':
          running++;
          break;
        case 'S':
        case 'D':
          sleeping++;
          break;
        case 'Z':
          zombie++;
          break;
        case 'T':
        case 't':
          stopped++;
          break;
        default:
          break;
        }
      }
    }
    fclose(sf);
  }
  closedir(proc);
  out->tasks_total = total;
  out->tasks_running = running;
  out->tasks_sleeping = sleeping;
  out->tasks_zombie = zombie;
  out->tasks_stopped = stopped;
}

unsigned sys_stats_read(struct sys_stats *out) {
  memset(out, 0, sizeof(*out));

  // Per-core CPU from /proc/stat
  FILE *f = fopen("/proc/stat", "r");
  if (!f)
    return 0;
  char line[1024];
  unsigned core = 0;
  while (fgets(line, sizeof(line), f)) {
    // Per-core lines start "cpuN " (cpu + digit(s) + space). The aggregate
    // line is "cpu " (cpu + space), so require a digit at line[3].
    if (strncmp(line, "cpu", 3) != 0)
      continue;
    if (line[3] < '0' || line[3] > '9')
      continue; // skip aggregate "cpu " line
    if (core >= SYS_STATS_MAX_CORES)
      break;

    // Split on whitespace with strtok. Fields: [0]=cpuN [1]=user [2]=nice
    // [3]=system [4]=idle [5]=iowait [6]=irq [7]=softirq [8]=steal [9]=guest
    // [10]=guest_nice
    char *tok[16];
    int ntok = 0;
    char *t = strtok(line, " \t\r\n");
    while (t && ntok < 16) {
      tok[ntok++] = t;
      t = strtok(NULL, " \t\r\n");
    }
    // Need the tag (1) + 10 time fields = 11 tokens.
    if (ntok < 11)
      continue;
    unsigned long long f_user = strtoull(tok[1], NULL, 10);
    unsigned long long f_nice = strtoull(tok[2], NULL, 10);
    unsigned long long f_system = strtoull(tok[3], NULL, 10);
    unsigned long long f_idle = strtoull(tok[4], NULL, 10);
    unsigned long long f_iowait = strtoull(tok[5], NULL, 10);
    unsigned long long f_irq = strtoull(tok[6], NULL, 10);
    unsigned long long f_softirq = strtoull(tok[7], NULL, 10);
    unsigned long long f_steal = strtoull(tok[8], NULL, 10);

    unsigned long long total = f_user + f_nice + f_system + f_idle + f_iowait + f_irq + f_softirq + f_steal;
    out->cores[core].idle = f_idle + f_iowait;
    out->cores[core].total = total;
    out->cores[core].user = f_user + f_nice;
    out->cores[core].system = f_system + f_irq + f_softirq + f_steal;
    core++;
  }
  fclose(f);
  out->core_count = core;

  // CPU% deltas
  for (unsigned i = 0; i < core; ++i) {
    if (prev.valid) {
      unsigned long long dt = out->cores[i].total - prev.total[i];
      unsigned long long di = out->cores[i].idle - prev.idle[i];
      unsigned long long du = out->cores[i].user - prev.user[i];
      unsigned long long ds = out->cores[i].system - prev.system[i];
      if (dt > 0) {
        out->core_pct[i] = 100.0 * (double)(dt - di) / (double)dt;
        out->core_user_pct[i] = 100.0 * (double)du / (double)dt;
        out->core_sys_pct[i] = 100.0 * (double)ds / (double)dt;
      } else {
        out->core_pct[i] = 0;
        out->core_user_pct[i] = 0;
        out->core_sys_pct[i] = 0;
      }
    } else {
      out->core_pct[i] = 0;
      out->core_user_pct[i] = 0;
      out->core_sys_pct[i] = 0;
    }
    prev.idle[i] = out->cores[i].idle;
    prev.total[i] = out->cores[i].total;
    prev.user[i] = out->cores[i].user;
    prev.system[i] = out->cores[i].system;
  }
  prev.count = core;
  prev.valid = true;

  read_meminfo(out);
  read_loadavg(out);
  read_uptime(out);
  read_task_counts(out);
  (void)read_u64;
  return core;
}
