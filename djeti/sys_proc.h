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

#ifndef NVTOP_SYS_PROC_H_
#define NVTOP_SYS_PROC_H_

#include <stdbool.h>
#include <sys/types.h>

#include "nvtop/time.h"

// One process record. Individually malloc'd by the pool (I4), stable address
// for its whole lifetime (I9), freed only at pool_free (I7).
//
// The PID is DATA in the record, never a key (I2). The record is addressed
// only by its slot position 0..h (I3).
struct sys_proc {
  // Identity
  pid_t pid;
  pid_t ppid;
  char user[13]; // 12 chars + NUL
  unsigned int uid; // real UID (for USER column colouring)

  // From /proc/[pid]/stat
  char state;          // R/S/D/Z/T/I/...
  int priority;
  int nice;
  long threads;
  unsigned long long vsize; // bytes
  unsigned long long rss;   // bytes (pages * page_size)

  // Raw CPU counters from /proc/[pid]/stat (clock ticks)
  unsigned long long utime;
  unsigned long long stime;
  unsigned long long cutime;
  unsigned long long cstime;

  // Derived values
  double cpu_user_pct;
  double cpu_sys_pct;
  double cpu_pct;    // user + sys
  double mem_pct;    // rss / total_ram * 100
  double total_time; // (utime + stime) in seconds, for the TIME column

  // GPU fields, valid only when has_gpu is true
  bool has_gpu;
  unsigned gpu_id;
  unsigned gpu_rate;

  // CPU% delta state. The record is addressed by PID via the pool's
  // indexed_collection[pid], so this baseline is stable per-pid across scans
  // (a dead lower-pid no longer shifts it, which is what zeroed per-slot
  // deltas and caused the flicker).
  nvtop_time prev_time;
  unsigned long long prev_utime;
  unsigned long long prev_stime;
  bool has_prev;

  // Producer sets true when it samples this record; consumer sets false when
  // it renders it. Safe under the out-of-phase semaphore ordering (producer
  // and consumer never touch a record simultaneously).
  bool fresh;

  // Command line, fixed-width, truncated to fit (I6). No arena, no malloc.
  char command[120];
  // True when command was taken from /proc/<pid>/comm (kernel threads, empty
  // cmdline) rather than the real /proc/<pid>/cmdline. Rendered in the default
  // (white) colour; a real cmdline is rendered green.
  bool cmd_from_comm;
};

#endif // NVTOP_SYS_PROC_H_
