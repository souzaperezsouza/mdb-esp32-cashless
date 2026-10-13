// Host test for main/event-queue.c (the real file, compiled against fakes).
// Run: sh test/host/run.sh   (needs a C compiler: cc, gcc, clang or zig cc)

#define CONFIG_EVENT_QUEUE_MAX 4
#include "../../main/event-queue.c"

#include <assert.h>
#include <stdlib.h>

// ── fakes ───────────────────────────────────────────────────────────────────
static queue_msg_t fake_q[64];
static int fake_q_n;

QueueHandle_t xQueueCreate(int length, size_t item_size) { fake_q_n = 0; return (QueueHandle_t) fake_q; }
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait) {
	if (fake_q_n == 64) return pdFALSE;
	memcpy(&fake_q[fake_q_n++], item, sizeof(queue_msg_t));
	return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait) { return pdFALSE; }
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *n, uint32_t s, void *a, int p, void *h, int c) { return pdTRUE; }

static int64_t clock_us;
int64_t esp_timer_get_time(void) { return clock_us; }

// NVS: survives "reboots" (only the RAM state of event-queue.c is reset).
static struct { char key[16]; char val[200]; bool is_u32; uint32_t u32; } nvs_kv[256];
static int nvs_n;
static int find_key(const char *k) {
	for (int i = 0; i < nvs_n; i++) if (!strcmp(nvs_kv[i].key, k)) return i;
	return -1;
}
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { *h = 1; return ESP_OK; }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *o) {
	int i = find_key(k);
	if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
	*o = nvs_kv[i].u32;
	return ESP_OK;
}
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v) {
	int i = find_key(k);
	if (i < 0) { i = nvs_n++; strcpy(nvs_kv[i].key, k); }
	nvs_kv[i].is_u32 = true; nvs_kv[i].u32 = v;
	return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *o, size_t *len) {
	int i = find_key(k);
	if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
	if (o) { strncpy(o, nvs_kv[i].val, *len); o[*len - 1] = '\0'; }
	*len = strlen(nvs_kv[i].val) + 1;
	return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) {
	int i = find_key(k);
	if (i < 0) { i = nvs_n++; strcpy(nvs_kv[i].key, k); }
	strcpy(nvs_kv[i].val, v);
	return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k) {
	int i = find_key(k);
	if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
	nvs_kv[i] = nvs_kv[--nvs_n];
	return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { return ESP_OK; }

// MQTT: records what was published; can pretend the outbox is full.
esp_mqtt_client_handle_t mqtt_client;
static char published[64][200];
static int published_n, next_msg_id = 1;
static bool outbox_full;
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t c, const char *topic, const char *data, int len, int qos, int retain, bool store) {
	if (outbox_full) return -2;
	snprintf(published[published_n++], 200, "%s %s", topic, data);
	return next_msg_id++;
}
void mqtt_topic(char *buf, size_t size, const char *event) { snprintf(buf, size, "maquinas/42/%s", event); }

// ── helpers ─────────────────────────────────────────────────────────────────
static void drain(void) {
	for (int i = 0; i < fake_q_n; i++) handle_msg(&fake_q[i]);
	fake_q_n = 0;
}

static void reboot(void) {
	head = tail = dropped = 0;
	connected = false;
	inflight_n = 0;
	event_queue_init();
}

static void push(const char *event, const char *line) { event_queue_push(event, line); drain(); }
static void connect(void) { event_queue_on_connected(); drain(); }
static void disconnect(void) { event_queue_on_disconnected(); drain(); }
static void puback(int msg_id) { event_queue_on_published(msg_id); drain(); }

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

int main(void) {
	reboot();

	// 1. Offline: nothing goes out, everything survives a reboot.
	push("sale", "350:12:1000:aa");
	push("vend_request", "350:12:1001:bb");
	push("vend_success", "350:12:1002:cc");
	CHECK(published_n == 0 && event_queue_pending() == 3);
	reboot();
	CHECK(event_queue_pending() == 3);

	// 2. Connect: all pending go out in order; removed only on PUBACK.
	connect();
	CHECK(published_n == 3);
	CHECK(!strcmp(published[0], "maquinas/42/sale 350:12:1000:aa"));
	CHECK(!strcmp(published[2], "maquinas/42/vend_success 350:12:1002:cc"));
	puback(2);                                  // out-of-order ack
	CHECK(event_queue_pending() == 3);          // head still waits for #1
	puback(1);
	CHECK(event_queue_pending() == 1);
	puback(999);                                // someone else's msg_id (status, info...): ignored
	CHECK(event_queue_pending() == 1);

	// 3a. Connection drops with no PUBACK: the event is resent on reconnect.
	disconnect();
	connect();
	CHECK(published_n == 4 && !strcmp(published[3], "maquinas/42/vend_success 350:12:1002:cc"));
	CHECK(event_queue_pending() == 1);
	puback(4);
	CHECK(event_queue_pending() == 0);

	// 3b. A PUBACK that arrives after the drop still proves delivery (esp-mqtt
	//     resends its outbox with the same msg_id): not resent on reconnect.
	push("sale", "150:3:1500:a1");              // msg_id 5
	disconnect();
	puback(5);
	CHECK(event_queue_pending() == 0);
	connect();
	CHECK(published_n == 5);

	// 4. Connected but no PUBACK for 30 s: published again.
	push("sale", "200:5:2000:dd");
	CHECK(published_n == 6);
	clock_us += 10 * 1000000LL; tick();
	CHECK(published_n == 6);
	clock_us += 25 * 1000000LL; tick();
	CHECK(published_n == 7);
	puback(7);
	CHECK(event_queue_pending() == 0);

	// 5. Outbox full: stays queued, goes out on a later tick.
	outbox_full = true;
	push("sale", "200:5:3000:ee");
	CHECK(event_queue_pending() == 1);
	outbox_full = false;
	tick();
	CHECK(!strcmp(published[published_n - 1], "maquinas/42/sale 200:5:3000:ee"));
	puback(next_msg_id - 1);
	CHECK(event_queue_pending() == 0);

	// 6. Queue full (max 4) while offline: the oldest are dropped and counted.
	disconnect();
	int before = published_n;
	for (int i = 0; i < 6; i++) {
		char line[32];
		snprintf(line, sizeof(line), "100:%d:4000:ff", i);
		push("sale", line);
	}
	CHECK(event_queue_pending() == 4 && event_queue_dropped() == 2);
	reboot();
	CHECK(event_queue_pending() == 4 && event_queue_dropped() == 2);
	connect();
	CHECK(published_n == before + 4);
	CHECK(!strcmp(published[before], "maquinas/42/sale 100:2:4000:ff"));   // 0 and 1 were dropped

	printf("event-queue: todos os testes passaram\n");
	return 0;
}
