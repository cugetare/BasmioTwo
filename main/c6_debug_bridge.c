#include "c6_debug_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifndef CONFIG_SPOD_C6_DEBUG_BRIDGE_UART_PORT
#define CONFIG_SPOD_C6_DEBUG_BRIDGE_UART_PORT 1
#endif

#ifndef CONFIG_SPOD_C6_DEBUG_BRIDGE_BAUDRATE
#define CONFIG_SPOD_C6_DEBUG_BRIDGE_BAUDRATE 115200
#endif

#ifndef CONFIG_SPOD_C6_DEBUG_BRIDGE_P4_TX_GPIO
#define CONFIG_SPOD_C6_DEBUG_BRIDGE_P4_TX_GPIO 11
#endif

#ifndef CONFIG_SPOD_C6_DEBUG_BRIDGE_P4_RX_GPIO
#define CONFIG_SPOD_C6_DEBUG_BRIDGE_P4_RX_GPIO 12
#endif

#ifndef CONFIG_SPOD_C6_DEBUG_BRIDGE_C6_BOOT_GPIO
#define CONFIG_SPOD_C6_DEBUG_BRIDGE_C6_BOOT_GPIO 13
#endif

#ifndef CONFIG_SPOD_C6_DEBUG_BRIDGE_C6_RESET_GPIO
#define CONFIG_SPOD_C6_DEBUG_BRIDGE_C6_RESET_GPIO 54
#endif

#ifndef CONFIG_SPOD_C6_DEBUG_BRIDGE_RESET_ON_START
#define CONFIG_SPOD_C6_DEBUG_BRIDGE_RESET_ON_START 1
#endif

#ifndef CONFIG_SPOD_C6_DEBUG_BRIDGE_FORCE_BOOTLOADER
#define CONFIG_SPOD_C6_DEBUG_BRIDGE_FORCE_BOOTLOADER 0
#endif

#define C6_BRIDGE_UART_RX_BUF_SIZE 4096
#define C6_BRIDGE_UART_TX_BUF_SIZE 4096

static const char *TAG = "c6_bridge";

static uart_port_t bridge_uart_port(void)
{
    return (uart_port_t)CONFIG_SPOD_C6_DEBUG_BRIDGE_UART_PORT;
}

static bool gpio_is_valid_num(int gpio)
{
    return gpio >= 0 && GPIO_IS_VALID_GPIO(gpio);
}

static esp_err_t configure_open_drain_output(int gpio, int initial_level)
{
    if (!gpio_is_valid_num(gpio)) {
        return ESP_OK;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << gpio),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio_config GPIO%d", gpio);
    return gpio_set_level((gpio_num_t)gpio, initial_level ? 1 : 0);
}

static esp_err_t c6_bridge_uart_init(void)
{
    const uart_port_t uart_num = bridge_uart_port();
    const uart_config_t uart_cfg = {
        .baud_rate = CONFIG_SPOD_C6_DEBUG_BRIDGE_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_param_config(uart_num, &uart_cfg), TAG, "uart_param_config");
    ESP_RETURN_ON_ERROR(uart_set_pin(uart_num,
                                     CONFIG_SPOD_C6_DEBUG_BRIDGE_P4_TX_GPIO,
                                     CONFIG_SPOD_C6_DEBUG_BRIDGE_P4_RX_GPIO,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG,
                        "uart_set_pin");
    ESP_RETURN_ON_ERROR(uart_driver_install(uart_num,
                                            C6_BRIDGE_UART_RX_BUF_SIZE,
                                            C6_BRIDGE_UART_TX_BUF_SIZE,
                                            0,
                                            NULL,
                                            0),
                        TAG,
                        "uart_driver_install");
    return ESP_OK;
}

static void c6_bridge_reset_target(void)
{
    const int boot_gpio = CONFIG_SPOD_C6_DEBUG_BRIDGE_C6_BOOT_GPIO;
    const int reset_gpio = CONFIG_SPOD_C6_DEBUG_BRIDGE_C6_RESET_GPIO;
    const bool bootloader = CONFIG_SPOD_C6_DEBUG_BRIDGE_FORCE_BOOTLOADER;

    if (gpio_is_valid_num(boot_gpio)) {
        /* GPIO9 low during reset asks ESP32-C6 for ROM download mode. */
        esp_err_t err = configure_open_drain_output(boot_gpio, bootloader ? 0 : 1);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "C6 boot GPIO%d setup failed: %s", boot_gpio, esp_err_to_name(err));
        }
    }

    if (!CONFIG_SPOD_C6_DEBUG_BRIDGE_RESET_ON_START || !gpio_is_valid_num(reset_gpio)) {
        return;
    }

    esp_err_t err = configure_open_drain_output(reset_gpio, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "C6 reset GPIO%d setup failed: %s", reset_gpio, esp_err_to_name(err));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    err = gpio_set_level((gpio_num_t)reset_gpio, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "C6 reset assert failed: %s", esp_err_to_name(err));
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(80));
    err = gpio_set_level((gpio_num_t)reset_gpio, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "C6 reset release failed: %s", esp_err_to_name(err));
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(120));

    if (bootloader && gpio_is_valid_num(boot_gpio)) {
        ESP_LOGI(TAG, "C6 reset into ROM download mode");
    } else {
        ESP_LOGI(TAG, "C6 reset released for normal boot");
    }
}

static void c6_uart_to_console_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    const uart_port_t uart_num = bridge_uart_port();

    while (1) {
        int n = uart_read_bytes(uart_num, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n > 0) {
            fwrite(buf, 1, (size_t)n, stdout);
            fflush(stdout);
        }
    }
}

static void console_to_c6_uart_task(void *arg)
{
    (void)arg;
    uint8_t buf[128];
    const uart_port_t uart_num = bridge_uart_port();

    while (1) {
        int n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n > 0) {
            (void)uart_write_bytes(uart_num, (const char *)buf, n);
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void c6_debug_bridge_run(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }

    ESP_LOGI(TAG, "ESP32-C6 debug bridge starting");
    ESP_LOGI(TAG,
             "P4 UART%d TX=GPIO%d -> C6_U0RXD, RX=GPIO%d <- C6_U0TXD, baud=%d",
             CONFIG_SPOD_C6_DEBUG_BRIDGE_UART_PORT,
             CONFIG_SPOD_C6_DEBUG_BRIDGE_P4_TX_GPIO,
             CONFIG_SPOD_C6_DEBUG_BRIDGE_P4_RX_GPIO,
             CONFIG_SPOD_C6_DEBUG_BRIDGE_BAUDRATE);
    ESP_LOGI(TAG,
             "C6 boot GPIO=%d reset GPIO=%d bootloader=%s",
             CONFIG_SPOD_C6_DEBUG_BRIDGE_C6_BOOT_GPIO,
             CONFIG_SPOD_C6_DEBUG_BRIDGE_C6_RESET_GPIO,
             CONFIG_SPOD_C6_DEBUG_BRIDGE_FORCE_BOOTLOADER ? "yes" : "no");

    ESP_ERROR_CHECK(c6_bridge_uart_init());
    c6_bridge_reset_target();

    puts("\r\n--- ESP32-C6 UART bridge active ---");
    puts("Raw console is now bridged between P4 USB serial and C6 UART0.");
    puts("Use baud 115200 first for boot logs. Disable this menuconfig option to return to normal UI.\r\n");

    BaseType_t ok = xTaskCreatePinnedToCore(c6_uart_to_console_task,
                                            "c6_to_usb",
                                            4096,
                                            NULL,
                                            10,
                                            NULL,
                                            0);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ok = xTaskCreatePinnedToCore(console_to_c6_uart_task,
                                 "usb_to_c6",
                                 4096,
                                 NULL,
                                 10,
                                 NULL,
                                 0);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
