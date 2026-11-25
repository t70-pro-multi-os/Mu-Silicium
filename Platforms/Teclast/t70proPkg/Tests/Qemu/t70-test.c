/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * T70Pro implementation-test board, not an MT6789 silicon model.
 * CPU/MMU/GIC/timers execute in QEMU TCG. Board supplier registers below
 * are explicit deterministic fixtures, never a physical-readiness claim.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "exec/ramlist.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/cpu.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/pci-host/gpex.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/ufs/ufs.h"
#include "qobject/qlist.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "qemu/module.h"
#include "target/arm/cpu.h"

bool t70_msdc_inserted(DeviceState *dev);

typedef struct {
    MemoryRegion mmio;
    MemoryRegion ufs_window;
    DeviceState *msdc;
    DeviceState *ufs;
    qemu_irq usb_vbus;
    MemoryRegion *ram;
    uint8_t *registers;
    uint16_t pmic[0x10000];
    uint32_t pwrap_data, pwrap_write, pwrap_fsm;
    bool usb_board;
    bool usb_board_cold_attach;
    uint8_t i2c_fifo[32], tcpc[256], sc[256], pointer[128];
    unsigned fifo_n, fifo_pos;
    uint32_t i2c_starts, chip_writes, i2c_nacks, bad_i2c_width;
    uint32_t tcpc98_event_reads, tcpc98_ack_writes, tcpc98_confirm_reads;
    uint32_t sc03_boost_on_writes, sc03_boost_off_writes;
    uint8_t sc0a_at_boost;
    uint32_t tcpc1a_open_writes, tcpc1a_source_writes;
    uint32_t tcpc_power_present_transitions;
    uint32_t tcpc_vsafe0_clear_transitions, tcpc_vsafe0_reassert_transitions;
    uint32_t tcpc1e_safe_reads, tcpc1e_present_reads;
    uint32_t tcpc97_safe_reads, tcpc97_clear_reads, sc03_boost_on_reads;
    uint32_t sc14_reads;
    uint8_t tcpc98_first_event_value, tcpc98_last_ack_value;
    uint8_t tcpc98_last_confirm_value;
    uint8_t sc14_last_read;
} T70Test;

#include "t70-usb-board.inc"

static uint64_t t70_read(void *opaque, hwaddr off, unsigned size)
{
    T70Test *s = opaque;
    uint64_t value = 0;
    if (s->usb_board && t70_usb_board_read(s, off, size, &value)) {
        return value;
    }
    if (off == 0x5200) {
        /* GPIO7, active high, is driven by real QEMU card media presence. */
        memcpy(&value, s->registers + off, size);
        return (value & ~(1U << 7)) |
               ((uint32_t)t70_msdc_inserted(s->msdc) << 7);
    }
    if (off == 0xf000) {
        /* Test-only observational counter: count and clear written FB pages.
         * Rewriting identical pixels still counts, unlike frame hashes. */
        const hwaddr base = 0x09000000, length = 0xc08000;
        DirtyBitmapSnapshot *snap = memory_region_snapshot_and_clear_dirty(
            s->ram, base, length, DIRTY_MEMORY_VGA);
        for (hwaddr at = base; at < base + length; at += 4096) {
            value += memory_region_snapshot_get_dirty(s->ram, snap, at, 4096);
        }
        g_free(snap);
        return value;
    }
    if (off == 0x268a8) {
        return s->pwrap_fsm << 1;
    }
    if (off == 0x26894) {
        return s->pwrap_data;
    }
    memcpy(&value, s->registers + off, size);
    return value;
}

static void t70_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    T70Test *s = opaque;
    if (t70_gpio_write(s, off, value, size)) {
        return;
    }
    if (s->usb_board && t70_usb_board_write(s, off, value, size)) {
        return;
    }
    memcpy(s->registers + off, &value, size);
    if (off == 0x26884) {
        s->pwrap_write = value;
    } else if (off == 0x268a4) {
        s->pwrap_fsm = 0;
    } else if (off == 0x26880) {
        unsigned reg = value & 0xffff;
        if (value & (1U << 29)) {
            s->pmic[reg] = s->pwrap_write;
            s->pwrap_fsm = 0;
        } else {
            s->pwrap_data = s->pmic[reg];
            s->pwrap_fsm = 6;
        }
    }
}

static const MemoryRegionOps t70_ops = {
    .read = t70_read,
    .write = t70_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

typedef struct {
    MachineState parent_obj;
    bool ufs;
    bool usb_board;
    bool usb_board_active;
    bool usb_board_cold_attach;
    T70Test *board;
} T70MachineState;

static bool t70_get_ufs(Object *obj, Error **errp)
{
    return ((T70MachineState *)obj)->ufs;
}

static void t70_set_ufs(Object *obj, bool value, Error **errp)
{
    ((T70MachineState *)obj)->ufs = value;
}

static bool t70_get_usb_board(Object *obj, Error **errp)
{
    return ((T70MachineState *)obj)->usb_board;
}

static void t70_set_usb_board(Object *obj, bool value, Error **errp)
{
    ((T70MachineState *)obj)->usb_board = value;
}

static bool t70_get_usb_board_active(Object *obj, Error **errp)
{
    return ((T70MachineState *)obj)->usb_board_active;
}

static void t70_set_usb_board_active(Object *obj, bool value, Error **errp)
{
    ((T70MachineState *)obj)->usb_board_active = value;
}

static bool t70_get_usb_board_cold_attach(Object *obj, Error **errp)
{
    return ((T70MachineState *)obj)->usb_board_cold_attach;
}

static void t70_set_usb_board_cold_attach(Object *obj, bool value, Error **errp)
{
    ((T70MachineState *)obj)->usb_board_cold_attach = value;
}

static void t70_ufs_init(MachineState *machine, T70Test *s, DeviceState *gic)
{
    DeviceState *bridge = qdev_new(TYPE_GPEX_HOST);
    DeviceState *ufs = qdev_new("ufs");

    sysbus_realize_and_unref(SYS_BUS_DEVICE(bridge), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(bridge), 0,
                      qdev_get_gpio_in(gic, 169 - 32));
    ufs->id = g_strdup("ufs0");
    qdev_prop_set_bit(ufs, "mcq", false);
    qdev_prop_set_int32(ufs, "addr", PCI_DEVFN(1, 0));
    qdev_realize_and_unref(ufs, BUS(PCI_HOST_BRIDGE(bridge)->bus), &error_fatal);
    s->ufs = ufs;
    /* PCI is only QEMU's transport plumbing. The unchanged firmware sees
     * its stock non-PCI UFS MMIO address and uses its actual HC protocol. */
    memory_region_init_alias(&s->ufs_window, OBJECT(machine),
        "t70.ufs-standard-registers",
        sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 1), 0x11270000, 0x1000);
    memory_region_add_subregion_overlap(get_system_memory(), 0x11270000,
                                       &s->ufs_window, 1);
}

static void t70_reset(MachineState *machine, ResetType type)
{
    T70Test *s = ((T70MachineState *)machine)->board;
    PCIDevice *pci;
    uint32_t value;
    qemu_devices_reset(type);
    if (s != NULL && s->usb_board && type != RESET_TYPE_SNAPSHOT_LOAD) {
        t70_usb_board_reset(s);
    }
    if (s == NULL || s->ufs == NULL || type == RESET_TYPE_SNAPSHOT_LOAD) {
        return;
    }
    /* Stock T70 geometry: two boot LUs plus an HPB-enabled user LU.
     * Configure the virtual descriptors before guest execution; no HPB cache
     * or MediaTek-specific hardware behavior is emulated. */
    UfsHc *ufs = UFS(s->ufs);
    /* The T70 handoff is defined for the board's UFSHCI 2.x register set. */
    ufs->reg.ver = 0x0210;
    for (unsigned lun = 0; lun < 3; lun++) {
        if (ufs->lus[lun]) {
            ufs->lus[lun]->unit_desc.lu_enable = lun == 2 ? 2 : 1;
            ufs->lus[lun]->unit_desc.boot_lun_id = lun < 2 ? lun + 1 : 0;
        }
    }
    pci = PCI_DEVICE(s->ufs);
    pci_default_write_config(pci, PCI_BASE_ADDRESS_0, 0x11270000, 4);
    pci_default_write_config(pci, PCI_COMMAND,
                             PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER, 2);
    /* Explicit inherited-clock/VEMC/link fixtures. This does not emulate
     * MediaTek PHY, secure monitor or physical cold power sequencing. */
    s->pmic[0x1b1c] = 1;
    s->pmic[0x1e38] = 0x500;
    value = cpu_to_le32(0x10000000);
    memcpy(s->registers + 0x1272240, &value, sizeof(value));
    value = cpu_to_le32(1);
    address_space_write(&address_space_memory, 0x11270034,
                         MEMTXATTRS_UNSPECIFIED, &value, sizeof(value));
    value = cpu_to_le32(0x16); /* standard UIC DME_LINK_STARTUP */
    address_space_write(&address_space_memory, 0x11270090,
                         MEMTXATTRS_UNSPECIFIED, &value, sizeof(value));
    value = cpu_to_le32(0xffffffff);
    address_space_write(&address_space_memory, 0x11270020,
                         MEMTXATTRS_UNSPECIFIED, &value, sizeof(value));
}

static void t70_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    ARMCPU *cpu = ARM_CPU(object_new(machine->cpu_type));
    DeviceState *cpudev = DEVICE(cpu);
    DeviceState *gic = qdev_new("arm-gicv3");
    SysBusDevice *gicbus = SYS_BUS_DEVICE(gic);
    QList *redists = qlist_new();
    T70Test *s = g_new0(T70Test, 1);
    const int timer_irqs[] = {30, 27, 26, 29, 28, 20, 19};
    ((T70MachineState *)machine)->board = s;
    s->usb_board = ((T70MachineState *)machine)->usb_board;
    s->usb_board_cold_attach = ((T70MachineState *)machine)->usb_board_cold_attach;
    object_property_set_bool(OBJECT(cpu), "has_el3", false, &error_fatal);
    object_property_set_bool(OBJECT(cpu), "has_el2", true, &error_fatal);
    object_property_set_int(OBJECT(cpu), "cntfrq", 13000000, &error_fatal);
    qdev_realize_and_unref(cpudev, NULL, &error_fatal);

    qdev_prop_set_uint32(gic, "revision", 3);
    qdev_prop_set_uint32(gic, "num-cpu", 1);
    qdev_prop_set_uint32(gic, "num-irq", 512);
    qdev_prop_set_bit(gic, "has-security-extensions", false);
    qlist_append_int(redists, 1);
    qdev_prop_set_array(gic, "redist-region-count", redists);
    sysbus_realize_and_unref(gicbus, &error_fatal);
    sysbus_mmio_map(gicbus, 0, 0x0c000000);
    sysbus_mmio_map(gicbus, 1, 0x0c040000);
    for (unsigned i = 0; i < ARRAY_SIZE(timer_irqs); ++i) {
        qdev_connect_gpio_out(cpudev, i, qdev_get_gpio_in(gic, 480 + timer_irqs[i]));
    }
    qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0,
                               qdev_get_gpio_in(gic, 480 + 25));
    sysbus_connect_irq(gicbus, 0, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
    sysbus_connect_irq(gicbus, 1, qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
    sysbus_connect_irq(gicbus, 2, qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
    sysbus_connect_irq(gicbus, 3, qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    sysbus_connect_irq(gicbus, 4, qdev_get_gpio_in(cpudev, ARM_CPU_NMI));
    sysbus_connect_irq(gicbus, 5, qdev_get_gpio_in(cpudev, ARM_CPU_VINMI));

    memory_region_add_subregion(sysmem, 0x40000000, machine->ram);
    s->ram = machine->ram;
    memory_region_set_log(s->ram, true, DIRTY_MEMORY_VGA);
    s->registers = g_malloc0(0x05000000);
    s->registers[0x26000] = 1; /* available PWRAP fixture */
    s->pmic[0x28] = 0xa;     /* PMIC presence fixture */
    /* Valid 3.3-V selectors, rails initially disabled. The actual PMIC
     * driver still reads, configures and enables these modeled registers. */
    s->pmic[0x1e48] = 0x500; /* VMCH */
    s->pmic[0x1e4c] = 0xd00; /* VMC */
    memset(s->registers + 0x10004, 0xff, 10); /* all keypad keys released */
    memory_region_init_io(&s->mmio, OBJECT(machine), &t70_ops, s,
                          "t70.explicit-supplier-fixtures", 0x05000000);
    memory_region_add_subregion(sysmem, 0x10000000, &s->mmio);
    if (((T70MachineState *)machine)->ufs) {
        t70_ufs_init(machine, s, gic);
    }
    s->msdc = qdev_new("t70-msdc");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->msdc), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->msdc), 0, 0x11240000, 1);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->msdc), 0,
                       qdev_get_gpio_in(gic, 167 - 32));
    DeviceState *musb = qdev_new("t70-musb");
    qdev_prop_set_bit(musb, "inherited-quiescent", s->usb_board &&
                     !((T70MachineState *)machine)->usb_board_active);
    qdev_prop_set_bit(musb, "cold-attach", s->usb_board_cold_attach);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(musb), &error_fatal);
    s->usb_vbus = qdev_get_gpio_in_named(musb, "vbus", 0);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(musb), 0, 0x11200000, 1);
    sysbus_connect_irq(SYS_BUS_DEVICE(musb), 0, qdev_get_gpio_in(gic, 160 - 32));
}

static void t70_machine_init(MachineClass *mc)
{
    mc->desc = "T70Pro firmware implementation test (explicit MMIO fixtures)";
    mc->init = t70_init;
    mc->reset = t70_reset;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a76");
    mc->default_ram_id = "t70.test.ram";
    mc->default_ram_size = 256 * MiB;
    mc->max_cpus = 1;
    object_class_property_add_bool(OBJECT_CLASS(mc), "usb-board-active",
                                   t70_get_usb_board_active, t70_set_usb_board_active);
    object_class_property_add_bool(OBJECT_CLASS(mc), "usb-board",
                                   t70_get_usb_board, t70_set_usb_board);
    object_class_property_add_bool(OBJECT_CLASS(mc), "usb-board-cold-attach",
                                   t70_get_usb_board_cold_attach,
                                   t70_set_usb_board_cold_attach);
    object_class_property_add_bool(OBJECT_CLASS(mc), "ufs",
                                   t70_get_ufs, t70_set_ufs);
}
DEFINE_MACHINE_EXTENDED("t70-test", MACHINE, T70MachineState,
                        t70_machine_init, false, aarch64_machine_interfaces)
