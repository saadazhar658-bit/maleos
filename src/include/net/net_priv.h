#ifndef MALEOS_NET_PRIV_H
#define MALEOS_NET_PRIV_H

#include "kernel/list.h"
#include "net/net.h"

/* Private definitions shared by the socket, UDP and TCP layers. Everything here is protected by
 * net_lock. */

#define TCP_BUF 16384 /* send and receive buffer per connection (also the window: no scaling) */
#define TCB_MAX 128
#define UDP_RXQ_MAX 32

enum tcp_state {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
};

struct socket;

#define ACCEPTQ_MAX 8

struct tcb {
    struct socket *sock; /* NULL once the user closed the descriptor (orphan) */
    struct tcb *parent;  /* listener this connection was spawned from, until accepted */
    enum tcp_state state;
    uint32_t lip, rip;
    uint16_t lport, rport;

    uint32_t iss, snd_una, snd_nxt, snd_wnd;
    uint32_t irs, rcv_nxt;
    uint32_t sb_seq; /* sequence number of the first byte in the send buffer */
    uint16_t mss;
    bool fin_pending, fin_sent, fin_received;
    int err;

    uint8_t *sbuf, *rbuf;
    uint32_t sb_off, sb_len; /* send ring: start offset and byte count (unacked + unsent) */
    uint32_t rb_off, rb_len;
    uint32_t adv_wnd; /* last window we advertised */

    /* retransmission */
    uint64_t rto_deadline; /* 0 = timer off */
    uint32_t rto, srtt, rttvar;
    int retries;
    uint32_t rtt_seq;
    uint64_t rtt_start;
    int dupacks;

    uint64_t tw_deadline; /* TIME_WAIT and orphan FIN_WAIT2 expiry */

    /* listeners */
    struct tcb *acceptq[ACCEPTQ_MAX];
    int aq_n;
    int backlog;
    int pending_children; /* SYN_RCVD plus queued */
};

struct dgram {
    struct list_node node;
    uint32_t ip;
    uint16_t port, len;
    uint8_t data[];
};

struct socket {
    bool used;
    int type; /* SOCK_STREAM or SOCK_DGRAM */
    bool bound, nonblock, reuse, connected;
    uint32_t lip, rip;
    uint16_t lport, rport;
    uint64_t rcvto_ticks; /* 0 = wait forever */
    struct waitq wq;
    int err;
    struct list_node rxq; /* UDP */
    int rxq_n;
    struct tcb *tcb; /* TCP */
};

extern struct socket socks[SOCK_MAX];
extern uint64_t net_stat_tcp_retrans, net_stat_udp_dropped;

/* udp.c */
int udp_output(struct socket *s, uint32_t dst, uint16_t dport, const void *data, size_t len);
void udp_socket_free(struct socket *s);

/* tcp.c */
struct tcb *tcp_connect(struct socket *s);
int tcp_listen(struct socket *s, int backlog);
struct tcb *tcp_accept_take(struct tcb *listener);
int64_t tcp_write(struct socket *s, const uint8_t *data, size_t len, bool nonblock);
int64_t tcp_read(struct socket *s, uint8_t *buf, size_t len, bool nonblock, uint64_t deadline);
void tcp_close(struct socket *s);
void tcp_shutdown_wr(struct socket *s);
int tcp_connection_count(void);
void tcp_free_child(struct tcb *c); /* an accepted-but-abandoned child */
bool tcp_port_in_use(uint16_t port);
void tcp_dump(void);

/* socket.c */
bool sock_port_in_use(int type, uint16_t port, uint32_t ip, const struct socket *except);
uint16_t sock_ephemeral_port(int type);
void sock_wake(struct socket *s);
int sock_block(struct socket *s, uint64_t deadline);
struct socket *sock_lookup_udp(uint32_t dst, uint16_t dport, uint32_t src, uint16_t sport);

#endif
