#pragma once

#include "hw/sysbus.h"
#include "hw/hw.h"

#define TYPE_ESP32C3_SARADC "esp32c3.saradc"
#define ESP32C3_SARADC(obj) OBJECT_CHECK(ESP32C3SarAdcState, (obj), TYPE_ESP32C3_SARADC)

/*
 * Minimal APB SAR ADC: registers keep what is written, a one-shot
 * conversion is done at once and returns `adc_value` (12 bits). Enough for
 * ESP-IDF's ADC calibration at start-up and for one-shot reads.
 */
typedef struct ESP32C3SarAdcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[256];
    uint32_t adc_value;
} ESP32C3SarAdcState;
