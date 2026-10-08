#include "ulib.h"

static char kind(uint32_t mode)
{
    return S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : '-';
}

static int list(const char *path, int longfmt)
{
    struct stat st;
    long rc = lstat(path, &st);
    if (rc < 0) {
        dprintf(2, "ls: %s: %s\n", path, strerror((int)-rc));
        return 1;
    }
    if (!S_ISDIR(st.mode)) {
        if (longfmt)
            printf("%c %8lu %s\n", kind(st.mode), (unsigned long)st.size, path);
        else
            printf("%s\n", path);
        return 0;
    }

    long fd = open(path, O_RDONLY);
    if (fd < 0) {
        dprintf(2, "ls: %s: %s\n", path, strerror((int)-fd));
        return 1;
    }
    struct dirent de;
    while (readdir((int)fd, &de) == 1) {
        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
            continue;
        if (longfmt) {
            char full[VFS_PATH_MAX];
            size_t n = strlcpy(full, path, sizeof(full));
            if (n && full[n - 1] != '/')
                n = (size_t)strlcpy(full + n, "/", sizeof(full) - n) + n;
            strlcpy(full + n, de.name, sizeof(full) - n);
            struct stat es;
            if (lstat(full, &es) < 0)
                es.size = 0, es.mode = 0;
            printf("%c %8lu %s\n", kind(es.mode), (unsigned long)es.size, de.name);
        } else {
            printf("%s%s\n", de.name, de.type == VT_DIR ? "/" : "");
        }
    }
    close((int)fd);
    return 0;
}

int main(int argc, char **argv)
{
    int longfmt = 0, first = 1;
    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        longfmt = 1;
        first = 2;
    }
    if (first >= argc)
        return list(".", longfmt);
    int status = 0;
    for (int i = first; i < argc; i++) {
        if (argc - first > 1)
            printf("%s:\n", argv[i]);
        status |= list(argv[i], longfmt);
    }
    return status;
}
