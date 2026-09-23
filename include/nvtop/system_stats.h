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

#ifndef NVTOP_SYSTEM_STATS_H_
#define NVTOP_SYSTEM_STATS_H_

#include <stdbool.h>
#include <stddef.h>

#define SYS_STATS_MAX_CORES 512

struct sys_core_stat {
  unsigned long long idle;    // idle + iowait
  unsigned long long total;   // all fields
  unsigned long long user;    // user + nice
  unsigned long long system;  // system + irq + softirq + steal
};

struct sys_stats {
  unsigned core_count;
  struct sys_core_stat cores[SYS_STATS_MAX_CORES];
  // CPU% per core, 0..100 (may be stale from previous tick on first call).
  double core_pct[SYS_STATS_MAX_CORES];
  double core_user_pct[SYS_STATS_MAX_CORES];
  double core_sys_pct[SYS_STATS_MAX_CORES];
  // Memory, in KiB.
  unsigned long long mem_total, mem_used, mem_free, mem_available;
  unsigned long long swap_total, swap_used, swap_free;
  // Load averages (1, 5, 15 min).
  double load1, load5, load15;
  // Uptime in seconds.
  double uptime;
  // Task counts.
  unsigned long long tasks_total, tasks_running, tasks_sleeping, tasks_zombie, tasks_stopped;
};

// Fill *out with a fresh sample. CPU% per core is computed as the delta
// against the previous call (first call yields 0). Memory/load/uptime/tasks
// are read directly. Returns the number of cores found (0 on error).
unsigned sys_stats_read(struct sys_stats *out);

#endif // NVTOP_SYSTEM_STATS_H_
