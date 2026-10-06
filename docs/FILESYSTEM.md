# Virtual File System (Phase 5)

## Layers

```text
vfs_open / read / write / lseek / readdir / stat / mkdir / unlink / symlink ...   (fd table, path walk, mounts)
        │
        ▼  struct vnode_ops
  ramfs (read-write)      ext2 (read-only)        ← filesystem drivers
        │                      │
   kernel heap           block layer (hda / sda)
```

`tar_unpack()` fills the root ramfs from the initrd module GRUB loads (`module2 /boot/initrd.tar initrd`).

## Objects

- **vnode** — one per live inode: type, size, mode, link count, reference count, `ops`, private pointer. Every function that returns a vnode returns a *reference*; release it with `vnode_put()`.
- **superblock / fs_type** — a mounted instance and the driver that created it (`mount(dev)` / `unmount(sb)`).
- **mount table** — up to 8 mounts. Each entry records the directory it covers and the filesystem root; `..` at a filesystem root continues from the covered directory.
- **file table** — one global table of 64 descriptors; fds 0–2 are reserved. There are no processes yet, so Phase 6 will turn this into a per-process table with the same open-file structure.

## Behaviour worth knowing

- All calls return `-errno` (Linux numbering). `vfs_read`/`vfs_write` return byte counts.
- One mutex serialises the whole VFS; filesystem drivers need no locking of their own and must never call back into `vfs_*`.
- Path walk: repeated slashes, `.` and `..` work; symlinks are followed (limit 8 deep → `-ELOOP`); a trailing slash requires a directory; names are at most 255 bytes, paths 1023.
- Relative paths resolve from `/` until processes have a working directory.
- `readdir` uses an opaque cookie kept in the open file; `lseek(fd, 0, SEEK_SET)` rewinds a directory.
- Unlinking an open file keeps its data alive until the last `close`.
- Unmount fails with `-EBUSY` while files are open, another mount sits below, or something else still references the mount.
- **Not implemented:** `rename`, hard links, permissions/ownership checks, timestamps, device nodes, writing to ext2.

## ramfs

Files are growable buffers (up to 32 MiB), directories are linked entry lists, symlinks store their target. Sparse writes and growing truncates read back as zeros.

## ext2

Read-only. Block sizes 1–16 KiB, revisions 0 and 1, direct plus single/double/triple indirect blocks, sparse files, fast and slow symlinks, `large_file` sizes, directories spanning many blocks. Mount refuses journal recovery, extents, 64-bit and other unsupported incompat features, and any superblock whose geometry does not fit the device. Every on-disk number is range-checked, so a corrupt image yields `-EIO` rather than a wild access (fuzzed during development). A 16-entry block cache and a live-vnode list keep one inode = one vnode.

## Test images

`scripts/mkdisks.sh` builds `build/disks/ide.img` (1 KiB blocks) and `sata.img` (4 KiB blocks) with `mke2fs -d`, including a 600 kB double-indirect file, a sparse file, a 200-entry directory, long names and fast/slow/dangling symlinks. `scripts/mkinitrd.sh` packs `initrd/` plus generated files (a pattern file, symlinks, a path over 100 bytes to exercise GNU long names).
