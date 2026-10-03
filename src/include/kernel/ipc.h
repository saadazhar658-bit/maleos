#ifndef MALEOS_KERNEL_IPC_H
#define MALEOS_KERNEL_IPC_H

#include <stddef.h>
#include <stdint.h>

#define IPC_MAX_PAYLOAD 48
#define IPC_FOREVER UINT64_MAX

#define IPC_OK 0
#define IPC_EFULL (-1)
#define IPC_EEMPTY (-2)
#define IPC_ETIMEOUT (-3)
#define IPC_ECLOSED (-4)
#define IPC_EINVAL (-5)
#define IPC_ENOMEM (-6)

/* Fixed-size message; copied into and out of the port, so there are no ownership issues. */
struct ipc_msg {
    uint32_t type;   /* application defined */
    uint32_t len;    /* bytes of payload in use */
    uint64_t sender; /* tid, filled in by the kernel on send */
    uint8_t payload[IPC_MAX_PAYLOAD];
};

struct ipc_port;

/* A port is a bounded queue of messages with blocking send and receive. */
struct ipc_port *ipc_port_create(const char *name, size_t capacity);
/* Wakes all blocked threads with IPC_ECLOSED; do not use the port afterwards. */
void ipc_port_destroy(struct ipc_port *p);

/* timeout_ms: 0 = do not block, IPC_FOREVER = wait as long as needed. */
int ipc_send_timeout(struct ipc_port *p, const struct ipc_msg *m, uint64_t timeout_ms);
int ipc_recv_timeout(struct ipc_port *p, struct ipc_msg *m, uint64_t timeout_ms);

static inline int ipc_send(struct ipc_port *p, const struct ipc_msg *m)
{
    return ipc_send_timeout(p, m, IPC_FOREVER);
}
static inline int ipc_recv(struct ipc_port *p, struct ipc_msg *m)
{
    return ipc_recv_timeout(p, m, IPC_FOREVER);
}
static inline int ipc_try_send(struct ipc_port *p, const struct ipc_msg *m)
{
    return ipc_send_timeout(p, m, 0);
}
static inline int ipc_try_recv(struct ipc_port *p, struct ipc_msg *m)
{
    return ipc_recv_timeout(p, m, 0);
}

/*
 * Synchronous request/reply. Sends `req` to `server`, then waits for the server to answer
 * with ipc_reply(). The timeout applies to the send and to the wait for the reply.
 */
int ipc_call(struct ipc_port *server, const struct ipc_msg *req, struct ipc_msg *reply,
             uint64_t timeout_ms);
/* Answer a request received from ipc_call(); routed by req->sender. */
int ipc_reply(const struct ipc_msg *req, const struct ipc_msg *reply);

#endif
