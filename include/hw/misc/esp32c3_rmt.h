#pragma once

#include "hw/sysbus.h"
#include "hw/hw.h"
#include "qemu/timer.h"
#include "hw/gpio/esp32c3_gpio.h"

#define TYPE_ESP32C3_RMT "esp32c3.rmt"
#define ESP32C3_RMT(obj) OBJECT_CHECK(ESP32C3RmtState, (obj), TYPE_ESP32C3_RMT)

#define ESP32C3_RMT_TX_CHANNELS   2
#define ESP32C3_RMT_CHANNELS      4
#define ESP32C3_RMT_BLOCK_WORDS   48
#define ESP32C3_RMT_MAX_ITEMS     (64 * 1024)

typedef struct ESP32C3RmtTx {
    QEMUTimer *timer;
    bool active;
    uint32_t rd;            /* read pointer, in words from the channel's first block */
    uint32_t since_thr;     /* items sent since the last threshold event */
    uint32_t pending;       /* interrupt bits to raise when the timer fires */
    bool pending_end;
    uint32_t *items;        /* everything sent in this transmission */
    int count;
} ESP32C3RmtTx;

/*
 * RMT transmitter: TX channels 0 and 1 read items from RMT RAM in real
 * (virtual) time, raise the threshold and end interrupts the ESP-IDF driver
 * waits for, and report each finished transmission through the GPIO block.
 * The RX channels only keep their registers.
 */
typedef struct ESP32C3RmtState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t conf0[ESP32C3_RMT_TX_CHANNELS];
    uint32_t rx_conf[4];
    uint32_t tx_lim[ESP32C3_RMT_TX_CHANNELS];
    uint32_t rx_lim[2];
    uint32_t carrier[4];
    uint32_t int_raw;
    uint32_t int_ena;
    uint32_t sys_conf;
    uint32_t ref_cnt;
    uint32_t mem[ESP32C3_RMT_CHANNELS * ESP32C3_RMT_BLOCK_WORDS];

    ESP32C3RmtTx tx[ESP32C3_RMT_TX_CHANNELS];
    ESP32C3GPIOState *gpio;
} ESP32C3RmtState;
