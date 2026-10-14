#pragma once

// Persistent queue for sales events (sale, vend_request, vend_success, vend_fail, vend_cancel).
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

// For events that happen before SNTP sets the clock: only "<fields>" is kept,
// with the device uptime. After the sync the queue rebuilds the real time
// (now - elapsed uptime; the sync moment if the device rebooted meanwhile),
// signs the line, saves it and only then publishes it.
bool event_queue_push_unstamped(const char *event, const char *fields);

// Called from the MQTT event handler.
void event_queue_on_connected(void);
void event_queue_on_disconnected(void);
void event_queue_on_published(int msg_id);

// Called once the clock is set (SNTP sync callback).
void event_queue_on_clock_synced(void);

uint32_t event_queue_pending(void);
uint32_t event_queue_dropped(void);
