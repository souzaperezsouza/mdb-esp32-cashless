// Minimal fakes of FreeRTOS, NVS, esp_timer, esp_log and esp-mqtt, just
// enough to run event-queue.c on a PC. State lives in test_event_queue.c.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND 0x1102

// FreeRTOS
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef void *QueueHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
QueueHandle_t xQueueCreate(int length, size_t item_size);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait);
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                                   int prio, void *handle, int core);

// log / timer
#define ESP_LOGI(tag, ...) ((void) 0)
#define ESP_LOGW(tag, ...) ((void) 0)
#define ESP_LOGE(tag, ...) ((void) 0)
int64_t esp_timer_get_time(void);

// NVS (single namespace is enough here)
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_commit(nvs_handle_t h);

// esp-mqtt
typedef void *esp_mqtt_client_handle_t;
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t c, const char *topic, const char *data, int len,
                            int qos, int retain, bool store);

// newlib (ESP-IDF) has strlcpy; not every host libc does.
static inline size_t test_strlcpy(char *dst, const char *src, size_t size) {
	size_t n = strlen(src);
	if (size) {
		size_t c = n < size - 1 ? n : size - 1;
		memcpy(dst, src, c);
		dst[c] = '\0';
	}
	return n;
}
#define strlcpy test_strlcpy
