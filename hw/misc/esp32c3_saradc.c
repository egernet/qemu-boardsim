/*
 * ESP32-C3 APB SAR ADC (minimal)
 *
 * Copyright (c) 2026 Christian Skaarup Enevoldsen
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/misc/esp32c3_saradc.h"

#define A_SAR1_DATA_STATUS  0x02C
#define A_SAR2_DATA_STATUS  0x030
#define A_INT_ENA           0x040
#define A_INT_RAW           0x044
#define A_INT_ST            0x048
#define A_INT_CLR           0x04C
#define A_DATE              0x3FC

#define INT_ADC_DONE        ((1u << 31) | (1u << 30))

static uint64_t saradc_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32C3SarAdcState *s = ESP32C3_SARADC(opaque);
    switch (addr) {
    case A_SAR1_DATA_STATUS:
    case A_SAR2_DATA_STATUS:
        return s->adc_value & 0xfff;     /* channel field 0: a valid result */
    case A_INT_RAW:
        return s->regs[A_INT_RAW / 4] | INT_ADC_DONE;
    case A_INT_ST:
        return (s->regs[A_INT_RAW / 4] | INT_ADC_DONE) & s->regs[A_INT_ENA / 4];
    case A_DATE:
        return 0x2007171;
    default:
        return addr < sizeof(s->regs) ? s->regs[addr / 4] : 0;
    }
}

static void saradc_write(void *opaque, hwaddr addr, uint64_t value, unsigned int size)
{
    ESP32C3SarAdcState *s = ESP32C3_SARADC(opaque);
    if (addr == A_INT_CLR) {
        s->regs[A_INT_RAW / 4] &= ~(uint32_t)value;
    } else if (addr < sizeof(s->regs)) {
        s->regs[addr / 4] = value;
    }
}

static const MemoryRegionOps saradc_ops = {
    .read = saradc_read,
    .write = saradc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void saradc_reset(DeviceState *dev)
{
    ESP32C3SarAdcState *s = ESP32C3_SARADC(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static void saradc_init(Object *obj)
{
    ESP32C3SarAdcState *s = ESP32C3_SARADC(obj);
    memory_region_init_io(&s->iomem, obj, &saradc_ops, s, TYPE_ESP32C3_SARADC, 0x400);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static Property saradc_properties[] = {
    DEFINE_PROP_UINT32("adc_value", ESP32C3SarAdcState, adc_value, 2048),
    DEFINE_PROP_END_OF_LIST(),
};

static void saradc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->reset = saradc_reset;
    device_class_set_props(dc, saradc_properties);
}

static const TypeInfo saradc_info = {
    .name = TYPE_ESP32C3_SARADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32C3SarAdcState),
    .instance_init = saradc_init,
    .class_init = saradc_class_init,
};

static void saradc_register_types(void)
{
    type_register_static(&saradc_info);
}

type_init(saradc_register_types)
