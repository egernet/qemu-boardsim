#pragma once

#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/registerfields.h"
#include "chardev/char-fe.h"

#define TYPE_ESP32C3_GPIO "esp32c3.gpio"
#define ESP32C3_GPIO(obj)           OBJECT_CHECK(ESP32C3GPIOState, (obj), TYPE_ESP32C3_GPIO)
#define ESP32C3_GPIO_GET_CLASS(obj) OBJECT_GET_CLASS(ESP32C3GPIOClass, obj, TYPE_ESP32C3_GPIO)
#define ESP32C3_GPIO_CLASS(klass)   OBJECT_CLASS_CHECK(ESP32C3GPIOClass, klass, TYPE_ESP32C3_GPIO)

#define ESP32C3_STRAP_MODE_FLASH_BOOT 0x8   /* SPI Boot */
#define ESP32C3_STRAP_MODE_UART_BOOT  0x2   /* Diagnostic Mode0+UART0 download Mode */
#define ESP32C3_STRAP_MODE_USB_BOOT   0x0   /* Diagnostic Mode1+USB download Mode */

#define ESP32C3_GPIO_COUNT      22
#define ESP32C3_GPIO_IN_SIGNALS 128

/* GPIO matrix output signal numbers (soc/gpio_sig_map.h) */
#define ESP32C3_SIG_RMT_OUT0    51
#define ESP32C3_SIG_GPIO_OUT    128

/*
 * The GPIO block keeps the output, enable and matrix registers, so the pins
 * can be watched from outside. With a "chardev" attached (id "boardsim" on
 * the command line), it reports pin changes and RMT transmissions as text
 * lines and takes input levels back:
 *
 *   to host:   O <out hex> <enable hex>        output or enable changed
 *              M <gpio> <signal>                matrix output selection changed
 *              R <gpio> <tick ps> <n> <items>   one RMT transmission, n 32-bit items in hex
 *              B                                the chip was reset
 *   from host: I <in hex>                       levels of the input pins
 */
typedef struct ESP32C3State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t strap_mode;

    uint32_t out;
    uint32_t enable;
    uint32_t in_ext;
    uint32_t pin[ESP32C3_GPIO_COUNT];
    uint32_t out_sel[ESP32C3_GPIO_COUNT];
    uint32_t in_sel[ESP32C3_GPIO_IN_SIGNALS];

    CharBackend chr;
    char rx[128];
    int rx_len;
    uint32_t sent_out;
    uint32_t sent_enable;
    bool sent_any;
} ESP32C3GPIOState;

typedef struct ESP32C3GPIOClass {
    SysBusDeviceClass parent;
} ESP32C3GPIOClass;

/* Sends one finished RMT transmission of TX channel `channel` to the host. */
void esp32c3_gpio_report_rmt(ESP32C3GPIOState *s, int channel, uint32_t tick_ps,
                             const uint32_t *items, int count);
