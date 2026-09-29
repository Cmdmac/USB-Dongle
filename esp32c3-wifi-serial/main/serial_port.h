/*
 * serial_port.h - 串口侧统一抽象
 *   SERIAL_SIDE_USB_CDC : 芯片内置 USB Serial/JTAG (无外设即可透传)
 *   SERIAL_SIDE_UART0/1 : 硬件 UART
 * 注意: ESP32-C3 只有 USB Serial/JTAG(固定 CDC+JTAG), 没有 USB-OTG,
 *       因此无法模拟 HID 键鼠, 只能做"虚拟串口"。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t serial_port_init(void);

/* 返回读取字节数, 0=超时无数据, <0=错误 */
int serial_port_read(uint8_t *buf, size_t len, uint32_t timeout_ms);
/* 返回写入字节数(可能少于 len, 表示对端没取走/超时) */
int serial_port_write(const uint8_t *buf, size_t len, uint32_t timeout_ms);

void        serial_port_flush(void);
bool        serial_port_is_usb(void);
const char *serial_port_desc(void);
