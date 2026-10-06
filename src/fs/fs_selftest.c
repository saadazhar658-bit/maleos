#include "fs/fs_boot.h"
#include "fs/tar.h"
#include "fs/vfs.h"
#include "kernel/printk.h"
#include "kernel/string.h"
#include "mm/heap.h"

static int checks;

#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond))                                                                               \
            kpanic("fs selftest failed: %s (%s:%d)", what, __FILE__, __LINE__);                    \
    } while (0)

#define CHECK_RC(expr, want, what)                                                                 \
    do {                                                                                           \
        long long rc_ = (expr);                                                                    \
        checks++;                                                                                  \
        if (rc_ != (long long)(want))                                                              \
            kpanic("fs selftest failed: %s: got %lld, want %lld (%s:%d)", what, rc_,               \
                   (long long)(want), __FILE__, __LINE__);                                         \
    } while (0)

/* Must match the generators in scripts/mkdisks.sh and scripts/mkinitrd.sh. */
static uint8_t pattern_byte(uint64_t i)
{
    return (uint8_t)((i * 31 + (i >> 8)) & 0xFF);
}

static uint64_t heap_used(void)
{
    struct heap_stats hs;
    heap_get_stats(&hs);
    return hs.used_bytes;
}

static int write_str(const char *path, const char *s)
{
    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return fd;
    int64_t n = vfs_write(fd, s, strlen(s));
    vfs_close(fd);
    return n == (int64_t)strlen(s) ? 0 : -EIO;
}

/* True if the file holds exactly the string `s`. */
static bool file_is(const char *path, const char *s)
{
    char buf[512];
    int64_t n = vfs_read_file(path, buf, sizeof(buf));
    return n == (int64_t)strlen(s) && memcmp(buf, s, (size_t)n) == 0;
}

/* Read a whole file in odd-sized chunks and compare against the generator pattern. */
static bool file_matches_pattern(const char *path, uint64_t size, size_t chunk)
{
    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0)
        return false;
    uint8_t *buf = kmalloc(chunk);
    uint64_t pos = 0;
    bool ok = buf != NULL;
    while (ok && pos < size) {
        int64_t n = vfs_read(fd, buf, chunk);
        if (n <= 0 || (uint64_t)n > size - pos) {
            ok = false;
            break;
        }
        for (int64_t i = 0; i < n; i++) {
            if (buf[i] != pattern_byte(pos + (uint64_t)i)) {
                ok = false;
                break;
            }
        }
        pos += (uint64_t)n;
    }
    if (ok)
        ok = vfs_read(fd, buf, chunk) == 0; /* and nothing beyond the expected size */
    kfree(buf);
    vfs_close(fd);
    return ok;
}

static int count_dir(const char *path, const char *must_have, bool *found)
{
    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0)
        return fd;
    int n = 0;
    struct dirent de;
    if (found)
        *found = false;
    while (vfs_readdir(fd, &de) == 1) {
        n++;
        if (found && must_have && strcmp(de.name, must_have) == 0)
            *found = true;
    }
    vfs_close(fd);
    return n;
}

static void join(char *out, const char *a, const char *b)
{
    strlcpy(out, a, VFS_PATH_MAX);
    strlcpy(out + strlen(out), b, VFS_PATH_MAX - strlen(out));
}

/* ---------- ramfs and the generic VFS paths ---------- */

static void test_ramfs_files(void)
{
    CHECK_RC(vfs_mkdir("/tmp/t1"), 0, "mkdir");
    CHECK_RC(vfs_mkdir("/tmp/t1"), -EEXIST, "mkdir existing");
    CHECK_RC(vfs_mkdir("/tmp/nope/x"), -ENOENT, "mkdir without parent");
    CHECK_RC(vfs_mkdir_p("/tmp/t1/a/b/c"), 0, "mkdir -p");
    CHECK_RC(vfs_mkdir_p("/tmp/t1/a/b/c"), 0, "mkdir -p again");

    CHECK_RC(write_str("/tmp/t1/hello", "hello world"), 0, "create+write");
    CHECK(file_is("/tmp/t1/hello", "hello world"), "read back");

    struct stat st;
    CHECK_RC(vfs_stat("/tmp/t1/hello", &st), 0, "stat file");
    CHECK(S_ISREG(st.mode) && st.size == 11 && st.nlink == 1, "stat file fields");
    CHECK_RC(vfs_stat("/tmp/t1", &st), 0, "stat dir");
    CHECK(S_ISDIR(st.mode) && st.nlink == 3, "dir link count (. + parent entry + a/)");

    int fd = vfs_open("/tmp/t1/hello", O_RDWR);
    char buf[64];
    CHECK(fd >= VFS_FD_FIRST, "open returns fd >= 3");
    CHECK_RC(vfs_read(fd, buf, 5), 5, "read 5");
    CHECK(memcmp(buf, "hello", 5) == 0, "read content");
    CHECK_RC(vfs_lseek(fd, 0, SEEK_CUR), 5, "lseek cur");
    CHECK_RC(vfs_lseek(fd, -5, SEEK_END), 6, "lseek end");
    CHECK_RC(vfs_read(fd, buf, 64), 5, "read clipped to EOF");
    CHECK_RC(vfs_read(fd, buf, 64), 0, "read at EOF");
    CHECK_RC(vfs_lseek(fd, -100, SEEK_SET), -EINVAL, "negative seek");
    CHECK_RC(vfs_lseek(fd, 0, 9), -EINVAL, "bad whence");
    CHECK_RC(vfs_lseek(fd, 6, SEEK_SET), 6, "seek set");
    CHECK_RC(vfs_write(fd, "WORLD", 5), 5, "overwrite");
    CHECK(file_is("/tmp/t1/hello", "hello WORLD"), "overwrite visible");

    /* Sparse write: the gap must read as zeros. */
    CHECK_RC(vfs_lseek(fd, 100, SEEK_SET), 100, "seek past end");
    CHECK_RC(vfs_write(fd, "X", 1), 1, "write past end");
    CHECK_RC(vfs_fstat(fd, &st), 0, "fstat");
    CHECK(st.size == 101, "size after sparse write");
    uint8_t gap[101];
    CHECK_RC(vfs_lseek(fd, 0, SEEK_SET), 0, "rewind");
    CHECK_RC(vfs_read(fd, gap, 101), 101, "read all");
    bool zeros = true;
    for (int i = 11; i < 100; i++)
        zeros &= gap[i] == 0;
    CHECK(zeros && gap[100] == 'X', "gap is zero-filled");

    CHECK_RC(vfs_ftruncate(fd, 5), 0, "shrink");
    CHECK_RC(vfs_ftruncate(fd, 20), 0, "grow");
    CHECK_RC(vfs_lseek(fd, 0, SEEK_SET), 0, "rewind 2");
    CHECK_RC(vfs_read(fd, gap, 20), 20, "read grown");
    zeros = memcmp(gap, "hello", 5) == 0;
    for (int i = 5; i < 20; i++)
        zeros &= gap[i] == 0; /* shrinking then growing must not resurrect old data */
    CHECK(zeros, "shrink then grow zero-fills");
    CHECK_RC(vfs_close(fd), 0, "close");
    CHECK_RC(vfs_close(fd), -EBADF, "double close");

    /* Access modes, O_APPEND, O_TRUNC, O_EXCL. */
    fd = vfs_open("/tmp/t1/hello", O_RDONLY);
    CHECK_RC(vfs_write(fd, "x", 1), -EBADF, "write on read-only fd");
    vfs_close(fd);
    fd = vfs_open("/tmp/t1/hello", O_WRONLY);
    CHECK_RC(vfs_read(fd, buf, 1), -EBADF, "read on write-only fd");
    CHECK_RC(vfs_ftruncate(fd, 0), 0, "ftruncate 0");
    vfs_close(fd);

    CHECK_RC(write_str("/tmp/t1/app", "abc"), 0, "app create");
    fd = vfs_open("/tmp/t1/app", O_WRONLY | O_APPEND);
    CHECK_RC(vfs_write(fd, "def", 3), 3, "append");
    vfs_lseek(fd, 0, SEEK_SET);
    CHECK_RC(vfs_write(fd, "ghi", 3), 3, "append ignores position");
    vfs_close(fd);
    CHECK(file_is("/tmp/t1/app", "abcdefghi"), "appended content");

    fd = vfs_open("/tmp/t1/app", O_WRONLY | O_TRUNC);
    vfs_close(fd);
    CHECK(file_is("/tmp/t1/app", ""), "O_TRUNC");
    CHECK_RC(vfs_open("/tmp/t1/app", O_WRONLY | O_CREAT | O_EXCL), -EEXIST, "O_EXCL");
    CHECK_RC(vfs_open("/tmp/t1/none", O_RDONLY), -ENOENT, "open missing");
    CHECK_RC(vfs_open("/tmp/t1", O_WRONLY), -EISDIR, "write-open a directory");
    CHECK_RC(vfs_open("/tmp/t1/hello", O_RDONLY | O_DIRECTORY), -ENOTDIR, "O_DIRECTORY on file");
    CHECK_RC(vfs_open("/tmp/t1/hello", 3), -EINVAL, "bad access mode");
    CHECK_RC(vfs_open("", O_RDONLY), -ENOENT, "empty path");

    fd = vfs_open("/tmp/t1", O_RDONLY);
    CHECK_RC(vfs_read(fd, buf, 1), -EISDIR, "read a directory");
    CHECK_RC(vfs_ftruncate(fd, 0), -EBADF, "ftruncate dir via read-only fd");
    vfs_close(fd);
    fd = vfs_open("/tmp/t1/hello", O_RDONLY);
    struct dirent de;
    CHECK_RC(vfs_readdir(fd, &de), -ENOTDIR, "readdir on a file");
    vfs_close(fd);
    CHECK_RC(vfs_read(99, buf, 1), -EBADF, "read bad fd");
    CHECK_RC(vfs_read(-1, buf, 1), -EBADF, "read negative fd");
    CHECK_RC(vfs_read(1, buf, 1), -EBADF, "fd 1 is reserved");
}

static void test_ramfs_large(void)
{
    /* 200 KB written in 777-byte pieces: exercises growth and unaligned writes. */
    int fd = vfs_open("/tmp/t1/large", O_RDWR | O_CREAT);
    uint8_t buf[777];
    uint64_t pos = 0;
    const uint64_t total = 200000;
    while (pos < total) {
        size_t n = total - pos < sizeof(buf) ? (size_t)(total - pos) : sizeof(buf);
        for (size_t i = 0; i < n; i++)
            buf[i] = pattern_byte(pos + i);
        CHECK_RC(vfs_write(fd, buf, n), (long long)n, "large write");
        pos += n;
    }
    vfs_close(fd);
    CHECK(file_matches_pattern("/tmp/t1/large", total, 1000), "large file pattern");
    CHECK(file_matches_pattern("/tmp/t1/large", total, 4096), "large file pattern, other chunk");
    CHECK_RC(vfs_unlink("/tmp/t1/large"), 0, "unlink large");
}

static void test_paths(void)
{
    CHECK_RC(vfs_mkdir_p("/tmp/p/q"), 0, "setup");
    CHECK_RC(write_str("/tmp/p/q/f", "F"), 0, "setup file");

    CHECK(file_is("/tmp//p///q/f", "F"), "repeated slashes");
    CHECK(file_is("/tmp/./p/./q/./f", "F"), "dot components");
    CHECK(file_is("/tmp/p/q/../q/../../p/q/f", "F"), "dotdot components");
    CHECK(file_is("/../../tmp/p/q/f", "F"), "dotdot at root stays at root");
    CHECK(file_is("tmp/p/q/f", "F"), "relative path resolves from root");

    struct stat a, b;
    CHECK_RC(vfs_stat("/tmp/p/q/", &a), 0, "trailing slash on dir");
    CHECK_RC(vfs_stat("/tmp/p/q/.", &b), 0, "dot at end");
    CHECK(a.ino == b.ino, "dir and dir/. are the same inode");
    CHECK_RC(vfs_stat("/tmp/p/q/f/", &a), -ENOTDIR, "trailing slash on file");
    CHECK_RC(vfs_stat("/tmp/p/q/f/x", &a), -ENOTDIR, "file used as directory");
    CHECK_RC(vfs_stat("/", &a), 0, "stat root");
    CHECK(S_ISDIR(a.mode), "root is a directory");
    CHECK_RC(vfs_stat("/tmp/p/zzz", &a), -ENOENT, "missing leaf");
    CHECK_RC(vfs_stat("/tmp/zzz/q", &a), -ENOENT, "missing middle");

    char longname[300];
    memset(longname, 'n', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = 0;
    char path[VFS_PATH_MAX + 64];
    join(path, "/tmp/p/", longname);
    CHECK_RC(vfs_open(path, O_RDONLY), -ENAMETOOLONG, "component too long");
    CHECK_RC(vfs_open(path, O_WRONLY | O_CREAT), -ENAMETOOLONG, "create with long name");

    char name255[VFS_NAME_MAX + 1];
    memset(name255, 'm', VFS_NAME_MAX);
    name255[VFS_NAME_MAX] = 0;
    join(path, "/tmp/p/", name255);
    CHECK_RC(write_str(path, "max"), 0, "255-byte name works");
    CHECK(file_is(path, "max"), "255-byte name read");
    CHECK_RC(vfs_unlink(path), 0, "255-byte name unlink");

    strlcpy(path, "/tmp/", sizeof(path));
    for (int i = 0; i < 600; i++)
        strlcpy(path + strlen(path), "a/", sizeof(path) - strlen(path));
    CHECK_RC(vfs_open(path, O_WRONLY | O_CREAT), -ENAMETOOLONG, "path too long");

    CHECK_RC(vfs_mkdir("/"), -EINVAL, "mkdir /");
    CHECK_RC(vfs_unlink("/"), -EINVAL, "unlink /");
    CHECK_RC(vfs_mkdir("/tmp/p/q/."), -EEXIST, "mkdir dot");
    CHECK_RC(vfs_rmdir("/tmp/p/q/.."), -EEXIST, "rmdir dotdot");
    CHECK_RC(vfs_unlink("/tmp/p/q/f/"), -ENOTDIR, "unlink file with trailing slash");

    /* Removal rules. */
    CHECK_RC(vfs_rmdir("/tmp/p"), -ENOTEMPTY, "rmdir non-empty");
    CHECK_RC(vfs_unlink("/tmp/p/q"), -EISDIR, "unlink a directory");
    CHECK_RC(vfs_rmdir("/tmp/p/q/f"), -ENOTDIR, "rmdir a file");
    CHECK_RC(vfs_unlink("/tmp/p/q/f"), 0, "unlink");
    CHECK_RC(vfs_unlink("/tmp/p/q/f"), -ENOENT, "unlink again");
    CHECK_RC(vfs_rmdir("/tmp/p/q"), 0, "rmdir");
    CHECK_RC(vfs_rmdir("/tmp/p"), 0, "rmdir parent");
    CHECK_RC(vfs_stat("/tmp/p", &a), -ENOENT, "gone");

    /* An open file survives unlink; the data stays readable until the last close. */
    CHECK_RC(write_str("/tmp/ghost", "boo"), 0, "ghost");
    int fd = vfs_open("/tmp/ghost", O_RDONLY);
    CHECK_RC(vfs_unlink("/tmp/ghost"), 0, "unlink open file");
    char buf[8];
    CHECK_RC(vfs_read(fd, buf, 8), 3, "read unlinked file");
    CHECK(memcmp(buf, "boo", 3) == 0, "unlinked content");
    CHECK_RC(vfs_close(fd), 0, "close unlinked");
    CHECK_RC(vfs_stat("/tmp/ghost", &a), -ENOENT, "still gone");
}

static void test_symlinks(void)
{
    CHECK_RC(vfs_mkdir_p("/tmp/s/sub"), 0, "setup");
    CHECK_RC(write_str("/tmp/s/sub/target", "T"), 0, "target");
    CHECK_RC(vfs_symlink("sub/target", "/tmp/s/rel"), 0, "relative link");
    CHECK_RC(vfs_symlink("/tmp/s/sub/target", "/tmp/s/abs"), 0, "absolute link");
    CHECK_RC(vfs_symlink("sub", "/tmp/s/dirlink"), 0, "link to a directory");
    CHECK_RC(vfs_symlink("missing", "/tmp/s/dangling"), 0, "dangling link");
    CHECK_RC(vfs_symlink("x", "/tmp/s/rel"), -EEXIST, "symlink over existing");
    CHECK_RC(vfs_symlink("", "/tmp/s/empty"), -EINVAL, "empty target");

    CHECK(file_is("/tmp/s/rel", "T"), "follow relative");
    CHECK(file_is("/tmp/s/abs", "T"), "follow absolute");
    CHECK(file_is("/tmp/s/dirlink/target", "T"), "link in the middle of a path");
    CHECK(file_is("/tmp/s/dirlink/../sub/target", "T"), "dotdot after a link");

    struct stat st;
    CHECK_RC(vfs_stat("/tmp/s/rel", &st), 0, "stat follows");
    CHECK(S_ISREG(st.mode) && st.size == 1, "stat sees the target");
    CHECK_RC(vfs_lstat("/tmp/s/rel", &st), 0, "lstat");
    CHECK(S_ISLNK(st.mode) && st.size == 10, "lstat sees the link");
    CHECK_RC(vfs_stat("/tmp/s/dangling", &st), -ENOENT, "stat dangling");
    CHECK_RC(vfs_lstat("/tmp/s/dangling", &st), 0, "lstat dangling");
    CHECK_RC(vfs_stat("/tmp/s/dirlink/", &st), 0, "trailing slash follows a final link");
    CHECK(S_ISDIR(st.mode), "dirlink/ is a directory");

    char buf[64];
    CHECK_RC(vfs_readlink("/tmp/s/rel", buf, sizeof(buf)), 10, "readlink length");
    CHECK(strcmp(buf, "sub/target") == 0, "readlink content");
    CHECK_RC(vfs_readlink("/tmp/s/rel", buf, 3), 3, "readlink truncates");
    CHECK(memcmp(buf, "sub", 3) == 0, "readlink truncated content");
    CHECK_RC(vfs_readlink("/tmp/s/sub/target", buf, sizeof(buf)), -EINVAL, "readlink on a file");

    CHECK_RC(vfs_open("/tmp/s/rel", O_RDONLY | O_NOFOLLOW), -ELOOP, "O_NOFOLLOW on a link");
    CHECK_RC(vfs_open("/tmp/s/dangling", O_RDONLY), -ENOENT, "open dangling");
    int fd = vfs_open("/tmp/s/newfile_via_link", O_RDONLY);
    CHECK_RC(fd, -ENOENT, "open missing");

    /* Create through a link: O_CREAT on a dangling link creates nothing in this VFS
     * (the target's directory entry is missing), so only existing targets are writable. */
    fd = vfs_open("/tmp/s/rel", O_WRONLY | O_TRUNC);
    CHECK(fd >= 0, "write through link");
    CHECK_RC(vfs_write(fd, "U", 1), 1, "write through link data");
    vfs_close(fd);
    CHECK(file_is("/tmp/s/sub/target", "U"), "target modified");

    /* Loops and depth limit. */
    CHECK_RC(vfs_symlink("/tmp/s/lb", "/tmp/s/la"), 0, "loop a");
    CHECK_RC(vfs_symlink("/tmp/s/la", "/tmp/s/lb"), 0, "loop b");
    CHECK_RC(vfs_open("/tmp/s/la", O_RDONLY), -ELOOP, "two-link loop");
    CHECK_RC(vfs_symlink("/tmp/s/self", "/tmp/s/self"), 0, "self link");
    CHECK_RC(vfs_stat("/tmp/s/self", &st), -ELOOP, "self loop");

    CHECK_RC(vfs_symlink("/tmp/s/sub/target", "/tmp/s/c0"), 0, "chain 0");
    char from[32], to[32];
    for (int i = 1; i <= 9; i++) {
        strlcpy(from, "/tmp/s/c0", sizeof(from));
        from[8] = (char)('0' + i - 1);
        strlcpy(to, "/tmp/s/c0", sizeof(to));
        to[8] = (char)('0' + i);
        CHECK_RC(vfs_symlink(from, to), 0, "chain link");
        if (i == 5)
            CHECK(file_is("/tmp/s/c5", "U"), "chain of 6 links resolves");
    }
    CHECK_RC(vfs_open("/tmp/s/c9", O_RDONLY), -ELOOP, "chain of 10 links is too deep");

    /* Unlinking a link removes the link, not the target. */
    CHECK_RC(vfs_unlink("/tmp/s/rel"), 0, "unlink link");
    CHECK(file_is("/tmp/s/sub/target", "U"), "target survives");
    CHECK_RC(vfs_lstat("/tmp/s/rel", &st), -ENOENT, "link gone");
}

static void test_readdir(void)
{
    CHECK_RC(vfs_mkdir_p("/tmp/rd"), 0, "setup");
    char path[64];
    for (int i = 0; i < 40; i++) {
        strlcpy(path, "/tmp/rd/file", sizeof(path));
        path[12] = (char)('a' + i / 10);
        path[13] = (char)('0' + i % 10);
        path[14] = 0;
        CHECK_RC(write_str(path, "x"), 0, "populate");
    }
    bool found;
    CHECK_RC(count_dir("/tmp/rd", "filec5", &found), 42, "40 files plus . and ..");
    CHECK(found, "readdir finds a name");

    /* The cookie survives interleaved work and the directory can be rewound. */
    int fd = vfs_open("/tmp/rd", O_RDONLY);
    struct dirent de;
    CHECK_RC(vfs_readdir(fd, &de), 1, "first entry");
    CHECK(strcmp(de.name, ".") == 0 && de.type == VT_DIR, "starts with dot");
    CHECK_RC(vfs_readdir(fd, &de), 1, "second entry");
    CHECK(strcmp(de.name, "..") == 0, "then dotdot");
    for (int i = 0; i < 10; i++)
        CHECK_RC(vfs_readdir(fd, &de), 1, "entries");
    CHECK_RC(vfs_lseek(fd, 0, SEEK_SET), 0, "rewinddir");
    CHECK_RC(vfs_readdir(fd, &de), 1, "after rewind");
    CHECK(strcmp(de.name, ".") == 0, "rewound to start");
    CHECK_RC(vfs_lseek(fd, 5, SEEK_SET), -ESPIPE, "arbitrary seek on a directory");
    vfs_close(fd);

    for (int i = 0; i < 40; i++) {
        strlcpy(path, "/tmp/rd/file", sizeof(path));
        path[12] = (char)('a' + i / 10);
        path[13] = (char)('0' + i % 10);
        path[14] = 0;
        CHECK_RC(vfs_unlink(path), 0, "cleanup");
    }
    CHECK_RC(count_dir("/tmp/rd", NULL, NULL), 2, "empty directory has . and ..");
    CHECK_RC(vfs_rmdir("/tmp/rd"), 0, "cleanup dir");
}

static void test_fd_limits_and_leaks(void)
{
    CHECK_RC(vfs_open_file_count(), 0, "no files open between tests");

    int fds[VFS_MAX_FDS];
    int n = 0;
    for (;;) {
        int fd = vfs_open("/etc/motd", O_RDONLY);
        if (fd < 0) {
            CHECK_RC(fd, -EMFILE, "table full");
            break;
        }
        CHECK(n == 0 || fd == fds[n - 1] + 1, "lowest free fd is handed out");
        fds[n++] = fd;
    }
    CHECK_RC(n, VFS_MAX_FDS - VFS_FD_FIRST, "fd table capacity");
    CHECK_RC(vfs_close(fds[10]), 0, "free one slot");
    int again = vfs_open("/etc/motd", O_RDONLY);
    CHECK_RC(again, fds[10], "freed slot is reused");
    for (int i = 0; i < n; i++)
        CHECK_RC(vfs_close(fds[i]), 0, "close all");
    CHECK_RC(vfs_open_file_count(), 0, "all closed");

    /* Creating and deleting many things must return every byte to the heap. */
    uint64_t before = heap_used();
    for (int round = 0; round < 3; round++) {
        CHECK_RC(vfs_mkdir_p("/tmp/leak/d1/d2"), 0, "leak: dirs");
        for (int i = 0; i < 50; i++) {
            char p[64] = "/tmp/leak/d1/f";
            p[14] = (char)('0' + i / 10);
            p[15] = (char)('0' + i % 10);
            p[16] = 0;
            CHECK_RC(write_str(p, "some data that needs a buffer to live in"), 0, "leak: file");
        }
        CHECK_RC(vfs_symlink("d1", "/tmp/leak/l"), 0, "leak: link");
        int fd = vfs_open("/tmp/leak/big", O_RDWR | O_CREAT);
        CHECK_RC(vfs_ftruncate(fd, 100000), 0, "leak: big");
        vfs_close(fd);
        for (int i = 0; i < 50; i++) {
            char p[64] = "/tmp/leak/d1/f";
            p[14] = (char)('0' + i / 10);
            p[15] = (char)('0' + i % 10);
            p[16] = 0;
            CHECK_RC(vfs_unlink(p), 0, "leak: unlink");
        }
        CHECK_RC(vfs_unlink("/tmp/leak/big"), 0, "leak: unlink big");
        CHECK_RC(vfs_unlink("/tmp/leak/l"), 0, "leak: unlink link");
        CHECK_RC(vfs_rmdir("/tmp/leak/d1/d2"), 0, "leak: rmdir");
        CHECK_RC(vfs_rmdir("/tmp/leak/d1"), 0, "leak: rmdir");
        CHECK_RC(vfs_rmdir("/tmp/leak"), 0, "leak: rmdir top");
    }
    CHECK(heap_used() == before, "ramfs operations do not leak memory");
}

/* ---------- tar ---------- */

static void tar_header(uint8_t *h, const char *name, char type, size_t size, const char *link)
{
    memset(h, 0, 512);
    strlcpy((char *)h, name, 100);
    strlcpy((char *)h + 100, "0000644", 8);
    strlcpy((char *)h + 108, "0000000", 8);
    strlcpy((char *)h + 116, "0000000", 8);
    for (int i = 0; i < 11; i++) /* 11 octal digits */
        h[124 + i] = (uint8_t)('0' + ((size >> (3 * (10 - i))) & 7));
    strlcpy((char *)h + 136, "00000000000", 12);
    h[156] = (uint8_t)type;
    if (link)
        strlcpy((char *)h + 157, link, 100);
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memset(h + 148, ' ', 8);
    uint32_t sum = 0;
    for (int i = 0; i < 512; i++)
        sum += h[i];
    for (int i = 0; i < 6; i++)
        h[148 + i] = (uint8_t)('0' + ((sum >> (3 * (5 - i))) & 7));
    h[154] = 0;
    h[155] = ' ';
}

static size_t tar_add(uint8_t *arc, size_t pos, const char *name, char type, const void *data,
                      size_t size, const char *link)
{
    tar_header(arc + pos, name, type, size, link);
    pos += 512;
    if (size) {
        memcpy(arc + pos, data, size);
        pos += (size + 511) & ~(size_t)511;
    }
    return pos;
}

static void test_tar(void)
{
    uint8_t *arc = kzalloc(16384);
    CHECK(arc != NULL, "archive buffer");
    uint8_t big[700];
    for (size_t i = 0; i < sizeof(big); i++)
        big[i] = pattern_byte(i);

    char longpath[200];
    strlcpy(longpath, "tdir/", sizeof(longpath));
    for (int i = 0; i < 12; i++)
        strlcpy(longpath + strlen(longpath), "longdirectory/", sizeof(longpath) - strlen(longpath));
    strlcpy(longpath + strlen(longpath), "leaf.txt", sizeof(longpath) - strlen(longpath));

    size_t pos = 0;
    pos = tar_add(arc, pos, "./", '5', NULL, 0, NULL);
    pos = tar_add(arc, pos, "tdir/", '5', NULL, 0, NULL);
    pos = tar_add(arc, pos, "tdir/a.txt", '0', "alpha\n", 6, NULL);
    pos = tar_add(arc, pos, "tdir/big.bin", '0', big, sizeof(big), NULL);
    pos = tar_add(arc, pos, "tdir/ln", '2', NULL, 0, "a.txt");
    pos =
        tar_add(arc, pos, "tdir/no/parent/yet.txt", '0', "orphan\n", 7, NULL); /* parents implied */
    pos = tar_add(arc, pos, "tdir/empty", '0', NULL, 0, NULL);
    pos = tar_add(arc, pos, "tdir/skipped", '3', NULL, 0, NULL); /* character device: ignored */
    pos = tar_add(arc, pos, "././@LongLink", 'L', longpath, strlen(longpath) + 1, NULL);
    pos = tar_add(arc, pos, "tdir/truncated-name", '0', "long\n", 5, NULL);
    size_t end = pos + 1024; /* two zero blocks */

    int n = tar_unpack(arc, end, "/tmp/tar");
    CHECK_RC(n, 7, "entries extracted (devices and the archive root are not counted)");
    CHECK(file_is("/tmp/tar/tdir/a.txt", "alpha\n"), "tar: file");
    CHECK(file_matches_pattern("/tmp/tar/tdir/big.bin", sizeof(big), 100), "tar: two-block file");
    CHECK(file_is("/tmp/tar/tdir/ln", "alpha\n"), "tar: symlink");
    CHECK(file_is("/tmp/tar/tdir/no/parent/yet.txt", "orphan\n"), "tar: implied parents");
    CHECK(file_is("/tmp/tar/tdir/empty", ""), "tar: empty file");
    struct stat st;
    CHECK_RC(vfs_stat("/tmp/tar/tdir/skipped", &st), -ENOENT, "tar: device skipped");
    char full[VFS_PATH_MAX];
    join(full, "/tmp/tar/", longpath);
    CHECK(file_is(full, "long\n"), "tar: GNU long name");

    /* Unpacking again overwrites cleanly. */
    CHECK_RC(tar_unpack(arc, end, "/tmp/tar"), 7, "tar: repeat unpack");

    /* Corrupt archives must be rejected, not half-trusted. */
    arc[3] ^= 0x55; /* first header's name changes, so its checksum no longer matches */
    CHECK_RC(tar_unpack(arc, end, "/tmp/tar2"), -EINVAL, "tar: bad checksum");
    arc[3] ^= 0x55;
    size_t cut = 512 * 5 + 100; /* inside the data of "tdir/big.bin" (header is block 4) */
    CHECK_RC(tar_unpack(arc, cut, "/tmp/tar3"), -EINVAL, "tar: truncated file data");
    CHECK_RC(tar_unpack(arc, 100, "/tmp/tar4"), 0, "tar: shorter than a header is just empty");
    CHECK_RC(tar_unpack(arc + end, 1024, "/tmp/tar5"), 0, "tar: only the end marker");

    /* Clean up (children first). */
    CHECK_RC(vfs_unlink(full), 0, "tar cleanup long");
    kfree(arc);
}

/* ---------- initrd ---------- */

static void test_initrd(void)
{
    CHECK(file_is("/etc/hostname", "maleos\n"), "initrd: hostname");
    char buf[128];
    int64_t n = vfs_read_file("/etc/motd", buf, sizeof(buf) - 1);
    CHECK(n > 8 && memcmp(buf, "Welcome", 7) == 0, "initrd: motd");
    CHECK(file_is("/share/hello.txt", "Hello from the Maleos initrd!\n"), "initrd: hello");
    CHECK(file_is("/share/hello.link", "Hello from the Maleos initrd!\n"), "initrd: relative link");
    char other[128];
    int64_t m = vfs_read_file("/share/motd.link", other, sizeof(other) - 1);
    CHECK(m == n && memcmp(other, buf, (size_t)n) == 0, "initrd: absolute link reaches motd");
    CHECK(file_matches_pattern("/share/pattern.bin", 3000, 512), "initrd: 3000-byte pattern file");

    char deep[VFS_PATH_MAX] = "/deep";
    for (int i = 0; i < 12; i++) {
        char part[24] = "/level00_dir";
        part[6] = (char)('0' + i / 10);
        part[7] = (char)('0' + i % 10);
        strlcpy(deep + strlen(deep), part, sizeof(deep) - strlen(deep));
    }
    strlcpy(deep + strlen(deep), "/bottom.txt", sizeof(deep) - strlen(deep));
    CHECK(strlen(deep) > 100, "initrd: the deep path really is over 100 bytes");
    CHECK(file_is(deep, "found the bottom\n"), "initrd: GNU long name in the real archive");

    bool found;
    CHECK(count_dir("/", "etc", &found) >= 8 && found, "initrd: root listing has etc");
    CHECK(count_dir("/", "share", &found) >= 8 && found, "initrd: root listing has share");
    struct stat st;
    CHECK_RC(vfs_lstat("/share/hello.link", &st), 0, "initrd: lstat link");
    CHECK(S_ISLNK(st.mode), "initrd: link kept as a link");
}

/* ---------- ext2 ---------- */

static void test_ext2_disk(const char *mnt, const char *label, bool is_ide)
{
    char p[VFS_PATH_MAX], want[96];
    struct stat st;

    join(p, mnt, "/hello.txt");
    strlcpy(want, "Hello from ext2 on ", sizeof(want));
    strlcpy(want + strlen(want), label, sizeof(want) - strlen(want));
    strlcpy(want + strlen(want), "!\n", sizeof(want) - strlen(want));
    CHECK(file_is(p, want), "ext2: small file");
    CHECK_RC(vfs_stat(p, &st), 0, "ext2: stat");
    CHECK(S_ISREG(st.mode) && st.size == strlen(want), "ext2: file size and type");

    join(p, mnt, "/empty");
    CHECK_RC(vfs_stat(p, &st), 0, "ext2: stat empty");
    CHECK(st.size == 0, "ext2: empty file");
    CHECK(file_is(p, ""), "ext2: reading an empty file");

    join(p, mnt, "/dir/nested/deep.txt");
    CHECK(file_is(p, "deep file\n"), "ext2: nested path");
    join(p, mnt, "/dir/nested/../nested/./deep.txt");
    CHECK(file_is(p, "deep file\n"), "ext2: dot and dotdot");

    join(p, mnt, "/medium.bin");
    CHECK(file_matches_pattern(p, 70000, 1000), "ext2: single-indirect file");
    join(p, mnt, "/big.bin");
    CHECK(file_matches_pattern(p, 600000, 4000), "ext2: large file");
    CHECK(file_matches_pattern(p, 600000, 333), "ext2: large file, odd chunks");

    /* Random access across indirect-block boundaries. */
    int fd = vfs_open(p, O_RDONLY);
    static const uint64_t offs[] = {
        0,      1,    11 * 1024 - 3, 12 * 1024 - 1, 12 * 1024, 268 * 1024 - 2, 268 * 1024 + 5,
        599990, 4095, 4096};
    uint8_t buf[16];
    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        CHECK_RC(vfs_lseek(fd, (int64_t)offs[i], SEEK_SET), (long long)offs[i], "ext2: seek");
        int64_t n = vfs_read(fd, buf, sizeof(buf));
        CHECK(n > 0, "ext2: read at offset");
        bool ok = true;
        for (int64_t j = 0; j < n; j++)
            ok &= buf[j] == pattern_byte(offs[i] + (uint64_t)j);
        CHECK(ok, "ext2: data at offset");
    }
    CHECK_RC(vfs_lseek(fd, 600000, SEEK_SET), 600000, "ext2: seek to EOF");
    CHECK_RC(vfs_read(fd, buf, 16), 0, "ext2: read at EOF");
    CHECK_RC(vfs_lseek(fd, 700000, SEEK_SET), 700000, "ext2: seek past EOF");
    CHECK_RC(vfs_read(fd, buf, 16), 0, "ext2: read past EOF");
    vfs_close(fd);

    /* Sparse file: 1 MiB logical size, almost nothing allocated. */
    join(p, mnt, "/sparse.bin");
    CHECK_RC(vfs_stat(p, &st), 0, "ext2: stat sparse");
    CHECK(st.size == 1048576, "ext2: sparse size");
    fd = vfs_open(p, O_RDONLY);
    uint8_t sp[8];
    CHECK_RC(vfs_read(fd, sp, 4), 4, "ext2: sparse head");
    CHECK(memcmp(sp, "HEAD", 4) == 0, "ext2: sparse head data");
    CHECK_RC(vfs_lseek(fd, 4, SEEK_SET), 4, "seek");
    uint8_t *hole = kmalloc(65536);
    CHECK_RC(vfs_read(fd, hole, 65536), 65536, "ext2: read through a hole");
    bool zeros = true;
    for (int i = 0; i < 65536; i++)
        zeros &= hole[i] == 0;
    CHECK(zeros, "ext2: holes read as zeros");
    kfree(hole);
    CHECK_RC(vfs_lseek(fd, 500000, SEEK_SET), 500000, "seek");
    CHECK_RC(vfs_read(fd, sp, 6), 6, "ext2: sparse middle");
    CHECK(memcmp(sp, "MIDDLE", 6) == 0, "ext2: sparse middle data");
    CHECK_RC(vfs_lseek(fd, -4, SEEK_END), 1048572, "seek to tail");
    CHECK_RC(vfs_read(fd, sp, 8), 4, "ext2: sparse tail");
    CHECK(memcmp(sp, "TAIL", 4) == 0, "ext2: sparse tail data");
    vfs_close(fd);

    /* Directories: a long name, and a directory spanning several blocks. */
    char lname[256] = "/name_";
    size_t l = strlen(lname);
    memset(lname + l, 'x', 190);
    strlcpy(lname + l + 190, ".txt", sizeof(lname) - l - 190);
    join(p, mnt, lname);
    CHECK(file_is(p, "long name\n"), "ext2: long file name");

    join(p, mnt, "/many");
    bool found;
    CHECK_RC(count_dir(p, "f199", &found), 202, "ext2: 200 entries plus . and ..");
    CHECK(found, "ext2: last entry of a big directory");
    for (int i = 0; i < 200; i += 37) {
        char one[VFS_PATH_MAX], num[8] = "/fNNN";
        num[2] = (char)('0' + i / 100);
        num[3] = (char)('0' + (i / 10) % 10);
        num[4] = (char)('0' + i % 10);
        join(one, p, num);
        char content[16] = "file ";
        char digits[8];
        int dl = 0;
        for (int v = i; v || dl == 0; v /= 10)
            digits[dl++] = (char)('0' + v % 10);
        for (int k = 0; k < dl; k++)
            content[5 + k] = digits[dl - 1 - k];
        content[5 + dl] = '\n';
        content[6 + dl] = 0;
        CHECK(file_is(one, content), "ext2: lookup in a big directory");
    }

    bool only_here;
    CHECK(count_dir(mnt, is_ide ? "only_ide.txt" : "only_sata.txt", &only_here) > 8 && only_here,
          "ext2: root listing has this disk's own file");
    CHECK(count_dir(mnt, is_ide ? "only_sata.txt" : "only_ide.txt", &only_here) > 8 && !only_here,
          "ext2: and not the other disk's");

    /* Symbolic links, fast and slow, and across the mount. */
    join(p, mnt, "/link");
    CHECK(file_is(p, want), "ext2: fast symlink");
    CHECK_RC(vfs_lstat(p, &st), 0, "lstat");
    CHECK(S_ISLNK(st.mode) && st.size == 9, "ext2: fast symlink type/size");
    join(p, mnt, "/longlink");
    CHECK(file_is(p, want), "ext2: slow symlink (target stored in a block)");
    CHECK_RC(vfs_lstat(p, &st), 0, "lstat");
    CHECK(S_ISLNK(st.mode) && st.size == 99, "ext2: slow symlink size");
    join(p, mnt, "/dirlink/deep.txt");
    CHECK(file_is(p, "deep file\n"), "ext2: link to a directory");
    join(p, mnt, "/dangling");
    CHECK_RC(vfs_stat(p, &st), -ENOENT, "ext2: dangling link");
    CHECK_RC(vfs_lstat(p, &st), 0, "ext2: lstat dangling");
    char lb[128];
    join(p, mnt, "/link");
    CHECK_RC(vfs_readlink(p, lb, sizeof(lb)), 9, "ext2: readlink");
    CHECK(strcmp(lb, "hello.txt") == 0, "ext2: readlink content");

    /* Mount boundaries. */
    join(p, mnt, "/..");
    struct stat rootst, mntst, up;
    CHECK_RC(vfs_stat("/", &rootst), 0, "stat /");
    CHECK_RC(vfs_stat(p, &up), 0, "stat mnt/..");
    CHECK(up.ino == rootst.ino && up.dev == rootst.dev, "ext2: dotdot leaves the mount");
    CHECK_RC(vfs_stat(mnt, &mntst), 0, "stat mnt");
    CHECK(mntst.dev != rootst.dev, "ext2: the mount point shows the mounted filesystem");
    CHECK(S_ISDIR(mntst.mode), "ext2: mount root is a directory");
    join(p, mnt, "/dir/../../tmp");
    CHECK_RC(vfs_stat(p, &st), 0, "ext2: dotdot twice crosses back into ramfs");

    /* Read-only: every kind of change is refused. */
    join(p, mnt, "/hello.txt");
    CHECK_RC(vfs_open(p, O_WRONLY), -EROFS, "ext2: open for writing");
    CHECK_RC(vfs_open(p, O_RDWR), -EROFS, "ext2: open read-write");
    CHECK_RC(vfs_open(p, O_RDONLY | O_TRUNC), -EROFS, "ext2: O_TRUNC");
    join(p, mnt, "/newfile");
    CHECK_RC(vfs_open(p, O_WRONLY | O_CREAT), -EROFS, "ext2: create");
    CHECK_RC(vfs_mkdir(p), -EROFS, "ext2: mkdir");
    CHECK_RC(vfs_symlink("x", p), -EROFS, "ext2: symlink");
    join(p, mnt, "/hello.txt");
    CHECK_RC(vfs_unlink(p), -EROFS, "ext2: unlink");
    join(p, mnt, "/dir");
    CHECK_RC(vfs_rmdir(p), -EROFS, "ext2: rmdir");
    CHECK_RC(vfs_open_file_count(), 0, "ext2: no descriptors leaked by failed opens");

    /* A file, not a mount point, cannot be mounted over; a mount point cannot be removed. */
    CHECK_RC(vfs_rmdir(mnt), -EBUSY, "mount point cannot be removed");
}

static void test_mounts(void)
{
    struct blockdev *hda = blk_find("hda");
    struct blockdev *sda = blk_find("sda");
    if (!hda || !sda) {
        printk("fs selftest: test disks missing, skipping ext2 tests\n");
        return;
    }

    test_ext2_disk("/mnt", "hda", true);
    test_ext2_disk("/mnt2", "sda", false);

    /* Mount error cases. */
    CHECK_RC(vfs_mount("hda", "/mnt", "ext2"), -EBUSY, "mount over a mount point");
    CHECK_RC(vfs_mount("hda", "/tmp", "ext2"), -EBUSY, "same device mounted twice");
    CHECK_RC(vfs_mount("nvme0", "/tmp", "ext2"), -ENODEV, "no such device");
    CHECK_RC(vfs_mount(NULL, "/tmp", "ext2"), -ENODEV, "device required");
    CHECK_RC(vfs_mount("hda", "/tmp", "fat32"), -ENODEV, "unknown filesystem");
    CHECK_RC(vfs_mount(NULL, "/etc/motd", "ramfs"), -ENOTDIR, "mount on a file");
    CHECK_RC(vfs_mount(NULL, "/does/not/exist", "ramfs"), -ENOENT, "mount on a missing dir");
    CHECK_RC(vfs_umount("/tmp"), -EINVAL, "umount of a plain directory");
    CHECK_RC(vfs_umount("/"), -EBUSY, "umount of the root");
    CHECK_RC(vfs_umount("/nowhere"), -ENOENT, "umount of a missing path");

    CHECK_RC(vfs_open_file_count(), 0, "no stray descriptors");

    /* Busy unmount: open file, then working inside the mount. */
    int fd = vfs_open("/mnt/hello.txt", O_RDONLY);
    CHECK_RC(vfs_umount("/mnt"), -EBUSY, "umount with an open file");
    CHECK_RC(vfs_close(fd), 0, "close");

    /* Nested ramfs mount inside an ext2 mount is impossible (read-only), but a ramfs can
     * be mounted on a ramfs directory and then blocks unmounting what is below it. */
    CHECK_RC(vfs_mkdir_p("/tmp/mnt3"), 0, "setup nested");
    CHECK_RC(vfs_mount(NULL, "/tmp/mnt3", "ramfs"), 0, "mount a second ramfs");
    CHECK_RC(write_str("/tmp/mnt3/inner", "in"), 0, "write inside the new ramfs");
    CHECK(file_is("/tmp/mnt3/inner", "in"), "read inside the new ramfs");
    struct stat a, b;
    CHECK_RC(vfs_stat("/tmp/mnt3", &a), 0, "stat");
    CHECK_RC(vfs_stat("/tmp", &b), 0, "stat");
    CHECK(a.dev != b.dev, "second ramfs is a different filesystem");
    CHECK_RC(vfs_rmdir("/tmp/mnt3"), -EBUSY, "rmdir of a mount point");
    CHECK_RC(vfs_umount("/tmp/mnt3"), 0, "umount ramfs");
    CHECK_RC(vfs_stat("/tmp/mnt3/inner", &a), -ENOENT, "mounted files vanish with the mount");
    CHECK_RC(vfs_rmdir("/tmp/mnt3"), 0, "rmdir after umount");

    /* Unmount and remount must give identical content and leak nothing. */
    CHECK_RC(vfs_umount("/mnt"), 0, "umount hda");
    uint64_t base = heap_used();
    CHECK_RC(vfs_stat("/mnt/hello.txt", &a), -ENOENT, "files gone after umount");
    CHECK_RC(vfs_umount("/mnt"), -EINVAL, "umount twice");
    CHECK_RC(vfs_mount("hda", "/mnt", "ext2"), 0, "remount hda");
    test_ext2_disk("/mnt", "hda", true);
    CHECK_RC(vfs_umount("/mnt"), 0, "umount again");
    CHECK(heap_used() == base, "mount + use + umount returns all memory");

    /* Cross mount: hda on /mnt2 and sda on /mnt, swapped, to prove nothing is path-specific. */
    CHECK_RC(vfs_umount("/mnt2"), 0, "umount sda");
    CHECK_RC(vfs_mount("sda", "/mnt", "ext2"), 0, "mount sda on /mnt");
    CHECK_RC(vfs_mount("hda", "/mnt2", "ext2"), 0, "mount hda on /mnt2");
    test_ext2_disk("/mnt", "sda", false);
    test_ext2_disk("/mnt2", "hda", true);
    CHECK_RC(vfs_umount("/mnt"), 0, "umount");
    CHECK_RC(vfs_umount("/mnt2"), 0, "umount");
    CHECK_RC(vfs_mount("hda", "/mnt", "ext2"), 0, "restore hda");
    CHECK_RC(vfs_mount("sda", "/mnt2", "ext2"), 0, "restore sda");

    /* ramfs takes no device, so a device name is ignored and does not count as "in use". */
    CHECK_RC(vfs_mount("hda", "/tmp", "ramfs"), 0, "ramfs ignores its device");
    CHECK_RC(vfs_umount("/tmp"), 0, "umount that ramfs again");
}

int fs_selftest(void)
{
    checks = 0;
    CHECK_RC(vfs_open_file_count(), 0, "start with no open files");

    test_ramfs_files();
    test_ramfs_large();
    test_paths();
    test_symlinks();
    test_readdir();
    test_tar();
    test_initrd();
    test_fd_limits_and_leaks();
    test_mounts();

    CHECK_RC(vfs_open_file_count(), 0, "no descriptors leaked by the whole suite");
    return checks;
}
