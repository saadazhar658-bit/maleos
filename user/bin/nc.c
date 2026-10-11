#include "ulib.h"

/*
 * nc [-u] host port [text...]     send text (and a newline), print the reply until the peer closes
 * nc [-u] -l port                 accept one connection (or datagram) and print what arrives
 * With -l and text the text is sent back as the reply. Hosts are IPv4 addresses.
 */
static int usage(void)
{
    dprintf(2, "usage: nc [-u] host port [text...]\n       nc [-u] -l port [reply text]\n");
    return 2;
}

static void join(char *dst, size_t size, int argc, char **argv, int from)
{
    dst[0] = 0;
    for (int i = from; i < argc; i++) {
        if (i > from)
            strlcpy(dst + strlen(dst), " ", size - strlen(dst));
        strlcpy(dst + strlen(dst), argv[i], size - strlen(dst));
    }
}

int main(int argc, char **argv)
{
    bool udp = false, listening = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-u"))
            udp = true;
        else if (!strcmp(argv[i], "-l"))
            listening = true;
        else
            return usage();
    }
    char text[256];
    char buf[512];

    if (listening) {
        if (i >= argc)
            return usage();
        uint16_t port = (uint16_t)atol(argv[i]);
        join(text, sizeof(text), argc, argv, i + 1);
        long s = socket(udp ? SOCK_DGRAM : SOCK_STREAM);
        long rc = s < 0 ? s : bind((int)s, 0, port);
        if (rc < 0) {
            dprintf(2, "nc: %s\n", strerror((int)-rc));
            return 1;
        }
        sockctl((int)s, SOCKCTL_RCVTIMEO, 20000);
        struct abi_sockaddr peer;
        if (udp) {
            long n = recvfrom((int)s, buf, sizeof(buf), &peer);
            if (n < 0) {
                dprintf(2, "nc: %s\n", strerror((int)-n));
                return 1;
            }
            write(1, buf, (size_t)n);
            if (text[0])
                sendto((int)s, text, strlen(text), peer.ip, peer.port);
            return 0;
        }
        rc = listen((int)s, 1);
        long c = rc < 0 ? rc : accept((int)s, &peer);
        if (c < 0) {
            dprintf(2, "nc: %s\n", strerror((int)-c));
            return 1;
        }
        sockctl((int)c, SOCKCTL_RCVTIMEO, 20000);
        for (;;) {
            long n = read((int)c, buf, sizeof(buf));
            if (n <= 0)
                break;
            write(1, buf, (size_t)n);
        }
        if (text[0])
            write((int)c, text, strlen(text));
        close((int)c);
        return 0;
    }

    uint32_t ip;
    if (i + 1 >= argc || parse_ip(argv[i], &ip) < 0)
        return usage();
    uint16_t port = (uint16_t)atol(argv[i + 1]);
    join(text, sizeof(text), argc, argv, i + 2);
    size_t len = strlen(text);
    if (len + 1 < sizeof(text))
        text[len++] = '\n';

    long s = socket(udp ? SOCK_DGRAM : SOCK_STREAM);
    long rc = s < 0 ? s : connect((int)s, ip, port);
    if (rc < 0) {
        dprintf(2, "nc: %s\n", strerror((int)-rc));
        return 1;
    }
    sockctl((int)s, SOCKCTL_RCVTIMEO, udp ? 3000 : 10000);
    if (len > 1 || argc > i + 2) {
        rc = write((int)s, text, len);
        if (rc < 0) {
            dprintf(2, "nc: %s\n", strerror((int)-rc));
            return 1;
        }
    }
    if (udp) {
        long n = read((int)s, buf, sizeof(buf));
        if (n > 0)
            write(1, buf, (size_t)n);
        return n > 0 ? 0 : 1;
    }
    sockctl((int)s, SOCKCTL_SHUT_WR, 0);
    for (;;) {
        long n = read((int)s, buf, sizeof(buf));
        if (n < 0) {
            dprintf(2, "nc: %s\n", strerror((int)-n));
            return 1;
        }
        if (n == 0)
            break;
        write(1, buf, (size_t)n);
    }
    close((int)s);
    return 0;
}
