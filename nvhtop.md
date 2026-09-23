# nvhtop: from nvtop to a full system process monitor

nvhtop is nvtop (v3.3.2 base) extended with an htop-style full-system process
monitor. The change is a single merge commit, `0a9474d`
("nvhtop: merge htop-style system process monitor into nvtop"), on top of
upstream nvtop master.

nvtop's original process table only shows processes that hold a GPU context,
sourced from the GPU driver (NVML/AMBA/i915/...). nvhtop adds a second,
default view: every process on the machine, sourced from `/proc`, with an
htop-style per-core CPU/mem block above the table.

## What was added

New files:

| File | Purpose |
|------|---------|
| `djeti/sys_proc.h` | `struct sys_proc`: one process record |
| `djeti/sys_proc_pool.c` / `.h` | The process pool: producer that walks `/proc`, consumer API |
| `djeti/test_pool.c` | Standalone smoke test for the pool |
| `include/nvtop/system_processes.h` | (legacy) GPU-process sampling helpers |
| `include/nvtop/system_stats.h` | `struct sys_stats` + `sys_stats_read()` |
| `src/system_processes.c` | (legacy) per-PID CPU% sampling from `/proc` |
| `src/system_stats.c` | Per-core CPU, memory, swap, load, uptime, task counts |

Modified: `src/interface.c` (UI, ~590 lines), `src/interface_layout_selection.c`
(layout band for the stats block), `src/interface_options.c`,
`src/interface_setup_win.c`, `src/nvtop.c`, `src/CMakeLists.txt` (target renamed
to `nvhtop`, `djeti/` added to sources and include path), and the interface
headers.

## The process pool (`djeti/sys_proc_pool.c`)

The pool is a single-producer / single-consumer frame buffer guarded by two
semaphores, no mutex. The design invariants (I1–I10) are documented in the
file header:

- **I1** `/proc/<pid>` is the only source of truth. The `records`/`sorted`
  arrays are disposable projections.
- **I2/I3** A PID is *data* in a record, never a key. Records are addressed by
  slot position `0..h`, the h-th live process of the current scan.
- **I4** Each slot holds one individually `calloc`'d `struct sys_proc`.
- **I5/I10** Pointer arrays grow one slot at a time on observed demand.
- **I6** Command line is a fixed `char[64]` in the record; no arena, no
  per-frame malloc.
- **I7** Slots are grow-only until `sys_proc_pool_free`.
- **I8** Two semaphores, strict out-of-phase alternation, no mutex.
- **I9** Records have stable addresses for their whole lifetime, so the
  consumer can hold pointers across the frame handoff.

### Producer (`sys_proc_pool_produce`)

Runs on a dedicated pthread at ~1 Hz. Each cycle:

1. `sem_wait(frame_done)` — never runs concurrently with the consumer's read.
2. `opendir("/proc")`, walk numeric entries in scan order. For each live PID:
   - reuse or `calloc` the slot's record,
   - parse `/proc/<pid>/stat` (anchoring on the last `)` so `comm` values with
     spaces/parens are safe), `/proc/<pid>` stat for the owner, and
     `/proc/<pid>/cmdline` (NULs joined to spaces, truncated to 64 chars),
   - compute CPU% as the delta of `utime`/`stime` against the previous scan,
     valid only when this slot held the *same* PID last time (slots are
     anonymous, so a slot change resets the delta to 0),
   - `mem_pct = rss / total_ram * 100`.
3. Copy the live record pointers into the `sorted` array (one pointer slot
   appended at a time), `qsort_r` by the requested field (PID tiebreak,
   direction from `sort_desc`). The sort key is passed via a `__thread`
   variable since `qsort_r`'s comparator takes no user state on this platform.
4. `sem_post(frame_ready)`.

### Consumer (`sys_proc_pool_frame_begin` / `_frame_done`)

The display thread blocks in `frame_begin` until a frame is ready, then
iterates the sorted pointer array directly and renders. `frame_done` clears
the `fresh` flags and posts `frame_done` so the producer can start the next
cycle. `sys_proc_pool_drain` releases a pending unconsumed frame before the
producer thread is joined, so `pthread_join` cannot deadlock on resize/exit.

### Lifecycle in the UI

`proc_pool_start` / `proc_pool_stop` in `src/interface.c` own the pool and its
thread. The producer thread loops `sys_proc_pool_produce` with a 100 ms
shutdown-check sleep. Stopping sets the running flag, drains, joins, frees.

## System stats block (`src/system_stats.c`)

`sys_stats_read()` fills `struct sys_stats`:

- per-core `idle`/`total`/`user`/`system` counters from `/proc/stat`, with
  CPU% computed as the delta against the previous call (first call yields 0),
- memory/swap in KiB from `/proc/meminfo`,
- load averages, uptime, and task counts.

`draw_sys_stats` in `src/interface.c` throttles reads to 1 Hz (CPU% deltas are
only meaningful over a ~1 s window) and renders an htop-style block:

- a per-core bar grid replicating htop's `Settings_defaultMeters` layout:
  cores split 50/50 left/right, each side a sub-column grid whose width is
  chosen by core count (1/2/4/8 sub-columns; >128 cores collapses to a single
  average bar),
- user portion green, system portion red,
- `Mem` and `Swp` bars, then a `Tasks / Load / Uptime` line.

The layout code (`src/interface_layout_selection.c`) reserves a band between
the GPU plots and the process table for this block, shrinking the plots if
needed while preserving the process table's minimum rows.

## UI integration

- **View modes** (`enum process_view_mode` in
  `include/nvtop/interface_internal_common.h`):
  - `process_view_all` (default): the full-system htop-style table, fed by the
    pool.
  - `process_view_gpu`: the legacy nvtop GPU-processes-only table.
  - Toggle with `a` (all) and `g` (gpu) keys.
- **Columns**: the table reuses nvtop's `enum process_field` and the
  htop-like default set (PID USER PRI NI S VIRT RES CPU% TIME COMMAND).
  GPU columns render `-` for processes without a GPU context.
- **Sort**: F6 opens the sort menu, `+`/`-` flip direction; the sort key is
  passed into the producer so the pool sorts each frame.
- **Kill**: F9 opens the signal menu; Enter sends the signal to the selected
  PID (works in both views).
- **Selection/scrolling**: `j`/`k` and arrows move the selection; the visible
  window follows it (`update_selected_offset_with_window_size`).

## Build

The CMake target is `nvhtop` (`src/CMakeLists.txt`); `djeti/sys_proc_pool.c`
is a source and `djeti/` is on the include path. Standard nvtop build:

```
cmake -B build -G Ninja
cmake --build build
```

## Test

`djeti/test_pool.c` is a standalone smoke test (not wired into CTest):

```
gcc -Wall -Wextra -O2 -Iinclude -Idjeti -o /tmp/pool_test \
    djeti/sys_proc_pool.c djeti/test_pool.c -lpthread
/tmp/pool_test
```

- Test 1: a producer thread emits 3 frames at 1 Hz while a consumer thread
  reads and prints every row (exercises the semaphore handoff under real
  concurrency).
- Test 2: produce one frame and verify the array is sorted by the requested
  key with PID tiebreak.
