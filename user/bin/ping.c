#include "ulib.h"

/* ping <a.b.c.d> [count]. The round-trip time has the resolution of the 10 ms scheduler tick. */
int main(int argc, char **argv)
{
    uint32_t ip;
    if (argc < 2 || parse_ip(argv[1], &ip) < 0) {
        dprintf(2, "usage: ping a.b.c.d [count]\n");
        return 2;
    }
    int count = argc > 2 ? (int)atol(argv[2]) : 4;
    if (count < 1 || count > 100)
        count = 4;
    printf("PING %s\n", argv[1]);
    int got = 0;
    for (int i = 1; i <= count; i++) {
        long rtt = ping(ip, 1000, (unsigned)i);
        if (rtt >= 0) {
            printf("reply from %s: seq=%d time=%s%ld ms\n", argv[1], i, rtt < 10 ? "<" : "",
                   rtt < 10 ? 10 : rtt);
            got++;
        } else {
            printf("seq=%d: %s\n", i, strerror((int)-rtt));
        }
        if (i < count)
            sleep_ms(300);
    }
    printf("%d sent, %d received, %d%% loss\n", count, got, (count - got) * 100 / count);
    return got ? 0 : 1;
}
