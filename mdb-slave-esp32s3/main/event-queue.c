#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>
#include <mqtt_client.h>

#include "event-queue.h"

#define TAG "event_queue"

#define EVENT_LEN       16
#define LINE_LEN        128
#define RESEND_AFTER_US (30 * 1000000LL)   // no PUBACK in 30 s while connected: publish again

// Owned by the main translation unit.
extern esp_mqtt_client_handle_t mqtt_client;
extern void mqtt_topic(char *buf, size_t size, const char *event);

typedef enum { MSG_NEW, MSG_CONNECTED, MSG_DISCONNECTED, MSG_PUBLISHED } msg_type_t;

typedef struct {
	msg_type_t type;
	int msg_id;
	char event[EVENT_LEN];
	char line[LINE_LEN];
} queue_msg_t;

// Events in flight: NVS sequence number -> MQTT msg_id of the last publish.
typedef struct {
	uint32_t seq;
	int msg_id;
	int64_t sent_us;
} inflight_t;

static QueueHandle_t msg_queue;
static nvs_handle_t nvs;
static uint32_t head, tail;     // pending events are the NVS keys e<head>..e<tail-1>
static uint32_t dropped;
static bool connected;
static inflight_t inflight[CONFIG_EVENT_QUEUE_MAX];
static int inflight_n;

static void key_for(uint32_t seq, char *key) {
	snprintf(key, 16, "e%08lx", (unsigned long) seq);
}

static void save_counters(void) {
	nvs_set_u32(nvs, "head", head);
	nvs_set_u32(nvs, "tail", tail);
	nvs_set_u32(nvs, "drop", dropped);
	nvs_commit(nvs);
}

static int find_inflight(uint32_t seq) {
	for (int i = 0; i < inflight_n; i++)
		if (inflight[i].seq == seq) return i;
	return -1;
}

static void forget_inflight(int i) {
	inflight[i] = inflight[--inflight_n];
}

// Removes acknowledged/dropped keys from the front so head always points to a pending event.
static void advance_head(void) {
	char key[16];
	size_t len;
	while (head != tail) {
		key_for(head, key);
		if (nvs_get_str(nvs, key, NULL, &len) == ESP_OK) break;
		head++;
	}
}

static void publish(uint32_t seq) {
	char key[16], stored[EVENT_LEN + LINE_LEN + 1], topic[96];
	size_t len = sizeof(stored);
	key_for(seq, key);
	if (nvs_get_str(nvs, key, stored, &len) != ESP_OK) return;

	// Stored as "<event>\t<signed line>".
	char *tab = strchr(stored, '\t');
	if (tab == NULL) return;
	*tab = '\0';
	mqtt_topic(topic, sizeof(topic), stored);

	int msg_id = esp_mqtt_client_enqueue(mqtt_client, topic, tab + 1, 0, 1, 0, true);
	if (msg_id < 0) {
		ESP_LOGW(TAG, "outbox full, event %lu stays queued", (unsigned long) seq);
		return;
	}

	int i = find_inflight(seq);
	if (i < 0 && inflight_n < CONFIG_EVENT_QUEUE_MAX) i = inflight_n++;
	if (i >= 0) inflight[i] = (inflight_t) { .seq = seq, .msg_id = msg_id, .sent_us = esp_timer_get_time() };
}

static void store(const char *event, const char *line) {
	char key[16], stored[EVENT_LEN + LINE_LEN + 1];

	if (tail - head >= CONFIG_EVENT_QUEUE_MAX) {
		// Full: drop the oldest. The daily DEX audit still counts the sale.
		key_for(head, key);
		nvs_erase_key(nvs, key);
		int i = find_inflight(head);
		if (i >= 0) forget_inflight(i);
		dropped++;
		ESP_LOGE(TAG, "queue full (%d events): dropped the oldest, %lu dropped so far",
				CONFIG_EVENT_QUEUE_MAX, (unsigned long) dropped);
		head++;
		advance_head();
	}

	snprintf(stored, sizeof(stored), "%s\t%s", event, line);
	key_for(tail, key);
	if (nvs_set_str(nvs, key, stored) != ESP_OK) {
		ESP_LOGE(TAG, "NVS write failed, event %s sent without persistence", event);
		if (connected) {
			char topic[96];
			mqtt_topic(topic, sizeof(topic), event);
			esp_mqtt_client_enqueue(mqtt_client, topic, line, 0, 1, 0, true);
		}
		return;
	}
	tail++;
	save_counters();

	if (connected) publish(tail - 1);
}

static void acknowledged(int msg_id) {
	for (int i = 0; i < inflight_n; i++) {
		if (inflight[i].msg_id != msg_id) continue;

		char key[16];
		key_for(inflight[i].seq, key);
		nvs_erase_key(nvs, key);
		forget_inflight(i);
		advance_head();
		save_counters();
		return;
	}
}

static void publish_pending(bool only_stale) {
	int64_t now = esp_timer_get_time();
	for (uint32_t seq = head; seq != tail; seq++) {
		int i = find_inflight(seq);
		if (i >= 0 && (!only_stale || now - inflight[i].sent_us < RESEND_AFTER_US)) continue;
		publish(seq);
	}
}

static void handle_msg(const queue_msg_t *msg) {
	switch (msg->type) {
	case MSG_NEW:
		store(msg->event, msg->line);
		break;
	case MSG_CONNECTED:
		connected = true;
		inflight_n = 0;   // msg_ids from the previous connection are meaningless now
		publish_pending(false);
		break;
	case MSG_DISCONNECTED:
		connected = false;
		break;
	case MSG_PUBLISHED:
		acknowledged(msg->msg_id);
		break;
	}
}

// Every 5 s without messages: republish what is still waiting for a PUBACK.
static void tick(void) {
	if (connected) publish_pending(true);
}

static void event_queue_task(void *arg) {
	queue_msg_t msg;
	for (;;) {
		if (xQueueReceive(msg_queue, &msg, pdMS_TO_TICKS(5000)) == pdTRUE)
			handle_msg(&msg);
		else
			tick();
	}
}

void event_queue_init(void) {
	msg_queue = xQueueCreate(16, sizeof(queue_msg_t));

	nvs_open("evq", NVS_READWRITE, &nvs);
	nvs_get_u32(nvs, "head", &head);
	nvs_get_u32(nvs, "tail", &tail);
	nvs_get_u32(nvs, "drop", &dropped);
	advance_head();
	ESP_LOGI(TAG, "%lu event(s) pending from before boot", (unsigned long) (tail - head));

	xTaskCreatePinnedToCore(event_queue_task, "event_queue", 4096, NULL, 3, NULL, 0);
}

bool event_queue_push(const char *event, const char *signed_line) {
	queue_msg_t msg = { .type = MSG_NEW };
	strlcpy(msg.event, event, sizeof(msg.event));
	strlcpy(msg.line, signed_line, sizeof(msg.line));
	if (xQueueSend(msg_queue, &msg, 0) != pdTRUE) {
		dropped++;
		ESP_LOGE(TAG, "RAM queue full, event %s dropped", event);
		return false;
	}
	return true;
}

static void notify(msg_type_t type, int msg_id) {
	queue_msg_t msg = { .type = type, .msg_id = msg_id };
	xQueueSend(msg_queue, &msg, 0);
}

void event_queue_on_connected(void)       { notify(MSG_CONNECTED, 0); }
void event_queue_on_disconnected(void)    { notify(MSG_DISCONNECTED, 0); }
void event_queue_on_published(int msg_id) { notify(MSG_PUBLISHED, msg_id); }

uint32_t event_queue_pending(void) { return tail - head; }
uint32_t event_queue_dropped(void) { return dropped; }
