/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test-only MTK register integration of upstream QEMU's MUSB PIO model.
 * USB descriptors, control transfers, HID and SCSI come from QEMU devices.
 * This does not model MT6789 clocks, PHY, Type-C or electrical timing. */
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "t70-musb-core.inc"

#define TYPE_T70_MUSB "t70-musb"
typedef struct {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    MUSBState *core;
    qemu_irq irq;
    uint16_t irq_levels;
    bool inherited_quiescent;
    bool cold_attach;
    bool vbus_present;
    uint32_t dma_control[8];
    uint16_t toggle[2];
    uint8_t fifo_size[16][2];
    uint32_t reads, writes, fifo_reads, fifo_writes;
} T70Musb;

static void t70_musb_vbus(void *opaque, int n, int level)
{
    T70Musb *s = opaque;
    s->vbus_present = level != 0;
}

static void t70_musb_irq(void *opaque, int n, int level)
{
    T70Musb *s = opaque;
    if (n < 10) {
        s->irq_levels = (s->irq_levels & ~(1U << n)) | (level << n);
        qemu_set_irq(s->irq, s->irq_levels != 0);
    }
    if (n == musb_irq_disconnect && level) {
        s->core->devctl &= ~(MGC_M_DEVCTL_FSDEV | MGC_M_DEVCTL_LSDEV);
    }
}

static uint64_t t70_musb_read(void *opaque, hwaddr addr, unsigned size)
{
    T70Musb *s = opaque;
    unsigned index = size == 1 ? 0 : size == 2 ? 1 : 2;
    s->reads++;
    if (addr >= 0xf000) {
        const uint32_t stats[] = {s->reads, s->writes, s->fifo_reads,
                                 s->fifo_writes, s->core->devctl, s->core->intr};
        unsigned n = (addr - 0xf000) / 4;
        return n < ARRAY_SIZE(stats) ? stats[n] : 0;
    }
    if (addr >= 0x20 && addr < 0x60) {
        s->fifo_reads++;
    }
    if (addr == 0x60) {
        if (s->cold_attach) {
            /* The guest must enable the modeled supply and SESSION before
             * host/session/VBUS readiness is returned. Speed remains upstream. */
            return (s->core->devctl & (s->core->port.dev ? 0x61 : 0x01)) |
                   (s->vbus_present ? 0x18 : 0) |
                   (s->vbus_present && (s->core->devctl & 1) ? 4 : 0);
        }
        /* Explicit fixture: upstream model owns speed/attach state;
         * the test board supplies powered A-device host/session state. */
        return (s->core->devctl & 0x60) | 0x1d;
    }
    if (addr == 0xa) {
        return s->core->intr; /* MTK INTRUSB is W1C, not read-to-clear. */
    }
    if (addr == 0x80 || addr == 0x84) {
        return s->toggle[addr == 0x80];
    }
    if (addr == 0x62 || addr == 0x63) {
        return s->fifo_size[s->core->idx][addr == 0x63];
    }
    if (addr == 0xc && size == 2) {
        return (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000) & 0x7ff;
    }
    if (s->cold_attach && addr >= 0x204 && addr <= 0x274 &&
        (addr & 0xf) == 4 && (size == 2 || size == 4)) {
        return s->dma_control[(addr - 0x204) / 0x10] &
               (size == 2 ? 0xffffU : UINT32_MAX);
    }
    if (addr >= 0x480 && addr < 0x500) {
        addr -= 0x400; /* MTK bus control window */
    } else if (addr >= 0x200) {
        return 0;
    }
    return musb_read[index](s->core, addr);
}

static void t70_musb_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    T70Musb *s = opaque;
    unsigned index = size == 1 ? 0 : size == 2 ? 1 : 2;
    s->writes++;
    if (addr >= 0x20 && addr < 0x60) {
        s->fifo_writes++;
    }
    if (addr == 0x80 || addr == 0x84) {
        unsigned dir = addr == 0x80;
        uint16_t mask = value >> 16;
        s->toggle[dir] = (s->toggle[dir] & ~mask) | (value & mask);
        return;
    }
    if (addr == 0xa) {
        for (unsigned i = 0; i < 8; i++) {
            if (value & (1U << i)) {
                musb_intr_set(s->core, i, 0);
            }
        }
        return;
    }
    if (addr == 0x62 || addr == 0x63) {
        s->fifo_size[s->core->idx][addr == 0x63] = value;
        return;
    }
    if (s->cold_attach && addr >= 0x204 && addr <= 0x274 &&
        (addr & 0xf) == 4 && (size == 2 || size == 4)) {
        /* MTK configuration readback only; no DMA engine is emulated. */
        unsigned channel = (addr - 0x204) / 0x10;
        uint32_t mask = size == 2 ? 0xffffU : UINT32_MAX;
        s->dma_control[channel] = (s->dma_control[channel] & ~mask) |
                                  (value & mask);
        return;
    }
    if (addr >= 0x480 && addr < 0x500) {
        addr -= 0x400;
    } else if (addr >= 0x200) {
        return;
    }
    musb_write[index](s->core, addr, value);
}

static const MemoryRegionOps t70_musb_ops = {
    .read = t70_musb_read, .write = t70_musb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1, .valid.max_access_size = 4,
};

static void t70_musb_realize(DeviceState *dev, Error **errp)
{
    T70Musb *s = (T70Musb *)dev;
    s->core = musb_init(dev, 0);
    for (unsigned i = 0; i < 16; i++) {
        s->core->ep[i].buf[0] = s->core->buf + i * 1024;
        s->core->ep[i].buf[1] = s->core->buf + i * 1024 + (i ? 512 : 0);
    }
    /* Explicit initial-state choice. Never change these after guest execution. */
    if (s->inherited_quiescent) {
        musb_write[1](s->core, 0x6, 0);
        musb_write[1](s->core, 0x8, 0);
    }
    musb_write[0](s->core, 0xb, s->inherited_quiescent ? 0 : 0x30);
    musb_write[0](s->core, 0x60, s->cold_attach ? 0 : 1);
}

static void t70_musb_init(Object *obj)
{
    T70Musb *s = (T70Musb *)obj;
    qdev_init_gpio_in(DEVICE(obj), t70_musb_irq, musb_irq_max);
    qdev_init_gpio_in_named(DEVICE(obj), t70_musb_vbus, "vbus", 1);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->mmio, obj, &t70_musb_ops, s, TYPE_T70_MUSB, 0x10000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const Property t70_musb_properties[] = {
    DEFINE_PROP_BOOL("inherited-quiescent", T70Musb, inherited_quiescent, false),
    DEFINE_PROP_BOOL("cold-attach", T70Musb, cold_attach, false),
};

static void t70_musb_class_init(ObjectClass *klass, const void *data)
{
    DEVICE_CLASS(klass)->realize = t70_musb_realize;
    device_class_set_props(DEVICE_CLASS(klass), t70_musb_properties);
}

static const TypeInfo t70_musb_type = {
    .name = TYPE_T70_MUSB, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(T70Musb), .instance_init = t70_musb_init,
    .class_init = t70_musb_class_init,
};
static void t70_musb_register_types(void) { type_register_static(&t70_musb_type); }
type_init(t70_musb_register_types)
