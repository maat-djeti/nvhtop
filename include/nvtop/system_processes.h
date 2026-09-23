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

#ifndef NVTOP_SYSTEM_PROCESSES_H_
#define NVTOP_SYSTEM_PROCESSES_H_

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

// The sys_proc struct is defined in djeti/sys_proc.h (the pool's record).
// This header only declares the legacy scan/free API used by the GPU view.

// Enumerate all PIDs from /proc. Returns a malloc'd array of sys_proc (or NULL).
// Computes CPU% as the delta against the previous call. First call yields 0%.
// `gpu_pids`/`gpu_rates`/`gpu_ids`/`gpu_count` optionally mark GPU processes.
struct sys_proc *sys_processes_scan(unsigned *count, const pid_t *gpu_pids, const unsigned *gpu_rates,
                                    const unsigned *gpu_ids, unsigned gpu_count);

// Free a scan result (including per-row command strings).
void sys_processes_free(struct sys_proc *procs, unsigned count);

#endif // NVTOP_SYSTEM_PROCESSES_H_
