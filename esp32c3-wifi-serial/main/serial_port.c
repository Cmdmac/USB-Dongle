/*
 * serial_port.c
 */
#include "serial_port.h"
#include "app_cfg.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "sdkconfig.h"

/* IDF 5.2+ 提供 VFS 侧开关, 让 read()/write() 也走驱动程序缓冲 */
#if defined(__has_include)
#  if __has_include("driver/usb_serial_jtag_vfs.h")
#    include "driver/usb_serial_jtag_vfs.h"
#    define HAVE_USJ_VFS 1
#  endif
#endif

static const char *TAG = "serial";

static bool s_inited  = false;
static bool s_is_usb  = false;
static int  s_uart    = -1;
static char s_desc[64] = "none";

/* ------------------------------------------------------------------ */
/* USB Serial/JTAG                                                     */
/* ------------------------------------------------------------------ */
static esp_err_t init_usb(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    ESP_LOGE(TAG, "USB Serial/JTAG 当前被日志控制台占用, 无法做透传!");
    ESP_LOGE(TAG, "请在 menuconfig 里改回: Component config -> ESP System Settings");
    ESP_LOGE(TAG, "  -> Channel for console output -> UART0  (或 None)");
    return ESP_ERR_INVALID_STATE;
#else
    if (usb_serial_jtag_is_driver_installed()) {
        ESP_LOGW(TAG, "USB Serial/JTAG 驱动已安装, 直接复用");
    } else {
        usb_serial_jtag_driver_config_t usj_cfg = {
            .tx_buffer_size = 1024,
            .rx_buffer_size = 2048,
        };
        esp_err_t err = usb_serial_jtag_driver_install(&usj_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "USB Serial/JTAG 驱动安装失败: %s", esp_err_to_name(err));
            return err;
        }
#ifdef HAVE_USJ_VFS
        /* 让 VFS 读写也使用驱动缓冲, 避免和其他任务抢字节 */
        usb_serial_jtag_vfs_use_driver();
#endif
    }
    s_is_usb = true;
    snprintf(s_desc, sizeof(s_desc), "USB-CDC (内置 USB Serial/JTAG)");
    ESP_LOGI(TAG, "串口侧 = %s, 插到电脑后会枚举出一个 COM/ttyACM 口", s_desc);
    ESP_LOGI(TAG, "提示: 该口被串口工具占用时无法同时烧录, 烧录前请先关闭工具");
    return ESP_OK;
#endif
}

/* ------------------------------------------------------------------ */
/* 硬件 UART                                                           */
/* ------------------------------------------------------------------ */
static esp_err_t init_uart(int num, int tx, int rx, int baud)
{
    if (num == UART_NUM_0) {
#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
        ESP_LOGW(TAG, "UART0 同时是日志控制台! 透传的数据会混入日志。");
        ESP_LOGW(TAG, "建议在 menuconfig 设置 CONFIG_ESP_CONSOLE_NONE=y 后再用 UART0 透传。");
#endif
    }

    if (uart_is_driver_installed(num)) {
        ESP_LOGW(TAG, "UART%d 驱动已安装(可能是日志控制台), 直接复用", num);
    } else {
        esp_err_t err = uart_driver_install(num, 2048, 2048, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "UART%d 驱动安装失败: %s", num, esp_err_to_name(err));
            return err;
        }
    }

    uart_config_t uc = {
        .baud_rate  = baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(num, &uc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config 失败: %s", esp_err_to_name(err));
        return err;
    }

    if (tx >= 0 && rx >= 0) {
        err = uart_set_pin(num, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart_set_pin 失败: %s", esp_err_to_name(err));
            return err;
        }
    }

    uart_flush_input(num);
    s_uart = num;
    s_is_usb = false;
    snprintf(s_desc, sizeof(s_desc), "UART%d @%d (TX=%d RX=%d)", num, baud, tx, rx);
    ESP_LOGI(TAG, "串口侧 = %s", s_desc);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 公共接口                                                            */
/* ------------------------------------------------------------------ */
esp_err_t serial_port_init(void)
{
    if (s_inited) return ESP_OK;

    app_cfg_t *c = app_cfg();
    esp_err_t err;

    switch (c->serial_side) {
    case SERIAL_SIDE_UART0:
        err = init_uart(UART_NUM_0, c->uart_tx_gpio >= 0 ? c->uart_tx_gpio : 21,
                        c->uart_rx_gpio >= 0 ? c->uart_rx_gpio : 20, (int)c->uart_baud);
        break;
    case SERIAL_SIDE_UART1:
    default:
        if (c->serial_side == SERIAL_SIDE_UART1) {
            err = init_uart(UART_NUM_1, (int)c->uart_tx_gpio, (int)c->uart_rx_gpio, (int)c->uart_baud);
        } else {
            err = init_usb();
        }
        break;
    }

    if (err != ESP_OK) return err;

    s_inited = true;
    return ESP_OK;
}

int serial_port_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    if (!s_inited) return -1;

    if (s_is_usb) {
        int n = usb_serial_jtag_read_bytes(buf, len, pdMS_TO_TICKS(timeout_ms));
        return n;   /* 0 = 超时无数据, -1 = 错误 */
    }
    if (s_uart >= 0) {
        int n = (int)uart_read_bytes(s_uart, buf, len, pdMS_TO_TICKS(timeout_ms));
        return n;
    }
    return -1;
}

int serial_port_write(const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    if (!s_inited || len == 0) return -1;

    if (s_is_usb) {
        int n = usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(timeout_ms));
        return n;
    }

    if (s_uart >= 0) {
        int64_t t0 = esp_timer_get_time();
        size_t off = 0;
        while (off < len) {
            size_t free_sz = 0;
            if (uart_get_tx_buffer_free_size(s_uart, &free_sz) != ESP_OK) break;
            if (free_sz == 0) {
                if ((esp_timer_get_time() - t0) / 1000 >= (int64_t)timeout_ms) break;
                vTaskDelay(1);
                continue;
            }
            size_t chunk = len - off;
            if (chunk > free_sz) chunk = free_sz;
            int w = uart_write_bytes(s_uart, buf + off, chunk);
            if (w <= 0) break;
            off += (size_t)w;
            if ((esp_timer_get_time() - t0) / 1000 >= (int64_t)timeout_ms) break;
        }
        return (int)off;
    }
    return -1;
}

void serial_port_flush(void)
{
    if (!s_inited) return;
    if (s_uart >= 0) uart_flush_input(s_uart);
}

bool serial_port_is_usb(void)
{
    return s_is_usb;
}

const char *serial_port_desc(void)
{
    return s_desc;
}
