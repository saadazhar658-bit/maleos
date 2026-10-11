#include "kernel/errno.h"
#include "kernel/random.h"
#include "kernel/string.h"
#include "mm/heap.h"
#include "net/net_priv.h"

struct socket socks[SOCK_MAX];

void sock_wake(struct socket *s)
{
    if (s)
        waitq_wake_all(&s->wq);
}

/* Sleep until woken or until `deadline` (absolute tick, 0 = no limit). Called with net_lock held
 * and interrupts off. Returns -EAGAIN once the deadline has passed. */
int sock_block(struct socket *s, uint64_t deadline)
{
    uint64_t ticks = 0;
    if (deadline) {
        uint64_t now = net_ticks();
        if (now >= deadline)
            return -EAGAIN;
        ticks = deadline - now;
    }
    waitq_wait(&s->wq, &net_lock, ticks);
    return 0;
}

static uint64_t deadline_for(const struct socket *s)
{
    return s->rcvto_ticks ? net_ticks() + s->rcvto_ticks : 0;
}

int sock_valid(int s)
{
    return s >= 0 && s < SOCK_MAX && socks[s].used;
}

int sock_count(void)
{
    int n = 0;
    for (int i = 0; i < SOCK_MAX; i++)
        n += socks[i].used;
    return n;
}

#define LOCK() uint64_t f_ = spin_lock_irqsave(&net_lock)
#define UNLOCK() spin_unlock_irqrestore(&net_lock, f_)
#define RET(v)                                                                                     \
    do {                                                                                           \
        int64_t r_ = (v);                                                                          \
        UNLOCK();                                                                                  \
        return r_;                                                                                 \
    } while (0)

bool sock_port_in_use(int type, uint16_t port, uint32_t ip, const struct socket *except)
{
    for (int i = 0; i < SOCK_MAX; i++) {
        const struct socket *o = &socks[i];
        if (!o->used || o == except || o->type != type || !o->bound || o->lport != port)
            continue;
        if (o->lip == 0 || ip == 0 || o->lip == ip)
            return true;
    }
    return type == SOCK_STREAM && tcp_port_in_use(port);
}

uint16_t sock_ephemeral_port(int type)
{
    for (int tries = 0; tries < 20000; tries++) {
        uint16_t p = (uint16_t)(49152 + random_u64() % (65536 - 49152));
        if (!sock_port_in_use(type, p, 0, NULL))
            return p;
    }
    return 0;
}

struct socket *sock_lookup_udp(uint32_t dst, uint16_t dport, uint32_t src, uint16_t sport)
{
    struct socket *best = NULL;
    for (int i = 0; i < SOCK_MAX; i++) {
        struct socket *s = &socks[i];
        if (!s->used || s->type != SOCK_DGRAM || !s->bound || s->lport != dport)
            continue;
        if (s->lip && s->lip != dst && dst != 0xffffffffu)
            continue;
        if (s->connected && (s->rip != src || s->rport != sport))
            continue;
        if (!best || s->connected)
            best = s;
    }
    return best;
}

static int alloc_socket(int type)
{
    for (int i = 0; i < SOCK_MAX; i++) {
        if (!socks[i].used) {
            memset(&socks[i], 0, sizeof(socks[i]));
            socks[i].used = true;
            socks[i].type = type;
            waitq_init(&socks[i].wq);
            list_init(&socks[i].rxq);
            return i;
        }
    }
    return -ENOMEM;
}

int sock_create(int type)
{
    if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return -EPROTONOSUPPORT;
    LOCK();
    int s = alloc_socket(type);
    RET(s);
}

int sock_close(int sd)
{
    LOCK();
    if (!sock_valid(sd))
        RET(-EBADF);
    struct socket *s = &socks[sd];
    if (s->type == SOCK_STREAM)
        tcp_close(s);
    else
        udp_socket_free(s);
    sock_wake(s);
    s->used = false;
    RET(0);
}

int sock_bind(int sd, uint32_t ip, uint16_t port)
{
    LOCK();
    if (!sock_valid(sd))
        RET(-EBADF);
    struct socket *s = &socks[sd];
    if (s->bound)
        RET(-EINVAL);
    if (ip && !net_is_local_ip(ip))
        RET(-EINVAL);
    if (port == 0) {
        port = sock_ephemeral_port(s->type);
        if (!port)
            RET(-EADDRINUSE);
    } else if (sock_port_in_use(s->type, port, ip, s)) {
        RET(-EADDRINUSE);
    }
    s->lip = ip;
    s->lport = port;
    s->bound = true;
    RET(0);
}

int sock_listen(int sd, int backlog)
{
    LOCK();
    if (!sock_valid(sd))
        RET(-EBADF);
    struct socket *s = &socks[sd];
    if (s->type != SOCK_STREAM)
        RET(-EOPNOTSUPP);
    if (s->tcb)
        RET(-EINVAL);
    if (!s->bound) {
        s->lport = sock_ephemeral_port(SOCK_STREAM);
        s->bound = true;
    }
    RET(tcp_listen(s, backlog));
}

int sock_accept(int sd, uint32_t *ip, uint16_t *port)
{
    LOCK();
    if (!sock_valid(sd))
        RET(-EBADF);
    struct socket *l = &socks[sd];
    if (l->type != SOCK_STREAM)
        RET(-EOPNOTSUPP);
    if (!l->tcb || l->tcb->state != TCP_LISTEN)
        RET(-EINVAL);
    uint64_t deadline = deadline_for(l);
    for (;;) {
        struct tcb *c = l->tcb ? tcp_accept_take(l->tcb) : NULL;
        if (c) {
            int ns = alloc_socket(SOCK_STREAM);
            if (ns < 0) {
                tcp_free_child(c);
                RET(ns);
            }
            struct socket *n = &socks[ns];
            n->bound = n->connected = true;
            n->lip = c->lip;
            n->lport = c->lport;
            n->rip = c->rip;
            n->rport = c->rport;
            n->tcb = c;
            c->sock = n;
            if (ip)
                *ip = c->rip;
            if (port)
                *port = c->rport;
            RET(ns);
        }
        if (!l->tcb)
            RET(-EINVAL);
        if (l->nonblock)
            RET(-EAGAIN);
        if (sock_block(l, deadline) < 0)
            RET(-EAGAIN);
    }
}

int sock_connect(int sd, uint32_t ip, uint16_t port)
{
    LOCK();
    if (!sock_valid(sd))
        RET(-EBADF);
    struct socket *s = &socks[sd];
    if (port == 0)
        RET(-EINVAL);
    if (s->connected || s->tcb)
        RET(-EISCONN);
    uint32_t src = net_pick_source(ip);
    if (!src)
        RET(-ENETUNREACH);
    if (!s->bound) {
        s->lport = sock_ephemeral_port(s->type);
        if (!s->lport)
            RET(-EADDRINUSE);
        s->bound = true;
    }
    if (!s->lip)
        s->lip = src;
    s->rip = ip;
    s->rport = port;

    if (s->type == SOCK_DGRAM) {
        s->connected = true;
        RET(0);
    }
    struct tcb *t = tcp_connect(s);
    if (!t) {
        s->rip = 0;
        s->rport = 0;
        RET(-ENOMEM);
    }
    s->connected = true;
    s->err = 0;
    for (;;) {
        t = s->tcb;
        if (!t) {
            s->connected = false;
            RET(-ECONNRESET);
        }
        if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT)
            RET(0);
        if (t->state == TCP_CLOSED) {
            int e = t->err ? t->err : ECONNREFUSED;
            s->err = 0;
            tcp_close(s); /* frees the dead connection */
            s->connected = false;
            s->rip = 0;
            s->rport = 0;
            RET(-e);
        }
        sock_block(s, 0);
    }
}

int64_t sock_send(int sd, const void *buf, size_t len, uint32_t ip, uint16_t port)
{
    LOCK();
    if (!sock_valid(sd))
        RET(-EBADF);
    struct socket *s = &socks[sd];
    if (s->type == SOCK_STREAM) {
        if (!s->tcb)
            RET(-ENOTCONN);
        RET(tcp_write(s, buf, len, s->nonblock));
    }
    if (!ip) {
        if (!s->connected)
            RET(-EDESTADDRREQ);
        ip = s->rip;
        port = s->rport;
    }
    if (port == 0)
        RET(-EINVAL);
    if (!s->bound) {
        s->lport = sock_ephemeral_port(SOCK_DGRAM);
        if (!s->lport)
            RET(-EADDRINUSE);
        s->bound = true;
    }
    int rc = udp_output(s, ip, port, buf, len);
    RET(rc < 0 ? rc : (int64_t)len);
}

int64_t sock_recv(int sd, void *buf, size_t len, uint32_t *ip, uint16_t *port)
{
    LOCK();
    if (!sock_valid(sd))
        RET(-EBADF);
    struct socket *s = &socks[sd];
    uint64_t deadline = deadline_for(s);
    if (s->type == SOCK_STREAM) {
        if (!s->tcb) {
            int e = s->err;
            s->err = 0;
            RET(e ? -e : -ENOTCONN);
        }
        if (s->tcb->state == TCP_LISTEN)
            RET(-ENOTCONN);
        int64_t n = tcp_read(s, buf, len, s->nonblock, deadline);
        if (n >= 0 && s->tcb) {
            if (ip)
                *ip = s->rip;
            if (port)
                *port = s->rport;
        }
        RET(n);
    }
    for (;;) {
        if (!list_empty(&s->rxq)) {
            struct dgram *d = container_of(s->rxq.next, struct dgram, node);
            list_del(&d->node);
            s->rxq_n--;
            size_t n = d->len < len ? d->len : len; /* the rest of a long datagram is discarded */
            memcpy(buf, d->data, n);
            if (ip)
                *ip = d->ip;
            if (port)
                *port = d->port;
            kfree(d);
            RET((int64_t)n);
        }
        if (s->nonblock)
            RET(-EAGAIN);
        if (sock_block(s, deadline) < 0)
            RET(-EAGAIN);
    }
}

int sock_ctl(int sd, int op, int64_t value, int64_t *out)
{
    LOCK();
    if (!sock_valid(sd))
        RET(-EBADF);
    struct socket *s = &socks[sd];
    *out = 0;
    switch (op) {
    case SOCKCTL_RCVTIMEO:
        if (value < 0 || value > 3600000)
            RET(-EINVAL);
        s->rcvto_ticks = value ? ms_to_ticks((uint64_t)value) : 0;
        RET(0);
    case SOCKCTL_NONBLOCK:
        s->nonblock = value != 0;
        RET(0);
    case SOCKCTL_REUSEADDR:
        s->reuse = value != 0;
        RET(0);
    case SOCKCTL_SHUT_WR:
        if (s->type != SOCK_STREAM)
            RET(-EOPNOTSUPP);
        if (!s->tcb)
            RET(-ENOTCONN);
        tcp_shutdown_wr(s);
        RET(0);
    case SOCKCTL_GETNAME:
        *out = s->lport;
        RET(0);
    default:
        RET(-EINVAL);
    }
}
