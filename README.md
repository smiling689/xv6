# xv6 (RISC-V) OS Labs

My solutions to the eight xv6-riscv labs in SJTU CS2953 (Operating Systems), Spring 2026. The labs are adapted from MIT 6.S081 / 6.1810.

## Overview

This repository started from the course starter code ([liyaoxuan/CS2953-2024](https://github.com/liyaoxuan/CS2953-2024), commit `0dd3f8f "init"`). I solved the labs one after another on the single `main` branch, so later labs build on earlier ones. For example, the COW lab reuses the per-CPU allocator from the lock lab, and the mmap page-fault path sits next to the COW handler.

Each lab's code is compiled only when `conf/lab.mk` selects that lab (`#ifdef LAB_<NAME>`). The tree is currently set to `LAB=mmap`, the last lab.

## What I Implemented

The scores are the `make grade` output that I copied into each lab's report file (`lab-*-report.txt` in the repo root). Each was recorded when that lab was finished.

| # | Lab | What I built | Key files | Score | Report |
|---|---|---|---|---|---|
| 1 | util + syscall | `sleep` and `pingpong` user programs; a `trace` syscall (per-process syscall mask, inherited across `fork`); a `sysinfo` syscall (free memory and process count) | `user/sleep.c`, `user/pingpong.c`, `kernel/syscall.c`, `kernel/sysproc.c`, `kernel/kalloc.c`, `kernel/proc.c` | 80/80 | [notes](lab-util-syscall-report.txt) |
| 2 | traps | `backtrace()` that walks saved frame pointers; `sigalarm` / `sigreturn` periodic user handlers. The handler saves and restores the full trapframe and does not re-enter itself. | `kernel/printf.c`, `kernel/trap.c`, `kernel/sysproc.c`, `kernel/proc.h` | 94/94 | [notes](lab-traps-report.txt) |
| 3 | thread | User-level threads with a callee-saved context switch in assembly; per-bucket mutexes for the pthread hash table (`ph`); a reusable round-based pthread barrier | `user/uthread.c`, `user/uthread_switch.S`, `notxv6/ph.c`, `notxv6/barrier.c` | 59/59 | [notes](lab-thread-report.txt) |
| 4 | lock | Per-CPU free lists in the page allocator. When a CPU runs out, it steals half of another CPU's list while holding only one lock at a time. The buffer cache uses 13 hash buckets with per-bucket locks; a global lock is taken only on a miss, and the lookup is re-checked so a block is never cached twice. | `kernel/kalloc.c`, `kernel/bio.c`, `kernel/buf.h` | 94/94 | [notes](lab-lock-report.txt) |
| 5 | pgtbl | Read-only `USYSCALL` page shared with user space for a faster `getpid()`; `vmprint()` page-table dump; `pgaccess()` syscall that reads and clears `PTE_A` | `kernel/proc.c`, `kernel/vm.c`, `kernel/exec.c`, `kernel/sysproc.c`, `kernel/riscv.h` | 45/45 | [notes](lab-pgtbl-report.txt) |
| 6 | cow | Copy-on-write `fork`: a software `PTE_COW` bit (RSW), per-page reference counts in the allocator, a store-page-fault handler, and COW handling in `copyout` | `kernel/vm.c`, `kernel/kalloc.c`, `kernel/trap.c`, `kernel/riscv.h` | 114/114 | [notes](lab-cow-report.txt) |
| 7 | fs | Large files via a doubly-indirect block (11 direct + 1 indirect + 1 double-indirect), with matching `itrunc`; `symlink` syscall with `O_NOFOLLOW` and a 10-level follow limit to break cycles | `kernel/fs.c`, `kernel/fs.h`, `kernel/file.h`, `kernel/sysfile.c`, `kernel/stat.h`, `kernel/fcntl.h` | 104/104 | [notes](lab-fs-report.txt) |
| 8 | mmap | `mmap` / `munmap` with a per-process VMA table (16 entries), lazy loading through page faults, write-back of `MAP_SHARED` pages, partial unmapping from the head or tail, and VMA copying on `fork` / cleanup on `exit` | `kernel/sysfile.c`, `kernel/trap.c`, `kernel/proc.c`, `kernel/proc.h` | 144/144 | [notes](lab-mmap-report.txt) |

The reports linked in the table are my notes for each lab, written in Chinese. They cover the design, the bugs I ran into, and the full `make grade` output.

## Design Notes

- **COW + per-CPU allocator.** Reference counts live in one array indexed by `(pa - KERNBASE) / PGSIZE`, protected by its own lock. `kfree` only returns a page to the current CPU's free list once its count reaches 0. At boot, `freerange` sets each count to 1 before calling `kfree`, so the first free brings it to 0.
- **COW and `copyout`.** When the kernel writes into user memory, the hardware raises no user page fault. So `copyout` checks for COW pages itself and copies them before writing.
- **mmap placement.** Mappings go in the high part of the address space, just below `TRAPFRAME`, not by growing `p->sz`. This means `uvmcopy` never touches pages that have not been loaded yet. A store fault is checked for COW first and then handed to the mmap handler.
- **Faster `bigfile`.** `balloc` uses a next-fit hint instead of scanning the bitmap from the start each time. New data blocks skip zeroing when they are about to be fully overwritten. The block is re-zeroed if that write fails. Without these changes, `bigfile` sometimes exceeded the grader's timeout.

## Build & Test

Requirements: a RISC-V GNU toolchain (`riscv64-unknown-elf-` or `riscv64-linux-gnu-`), `qemu-system-riscv64`, and Python 3 for the grader.

```bash
# Choose the lab to build and grade
echo "LAB=cow" > conf/lab.mk      # one of: syscall traps thread lock pgtbl cow fs mmap

make qemu          # boot xv6 (Ctrl-a x to quit)
make grade         # run ./grade-lab-$(LAB)
```

Earlier labs' code is still there, but it is behind `#ifdef LAB_<NAME>`, so switch `conf/lab.mk` to build or grade an earlier lab. I have not re-graded the earlier labs on the final tree.

`grade-lab-syscall` is a small wrapper I added; it calls `grade-lab-util-syscall`.

The build packs the stock `README` file into `fs.img`, so that file has to stay in the repo.

## Acknowledgements

- SJTU CS2953 course staff, for the lab handouts, starter code, and graders.
- [MIT 6.S081 / 6.1810](https://pdos.csail.mit.edu/6.1810/) and the [xv6-riscv](https://github.com/mit-pdos/xv6-riscv) authors (Russ Cox, Frans Kaashoek, Robert Morris). xv6 is MIT-licensed; see `LICENSE`.
