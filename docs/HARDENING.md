# Hardening (Phase 7a)

## Stack protector

Kernel and user code are built with `-fstack-protector-strong -mstack-protector-guard=global`; the canary is the global `__stack_chk_guard`. `random_init()` replaces its compile-time value with a random one (low byte zero, so string overflows cannot write it) right after the IDT is loaded. `kmain` and `random_init` are marked `no_stack_protector` because the canary changes while they run. A mismatch calls `__stack_chk_fail`, which panics in the kernel and exits a user program with status 134.

Each user program gets its own random canary from `getrandom` in `crt0`.

## Randomness

`src/kernel/random.c` seeds a xoshiro256** generator from RDRAND (when CPUID reports it, otherwise from the TSC) and mixes the TSC into every output. It is fast and unpredictable enough for ASLR and canaries, but it is **not a cryptographic generator**. User programs read it with `getrandom(buf, len)` (syscall 28, at most 256 bytes per call).

## ASLR (user space)

| Item | Randomisation |
|---|---|
| Stack top | Slides down by 0..511 pages |
| Heap start (`sbrk` base) | Slides up by 0..255 pages, page aligned |

The image is a fixed-address `ET_EXEC`, so code and data are not randomised. The kernel image is not randomised either: it is linked at a fixed address with `-mcmodel=kernel`, and KASLR would need a relocatable kernel (see Limitations).

## Syscall fuzzer

`user/test/t_fuzz.c` fires random system calls with hostile arguments: kernel addresses, unmapped and page-straddling buffers, NULL, huge lengths, bad descriptors, over-long paths and unknown call numbers. Every result must be a success or a plausible `-errno`, and unknown numbers must return `-ENOSYS`. File-changing calls only touch `/tmp/fz`, and symlink targets stay inside it. The kernel self-test runs 8 seeds of 2500 calls on every boot and then checks for leaked frames, heap, descriptors and processes. `t_fuzz <seed> <iterations>` can also be run from the shell.

The fuzzer already found one real bug: 32-bit arguments such as file descriptors were truncated, so descriptor `1<<32` aliased the console and a `read` blocked forever. Such values are now rejected (`ARG_INT` in `syscall.c`).

## Other tests added

- `t_smash`: overflows a stack buffer; the self-test expects the protector to end it with status 134.
- `t_aslr`: prints its stack and heap addresses; the self-test runs it 24 times and requires them to vary.

## Limitations

- No kernel KASLR and no code ASLR for user programs.
- The random generator is not cryptographically secure.
- The heap never returns frames to the physical allocator, so large temporary files raise its high-water mark. The leak checks account for this with a warm-up run.
