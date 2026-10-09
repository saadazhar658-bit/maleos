# Userland (Phase 6)

Ring-3 programs, a syscall interface, per-process address spaces, an ELF loader, `init` and a shell.

## Privilege transitions

`syscall_init()` enables `EFER.SCE` and programs `STAR`, `LSTAR` and `FMASK` (which clears IF, DF, TF and AC on entry). The entry stub in `src/arch/x86_64/syscall.asm` switches to a dedicated kernel stack, builds a full `interrupt_frame` and calls `syscall_dispatch`. It always returns with `iretq`, never `sysret`, so a non-canonical user RIP can never fault in ring 0.

GDT: `0x08` kernel code, `0x10` kernel data, `0x18` unused (32-bit user code placeholder), `0x20|3` user data, `0x28|3` user code, `0x30` TSS. The scheduler hook updates `TSS.rsp0` and CR3 on every switch to a process thread.

When CPUID reports them, SMEP and SMAP are enabled (QEMU needs `-cpu max`). All kernel access to user memory goes through `uspace_*` helpers that translate page by page and copy through the direct map, so SMAP never has to be relaxed.

## Syscall ABI

`rax` = number, `rdi`, `rsi`, `rdx` = arguments, result in `rax`, `-errno` on failure. Other registers are preserved. The definitions live in `src/include/abi/abi.h`, shared by kernel and user code.

| Group | Calls |
|---|---|
| Process | `exit`, `getpid`, `yield`, `sleep_ms`, `uptime_ms`, `spawn`, `wait`, `sbrk`, `procinfo`, `meminfo`, `getrandom`, `poweroff` |
| Files | `open`, `close`, `read`, `write`, `lseek`, `stat`, `lstat`, `fstat`, `readdir`, `mkdir`, `rmdir`, `unlink`, `symlink`, `readlink`, `chdir`, `getcwd`, `ftruncate` |

`spawn(path, argv, attr)` creates a child running the given ELF (up to 16 arguments of 256 bytes). The optional `abi_spawn_attr` redirects the child's stdout to a file (`O_TRUNC` / `O_APPEND`). `wait(pid, &status)` blocks until the child exits; status is the exit code, or an `EXIT_SIG*` value if it was killed by a fault.

## Address space

| Region | Address | Notes |
|---|---|---|
| Image | `0x400000` upward (min `0x10000`) | Segments mapped by the loader with their own permissions |
| Heap | after the image | `sbrk`, at most 2048 pages |
| Stack | top at `0x00007FFFFFFFF000` minus a random 0..511 pages | 16 pages, unmapped guard page below |
| Kernel | PML4 entries 256..511 | Shared by every process; pre-populated so later kernel mappings are visible everywhere |

Each process is limited to 4096 pages. `vmm_space_destroy` frees all user pages, tables and the PML4. Spawn/exit cycles are checked for frame, heap, fd and process-slot leaks.

## ELF loader

Accepts static x86-64 `ET_EXEC` files only. Rules: at most 16 program headers, no W+X segment, all ranges overflow-checked and inside the user range, the entry point inside an executable `PT_LOAD`, no two segments sharing a page. `user/user.ld` aligns segments (R+X, R, RW) to pages to satisfy these rules.

## Processes, console and faults

- Each process has a pid, parent, 16-entry fd table (console or VFS), cwd, heap bounds and one kernel thread. Orphans are reaped by a kernel reaper thread. The table holds `PROC_MAX` processes.
- A fault in user mode kills only that process (`[pid N name] killed: ...`) and wakes its waiter.
- The console is a ring buffer fed by a keyboard pump thread and the serial receive IRQ (IRQ4). Reads are cooked: line editing, backspace, Ctrl-C echoed as `^C`. Output goes to serial and VGA.

## Programs

`user/bin` is installed in `/bin` of the initrd: `init`, `sh`, `hello`, `echo`, `cat`, `ls`, `mkdir`, `rmdir`, `rm`, `ln`, `stat`, `ps`, `free`, `uptime`, `uname`, `sleep`. `init` starts `sh`, respawns it when it exits and powers off after three quick failures. The shell has the builtins `cd`, `pwd`, `exit [n]`, `help` and `poweroff`, runs other commands from `/bin` (or an explicit path), supports `>` and `>>`, and prints `[exit status N]` for non-zero exits.

`user/test` is installed in `/tests` and used by the self-test.

## Testing

| Test | What it covers |
|---|---|
| `USER SELFTEST` (every boot) | 496 checks: programs, 18 fault kinds, concurrency and table saturation, orphans, console input, ELF mutations and 600 random header flips, stack protector and ASLR, syscall fuzzing, leak checks |
| `make shell-test` | Boots to the prompt and runs ~40 shell commands over serial, including `exit 3` with respawn and `poweroff` (`--mem`, `--cpu` options) |
| `make input-test` | Types a command through the PS/2 keyboard and checks the shell ran it |
| `CPU=max make test` | Boot test with SMEP and SMAP enabled |

## Limitations

- One CPU. No FPU/SSE state is saved, so user code is built with `-mno-sse`.
- Static `ET_EXEC` only: no dynamic linking, `fork`/`exec`, pipes, signals or `kill`.
- The per-process fd table maps onto the global VFS fd table (61 slots); no `dup` or fd sharing.
- The cwd is textual, so `..` and symlink handling in relative paths is approximate.
- A write that crosses the end of a user mapping returns the bytes written so far; a read returns `-EFAULT`.
- No priority inheritance; ATA/AHCI are polled; ext2 is read-only.
