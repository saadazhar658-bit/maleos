#ifndef MALEOS_ABI_ABI_H
#define MALEOS_ABI_ABI_H

/*
 * The kernel/user ABI: everything a user program and the kernel must agree on. This header
 * is included by both sides and must stay freestanding (only <stdint.h>).
 *
 * System call convention (x86_64 `syscall` instruction):
 *   rax = number, rdi, rsi, rdx, r10, r8, r9 = arguments, result in rax.
 *   A result in [-4095, -1] is -errno. rcx and r11 are clobbered by the instruction;
 *   every other register is preserved.
 */

#include <stdint.h>

#define VFS_NAME_MAX 255
#define VFS_PATH_MAX 1024

/* ---- file types and flags (same numeric values as POSIX/Linux) ---- */

#define S_IFMT 0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IFLNK 0120000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_ACCMODE 3
#define O_CREAT 0000100
#define O_EXCL 0000200
#define O_TRUNC 0001000
#define O_APPEND 0002000
#define O_DIRECTORY 0200000
#define O_NOFOLLOW 0400000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

enum vtype { VT_REG = 1, VT_DIR, VT_LNK, VT_OTHER };

struct dirent {
    uint64_t ino;
    uint8_t type; /* enum vtype */
    char name[VFS_NAME_MAX + 1];
};

struct stat {
    uint64_t dev; /* id of the mounted filesystem */
    uint64_t ino;
    uint32_t mode; /* S_IF* | permission bits */
    uint32_t nlink;
    uint64_t size;
    uint32_t blksize;
};

/* ---- system calls ---- */

#define SYS_EXIT 0       /* (code)                          does not return */
#define SYS_WRITE 1      /* (fd, buf, len) -> bytes */
#define SYS_READ 2       /* (fd, buf, len) -> bytes, 0 at EOF */
#define SYS_OPEN 3       /* (path, flags) -> fd */
#define SYS_CLOSE 4      /* (fd) */
#define SYS_LSEEK 5      /* (fd, offset, whence) -> new offset */
#define SYS_STAT 6       /* (path, struct stat *) */
#define SYS_LSTAT 7      /* (path, struct stat *) */
#define SYS_FSTAT 8      /* (fd, struct stat *) */
#define SYS_READDIR 9    /* (fd, struct dirent *) -> 1 entry, 0 end */
#define SYS_MKDIR 10     /* (path) */
#define SYS_RMDIR 11     /* (path) */
#define SYS_UNLINK 12    /* (path) */
#define SYS_SYMLINK 13   /* (target, linkpath) */
#define SYS_READLINK 14  /* (path, buf, size) -> length */
#define SYS_CHDIR 15     /* (path) */
#define SYS_GETCWD 16    /* (buf, size) -> length including the NUL */
#define SYS_GETPID 17    /* () -> pid */
#define SYS_YIELD 18     /* () */
#define SYS_SLEEP_MS 19  /* (ms) */
#define SYS_UPTIME_MS 20 /* () -> milliseconds since boot */
#define SYS_SPAWN 21     /* (path, argv, struct abi_spawn_attr *or NULL) -> pid */
#define SYS_WAIT 22      /* (pid, int *status) -> pid; only a child, blocks until it ends */
#define SYS_SBRK 23      /* (increment) -> previous break */
#define SYS_PROCINFO 24  /* (struct abi_procinfo *, max) -> count */
#define SYS_MEMINFO 25   /* (struct abi_meminfo *) */
#define SYS_POWEROFF 26  /* () does not return */
#define SYS_FTRUNCATE 27 /* (fd, size) */
#define SYS_GETRANDOM 28 /* (buf, len) -> len; len <= ABI_RANDOM_MAX */
#define SYS_SOCKET 29    /* (type) -> fd; SOCK_STREAM or SOCK_DGRAM */
#define SYS_BIND 30      /* (fd, const struct abi_sockaddr *) */
#define SYS_LISTEN 31    /* (fd, backlog) */
#define SYS_ACCEPT 32    /* (fd, struct abi_sockaddr * or NULL) -> new fd */
#define SYS_CONNECT 33   /* (fd, const struct abi_sockaddr *) */
#define SYS_SENDTO 34    /* (fd, const struct abi_dgram *) -> bytes sent */
#define SYS_RECVFROM 35  /* (fd, struct abi_dgram *) -> bytes received, 0 = end of stream */
#define SYS_SOCKCTL 36   /* (fd, op, value) see SOCKCTL_* */
#define SYS_NETINFO 37   /* (struct abi_netinfo *) */
#define SYS_PING 38      /* (ip, timeout_ms, seq) -> round-trip time in ms */
#define SYS_CPUINFO 39   /* (struct abi_cpuinfo *) */
#define SYS_COUNT 40
#define ABI_RANDOM_MAX 256

#define SOCK_STREAM 1
#define SOCK_DGRAM 2

#define SOCKCTL_RCVTIMEO 1  /* value = milliseconds, 0 = wait forever */
#define SOCKCTL_NONBLOCK 2  /* value = 0 or 1 */
#define SOCKCTL_SHUT_WR 3   /* send FIN after the queued data */
#define SOCKCTL_GETNAME 4   /* returns local port; value ignored */
#define SOCKCTL_REUSEADDR 5 /* value = 0 or 1 */

/* IPv4 address in host byte order (10.0.2.15 == 0x0A00020F). */
struct abi_sockaddr {
    uint32_t ip;
    uint16_t port;
    uint16_t reserved;
};

struct abi_dgram {
    void *buf;
    uint64_t len;
    struct abi_sockaddr addr; /* destination for sendto on UDP, source after recvfrom */
};

struct abi_netinfo {
    uint8_t mac[6];
    uint8_t present; /* 1 if a network card was found */
    uint8_t up;
    uint32_t ip, mask, gw;
    uint64_t rx_frames, tx_frames, rx_bytes, tx_bytes, rx_dropped;
    uint64_t tcp_conns, tcp_retrans, udp_dropped, bad_checksums;
};

#define ABI_CPU_MAX 8
struct abi_cpuinfo {
    uint32_t ncpus;                   /* CPUs running the scheduler */
    uint32_t cpu;                     /* the CPU this call ran on */
    uint64_t ticks[ABI_CPU_MAX];      /* timer ticks seen by each CPU */
    uint64_t idle_ticks[ABI_CPU_MAX]; /* ... while idle */
    uint64_t user_ticks[ABI_CPU_MAX]; /* ... while running user code */
};

#define ABI_MAX_ARGS 16 /* argv entries accepted by SYS_SPAWN (including argv[0]) */
#define ABI_ARG_MAX 256 /* bytes per argument including the NUL */

/* Exit status of a process killed by a CPU exception: 128 + the matching Unix signal. */
#define EXIT_SIGILL 132
#define EXIT_SIGTRAP 133
#define EXIT_SIGFPE 136
#define EXIT_SIGSEGV 139

struct abi_spawn_attr {
    const char *stdout_path; /* NULL: the console. Otherwise opened as the child's fd 1 */
    int32_t stdout_flags;    /* O_TRUNC or O_APPEND (O_WRONLY|O_CREAT are implied) */
    int32_t reserved;
};

enum { PROC_RUNNING = 1, PROC_ZOMBIE = 2 };

struct abi_procinfo {
    uint32_t pid;
    uint32_t ppid;
    uint32_t state;
    uint32_t pages; /* user pages owned */
    uint64_t cpu_ticks;
    char name[24];
};

struct abi_meminfo {
    uint64_t page_size;
    uint64_t total_frames;
    uint64_t free_frames;
    uint32_t processes;
    uint32_t reserved;
};

#endif
