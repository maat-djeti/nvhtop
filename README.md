# nvhtop

nvhtop is nvtop extended with an htop-style full-system process monitor:
a default table of **every** process on the machine (sourced from `/proc`,
not just GPU contexts) plus a per-core CPU/memory block, alongside the
original GPU-process view.

## Documentation

- **[nvhtop.md](nvhtop.md)** — what was added to turn nvtop into nvhtop:
  the `/proc` process pool, the system stats block, UI changes, build and test.
- **[nvtop-README.md](nvtop-README.md)** — the original upstream nvtop
  README: features, supported GPUs, build instructions, and usage.

## Screenshot

![nvhtop interface](/screenshot/NVHTOP.png)
