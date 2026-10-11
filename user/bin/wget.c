#include "ulib.h"

/* wget [-O file] http://a.b.c.d[:port]/path   (IPv4 literals only; there is no DNS client) */
static int fail(const char *what, long err)
{
    dprintf(2, "wget: %s: %s\n", what, strerror((int)-err));
    return 1;
}

int main(int argc, char **argv)
{
    const char *out = NULL, *url = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O") && i + 1 < argc)
            out = argv[++i];
        else
            url = argv[i];
    }
    if (!url || strncmp(url, "http://", 7)) {
        dprintf(2, "usage: wget [-O file] http://a.b.c.d[:port]/path\n");
        return 2;
    }
    const char *p = url + 7;
    char host[32];
    size_t hl = 0;
    while (p[hl] && p[hl] != ':' && p[hl] != '/' && hl < sizeof(host) - 1)
        hl++;
    memcpy(host, p, hl);
    host[hl] = 0;
    p += hl;
    uint16_t port = 80;
    if (*p == ':') {
        port = (uint16_t)atol(p + 1);
        while (*p && *p != '/')
            p++;
    }
    const char *path = *p ? p : "/";
    uint32_t ip;
    if (parse_ip(host, &ip) < 0) {
        dprintf(2, "wget: %s: only IPv4 addresses are supported\n", host);
        return 2;
    }

    long s = socket(SOCK_STREAM);
    if (s < 0)
        return fail("socket", s);
    long rc = connect((int)s, ip, port);
    if (rc < 0)
        return fail("connect", rc);
    sockctl((int)s, SOCKCTL_RCVTIMEO, 15000);

    char req[320];
    strlcpy(req, "GET ", sizeof(req));
    strlcpy(req + strlen(req), path, sizeof(req) - strlen(req));
    strlcpy(req + strlen(req), " HTTP/1.0\r\nHost: ", sizeof(req) - strlen(req));
    strlcpy(req + strlen(req), host, sizeof(req) - strlen(req));
    strlcpy(req + strlen(req), "\r\nConnection: close\r\n\r\n", sizeof(req) - strlen(req));
    rc = write((int)s, req, strlen(req));
    if (rc < 0)
        return fail("send", rc);

    int ofd = 1;
    if (out) {
        long f = open(out, O_WRONLY | O_CREAT | O_TRUNC);
        if (f < 0)
            return fail(out, f);
        ofd = (int)f;
    }

    static char hdr[2048];
    size_t hlen = 0;
    bool in_body = false;
    unsigned long body = 0;
    int status = 0;
    char buf[1024];
    for (;;) {
        long r = read((int)s, buf, sizeof(buf));
        if (r < 0)
            return fail("receive", r);
        if (r == 0)
            break;
        const char *data = buf;
        size_t len = (size_t)r;
        if (!in_body) {
            size_t take = len < sizeof(hdr) - 1 - hlen ? len : sizeof(hdr) - 1 - hlen;
            memcpy(hdr + hlen, data, take);
            hlen += take;
            hdr[hlen] = 0;
            char *end = NULL;
            for (size_t i = 0; i + 3 < hlen; i++)
                if (hdr[i] == '\r' && hdr[i + 1] == '\n' && hdr[i + 2] == '\r' &&
                    hdr[i + 3] == '\n') {
                    end = hdr + i + 4;
                    break;
                }
            if (!end) {
                if (hlen >= sizeof(hdr) - 1) {
                    dprintf(2, "wget: response header too long\n");
                    return 1;
                }
                continue;
            }
            if (!strncmp(hdr, "HTTP/", 5)) {
                const char *sp = strchr(hdr, ' ');
                status = sp ? (int)atol(sp + 1) : 0;
            }
            in_body = true;
            size_t consumed_from_this = (size_t)(end - hdr) - (hlen - take);
            data += consumed_from_this;
            len -= consumed_from_this;
        }
        if (len) {
            write(ofd, data, len);
            body += len;
        }
    }
    close((int)s);
    if (out)
        close(ofd);
    dprintf(2, "HTTP %d, %lu bytes\n", status, body);
    return status >= 200 && status < 300 ? 0 : 1;
}
