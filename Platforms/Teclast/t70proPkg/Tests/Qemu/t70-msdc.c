/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test-only MTK MSDC PIO register adapter to the independent QEMU SD model.
 * Models synchronous commands, R1/R2 responses, 128-byte RX FIFO, Auto CMD12,
 * W1C interrupt state, reset/clock-stable status and card detect.
 * Does not model DMA, CRC faults, signal timing, tuning or physical supplies.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/sd/sd.h"
#include "qemu/module.h"

#define TYPE_T70_MSDC "t70-msdc"
#define TYPE_T70_SD_BUS "t70-sd-bus"
OBJECT_DECLARE_SIMPLE_TYPE(T70Msdc, T70_MSDC)

struct T70Msdc {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    SDBus bus;
    qemu_irq irq;
    uint32_t regs[0x1000 / 4];
    uint32_t remaining, raw_cmd;
    uint32_t commands, bytes_read, errors, last_cmd;
    bool auto12;
};

bool t70_msdc_inserted(DeviceState *dev);

bool t70_msdc_inserted(DeviceState *dev)
{
    return sdbus_get_inserted(&T70_MSDC(dev)->bus);
}

static void t70_msdc_irq(T70Msdc *s)
{
    qemu_set_irq(s->irq, !!(s->regs[0x00c/4] & s->regs[0x010/4]));
}

static void t70_msdc_command(T70Msdc *s, uint32_t raw)
{
    SDRequest req = { .cmd = raw & 0x3f, .arg = s->regs[0x038/4] };
    uint8_t response[16] = {0};
    unsigned rsp = (raw >> 7) & 7;
    size_t len;
    s->commands++;
    s->last_cmd = req.cmd;
    s->raw_cmd = raw;
    s->remaining = 0;
    s->auto12 = !!(raw & (1U << 28));
    len = sdbus_do_command(&s->bus, &req, response, sizeof(response));
    if ((rsp == 2 && len != 16) || (rsp != 0 && rsp != 2 && len != 4)) {
        s->regs[0x00c/4] |= 1U << 9; /* command timeout */
        s->errors++;
        t70_msdc_irq(s);
        return;
    }
    if (len == 16) {
        /* R2: RESP3 carries bits 127:96, RESP0 carries bits 31:0. */
        for (unsigned i = 0; i < 4; i++) {
            s->regs[0x040/4+i] = ldl_be_p(response + (3-i)*4);
        }
    } else if (len == 4) {
        s->regs[0x040/4] = ldl_be_p(response);
    }
    s->regs[0x00c/4] |= 1U << 8; /* command response ready */
    if (raw & ((1U << 11) | (1U << 12))) {
        unsigned block_size = (raw >> 16) & 0xfff;
        s->remaining = block_size * s->regs[0x050/4];
        if (!block_size || (raw & (1U << 13)) ||
            !sdbus_data_ready(&s->bus)) {
            /* TX is deliberately unsupported; fail rather than accept it. */
            s->remaining = 0;
            s->regs[0x00c/4] |= 1U << 14;
            s->errors++;
        }
    }
    t70_msdc_irq(s);
}

static uint64_t t70_msdc_read(void *opaque, hwaddr off, unsigned size)
{
    T70Msdc *s = opaque;
    if (off == 0xf00) { return s->commands; }
    if (off == 0xf04) { return s->bytes_read; }
    if (off == 0xf08) { return s->errors; }
    if (off == 0xf0c) { return s->last_cmd; }
    if (off == 0xf10) { return s->remaining; }
    if (off == 0x000) { return (s->regs[0] & ~4U) | 0x80; }
    if (off == 0x008) {
        return ((uint32_t)sdbus_get_dat_lines(&s->bus) << 16) |
               ((uint32_t)sdbus_get_cmd_line(&s->bus) << 24);
    }
    if (off == 0x014) { return MIN(s->remaining, 128); }
    if (off == 0x03c) { return 0; } /* synchronous PIO controller is idle */
    if (off == 0x01c) {
        uint64_t value = 0;
        for (unsigned i = 0; i < size && s->remaining; i++) {
            if (!sdbus_data_ready(&s->bus)) {
                s->regs[0x00c/4] |= 1U << 14;
                s->errors++;
                break;
            }
            value |= (uint64_t)sdbus_read_byte(&s->bus) << (i * 8);
            s->remaining--;
            s->bytes_read++;
            if (!s->remaining) {
                s->regs[0x00c/4] |= 1U << 12;
                if (s->auto12) {
                    SDRequest stop = { .cmd = 12 };
                    uint8_t response[16];
                    size_t len = sdbus_do_command(&s->bus, &stop,
                                                  response, sizeof(response));
                    s->regs[0x00c/4] |= 1U << (len == 4 ? 3 : 4);
                    s->regs[0x080/4] = len == 4 ? ldl_be_p(response) : 0;
                }
                t70_msdc_irq(s);
            }
        }
        return value;
    }
    if (off < sizeof(s->regs)) { return s->regs[off/4]; }
    return 0;
}

static void t70_msdc_write(void *opaque, hwaddr off, uint64_t value,
                           unsigned size)
{
    T70Msdc *s = opaque;
    if (off >= sizeof(s->regs)) { return; }
    if (off == 0x00c) {
        s->regs[off/4] &= ~value;
    } else if (off == 0x014 && (value & (1U << 31))) {
        s->remaining = 0;
    } else if (off == 0x000 && (value & 4)) {
        s->remaining = 0;
        s->regs[0x00c/4] = 0;
        s->regs[0] = value & ~4U;
    } else {
        s->regs[off/4] = value;
        if (off == 0x034) { t70_msdc_command(s, value); }
    }
    t70_msdc_irq(s);
}

static const MemoryRegionOps t70_msdc_ops = {
    .read = t70_msdc_read,
    .write = t70_msdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void t70_msdc_init(Object *obj)
{
    T70Msdc *s = T70_MSDC(obj);
    memory_region_init_io(&s->mmio, obj, &t70_msdc_ops, s, TYPE_T70_MSDC, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qbus_init(&s->bus, sizeof(s->bus), TYPE_T70_SD_BUS, DEVICE(obj), "sd-bus");
}

static const TypeInfo t70_msdc_types[] = {
    {
        .name = TYPE_T70_MSDC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(T70Msdc),
        .instance_init = t70_msdc_init,
    },
    {
        .name = TYPE_T70_SD_BUS,
        .parent = TYPE_SD_BUS,
        .instance_size = sizeof(SDBus),
    },
};
DEFINE_TYPES(t70_msdc_types)
