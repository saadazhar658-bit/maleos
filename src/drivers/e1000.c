#include "drivers/e1000.h"

#include "arch/ioapic.h"
#include "arch/irq.h"
#include "kernel/errno.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/spinlock.h"
#include "kernel/string.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "net/net.h"

/*
 * Intel 8254x driver: legacy descriptors, 32 receive and 16 transmit buffers of 2 KiB. Receive
 * runs in a kernel thread that is woken by the card's interrupt (and polls once per tick as a
 * safety net, so a lost or misrouted interrupt only costs latency).
 */

#define REG_CTRL 0x0000
#define REG_STATUS 0x0008
#define REG_EERD 0x0014
#define REG_ICR 0x00C0
#define REG_IMS 0x00D0
#define REG_IMC 0x00D8
#define REG_RCTL 0x0100
#define REG_TCTL 0x0400
#define REG_TIPG 0x0410
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH 0x2810
#define REG_RDT 0x2818
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH 0x3810
#define REG_TDT 0x3818
#define REG_MTA 0x5200
#define REG_RAL0 0x5400
#define REG_RAH0 0x5404

#define CTRL_SLU (1u << 6)
#define CTRL_RST (1u << 26)
#define RCTL_EN (1u << 1)
#define RCTL_BAM (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN (1u << 1)
#define TCTL_PSP (1u << 3)
#define ICR_RXT0 (1u << 7)
#define ICR_RXO (1u << 6)
#define ICR_RXDMT0 (1u << 4)
#define ICR_LSC (1u << 2)

#define RX_COUNT 32
#define TX_COUNT 16
#define BUF_SIZE 2048
#define VEC_NIC 0x33

struct rx_desc {
    uint64_t addr;
    uint16_t length, csum;
    uint8_t status, errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso, cmd, status, css;
    uint16_t special;
} __attribute__((packed));

#define DESC_DD 0x01
#define RX_EOP 0x02
#define TX_CMD_EOP 0x01
#define TX_CMD_IFCS 0x02
#define TX_CMD_RS 0x08

static struct {
    volatile uint8_t *mmio;
    struct rx_desc *rx;
    struct tx_desc *tx;
    uint8_t *rx_buf, *tx_buf;
    uint64_t rx_buf_phys, tx_buf_phys;
    uint32_t rx_next, tx_next;
    struct netif *nif;
    struct waitq wq;
    spinlock_t lock;
    volatile bool irq_pending;
    uint64_t irqs;
} nic;

static inline uint32_t rd(uint32_t reg)
{
    return *(volatile uint32_t *)(nic.mmio + reg);
}

static inline void wr(uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(nic.mmio + reg) = v;
}

static int e1000_send(struct netif *nif, const void *frame, size_t len)
{
    (void)nif;
    if (len < 14 || len > BUF_SIZE)
        return -EMSGSIZE;
    uint64_t f = spin_lock_irqsave(&nic.lock);
    uint32_t i = nic.tx_next;
    volatile struct tx_desc *d = &nic.tx[i];
    if (!(d->status & DESC_DD)) { /* the ring is full: the packet is lost like on a busy wire */
        spin_unlock_irqrestore(&nic.lock, f);
        return -EAGAIN;
    }
    uint8_t *buf = nic.tx_buf + (size_t)i * BUF_SIZE;
    memcpy(buf, frame, len);
    if (len < 60) { /* pad to the Ethernet minimum (the card appends the FCS) */
        memset(buf + len, 0, 60 - len);
        len = 60;
    }
    d->length = (uint16_t)len;
    d->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    d->status = 0;
    __asm__ volatile("" ::: "memory");
    nic.tx_next = (i + 1) % TX_COUNT;
    wr(REG_TDT, nic.tx_next);
    spin_unlock_irqrestore(&nic.lock, f);
    return 0;
}

static void nic_irq(struct interrupt_frame *fr)
{
    (void)fr;
    uint32_t icr = rd(REG_ICR); /* reading clears the cause and drops the level-triggered line */
    if (icr) {
        nic.irqs++;
        nic.irq_pending = true;
        waitq_wake_all(&nic.wq);
    }
}

static void rx_thread(void *arg)
{
    (void)arg;
    uint8_t frame[BUF_SIZE];
    for (;;) {
        uint64_t f = spin_lock_irqsave(&nic.lock);
        if (!nic.irq_pending && !(nic.rx[nic.rx_next].status & DESC_DD))
            waitq_wait(&nic.wq, &nic.lock, 1);
        nic.irq_pending = false;
        spin_unlock_irqrestore(&nic.lock, f);

        for (;;) {
            volatile struct rx_desc *d = &nic.rx[nic.rx_next];
            if (!(d->status & DESC_DD))
                break;
            size_t len = d->length;
            bool ok = (d->status & RX_EOP) && d->errors == 0 && len > 0 && len <= BUF_SIZE;
            if (ok)
                memcpy(frame, nic.rx_buf + (size_t)nic.rx_next * BUF_SIZE, len);
            d->status = 0;
            uint32_t done = nic.rx_next;
            nic.rx_next = (nic.rx_next + 1) % RX_COUNT;
            wr(REG_RDT, done); /* hand the slot back to the card */
            if (ok)
                net_input(nic.nif, frame, len);
        }
    }
}

static uint16_t eeprom_read(uint8_t word)
{
    wr(REG_EERD, 1u | ((uint32_t)word << 8));
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(REG_EERD);
        if (v & (1u << 4))
            return (uint16_t)(v >> 16);
    }
    return 0;
}

static int e1000_probe(struct pci_dev *pci)
{
    const struct pci_bar *bar = &pci->bar[0];
    if (!bar->size || bar->io)
        return -1;
    pci_enable(pci, PCI_CMD_MEM | PCI_CMD_BUSMASTER);
    nic.mmio = vmm_ioremap(bar->base, bar->size < 0x20000 ? 0x20000 : bar->size);
    if (!nic.mmio)
        return -1;

    wr(REG_IMC, 0xffffffff);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    for (volatile int i = 0; i < 1000000; i++)
        if (!(rd(REG_CTRL) & CTRL_RST))
            break;
    wr(REG_IMC, 0xffffffff);
    rd(REG_ICR);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU);

    uint8_t mac[6];
    uint32_t ral = rd(REG_RAL0), rah = rd(REG_RAH0);
    if (ral == 0 && (rah & 0xffff) == 0) {
        uint16_t w0 = eeprom_read(0), w1 = eeprom_read(1), w2 = eeprom_read(2);
        mac[0] = (uint8_t)w0;
        mac[1] = (uint8_t)(w0 >> 8);
        mac[2] = (uint8_t)w1;
        mac[3] = (uint8_t)(w1 >> 8);
        mac[4] = (uint8_t)w2;
        mac[5] = (uint8_t)(w2 >> 8);
    } else {
        mac[0] = (uint8_t)ral;
        mac[1] = (uint8_t)(ral >> 8);
        mac[2] = (uint8_t)(ral >> 16);
        mac[3] = (uint8_t)(ral >> 24);
        mac[4] = (uint8_t)rah;
        mac[5] = (uint8_t)(rah >> 8);
    }
    wr(REG_RAL0,
       (uint32_t)mac[0] | (uint32_t)mac[1] << 8 | (uint32_t)mac[2] << 16 | (uint32_t)mac[3] << 24);
    wr(REG_RAH0, (uint32_t)mac[4] | (uint32_t)mac[5] << 8 | (1u << 31));
    for (int i = 0; i < 128; i++)
        wr(REG_MTA + (uint32_t)i * 4, 0);

    /* One physically contiguous block: descriptor rings first, then the packet buffers. */
    size_t rings = RX_COUNT * sizeof(struct rx_desc) + TX_COUNT * sizeof(struct tx_desc);
    size_t total = rings + (size_t)(RX_COUNT + TX_COUNT) * BUF_SIZE;
    uint64_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_frames(pages);
    if (!phys)
        return -ENOMEM;
    uint8_t *base = phys_to_virt(phys);
    memset(base, 0, pages * PAGE_SIZE);
    nic.rx = (struct rx_desc *)base;
    nic.tx = (struct tx_desc *)(base + RX_COUNT * sizeof(struct rx_desc));
    nic.rx_buf = base + rings;
    nic.tx_buf = nic.rx_buf + (size_t)RX_COUNT * BUF_SIZE;
    nic.rx_buf_phys = phys + rings;
    nic.tx_buf_phys = nic.rx_buf_phys + (size_t)RX_COUNT * BUF_SIZE;
    uint64_t rx_phys = phys;
    uint64_t tx_phys = phys + RX_COUNT * sizeof(struct rx_desc);

    for (int i = 0; i < RX_COUNT; i++)
        nic.rx[i].addr = nic.rx_buf_phys + (uint64_t)i * BUF_SIZE;
    for (int i = 0; i < TX_COUNT; i++) {
        nic.tx[i].addr = nic.tx_buf_phys + (uint64_t)i * BUF_SIZE;
        nic.tx[i].status = DESC_DD; /* free */
    }
    wr(REG_RDBAL, (uint32_t)rx_phys);
    wr(REG_RDBAH, (uint32_t)(rx_phys >> 32));
    wr(REG_RDLEN, RX_COUNT * sizeof(struct rx_desc));
    wr(REG_RDH, 0);
    wr(REG_RDT, RX_COUNT - 1);
    wr(REG_TDBAL, (uint32_t)tx_phys);
    wr(REG_TDBAH, (uint32_t)(tx_phys >> 32));
    wr(REG_TDLEN, TX_COUNT * sizeof(struct tx_desc));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);

    waitq_init(&nic.wq);

    nic.nif = net_register("eth0", mac, e1000_send, &nic);
    if (!nic.nif)
        return -1;

    /* PCI interrupts on QEMU's i440FX: INTx lines are swizzled across slots onto GSIs 16..19. */
    uint8_t pin = pci_read8(pci, 0x3d);
    int routed = -1;
    if (pin >= 1 && pin <= 4) {
        uint32_t gsi = 16 + ((uint32_t)(pin - 1) + pci->dev - 1) % 4;
        irq_register(VEC_NIC, nic_irq);
        routed = ioapic_route_gsi(gsi, VEC_NIC, true, true);
    }

    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC); /* 2048-byte buffers, broadcast accepted */
    wr(REG_TIPG, 0x0060200A);
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12));
    if (routed == 0)
        wr(REG_IMS, ICR_RXT0 | ICR_RXO | ICR_RXDMT0 | ICR_LSC);

    struct thread *t = thread_create("e1000-rx", rx_thread, NULL, 2);
    thread_detach(t);

    printk("e1000: %02x:%02x:%02x:%02x:%02x:%02x, %s\n", mac[0], mac[1], mac[2], mac[3], mac[4],
           mac[5], routed == 0 ? "interrupt-driven" : "polling");
    return 0;
}

static const struct pci_match e1000_ids[] = {
    {.flags = MATCH_VENDOR | MATCH_DEVICE, .vendor = 0x8086, .device = 0x100E}, /* 82540EM */
    {.flags = MATCH_VENDOR | MATCH_DEVICE, .vendor = 0x8086, .device = 0x100F}, /* 82545EM */
    {.flags = MATCH_VENDOR | MATCH_DEVICE, .vendor = 0x8086, .device = 0x1004}, /* 82544GC */
    {0},
};

const struct driver e1000_driver = {
    .name = "e1000",
    .match = e1000_ids,
    .probe_pci = e1000_probe,
};
