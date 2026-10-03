# Coding Standards

These rules keep the kernel readable, portable, and safe. Formatting is enforced by `.clang-format` and `.editorconfig`.

## Language and tooling

- Kernel C is **C11**, freestanding (`-ffreestanding`), no standard library.
- Assembly uses **NASM** syntax, Intel flavor.
- Builds must be warning-free: `-Wall -Wextra -Werror`.
- Run `make format` before committing.

## Formatting

- 4 spaces, no tabs (tabs only in Makefiles).
- 100 column limit.
- Linux-style braces: opening brace on the same line, except for function bodies.
- LF line endings, final newline, no trailing whitespace.

## Naming

| Thing | Style | Example |
|---|---|---|
| Functions, variables | `snake_case` | `pmm_alloc_frame` |
| Types | `snake_case_t` | `page_table_t` |
| Macros, constants | `UPPER_SNAKE_CASE` | `PAGE_SIZE` |
| Public symbols | subsystem prefix | `vmm_map_page`, `sched_yield` |
| Header guards | `MALEOS_<PATH>_H` | `MALEOS_MM_PMM_H` |

## Safety rules

- Use fixed-width types (`uint64_t`, `size_t`) for anything with a defined size.
- Every function that can fail returns a status code; callers must check it.
- No unbounded copies or string functions. Always pass explicit lengths.
- Validate every pointer and length that crosses the syscall boundary.
- Keep interrupt handlers short; defer heavy work.
- Do not use floating point or SSE in kernel code (the build flags disable it).
- Document locking rules for any shared data structure. Use `spin_lock_irqsave` for data touched from interrupt handlers; never sleep while holding a spinlock; respect the lock order heap → vmm → pmm.
- Mutexes and semaphores are for thread context only, never interrupt handlers.
- Mark intentionally unused values explicitly and avoid hidden global state.

## Architecture boundaries

- Machine-specific code lives under `src/arch/<arch>/` behind the HAL. Core kernel code must not contain inline assembly or hardware port access outside the HAL.
- Subsystems talk through their public headers in `src/include/`, never by reaching into each other's internals.

## Comments and documentation

- Explain **why**, not what. The code already says what.
- Every public function gets a short comment describing its contract (inputs, outputs, errors).
- Update `README.md` and `docs/` when behavior or structure changes.

## Testing

- `make test` must pass. It boots the ISO headless and checks the serial output.
- New subsystems should print a clear status line to the serial log when they initialize.
- Memory-management changes must keep `mm_selftest()` green, and scheduler/IPC changes must keep `sched_selftest()` green; add checks for new behavior.
