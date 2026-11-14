/*
 * ESP32-C3 RMT (remote control) transmitter emulation
 *
 * Copyright (c) 2026 Christian Skaarup Enevoldsen
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/misc/esp32c3_rmt.h"

/* Register offsets (soc/rmt_reg.h) */
#define A_CH0DATA      0x000
#define A_CH0CONF0     0x010
#define A_CH1CONF0     0x014
#define A_CH2CONF0     0x018
#define A_CH3CONF1     0x024
#define A_CH0STATUS    0x028
#define A_CH3STATUS    0x034
#define A_INT_RAW      0x038
#define A_INT_ST       0x03C
#define A_INT_ENA      0x040
#define A_INT_CLR      0x044
#define A_CH0CARRIER   0x048
#define A_CH3CARRIER   0x054
#define A_CH0_TX_LIM   0x058
#define A_CH1_TX_LIM   0x05C
#define A_CH2_RX_LIM   0x060
#define A_CH3_RX_LIM   0x064
#define A_SYS_CONF     0x068
#define A_REF_CNT_RST  0x070
#define A_DATE         0x0CC
#define A_RAM          0x400
#define RAM_WORDS      (ESP32C3_RMT_CHANNELS * ESP32C3_RMT_BLOCK_WORDS)

/* CHnCONF0 bits */
#define CONF0_TX_START      (1u << 0)
#define CONF0_MEM_RD_RST    (1u << 1)
#define CONF0_MEM_RST       (1u << 2)
#define CONF0_TX_CONTI      (1u << 3)
#define CONF0_WRAP_EN       (1u << 4)
#define CONF0_TX_STOP       (1u << 7)
#define CONF0_DIV(v)        (((v) >> 8) & 0xff)
#define CONF0_MEM_SIZE(v)   (((v) >> 16) & 0x7)
#define CONF0_AFIFO_RST     (1u << 23)
#define CONF0_CONF_UPDATE   (1u << 24)
#define CONF0_PULSE_BITS    (CONF0_TX_START | CONF0_MEM_RD_RST | CONF0_MEM_RST | CONF0_TX_STOP | \
                             CONF0_AFIFO_RST | CONF0_CONF_UPDATE)

/* Interrupt bits */
#define INT_TX_END(ch)      (1u << (ch))
#define INT_TX_THR(ch)      (1u << (8 + (ch)))

/* Hold back long runs so the CPU gets time to refill RAM between events. */
#define MAX_ITEMS_PER_STEP  ESP32C3_RMT_BLOCK_WORDS

static void rmt_update_irq(ESP32C3RmtState *s)
{
    qemu_set_irq(s->irq, (s->int_raw & s->int_ena) != 0);
}

/* Length of one RMT tick in picoseconds for a TX channel. */
static uint32_t rmt_tick_ps(ESP32C3RmtState *s, int ch)
{
    uint32_t sclk_sel = (s->sys_conf >> 24) & 0x3;
    uint32_t sclk_div = ((s->sys_conf >> 4) & 0xff) + 1;
    uint64_t src_hz = sclk_sel == 2 ? 17500000 : sclk_sel == 3 ? 40000000 : 80000000;
    uint32_t div = CONF0_DIV(s->conf0[ch]);
    if (div == 0) {
        div = 256;
    }
    return (uint32_t)(1000000000000ULL * sclk_div * div / src_hz);
}

static uint32_t rmt_span(ESP32C3RmtState *s, int ch)
{
    uint32_t blocks = CONF0_MEM_SIZE(s->conf0[ch]);
    if (blocks == 0) {
        blocks = 1;
    }
    uint32_t max = ESP32C3_RMT_CHANNELS - ch;
    return MIN(blocks, max) * ESP32C3_RMT_BLOCK_WORDS;
}

static void rmt_append(ESP32C3RmtTx *t, uint32_t item)
{
    if (t->count < ESP32C3_RMT_MAX_ITEMS) {
        t->items[t->count++] = item;
    }
}

/*
 * Reads items from the current position up to the next event (threshold,
 * end, or a batch limit), then arms the timer for when the hardware would
 * have sent them.
 */
static void rmt_step(ESP32C3RmtState *s, int ch)
{
    ESP32C3RmtTx *t = &s->tx[ch];
    uint32_t base = ch * ESP32C3_RMT_BLOCK_WORDS;
    uint32_t span = rmt_span(s, ch);
    uint32_t limit = s->tx_lim[ch] & 0x1ff;
    bool wrap = s->conf0[ch] & CONF0_WRAP_EN;
    uint64_t ticks = 0;

    t->pending = 0;
    t->pending_end = false;

    for (int n = 0; n < MAX_ITEMS_PER_STEP; n++) {
        uint32_t item = s->mem[(base + t->rd) % RAM_WORDS];
        uint32_t d0 = item & 0x7fff;
        uint32_t d1 = (item >> 16) & 0x7fff;

        if (d0 == 0) {
            t->pending_end = true;
            break;
        }
        rmt_append(t, item);
        ticks += d0 + d1;
        if (d1 == 0) {
            t->pending_end = true;
            break;
        }

        t->rd++;
        t->since_thr++;
        if (t->rd >= span) {
            if (!wrap) {
                /* Ran off the end without an end marker: the hardware stops too. */
                t->pending_end = true;
                break;
            }
            t->rd = 0;
        }
        if (wrap && limit && t->since_thr >= limit) {
            t->since_thr = 0;
            t->pending |= INT_TX_THR(ch);
            break;
        }
    }

    uint64_t delay_ns = ticks * rmt_tick_ps(s, ch) / 1000;
    timer_mod(t->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MAX(delay_ns, 1));
}

static void rmt_timer_cb(void *opaque);

static void rmt_tx_start(ESP32C3RmtState *s, int ch)
{
    ESP32C3RmtTx *t = &s->tx[ch];
    t->active = true;
    t->since_thr = 0;
    t->count = 0;
    rmt_step(s, ch);
}

static void rmt_tx_finish(ESP32C3RmtState *s, int ch)
{
    ESP32C3RmtTx *t = &s->tx[ch];
    t->active = false;
    if (s->gpio) {
        esp32c3_gpio_report_rmt(s->gpio, ch, rmt_tick_ps(s, ch), t->items, t->count);
    }
    t->count = 0;
    s->int_raw |= INT_TX_END(ch);
    rmt_update_irq(s);
}

typedef struct {
    ESP32C3RmtState *s;
    int ch;
} RmtTimerArg;

static RmtTimerArg rmt_args[ESP32C3_RMT_TX_CHANNELS];

static void rmt_timer_cb(void *opaque)
{
    RmtTimerArg *a = opaque;
    ESP32C3RmtState *s = a->s;
    ESP32C3RmtTx *t = &s->tx[a->ch];
    if (!t->active) {
        return;
    }
    if (t->pending_end) {
        rmt_tx_finish(s, a->ch);
        return;
    }
    if (t->pending) {
        s->int_raw |= t->pending;
        rmt_update_irq(s);
    }
    rmt_step(s, a->ch);
}

static void rmt_write_conf0(ESP32C3RmtState *s, int ch, uint32_t v)
{
    ESP32C3RmtTx *t = &s->tx[ch];
    s->conf0[ch] = v & ~CONF0_PULSE_BITS;
    if (v & (CONF0_MEM_RD_RST | CONF0_MEM_RST)) {
        t->rd = 0;
    }
    if (v & CONF0_TX_STOP) {
        t->active = false;
        timer_del(t->timer);
    }
    if (v & CONF0_TX_START) {
        rmt_tx_start(s, ch);
    }
}

static uint64_t esp32c3_rmt_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32C3RmtState *s = ESP32C3_RMT(opaque);

    if (addr >= A_RAM && addr < A_RAM + RAM_WORDS * 4) {
        return s->mem[(addr - A_RAM) / 4];
    }
    switch (addr) {
    case A_CH0CONF0: return s->conf0[0];
    case A_CH1CONF0: return s->conf0[1];
    case A_CH2CONF0 ... A_CH3CONF1: return s->rx_conf[(addr - A_CH2CONF0) / 4];
    case A_CH0STATUS ... A_CH3STATUS: {
        int ch = (addr - A_CH0STATUS) / 4;
        /* TX status: read address and an idle/busy state field. */
        return ch < ESP32C3_RMT_TX_CHANNELS ? ((s->tx[ch].rd & 0x1ff) | (s->tx[ch].active ? (1 << 9) : 0)) : 0;
    }
    case A_INT_RAW: return s->int_raw;
    case A_INT_ST:  return s->int_raw & s->int_ena;
    case A_INT_ENA: return s->int_ena;
    case A_CH0CARRIER ... A_CH3CARRIER: return s->carrier[(addr - A_CH0CARRIER) / 4];
    case A_CH0_TX_LIM: return s->tx_lim[0];
    case A_CH1_TX_LIM: return s->tx_lim[1];
    case A_CH2_RX_LIM: return s->rx_lim[0];
    case A_CH3_RX_LIM: return s->rx_lim[1];
    case A_SYS_CONF: return s->sys_conf;
    case A_REF_CNT_RST: return s->ref_cnt;
    case A_DATE: return 0x2006231;
    default: return 0;
    }
}

static void esp32c3_rmt_write(void *opaque, hwaddr addr, uint64_t value, unsigned int size)
{
    ESP32C3RmtState *s = ESP32C3_RMT(opaque);
    uint32_t v = value;

    if (addr >= A_RAM && addr < A_RAM + RAM_WORDS * 4) {
        s->mem[(addr - A_RAM) / 4] = v;
        return;
    }
    switch (addr) {
    case A_CH0CONF0: rmt_write_conf0(s, 0, v); break;
    case A_CH1CONF0: rmt_write_conf0(s, 1, v); break;
    case A_CH2CONF0 ... A_CH3CONF1: s->rx_conf[(addr - A_CH2CONF0) / 4] = v; break;
    case A_INT_RAW: break;
    case A_INT_ENA: s->int_ena = v; rmt_update_irq(s); break;
    case A_INT_CLR: s->int_raw &= ~v; rmt_update_irq(s); break;
    case A_CH0CARRIER ... A_CH3CARRIER: s->carrier[(addr - A_CH0CARRIER) / 4] = v; break;
    case A_CH0_TX_LIM: s->tx_lim[0] = v; break;
    case A_CH1_TX_LIM: s->tx_lim[1] = v; break;
    case A_CH2_RX_LIM: s->rx_lim[0] = v; break;
    case A_CH3_RX_LIM: s->rx_lim[1] = v; break;
    case A_SYS_CONF: s->sys_conf = v; break;
    case A_REF_CNT_RST: s->ref_cnt = v; break;
    default:
        qemu_log_mask(LOG_UNIMP, "esp32c3.rmt: unsupported write at 0x%" HWADDR_PRIx "\n", addr);
        break;
    }
}

static const MemoryRegionOps esp32c3_rmt_ops = {
    .read = esp32c3_rmt_read,
    .write = esp32c3_rmt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void esp32c3_rmt_reset(DeviceState *dev)
{
    ESP32C3RmtState *s = ESP32C3_RMT(dev);
    for (int ch = 0; ch < ESP32C3_RMT_TX_CHANNELS; ch++) {
        timer_del(s->tx[ch].timer);
        s->tx[ch].active = false;
        s->tx[ch].rd = 0;
        s->tx[ch].count = 0;
        /* Reset value: divider 2, one memory block. */
        s->conf0[ch] = (2u << 8) | (1u << 16);
        s->tx_lim[ch] = 0x80;
    }
    memset(s->rx_conf, 0, sizeof(s->rx_conf));
    s->int_raw = 0;
    s->int_ena = 0;
    s->sys_conf = (1u << 24) | (1u << 31); /* APB clock, clock enabled */
    rmt_update_irq(s);
}

static void esp32c3_rmt_realize(DeviceState *dev, Error **errp)
{
    ESP32C3RmtState *s = ESP32C3_RMT(dev);
    for (int ch = 0; ch < ESP32C3_RMT_TX_CHANNELS; ch++) {
        rmt_args[ch].s = s;
        rmt_args[ch].ch = ch;
        s->tx[ch].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rmt_timer_cb, &rmt_args[ch]);
        s->tx[ch].items = g_new0(uint32_t, ESP32C3_RMT_MAX_ITEMS);
    }
}

static void esp32c3_rmt_init(Object *obj)
{
    ESP32C3RmtState *s = ESP32C3_RMT(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&s->iomem, obj, &esp32c3_rmt_ops, s, TYPE_ESP32C3_RMT, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void esp32c3_rmt_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->reset = esp32c3_rmt_reset;
    dc->realize = esp32c3_rmt_realize;
}

static const TypeInfo esp32c3_rmt_info = {
    .name = TYPE_ESP32C3_RMT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32C3RmtState),
    .instance_init = esp32c3_rmt_init,
    .class_init = esp32c3_rmt_class_init,
};

static void esp32c3_rmt_register_types(void)
{
    type_register_static(&esp32c3_rmt_info);
}

type_init(esp32c3_rmt_register_types)
