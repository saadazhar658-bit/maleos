#include "kernel/errno.h"
#include "kernel/printk.h"
#include "kernel/random.h"
#include "kernel/string.h"
#include "mm/heap.h"
#include "net/net_priv.h"

/*
 * TCP: three-way handshake, in-order delivery, cumulative ACKs, go-back-N retransmission with an
 * adaptive timeout, receiver window, zero-window probes, orderly and abortive close.
 * Not implemented: congestion control, selective ACK, window scaling, timestamps, urgent data,
 * out-of-order reassembly (out-of-order data is dropped and re-requested by a duplicate ACK),
 * and a full 2*MSL TIME_WAIT (it lasts 2 seconds).
 */

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

#define RTO_INIT 50 /* ticks */
#define RTO_MIN 20
#define RTO_MAX 6000
#define MAX_RETRIES 8
#define MAX_SYN_RETRIES 5
#define TIME_WAIT_TICKS (2 * TIMER_HZ)
#define FIN_WAIT2_TICKS (60 * TIMER_HZ)

uint64_t net_stat_tcp_retrans;

static struct tcb *tcbs[TCB_MAX];

struct tcp_hdr {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t off; /* data offset in the high nibble */
    uint8_t flags;
    uint16_t wnd, csum, urg;
} __attribute__((packed));

static inline int32_t sdiff(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}
#define SEQ_LT(a, b) (sdiff(a, b) < 0)
#define SEQ_LE(a, b) (sdiff(a, b) <= 0)
#define SEQ_GT(a, b) (sdiff(a, b) > 0)
#define SEQ_GE(a, b) (sdiff(a, b) >= 0)

static void tcp_output(struct tcb *t);

/* ---------- helpers ---------- */

static uint16_t tcp_checksum(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len)
{
    uint8_t ph[12] = {(uint8_t)(src >> 24),
                      (uint8_t)(src >> 16),
                      (uint8_t)(src >> 8),
                      (uint8_t)src,
                      (uint8_t)(dst >> 24),
                      (uint8_t)(dst >> 16),
                      (uint8_t)(dst >> 8),
                      (uint8_t)dst,
                      0,
                      6,
                      (uint8_t)(len >> 8),
                      (uint8_t)len};
    uint32_t sum = net_csum_add(0, ph, sizeof(ph));
    sum = net_csum_add(sum, seg, len);
    return net_csum_fold(sum);
}

static void send_segment(uint32_t lip, uint32_t rip, uint16_t lport, uint16_t rport, uint32_t seq,
                         uint32_t ack, uint8_t flags, uint32_t wnd, const void *data, size_t len,
                         uint16_t mss_opt)
{
    uint8_t seg[NET_MTU];
    struct tcp_hdr *h = (struct tcp_hdr *)seg;
    size_t hlen = sizeof(*h) + (mss_opt ? 4 : 0);
    if (hlen + len > sizeof(seg))
        return;
    h->sport = htons(lport);
    h->dport = htons(rport);
    h->seq = htonl(seq);
    h->ack = htonl(ack);
    h->off = (uint8_t)((hlen / 4) << 4);
    h->flags = flags;
    h->wnd = htons((uint16_t)(wnd > 0xffff ? 0xffff : wnd));
    h->csum = 0;
    h->urg = 0;
    if (mss_opt) {
        seg[20] = 2;
        seg[21] = 4;
        seg[22] = (uint8_t)(mss_opt >> 8);
        seg[23] = (uint8_t)mss_opt;
    }
    if (len)
        memcpy(seg + hlen, data, len);
    h->csum = htons(tcp_checksum(lip, rip, seg, hlen + len));
    ip_output(lip, rip, 6, seg, hlen + len);
}

static void send_rst_for(uint32_t src, uint32_t dst, const struct tcp_hdr *h, size_t seglen)
{
    uint8_t fl = h->flags;
    if (fl & F_RST)
        return;
    if (fl & F_ACK) {
        send_segment(dst, src, ntohs(h->dport), ntohs(h->sport), ntohl(h->ack), 0, F_RST, 0, NULL,
                     0, 0);
    } else {
        uint32_t ack =
            ntohl(h->seq) + (uint32_t)seglen + ((fl & F_SYN) ? 1 : 0) + ((fl & F_FIN) ? 1 : 0);
        send_segment(dst, src, ntohs(h->dport), ntohs(h->sport), 0, ack, F_RST | F_ACK, 0, NULL, 0,
                     0);
    }
}

static uint32_t rcv_window(const struct tcb *t)
{
    return TCP_BUF - t->rb_len;
}

static void send_ctl(struct tcb *t, uint8_t flags)
{
    t->adv_wnd = rcv_window(t);
    send_segment(t->lip, t->rip, t->lport, t->rport, t->snd_nxt, t->rcv_nxt, flags, t->adv_wnd,
                 NULL, 0, 0);
}

static void wake(struct tcb *t)
{
    if (t->sock)
        sock_wake(t->sock);
    if (t->parent)
        sock_wake(t->parent->sock);
}

static void arm_rto(struct tcb *t)
{
    t->rto_deadline = net_ticks() + t->rto;
}

/* ---------- table ---------- */

static struct tcb *tcb_new(void)
{
    int slot = -1;
    for (int i = 0; i < TCB_MAX; i++)
        if (!tcbs[i]) {
            slot = i;
            break;
        }
    if (slot < 0)
        return NULL;
    struct tcb *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    t->sbuf = kmalloc(TCP_BUF);
    t->rbuf = kmalloc(TCP_BUF);
    if (!t->sbuf || !t->rbuf) {
        kfree(t->sbuf);
        kfree(t->rbuf);
        kfree(t);
        return NULL;
    }
    t->mss = 536; /* until the peer's option says otherwise */
    t->rto = RTO_INIT;
    t->snd_wnd = TCP_BUF;
    tcbs[slot] = t;
    return t;
}

static void tcb_free(struct tcb *t)
{
    for (int i = 0; i < TCB_MAX; i++)
        if (tcbs[i] == t)
            tcbs[i] = NULL;
    if (t->sock && t->sock->tcb == t)
        t->sock->tcb = NULL;
    if (t->parent) {
        struct tcb *p = t->parent;
        for (int i = 0; i < p->aq_n; i++)
            if (p->acceptq[i] == t) {
                memmove(&p->acceptq[i], &p->acceptq[i + 1],
                        (size_t)(p->aq_n - i - 1) * sizeof(p->acceptq[0]));
                p->aq_n--;
                break;
            }
        if (p->pending_children > 0)
            p->pending_children--;
    }
    kfree(t->sbuf);
    kfree(t->rbuf);
    kfree(t);
}

/* Terminate a connection after an error or reset. A connection nobody holds (an orphan, or a
 * child still waiting in a listener's queue) is freed; one with a socket stays until close(). */
static void tcb_abort(struct tcb *t, int err)
{
    struct socket *owner = t->sock;
    struct socket *lsock = t->parent ? t->parent->sock : NULL;
    t->err = err;
    t->state = TCP_CLOSED;
    t->rto_deadline = 0;
    t->tw_deadline = 0;
    if (owner) {
        owner->err = err;
        sock_wake(owner);
    } else {
        tcb_free(t);
    }
    if (lsock)
        sock_wake(lsock);
}

static struct tcb *find_conn(uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport)
{
    for (int i = 0; i < TCB_MAX; i++) {
        struct tcb *t = tcbs[i];
        if (t && t->state != TCP_LISTEN && t->state != TCP_CLOSED && t->lport == lport &&
            t->rport == rport && t->rip == rip && t->lip == lip)
            return t;
    }
    return NULL;
}

static struct tcb *find_listener(uint32_t lip, uint16_t lport)
{
    for (int i = 0; i < TCB_MAX; i++) {
        struct tcb *t = tcbs[i];
        if (t && t->state == TCP_LISTEN && t->lport == lport && (t->lip == 0 || t->lip == lip))
            return t;
    }
    return NULL;
}

bool tcp_port_in_use(uint16_t port)
{
    for (int i = 0; i < TCB_MAX; i++)
        if (tcbs[i] && tcbs[i]->lport == port && tcbs[i]->state != TCP_TIME_WAIT)
            return true;
    return false;
}

int tcp_connection_count(void)
{
    int n = 0;
    for (int i = 0; i < TCB_MAX; i++)
        n += tcbs[i] != NULL;
    return n;
}

/* ---------- options ---------- */

static uint16_t parse_mss(const uint8_t *opts, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint8_t kind = opts[i];
        if (kind == 0)
            break;
        if (kind == 1) {
            i++;
            continue;
        }
        if (i + 1 >= len || opts[i + 1] < 2 || i + opts[i + 1] > len)
            break;
        if (kind == 2 && opts[i + 1] == 4)
            return (uint16_t)((opts[i + 2] << 8) | opts[i + 3]);
        i += opts[i + 1];
    }
    return 0;
}

static void set_mss(struct tcb *t, uint16_t peer)
{
    uint16_t m = peer ? peer : 536;
    if (m > TCP_MSS)
        m = TCP_MSS;
    if (m < 64)
        m = 64;
    t->mss = m;
}

/* ---------- output ---------- */

static void ring_out(const uint8_t *ring, uint32_t off, uint32_t n, uint8_t *dst)
{
    uint32_t first = TCP_BUF - off;
    if (first >= n) {
        memcpy(dst, ring + off, n);
    } else {
        memcpy(dst, ring + off, first);
        memcpy(dst + first, ring, n - first);
    }
}

static void send_syn(struct tcb *t, bool with_ack)
{
    t->adv_wnd = rcv_window(t);
    send_segment(t->lip, t->rip, t->lport, t->rport, t->iss, t->rcv_nxt,
                 (uint8_t)(F_SYN | (with_ack ? F_ACK : 0)), t->adv_wnd, NULL, 0, TCP_MSS);
}

static void tcp_output(struct tcb *t)
{
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT && t->state != TCP_FIN_WAIT1 &&
        t->state != TCP_CLOSING && t->state != TCP_LAST_ACK)
        return;

    for (;;) {
        uint32_t data_end = t->sb_seq + t->sb_len;
        uint32_t inflight = t->snd_nxt - t->snd_una;
        uint32_t wnd = t->snd_wnd < TCP_BUF ? t->snd_wnd : TCP_BUF;
        uint32_t room = wnd > inflight ? wnd - inflight : 0;

        if (SEQ_LT(t->snd_nxt, data_end)) {
            uint32_t avail = data_end - t->snd_nxt;
            uint32_t n = avail < room ? avail : room;
            if (n > t->mss)
                n = t->mss;
            if (n == 0) {
                if (t->snd_wnd == 0 && !t->rto_deadline)
                    arm_rto(t); /* persist timer */
                break;
            }
            uint8_t buf[TCP_MSS];
            uint32_t off = (t->sb_off + (t->snd_nxt - t->sb_seq)) % TCP_BUF;
            ring_out(t->sbuf, off, n, buf);
            bool last = n == avail;
            t->adv_wnd = rcv_window(t);
            send_segment(t->lip, t->rip, t->lport, t->rport, t->snd_nxt, t->rcv_nxt,
                         (uint8_t)(F_ACK | (last ? F_PSH : 0)), t->adv_wnd, buf, n, 0);
            if (!t->rtt_start) {
                t->rtt_start = net_ticks();
                t->rtt_seq = t->snd_nxt + n;
            }
            t->snd_nxt += n;
            if (!t->rto_deadline)
                arm_rto(t);
            continue;
        }
        if (t->fin_pending && !t->fin_sent && t->snd_nxt == data_end) {
            send_ctl(t, F_FIN | F_ACK);
            t->snd_nxt++;
            t->fin_sent = true;
            if (!t->rto_deadline)
                arm_rto(t);
        }
        break;
    }
}

/* ---------- ACK processing ---------- */

static void rtt_sample(struct tcb *t, uint32_t sample)
{
    if (t->srtt == 0) {
        t->srtt = sample ? sample : 1;
        t->rttvar = t->srtt / 2;
    } else {
        uint32_t err = sample > t->srtt ? sample - t->srtt : t->srtt - sample;
        t->rttvar = (3 * t->rttvar + err) / 4;
        t->srtt = (7 * t->srtt + sample) / 8;
    }
    uint32_t v = 4 * t->rttvar;
    t->rto = t->srtt + (v ? v : 1);
    if (t->rto < RTO_MIN)
        t->rto = RTO_MIN;
    if (t->rto > RTO_MAX)
        t->rto = RTO_MAX;
}

static void retransmit(struct tcb *t)
{
    net_stat_tcp_retrans++;
    switch (t->state) {
    case TCP_SYN_SENT:
        send_syn(t, false);
        return;
    case TCP_SYN_RCVD:
        send_syn(t, true);
        return;
    default:
        break;
    }
    /* Go back to the oldest unacknowledged byte and resend from there. */
    t->snd_nxt = t->snd_una;
    t->fin_sent = false;
    t->rtt_start = 0;
    tcp_output(t);
}

/* Returns false if the segment must not be processed further (connection gone). */
static bool process_ack(struct tcb *t, uint32_t ack, uint32_t wnd, bool had_data)
{
    if (SEQ_GT(ack, t->snd_nxt))
        return true; /* acknowledges something we never sent: ignore (and ACK back by caller) */

    if (SEQ_GT(ack, t->snd_una)) {
        uint32_t acked = ack - t->snd_una;
        uint32_t data_acked = acked;
        /* The SYN and FIN each take one sequence number but no buffer space. */
        if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RCVD)
            data_acked = 0;
        if (t->fin_sent && ack == t->snd_nxt && data_acked > 0)
            data_acked--;
        if (data_acked > t->sb_len)
            data_acked = t->sb_len;
        t->sb_off = (t->sb_off + data_acked) % TCP_BUF;
        t->sb_len -= data_acked;
        t->sb_seq += data_acked;
        t->snd_una = ack;
        t->dupacks = 0;
        t->retries = 0;
        if (t->rtt_start && SEQ_GE(ack, t->rtt_seq)) {
            rtt_sample(t, (uint32_t)(net_ticks() - t->rtt_start));
            t->rtt_start = 0;
        }
        t->rto_deadline = 0;
        if (SEQ_LT(t->snd_una, t->snd_nxt))
            arm_rto(t);
        if (t->sock)
            sock_wake(t->sock);
    } else if (ack == t->snd_una && !had_data && SEQ_LT(t->snd_una, t->snd_nxt) &&
               wnd == t->snd_wnd) {
        if (++t->dupacks == 3) { /* fast retransmit of the oldest segment */
            t->rtt_start = 0;
            retransmit(t);
        }
    }
    t->snd_wnd = wnd;
    if (t->snd_wnd && t->rto_deadline && t->snd_una == t->snd_nxt)
        t->rto_deadline = 0; /* persist timer no longer needed */

    bool fin_acked = t->fin_sent && t->snd_una == t->snd_nxt;
    switch (t->state) {
    case TCP_SYN_RCVD:
        if (SEQ_GE(ack, t->iss + 1)) {
            t->state = TCP_ESTABLISHED;
            if (t->parent) {
                struct tcb *p = t->parent;
                if (p->aq_n < ACCEPTQ_MAX)
                    p->acceptq[p->aq_n++] = t;
                sock_wake(p->sock);
            }
            wake(t);
        }
        break;
    case TCP_FIN_WAIT1:
        if (fin_acked) {
            t->state = TCP_FIN_WAIT2;
            if (!t->sock)
                t->tw_deadline = net_ticks() + FIN_WAIT2_TICKS;
        }
        break;
    case TCP_CLOSING:
        if (fin_acked) {
            t->state = TCP_TIME_WAIT;
            t->tw_deadline = net_ticks() + TIME_WAIT_TICKS;
            t->rto_deadline = 0;
        }
        break;
    case TCP_LAST_ACK:
        if (fin_acked) {
            tcb_abort(t, 0);
            return false;
        }
        break;
    default:
        break;
    }
    tcp_output(t);
    return true;
}

/* ---------- input ---------- */

static void accept_syn(struct tcb *l, uint32_t src, uint32_t dst, const struct tcp_hdr *h,
                       uint16_t peer_mss)
{
    if (l->pending_children >= l->backlog)
        return; /* full: drop and let the client retry */
    struct tcb *c = tcb_new();
    if (!c)
        return;
    c->parent = l;
    l->pending_children++;
    c->state = TCP_SYN_RCVD;
    c->lip = dst;
    c->rip = src;
    c->lport = ntohs(h->dport);
    c->rport = ntohs(h->sport);
    c->irs = ntohl(h->seq);
    c->rcv_nxt = c->irs + 1;
    c->iss = (uint32_t)random_u64();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    c->sb_seq = c->iss + 1;
    c->snd_wnd = ntohs(h->wnd);
    set_mss(c, peer_mss);
    send_syn(c, true);
    arm_rto(c);
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *data, size_t len)
{
    if (len < sizeof(struct tcp_hdr)) {
        net_stat_bad_len++;
        return;
    }
    const struct tcp_hdr *h = (const struct tcp_hdr *)data;
    size_t hlen = (size_t)(h->off >> 4) * 4;
    if (hlen < sizeof(*h) || hlen > len) {
        net_stat_bad_len++;
        return;
    }
    if (tcp_checksum(src, dst, data, len) != 0) {
        net_stat_bad_csum++;
        return;
    }
    const uint8_t *payload = data + hlen;
    size_t plen = len - hlen;
    uint8_t fl = h->flags;
    uint32_t seq = ntohl(h->seq), ack = ntohl(h->ack), wnd = ntohs(h->wnd);
    uint16_t sport = ntohs(h->sport), dport = ntohs(h->dport);

    struct tcb *t = find_conn(dst, dport, src, sport);

    if (t && t->state == TCP_TIME_WAIT && (fl & F_SYN) && !(fl & F_ACK)) {
        tcb_free(t); /* a new connection reusing the 4-tuple */
        t = NULL;
    }

    if (!t) {
        struct tcb *l = find_listener(dst, dport);
        if (l && (fl & F_SYN) && !(fl & (F_ACK | F_RST)))
            accept_syn(l, src, dst, h, parse_mss(data + sizeof(*h), hlen - sizeof(*h)));
        else if (!(fl & F_RST))
            send_rst_for(src, dst, h, plen);
        return;
    }

    /* --- SYN_SENT --- */
    if (t->state == TCP_SYN_SENT) {
        if (fl & F_ACK) {
            if (ack != t->iss + 1) {
                if (!(fl & F_RST))
                    send_segment(t->lip, t->rip, t->lport, t->rport, ack, 0, F_RST, 0, NULL, 0, 0);
                return;
            }
        }
        if (fl & F_RST) {
            if (fl & F_ACK)
                tcb_abort(t, ECONNREFUSED);
            return;
        }
        if (fl & F_SYN) {
            t->irs = seq;
            t->rcv_nxt = seq + 1;
            set_mss(t, parse_mss(data + sizeof(*h), hlen - sizeof(*h)));
            t->snd_wnd = wnd;
            if (fl & F_ACK) {
                t->snd_una = ack;
                t->sb_seq = t->iss + 1;
                t->state = TCP_ESTABLISHED;
                t->rto_deadline = 0;
                t->retries = 0;
                send_ctl(t, F_ACK);
                wake(t);
                tcp_output(t);
            } else { /* simultaneous open */
                t->state = TCP_SYN_RCVD;
                send_syn(t, true);
            }
        }
        return;
    }

    /* --- everything else: sequence check --- */
    if (fl & F_RST) {
        uint32_t win = rcv_window(t);
        if (SEQ_GE(seq, t->rcv_nxt) && SEQ_LT(seq, t->rcv_nxt + (win ? win : 1))) {
            tcb_abort(t, t->state == TCP_SYN_RCVD ? ECONNREFUSED : ECONNRESET);
        }
        return;
    }
    if (fl & F_SYN) { /* an unexpected SYN: answer with our current state */
        if (t->state == TCP_SYN_RCVD && seq == t->irs) {
            if ((fl & F_ACK) && ack == t->iss + 1) {
                /* both ends opened at once (also a socket connected to itself): this SYN|ACK
                 * completes the handshake */
                t->snd_wnd = wnd;
                process_ack(t, ack, wnd, false);
            } else {
                send_syn(t, true); /* the client missed our SYN|ACK */
            }
        } else {
            send_ctl(t, F_ACK);
        }
        return;
    }
    if (!(fl & F_ACK))
        return;

    bool fin = fl & F_FIN;
    uint32_t total = (uint32_t)plen + (fin ? 1u : 0u);
    uint32_t eff_seq = seq;

    if (SEQ_LT(seq, t->rcv_nxt) && total > 0) {
        uint32_t skip = t->rcv_nxt - seq;
        if (skip >= total) { /* everything in it was received before: just remind the peer */
            if (process_ack(t, ack, wnd, false) && t->state != TCP_CLOSED)
                send_ctl(t, F_ACK);
            return;
        }
        payload += skip; /* skip < total, and the FIN (if any) is the last unit, so skip <= plen */
        plen -= skip;
        eff_seq = t->rcv_nxt;
    }

    if (SEQ_GT(eff_seq, t->rcv_nxt)) { /* a gap: drop the data, keep the ACK, ask again */
        if (!process_ack(t, ack, wnd, plen > 0))
            return;
        if (total)
            send_ctl(t, F_ACK);
        return;
    }

    if (!process_ack(t, ack, wnd, plen > 0))
        return;
    if (t->state == TCP_CLOSED)
        return;

    bool can_receive =
        t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT1 || t->state == TCP_FIN_WAIT2;
    bool need_ack = false;

    if (plen && can_receive) {
        if (t->sock == NULL && t->parent == NULL) {
            /* The user closed the socket: the data has nowhere to go. Reset the peer. */
            send_segment(t->lip, t->rip, t->lport, t->rport, t->snd_nxt, 0, F_RST, 0, NULL, 0, 0);
            tcb_abort(t, ECONNRESET);
            return;
        }
        uint32_t space = rcv_window(t);
        uint32_t n = plen < space ? (uint32_t)plen : space;
        uint32_t wr = (t->rb_off + t->rb_len) % TCP_BUF;
        uint32_t first = TCP_BUF - wr < n ? TCP_BUF - wr : n;
        memcpy(t->rbuf + wr, payload, first);
        memcpy(t->rbuf, payload + first, n - first);
        t->rb_len += n;
        t->rcv_nxt += n;
        need_ack = true;
        if (n < plen)
            fin = false; /* the FIN follows data we could not accept yet */
        wake(t);
    } else if (plen) {
        need_ack = true; /* data in a state that cannot take it */
        fin = false;
    }

    if (fin && !t->fin_received && (can_receive || t->state == TCP_SYN_RCVD)) {
        t->fin_received = true;
        t->rcv_nxt++;
        need_ack = true;
        switch (t->state) {
        case TCP_ESTABLISHED:
            t->state = TCP_CLOSE_WAIT;
            break;
        case TCP_FIN_WAIT1:
            if (t->fin_sent && t->snd_una == t->snd_nxt) {
                t->state = TCP_TIME_WAIT;
                t->tw_deadline = net_ticks() + TIME_WAIT_TICKS;
                t->rto_deadline = 0;
            } else {
                t->state = TCP_CLOSING;
            }
            break;
        case TCP_FIN_WAIT2:
            t->state = TCP_TIME_WAIT;
            t->tw_deadline = net_ticks() + TIME_WAIT_TICKS;
            t->rto_deadline = 0;
            break;
        default:
            break;
        }
        wake(t);
    } else if (fin && t->state == TCP_TIME_WAIT) {
        need_ack = true;
        t->tw_deadline = net_ticks() + TIME_WAIT_TICKS;
    }
    if (need_ack)
        send_ctl(t, F_ACK);
}

/* ---------- timers ---------- */

void tcp_timer(void)
{
    uint64_t now = net_ticks();
    for (int i = 0; i < TCB_MAX; i++) {
        struct tcb *t = tcbs[i];
        if (!t)
            continue;
        if (t->tw_deadline && now >= t->tw_deadline) {
            tcb_abort(t, 0); /* TIME_WAIT or abandoned FIN_WAIT2 is over */
            continue;
        }
        if (!t->rto_deadline || now < t->rto_deadline)
            continue;

        bool syn = t->state == TCP_SYN_SENT || t->state == TCP_SYN_RCVD;
        bool probe = t->snd_wnd == 0 && t->snd_una == t->snd_nxt && t->sb_len > 0;
        if (++t->retries > (syn ? MAX_SYN_RETRIES : MAX_RETRIES)) {
            tcb_abort(t, ETIMEDOUT);
            continue;
        }
        t->rto = t->rto * 2 > RTO_MAX ? RTO_MAX : t->rto * 2;
        if (probe) {
            /* Zero window: poke the peer with one byte beyond the window to learn when it opens. */
            uint8_t b;
            ring_out(t->sbuf, t->sb_off, 1, &b);
            send_segment(t->lip, t->rip, t->lport, t->rport, t->snd_nxt, t->rcv_nxt, F_ACK,
                         rcv_window(t), &b, 1, 0);
            arm_rto(t);
            continue;
        }
        retransmit(t);
        arm_rto(t);
    }
}

/* ---------- user operations ---------- */

struct tcb *tcp_connect(struct socket *s)
{
    struct tcb *t = tcb_new();
    if (!t)
        return NULL;
    t->sock = s;
    t->state = TCP_SYN_SENT;
    t->lip = s->lip;
    t->rip = s->rip;
    t->lport = s->lport;
    t->rport = s->rport;
    t->iss = (uint32_t)random_u64();
    t->snd_una = t->iss;
    t->snd_nxt = t->iss + 1;
    t->sb_seq = t->iss + 1;
    s->tcb = t;
    send_syn(t, false);
    arm_rto(t);
    return t;
}

int tcp_listen(struct socket *s, int backlog)
{
    struct tcb *t = tcb_new();
    if (!t)
        return -ENOMEM;
    t->sock = s;
    t->state = TCP_LISTEN;
    t->lip = s->lip;
    t->lport = s->lport;
    t->backlog = backlog < 1 ? 1 : (backlog > ACCEPTQ_MAX ? ACCEPTQ_MAX : backlog);
    s->tcb = t;
    return 0;
}

int64_t tcp_write(struct socket *s, const uint8_t *data, size_t len, bool nonblock)
{
    size_t done = 0;
    while (done < len) {
        struct tcb *t = s->tcb;
        if (!t)
            return done ? (int64_t)done : -ENOTCONN;
        if (t->state == TCP_CLOSED)
            return done ? (int64_t)done : -(t->err ? t->err : EPIPE);
        if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT)
            return done ? (int64_t)done
                        : (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RCVD ? -ENOTCONN
                                                                                : -EPIPE);
        if (t->fin_pending)
            return done ? (int64_t)done : -EPIPE;
        uint32_t space = TCP_BUF - t->sb_len;
        if (space == 0) {
            if (nonblock)
                return done ? (int64_t)done : -EAGAIN;
            sock_block(s, 0);
            continue;
        }
        size_t n = len - done < space ? len - done : space;
        uint32_t wr = (t->sb_off + t->sb_len) % TCP_BUF;
        uint32_t first = TCP_BUF - wr < n ? TCP_BUF - wr : (uint32_t)n;
        memcpy(t->sbuf + wr, data + done, first);
        memcpy(t->sbuf, data + done + first, n - first);
        t->sb_len += (uint32_t)n;
        done += n;
        tcp_output(t);
    }
    return (int64_t)done;
}

int64_t tcp_read(struct socket *s, uint8_t *buf, size_t len, bool nonblock, uint64_t deadline)
{
    for (;;) {
        struct tcb *t = s->tcb;
        if (!t) {
            int e = s->err;
            s->err = 0;
            return e ? -e : 0;
        }
        if (t->rb_len > 0) {
            uint32_t n = t->rb_len < len ? t->rb_len : (uint32_t)len;
            ring_out(t->rbuf, t->rb_off, n, buf);
            t->rb_off = (t->rb_off + n) % TCP_BUF;
            t->rb_len -= n;
            /* Tell the sender when the window has opened noticeably. */
            if (rcv_window(t) - t->adv_wnd >= (TCP_BUF / 2 < t->mss ? TCP_BUF / 2 : t->mss) &&
                (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT1 ||
                 t->state == TCP_FIN_WAIT2 || t->state == TCP_CLOSE_WAIT))
                send_ctl(t, F_ACK);
            return n;
        }
        if (t->state == TCP_CLOSED) {
            int e = t->err ? t->err : s->err;
            s->err = 0;
            t->err = 0;
            return e ? -e : 0;
        }
        if (t->fin_received)
            return 0;
        if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RCVD || t->state == TCP_LISTEN)
            return -ENOTCONN;
        if (nonblock)
            return -EAGAIN;
        int rc = sock_block(s, deadline);
        if (rc < 0)
            return rc;
    }
}

void tcp_shutdown_wr(struct socket *s)
{
    struct tcb *t = s->tcb;
    if (!t || t->fin_pending)
        return;
    if (t->state == TCP_ESTABLISHED) {
        t->fin_pending = true;
        t->state = TCP_FIN_WAIT1;
        tcp_output(t);
    } else if (t->state == TCP_CLOSE_WAIT) {
        t->fin_pending = true;
        t->state = TCP_LAST_ACK;
        tcp_output(t);
    }
}

struct tcb *tcp_accept_take(struct tcb *l)
{
    if (l->aq_n == 0)
        return NULL;
    struct tcb *c = l->acceptq[0];
    memmove(&l->acceptq[0], &l->acceptq[1], (size_t)(l->aq_n - 1) * sizeof(l->acceptq[0]));
    l->aq_n--;
    if (l->pending_children > 0)
        l->pending_children--;
    c->parent = NULL;
    return c;
}

void tcp_free_child(struct tcb *c)
{
    c->sock = NULL;
    send_segment(c->lip, c->rip, c->lport, c->rport, c->snd_nxt, 0, F_RST, 0, NULL, 0, 0);
    tcb_abort(c, ECONNRESET);
}

void tcp_close(struct socket *s)
{
    struct tcb *t = s->tcb;
    if (!t)
        return;
    s->tcb = NULL;
    if (t->state == TCP_LISTEN) {
        for (int i = 0; i < TCB_MAX; i++) {
            struct tcb *c = tcbs[i];
            if (c && c->parent == t) {
                c->parent = NULL; /* so tcb_abort frees it */
                c->sock = NULL;
                send_segment(c->lip, c->rip, c->lport, c->rport, c->snd_nxt, 0, F_RST, 0, NULL, 0,
                             0);
                tcb_abort(c, ECONNRESET);
            }
        }
        t->sock = NULL;
        tcb_free(t);
        return;
    }
    t->sock = NULL;
    switch (t->state) {
    case TCP_CLOSED:
    case TCP_SYN_SENT:
        tcb_free(t);
        break;
    case TCP_SYN_RCVD:
        send_segment(t->lip, t->rip, t->lport, t->rport, t->snd_nxt, 0, F_RST, 0, NULL, 0, 0);
        tcb_free(t);
        break;
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        if (t->rb_len > 0) { /* unread data: reset instead of closing politely */
            send_segment(t->lip, t->rip, t->lport, t->rport, t->snd_nxt, 0, F_RST, 0, NULL, 0, 0);
            tcb_free(t);
            break;
        }
        t->fin_pending = true;
        t->state = t->state == TCP_ESTABLISHED ? TCP_FIN_WAIT1 : TCP_LAST_ACK;
        tcp_output(t);
        break;
    case TCP_FIN_WAIT1:
    case TCP_FIN_WAIT2:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
    case TCP_TIME_WAIT:
        if (t->state == TCP_FIN_WAIT2)
            t->tw_deadline = net_ticks() + FIN_WAIT2_TICKS;
        break; /* already closing: the timers finish the job */
    default:
        break;
    }
}

void tcp_dump(void)
{
    static const char *const names[] = {"CLOSED",  "LISTEN",    "SYN_SENT",  "SYN_RCVD",
                                        "ESTAB",   "FIN_WAIT1", "FIN_WAIT2", "CLOSE_WAIT",
                                        "CLOSING", "LAST_ACK",  "TIME_WAIT"};
    for (int i = 0; i < TCB_MAX; i++) {
        struct tcb *t = tcbs[i];
        if (t)
            printk("  tcp %u -> %u.%u.%u.%u:%u %s\n", t->lport, t->rip >> 24, (t->rip >> 16) & 255,
                   (t->rip >> 8) & 255, t->rip & 255, t->rport, names[t->state]);
    }
}
