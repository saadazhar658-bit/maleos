# Network stack (Phase 7b)

Sources: `src/net/` (net.c, udp.c, tcp.c, socket.c, net_selftest.c), `src/drivers/e1000.c`,
headers in `src/include/net/`.

## Design
- One global IRQ-safe spinlock `net_lock` protects all stack state. Blocking socket calls sleep on
  per-socket wait queues using that lock.
- Interfaces: loopback `lo` (queued, drained by the `net` worker thread, which also runs the TCP and
  ARP timers each tick) and `eth0` (Intel e1000, PCI 8086:100E/100F/1004, interrupt-driven with a
  per-tick poll as a safety net).
- `eth0` is statically configured: 10.0.2.15/24, gateway 10.0.2.2 (QEMU user networking). No DHCP, no DNS.

## Protocols
| Layer | Implemented | Not implemented |
|---|---|---|
| ARP | 16-entry cache, 1 queued packet per pending entry, retries, 5 min lifetime | gratuitous ARP |
| IPv4 | checksum, TTL, DF, loopback delivery | fragmentation (dropped and counted), options, routing table |
| ICMP | echo request/reply, port unreachable | other types |
| UDP | bind, connected filtering, 32-datagram queue, checksum | broadcast options |
| TCP | handshake incl. simultaneous open, cumulative ACK, adaptive RTO, fast retransmit, zero-window probes, MSS, orderly/abortive close, TIME_WAIT (2 s), backlog 8 | congestion control, SACK, out-of-order reassembly, window scaling, timestamps, urgent data |

Limits: 64 sockets, 128 TCBs, 16 KiB TCP buffers.

## System calls
SOCKET 29, BIND 30, LISTEN 31, ACCEPT 32, CONNECT 33, SENDTO 34, RECVFROM 35, SOCKCTL 36
(RCVTIMEO, NONBLOCK, SHUT_WR, GETNAME, REUSEADDR), NETINFO 37, PING 38. Stream sockets also work
through read/write. `connect` always blocks (about 15 s of SYN retries to a silent host).

## User programs
`ifconfig`, `ping`, `nc`, `wget` (IPv4 literals only), `httpd` (serves /www on port 80), `echod`
(TCP and UDP echo on port 7). `init` starts httpd and echod at boot.

## Testing
- Kernel selftest `NET SELFTEST` (155 checks): checksums, loopback TCP/UDP/ICMP, loss injection,
  zero window, parallel connections, ARP and garbage input, 6000 fuzzed packets, leak checks.
- User-space `t_net` and the network syscalls in `t_fuzz`.
- `make net-test`: end-to-end against the host through QEMU port forwarding.
- Boot without a NIC: `NET=0 ./scripts/boot-test.sh`.
