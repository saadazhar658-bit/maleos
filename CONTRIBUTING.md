# Contributing to Maleos

Thanks for helping build Maleos! This guide covers how to set up, make changes, and get them merged.

## Getting set up

```bash
git clone https://github.com/<your-username>/maleos.git
cd maleos
./scripts/install-deps.sh     # Ubuntu / Debian
make iso && make test         # verify everything works
```

An `x86_64-elf` cross compiler is optional. The Makefile uses it automatically if it is on your `PATH`
(build one with `./scripts/build-toolchain.sh`) and otherwise falls back to the host GCC in freestanding mode.

## Workflow

1. Fork the repo and create a branch: `git checkout -b feature/short-description`
2. Make focused changes. One logical change per pull request.
3. Run `make format` and `make test` before pushing.
4. Open a pull request and fill in the template.

### Branch names

`feature/...`, `fix/...`, `docs/...`, `refactor/...`

### Commit messages

Use the imperative mood with a short subject line (50 characters or fewer is ideal):

```
mm: add physical page frame allocator

Track 4 KiB frames with a bitmap built from the multiboot2 memory map.
```

Prefix with the subsystem when it helps: `boot`, `mm`, `sched`, `vfs`, `drivers`, `hal`, `build`, `docs`.

## Standards

- Code style and rules: [docs/CODING_STANDARDS.md](docs/CODING_STANDARDS.md)
- Where things live: [docs/REPOSITORY_LAYOUT.md](docs/REPOSITORY_LAYOUT.md)
- CI must pass: it builds, packages the ISO, and boot-tests it in headless QEMU.

## Reporting bugs and proposing features

Use the issue templates. Include the serial output and your QEMU and toolchain versions for bugs.

## License

By contributing you agree that your contributions are licensed under the [MIT License](LICENSE).
