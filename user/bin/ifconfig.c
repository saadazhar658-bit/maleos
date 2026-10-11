#include "ulib.h"

static void show_ip(const char *label, uint32_t ip)
{
    printf("  %s %u.%u.%u.%u\n", label, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

int main(void)
{
    struct abi_netinfo ni;
    if (netinfo(&ni) < 0) {
        dprintf(2, "ifconfig: cannot read network state\n");
        return 1;
    }
    printf("lo: 127.0.0.1/8 (loopback, always up)\n");
    if (!ni.present) {
        printf("eth0: no network card found\n");
        return 0;
    }
    printf("eth0: %s, hardware address %02x:%02x:%02x:%02x:%02x:%02x\n", ni.up ? "up" : "down",
           ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
    show_ip("address", ni.ip);
    show_ip("netmask", ni.mask);
    show_ip("gateway", ni.gw);
    printf("  received %lu frames (%lu bytes), sent %lu frames (%lu bytes), %lu dropped\n",
           (unsigned long)ni.rx_frames, (unsigned long)ni.rx_bytes, (unsigned long)ni.tx_frames,
           (unsigned long)ni.tx_bytes, (unsigned long)ni.rx_dropped);
    printf("  bad checksums: %lu\n", (unsigned long)ni.bad_checksums);
    return 0;
}
