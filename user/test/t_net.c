#include "ulib.h"

/* Sockets from user space over the loopback interface. Exit status 0 = everything passed. */

static int failures;

static void check(long got, long want, const char *what)
{
    if (got != want) {
        printf("t_net: %s: got %ld, want %ld\n", what, got, want);
        failures++;
    }
}

#define LO 0x7F000001u

static void test_udp(void)
{
    long a = socket(SOCK_DGRAM), b = socket(SOCK_DGRAM);
    check(a >= 3 && b >= 3, 1, "udp sockets");
    check(bind((int)a, 0, 6000), 0, "bind");
    check(bind((int)b, 0, 6000), -EADDRINUSE, "duplicate bind");
    check(bind((int)b, 0, 0), 0, "bind to an ephemeral port");
    long bport = sockctl((int)b, SOCKCTL_GETNAME, 0);
    check(bport >= 49152, 1, "ephemeral port range");

    char buf[64];
    struct abi_sockaddr from;
    check(sendto((int)b, "ping", 4, LO, 6000), 4, "sendto");
    check(recvfrom((int)a, buf, sizeof(buf), &from), 4, "recvfrom");
    check(memcmp(buf, "ping", 4), 0, "payload");
    check(from.ip, LO, "source address");
    check(from.port, bport, "source port");

    sockctl((int)a, SOCKCTL_RCVTIMEO, 50);
    unsigned long t0 = uptime_ms();
    check(recvfrom((int)a, buf, sizeof(buf), &from), -EAGAIN, "receive timeout");
    unsigned long waited = uptime_ms() - t0;
    check(waited >= 40 && waited < 200, 1, "timeout length");

    sockctl((int)a, SOCKCTL_NONBLOCK, 1);
    check(recvfrom((int)a, buf, sizeof(buf), &from), -EAGAIN, "nonblocking receive");

    check(sendto((int)b, (void *)0xFFFF800000000000UL, 4, LO, 6000), -EFAULT, "kernel buffer");
    check(recvfrom((int)a, (void *)8, 4, &from), -EFAULT, "bad receive buffer");
    check(sendto(1, "x", 1, LO, 6000), -ENOTSOCK, "console is not a socket");
    check(sendto(99, "x", 1, LO, 6000), -EBADF, "bad descriptor");
    check(socket(77), -EPROTONOSUPPORT, "unknown socket type");
    check(sockctl((int)a, 99, 0), -EINVAL, "unknown sockctl");
    check(listen((int)a, 1), -EOPNOTSUPP, "listen on UDP");
    close((int)a);
    close((int)b);
}

static void test_tcp(void)
{
    long l = socket(SOCK_STREAM);
    check(bind((int)l, 0, 0), 0, "bind listener");
    long port = sockctl((int)l, SOCKCTL_GETNAME, 0);
    check(listen((int)l, 4), 0, "listen");

    long c = socket(SOCK_STREAM);
    check(connect((int)c, LO, (uint16_t)port), 0, "connect to ourselves");
    check(connect((int)c, LO, (uint16_t)port), -EISCONN, "connect twice");
    struct abi_sockaddr peer;
    long s = accept((int)l, &peer);
    check(s >= 3, 1, "accept");
    check(peer.ip, LO, "peer address");

    /* Data in both directions, through read/write and through sendto/recvfrom. */
    static char out[30000], in[30000];
    for (unsigned i = 0; i < sizeof(out); i++)
        out[i] = (char)(i * 7 + 3);
    size_t sent = 0;
    while (sent < sizeof(out)) {
        long n = write((int)c, out + sent, sizeof(out) - sent);
        if (n <= 0) {
            check(n, 1, "write");
            break;
        }
        sent += (size_t)n;
    }
    size_t got = 0;
    while (got < sizeof(in)) {
        long n = recvfrom((int)s, in + got, sizeof(in) - got, NULL);
        if (n <= 0) {
            check(n, 1, "read");
            break;
        }
        got += (size_t)n;
    }
    check(got, sizeof(in), "all bytes arrived");
    check(memcmp(in, out, sizeof(in)), 0, "data intact");
    check(sendto((int)s, "ack", 3, 0, 0), 3, "reply");
    char buf[8];
    check(read((int)c, buf, sizeof(buf)), 3, "reply received");
    check(memcmp(buf, "ack", 3), 0, "reply payload");

    check(sockctl((int)c, SOCKCTL_SHUT_WR, 0), 0, "shutdown write side");
    check(read((int)s, buf, sizeof(buf)), 0, "peer sees end of stream");
    check(write((int)s, "late", 4), 4, "the other direction still works");
    check(read((int)c, buf, sizeof(buf)), 4, "half-closed read");
    check(write((int)c, "x", 1), -EPIPE, "write after shutdown");
    close((int)s);
    check(read((int)c, buf, sizeof(buf)), 0, "end of stream after the peer closed");
    close((int)c);
    close((int)l);

    /* Refused connections and misuse. */
    c = socket(SOCK_STREAM);
    check(connect((int)c, LO, 6999), -ECONNREFUSED, "connection refused");
    check(write((int)c, "x", 1), -ENOTCONN, "write when not connected");
    check(accept((int)c, NULL), -EINVAL, "accept on a plain socket");
    close((int)c);

    /* Running out of descriptors is an error, not a crash. */
    int fds[20], n = 0;
    for (;;) {
        long f = socket(SOCK_DGRAM);
        if (f < 0) {
            check(f, -EMFILE, "descriptor table full");
            break;
        }
        fds[n++] = (int)f;
    }
    while (n > 0)
        close(fds[--n]);
}

int main(void)
{
    check(ping(LO, 500, 1) >= 0, 1, "ping loopback");
    check(ping(LO, 500, 2) >= 0, 1, "ping loopback again");
    test_udp();
    test_tcp();
    struct abi_netinfo ni;
    check(netinfo(&ni), 0, "netinfo");
    check(netinfo((struct abi_netinfo *)8), -EFAULT, "netinfo with a bad pointer");
    if (!failures)
        puts("t_net: all checks passed");
    return failures ? 1 : 0;
}
