#include "ulib.h"

int main(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        long rc = lstat(argv[i], &st);
        if (rc < 0) {
            dprintf(2, "stat: %s: %s\n", argv[i], strerror((int)-rc));
            status = 1;
            continue;
        }
        const char *type = S_ISDIR(st.mode)   ? "directory"
                           : S_ISLNK(st.mode) ? "symbolic link"
                                              : "regular file";
        printf("%s: %s, %lu bytes, inode %lu, device %lu, links %u, mode %o\n", argv[i], type,
               (unsigned long)st.size, (unsigned long)st.ino, (unsigned long)st.dev, st.nlink,
               st.mode & 07777);
        if (S_ISLNK(st.mode)) {
            char target[VFS_PATH_MAX];
            long n = readlink(argv[i], target, sizeof(target) - 1);
            if (n >= 0) {
                target[n] = 0;
                printf("  -> %s\n", target);
            }
        }
    }
    return status;
}
