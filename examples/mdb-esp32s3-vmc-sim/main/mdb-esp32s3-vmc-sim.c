/*
 * mdb-esp32s3-vmc-sim.c - bench VMC (vending machine controller) emulator
 *
 * Plays the vending machine toward an MDB cashless reader (address 0x10), so
 * the mdb-slave-esp32s3 firmware can be exercised on the bench without a real
 * machine. Bus master on UART2 at 9600 baud, 9th (mode) bit produced with the
 * parity trick used by mdb-esp32s3-bridge.
 *
 * Wiring (two bare ESP32-S3 devkits, 3.3 V logic, NO MDB board in between):
 *   this GPIO2 (TX)  ->  reader GPIO4 (PIN_MDB_RX)
 *   this GPIO1 (RX)  <-  reader GPIO5 (PIN_MDB_TX)
 *   GND              --  GND
 *
 * Commands typed in `idf.py monitor` (end with Enter):
 *   v <item> <cents>   VEND REQUEST (BOOT button = "v 12 350")
 *   m s | m f | m n    after VEND APPROVED: success / failure / do nothing
 *   c <item> <cents>   CASH SALE
 *   x                  VEND CANCEL
 *   e | d              READER ENABLE / READER DISABLE
 *   r                  RESET and run the setup sequence again
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <esp_log.h>

#define TAG "vmc_sim"

#define PIN_MDB_TX      GPIO_NUM_2
#define PIN_MDB_RX      GPIO_NUM_1
#define PIN_BOOT        GPIO_NUM_0
#define MDB_UART        UART_NUM_2
#define CONSOLE_UART    UART_NUM_0

#define BIT_MODE_SET    0x100
#define ACK             0x00
#define ADDR_CASHLESS   0x10

enum { CMD_RESET = 0x00, CMD_SETUP = 0x01, CMD_POLL = 0x02, CMD_VEND = 0x03, CMD_READER = 0x04, CMD_EXPANSION = 0x07 };
enum { VEND_REQUEST = 0x00, VEND_CANCEL = 0x01, VEND_SUCCESS = 0x02, VEND_FAILURE = 0x03, SESSION_COMPLETE = 0x04, CASH_SALE = 0x05 };

#define VEND_TIMEOUT_MS 90000   // a machine gives up waiting for approval after a while; then VEND CANCEL

static char mode = 's';             // what to do after VEND APPROVED
static uint8_t scale_factor = 1;    // from the reader CONFIG DATA reply
static uint8_t decimal_places = 2;
static bool session_open = false;
static bool vending = false;
static uint16_t vend_item = 0;
static int64_t vend_started_ms = 0;

//------------------------------------------------------------------------//
// 9-bit MDB over UART2 (same parity trick as mdb-esp32s3-bridge)
//------------------------------------------------------------------------//

static void write_9(uint16_t nth9) {
    uint8_t ones = __builtin_popcount((uint8_t) nth9);
    uart_wait_tx_done(MDB_UART, pdMS_TO_TICKS(250));
    if ((nth9 >> 8) & 1)
        uart_set_parity(MDB_UART, ones % 2 ? UART_PARITY_EVEN : UART_PARITY_ODD);
    else
        uart_set_parity(MDB_UART, ones % 2 ? UART_PARITY_ODD : UART_PARITY_EVEN);
    uart_write_bytes(MDB_UART, (uint8_t *) &nth9, 1);
    uart_wait_tx_done(MDB_UART, pdMS_TO_TICKS(250));
}

// Sends one command (first byte carries the mode bit) and reads the reply.
// Returns the reply length without the checksum byte, -1 if nothing came back.
static int transact(const uint8_t *tx, uint8_t len, uint8_t *rx) {
    uint8_t checksum = 0;
    uart_flush_input(MDB_UART);
    for (uint8_t i = 0; i < len; i++) {
        write_9(i == 0 ? (tx[i] | BIT_MODE_SET) : tx[i]);
        checksum += tx[i];
    }
    write_9(checksum);

    // The reader answers within 5 ms (MDB t-response); collect what arrives.
    int n = uart_read_bytes(MDB_UART, rx, 36, pdMS_TO_TICKS(30));
    if (n <= 0) return -1;
    if (n == 1 && rx[0] == ACK) return 0;   // plain ACK

    uint8_t sum = 0;
    for (int i = 0; i < n - 1; i++) sum += rx[i];
    if (sum != rx[n - 1]) {
        ESP_LOGW(TAG, "bad checksum from reader (%d bytes)", n);
        return -1;
    }
    write_9(ACK);   // acknowledge data replies
    return n - 1;
}

static uint16_t cents_to_mdb(uint32_t cents) {
    uint32_t unit = scale_factor;   // cents per MDB unit = SF * 10^(2 - DEC)
    for (int i = decimal_places; i < 2; i++) unit *= 10;
    return (uint16_t) (cents / unit);
}

static int64_t now_ms(void) {
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

//------------------------------------------------------------------------//
// Commands toward the reader
//------------------------------------------------------------------------//

static void vend_cmd(uint8_t sub, uint16_t a, uint16_t b, uint8_t nargs) {
    uint8_t tx[6] = { ADDR_CASHLESS | CMD_VEND, sub, a >> 8, a, b >> 8, b }, rx[36];
    transact(tx, 2 + nargs * 2, rx);
}

static void setup_reader(void) {
    uint8_t tx[36], rx[36];
    int n;

    tx[0] = ADDR_CASHLESS | CMD_RESET;
    transact(tx, 1, rx);
    session_open = vending = false;

    // Wait for JUST RESET (0x00) in the POLL replies.
    for (int i = 0; i < 20; i++) {
        tx[0] = ADDR_CASHLESS | CMD_POLL;
        if ((n = transact(tx, 1, rx)) >= 1 && rx[0] == 0x00) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    tx[0] = ADDR_CASHLESS | CMD_SETUP; tx[1] = 0x00; tx[2] = 3; tx[3] = 0; tx[4] = 0; tx[5] = 0;
    n = transact(tx, 6, rx);
    if (n >= 6 && rx[0] == 0x01) {
        scale_factor = rx[4];
        decimal_places = rx[5];
        ESP_LOGI(TAG, "reader config: level %d, currency 0x%02x%02x, scale %d, decimals %d",
                 rx[1], rx[2], rx[3], scale_factor, decimal_places);
    } else {
        ESP_LOGW(TAG, "no CONFIG DATA from reader (is it wired and powered?)");
    }

    tx[0] = ADDR_CASHLESS | CMD_SETUP; tx[1] = 0x01; tx[2] = 0xFF; tx[3] = 0xFE; tx[4] = 0x00; tx[5] = 0x00;
    transact(tx, 6, rx);

    memset(tx, 0, sizeof(tx));
    tx[0] = ADDR_CASHLESS | CMD_EXPANSION; tx[1] = 0x00;
    memcpy(&tx[2], "SIM", 3);
    transact(tx, 31, rx);

    tx[0] = ADDR_CASHLESS | CMD_READER; tx[1] = 0x01;
    transact(tx, 2, rx);
    ESP_LOGI(TAG, "reader enabled; polling");
}

// Interprets one POLL reply (several responses may come concatenated).
static void handle_poll_reply(const uint8_t *rx, int n) {
    for (int i = 0; i < n; ) {
        switch (rx[i]) {
        case 0x00: ESP_LOGI(TAG, "<- JUST RESET"); i += 1; break;
        case 0x03: {
            uint16_t funds = (rx[i + 1] << 8) | rx[i + 2];
            session_open = true;
            if (funds == 0xFFFF) ESP_LOGI(TAG, "<- BEGIN SESSION (funds unknown: approval per vend)");
            else ESP_LOGI(TAG, "<- BEGIN SESSION funds=%u", funds);
            i += 3;
            break;
        }
        case 0x04:
            ESP_LOGI(TAG, "<- SESSION CANCEL REQUEST -> SESSION COMPLETE");
            vend_cmd(SESSION_COMPLETE, 0, 0, 0);
            i += 1;
            break;
        case 0x05: {
            uint16_t amount = (rx[i + 1] << 8) | rx[i + 2];
            ESP_LOGI(TAG, "<- VEND APPROVED amount=%u (mode '%c')", amount, mode);
            vending = false;
            if (mode == 's') {
                ESP_LOGI(TAG, "-> VEND SUCCESS item %u, SESSION COMPLETE", vend_item);
                vend_cmd(VEND_SUCCESS, vend_item, 0, 1);
                vend_cmd(SESSION_COMPLETE, 0, 0, 0);
            } else if (mode == 'f') {
                ESP_LOGI(TAG, "-> VEND FAILURE, SESSION COMPLETE");
                vend_cmd(VEND_FAILURE, 0, 0, 0);
                vend_cmd(SESSION_COMPLETE, 0, 0, 0);
            }
            i += 3;
            break;
        }
        case 0x06:
            ESP_LOGI(TAG, "<- VEND DENIED -> SESSION COMPLETE");
            vending = false;
            vend_cmd(SESSION_COMPLETE, 0, 0, 0);
            i += 1;
            break;
        case 0x07:
            ESP_LOGI(TAG, "<- END SESSION");
            session_open = false;
            i += 1;
            break;
        case 0x08: ESP_LOGI(TAG, "<- CANCELLED"); i += 1; break;
        case 0x09: ESP_LOGI(TAG, "<- PERIPHERAL ID"); i = n; break;
        case 0x0B: ESP_LOGI(TAG, "<- COMMAND OUT OF SEQUENCE"); i += 1; break;
        default:
            ESP_LOGW(TAG, "<- unknown 0x%02x", rx[i]);
            i = n;
            break;
        }
    }
}

static void vend_request(uint16_t item, uint32_t cents) {
    if (vending) {
        ESP_LOGW(TAG, "a vend is already waiting for approval");
        return;
    }
    if (!session_open) ESP_LOGW(TAG, "no session open; sending VEND REQUEST anyway");
    vend_item = item;
    vending = true;
    vend_started_ms = now_ms();
    ESP_LOGI(TAG, "-> VEND REQUEST item %u, %lu cents (%u MDB units)", item, (unsigned long) cents, cents_to_mdb(cents));
    vend_cmd(VEND_REQUEST, cents_to_mdb(cents), item, 2);
}

static void console_command(char *line) {
    unsigned int item = 0, cents = 0;
    char m;
    if (sscanf(line, "v %u %u", &item, &cents) == 2) vend_request(item, cents);
    else if (sscanf(line, "c %u %u", &item, &cents) == 2) {
        ESP_LOGI(TAG, "-> CASH SALE item %u, %u cents", item, cents);
        vend_cmd(CASH_SALE, cents_to_mdb(cents), item, 2);
    } else if (sscanf(line, "m %c", &m) == 1 && strchr("sfn", m)) {
        mode = m;
        ESP_LOGI(TAG, "after approval: %s", m == 's' ? "VEND SUCCESS" : m == 'f' ? "VEND FAILURE" : "do nothing");
    } else if (line[0] == 'x') {
        ESP_LOGI(TAG, "-> VEND CANCEL");
        vend_cmd(VEND_CANCEL, 0, 0, 0);
        vending = false;
    } else if (line[0] == 'e' || line[0] == 'd') {
        uint8_t tx[2] = { ADDR_CASHLESS | CMD_READER, line[0] == 'e' ? 0x01 : 0x00 }, rx[36];
        transact(tx, 2, rx);
        ESP_LOGI(TAG, "-> READER %s", line[0] == 'e' ? "ENABLE" : "DISABLE");
    } else if (line[0] == 'r') {
        setup_reader();
    } else if (line[0]) {
        ESP_LOGI(TAG, "commands: v <item> <cents> | c <item> <cents> | m s|f|n | x | e | d | r");
    }
}

//------------------------------------------------------------------------//
// Main loop: POLL every 100 ms, console and BOOT button in between
//------------------------------------------------------------------------//

static void vmc_task(void *arg) {
    uint8_t tx[1] = { ADDR_CASHLESS | CMD_POLL }, rx[36];
    char line[48];
    int len = 0;
    int last_boot = 1;

    setup_reader();

    for (;;) {
        int n = transact(tx, 1, rx);
        if (n > 0) handle_poll_reply(rx, n);

        if (vending && now_ms() - vend_started_ms > VEND_TIMEOUT_MS) {
            ESP_LOGW(TAG, "no approval in %d s -> VEND CANCEL", VEND_TIMEOUT_MS / 1000);
            vend_cmd(VEND_CANCEL, 0, 0, 0);
            vending = false;
        }

        uint8_t ch;
        while (uart_read_bytes(CONSOLE_UART, &ch, 1, 0) == 1) {
            if (ch == '\r' || ch == '\n') {
                line[len] = '\0';
                console_command(line);
                len = 0;
            } else if (len < (int) sizeof(line) - 1) {
                line[len++] = ch;
            }
        }

        int boot = gpio_get_level(PIN_BOOT);
        if (last_boot == 1 && boot == 0) vend_request(12, 350);
        last_boot = boot;

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void) {
    uart_config_t mdb_cfg = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_EVEN,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(MDB_UART, &mdb_cfg);
    uart_set_pin(MDB_UART, PIN_MDB_TX, PIN_MDB_RX, -1, -1);
    uart_driver_install(MDB_UART, 256, 256, 0, NULL, 0);

    // Console input from `idf.py monitor` (UART0 is also the log output).
    uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0);

    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << PIN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&boot_cfg);

    xTaskCreate(vmc_task, "vmc_task", 4096, NULL, 5, NULL);
}
