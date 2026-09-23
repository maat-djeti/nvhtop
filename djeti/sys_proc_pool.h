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

#ifndef NVTOP_SYS_PROC_POOL_H_
#define NVTOP_SYS_PROC_POOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "nvtop/interface_common.h" // enum process_field
#include "sys_proc.h"

// Opaque handle to the process pool.
struct sys_proc_pool;

// Create the pool. Aborts on OOM.
struct sys_proc_pool *sys_proc_pool_new(void);

// Destroy the pool and free every record and buffer.
void sys_proc_pool_free(struct sys_proc_pool *pool);

// Producer: walk /proc, sample stat+io for every live PID into the active
// list (creating/reusing records), compute CPU%/MEM% deltas, copy the live
// records into the sorted buffer and qsort by `sort_key` (tiebreak pid,
// inverted by `sort_desc`). Posts the frame_ready semaphore when done.
// Blocks on frame_done first, so it never runs concurrently with the
// consumer's read of the sorted buffer.
void sys_proc_pool_produce(struct sys_proc_pool *pool, enum process_field sort_key, bool sort_desc);

// Consumer: block until a frame is ready. Returns the sorted array of record
// pointers and sets *count to the number of live records. The caller iterates
// the array directly. The array and records are valid until the caller invokes
// sys_proc_pool_frame_done, which releases the frame back to the producer.
const struct sys_proc **sys_proc_pool_frame_begin(struct sys_proc_pool *pool, unsigned *count);

// Release the current frame: clear fresh flags and post frame_done so the
// producer can start the next cycle. Must be called exactly once after
// sys_proc_pool_frame_begin.
void sys_proc_pool_frame_done(struct sys_proc_pool *pool);

// Drain any pending frame: if a frame is ready but unconsumed, release it
// (post frame_done) so a blocked consumer unblocks. Call before stopping the
// producer thread to avoid deadlock in pthread_join.
void sys_proc_pool_drain(struct sys_proc_pool *pool);

#endif // NVTOP_SYS_PROC_POOL_H_
