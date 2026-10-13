#pragma once

// Persistent queue for sales events (sale, vend_request, vend_success, vend_fail).
//
// Events are kept in NVS until the broker acknowledges them (QoS 1 PUBACK ->
// MQTT_EVENT_PUBLISHED with the msg_id) and are resent on every (re)connect,
// so a reboot or a long internet outage does not lose them. At most
// CONFIG_EVENT_QUEUE_MAX events are kept; when full, the oldest is dropped and
// counted.
//
// event_queue_push() only copies into a RAM queue and never blocks, so it is
// safe from the MDB task: flash writes and MQTT calls run on the queue's own
// task (core 0).

#include <stdbool.h>
#include <stdint.h>

void event_queue_init(void);

// event: topic suffix ("sale", ...); signed_line: "<fields>:<ts>:<hmac>".
bool event_queue_push(const char *event, const char *signed_line);

// Called from the MQTT event handler.
void event_queue_on_connected(void);
void event_queue_on_disconnected(void);
void event_queue_on_published(int msg_id);

uint32_t event_queue_pending(void);
uint32_t event_queue_dropped(void);
