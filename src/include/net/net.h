#ifndef MALEOS_NET_NET_H
#define MALEOS_NET_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi/abi.h"
#include "kernel/sched.h"
#include "kernel/spinlock.h"

/*
 * Network stack: Ethernet, ARP, IPv4, ICMP echo, UDP and TCP over one or more interfaces
 * (the loopback "lo" and, when a card is present, "eth0").
 *
 * Locking: one global spinlock (net_lock, interrupts off) protects all stack state. Packet
 * processing, socket calls and the timer all run under it; blocking calls sleep on wait
 * queues with that lock, so no other lock exists to order against.
 */

#define NET_FRAME_MAX 1536 /* a full Ethernet frame without the FCS */
#define NET_MTU 1500
#define IP_HDR_MIN 20
#define TCP_MSS (NET_MTU - IP_HDR_MIN - 20)
#define UDP_MAX_PAYLOAD (NET_MTU - IP_HDR_MIN - 8)

static inline uint16_t htons(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}
static inline uint16_t ntohs(uint16_t v)
{
    return htons(v);
}
static inline uint32_t htonl(uint32_t v)
{
    return __builtin_bswap32(v);
}
static inline uint32_t ntohl(uint32_t v)
{
    return __builtin_bswap32(v);
}

/* IPv4 addresses are kept in host byte order: 10.0.2.15 == 0x0A00020F. */
#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (d))

struct netif {
    char name[8];
    uint8_t mac[6];
    uint32_t ip, mask, gw; /* host order; 0 = not configured */
    bool up;
    bool loopback;
    void *ctx;                                                     /* driver private data */
    int (*send)(struct netif *nif, const void *frame, size_t len); /* frame incl. Ethernet header */
    uint64_t rx_frames, tx_frames, rx_bytes, tx_bytes, rx_dropped, tx_dropped;
};

extern spinlock_t net_lock;
extern int net_test_loss_percent;
int arp_lookup_for_test(uint32_t ip, uint8_t mac[6]);

/* ---- driver interface ---- */
struct netif *net_register(const char *name, const uint8_t mac[6],
                           int (*send)(struct netif *, const void *, size_t), void *ctx);
/* Called by a driver thread for every received frame (thread context, lock not held). */
void net_input(struct netif *nif, const void *frame, size_t len);

/* ---- stack ---- */
void net_init(void); /* loopback, worker thread; call after the scheduler runs */
struct netif *net_eth0(void);
struct netif *net_lo(void);
void net_configure(struct netif *nif, uint32_t ip, uint32_t mask, uint32_t gw);
void net_get_info(struct abi_netinfo *out);

/* Internet checksum over `len` bytes, continuing from `sum` (pass 0 to start). */
uint32_t net_csum_add(uint32_t sum, const void *data, size_t len);
uint16_t net_csum_fold(uint32_t sum);

/* ---- internal layers (net.c, udp.c, tcp.c) ---- */
int ip_output(uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t len);
void udp_input(uint32_t src, uint32_t dst, const uint8_t *data, size_t len);
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *data, size_t len);
void tcp_timer(void);
void arp_timer(void);
void icmp_unreachable(uint32_t to, uint8_t code, const uint8_t *orig_ip, size_t len);
bool net_is_local_ip(uint32_t ip);
uint32_t net_pick_source(uint32_t dst);
uint64_t net_ticks(void);
extern uint64_t net_stat_bad_csum, net_stat_bad_len, net_stat_frag_drop, net_stat_no_route;

/* ---- sockets (socket.c) ---- */
#define SOCK_MAX 64

int sock_create(int type);
int sock_close(int s);
int sock_bind(int s, uint32_t ip, uint16_t port);
int sock_listen(int s, int backlog);
int sock_accept(int s, uint32_t *ip, uint16_t *port); /* returns a new socket */
int sock_connect(int s, uint32_t ip, uint16_t port);
int64_t sock_send(int s, const void *buf, size_t len, uint32_t ip, uint16_t port);
int64_t sock_recv(int s, void *buf, size_t len, uint32_t *ip, uint16_t *port);
int sock_ctl(int s, int op, int64_t value, int64_t *out);
int sock_valid(int s);
int sock_count(void);

/* ---- ping ---- */
int net_ping(uint32_t ip, uint32_t timeout_ms, uint16_t seq); /* RTT in ms or -errno */

/* ---- diagnostics ---- */
void net_dump(void);
int net_selftest(void);

#endif
