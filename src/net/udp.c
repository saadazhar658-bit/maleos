#include "kernel/errno.h"
#include "kernel/string.h"
#include "mm/heap.h"
#include "net/net_priv.h"

uint64_t net_stat_udp_dropped;

struct udp_hdr {
    uint16_t sport, dport, len, csum;
} __attribute__((packed));

static uint16_t udp_checksum(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len)
{
    uint8_t ph[12];
    ph[0] = (uint8_t)(src >> 24);
    ph[1] = (uint8_t)(src >> 16);
    ph[2] = (uint8_t)(src >> 8);
    ph[3] = (uint8_t)src;
    ph[4] = (uint8_t)(dst >> 24);
    ph[5] = (uint8_t)(dst >> 16);
    ph[6] = (uint8_t)(dst >> 8);
    ph[7] = (uint8_t)dst;
    ph[8] = 0;
    ph[9] = 17;
    ph[10] = (uint8_t)(len >> 8);
    ph[11] = (uint8_t)len;
    uint32_t sum = net_csum_add(0, ph, sizeof(ph));
    sum = net_csum_add(sum, seg, len);
    return net_csum_fold(sum);
}

int udp_output(struct socket *s, uint32_t dst, uint16_t dport, const void *data, size_t len)
{
    if (len > UDP_MAX_PAYLOAD)
        return -EMSGSIZE;
    uint8_t seg[NET_MTU];
    struct udp_hdr *h = (struct udp_hdr *)seg;
    uint32_t src = s->lip ? s->lip : net_pick_source(dst);
    if (!src)
        return -ENETUNREACH;
    h->sport = htons(s->lport);
    h->dport = htons(dport);
    h->len = htons((uint16_t)(8 + len));
    h->csum = 0;
    memcpy(seg + 8, data, len);
    uint16_t c = udp_checksum(src, dst, seg, 8 + len);
    h->csum = htons(c ? c : 0xffff);
    return ip_output(src, dst, 17, seg, 8 + len);
}

void udp_input(uint32_t src, uint32_t dst, const uint8_t *data, size_t len)
{
    if (len < 8) {
        net_stat_bad_len++;
        return;
    }
    const struct udp_hdr *h = (const struct udp_hdr *)data;
    size_t ulen = ntohs(h->len);
    if (ulen < 8 || ulen > len) {
        net_stat_bad_len++;
        return;
    }
    if (h->csum && udp_checksum(src, dst, data, ulen) != 0) {
        net_stat_bad_csum++;
        return;
    }
    uint16_t sport = ntohs(h->sport), dport = ntohs(h->dport);
    struct socket *s = sock_lookup_udp(dst, dport, src, sport);
    if (!s) {
        if (dst != 0xffffffffu) {
            /* Quote the IP header and 8 payload bytes: rebuild a minimal header for the ICMP body.
             */
            uint8_t orig[28] = {0x45};
            orig[9] = 17;
            orig[12] = (uint8_t)(src >> 24);
            orig[13] = (uint8_t)(src >> 16);
            orig[14] = (uint8_t)(src >> 8);
            orig[15] = (uint8_t)src;
            orig[16] = (uint8_t)(dst >> 24);
            orig[17] = (uint8_t)(dst >> 16);
            orig[18] = (uint8_t)(dst >> 8);
            orig[19] = (uint8_t)dst;
            memcpy(orig + 20, data, 8);
            icmp_unreachable(src, 3, orig, sizeof(orig));
        }
        return;
    }
    size_t plen = ulen - 8;
    if (s->rxq_n >= UDP_RXQ_MAX) {
        net_stat_udp_dropped++;
        return;
    }
    struct dgram *d = kmalloc(sizeof(*d) + plen);
    if (!d) {
        net_stat_udp_dropped++;
        return;
    }
    d->ip = src;
    d->port = sport;
    d->len = (uint16_t)plen;
    memcpy(d->data, data + 8, plen);
    list_add_tail(&s->rxq, &d->node);
    s->rxq_n++;
    sock_wake(s);
}

void udp_socket_free(struct socket *s)
{
    while (!list_empty(&s->rxq)) {
        struct dgram *d = container_of(s->rxq.next, struct dgram, node);
        list_del(&d->node);
        kfree(d);
    }
    s->rxq_n = 0;
}
