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

#include "nvtop/system_processes.h"
#include "nvtop/time.h"
#include "sys_proc.h"

#include <dirent.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define pid_path_size 1024
static char pid_path[pid_path_size];

// Previous-tick sample used to compute CPU% deltas.
struct prev_sample {
  pid_t pid;
  double total_time; // utime+stime in seconds
  nvtop_time timestamp;
};

static struct prev_sample *prev_samples = NULL;
static unsigned prev_count = 0;
static unsigned prev_cap = 0;

static void prev_reserve(unsigned cap) {
  if (cap <= prev_cap)
    return;
  unsigned new_cap = cap < 256 ? 256 : cap * 2;
  struct prev_sample *ns = realloc(prev_samples, new_cap * sizeof(*ns));
  if (!ns)
    return;
  prev_samples = ns;
  prev_cap = new_cap;
}

static struct prev_sample *prev_find(pid_t pid) {
  for (unsigned i = 0; i < prev_count; ++i)
    if (prev_samples[i].pid == pid)
      return &prev_samples[i];
  return NULL;
}

static void prev_store(pid_t pid, double total_time, nvtop_time ts) {
  struct prev_sample *s = prev_find(pid);
  if (s) {
    s->total_time = total_time;
    s->timestamp = ts;
    return;
  }
  prev_reserve(prev_count + 1);
  prev_samples[prev_count].pid = pid;
  prev_samples[prev_count].total_time = total_time;
  prev_samples[prev_count].timestamp = ts;
  prev_count++;
}


static void read_user(pid_t pid, char *out, size_t out_size) {
  int written = snprintf(pid_path, pid_path_size, "/proc/%" PRIdMAX, (intmax_t)pid);
  if (written == pid_path_size) {
    out[0] = '\0';
    return;
  }
  struct stat st;
  if (stat(pid_path, &st) == -1) {
    out[0] = '\0';
    return;
  }
  struct passwd *pw = getpwuid(st.st_uid);
  if (pw && pw->pw_name)
    snprintf(out, out_size, "%s", pw->pw_name);
  else
    snprintf(out, out_size, "%u", (unsigned)st.st_uid);
}

static void read_command(pid_t pid, char **out) {
  *out = NULL;
  int written = snprintf(pid_path, pid_path_size, "/proc/%" PRIdMAX "/cmdline", (intmax_t)pid);
  if (written == pid_path_size)
    return;
  FILE *f = fopen(pid_path, "r");
  if (!f)
    return;
  size_t cap = 256;
  char *buf = malloc(cap);
  if (!buf) {
    fclose(f);
    return;
  }
  size_t total = 0;
  size_t n;
  while ((n = fread(buf + total, 1, cap - total, f)) > 0) {
    total += n;
    if (total == cap) {
      cap *= 2;
      char *nb = realloc(buf, cap);
      if (!nb) {
        free(buf);
        fclose(f);
        return;
      }
      buf = nb;
    }
  }
  fclose(f);
  if (total == 0) {
    free(buf);
    return;
  }
  // Replace NUL separators with spaces, trim trailing
  for (size_t i = 0; i < total; ++i)
    if (buf[i] == '\0')
      buf[i] = ' ';
  while (total > 0 && buf[total - 1] == ' ')
    total--;
  buf[total] = '\0';
  *out = buf;
}

// Parse /proc/[pid]/stat. Returns true on success.
static bool read_stat(pid_t pid, pid_t *ppid, int *priority, int *nice, char *state, long *threads,
                      unsigned long long *vsize, unsigned long long *rss, double *total_time) {
  int written = snprintf(pid_path, pid_path_size, "/proc/%" PRIdMAX "/stat", (intmax_t)pid);
  if (written == pid_path_size)
    return false;
  FILE *f = fopen(pid_path, "r");
  if (!f)
    return false;
  char line[4096];
  bool ok = false;
  if (fgets(line, sizeof(line), f)) {
    // comm is field 2, in parentheses; may contain spaces. Find the last ')'.
    char *open = strchr(line, '(');
    char *close = open ? strrchr(open, ')') : NULL;
    if (open && close) {
      // After ") " comes field 3 (state) onward.
      char *p = close + 1;
      while (*p == ' ')
        p++;
      // Fields after ")": 3 state, 4 ppid, 5 pgrp, 6 session, 7 tty, 8 tpgid,
      // 9 flags, 10 minflt, 11 cminflt, 12 majflt, 13 cmajflt, 14 utime,
      // 15 stime, 16 cutime, 17 cstime, 18 priority, 19 nice, 20 num_threads,
      // 21 itrealvalue, 22 starttime, 23 vsize, 24 rss
      char state_c;
      unsigned long long utime, stime, vsize_v;
      long rss_v;
      // f3 state, f4 ppid, [f5..f13 skipped], f14 utime, f15 stime, [f16,f17 skipped],
      // f18 priority, f19 nice, f20 num_threads, [f21,f22 skipped], f23 vsize, f24 rss
      int fields = sscanf(p,
                          "%c %d %*d %*d %*d %*d %*d %*d %*d %*d %*d %llu %llu %*d %*d %d %d %ld %*d %*d %llu %ld",
                          &state_c, ppid, &utime, &stime, priority, nice, threads, &vsize_v, &rss_v);
      if (fields == 9) {
        double ticks = (double)sysconf(_SC_CLK_TCK);
        double page = (double)sysconf(_SC_PAGESIZE);
        *state = state_c;
        *vsize = vsize_v;
        *rss = (unsigned long long)rss_v * (unsigned long long)page;
        *total_time = (double)(utime + stime) / ticks;
        ok = true;
      }
    }
  }
  fclose(f);
  return ok;
}

struct sys_proc *sys_processes_scan(unsigned *count, const pid_t *gpu_pids, const unsigned *gpu_rates,
                                    const unsigned *gpu_ids, unsigned gpu_count) {
  *count = 0;
  DIR *proc = opendir("/proc");
  if (!proc)
    return NULL;

  unsigned cap = 512;
  struct sys_proc *procs = malloc(cap * sizeof(*procs));
  if (!procs) {
    closedir(proc);
    return NULL;
  }
  unsigned n = 0;

  double ticks = (double)sysconf(_SC_CLK_TCK);
  (void)ticks;

  struct dirent *ent;
  while ((ent = readdir(proc)) != NULL) {
    // Only numeric entries are PIDs
    if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
      continue;
    char *endp;
    long v = strtol(ent->d_name, &endp, 10);
    if (endp == ent->d_name || *endp != '\0')
      continue;
    pid_t pid = (pid_t)v;
    if (pid <= 0)
      continue;

    if (n == cap) {
      cap *= 2;
      struct sys_proc *np = realloc(procs, cap * sizeof(*procs));
      if (!np)
        break;
      procs = np;
    }

    struct sys_proc *sp = &procs[n];
    memset(sp, 0, sizeof(*sp));
    sp->pid = pid;
    sp->command[0] = '\0';

    pid_t ppid;
    int priority, nice;
    char state;
    long threads;
    unsigned long long vsize, rss;
    double total_time;
    if (!read_stat(pid, &ppid, &priority, &nice, &state, &threads, &vsize, &rss, &total_time))
      continue;

    sp->ppid = ppid;
    sp->priority = priority;
    sp->nice = nice;
    sp->state = state;
    sp->threads = threads;
    sp->vsize = vsize;
    sp->rss = rss;
    sp->total_time = total_time;
    read_user(pid, sp->user, sizeof(sp->user));
    // read_command writes into the fixed command[64] buffer
    {
      char *cmd = sp->command;
      read_command(pid, &cmd);
      // read_command may have set cmd to a malloc'd string; copy into fixed buffer
      if (cmd && cmd != sp->command) {
        snprintf(sp->command, sizeof(sp->command), "%s", cmd);
        free(cmd);
      }
    }

    // CPU% delta
    nvtop_time now;
    nvtop_get_current_time(&now);
    struct prev_sample *ps = prev_find(pid);
    if (ps) {
      double dt = nvtop_difftime(ps->timestamp, now);
      if (dt > 0.)
        sp->cpu_pct = 100. * (total_time - ps->total_time) / dt;
      else
        sp->cpu_pct = 0.;
    } else {
      sp->cpu_pct = 0.;
    }
    prev_store(pid, total_time, now);

    // GPU marking
    for (unsigned g = 0; g < gpu_count; ++g) {
      if (gpu_pids[g] == pid) {
        sp->has_gpu = true;
        sp->gpu_id = gpu_ids ? gpu_ids[g] : 0;
        sp->gpu_rate = gpu_rates ? gpu_rates[g] : 0;
        break;
      }
    }
    n++;
  }
  closedir(proc);

  // Drop previous samples for PIDs that no longer exist
  unsigned w = 0;
  for (unsigned i = 0; i < prev_count; ++i) {
    bool alive = false;
    for (unsigned j = 0; j < n; ++j)
      if (procs[j].pid == prev_samples[i].pid) {
        alive = true;
        break;
      }
    if (alive) {
      if (w != i)
        prev_samples[w] = prev_samples[i];
      w++;
    }
  }
  prev_count = w;

  *count = n;
  return procs;
}

void sys_processes_free(struct sys_proc *procs, unsigned count) {
  if (procs) {
    for (unsigned i = 0; i < count; ++i)
      free(procs[i].command);
    free(procs);
  }
  // NOTE: do NOT reset prev_samples here. The previous-tick CPU% state must
  // persist across calls so the next scan can compute deltas. It is cleaned up
  // incrementally inside sys_processes_scan (dead PIDs are dropped there).
}
