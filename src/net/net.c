#include "net/net.h"

#include "kernel/errno.h"
#include "kernel/list.h"
#include "kernel/printk.h"
#include "kernel/string.h"
#include "mm/heap.h"

spinlock_t net_lock = SPINLOCK_INIT;

int net_test_loss_percent; /* self-test only: drop this percentage of loopback packets */
static uint32_t loss_state = 12345;

uint64_t net_stat_bad_csum, net_stat_bad_len, net_stat_frag_drop, net_stat_no_route;

#define ETH_HDR 14
#define ETH_IP 0x0800
#define ETH_ARP 0x0806

#define PROTO_ICMP 1
#define PROTO_TCP 6
#define PROTO_UDP 17

static struct netif lo, eth0;
static bool eth0_present;
static struct waitq net_wq;
static bool net_ready;

/* Packets sent to ourselves wait here until the worker thread delivers them. */
struct lo_pkt {
    struct list_node node;
    size_t len;
    uint8_t data[];
};
static struct list_node lo_queue;
static int lo_queued;
#define LO_QUEUE_MAX 256

uint64_t net_ticks(void)
{
    return timer_ticks();
}

/* ---------- checksum ---------- */

uint32_t net_csum_add(uint32_t sum, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)(p[0] << 8);
    return sum;
}

uint16_t net_csum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ---------- interfaces ---------- */

struct netif *net_eth0(void)
{
    return eth0_present ? &eth0 : NULL;
}

struct netif *net_lo(void)
{
    return &lo;
}

void net_configure(struct netif *nif, uint32_t ip, uint32_t mask, uint32_t gw)
{
    uint64_t f = spin_lock_irqsave(&net_lock);
    nif->ip = ip;
    nif->mask = mask;
    nif->gw = gw;
    spin_unlock_irqrestore(&net_lock, f);
}

struct netif *net_register(const char *name, const uint8_t mac[6],
                           int (*send)(struct netif *, const void *, size_t), void *ctx)
{
    if (eth0_present)
        return NULL;
    memset(&eth0, 0, sizeof(eth0));
    strlcpy(eth0.name, name, sizeof(eth0.name));
    memcpy(eth0.mac, mac, 6);
    eth0.send = send;
    eth0.ctx = ctx;
    eth0.up = true;
    /* Static configuration matching QEMU's user-mode network; there is no DHCP client. */
    eth0.ip = IP4(10, 0, 2, 15);
    eth0.mask = IP4(255, 255, 255, 0);
    eth0.gw = IP4(10, 0, 2, 2);
    eth0_present = true;
    return &eth0;
}

bool net_is_local_ip(uint32_t ip)
{
    if ((ip >> 24) == 127)
        return true;
    return eth0_present && eth0.ip && ip == eth0.ip;
}

uint32_t net_pick_source(uint32_t dst)
{
    if ((dst >> 24) == 127)
        return IP4(127, 0, 0, 1);
    if (net_is_local_ip(dst))
        return dst;
    return eth0_present ? eth0.ip : 0;
}

/* ---------- ARP ---------- */

#define ARP_ENTRIES 16
#define ARP_PENDING 0
#define ARP_VALID 1
#define ARP_RETRY_TICKS 100 /* 1 s */
#define ARP_MAX_TRIES 3
#define ARP_LIFETIME_TICKS (5 * 60 * TIMER_HZ)

struct arp_entry {
    bool used;
    bool valid;
    uint32_t ip;
    uint8_t mac[6];
    uint64_t stamp; /* last request (pending) or last confirmation (valid) */
    int tries;
    uint8_t *queued; /* one IP packet waiting for the answer */
    size_t queued_len;
};
static struct arp_entry arp_cache[ARP_ENTRIES];

static struct arp_entry *arp_find(uint32_t ip)
{
    for (int i = 0; i < ARP_ENTRIES; i++)
        if (arp_cache[i].used && arp_cache[i].ip == ip)
            return &arp_cache[i];
    return NULL;
}

static void arp_free(struct arp_entry *e)
{
    kfree(e->queued);
    memset(e, 0, sizeof(*e));
}

static struct arp_entry *arp_alloc(uint32_t ip)
{
    struct arp_entry *victim = NULL;
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (!arp_cache[i].used) {
            victim = &arp_cache[i];
            break;
        }
        if (!victim || arp_cache[i].stamp < victim->stamp)
            victim = &arp_cache[i];
    }
    arp_free(victim);
    victim->used = true;
    victim->ip = ip;
    victim->stamp = net_ticks();
    return victim;
}

struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[6];
    uint8_t spa[4];
    uint8_t tha[6];
    uint8_t tpa[4];
} __attribute__((packed));

struct eth_hdr {
    uint8_t dst[6], src[6];
    uint16_t type;
} __attribute__((packed));

static void put_ip(uint8_t *dst, uint32_t ip)
{
    dst[0] = (uint8_t)(ip >> 24);
    dst[1] = (uint8_t)(ip >> 16);
    dst[2] = (uint8_t)(ip >> 8);
    dst[3] = (uint8_t)ip;
}

static uint32_t get_ip(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) | src[3];
}

static int eth_send(struct netif *nif, const uint8_t dst[6], uint16_t type, const void *payload,
                    size_t len)
{
    uint8_t frame[NET_FRAME_MAX];
    if (len > NET_FRAME_MAX - ETH_HDR)
        return -EMSGSIZE;
    struct eth_hdr *eh = (struct eth_hdr *)frame;
    memcpy(eh->dst, dst, 6);
    memcpy(eh->src, nif->mac, 6);
    eh->type = htons(type);
    memcpy(frame + ETH_HDR, payload, len);
    size_t total = ETH_HDR + len;
    int rc = nif->send(nif, frame, total);
    if (rc < 0) {
        nif->tx_dropped++;
        return rc;
    }
    nif->tx_frames++;
    nif->tx_bytes += total;
    return 0;
}

static void arp_send(struct netif *nif, uint16_t op, const uint8_t tha[6], uint32_t tpa)
{
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t zero[6] = {0};
    struct arp_pkt a;
    a.htype = htons(1);
    a.ptype = htons(ETH_IP);
    a.hlen = 6;
    a.plen = 4;
    a.op = htons(op);
    memcpy(a.sha, nif->mac, 6);
    put_ip(a.spa, nif->ip);
    memcpy(a.tha, op == 1 ? zero : tha, 6);
    put_ip(a.tpa, tpa);
    eth_send(nif, op == 1 ? bcast : tha, ETH_ARP, &a, sizeof(a));
}

static void arp_input(struct netif *nif, const uint8_t *data, size_t len)
{
    if (len < sizeof(struct arp_pkt))
        return;
    const struct arp_pkt *a = (const struct arp_pkt *)data;
    if (ntohs(a->htype) != 1 || ntohs(a->ptype) != ETH_IP || a->hlen != 6 || a->plen != 4)
        return;
    uint16_t op = ntohs(a->op);
    uint32_t spa = get_ip(a->spa), tpa = get_ip(a->tpa);
    if (spa == 0 || (spa >> 24) == 127 || (a->sha[0] & 1))
        return; /* unspecified, loopback or multicast/broadcast sender: never cache */

    bool for_us = nif->ip && tpa == nif->ip;
    struct arp_entry *e = arp_find(spa);
    if (!e && for_us && op == 1)
        e = arp_alloc(spa); /* a neighbour that talks to us will probably get an answer from us */
    if (e) {
        memcpy(e->mac, a->sha, 6);
        e->valid = true;
        e->stamp = net_ticks();
        if (e->queued) {
            uint8_t *pkt = e->queued;
            size_t plen = e->queued_len;
            e->queued = NULL;
            eth_send(nif, e->mac, ETH_IP, pkt, plen);
            kfree(pkt);
        }
    }
    if (op == 1 && for_us)
        arp_send(nif, 2, a->sha, spa);
}

void arp_timer(void)
{
    uint64_t now = net_ticks();
    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *e = &arp_cache[i];
        if (!e->used)
            continue;
        if (!e->valid) {
            if (now - e->stamp >= ARP_RETRY_TICKS) {
                if (++e->tries >= ARP_MAX_TRIES) {
                    arp_free(e);
                } else {
                    e->stamp = now;
                    arp_send(&eth0, 1, NULL, e->ip);
                }
            }
        } else if (now - e->stamp >= ARP_LIFETIME_TICKS) {
            arp_free(e);
        }
    }
}

/* Send an IP packet out of eth0 to the on-link address `next`. */
static int eth_ip_send(uint32_t next, const uint8_t *pkt, size_t len)
{
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (next == 0xffffffffu || (eth0.mask && next == (eth0.ip | ~eth0.mask)))
        return eth_send(&eth0, bcast, ETH_IP, pkt, len);

    struct arp_entry *e = arp_find(next);
    if (e && e->valid)
        return eth_send(&eth0, e->mac, ETH_IP, pkt, len);

    if (!e) {
        e = arp_alloc(next);
        arp_send(&eth0, 1, NULL, next);
    }
    if (!e->valid && !e->queued) {
        e->queued = kmalloc(len);
        if (e->queued) {
            memcpy(e->queued, pkt, len);
            e->queued_len = len;
        }
    }
    return 0; /* queued or dropped like a lost packet: upper layers retransmit */
}

int arp_lookup_for_test(uint32_t ip, uint8_t mac[6]);
int arp_lookup_for_test(uint32_t ip, uint8_t mac[6])
{
    struct arp_entry *e = arp_find(ip);
    if (!e || !e->valid)
        return -ENOENT;
    memcpy(mac, e->mac, 6);
    return 0;
}

/* ---------- IPv4 ---------- */

struct ip_hdr {
    uint8_t vihl, tos;
    uint16_t total, id, frag;
    uint8_t ttl, proto;
    uint16_t csum;
    uint32_t src, dst;
} __attribute__((packed));

static uint16_t ip_id;

int ip_output(uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t len)
{
    if (len > NET_MTU - IP_HDR_MIN)
        return -EMSGSIZE;
    if (!src)
        src = net_pick_source(dst);

    bool to_lo = (dst >> 24) == 127 || net_is_local_ip(dst);
    uint32_t next = dst;
    if (!to_lo) {
        if (!eth0_present || !eth0.up || !eth0.ip) {
            net_stat_no_route++;
            return -ENETUNREACH;
        }
        bool onlink = (dst & eth0.mask) == (eth0.ip & eth0.mask) || dst == 0xffffffffu;
        if (!onlink) {
            if (!eth0.gw) {
                net_stat_no_route++;
                return -ENETUNREACH;
            }
            next = eth0.gw;
        }
    }
    if (!src)
        return -ENETUNREACH;

    uint8_t pkt[NET_MTU];
    struct ip_hdr *h = (struct ip_hdr *)pkt;
    h->vihl = 0x45;
    h->tos = 0;
    h->total = htons((uint16_t)(IP_HDR_MIN + len));
    h->id = htons(ip_id++);
    h->frag = htons(0x4000); /* don't fragment */
    h->ttl = 64;
    h->proto = proto;
    h->csum = 0;
    h->src = htonl(src);
    h->dst = htonl(dst);
    h->csum = htons(net_csum_fold(net_csum_add(0, h, IP_HDR_MIN)));
    memcpy(pkt + IP_HDR_MIN, payload, len);
    size_t total = IP_HDR_MIN + len;

    if (to_lo) {
        if (net_test_loss_percent) {
            loss_state = loss_state * 1103515245u + 12345u;
            if ((loss_state >> 16) % 100 < (uint32_t)net_test_loss_percent) {
                lo.tx_dropped++;
                return 0;
            }
        }
        if (lo_queued >= LO_QUEUE_MAX) {
            lo.tx_dropped++;
            return -ENOMEM;
        }
        struct lo_pkt *lp = kmalloc(sizeof(*lp) + total);
        if (!lp)
            return -ENOMEM;
        lp->len = total;
        memcpy(lp->data, pkt, total);
        list_add_tail(&lo_queue, &lp->node);
        lo_queued++;
        lo.tx_frames++;
        lo.tx_bytes += total;
        waitq_wake_all(&net_wq);
        return 0;
    }
    return eth_ip_send(next, pkt, total);
}

/* ---------- ICMP ---------- */

struct icmp_hdr {
    uint8_t type, code;
    uint16_t csum;
    uint16_t id, seq;
} __attribute__((packed));

#define PING_SLOTS 4
static struct {
    bool used, done;
    uint16_t id, seq;
    uint32_t dst;
    uint64_t sent;
    uint64_t rtt_ticks;
    struct waitq wq;
} pings[PING_SLOTS];
static uint16_t ping_next_id = 0x4d4c;

static int icmp_send(uint32_t src, uint32_t dst, uint8_t type, uint8_t code, uint32_t rest,
                     const void *body, size_t len)
{
    uint8_t buf[NET_MTU - IP_HDR_MIN];
    if (len > sizeof(buf) - 8)
        len = sizeof(buf) - 8;
    buf[0] = type;
    buf[1] = code;
    buf[2] = buf[3] = 0;
    put_ip(buf + 4, rest);
    memcpy(buf + 8, body, len);
    uint16_t c = net_csum_fold(net_csum_add(0, buf, 8 + len));
    buf[2] = (uint8_t)(c >> 8);
    buf[3] = (uint8_t)c;
    return ip_output(src, dst, PROTO_ICMP, buf, 8 + len);
}

void icmp_unreachable(uint32_t to, uint8_t code, const uint8_t *orig_ip, size_t len)
{
    if (to == 0 || to == 0xffffffffu)
        return;
    icmp_send(0, to, 3, code, 0, orig_ip, len < 28 ? len : 28);
}

static void icmp_input(uint32_t src, uint32_t dst, const uint8_t *data, size_t len)
{
    if (len < sizeof(struct icmp_hdr))
        return;
    if (net_csum_fold(net_csum_add(0, data, len)) != 0) {
        net_stat_bad_csum++;
        return;
    }
    const struct icmp_hdr *h = (const struct icmp_hdr *)data;
    if (h->type == 8 && h->code == 0) { /* echo request */
        if (dst == 0xffffffffu || (eth0_present && eth0.mask && dst == (eth0.ip | ~eth0.mask)))
            return; /* no replies to broadcast pings */
        uint32_t rest = ((uint32_t)ntohs(h->id) << 16) | ntohs(h->seq);
        icmp_send(dst, src, 0, 0, rest, data + 8, len - 8);
    } else if (h->type == 0 && h->code == 0) { /* echo reply */
        for (int i = 0; i < PING_SLOTS; i++) {
            if (pings[i].used && !pings[i].done && pings[i].id == ntohs(h->id) &&
                pings[i].seq == ntohs(h->seq) && pings[i].dst == src) {
                pings[i].done = true;
                pings[i].rtt_ticks = net_ticks() - pings[i].sent;
                waitq_wake_all(&pings[i].wq);
            }
        }
    }
}

int net_ping(uint32_t ip, uint32_t timeout_ms, uint16_t seq)
{
    uint64_t f = spin_lock_irqsave(&net_lock);
    int slot = -1;
    for (int i = 0; i < PING_SLOTS; i++)
        if (!pings[i].used) {
            slot = i;
            break;
        }
    if (slot < 0) {
        spin_unlock_irqrestore(&net_lock, f);
        return -EAGAIN;
    }
    pings[slot].used = true;
    pings[slot].done = false;
    pings[slot].id = ping_next_id++;
    pings[slot].seq = seq;
    pings[slot].dst = ip;
    pings[slot].sent = net_ticks();
    waitq_init(&pings[slot].wq);

    uint8_t payload[32];
    for (int i = 0; i < 32; i++)
        payload[i] = (uint8_t)('a' + i % 23);
    uint32_t rest = ((uint32_t)pings[slot].id << 16) | seq;
    int rc = icmp_send(0, ip, 8, 0, rest, payload, sizeof(payload));
    if (rc == 0) {
        uint64_t ticks = ms_to_ticks(timeout_ms ? timeout_ms : 1000);
        uint64_t deadline = net_ticks() + ticks;
        while (!pings[slot].done) {
            uint64_t now = net_ticks();
            if (now >= deadline)
                break;
            waitq_wait(&pings[slot].wq, &net_lock, deadline - now);
        }
        rc = pings[slot].done ? (int)(pings[slot].rtt_ticks * 1000 / TIMER_HZ) : -ETIMEDOUT;
    }
    pings[slot].used = false;
    spin_unlock_irqrestore(&net_lock, f);
    return rc;
}

/* ---------- IP input ---------- */

static void ip_input(const uint8_t *pkt, size_t len)
{
    if (len < IP_HDR_MIN)
        return;
    const struct ip_hdr *h = (const struct ip_hdr *)pkt;
    size_t ihl = (size_t)(h->vihl & 0x0f) * 4;
    if ((h->vihl >> 4) != 4 || ihl < IP_HDR_MIN || ihl > len) {
        net_stat_bad_len++;
        return;
    }
    size_t total = ntohs(h->total);
    if (total < ihl || total > len) {
        net_stat_bad_len++;
        return;
    }
    if (net_csum_fold(net_csum_add(0, pkt, ihl)) != 0) {
        net_stat_bad_csum++;
        return;
    }
    if (ntohs(h->frag) & 0x3fff) { /* more-fragments flag or a fragment offset */
        net_stat_frag_drop++;
        return;
    }
    uint32_t src = ntohl(h->src), dst = ntohl(h->dst);
    bool mine = net_is_local_ip(dst) || dst == 0xffffffffu ||
                (eth0_present && eth0.mask && dst == (eth0.ip | ~eth0.mask));
    if (!mine)
        return;
    if ((src >> 24) == 127 && !((dst >> 24) == 127 || net_is_local_ip(dst)))
        return; /* spoofed loopback source on a real wire */
    const uint8_t *payload = pkt + ihl;
    size_t plen = total - ihl;
    switch (h->proto) {
    case PROTO_ICMP:
        icmp_input(src, dst, payload, plen);
        break;
    case PROTO_UDP:
        udp_input(src, dst, payload, plen);
        break;
    case PROTO_TCP:
        tcp_input(src, dst, payload, plen);
        break;
    default:
        break;
    }
}

/* ---------- Ethernet input and the worker thread ---------- */

void net_input(struct netif *nif, const void *frame, size_t len)
{
    uint64_t f = spin_lock_irqsave(&net_lock);
    nif->rx_frames++;
    nif->rx_bytes += len;
    if (len < ETH_HDR || len > NET_FRAME_MAX) {
        nif->rx_dropped++;
        net_stat_bad_len++;
        goto out;
    }
    const struct eth_hdr *eh = (const struct eth_hdr *)frame;
    bool to_us = memcmp(eh->dst, nif->mac, 6) == 0 || (eh->dst[0] & 1);
    if (!to_us) {
        nif->rx_dropped++;
        goto out;
    }
    const uint8_t *payload = (const uint8_t *)frame + ETH_HDR;
    switch (ntohs(eh->type)) {
    case ETH_ARP:
        arp_input(nif, payload, len - ETH_HDR);
        break;
    case ETH_IP:
        ip_input(payload, len - ETH_HDR);
        break;
    default:
        nif->rx_dropped++;
        break;
    }
out:
    spin_unlock_irqrestore(&net_lock, f);
}

static void net_thread(void *arg)
{
    (void)arg;
    uint64_t f = spin_lock_irqsave(&net_lock);
    uint64_t last = net_ticks();
    for (;;) {
        while (!list_empty(&lo_queue)) {
            struct lo_pkt *lp = container_of(lo_queue.next, struct lo_pkt, node);
            list_del(&lp->node);
            lo_queued--;
            lo.rx_frames++;
            lo.rx_bytes += lp->len;
            ip_input(lp->data, lp->len);
            kfree(lp);
        }
        uint64_t now = net_ticks();
        if (now != last) {
            last = now;
            tcp_timer();
            if (eth0_present)
                arp_timer();
        }
        waitq_wait(&net_wq, &net_lock, 1);
    }
    spin_unlock_irqrestore(&net_lock, f);
}

void net_init(void)
{
    if (net_ready)
        return;
    list_init(&lo_queue);
    waitq_init(&net_wq);
    for (int i = 0; i < PING_SLOTS; i++)
        waitq_init(&pings[i].wq);
    memset(&lo, 0, sizeof(lo));
    strlcpy(lo.name, "lo", sizeof(lo.name));
    lo.ip = IP4(127, 0, 0, 1);
    lo.mask = IP4(255, 0, 0, 0);
    lo.up = true;
    lo.loopback = true;
    net_ready = true;
    struct thread *t = thread_create("net", net_thread, NULL, 2);
    thread_detach(t);
}

void net_get_info(struct abi_netinfo *out)
{
    memset(out, 0, sizeof(*out));
    uint64_t f = spin_lock_irqsave(&net_lock);
    if (eth0_present) {
        out->present = 1;
        out->up = eth0.up;
        memcpy(out->mac, eth0.mac, 6);
        out->ip = eth0.ip;
        out->mask = eth0.mask;
        out->gw = eth0.gw;
        out->rx_frames = eth0.rx_frames;
        out->tx_frames = eth0.tx_frames;
        out->rx_bytes = eth0.rx_bytes;
        out->tx_bytes = eth0.tx_bytes;
        out->rx_dropped = eth0.rx_dropped;
    }
    out->bad_checksums = net_stat_bad_csum;
    spin_unlock_irqrestore(&net_lock, f);
}

void net_dump(void)
{
    struct netif *n[2] = {&lo, eth0_present ? &eth0 : NULL};
    for (int i = 0; i < 2; i++) {
        if (!n[i])
            continue;
        printk("  %s: %u.%u.%u.%u/%u.%u.%u.%u rx %lu tx %lu\n", n[i]->name, n[i]->ip >> 24,
               (n[i]->ip >> 16) & 255, (n[i]->ip >> 8) & 255, n[i]->ip & 255, n[i]->mask >> 24,
               (n[i]->mask >> 16) & 255, (n[i]->mask >> 8) & 255, n[i]->mask & 255,
               (unsigned long)n[i]->rx_frames, (unsigned long)n[i]->tx_frames);
    }
}
