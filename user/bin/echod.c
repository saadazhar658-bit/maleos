#include "ulib.h"

/* An echo server (RFC 862). Usage: echod [tcp|udp] [port]   (default tcp, port 7) */
int main(int argc, char **argv)
{
    bool udp = argc > 1 && !strcmp(argv[1], "udp");
    uint16_t port = argc > 2 ? (uint16_t)atol(argv[2]) : 7;
    long s = socket(udp ? SOCK_DGRAM : SOCK_STREAM);
    long rc = s < 0 ? s : bind((int)s, 0, port);
    if (rc >= 0 && !udp)
        rc = listen((int)s, 8);
    if (rc < 0) {
        dprintf(2, "echod: port %u: %s\n", port, strerror((int)-rc));
        return 1;
    }
    char buf[2048];
    if (udp) {
        for (;;) {
            struct abi_sockaddr from;
            long n = recvfrom((int)s, buf, sizeof(buf), &from);
            if (n >= 0)
                sendto((int)s, buf, (size_t)n, from.ip, from.port);
        }
    }
    for (;;) {
        long c = accept((int)s, NULL);
        if (c < 0)
            continue;
        sockctl((int)c, SOCKCTL_RCVTIMEO, 10000);
        for (;;) {
            long n = read((int)c, buf, sizeof(buf));
            if (n <= 0)
                break;
            long done = 0;
            while (done < n) {
                long w = write((int)c, buf + done, (size_t)(n - done));
                if (w <= 0)
                    break;
                done += w;
            }
            if (done < n)
                break;
        }
        close((int)c);
    }
}
