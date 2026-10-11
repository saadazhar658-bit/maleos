#include "kernel/errno.h"
#include "kernel/printk.h"
#include "kernel/random.h"
#include "kernel/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "net/net_priv.h"

static int checks;

#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond))                                                                               \
            kpanic("net selftest failed: %s (%s:%d)", what, __FILE__, __LINE__);                   \
    } while (0)

#define CHECK_EQ(got, want, what)                                                                  \
    do {                                                                                           \
        long long g_ = (got), w_ = (want);                                                         \
        checks++;                                                                                  \
        if (g_ != w_)                                                                              \
            kpanic("net selftest failed: %s: got %lld, want %lld (%s:%d)", what, g_, w_, __FILE__, \
                   __LINE__);                                                                      \
    } while (0)

#define LO IP4(127, 0, 0, 1)

static uint64_t heap_used(void)
{
    struct heap_stats hs;
    heap_get_stats(&hs);
    return hs.used_bytes;
}

static uint64_t heap_mapped(void)
{
    struct heap_stats hs;
    heap_get_stats(&hs);
    return hs.mapped_bytes;
}

static uint8_t pattern(uint64_t i)
{
    return (uint8_t)((i * 2654435761u) >> 13);
}

/* ---------- unit tests ---------- */

static void test_checksum(void)
{
    /* RFC 1071 section 3 example. */
    static const uint8_t ex[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    CHECK_EQ(net_csum_fold(net_csum_add(0, ex, sizeof(ex))), 0x220d, "RFC 1071 example");
    /* A buffer with its own checksum appended sums to zero, also with an odd length. */
    uint8_t buf[33];
    for (int i = 0; i < 31; i++)
        buf[i] = pattern((uint64_t)i);
    uint16_t c = net_csum_fold(net_csum_add(0, buf, 31));
    buf[31] = (uint8_t)(c >> 8);
    buf[32] = (uint8_t)c;
    /* odd-length data pads with a zero byte, so the checksum goes in a 16-bit slot after it */
    uint8_t even[34];
    memcpy(even, buf, 31);
    even[31] = 0;
    even[32] = (uint8_t)(c >> 8);
    even[33] = (uint8_t)c;
    CHECK_EQ(net_csum_fold(net_csum_add(0, even, sizeof(even))), 0, "checksum verifies");
    CHECK_EQ(htons(0x1234), 0x3412, "htons");
    CHECK_EQ(htonl(0x11223344u), 0x44332211u, "htonl");
    CHECK_EQ(IP4(10, 0, 2, 15), 0x0A00020Fu, "IP4 macro");
}

static void test_ping_loopback(void)
{
    int rtt = net_ping(LO, 1000, 1);
    CHECK(rtt >= 0, "ping 127.0.0.1");
    CHECK(net_ping(LO, 1000, 2) >= 0, "second ping");
    if (!net_eth0()) {
        CHECK_EQ(net_ping(IP4(10, 0, 2, 2), 200, 3), -ENETUNREACH, "no route without a card");
    }
}

/* ---------- UDP ---------- */

static void test_udp(void)
{
    int a = sock_create(SOCK_DGRAM), b = sock_create(SOCK_DGRAM);
    CHECK(a >= 0 && b >= 0, "udp sockets");
    CHECK_EQ(sock_bind(a, 0, 5000), 0, "bind a");
    CHECK_EQ(sock_bind(b, 0, 5000), -EADDRINUSE, "double bind");
    CHECK_EQ(sock_bind(b, 0, 5001), 0, "bind b");
    CHECK_EQ(sock_bind(b, 0, 5002), -EINVAL, "bind twice");

    char buf[64];
    uint32_t ip;
    uint16_t port;
    int64_t one = 1;
    CHECK_EQ(sock_ctl(a, SOCKCTL_NONBLOCK, 1, &one), 0, "nonblock");
    CHECK_EQ(sock_recv(a, buf, sizeof(buf), &ip, &port), -EAGAIN, "empty nonblocking recv");

    CHECK_EQ(sock_send(b, "hello", 5, LO, 5000), 5, "sendto");
    thread_sleep_ms(30);
    CHECK_EQ(sock_recv(a, buf, sizeof(buf), &ip, &port), 5, "recvfrom");
    CHECK(memcmp(buf, "hello", 5) == 0, "payload");
    CHECK_EQ(ip, LO, "source address");
    CHECK_EQ(port, 5001, "source port");

    /* A reply, a truncated receive, and an empty datagram. */
    CHECK_EQ(sock_send(a, "0123456789", 10, LO, 5001), 10, "reply");
    thread_sleep_ms(30);
    CHECK_EQ(sock_recv(b, buf, 4, &ip, &port), 4, "truncated recv returns the buffer size");
    CHECK(memcmp(buf, "0123", 4) == 0, "truncated payload");
    CHECK_EQ(sock_send(a, "", 0, LO, 5001), 0, "empty datagram");
    thread_sleep_ms(30);
    CHECK_EQ(sock_recv(b, buf, sizeof(buf), &ip, &port), 0, "empty datagram arrives");

    /* The receive timeout. */
    int64_t out;
    sock_ctl(a, SOCKCTL_NONBLOCK, 0, &out);
    sock_ctl(a, SOCKCTL_RCVTIMEO, 100, &out);
    uint64_t t0 = net_ticks();
    CHECK_EQ(sock_recv(a, buf, sizeof(buf), &ip, &port), -EAGAIN, "recv times out");
    uint64_t waited = net_ticks() - t0;
    CHECK(waited >= 9 && waited <= 14, "timeout is about 100 ms");

    /* Size limits, errors, and a packet to a port nobody listens on. */
    static uint8_t big[2000];
    CHECK_EQ(sock_send(b, big, UDP_MAX_PAYLOAD, LO, 5000), UDP_MAX_PAYLOAD, "largest datagram");
    CHECK_EQ(sock_send(b, big, UDP_MAX_PAYLOAD + 1, LO, 5000), -EMSGSIZE, "too large");
    CHECK_EQ(sock_send(b, "x", 1, LO, 0), -EINVAL, "port 0");
    CHECK_EQ(sock_send(b, "x", 1, LO, 6000), 1, "to a closed port is not an error for UDP");
    thread_sleep_ms(30);
    sock_ctl(a, SOCKCTL_RCVTIMEO, 100, &out);
    CHECK_EQ(sock_recv(a, big, sizeof(big), &ip, &port), UDP_MAX_PAYLOAD, "big datagram received");

    /* Queue overflow drops, not crashes. */
    uint64_t dropped = net_stat_udp_dropped;
    for (int i = 0; i < UDP_RXQ_MAX + 8; i++)
        sock_send(b, "q", 1, LO, 5000);
    thread_sleep_ms(100);
    CHECK(net_stat_udp_dropped > dropped, "overflow is counted");

    /* connect() filters by peer. */
    int c = sock_create(SOCK_DGRAM);
    CHECK_EQ(sock_connect(c, LO, 5001), 0, "udp connect");
    CHECK_EQ(sock_send(c, "via connect", 11, 0, 0), 11, "send on a connected socket");
    CHECK_EQ(sock_send(sock_create(SOCK_DGRAM), "x", 1, 0, 0), -EDESTADDRREQ, "no destination");

    CHECK_EQ(sock_close(a), 0, "close a");
    CHECK_EQ(sock_close(b), 0, "close b");
    CHECK_EQ(sock_close(c), 0, "close c");
    CHECK_EQ(sock_close(c), -EBADF, "double close");
    for (int i = 0; i < SOCK_MAX; i++) /* the stray socket from the EDESTADDRREQ check */
        if (socks[i].used)
            sock_close(i);
    thread_sleep_ms(50);
}

/* ---------- TCP ---------- */

struct echo_arg {
    int listener;
    int mode;       /* 0 echo until EOF, 1 read nothing then close, 2 sleep then read */
    int conns;      /* connections to serve */
    uint64_t total; /* bytes echoed */
};

static void echo_thread(void *p)
{
    struct echo_arg *a = p;
    static uint8_t buf[4096];
    for (int n = 0; n < a->conns; n++) {
        int c = sock_accept(a->listener, NULL, NULL);
        if (c < 0)
            break;
        if (a->mode == 1) {
            sock_close(c);
            continue;
        }
        if (a->mode == 2)
            thread_sleep_ms(400);
        for (;;) {
            int64_t r = sock_recv(c, buf, sizeof(buf), NULL, NULL);
            if (r <= 0)
                break;
            int64_t w = sock_send(c, buf, (size_t)r, 0, 0);
            if (w != r)
                break;
            a->total += (uint64_t)r;
        }
        sock_close(c);
    }
    thread_exit(0);
}

static int make_listener(uint16_t port, int backlog)
{
    int l = sock_create(SOCK_STREAM);
    CHECK(l >= 0, "listener socket");
    CHECK_EQ(sock_bind(l, 0, port), 0, "bind listener");
    CHECK_EQ(sock_listen(l, backlog), 0, "listen");
    return l;
}

static int64_t send_all(int s, const uint8_t *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        int64_t w = sock_send(s, buf + done, len - done, 0, 0);
        if (w < 0)
            return w;
        done += (size_t)w;
    }
    return (int64_t)done;
}

static int64_t recv_all(int s, uint8_t *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        int64_t r = sock_recv(s, buf + done, len - done, NULL, NULL);
        if (r < 0)
            return r;
        if (r == 0)
            break;
        done += (size_t)r;
    }
    return (int64_t)done;
}

/* Send `len` patterned bytes through an echo server and compare what comes back. */
static void roundtrip(uint16_t port, size_t len, const char *what)
{
    static uint8_t out[1 << 17], in[1 << 17];
    CHECK(len <= sizeof(out), "roundtrip size");
    for (size_t i = 0; i < len; i++)
        out[i] = pattern(i);

    int l = make_listener(port, 4);
    struct echo_arg ea = {.listener = l, .mode = 0, .conns = 1};
    struct thread *t = thread_create("echo", echo_thread, &ea, 4);

    int c = sock_create(SOCK_STREAM);
    CHECK_EQ(sock_connect(c, LO, port), 0, "connect");
    /* Send and receive concurrently: a 128 KiB echo cannot fit in the buffers of both sides. */
    size_t sent = 0, got = 0;
    int64_t one = 1, unused;
    sock_ctl(c, SOCKCTL_NONBLOCK, 1, &unused);
    (void)one;
    uint64_t start = net_ticks();
    while (got < len) {
        bool progress = false;
        if (sent < len) {
            int64_t w = sock_send(c, out + sent, len - sent, 0, 0);
            if (w > 0) {
                sent += (size_t)w;
                progress = true;
            }
            if (sent == len)
                sock_ctl(c, SOCKCTL_SHUT_WR, 0, &unused);
        }
        int64_t r = sock_recv(c, in + got, len - got, NULL, NULL);
        if (r > 0) {
            got += (size_t)r;
            progress = true;
        } else if (r == 0) {
            break;
        } else if (r != -EAGAIN) {
            kpanic("net selftest failed: %s: recv error %lld", what, (long long)r);
        }
        if (!progress)
            thread_sleep_ms(1);
        if (net_ticks() - start > 60 * TIMER_HZ)
            kpanic("net selftest failed: %s: transfer stalled at %zu/%zu sent, %zu received", what,
                   sent, len, got);
    }
    CHECK_EQ(got, len, what);
    CHECK(memcmp(in, out, len) == 0, "data survives the round trip");
    /* After our FIN the server sees EOF, finishes, and closes: we must see EOF too. */
    uint8_t tmp[8];
    sock_ctl(c, SOCKCTL_NONBLOCK, 0, &unused);
    sock_ctl(c, SOCKCTL_RCVTIMEO, 3000, &unused);
    CHECK_EQ(sock_recv(c, tmp, sizeof(tmp), NULL, NULL), 0, "orderly EOF");
    sock_close(c);
    thread_join(t);
    CHECK_EQ(ea.total, len, "server echoed everything");
    sock_close(l);
}

static void test_tcp_basic(void)
{
    roundtrip(7001, 1, "one byte");
    roundtrip(7002, 1460, "exactly one segment");
    roundtrip(7003, 1461, "one segment and a byte");
    roundtrip(7004, 100000, "100 KB");
}

static void test_tcp_errors(void)
{
    int c = sock_create(SOCK_STREAM);
    CHECK_EQ(sock_connect(c, LO, 7100), -ECONNREFUSED, "connection refused");
    CHECK_EQ(sock_send(c, "x", 1, 0, 0), -ENOTCONN, "send on an unconnected socket");
    uint8_t b[4];
    CHECK_EQ(sock_recv(c, b, sizeof(b), NULL, NULL), -ENOTCONN, "recv on an unconnected socket");
    sock_close(c);

    int l = make_listener(7101, 2);
    CHECK_EQ(sock_listen(l, 2), -EINVAL, "listen twice");
    int d = sock_create(SOCK_STREAM);
    CHECK_EQ(sock_bind(d, 0, 7101), -EADDRINUSE, "port taken by a listener");
    CHECK_EQ(sock_accept(d, NULL, NULL), -EINVAL, "accept on a non-listener");
    sock_close(d);
    int64_t out;
    sock_ctl(l, SOCKCTL_NONBLOCK, 1, &out);
    CHECK_EQ(sock_accept(l, NULL, NULL), -EAGAIN, "nothing to accept");
    sock_close(l);

    /* Server closes at once without reading: the client sees EOF or a reset, never a hang. */
    l = make_listener(7102, 2);
    struct echo_arg ea = {.listener = l, .mode = 1, .conns = 1};
    struct thread *t = thread_create("echo", echo_thread, &ea, 4);
    c = sock_create(SOCK_STREAM);
    CHECK_EQ(sock_connect(c, LO, 7102), 0, "connect to a closing server");
    sock_ctl(c, SOCKCTL_RCVTIMEO, 3000, &out);
    int64_t r = sock_recv(c, b, sizeof(b), NULL, NULL);
    CHECK(r == 0 || r == -ECONNRESET, "EOF or reset after the server closed");
    sock_close(c);
    thread_join(t);
    sock_close(l);

    /* Unread data at close sends a reset. */
    l = make_listener(7103, 2);
    ea = (struct echo_arg){.listener = l, .mode = 1, .conns = 1};
    t = thread_create("echo", echo_thread, &ea, 4);
    c = sock_create(SOCK_STREAM);
    CHECK_EQ(sock_connect(c, LO, 7103), 0, "connect");
    sock_ctl(c, SOCKCTL_RCVTIMEO, 3000, &out);
    thread_sleep_ms(100);
    int64_t w = 0;
    for (int i = 0; i < 20 && w >= 0; i++) {
        w = sock_send(c, "data", 4, 0, 0);
        thread_sleep_ms(20);
    }
    CHECK(w < 0, "writing to a closed peer eventually fails");
    sock_close(c);
    thread_join(t);
    sock_close(l);
}

static void client_thread(void *p)
{
    uint64_t *res = p;
    uint16_t port = (uint16_t)(*res);
    int c = sock_create(SOCK_STREAM);
    *res = 0;
    if (sock_connect(c, LO, port) == 0) {
        uint8_t msg[16], back[16];
        for (int i = 0; i < 16; i++)
            msg[i] = (uint8_t)(port + i);
        if (send_all(c, msg, 16) == 16 && recv_all(c, back, 16) == 16 && !memcmp(msg, back, 16))
            *res = 1;
    }
    sock_close(c);
    thread_exit(0);
}

static void test_tcp_parallel(void)
{
    enum { N = 6 };
    int l = make_listener(7200, 8);
    struct echo_arg ea = {.listener = l, .mode = 0, .conns = N};
    struct thread *srv[N], *cli[N];
    uint64_t res[N];
    /* One echo thread per connection so they run in parallel. */
    struct echo_arg eas[N];
    for (int i = 0; i < N; i++) {
        eas[i] = (struct echo_arg){.listener = l, .mode = 0, .conns = 1};
        srv[i] = thread_create("echo", echo_thread, &eas[i], 4);
    }
    for (int i = 0; i < N; i++) {
        res[i] = 7200;
        cli[i] = thread_create("cli", client_thread, &res[i], 4);
    }
    for (int i = 0; i < N; i++)
        thread_join(cli[i]);
    for (int i = 0; i < N; i++)
        thread_join(srv[i]);
    for (int i = 0; i < N; i++)
        CHECK_EQ(res[i], 1, "parallel connection works");
    sock_close(l);
    (void)ea;
}

static void test_tcp_zero_window(void)
{
    /* The server sleeps before reading, so the client fills its window and must wait. */
    static uint8_t out[60000], in[60000];
    for (size_t i = 0; i < sizeof(out); i++)
        out[i] = pattern(i + 7);
    int l = make_listener(7300, 2);
    struct echo_arg ea = {.listener = l, .mode = 2, .conns = 1};
    struct thread *t = thread_create("echo", echo_thread, &ea, 4);
    int c = sock_create(SOCK_STREAM);
    CHECK_EQ(sock_connect(c, LO, 7300), 0, "connect");
    int64_t unused;
    sock_ctl(c, SOCKCTL_NONBLOCK, 1, &unused);
    size_t sent = 0, got = 0;
    uint64_t start = net_ticks();
    while (got < sizeof(out)) {
        if (sent < sizeof(out)) {
            int64_t w = sock_send(c, out + sent, sizeof(out) - sent, 0, 0);
            if (w > 0)
                sent += (size_t)w;
            if (sent == sizeof(out))
                sock_ctl(c, SOCKCTL_SHUT_WR, 0, &unused);
        }
        int64_t r = sock_recv(c, in + got, sizeof(in) - got, NULL, NULL);
        if (r > 0)
            got += (size_t)r;
        else if (r == 0)
            break;
        thread_sleep_ms(2);
        if (net_ticks() - start > 60 * TIMER_HZ)
            kpanic("net selftest failed: zero-window transfer stalled at %zu/%zu", sent, got);
    }
    CHECK_EQ(got, sizeof(out), "everything came back after the window closed");
    CHECK(memcmp(in, out, sizeof(out)) == 0, "data intact after zero-window stalls");
    sock_close(c);
    thread_join(t);
    sock_close(l);
}

static void test_tcp_loss(void)
{
    uint64_t retrans = net_stat_tcp_retrans;
    net_test_loss_percent = 10;
    roundtrip(7400, 30000, "30 KB with 10% packet loss");
    net_test_loss_percent = 0;
    CHECK(net_stat_tcp_retrans > retrans, "lost packets were retransmitted");
}

/* ---------- malformed and hostile packets ---------- */

static uint8_t fake_tx[2048];
static size_t fake_tx_len;
static int fake_send(struct netif *n, const void *f, size_t len)
{
    (void)n;
    fake_tx_len = len < sizeof(fake_tx) ? len : sizeof(fake_tx);
    memcpy(fake_tx, f, fake_tx_len);
    return 0;
}

static size_t build_ip(uint8_t *pkt, uint8_t proto, uint32_t src, uint32_t dst,
                       const uint8_t *payload, size_t len)
{
    pkt[0] = 0x45;
    pkt[1] = 0;
    pkt[2] = (uint8_t)((20 + len) >> 8);
    pkt[3] = (uint8_t)(20 + len);
    pkt[4] = pkt[5] = 0;
    pkt[6] = 0x40;
    pkt[7] = 0;
    pkt[8] = 64;
    pkt[9] = proto;
    pkt[10] = pkt[11] = 0;
    pkt[12] = (uint8_t)(src >> 24);
    pkt[13] = (uint8_t)(src >> 16);
    pkt[14] = (uint8_t)(src >> 8);
    pkt[15] = (uint8_t)src;
    pkt[16] = (uint8_t)(dst >> 24);
    pkt[17] = (uint8_t)(dst >> 16);
    pkt[18] = (uint8_t)(dst >> 8);
    pkt[19] = (uint8_t)dst;
    uint16_t c = net_csum_fold(net_csum_add(0, pkt, 20));
    pkt[10] = (uint8_t)(c >> 8);
    pkt[11] = (uint8_t)c;
    memcpy(pkt + 20, payload, len);
    return 20 + len;
}

static void fix_ip_csum(uint8_t *pkt, size_t len)
{
    if (len < 20)
        return;
    pkt[10] = pkt[11] = 0;
    size_t ihl = (size_t)(pkt[0] & 15) * 4;
    if (ihl < 20 || ihl > len)
        return;
    uint16_t c = net_csum_fold(net_csum_add(0, pkt, ihl));
    pkt[10] = (uint8_t)(c >> 8);
    pkt[11] = (uint8_t)c;
}

static void test_arp_and_garbage(void)
{
    static const uint8_t fmac[6] = {0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc};
    static const uint8_t peer[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    struct netif fake;
    memset(&fake, 0, sizeof(fake));
    memcpy(fake.mac, fmac, 6);
    fake.ip = IP4(10, 9, 9, 9);
    fake.up = true;
    fake.send = fake_send;

    /* An ARP request for us is answered, and teaches us the sender. */
    uint8_t f[64] = {0};
    memset(f, 0xff, 6);
    memcpy(f + 6, peer, 6);
    f[12] = 0x08;
    f[13] = 0x06;
    uint8_t *a = f + 14;
    a[1] = 1;
    a[2] = 0x08;
    a[4] = 6;
    a[5] = 4;
    a[7] = 1; /* request */
    memcpy(a + 8, peer, 6);
    a[14] = 10;
    a[15] = 9;
    a[16] = 9;
    a[17] = 7; /* sender 10.9.9.7 */
    a[24] = 10;
    a[25] = 9;
    a[26] = 9;
    a[27] = 9; /* target 10.9.9.9 */
    fake_tx_len = 0;
    net_input(&fake, f, 14 + 28);
    CHECK_EQ(fake_tx_len, 14 + 28, "ARP reply sent");
    CHECK(fake_tx[12] == 0x08 && fake_tx[13] == 0x06 && fake_tx[21] == 2, "it is a reply");
    CHECK(memcmp(fake_tx, peer, 6) == 0, "reply goes to the asker");
    CHECK(memcmp(fake_tx + 22, fmac, 6) == 0, "reply carries our address");
    uint8_t got[6];
    CHECK_EQ(arp_lookup_for_test(IP4(10, 9, 9, 7), got), 0, "sender was cached");
    CHECK(memcmp(got, peer, 6) == 0, "cached address");

    /* A request for somebody else is not answered. */
    a[27] = 10;
    fake_tx_len = 0;
    net_input(&fake, f, 14 + 28);
    CHECK_EQ(fake_tx_len, 0, "ARP for another host is ignored");

    /* A broadcast or loopback sender must not poison the cache. */
    a[27] = 9;
    a[14] = 127;
    a[15] = a[16] = 0;
    a[17] = 1;
    net_input(&fake, f, 14 + 28);
    CHECK_EQ(arp_lookup_for_test(IP4(127, 0, 0, 1), got), -ENOENT, "loopback sender is not cached");

    /* Frames for another MAC address, and runt frames, are dropped. */
    uint8_t other[60] = {0};
    other[0] = 0x02;
    other[12] = 0x08;
    uint64_t dropped = fake.rx_dropped;
    net_input(&fake, other, sizeof(other));
    CHECK(fake.rx_dropped > dropped, "foreign unicast dropped");
    net_input(&fake, other, 5);
    net_input(&fake, other, 0);

    /* Corrupt checksums and fragments are counted and ignored. */
    int s = sock_create(SOCK_DGRAM);
    sock_bind(s, 0, 9000);
    uint8_t udp[8 + 4] = {0x13, 0x88, 0x23, 0x28, 0, 12, 0, 0, 'a', 'b', 'c', 'd'};
    uint8_t pkt[128], frame[160];
    size_t n = build_ip(pkt, 17, LO, LO, udp, sizeof(udp));
    memcpy(frame, fmac, 6);
    memcpy(frame + 6, peer, 6);
    frame[12] = 0x08;
    frame[13] = 0x00;
    memcpy(frame + 14, pkt, n);
    uint64_t bad = net_stat_bad_csum, frag = net_stat_frag_drop;
    frame[14 + 10] ^= 0xff; /* IP header checksum */
    net_input(&fake, frame, 14 + n);
    CHECK(net_stat_bad_csum > bad, "bad IP checksum counted");
    frame[14 + 10] ^= 0xff;
    frame[14 + 6] = 0x20; /* more fragments */
    fix_ip_csum(frame + 14, n);
    net_input(&fake, frame, 14 + n);
    CHECK(net_stat_frag_drop > frag, "fragments dropped");
    frame[14 + 6] = 0x40;
    fix_ip_csum(frame + 14, n);
    bad = net_stat_bad_csum;
    net_input(&fake, frame, 14 + n); /* UDP checksum is 0 = none: accepted */
    thread_sleep_ms(20);
    int64_t out;
    sock_ctl(s, SOCKCTL_NONBLOCK, 1, &out);
    char rb[16];
    CHECK_EQ(sock_recv(s, rb, sizeof(rb), NULL, NULL), 4,
             "a UDP packet with no checksum is accepted");
    frame[14 + 20 + 6] = 0x12; /* a checksum that is wrong */
    net_input(&fake, frame, 14 + n);
    CHECK(net_stat_bad_csum > bad, "bad UDP checksum counted");
    CHECK_EQ(sock_recv(s, rb, sizeof(rb), NULL, NULL), -EAGAIN, "and the packet is not delivered");
    sock_close(s);
}

static void test_fuzz_packets(void)
{
    static const uint8_t fmac[6] = {0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc};
    struct netif fake;
    memset(&fake, 0, sizeof(fake));
    memcpy(fake.mac, fmac, 6);
    fake.ip = IP4(10, 9, 9, 9);
    fake.send = fake_send;

    /* Live targets for the fuzzed packets. */
    int u = sock_create(SOCK_DGRAM);
    sock_bind(u, 0, 9100);
    int l = make_listener(9101, 4);

    uint8_t udp[16] = {0x23, 0x8c, 0x23, 0x8c, 0, 16, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t tcp[24] = {0x30, 0x39, 0x23, 0x8d, 0,    0,    0, 100, 0, 0,
                       0,    0,    0x50, 0x02, 0xff, 0xff, 0, 0,   0, 0};
    uint8_t icmp[16] = {8, 0, 0, 0, 0x12, 0x34, 0, 1, 'p', 'i', 'n', 'g'};
    const uint8_t *protos[3] = {udp, tcp, icmp};
    size_t plens[3] = {16, 20, 12};
    uint8_t pnum[3] = {17, 6, 1};

    uint64_t rnd = 0x1234;
    for (int iter = 0; iter < 4000; iter++) {
        rnd = rnd * 6364136223846793005ULL + 1442695040888963407ULL;
        int k = (int)((rnd >> 33) % 3);
        uint8_t body[64];
        size_t blen = plens[k];
        memcpy(body, protos[k], blen);
        uint8_t pkt[160], frame[192];
        size_t n = build_ip(pkt, pnum[k], IP4(127, 0, 0, 1), LO, body, blen);
        int flips = (int)((rnd >> 20) % 4);
        for (int i = 0; i < flips; i++) {
            rnd = rnd * 6364136223846793005ULL + 1442695040888963407ULL;
            pkt[(rnd >> 33) % n] ^= (uint8_t)(1u << ((rnd >> 20) % 8));
        }
        if ((rnd >> 50) & 1) /* usually repair the checksums so the packet reaches the protocol */
            fix_ip_csum(pkt, n);
        size_t cut = ((rnd >> 40) % 4 == 0) ? (size_t)((rnd >> 8) % (n + 1)) : n;
        memcpy(frame, fmac, 6);
        memset(frame + 6, 2, 6);
        frame[12] = 0x08;
        frame[13] = 0x00;
        memcpy(frame + 14, pkt, cut);
        net_input(&fake, frame, 14 + cut);
    }
    /* Pure random garbage as well. */
    for (int iter = 0; iter < 2000; iter++) {
        uint8_t frame[256];
        size_t len = random_u64() % sizeof(frame);
        random_bytes(frame, len);
        if (len >= 14 && (iter & 1)) {
            memcpy(frame, fmac, 6);
            frame[12] = (iter & 2) ? 0x08 : 0x08;
            frame[13] = (iter & 4) ? 0x00 : 0x06;
        }
        net_input(&fake, frame, len);
    }
    thread_sleep_ms(100); /* let the worker thread drain the loopback queue */
    sock_close(u);
    sock_close(l);
    thread_sleep_ms(50);
}

/* ---------- the card ---------- */

static void test_nic(void)
{
    struct netif *e = net_eth0();
    if (!e) {
        printk("  no network card: skipping the wire tests\n");
        return;
    }
    CHECK(e->up && e->ip == IP4(10, 0, 2, 15), "eth0 is configured for QEMU user networking");
    /* ARP for the gateway, then an ICMP echo through the card and back. */
    int rtt = net_ping(IP4(10, 0, 2, 2), 2000, 1);
    if (rtt < 0) {
        printk("  gateway 10.0.2.2 does not answer (%d): not QEMU user networking, skipping\n",
               rtt);
        return;
    }
    CHECK(rtt < 1000, "gateway ping");
    uint8_t mac[6];
    CHECK_EQ(arp_lookup_for_test(IP4(10, 0, 2, 2), mac), 0, "the gateway is in the ARP cache");
    CHECK(net_ping(IP4(10, 0, 2, 2), 2000, 2) >= 0, "ping again from the cache");
    CHECK_EQ(net_ping(IP4(10, 0, 2, 77), 400, 3), -ETIMEDOUT, "an address that does not answer");
    printk("  eth0 answers: ping 10.0.2.2 in %d ms\n", rtt);
}

int net_selftest(void)
{
    checks = 0;
    test_checksum();

    /* Warm-up: first use allocates buffers and queues that stay around. */
    net_ping(LO, 500, 0);
    {
        int a = sock_create(SOCK_DGRAM);
        sock_bind(a, 0, 4999);
        sock_send(a, "w", 1, LO, 4999);
        thread_sleep_ms(30);
        char c;
        sock_recv(a, &c, 1, NULL, NULL);
        sock_close(a);
    }
    roundtrip(7999, 70000, "warm-up transfer");
    thread_sleep_ms(2500); /* let TIME_WAIT end so the baseline has no connections */
    uint64_t heap = heap_used(), frames = pmm_free_frame_count(), mapped = heap_mapped();
    int conns = tcp_connection_count();

    test_ping_loopback();
    test_udp();
    test_tcp_basic();
    test_tcp_errors();
    test_tcp_parallel();
    test_tcp_zero_window();
    test_tcp_loss();
    test_arp_and_garbage();
    test_fuzz_packets();
    test_nic();

    thread_sleep_ms(2600); /* TIME_WAIT */
    CHECK_EQ(sock_count(), 0, "no sockets left");
    CHECK_EQ(tcp_connection_count(), conns, "no connections left");
    /* The heap never gives frames back, so a new high-water mark legitimately uses some. */
    CHECK(frames - pmm_free_frame_count() <= (heap_mapped() - mapped) / PAGE_SIZE,
          "no frames leaked outside the heap");
    CHECK_EQ(heap_used(), heap, "no heap leaked");
    return checks;
}
