/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Deterministic retained-source/cold-attach fixtures, not an MT6789 electrical model.
 * Executes the guest's real I2C controller/SC8989x/HUSB311 reads and USB
 * readiness producer. No ready protocol is installed by the test harness.
 * Only the <= 16-byte PIO transactions used by power status are modeled.
 * Cold attach is a named chip-register scenario: its TCPC event is cleared
 * only when guest code performs the real W1C I2C write. */
#define T70_I2C 0x101a000
#define T70_DMA 0x217480

static uint32_t t70_reg(T70Test *s, hwaddr off)
{
    return ldl_le_p(s->registers + off);
}

static void t70_put(T70Test *s, hwaddr off, uint32_t value)
{
    stl_le_p(s->registers + off, value);
}

static bool t70_usb_board_stats_read(const uint32_t *words, unsigned count,
                                     hwaddr base, hwaddr off, unsigned size,
                                     uint64_t *value)
{
    hwaddr byte;
    if (off < base || size == 0 || size > 8) {
        return false;
    }
    byte = off - base;
    if (byte > (hwaddr)count * 4 || size > (hwaddr)count * 4 - byte) {
        return false;
    }
    *value = 0;
    for (unsigned i = 0; i < size; i++) {
        hwaddr at = byte + i;
        uint8_t part = (uint8_t)(words[at / 4] >> ((at % 4) * 8));
        *value |= (uint64_t)part << (i * 8);
    }
    return true;
}

static bool t70_usb_board_read(T70Test *s, hwaddr off, unsigned size,
                                uint64_t *value)
{
    if (off == T70_I2C && size == 1) {
        *value = s->fifo_pos < s->fifo_n ? s->i2c_fifo[s->fifo_pos++] : 0;
        return true;
    }
    if (off >= T70_I2C + 0xf00 && off < T70_I2C + 0xf10) {
        const uint32_t stats[] = {s->i2c_starts, s->chip_writes,
                                 s->i2c_nacks, s->bad_i2c_width};
        return t70_usb_board_stats_read(stats, ARRAY_SIZE(stats),
            T70_I2C + 0xf00, off, size, value);
    }
    if (off >= T70_I2C + 0xf10 && off < T70_I2C + 0xf84) {
        const uint32_t cold[] = {
            s->usb_board_cold_attach,
            s->tcpc98_event_reads, s->tcpc98_ack_writes,
            s->tcpc98_confirm_reads, s->sc03_boost_on_writes,
            s->sc03_boost_off_writes, s->tcpc1a_open_writes,
            s->tcpc1a_source_writes, s->tcpc_power_present_transitions,
            s->tcpc_vsafe0_clear_transitions,
            s->tcpc_vsafe0_reassert_transitions,
            s->tcpc98_first_event_value, s->tcpc98_last_ack_value,
            s->tcpc98_last_confirm_value, s->tcpc[0x98], s->tcpc[0x1e],
            s->tcpc[0x97], s->sc[0x03], s->tcpc[0x1a], s->tcpc[0x1d],
            s->tcpc1e_safe_reads, s->tcpc1e_present_reads,
            s->tcpc97_safe_reads, s->tcpc97_clear_reads,
            s->sc03_boost_on_reads, s->sc14_reads, s->sc14_last_read,
            s->sc[0x0a], s->sc0a_at_boost
        };
        return t70_usb_board_stats_read(cold, ARRAY_SIZE(cold),
            T70_I2C + 0xf10, off, size, value);
    }
    return false;
}

static void t70_i2c_start(T70Test *s, uint32_t start)
{
    unsigned address = t70_reg(s, T70_I2C + 0x94) >> 1;
    bool read = t70_reg(s, T70_I2C + 0x94) & 1;
    unsigned length = t70_reg(s, T70_I2C + 0x14);
    unsigned aux = t70_reg(s, T70_I2C + 0x44);
    uint8_t *chip = address == 0x4e ? s->tcpc : address == 0x6a ? s->sc : NULL;
    s->i2c_starts++;
    if (chip == NULL || address >= 128 || length > 16 || aux > 16 ||
        (!read && s->fifo_n != length)) {
        s->i2c_nacks++;
        t70_put(s, T70_I2C + 0x0c, 2); /* ACKERR */
        t70_put(s, T70_I2C + 0x24, 0);
        return;
    }
    if (!read) {
        s->pointer[address] = s->i2c_fifo[0];
        for (unsigned i = 1; i < length; i++) {
            unsigned reg = s->pointer[address]++;
            uint8_t data = s->i2c_fifo[i];
            uint8_t before = chip[reg];
            if (s->usb_board_cold_attach && address == 0x4e && reg == 0x98) {
                /* HUSB311 INT_STATUS is W1C; do not clear it in the fixture
                 * until the production I2C client writes the event bit. */
                chip[reg] = (uint8_t)(before & (uint8_t)~data);
                if (data & 0x02U) {
                    s->tcpc98_ack_writes++;
                    s->tcpc98_last_ack_value = data;
                }
            } else {
                chip[reg] = data;
            }
            if (s->usb_board_cold_attach && address == 0x4e && reg == 0x1a) {
                /* The Rp level does not affect either open CC pin. */
                if ((data & 0x4fU) == 0x0fU) {
                    s->tcpc1a_open_writes++;
                    s->tcpc[0x1d] = 0x00;
                } else if (data == 0x05U) {
                    s->tcpc1a_source_writes++;
                    s->tcpc[0x1d] = 0x02; /* CC1 Rd with source Rp */
                }
            }
            if (s->usb_board_cold_attach && address == 0x6a && reg == 0x03) {
                bool was_on = (before & 0x20U) != 0;
                bool is_on = (data & 0x20U) != 0;
                qemu_set_irq(s->usb_vbus, is_on);
                if (!was_on && is_on) {
                    s->sc03_boost_on_writes++;
                    s->sc0a_at_boost = s->sc[0x0a];
                    if (s->tcpc[0x1e] != 0x0cU) {
                        s->tcpc[0x1e] = 0x0cU; /* POWER_STATUS: VBUS present */
                        s->tcpc_power_present_transitions++;
                    }
                    if (s->tcpc[0x97] != 0x00U) {
                        s->tcpc[0x97] = 0x00U; /* vSafe0V deasserted */
                        s->tcpc_vsafe0_clear_transitions++;
                    }
                } else if (was_on && !is_on) {
                    s->sc03_boost_off_writes++;
                    if (s->tcpc[0x1e] != 0x08U) {
                        s->tcpc[0x1e] = 0x08U;
                    }
                    if (s->tcpc[0x97] != 0x02U) {
                        s->tcpc[0x97] = 0x02U; /* vSafe0V asserted */
                        s->tcpc_vsafe0_reassert_transitions++;
                    }
                }
            }
            s->chip_writes++;
        }
    }
    s->fifo_n = s->fifo_pos = 0;
    if (read || aux) {
        for (unsigned i = 0; i < (read ? length : aux); i++) {
            unsigned reg = s->pointer[address]++;
            uint8_t data = chip[reg];
            s->i2c_fifo[s->fifo_n++] = data;
            if (s->usb_board_cold_attach && address == 0x4e && reg == 0x98) {
                if (s->tcpc98_ack_writes != 0U) {
                    s->tcpc98_confirm_reads++;
                    s->tcpc98_last_confirm_value = data;
                } else if (data & 0x02U) {
                    s->tcpc98_event_reads++;
                    s->tcpc98_first_event_value = data;
                }
            }
            if (s->usb_board_cold_attach && address == 0x4e && reg == 0x1e) {
                if (data == 0x08U) s->tcpc1e_safe_reads++;
                if (data == 0x0cU) s->tcpc1e_present_reads++;
            }
            if (s->usb_board_cold_attach && address == 0x4e && reg == 0x97) {
                if (data == 0x02U) s->tcpc97_safe_reads++;
                if (data == 0x00U && s->sc03_boost_on_writes != 0U) {
                    s->tcpc97_clear_reads++;
                }
            }
            if (s->usb_board_cold_attach && address == 0x6a && reg == 0x03 &&
                (data & 0x20U) != 0U && s->sc03_boost_on_writes != 0U) {
                s->sc03_boost_on_reads++;
            }
            if (s->usb_board_cold_attach && address == 0x6a && reg == 0x14) {
                s->sc14_reads++;
                s->sc14_last_read = data;
            }
        }
    }
    /* A repeated START pauses the bus until the next message; final STOP
     * releases START before the production driver's completion check. */
    t70_put(s, T70_I2C + 0x0c, (start & 0x8000) ? 16 : 1);
    t70_put(s, T70_I2C + 0x24, (start & 0x8000) ? 1 : 0);
}

/* GPIO SET/CLR/MWR aliases belong to the shared digital controller, even
 * when no Type-C chip fixture is enabled. This does not model pad voltages. */
static bool t70_gpio_write(T70Test *s, hwaddr off, uint64_t value, unsigned size)
{
    if (size == 4 &&
        off >= 0x5000 && off < 0x5600) {
        hwaddr base = off & ~(hwaddr)0xf;
        unsigned alias = off & 0xf;
        if ((base >= 0x5000 && base < 0x5080) ||
            (base >= 0x5100 && base < 0x5180)) {
            uint32_t old = t70_reg(s, base), next;
            unsigned bank = base & 0xf0;
            if (alias == 0) next = value;
            else if (alias == 4) next = old | value;
            else if (alias == 8) next = old & ~value;
            else return false;
            t70_put(s, base, next);
            /* Explicit digital GPIO fixture: output DIN follows DOUT;
             * external inputs, including ID, retain their stimulus value. */
            uint32_t dir = t70_reg(s, 0x5000 + bank);
            uint32_t din = t70_reg(s, 0x5200 + bank);
            uint32_t dout = t70_reg(s, 0x5100 + bank);
            t70_put(s, 0x5200 + bank, (din & ~dir) | (dout & dir));
            return true;
        }
        if (base >= 0x5300 && base < 0x5480 && alias == 0xc) {
            uint32_t next = t70_reg(s, base);
            for (unsigned shift = 0; shift < 32; shift += 4) {
                if (value & (8U << shift)) {
                    next = (next & ~(0xfU << shift)) | (value & (7U << shift));
                }
            }
            t70_put(s, base, next);
            return true;
        }
    }
    return false;
}

static bool t70_usb_board_write(T70Test *s, hwaddr off, uint64_t value,
                                 unsigned size)
{
    if (off >= T70_I2C && off < T70_I2C + 0x100) {
        if (off == T70_I2C && size == 1) {
            if (s->fifo_n < sizeof(s->i2c_fifo)) {
                s->i2c_fifo[s->fifo_n++] = value;
            }
            return true;
        }
        if (size != 4) {
            s->bad_i2c_width++;
            return true;
        }
        if (off == T70_I2C + 0x0c) {
            t70_put(s, off, t70_reg(s, off) & ~value); /* W1C */
        } else {
            t70_put(s, off, value);
        }
        if (off == T70_I2C + 0x38 && (value & 1)) {
            s->fifo_n = s->fifo_pos = 0;
        }
        if (off == T70_I2C + 0x24 && (value & 1)) {
            t70_i2c_start(s, value);
        }
        return true;
    }
    if (off == T70_DMA + 0x0c && size == 4) {
        t70_put(s, off, value & ~1U); /* modeled DMA reset completion */
        t70_put(s, T70_DMA + 8, 0);
        return true;
    }
    return false;
}

static void t70_usb_board_reset(T70Test *s)
{
    const unsigned high[] = {4, 48, 86, 99, 104};
    s->tcpc98_event_reads = s->tcpc98_ack_writes = s->tcpc98_confirm_reads = 0;
    s->sc03_boost_on_writes = s->sc03_boost_off_writes = 0;
    s->tcpc1a_open_writes = s->tcpc1a_source_writes = 0;
    s->tcpc_power_present_transitions = 0;
    s->tcpc_vsafe0_clear_transitions = s->tcpc_vsafe0_reassert_transitions = 0;
    s->tcpc1e_safe_reads = s->tcpc1e_present_reads = 0;
    s->tcpc97_safe_reads = s->tcpc97_clear_reads = s->sc03_boost_on_reads = 0;
    s->sc14_reads = 0;
    s->sc14_last_read = 0;
    s->tcpc98_first_event_value = s->tcpc98_last_ack_value = 0;
    s->tcpc98_last_confirm_value = 0;
    memset(s->sc, 0, sizeof(s->sc));
    memset(s->tcpc, 0, sizeof(s->tcpc));
    s->sc[0] = 0x80;
    s->sc[3] = s->usb_board_cold_attach ? 0x1a : 0x30;
    /* Cold firmware must apply the board selectors without losing bit 3. */
    s->sc[0xa] = s->usb_board_cold_attach ? 0x7b : 0xc3;
    s->sc[0x14] = s->usb_board_cold_attach ? 0x64 : 0x10;
    /* The named cold fixture begins with boost off and vSafe0V asserted.
     * The retained-source baseline below remains byte-for-byte unchanged. */
    s->tcpc[0] = 0x99; s->tcpc[1] = 0x2e;
    s->tcpc[2] = 0x11; s->tcpc[3] = 3;
    s->tcpc[0x1a] = 0x05; s->tcpc[0x1d] = 0x02;
    s->tcpc[0x1e] = s->usb_board_cold_attach ? 0x08 : 0x0c;
    if (s->usb_board_cold_attach) {
        s->tcpc[0x97] = 0x02; /* vSafe0V */
        s->tcpc[0x98] = 0x02; /* cold attach event, cleared by guest W1C */
    }
    for (unsigned i = 0; i < ARRAY_SIZE(high); i++) {
        unsigned bank = high[i] / 32 * 16;
        unsigned bit = 1U << (high[i] % 32);
        t70_put(s, 0x5000 + bank, t70_reg(s, 0x5000 + bank) | bit);
        if (!s->usb_board_cold_attach || high[i] == 104U) {
            t70_put(s, 0x5100 + bank, t70_reg(s, 0x5100 + bank) | bit);
            t70_put(s, 0x5200 + bank, t70_reg(s, 0x5200 + bank) | bit);
        }
    }
    if (s->usb_board_cold_attach) {
        /* Type-C-only attachment: ID is an input HIGH, inactive hub route
         * stays LOW, inherited hub reset HIGH, charger outputs initially LOW. */
        t70_put(s, 0x5000, t70_reg(s, 0x5000) & ~(1U << 10));
        t70_put(s, 0x5200, t70_reg(s, 0x5200) | (1U << 10));
    }
    t70_put(s, 0x5410, 0x11000000); /* I2C6 pin 142/143 function 1 */
    t70_put(s, 0x5240, 3U << 14); /* explicit SCL/SDA pulled high */
    /* Synthetic 2496 MHz PCW for cold preparation; keep retained baseline. */
    t70_put(s, 0xc3c4, s->usb_board_cold_attach ? 0x00980000 : 0x49d89);
    t70_put(s, 0xc3cc, 5);
    t70_put(s, 0x1f40304, 0x4400); t70_put(s, 0x1f40318, 0x50100075);
    t70_put(s, 0x1f40368, 0); t70_put(s, 0x1f4036c, 0x224);
}
