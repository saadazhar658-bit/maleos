#include "ulib.h"

/*
 * A small shell. Built-ins: cd, pwd, exit, help, poweroff. Everything else is a program,
 * looked up as given (if it contains a '/') or in /bin. A trailing "> file" or ">> file"
 * redirects the program's standard output.
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
    puts("Programs in /bin:  ls cat echo mkdir rmdir rm ln stat ps free uptime uname sleep hello");
    puts("Redirect output with > file or >> file.  Ctrl-C cancels a line, Ctrl-D at an empty line "
         "exits.");
}

static void run(char **argv, const char *out, int out_flags)
{
    char path[VFS_PATH_MAX];
    if (strchr(argv[0], '/')) {
        strlcpy(path, argv[0], sizeof(path));
    } else {
        strlcpy(path, "/bin/", sizeof(path));
        strlcpy(path + 5, argv[0], sizeof(path) - 5);
    }

    struct abi_spawn_attr attr = {.stdout_path = out, .stdout_flags = out_flags};
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

        const char *out = NULL;
        int out_flags = O_TRUNC;
        if (n >= 3 && (strcmp(tok[n - 2], ">") == 0 || strcmp(tok[n - 2], ">>") == 0)) {
            out = tok[n - 1];
            out_flags = tok[n - 2][1] ? O_APPEND : O_TRUNC;
            tok[n - 2] = NULL;
            n -= 2;
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
            run(tok, out, out_flags);
        }
    }
}
