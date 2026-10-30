
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define MQTT_SERVICE_MAX_SUBS 8
#define MQTT_SERVICE_MAX_TOPIC 128
#define MQTT_SERVICE_MAX_PAYLOAD 2048
#define MQTT_SERVICE_QUEUE_DEPTH 8

typedef void (*mqtt_message_callback_t)(
    const char *topic,
    const uint8_t *payload,
    size_t payload_len,
    int qos,
    bool retained);

typedef void (*mqtt_published_callback_t)(
    int message_id);

typedef struct
{
    // Must use wss:// for TLS encryption.
    const char *broker_uri;

    const char *client_id;
    const char *username;
    const char *password;

    // NULL = ESP-IDF certificate bundle.
    // Otherwise supply a trusted PEM CA certificate.
    const char *ca_cert;

    // Availability / Last Will configuration.
    const char *status_topic;
    const char *online_message;
    const char *offline_message;

    mqtt_message_callback_t on_message;
    mqtt_published_callback_t on_published;
} mqtt_service_config_t;

esp_err_t mqtt_service_init(
    const mqtt_service_config_t *config);

esp_err_t mqtt_service_start(void);
esp_err_t mqtt_service_stop(void);

esp_err_t mqtt_service_subscribe(
    const char *topic,
    int qos);

// Supports binary payloads.
// out_message_id is optional (may be NULL).
esp_err_t mqtt_service_publish(
    const char *topic,
    const void *payload,
    size_t payload_len,
    int qos,
    bool retain,
    int *out_message_id);

bool mqtt_service_is_connected(void);

uint32_t mqtt_service_dropped_messages(void);

// Call only from an application management task.
void mqtt_service_deinit(void);
