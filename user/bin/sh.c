#include "ulib.h"

/*
 * A small shell. Built-ins: cd, pwd, exit, help, poweroff. Everything else is a program,
 * looked up as given (if it contains a '/') or in /bin. A trailing "> file" or ">> file"
 * redirects the standard output, "< file" reads standard input from a file, and "a | b | c"
 * connects programs with pipes (input redirection applies to the first, output to the last).
 */

#define LINE_MAX 256
#define MAX_TOKENS 16

static int split(char *line, char **tok)
{
    int n = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (n == MAX_TOKENS - 1)
            return -1;
        char quote = 0;
        if (*p == '"' || *p == '\'')
            quote = *p++;
        tok[n++] = p;
        char *w = p;
        while (*p && (quote ? *p != quote : (*p != ' ' && *p != '\t')))
            *w++ = *p++;
        int end = *p == 0;
        if (!end)
            p++;
        *w = 0;
    }
    tok[n] = NULL;
    return n;
}

static void builtin_help(void)
{
    puts("Built-ins:  cd [dir]  pwd  exit [n]  help  poweroff");
    puts("Programs in /bin:  ls cat echo mkdir rmdir rm ln stat ps free uptime uname sleep hello "
         "wc grep head cpus");
    puts("Redirect with < file, > file or >> file, chain programs with a | b.  Ctrl-C cancels a "
         "line, Ctrl-D at an empty line exits.");
}

static void run(char **argv, const char *in, const char *out, int out_flags)
{
    char path[VFS_PATH_MAX];
    if (strchr(argv[0], '/')) {
        strlcpy(path, argv[0], sizeof(path));
    } else {
        strlcpy(path, "/bin/", sizeof(path));
        strlcpy(path + 5, argv[0], sizeof(path) - 5);
    }

    struct abi_spawn_attr attr = {.stdout_path = out, .stdout_flags = out_flags, .stdin_path = in};
    long pid = spawn(path, argv, &attr);
    if (pid < 0) {
        dprintf(2, "sh: %s: %s\n", argv[0], strerror((int)-pid));
        return;
    }
    int status = 0;
    wait((int)pid, &status);
    if (status != 0)
        printf("[exit status %d]\n", status);
}

static void program_path(const char *name, char *path)
{
    if (strchr(name, '/')) {
        strlcpy(path, name, VFS_PATH_MAX);
    } else {
        strlcpy(path, "/bin/", VFS_PATH_MAX);
        strlcpy(path + 5, name, VFS_PATH_MAX - 5);
    }
}

/* Run stages[0] | stages[1] | ... Each stage is a NULL-terminated argv inside `tok`. */
static void run_pipeline(char ***stages, int nstages, const char *in, const char *out,
                         int out_flags)
{
    long pids[MAX_TOKENS];
    int started = 0;
    int prev_read = -1;

    for (int i = 0; i < nstages; i++) {
        int fds[2] = {-1, -1};
        int last = i == nstages - 1;
        if (!last && pipe(fds) < 0) {
            dprintf(2, "sh: cannot create a pipe\n");
            break;
        }

        struct abi_spawn_attr attr = {0};
        if (i == 0)
            attr.stdin_path = in;
        if (prev_read >= 0) {
            attr.flags |= SPAWN_STDIN_FD;
            attr.stdin_fd = prev_read;
        }
        if (!last) {
            attr.flags |= SPAWN_STDOUT_FD;
            attr.stdout_fd = fds[1];
        } else if (out) {
            attr.stdout_path = out;
            attr.stdout_flags = out_flags;
        }

        char path[VFS_PATH_MAX];
        program_path(stages[i][0], path);
        long pid = spawn(path, stages[i], &attr);

        /* The child has its own copies now; keeping ours would stop the pipe from ever ending. */
        if (prev_read >= 0)
            close(prev_read);
        if (!last)
            close(fds[1]);
        prev_read = last ? -1 : fds[0];

        if (pid < 0) {
            dprintf(2, "sh: %s: %s\n", stages[i][0], strerror((int)-pid));
            if (prev_read >= 0)
                close(prev_read);
            break;
        }
        pids[started++] = pid;
    }

    for (int i = 0; i < started; i++) {
        int status = 0;
        wait((int)pids[i], &status);
        if (status != 0)
            printf("[exit status %d]\n", status);
    }
}

int main(void)
{
    printf("Maleos shell. Type 'help' for commands.\n");
    char line[LINE_MAX];

    for (;;) {
        char cwd[VFS_PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) < 0)
            strlcpy(cwd, "?", sizeof(cwd));
        printf("maleos:%s$ ", cwd);

        int len = readline(line, sizeof(line));
        if (len < 0) {
            printf("exit\n");
            return 0;
        }

        char *tok[MAX_TOKENS];
        int n = split(line, tok);
        if (n < 0) {
            puts("sh: too many words");
            continue;
        }
        if (n == 0)
            continue;

        /* Redirections ("< file", "> file", ">> file") may appear anywhere; remove them. */
        const char *in = NULL;
        const char *out = NULL;
        int out_flags = O_TRUNC;
        int bad_redirect = 0;
        for (int i = 0; i < n;) {
            int kind = strcmp(tok[i], "<") == 0    ? 1
                       : strcmp(tok[i], ">") == 0  ? 2
                       : strcmp(tok[i], ">>") == 0 ? 3
                                                   : 0;
            if (!kind) {
                i++;
                continue;
            }
            if (i + 1 >= n || strcmp(tok[i + 1], "|") == 0 || tok[i + 1][0] == '<' ||
                tok[i + 1][0] == '>') {
                bad_redirect = 1;
                break;
            }
            if (kind == 1) {
                in = tok[i + 1];
            } else {
                out = tok[i + 1];
                out_flags = kind == 3 ? O_APPEND : O_TRUNC;
            }
            for (int j = i; j + 2 <= n; j++)
                tok[j] = tok[j + 2]; /* also moves the final NULL */
            n -= 2;
        }
        if (bad_redirect || n == 0) {
            puts("sh: syntax error in redirection");
            continue;
        }

        /* Split at "|" words into pipeline stages. */
        char **stages[MAX_TOKENS];
        int nstages = 1;
        int empty_stage = 0;
        stages[0] = tok;
        for (int i = 0; i < n; i++) {
            if (strcmp(tok[i], "|") == 0) {
                tok[i] = NULL;
                if (i == 0 || tok[i - 1] == NULL || (i + 1 < n && strcmp(tok[i + 1], "|") == 0) ||
                    i + 1 == n)
                    empty_stage = 1;
                stages[nstages++] = &tok[i + 1];
            }
        }
        if (empty_stage) {
            puts("sh: syntax error near |");
            continue;
        }

        if (nstages > 1) {
            run_pipeline(stages, nstages, in, out, out_flags);
            continue;
        }

        if (strcmp(tok[0], "exit") == 0) {
            return n > 1 ? (int)atol(tok[1]) : 0;
        } else if (strcmp(tok[0], "cd") == 0) {
            long rc = chdir(n > 1 ? tok[1] : "/");
            if (rc < 0)
                dprintf(2, "cd: %s: %s\n", n > 1 ? tok[1] : "/", strerror((int)-rc));
        } else if (strcmp(tok[0], "pwd") == 0) {
            puts(cwd);
        } else if (strcmp(tok[0], "help") == 0) {
            builtin_help();
        } else if (strcmp(tok[0], "poweroff") == 0) {
            poweroff();
        } else {
            run(tok, in, out, out_flags);
        }
    }
}
