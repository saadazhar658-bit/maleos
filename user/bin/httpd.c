#include "ulib.h"

/*
 * A small HTTP/1.0 server for static files under /www. One connection at a time, GET only,
 * "Connection: close". Usage: httpd [port]   (default 80)
 */
#define ROOT "/www"

static void send_all(int fd, const char *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        long n = write(fd, buf + done, len - done);
        if (n <= 0)
            return;
        done += (size_t)n;
    }
}

static void respond(int c, int code, const char *text, const char *type, unsigned long length)
{
    char head[200];
    size_t n = 0;
    const char *parts[] = {"HTTP/1.0 ",
                           NULL,
                           " ",
                           text,
                           "\r\nContent-Type: ",
                           type,
                           "\r\nConnection: close\r\nContent-Length: "};
    char num[24], code_s[8];
    unsigned long v = length;
    int i = 23;
    num[i] = 0;
    do {
        num[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    code_s[0] = (char)('0' + code / 100);
    code_s[1] = (char)('0' + code / 10 % 10);
    code_s[2] = (char)('0' + code % 10);
    code_s[3] = 0;
    parts[1] = code_s;
    for (unsigned k = 0; k < sizeof(parts) / sizeof(parts[0]); k++) {
        strlcpy(head + n, parts[k], sizeof(head) - n);
        n += strlen(parts[k]);
    }
    strlcpy(head + n, num + i, sizeof(head) - n);
    n += strlen(num + i);
    strlcpy(head + n, "\r\n\r\n", sizeof(head) - n);
    n += 4;
    send_all(c, head, n);
}

static void error_page(int c, int code, const char *text)
{
    respond(c, code, text, "text/plain", strlen(text) + 1);
    send_all(c, text, strlen(text));
    send_all(c, "\n", 1);
}

static const char *content_type(const char *path)
{
    size_t n = strlen(path);
    if (n > 5 && !strcmp(path + n - 5, ".html"))
        return "text/html";
    if (n > 4 && !strcmp(path + n - 4, ".txt"))
        return "text/plain";
    return "application/octet-stream";
}

static void serve(int c)
{
    char req[1024];
    size_t have = 0;
    while (have < sizeof(req) - 1) { /* read until the blank line that ends the header */
        long n = read(c, req + have, sizeof(req) - 1 - have);
        if (n <= 0)
            break;
        have += (size_t)n;
        req[have] = 0;
        bool done = false;
        for (size_t i = 0; i + 3 < have; i++)
            if (req[i] == '\r' && req[i + 1] == '\n' && req[i + 2] == '\r' && req[i + 3] == '\n')
                done = true;
        if (done)
            break;
    }
    req[have] = 0;
    if (strncmp(req, "GET ", 4)) {
        error_page(c, 405, "Method Not Allowed");
        return;
    }
    char *target = req + 4;
    char *end = strchr(target, ' ');
    if (!end || target[0] != '/') {
        error_page(c, 400, "Bad Request");
        return;
    }
    *end = 0;
    for (char *q = target; *q; q++)
        if (q[0] == '.' && q[1] == '.') {
            error_page(c, 403, "Forbidden");
            return;
        }
    char path[96];
    strlcpy(path, ROOT, sizeof(path));
    strlcpy(path + strlen(path), target, sizeof(path) - strlen(path));
    if (path[strlen(path) - 1] == '/')
        strlcpy(path + strlen(path), "index.html", sizeof(path) - strlen(path));

    struct stat st;
    if (stat(path, &st) < 0 || !S_ISREG(st.mode)) {
        error_page(c, 404, "Not Found");
        return;
    }
    long f = open(path, O_RDONLY);
    if (f < 0) {
        error_page(c, 404, "Not Found");
        return;
    }
    respond(c, 200, "OK", content_type(path), st.size);
    char buf[1024];
    for (;;) {
        long n = read((int)f, buf, sizeof(buf));
        if (n <= 0)
            break;
        send_all(c, buf, (size_t)n);
    }
    close((int)f);
}

int main(int argc, char **argv)
{
    uint16_t port = argc > 1 ? (uint16_t)atol(argv[1]) : 80;
    long s = socket(SOCK_STREAM);
    long rc = s < 0 ? s : bind((int)s, 0, port);
    if (rc >= 0)
        rc = listen((int)s, 8);
    if (rc < 0) {
        dprintf(2, "httpd: port %u: %s\n", port, strerror((int)-rc));
        return 1;
    }
    for (;;) {
        long c = accept((int)s, NULL);
        if (c < 0)
            continue;
        sockctl((int)c, SOCKCTL_RCVTIMEO, 3000); /* a stalled client must not block the server */
        serve((int)c);
        close((int)c);
    }
}
