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

(This code was updated multiple times using Qwen3.8_27B in stages so I could code 
review and fix bugs. AI agents need oversight - this code was performed under supervision with llama-server running in an LXC with the --agent flag on, and a squid proxy to prevent POST and PUT :-) )
