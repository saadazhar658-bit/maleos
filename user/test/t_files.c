#include "ulib.h"

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        dprintf(2, "t_files: FAILED: %s\n", what);
        failures++;
    }
}

static uint8_t pat(unsigned long i)
{
    return (uint8_t)((i * 31 + (i >> 8)) & 0xFF);
}

int main(void)
{
    static uint8_t buf[10000];

    /* Write 10000 bytes, read them back, seek around. */
    long fd = open("/tmp/t_files.dat", O_RDWR | O_CREAT | O_TRUNC);
    check(fd >= 3, "create");
    for (unsigned i = 0; i < sizeof(buf); i++)
        buf[i] = pat(i);
    check(write((int)fd, buf, sizeof(buf)) == sizeof(buf), "write");
    struct stat st;
    check(fstat((int)fd, &st) == 0 && st.size == sizeof(buf) && S_ISREG(st.mode), "fstat");
    check(lseek((int)fd, 0, SEEK_SET) == 0, "rewind");
    memset(buf, 0, sizeof(buf));
    check(read((int)fd, buf, sizeof(buf)) == sizeof(buf), "read back");
    int ok = 1;
    for (unsigned i = 0; i < sizeof(buf); i++)
        ok &= buf[i] == pat(i);
    check(ok, "data intact");
    check(lseek((int)fd, -10, SEEK_END) == 9990, "seek from end");
    check(read((int)fd, buf, 100) == 10, "short read at the end");
    check(read((int)fd, buf, 100) == 0, "EOF");
    check(ftruncate((int)fd, 100) == 0, "ftruncate");
    check(fstat((int)fd, &st) == 0 && st.size == 100, "size after truncate");
    close((int)fd);

    /* Directories and relative paths. */
    check(mkdir("/tmp/t_files.d") == 0, "mkdir");
    check(chdir("/tmp/../tmp/./t_files.d/") == 0, "chdir with dots");
    char cwd[VFS_PATH_MAX];
    check(getcwd(cwd, sizeof(cwd)) > 0 && strcmp(cwd, "/tmp/t_files.d") == 0,
          "getcwd is normalised");
    fd = open("rel.txt", O_WRONLY | O_CREAT);
    check(fd >= 3, "relative create");
    write((int)fd, "rel", 3);
    close((int)fd);
    check(stat("/tmp/t_files.d/rel.txt", &st) == 0 && st.size == 3, "relative path landed in cwd");
    check(chdir("..") == 0 && getcwd(cwd, sizeof(cwd)) > 0 && strcmp(cwd, "/tmp") == 0, "chdir ..");

    fd = open("t_files.d", O_RDONLY);
    check(fd >= 3, "open directory");
    struct dirent de;
    int names = 0, saw = 0;
    while (readdir((int)fd, &de) == 1) {
        names++;
        saw |= strcmp(de.name, "rel.txt") == 0;
    }
    check(names == 3 && saw, "readdir lists ., .. and the file");
    close((int)fd);
    check(rmdir("t_files.d") == -ENOTEMPTY, "rmdir non-empty");
    check(unlink("t_files.d/rel.txt") == 0, "unlink");
    check(rmdir("t_files.d") == 0, "rmdir");
    check(unlink("/tmp/t_files.dat") == 0, "cleanup");

    /* Symbolic links. */
    check(symlink("/etc/motd", "/tmp/t_files.link") == 0, "symlink");
    char target[64];
    long n = readlink("/tmp/t_files.link", target, sizeof(target));
    check(n == 9 && memcmp(target, "/etc/motd", 9) == 0, "readlink");
    check(lstat("/tmp/t_files.link", &st) == 0 && S_ISLNK(st.mode), "lstat sees the link");
    check(stat("/tmp/t_files.link", &st) == 0 && S_ISREG(st.mode), "stat follows it");
    unlink("/tmp/t_files.link");

    /* The ext2 disk, if it is attached. */
    fd = open("/mnt/big.bin", O_RDONLY);
    if (fd >= 0) {
        unsigned long pos = 0;
        ok = 1;
        for (;;) {
            long got = read((int)fd, buf, sizeof(buf));
            if (got <= 0)
                break;
            for (long i = 0; i < got; i++)
                ok &= buf[i] == pat(pos + (unsigned long)i);
            pos += (unsigned long)got;
        }
        check(ok && pos == 600000, "ext2 file read through system calls");
        close((int)fd);
        check(open("/mnt/hello.txt", O_WRONLY) == -EROFS, "ext2 is read-only");
    }

    return failures ? 1 : 0;
}
