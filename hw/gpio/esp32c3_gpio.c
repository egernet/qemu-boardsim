/*
 * ESP32-C3 GPIO emulation
 *
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 * Output, enable and GPIO matrix registers, and the BoardSim pin bridge:
 * Copyright (c) 2026 Christian Skaarup Enevoldsen
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/gpio/esp32c3_gpio.h"
#include "sysemu/runstate.h"
#include "qemu/timer.h"

#define A_GPIO_OUT          0x0004
#define A_GPIO_OUT_W1TS     0x0008
#define A_GPIO_OUT_W1TC     0x000C
#define A_GPIO_ENABLE       0x0020
#define A_GPIO_ENABLE_W1TS  0x0024
#define A_GPIO_ENABLE_W1TC  0x0028
#define A_GPIO_STRAP_C3     0x0038
#define A_GPIO_IN           0x003C
#define A_GPIO_PIN0         0x0074
#define A_GPIO_FUNC0_IN     0x0154
#define A_GPIO_FUNC0_OUT    0x0554

#define PIN_MASK ((1u << ESP32C3_GPIO_COUNT) - 1)

static void gpio_send(ESP32C3GPIOState *s, const char *line)
{
    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_write_all(&s->chr, (const uint8_t *)line, strlen(line));
    }
}

static void gpio_report_pins(ESP32C3GPIOState *s)
{
    if (s->sent_any && s->sent_out == s->out && s->sent_enable == s->enable) {
        return;
    }
    char line[64];
    snprintf(line, sizeof(line), "O %x %x %" PRId64 "\n", s->out & PIN_MASK, s->enable & PIN_MASK,
             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    gpio_send(s, line);
    s->sent_out = s->out;
    s->sent_enable = s->enable;
    s->sent_any = true;
}

void esp32c3_gpio_report_rmt(ESP32C3GPIOState *s, int channel, uint32_t tick_ps,
                             const uint32_t *items, int count)
{
    if (!qemu_chr_fe_backend_connected(&s->chr)) {
        return;
    }
    int gpio = -1;
    for (int i = 0; i < ESP32C3_GPIO_COUNT; i++) {
        if ((s->out_sel[i] & 0xff) == ESP32C3_SIG_RMT_OUT0 + channel) {
            gpio = i;
            break;
        }
    }
    GString *str = g_string_sized_new(32 + count * 8);
    g_string_append_printf(str, "R %d %u %d ", gpio, tick_ps, count);
    for (int i = 0; i < count; i++) {
        g_string_append_printf(str, "%08x", items[i]);
    }
    g_string_append_printf(str, " %" PRId64 "\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    qemu_chr_fe_write_all(&s->chr, (const uint8_t *)str->str, str->len);
    g_string_free(str, true);
}

static uint64_t esp32c3_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(opaque);

    if (addr >= A_GPIO_PIN0 && addr < A_GPIO_PIN0 + 4 * ESP32C3_GPIO_COUNT) {
        return s->pin[(addr - A_GPIO_PIN0) / 4];
    }
    if (addr >= A_GPIO_FUNC0_IN && addr < A_GPIO_FUNC0_IN + 4 * ESP32C3_GPIO_IN_SIGNALS) {
        return s->in_sel[(addr - A_GPIO_FUNC0_IN) / 4];
    }
    if (addr >= A_GPIO_FUNC0_OUT && addr < A_GPIO_FUNC0_OUT + 4 * ESP32C3_GPIO_COUNT) {
        return s->out_sel[(addr - A_GPIO_FUNC0_OUT) / 4];
    }

    switch (addr) {
    case A_GPIO_OUT:
    case A_GPIO_OUT_W1TS:
    case A_GPIO_OUT_W1TC:
        return s->out;
    case A_GPIO_ENABLE:
    case A_GPIO_ENABLE_W1TS:
    case A_GPIO_ENABLE_W1TC:
        return s->enable;
    case A_GPIO_STRAP_C3:
        return s->strap_mode;
    case A_GPIO_IN:
        /* A pin driven by the chip reads its own level; the rest read the host's levels. */
        return ((s->out & s->enable) | (s->in_ext & ~s->enable)) & PIN_MASK;
    default:
        return 0;
    }
}

static void esp32c3_gpio_write(void *opaque, hwaddr addr, uint64_t value, unsigned int size)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(opaque);
    uint32_t v = value;

    if (addr >= A_GPIO_PIN0 && addr < A_GPIO_PIN0 + 4 * ESP32C3_GPIO_COUNT) {
        s->pin[(addr - A_GPIO_PIN0) / 4] = v;
        return;
    }
    if (addr >= A_GPIO_FUNC0_IN && addr < A_GPIO_FUNC0_IN + 4 * ESP32C3_GPIO_IN_SIGNALS) {
        s->in_sel[(addr - A_GPIO_FUNC0_IN) / 4] = v;
        return;
    }
    if (addr >= A_GPIO_FUNC0_OUT && addr < A_GPIO_FUNC0_OUT + 4 * ESP32C3_GPIO_COUNT) {
        int gpio = (addr - A_GPIO_FUNC0_OUT) / 4;
        if ((s->out_sel[gpio] & 0xff) != (v & 0xff)) {
            char line[32];
            snprintf(line, sizeof(line), "M %d %u\n", gpio, v & 0xff);
            gpio_send(s, line);
        }
        s->out_sel[gpio] = v;
        return;
    }

    switch (addr) {
    case A_GPIO_OUT:         s->out = v; break;
    case A_GPIO_OUT_W1TS:    s->out |= v; break;
    case A_GPIO_OUT_W1TC:    s->out &= ~v; break;
    case A_GPIO_ENABLE:      s->enable = v; break;
    case A_GPIO_ENABLE_W1TS: s->enable |= v; break;
    case A_GPIO_ENABLE_W1TC: s->enable &= ~v; break;
    default: return;
    }
    gpio_report_pins(s);
}

static const MemoryRegionOps esp32c3_gpio_ops = {
    .read = esp32c3_gpio_read,
    .write = esp32c3_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static int gpio_can_receive(void *opaque)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(opaque);
    return sizeof(s->rx) - s->rx_len;
}

static void gpio_receive(void *opaque, const uint8_t *buf, int size)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(opaque);
    for (int i = 0; i < size; i++) {
        char c = buf[i];
        if (c == '\n' || s->rx_len == sizeof(s->rx) - 1) {
            s->rx[s->rx_len] = 0;
            if (s->rx[0] == 'I' && s->rx[1] == ' ') {
                s->in_ext = strtoul(s->rx + 2, NULL, 16) & PIN_MASK;
            } else if (s->rx[0] == 'S' && s->rx[1] == ' ') {
                /* Strapping pins as sampled at the next reset (0x08 flash boot, 0x02 UART download). */
                s->strap_mode = strtoul(s->rx + 2, NULL, 16);
            } else if (s->rx[0] == 'H' && s->rx[1] == ' ') {
                /* CHIP_EN: held low stops the chip; released, it boots again. */
                if (s->rx[2] == '1') {
                    if (runstate_is_running()) {
                        vm_stop(RUN_STATE_PAUSED);
                    }
                } else {
                    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
                    if (!runstate_is_running()) {
                        vm_start();
                    }
                }
            } else if (s->rx[0] == 'X') {
                qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            }
            s->rx_len = 0;
        } else if (c != '\r') {
            s->rx[s->rx_len++] = c;
        }
    }
}

static void esp32c3_gpio_reset(DeviceState *dev)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(dev);
    s->out = 0;
    s->enable = 0;
    memset(s->pin, 0, sizeof(s->pin));
    memset(s->in_sel, 0, sizeof(s->in_sel));
    for (int i = 0; i < ESP32C3_GPIO_COUNT; i++) {
        s->out_sel[i] = ESP32C3_SIG_GPIO_OUT;
    }
    s->sent_any = false;
    s->rx_len = 0;
    gpio_send(s, "B\n");
    gpio_report_pins(s);
}

static void esp32c3_gpio_realize(DeviceState *dev, Error **errp)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(dev);
    qemu_chr_fe_set_handlers(&s->chr, gpio_can_receive, gpio_receive, NULL, NULL, s, NULL, true);
}

static void esp32c3_gpio_init(Object *obj)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32c3_gpio_ops, s, TYPE_ESP32C3_GPIO, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static Property esp32c3_gpio_properties[] = {
    DEFINE_PROP_UINT32("strap_mode", ESP32C3GPIOState, strap_mode, ESP32C3_STRAP_MODE_FLASH_BOOT),
    DEFINE_PROP_CHR("chardev", ESP32C3GPIOState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void esp32c3_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = esp32c3_gpio_reset;
    dc->realize = esp32c3_gpio_realize;
    device_class_set_props(dc, esp32c3_gpio_properties);
}

static const TypeInfo esp32c3_gpio_info = {
    .name = TYPE_ESP32C3_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32C3GPIOState),
    .instance_init = esp32c3_gpio_init,
    .class_init = esp32c3_gpio_class_init,
    .class_size = sizeof(ESP32C3GPIOClass),
};

static void esp32c3_gpio_register_types(void)
{
    type_register_static(&esp32c3_gpio_info);
}

type_init(esp32c3_gpio_register_types)
